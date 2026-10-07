#!/bin/sh
# Compile the ATA model rows, optionally proving their write-count checks.
set -eu
case "${1-}" in
    '') [ "$#" -eq 0 ] ;;
    --mutate | --texts) [ "$#" -eq 1 ] ;;
    *) echo "ata-rate-test: usage: ata_rate_test.sh [--mutate | --texts]" >&2; exit 2 ;;
esac
repo=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
cd "$repo"
mkdir -p build/review-proof
work=$(mktemp -d "$repo/build/review-proof/ata-rate.XXXXXX")
mkdir -p "$work/rach2" "$work/tests/host"
cp -R rach2/sys "$work/rach2/sys"
cp tests/host/ata_hosttest.c "$work/tests/host/"
source="$work/rach2/sys/block/ata.c"
cp "$source" "$work/ata.saved.c"
child=

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

if [ "${1-}" = --texts ]; then
    source=rach2/sys/block/ata.c
    run_phase() { :; }
    restore() { :; }
    trap - EXIT
    RACH2_FAULT_TEXTS=1
    export RACH2_FAULT_TEXTS
else
    echo "ata-rate-test: logs: $work"
    run_phase fixed
    [ "${1-}" = --mutate ] || exit 0
fi
for fault in pio spin; do
    case "$fault" in
        pio) failure='a DMA write uses one data command per 32 KiB and no data-port sectors' ;;
        spin) failure='a delayed write and flush use at most 32 status reads per 32 KiB' ;;
    esac
    python3 - "$source" "$fault" <<'PY'
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
    run_phase "$fault-mutated"
    restore
    run_phase "$fault-restored"
    cmp "$work/ata.saved.c" "$source"
done
if [ "${1-}" = --texts ]; then
    rm -rf "$work"
    echo 'ata-rate-test: texts: every fault is once in its source'
fi
