#!/bin/sh
# Prove the row of tests/host/pagecache_ring_hosttest.c against the fault it
# was written for, entry 110 of milestone 50: the page cache back at 1024
# slots, where a compile's lookups over the ring with the manual staged read
# the disk again every round. The fault is compiled into a private copy of
# rach2/sys/mm/pagecache.h and must fail the row; the copy restored, touched
# and rebuilt must pass. The test includes pagecache.c whole, as
# tests/system/suites/host.sh compiles it, from the private copy.
#
# Usage: pagecache_ring_mutate.sh [--texts]
set -eu
repo=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
cd "$repo"

# apply_fault ARGS...: put one fault into its source; with
# RACH2_FAULT_TEXTS set, only check its text and write nothing.
apply_fault()
{
    python3 - "$1" <<'PY'
import pathlib
import sys
import os
if os.environ.get('RACH2_FAULT_TEXTS'):
    pathlib.Path.write_text = lambda self, *args, **kwargs: 0

path = pathlib.Path(sys.argv[1])
text = path.read_text()
old = '#define PC_SLOTS 2048u\n'
if text.count(old) != 1:
    raise SystemExit('pagecache-ring-proof: the slots are not defined once as 2048')
path.write_text(text.replace(old, '#define PC_SLOTS 1024u\n'))
PY
}

if [ "${1:-}" = --texts ]; then
    RACH2_FAULT_TEXTS=1
    export RACH2_FAULT_TEXTS
    apply_fault rach2/sys/mm/pagecache.h >/dev/null
    echo 'pagecache-ring-proof: texts: every fault is once in its source'
    exit 0
fi


[ "$#" -eq 0 ] || {
    echo 'usage: pagecache_ring_mutate.sh' >&2
    exit 2
}
mkdir -p build/tests
work=$(mktemp -d "$repo/build/tests/pagecache-ring-proof.XXXXXX")
child=
cp -R rach2/sys "$work/sys"
source="$work/sys/mm/pagecache.h"
cp "$source" "$work/pagecache.saved"

restore()
{
    cp "$work/pagecache.saved" "$source"
    touch "$source"
}
cleanup()
{
    if [ -n "$child" ]; then
        kill -TERM "$child" 2>/dev/null || :
        wait "$child" 2>/dev/null || :
        child=
    fi
    restore
}
trap cleanup EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

# run_phase PHASE STATUS MESSAGE: compile and run the test; it must exit
# with STATUS and print MESSAGE.
run_phase()
{
    phase=$1
    expected=$2
    message=$3
    binary="$work/$phase"
    if ! "${CC:-cc}" -std=c99 -D_DEFAULT_SOURCE -D_FORTIFY_SOURCE=0 -DRACH2_HOSTTEST \
        -g -O1 -ffreestanding -Wno-empty-body -Wall -Wextra -Werror \
        -fsanitize=address,undefined -fno-sanitize-recover=all \
        -I "$work/sys" -I "$work/sys/lib" -I "$work/sys/proc" \
        tests/host/pagecache_ring_hosttest.c -o "$binary" >"$binary.compile.log" 2>&1; then
        cat "$binary.compile.log" >&2
        exit 1
    fi
    cksum "$source" "$binary" >"$binary.cksum"
    perl -e 'alarm 60; exec @ARGV or die "pagecache-ring-proof: exec: $!\n"' \
        "$binary" >"$binary.log" 2>&1 &
    child=$!
    status=0
    wait "$child" || status=$?
    child=
    cat "$binary.log"
    [ "$status" -eq "$expected" ] && grep -Fq "$message" "$binary.log" || exit 1
    echo "pagecache-ring-proof: result: $phase status $status"
}

echo "pagecache-ring-proof: logs: $work"
run_phase fixed 0 'pagecache_ring_hosttest: result: pass:'
apply_fault "$source"
run_phase slots-1024 1 \
    "pagecache_ring_hosttest: FAIL: at PC_SLOTS a compile's lookups over the ring with the manual read nothing from the disk once warm"
restore
run_phase restored 0 'pagecache_ring_hosttest: result: pass:'
cmp "$work/pagecache.saved" "$source"
echo 'pagecache-ring-proof: result: pass: 1024 slots failed the row and the restored 2048 passed'
