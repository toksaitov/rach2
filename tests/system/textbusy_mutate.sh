#!/bin/sh
# Prove that textbusy detects removal of executable write exclusion.
set -eu
repo=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
cd "$repo"
. "$repo/tests/system/runlock.sh"
runlock_hold "$repo" textbusy-faults "$repo/tests/system/textbusy_mutate.sh" "$@"
source=rach2/sys/fs/fd.c
old='            return -ETXTBSY;'
if [ "${1:-}" = --texts ]; then
    python3 - "$source" "$old" <<'PY'
from pathlib import Path
import sys
assert Path(sys.argv[1]).read_text().count(sys.argv[2]) == 1
PY
    echo 'textbusy-faults: checked: exclusion text'
    exit
fi
[ "$#" -eq 0 ] || exit 2
work=
child=
changed=
finished=
restore()
{
    if [ -n "$child" ]; then
        kill -TERM "-$child" 2>/dev/null || :
        wait "$child" 2>/dev/null || :
        child=
    fi
    if [ -n "$changed" ]; then
        cp "$work/fd.c" "$source"
        touch "$source"
        changed=
        make -C rach2 ARCH=x86_64
    fi
    [ -z "$work" ] || rm -rf "$work"
}
trap 'restore; [ -n "$finished" ]' EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM
work=$(mktemp -d "${TMPDIR:-/tmp}/rach2-textbusy-faults.XXXXXX")
cp "$source" "$work/fd.c"
changed=1
python3 - "$source" "$old" <<'PY'
from pathlib import Path
import sys
p = Path(sys.argv[1])
s = p.read_text()
assert s.count(sys.argv[2]) == 1
p.write_text(s.replace(sys.argv[2], '            continue;'))
PY
setsid env ARCH=x86_64 RACH2_WORKERS=4 tests/run textbusy > "$work/fault.log" 2>&1 &
child=$!
rc=0
wait "$child" || rc=$?
child=
cat "$work/fault.log"
[ "$rc" -ne 0 ]
grep -q 'TEXTBUSYMUSL FAIL (exec writer)' build/x86_64/images/textbusy-test.log
echo 'textbusy-faults: caught: executable write exclusion removed'
restore
work=
ARCH=x86_64 RACH2_WORKERS=4 tests/run textbusy
echo 'textbusy-faults: passed: restored executable write exclusion'
finished=1
