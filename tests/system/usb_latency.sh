#!/bin/sh
# The two USB wake-to-dispatch rows, also run by suites/usb.sh.
set -eu
kernel=$1
image=$2
harness=$3
repo=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
out=${RACH2_USB_LOG_PREFIX:-$(dirname "$image")/usb-test}
fail=0
sh "$repo/tests/system/usb_latency_test.sh"
for mode in spin random; do
    log="$out.latency-$mode.log"
    runlog="$out.latency-$mode.harness.log"
    rm -f "$log" "$runlog"
    if [ "$mode" = random ]; then
        command='exec /keylatprobe random'
        ready='KEYLAT HOG'
        random=1
    else
        command='exec /keylatprobe'
        ready='KEYLAT READING'
        random=0
    fi
    if "$harness" --kernel "$kernel" --initrd "$image" \
        --net hubport,id=net0,hubid=0 --qemu-arg=-cpu --qemu-arg=max \
        --qemu-arg=-machine --qemu-arg=pc,i8042=off \
        --qemu-arg=-device --qemu-arg=qemu-xhci,id=xhci,msi=on \
        --qemu-arg=-device --qemu-arg=usb-kbd,bus=xhci.0 \
        --append "rach2.usblatency" --log "$log" \
        --timeout 120 --steptmo 45 --ok "KEYLAT OK" --never "KEYLAT FAIL" \
        "seen=class 3 protocol 1" "seen=Type 'help'" "send=$command" \
        "seen=$ready" "seen=usb: latency: reader waiting" key=ret \
        >"$runlog" 2>&1; then
        if awk -v random="$random" -f "$repo/tests/system/checkers/usblatency.awk" "$log" &&
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
