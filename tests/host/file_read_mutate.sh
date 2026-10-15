#!/bin/sh
# Prove file_read_hosttest.c with private faults that ignore read errors.
set -eu

repo=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
cd "$repo"

# apply_fault FILE: disable its read-error check; --texts only checks it.
apply_fault()
{
    python3 - "$1" "${2:-write}" <<'PYFAULT'
from pathlib import Path
import sys

path = Path(sys.argv[1])
source = path.read_text()
condition = "if (errno != 0)" if path.name == "ls.c" else "if (n < 0)"
if source.count(condition) != 1:
    raise SystemExit("file-read-mutation: source pattern is not unique")
if sys.argv[2] != "texts":
    path.write_text(source.replace(condition, "if (0)"))
PYFAULT
}

if [ "${1:-}" = --texts ]; then
    for program in cp cat ls; do
        apply_fault "utils/$program.c" texts
    done
    echo 'file-read-mutation: texts: every fault is once in its source'
    exit 0
fi

. "$repo/tests/system/runlock.sh"
runlock_hold "$repo" file-read-mutation "$repo/tests/host/file_read_mutate.sh" "$@"

mkdir -p build/tests
work=$(mktemp -d "$repo/build/tests/file-read-proof.XXXXXX")
child=
mkdir "$work/utils"
mkdir "$work/saved"
for program in cp cat ls; do
    cp "utils/$program.c" "$work/utils/$program.c"
    cp "utils/$program.c" "$work/saved/$program.c"
done

# cleanup: stop the test and restore the private sources.
cleanup()
{
    if [ -n "$child" ]; then
        kill -TERM "$child" 2>/dev/null || :
        wait "$child" 2>/dev/null || :
        child=
    fi
    for program in cp cat ls; do
        cp "$work/saved/$program.c" "$work/utils/$program.c"
        touch "$work/utils/$program.c"
    done
}
trap cleanup EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

# run_phase PHASE STATUS MESSAGE: compile the private sources and require
# STATUS and MESSAGE from a test bounded by ten seconds.
run_phase()
{
    phase=$1
    expected=$2
    message=$3
    binary="$work/$phase"
    if ! "${CC:-cc}" -std=c99 -D_DEFAULT_SOURCE -D_FORTIFY_SOURCE=0 \
        -DRACH2_HOSTTEST -g -O1 -ffreestanding -Wall -Wextra -Werror \
        -fsanitize=address,undefined -fno-sanitize-recover=all \
        -I "$work/utils" tests/host/file_read_hosttest.c -o "$binary" \
        >"$binary.compile.log" 2>&1; then
        cat "$binary.compile.log" >&2
        exit 1
    fi
    cksum "$work/utils/"*.c "$binary" >"$binary.cksum"
    perl -e 'alarm 10; exec @ARGV or die "file-read-mutation: exec: $!\n"' \
        "$binary" >"$binary.log" 2>&1 &
    child=$!
    status=0
    wait "$child" || status=$?
    child=
    cat "$binary.log"
    [ "$status" -eq "$expected" ] && grep -Fq "$message" "$binary.log" || exit 1
    echo "file-read-mutation: result: $phase status $status"
}

echo "file-read-mutation: logs: $work"
run_phase fixed 0 'file-read-hosttest: result: pass:'
for target in cp cat ls; do
    apply_fault "$work/utils/$target.c"
    run_phase "$target-ignored" 1 "file-read-hosttest: fail: $target blocks 0 error 1"
    cleanup
    run_phase "$target-restored" 0 'file-read-hosttest: result: pass:'
    cmp "$work/saved/$target.c" "$work/utils/$target.c"
done
echo 'file-read-mutation: result: pass: compiled faults failed and restored sources passed'
