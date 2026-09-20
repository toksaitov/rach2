#!/bin/sh
# Compile rm's fixed, faulty and restored forms in a private tree.
set -eu

repo=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
cd "$repo"
mkdir -p build/tests
work=$(mktemp -d "$repo/build/tests/rm-proof.XXXXXX")
child=
mkdir "$work/utils"
cp utils/rm.c "$work/utils/rm.c"
cp "$work/utils/rm.c" "$work/rm.saved"

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
python3 - "$work/utils/rm.c" <<'PY'
from pathlib import Path
import sys

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
run_phase unguarded 1 'rm-hosttest: fail: denied operand preserves target contents'
if cmp -s "$work/fixed" "$work/unguarded"; then
    echo "rm-mutation: binary did not change" >&2
    exit 1
fi
cleanup
run_phase restored 0 'rm-hosttest: result: pass:'
cmp "$work/rm.saved" "$work/utils/rm.c"
echo 'rm-mutation: result: pass: compiled fault failed and restored source passed'
