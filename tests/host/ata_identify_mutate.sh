#!/bin/sh
# Prove the ATA driver's refusal of a drive whose logical sector is not 512
# bytes, entry 11 of milestone 50, with private mutations of its IDENTIFY
# check in rach2/sys/block/ata.c.
#
# Usage: ata_identify_mutate.sh [--check | --texts]
# With --check it compiles and runs the unmodified test alone. The faults:
# the check dropped, which takes a 4096-byte drive; and the check reading
# bit 12 of word 106 without the word's valid bit, which refuses a drive
# whose word 106 says nothing.
set -eu

repo=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
cd "$repo"

# apply_fault ARGS...: put one fault into its source; with
# RACH2_FAULT_TEXTS set, only check its text and write nothing.
apply_fault()
{
    python3 - "$1" "$2" <<'PY'
from pathlib import Path
import sys
import os
import pathlib
if os.environ.get('RACH2_FAULT_TEXTS'):
    pathlib.Path.write_text = lambda self, *args, **kwargs: 0

path = Path(sys.argv[1])
source = path.read_text()
start = source.index('static int ata_identify(')
end = source.index('\n}\n', start)
body = source[start:end]
check = ('    if ((identify[106] & 0x4000u) != 0 && (identify[106] & 0x1000u) != 0) {\n'
         '        klog(KLOG_ERR, "ata", "%s %s: refused: the logical sector is not 512 bytes", ch->name,\n'
         '             d->slave ? "slave" : "master");\n'
         '        return -1;\n'
         '    }\n')
if body.count(check) != 1:
    raise SystemExit('ata-identify-proof: the sector size check is not unique')
if sys.argv[2] == 'dropped':
    body = body.replace(check, '')
else:
    body = body.replace(check, check.replace(
        '(identify[106] & 0x4000u) != 0 && (identify[106] & 0x1000u) != 0',
        '(identify[106] & 0x1000u) != 0'))
path.write_text(source[:start] + body + source[end:])
PY
}

faults='dropped unvalidated'
if [ "${1:-}" = --texts ]; then
    for fault in $faults; do
        RACH2_FAULT_TEXTS=1 apply_fault rach2/sys/block/ata.c "$fault" >/dev/null || exit 1
    done
    echo 'ata-identify-proof: texts: every fault is once in its source'
    exit 0
fi

. "$repo/tests/system/runlock.sh"
runlock_hold "$repo" ata-identify-proof "$repo/tests/host/ata_identify_mutate.sh" "$@"

[ "$#" -eq 0 ] || [ "$*" = --check ] || {
    echo 'ata-identify-proof: usage: ata_identify_mutate.sh [--check]' >&2
    exit 2
}
mkdir -p build/tests
work=$(mktemp -d "$repo/build/tests/ata-identify-proof.XXXXXX")
mkdir -p "$work/rach2" "$work/tests/host"
cp -R rach2/sys "$work/rach2/sys"
cp tests/host/ata_hosttest.c "$work/tests/host/"
source="$work/rach2/sys/block/ata.c"
cp "$source" "$work/source.saved"
child=

cleanup()
{
    if [ -n "$child" ]; then
        kill -TERM "-$child" 2>/dev/null || :
        wait "$child" 2>/dev/null || :
        while kill -0 "-$child" 2>/dev/null; do
            sleep 1
        done
        child=
    fi
    cp "$work/source.saved" "$source"
    touch "$source"
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
    if ! "${CC:-cc}" -std=c99 -D_DEFAULT_SOURCE -D_FORTIFY_SOURCE=0 -DRACH2_HOSTTEST \
        -fsanitize=address,undefined -fno-sanitize-recover=all -g -O1 \
        -Wno-empty-body -ffreestanding \
        -I "$work/rach2/sys" -I "$work/rach2/sys/lib" -I "$work/rach2/sys/proc" \
        -I utils -I krffs -I tests/host/compat \
        -o "$binary" "$work/tests/host/ata_hosttest.c" \
        "$work/rach2/sys/lib/kstring.c" "$work/rach2/sys/lib/kctypes.c" \
        >"$binary.compile.log" 2>&1; then
        cat "$binary.compile.log" >&2
        exit 1
    fi
    cksum "$source" "$binary" >"$binary.cksum"
    perl -e 'setpgrp(0, 0) or die "ata-identify-proof: setpgrp: $!\n";
             alarm 60; exec @ARGV or die "ata-identify-proof: exec: $!\n"' \
        "$binary" >"$binary.log" 2>&1 &
    child=$!
    status=0
    wait "$child" || status=$?
    cleanup
    cat "$binary.log"
    [ "$status" -eq "$expected" ] && grep -Fq "$message" "$binary.log" || exit 1
    echo "ata-identify-proof: result: $phase status $status"
}

echo "ata-identify-proof: logs: $work"
run_phase fixed 0 'ata-test: OK'
[ "$#" -eq 0 ] || exit 0
for fault in $faults; do
    apply_fault "$source" "$fault"
    touch "$source"
    case $fault in
        dropped) message='ata-test: fail: IDENTIFY with a 4096-byte logical sector is refused' ;;
        unvalidated)
            message='ata-test: fail: bit 12 of word 106 without its valid bit says nothing of the sector'
            ;;
    esac
    run_phase "$fault-mutated" 1 "$message"
    cmp "$source" "$work/source.saved"
    run_phase "$fault-restored" 0 'ata-test: OK'
    cmp "$source" "$work/source.saved"
done
echo 'ata-identify-proof: result: pass: both compiled faults failed their rows and the restored source passed'
