#!/bin/sh
# nativecheck_mutate.sh [--texts]: prove tests/system/nativecheck_test.sh
# with faults in private copies of tools/verify/nativecheck.sh: a program
# with both a case and a reason passed, an id that maps lacks passed, a volume
# of another type than its image passed, and a fifth volume passed. Each must
# fail the test, and the restored copy must pass it. --texts checks that each
# fault's text is once in the checker and runs nothing.
set -eu
repo=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
cd "$repo"

# apply_fault FILE NAME: put the fault NAME into FILE; with RACH2_FAULT_TEXTS
# set, only check its text.
apply_fault()
{
    python3 - "$1" "$2" <<'PY'
from pathlib import Path
import os
import sys
path = Path(sys.argv[1])
source = path.read_text()
faults = {
    'both': ('if (p in cased && p in machine) fail(', 'if (0) fail('),
    'ids': ('if (kind == "id" && !(item[i] in idnew))', 'if (0)'),
    'type': ('else if (imagetype[p[3]] != p[2])', 'else if (0)'),
    'loops': ('if (volumes > 4)', 'if (volumes > 5)'),
}
old, new = faults[sys.argv[2]]
if source.count(old) != 1:
    raise SystemExit('nativecheck-mutation: fault text is not unique: ' + sys.argv[2])
if not os.environ.get('RACH2_FAULT_TEXTS'):
    path.write_text(source.replace(old, new))
PY
}

if [ "${1:-}" = --texts ]; then
    RACH2_FAULT_TEXTS=1
    export RACH2_FAULT_TEXTS
    for fault in both ids type loops; do
        apply_fault tools/verify/nativecheck.sh "$fault"
    done
    echo 'nativecheck-mutation: texts: every fault found once'
    exit 0
fi
work=
trap 'rm -rf ${work:+"$work"}' 0
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM
work=$(mktemp -d "${TMPDIR:-/tmp}/rach2-nativecheck-proof.XXXXXX")

# run_phase NAME STATUS: run the test against the private checker and require
# STATUS.
run_phase()
{
    status=0
    NATIVECHECK="$work/nativecheck.sh" sh tests/system/nativecheck_test.sh \
        > "$work/$1.log" 2>&1 || status=$?
    if [ "$status" -ne "$2" ]; then
        cat "$work/$1.log" >&2
        echo "nativecheck-mutation: $1: expected $2, got $status" >&2
        exit 1
    fi
    echo "nativecheck-mutation: result: $1 status $status"
}
cp tools/verify/nativecheck.sh "$work/nativecheck.sh"
run_phase fixed 0
for fault in both ids type loops; do
    cp tools/verify/nativecheck.sh "$work/nativecheck.sh"
    apply_fault "$work/nativecheck.sh" "$fault"
    run_phase "$fault-mutated" 1
done
cp tools/verify/nativecheck.sh "$work/nativecheck.sh"
run_phase restored 0
