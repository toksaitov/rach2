#!/bin/sh
# usb_latency.sh KERNEL IMAGE HARNESS: the two boots of the path from a
# keystroke's interrupt to its dispatch, on KERNEL, a harness kernel, which
# tests/proofs/usb-latency-guest.json runs on kernels with faults in them.
# The usb suite's cases of the same two boots are in
# tests/cases/machine.usb.json.
#
# Each row boots with a USB keyboard on an xHCI controller and
# `rach2.usblatency`, and runs keylatprobe, once beside a spinning loop and
# once beside a loop that reads getrandom. The row types a key for each of
# the probe's reads, each once the last key's record and the next read's
# wait are on the console. checkers/usblatency.awk judges the records, after
# usb_latency_test.sh has checked the checker; a `usb: pump:` line fails the
# row. The exit status is 0 when both pass.
#
# A key the timer's tick takes rather than the controller's interrupt has its
# reader woken and dispatched by that tick, preemption or not, so its record
# proves nothing about the switch that follows a device's interrupt. The
# checker requires the spin row to have one record taken by the interrupt,
# and eight keys make a row of keys all taken by the tick rare: about one in
# 6600 when a third are.
set -eu
kernel=$1
image=$2
harness=$3
repo=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
out=${RACH2_USB_LOG_PREFIX:-$(dirname "$image")/usb-test}
keys=8
fail=0
sh "$repo/tests/system/usb_latency_test.sh"
for mode in spin random; do
    log="$out.latency-$mode.log"
    runlog="$out.latency-$mode.harness.log"
    rm -f "$log" "$runlog"
    if [ "$mode" = random ]; then
        ready='KEYLAT HOG'
        random=1
    else
        ready='KEYLAT READING'
        random=0
    fi
    set -- "seen=$ready" "seen=usb: latency: reader waiting" key=ret
    typed=1
    while [ "$typed" -lt "$keys" ]; do
        set -- "$@" "expect=usb: latency: msi" "expect=usb: latency: reader waiting" key=ret
        typed=$((typed + 1))
    done
    if "$harness" --kernel "$kernel" --initrd "$image" \
        --net hubport,id=net0,hubid=0 --qemu-arg=-cpu --qemu-arg=max \
        --qemu-arg=-machine --qemu-arg=pc,i8042=off \
        --qemu-arg=-device --qemu-arg=qemu-xhci,id=xhci,msi=on \
        --qemu-arg=-device --qemu-arg=usb-kbd,bus=xhci.0 \
        --append "rach2.usblatency" --log "$log" \
        --timeout 120 --steptmo 45 --ok "KEYLAT OK" --never "KEYLAT FAIL" \
        "seen=class 3 protocol 1" "seen=Type 'help'" "send=exec /keylatprobe $mode $keys" \
        "$@" >"$runlog" 2>&1; then
        if awk -v random="$random" -v keys="$keys" \
            -f "$repo/tests/system/checkers/usblatency.awk" "$log" &&
            ! grep -q 'usb: pump:' "$log"; then
            echo "usb-test: result: $mode dispatch counts passed"
            grep 'usb: latency:' "$log"
        else
            echo "usb-test: invalid dispatch record: $mode" >&2
            cat "$log" >&2
            fail=1
        fi
    else
        echo "usb-test: keystroke boot failed: $mode" >&2
        cat "$runlog" >&2
        [ ! -f "$log" ] || cat "$log" >&2
        fail=1
        break
    fi
done
exit "$fail"
