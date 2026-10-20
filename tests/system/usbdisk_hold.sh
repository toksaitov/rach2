#!/bin/sh
# usbdisk_hold.sh KERNEL: boot KERNEL with the test image and the usbdisk
# suite's MBR stick, and remove the stick while usbdiskprobe holds a nested
# mount on it open, in its holdnested and holdnestedexfat modes, on the
# architecture ARCH names, i686 by default.
#
# Each mode must pass: the removed mount must stop resolving, a cached
# directory must stop listing, and the removed child mount must keep its
# open description. Both modes run whatever the first gives; the script
# fails if either does. The sticks are made by usbdisk_sticks.sh in a
# directory under build/tests, which the script removes on every exit, and
# each boot's console is printed. tests/proofs/usbdisk.json runs it.
set -eu
repo=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
cd "$repo"
[ "$#" -eq 1 ] || { echo 'usage: usbdisk_hold.sh KERNEL' >&2; exit 2; }
kernel=$1
RACH2_ARCH=${RACH2_ARCH:-${ARCH:-i686}}
REPO=$repo
. "$repo/mk/arch.sh"
RACH2_QEMU=$ARCH_QEMU
export RACH2_QEMU
work=
trap 'rm -rf ${work:+"$work"}' EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM
mkdir -p build/tests
work=$(mktemp -d "$repo/build/tests/usbdisk-hold.XXXXXX")
sh tests/system/usbdisk_sticks.sh "$work" build/tests/host/mkexfatfixture

failed=0
for mode in holdnested holdnestedexfat; do
    cp "$work/stick-mbr.img" "$work/$mode.img"
    status=0
    build/tests/system/harness --kernel "$kernel" --initrd "$ARCH_BUILD/images/test.krffs" \
        --qemu-arg=-cpu --qemu-arg=max \
        --qemu-arg=-device --qemu-arg=qemu-xhci,id=xhci \
        --qemu-arg=-device --qemu-arg=usb-kbd,bus=xhci.0 \
        --qemu-arg=-device --qemu-arg=usb-mouse,bus=xhci.0 \
        --drive "if=none,id=stick,format=raw,file=$work/$mode.img" \
        --qemu-arg=-device --qemu-arg=usb-storage,bus=xhci.0,drive=stick,id=st0 \
        --timeout 180 --steptmo 60 --log "$work/$mode.serial.log" \
        --ok 'USBDISKPROBE OK' --never 'USBDISKPROBE FAIL' \
        "expect=Type 'help'" "send=exec /usbdiskprobe $mode" \
        'expect=USBDISKPROBE HOLDING' 'usb=del:st0' \
        'expect=USBDISKPROBE ' >"$work/$mode.log" 2>&1 || status=$?
    cat "$work/$mode.log" "$work/$mode.serial.log"
    if [ "$status" -eq 0 ]; then
        echo "usbdisk-hold: result: $mode: pass"
    else
        echo "usbdisk-hold: fail: $mode: status $status"
        failed=1
    fi
done
exit "$failed"
