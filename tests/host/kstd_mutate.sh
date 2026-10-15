#!/bin/sh
# Prove kstd_hosttest.c's rows on k_itoa with a private mutation of
# rach2/sys/lib/kstd.c.
#
# The fault is a plus `force_sign` counts and does not write. The test's status
# is its count of failed checks, so the faulty build must end with status 1,
# and the fixed and restored builds with 0.
set -eu

repo=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
cd "$repo"

# apply_fault FILE: put the fault into FILE, a copy of kstd.c; with
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
old = ("        } else if (is_signed && initial_input > 0 && force_sign) {\n"
       "            *tmp_str++ = '+';\n"
       "        }\n")
if source.count(old) != 1:
    raise SystemExit("kstd-mutation: source pattern is not unique: plus")
path.write_text(source.replace(old, "        }\n"))
PY
}

if [ "${1:-}" = --texts ]; then
    RACH2_FAULT_TEXTS=1
    export RACH2_FAULT_TEXTS
    apply_fault rach2/sys/lib/kstd.c >/dev/null
    echo 'kstd-mutation: texts: every fault is once in its source'
    exit 0
fi

. "$repo/tests/system/runlock.sh"
runlock_hold "$repo" kstd-mutation "$repo/tests/host/kstd_mutate.sh" "$@"


mkdir -p build/tests
work=$(mktemp -d "$repo/build/tests/kstd-proof.XXXXXX")
cp -R rach2/sys/lib "$work/lib"
cp -R rach2/sys/arch "$work/arch"
cp "$work/lib/kstd.c" "$work/kstd.saved"
child=

# cleanup: stop the running test and put the saved kstd.c back; the script
# calls it after the fault, and it is the exit trap.
cleanup()
{
    if [ -n "$child" ]; then
        kill -TERM "$child" 2>/dev/null || :
        wait "$child" 2>/dev/null || :
        child=
    fi
    cp "$work/kstd.saved" "$work/lib/kstd.c"
    touch "$work/lib/kstd.c"
}
trap cleanup EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

# run_phase PHASE STATUS: compile kstd_hosttest.c over the copy as
# $work/PHASE, run it for at most 15 seconds, and exit unless it ends with
# STATUS.
run_phase()
{
    phase=$1
    expected=$2
    binary="$work/$phase"
    if ! "${CC:-cc}" -std=c99 -D_DEFAULT_SOURCE -D_FORTIFY_SOURCE=0 \
        -DRACH2_HOSTTEST -g -O1 -Wno-empty-body -ffreestanding \
        -fsanitize=address,undefined -fno-sanitize-recover=all \
        -I "$work/lib" tests/host/kstd_hosttest.c "$work/lib/kstd.c" \
        "$work/lib/kstring.c" "$work/lib/kctypes.c" -o "$binary" \
        >"$binary.compile.log" 2>&1; then
        cat "$binary.compile.log" >&2
        exit 1
    fi
    cksum "$work/lib/kstd.c" "$binary" >"$binary.cksum"
    perl -e 'alarm 15; exec @ARGV or die "kstd-mutation: exec: $!\n"' \
        "$binary" >"$binary.log" 2>&1 &
    child=$!
    status=0
    wait "$child" || status=$?
    child=
    cat "$binary.log"
    [ "$status" -eq "$expected" ] || exit 1
    echo "kstd-mutation: result: $phase status $status"
}

echo "kstd-mutation: logs: $work"
run_phase fixed 0
apply_fault "$work/lib/kstd.c"
run_phase plus-mutated 1
cleanup
run_phase plus-restored 0
cmp "$work/kstd.saved" "$work/lib/kstd.c"
