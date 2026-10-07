#!/bin/sh
# Prove the USB record checker with compiled production recorder and scheduler.
# --mutate adds the faults below; --texts checks their texts and builds nothing.
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

root = Path(sys.argv[1])
if sys.argv[2] == 'cutoff':
    path = root / 'usb/stamp.c'
    old = '    result = stamp;'
    new = '''    if (read_us - stamp.wake > 20000u) {
        kirq_restore(flags);
        return;
    }
''' + old
else:
    path = root / 'proc/sched.c'
    old = '    usb_stamp_dispatch(next->pid);'
    new = '    (void)next->pid;'
source = path.read_text()
assert source.count(old) == 1
path.write_text(source.replace(old, new))
PY
}

if [ "${1:-}" = --texts ]; then
    RACH2_FAULT_TEXTS=1
    export RACH2_FAULT_TEXTS
    apply_fault rach2/sys cutoff >/dev/null
    apply_fault rach2/sys dispatch >/dev/null
    echo 'usb-latency-test: texts: every fault is once in its source'
    exit 0
fi


mkdir -p build/tests
work=$(mktemp -d "$repo/build/tests/usb-latency-proof.XXXXXX")
cp -R rach2/sys "$work/sys"
cp "$work/sys/usb/stamp.c" "$work/stamp.saved"
cp "$work/sys/proc/sched.c" "$work/sched.saved"
child=

restore()
{
    if [ -n "$child" ]; then
        kill -TERM "$child" 2>/dev/null || :
        wait "$child" 2>/dev/null || :
        child=
    fi
    cp "$work/stamp.saved" "$work/sys/usb/stamp.c"
    cp "$work/sched.saved" "$work/sys/proc/sched.c"
    touch "$work/sys/usb/stamp.c" "$work/sys/proc/sched.c"
}
trap restore EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

run_phase()
{
    phase=$1
    expected=$2
    "${CC:-cc}" -std=c99 -DRACH2_HOSTTEST -g -O1 -ffreestanding \
        -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=all \
        -I "$work/sys" -I "$work/sys/lib" -I "$work/sys/proc" \
        tests/host/usbstamp_hosttest.c -o "$work/test" >"$work/$phase.build.log" 2>&1 || {
            cat "$work/$phase.build.log" >&2; exit 1;
        }
    cp "$work/test" "$work/$phase.binary"
    cksum "$work/sys/usb/stamp.c" "$work/sys/proc/sched.c" "$work/test" \
        >"$work/$phase.cksum"
    "$work/test" >"$work/$phase.log" 2>&1 &
    child=$!
    status=0
    wait "$child" || status=$?
    child=
    cat "$work/$phase.log"
    [ "$status" -eq "$expected" ] || exit 1
    for mode in spin timer random pump work undispatched; do
        status=0
        "$work/test" "$mode" >"$work/$phase.$mode.log" 2>&1 &
        child=$!
        wait "$child" || status=$?
        child=
        if [ "$expected" -eq 0 ]; then
            [ "$status" -eq 0 ] || exit 1
        fi
        want=$expected
        case $mode in pump|work|undispatched) want=1 ;; esac
        random=0
        case $mode in random|work) random=1 ;; esac
        status=0
        awk -v random="$random" -f tests/system/checkers/usblatency.awk \
            "$work/$phase.$mode.log" || status=$?
        [ "$status" -eq "$want" ] || {
            echo "usb-latency-test: wrong checker verdict: $phase $mode" >&2
            cat "$work/$phase.$mode.log" >&2
            exit 1
        }
    done
    echo "usb-latency-test: result: $phase matched expected status $expected"
}

echo "usb-latency-test: logs: $work"
run_phase fixed 0
if [ "${1:-}" = --mutate ]; then
    for fault in cutoff dispatch; do
        apply_fault "$work/sys" "$fault"
        run_phase "$fault-mutated" 1
        if cmp -s "$work/fixed.binary" "$work/$fault-mutated.binary"; then
            echo "usb-latency-test: mutation did not change the binary" >&2; exit 1
        fi
        restore
        run_phase "$fault-restored" 0
        cmp "$work/stamp.saved" "$work/sys/usb/stamp.c"
        cmp "$work/sched.saved" "$work/sys/proc/sched.c"
    done
fi

sed 's/ tracked 1,/ tracked x,/' "$work/fixed.spin.log" >"$work/malformed.log"
sed 's/ dispatch 1,/ dispatch 1, dispatch 1,/' "$work/fixed.spin.log" >"$work/duplicate.log"
sed 's/ pumps 0,//' "$work/fixed.spin.log" >"$work/missing.log"
: >"$work/empty.log"
for kind in malformed duplicate missing empty; do
    if awk -f tests/system/checkers/usblatency.awk "$work/$kind.log"; then
        echo "usb-latency-test: checker accepted $kind record" >&2; exit 1
    fi
done
echo "usb-latency-test: result: pass"
