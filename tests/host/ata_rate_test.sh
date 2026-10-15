#!/bin/sh
# Compile and run ata_hosttest.c for the diskrate suite; with --mutate, also
# prove its two write-count checks with private mutations of ata.c.
#
# Usage: ata_rate_test.sh [--mutate | --texts]
# The faults: `pio` turns off the DMA path of ata_write_on, and `spin` raises
# ATA_SPIN_BEFORE_SLEEP from 8 status reads to 512.
set -eu
case "${1-}" in
    '') [ "$#" -eq 0 ] ;;
    --mutate | --texts) [ "$#" -eq 1 ] ;;
    *) echo "ata-rate-test: usage: ata_rate_test.sh [--mutate | --texts]" >&2; exit 2 ;;
esac
repo=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
cd "$repo"

# apply_fault FILE FAULT: put FAULT into FILE, a copy of ata.c; with
# RACH2_FAULT_TEXTS set, only check that its text is once in FILE.
apply_fault()
{
    python3 - "$1" "$2" <<'PY'
import os
import pathlib
import sys

path = pathlib.Path(sys.argv[1])
text = path.read_text()
if sys.argv[2] == "pio":
    start = text.index("static int ata_write_on(")
    prefix, body = text[:start], text[start:]
    old = "if (d->dma && owned)"
    new = "if (0 && d->dma && owned)"
else:
    prefix, body = "", text
    old = "#define ATA_SPIN_BEFORE_SLEEP 8"
    new = "#define ATA_SPIN_BEFORE_SLEEP 512"
if body.count(old) != 1:
    raise SystemExit("ata-rate-test: source pattern is not unique")
if not os.environ.get("RACH2_FAULT_TEXTS"):
    path.write_text(prefix + body.replace(old, new))
PY
}

if [ "${1-}" = --texts ]; then
    for fault in pio spin; do
        RACH2_FAULT_TEXTS=1 apply_fault rach2/sys/block/ata.c "$fault"
    done
    echo 'ata-rate-test: texts: every fault is once in its source'
    exit 0
fi

. "$repo/tests/system/runlock.sh"
runlock_hold "$repo" ata-rate-test "$repo/tests/host/ata_rate_test.sh" "$@"
mkdir -p build/review-proof
work=$(mktemp -d "$repo/build/review-proof/ata-rate.XXXXXX")
mkdir -p "$work/rach2" "$work/tests/host"
cp -R rach2/sys "$work/rach2/sys"
cp tests/host/ata_hosttest.c "$work/tests/host/"
source="$work/rach2/sys/block/ata.c"
cp "$source" "$work/ata.saved.c"
child=

# restore: stop the running test and put the saved ata.c back; it is also the
# exit trap.
restore()
{
    if [ -n "$child" ]; then
        kill -TERM "$child" 2>/dev/null || :
        wait "$child" 2>/dev/null || :
        child=
    fi
    cp "$work/ata.saved.c" "$source"
    touch "$source"
}
trap restore EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

# run_phase PHASE: compile ata_hosttest.c over the copy and run it for at most
# 30 seconds. A PHASE ending in -mutated must fail with $failure in its log and
# a binary that differs from the fixed one; any other must pass. Exit if not.
run_phase()
{
    phase=$1
    binary="$work/ata_hosttest"
    if ! "${CC:-cc}" -std=c99 -DRACH2_HOSTTEST -g -O1 -ffreestanding \
        -fsanitize=address,undefined -fno-sanitize-recover=all \
        -I "$work/rach2/sys" -I "$work/rach2/sys/lib" \
        "$work/tests/host/ata_hosttest.c" "$work/rach2/sys/lib/kstring.c" \
        "$work/rach2/sys/lib/kctypes.c" -o "$binary" \
        >"$work/$phase.compile.log" 2>&1; then
        cat "$work/$phase.compile.log" >&2
        exit 1
    fi
    cp "$binary" "$work/$phase.binary"
    cksum "$source" "$binary" > "$work/$phase.cksum"
    perl -e 'alarm 30; exec @ARGV or die "ata-rate-test: exec: $!\n"' \
        "$binary" >"$work/$phase.log" 2>&1 &
    child=$!
    status=0
    wait "$child" || status=$?
    child=
    cat "$work/$phase.log"
    case "$phase" in
        *-mutated)
            [ "$status" -eq 1 ] && grep -Fq "$failure" "$work/$phase.log" || exit 1
            if cmp -s "$work/fixed.binary" "$binary"; then
                echo "ata-rate-test: mutation did not change the binary" >&2
                exit 1
            fi ;;
        *) [ "$status" -eq 0 ] ;;
    esac
    echo "ata-rate-test: result: $phase status $status"
}

echo "ata-rate-test: logs: $work"
run_phase fixed
[ "${1-}" = --mutate ] || exit 0
for fault in pio spin; do
    case "$fault" in
        pio) failure='a DMA write uses one data command per 32 KiB and no data-port sectors' ;;
        spin) failure='a delayed write and flush use at most 32 status reads per 32 KiB' ;;
    esac
    apply_fault "$source" "$fault"
    run_phase "$fault-mutated"
    restore
    run_phase "$fault-restored"
    cmp "$work/ata.saved.c" "$source"
done
