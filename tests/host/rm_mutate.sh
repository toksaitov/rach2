#!/bin/sh
# Prove rm_hosttest.c with one private mutation of utils/rm.c: after a failed
# unlink, remove_recursive empties the operand as a directory without checking
# that it is one.
set -eu

repo=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
cd "$repo"

# apply_fault FILE: put the fault into FILE, a copy of rm.c; with
# RACH2_FAULT_TEXTS set, only check its text and write nothing.
apply_fault()
{
    python3 - "$1" <<'PY'
from pathlib import Path
import sys
import os
import pathlib
if os.environ.get('RACH2_FAULT_TEXTS'):
    pathlib.Path.write_text = lambda self, *args, **kwargs: 0

path = Path(sys.argv[1])
source = path.read_text()
changes = [
    ("    struct stat st;\n\n", ""),
    ("    if (lstat(path, &st) < 0 || !S_ISDIR(st.st_mode)) {\n"
     "        return -1;\n    }\n", ""),
]
for old, new in changes:
    if source.count(old) != 1:
        raise SystemExit("rm-mutation: source pattern is not unique")
    source = source.replace(old, new)
path.write_text(source)
PY
}

if [ "${1:-}" = --texts ]; then
    RACH2_FAULT_TEXTS=1
    export RACH2_FAULT_TEXTS
    apply_fault utils/rm.c >/dev/null
    echo 'rm-mutation: texts: every fault is once in its source'
    exit 0
fi

. "$repo/tests/system/runlock.sh"
runlock_hold "$repo" rm-mutation "$repo/tests/host/rm_mutate.sh" "$@"


mkdir -p build/tests
work=$(mktemp -d "$repo/build/tests/rm-proof.XXXXXX")
child=
mkdir "$work/utils"
cp utils/rm.c "$work/utils/rm.c"
cp "$work/utils/rm.c" "$work/rm.saved"

# cleanup: stop the running test and put the saved rm.c back; the script calls
# it after the fault, and it is the exit trap.
cleanup()
{
    if [ -n "$child" ]; then
        kill -TERM "$child" 2>/dev/null || :
        wait "$child" 2>/dev/null || :
        child=
    fi
    cp "$work/rm.saved" "$work/utils/rm.c"
    touch "$work/utils/rm.c"
}
trap cleanup EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

# run_phase PHASE STATUS MESSAGE: compile rm_hosttest.c over the copy as
# $work/PHASE, run it for at most 10 seconds, and exit unless it ends with
# STATUS and its log contains MESSAGE.
run_phase()
{
    phase=$1
    expected=$2
    message=$3
    binary="$work/$phase"
    if ! "${CC:-cc}" -std=c99 -D_DEFAULT_SOURCE -D_FORTIFY_SOURCE=0 \
        -DRACH2_HOSTTEST -g -O1 -ffreestanding -Wall -Wextra -Werror \
        -fsanitize=address,undefined -fno-sanitize-recover=all \
        -I "$work/utils" tests/host/rm_hosttest.c -o "$binary" \
        >"$binary.compile.log" 2>&1; then
        cat "$binary.compile.log" >&2
        exit 1
    fi
    cksum "$work/utils/rm.c" "$binary" >"$binary.cksum"
    perl -e 'alarm 10; exec @ARGV or die "rm-mutation: exec: $!\n"' \
        "$binary" >"$binary.log" 2>&1 &
    child=$!
    status=0
    wait "$child" || status=$?
    child=
    cat "$binary.log"
    [ "$status" -eq "$expected" ] && grep -Fq "$message" "$binary.log" || exit 1
    echo "rm-mutation: result: $phase status $status"
}

echo "rm-mutation: logs: $work"
run_phase fixed 0 'rm-hosttest: result: pass:'
apply_fault "$work/utils/rm.c"
run_phase unguarded 1 'rm-hosttest: fail: denied operand preserves target contents'
if cmp -s "$work/fixed" "$work/unguarded"; then
    echo "rm-mutation: binary did not change" >&2
    exit 1
fi
cleanup
run_phase restored 0 'rm-hosttest: result: pass:'
cmp "$work/rm.saved" "$work/utils/rm.c"
echo 'rm-mutation: result: pass: compiled fault failed and restored source passed'
