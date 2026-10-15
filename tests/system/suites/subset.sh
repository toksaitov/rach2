#!/usr/bin/env sh
#
# subset.sh: the kernel with each optional device switched off.
#
# On a machine with no serial port, a boot that stops has no diagnostic but
# the screen, and the way to find which probe stopped it is to switch the
# probes off one at a time from the GRUB command line. Each switch is booted
# here.
#
# Reaching a shell does not show that a switch works, since a kernel that
# ignored every one of these options would reach one too. Each probe records
# `started` when it begins and `skipped: rach2.noX` when it is told not to,
# and a boot with the option requires the second record and forbids the
# first. A boot then leaves out all five at once, the case the options exist
# for, and an ordinary boot must run them all.
#
# Usage: subset.sh <kernel> <image> <harness>
#
set -u

KERNEL="$1"
IMAGE="$2"
HARNESS="$3"

# run_one OPTION SUBSYSTEM EXPECT...: boot with OPTION, which must skip
# SUBSYSTEM's probe, and require the EXPECT steps of three other probes that
# must still run; return 1 when the boot fails. Without the three, every check
# here would pass on a kernel where any one option switched every probe off.
run_one() {
    opt="$1"
    subsystem="$2"
    shift 2

    set -- "$HARNESS" --kernel "$KERNEL" --initrd "$IMAGE" \
        --qemu-arg=-cpu --qemu-arg=max --append "$opt" \
        --timeout 90 --ok "$subsystem: skipped: $opt" \
        --never "$subsystem: started" "$@" "expect=Type 'help'"
    if ! "$@" >/dev/null 2>&1; then
        echo "subset-test: FAIL: with [$opt] the machine did not skip" >&2
        echo "subset-test: $subsystem and run the other three" >&2
        return 1
    fi
    echo "subset-test: $opt: $subsystem skipped, the rest ran: OK"
}

run_one "rach2.noaudio" audio \
    "expect=mouse: started" "expect=block: started" "expect=net: started" || exit 1
run_one "rach2.nomouse" mouse \
    "expect=audio: started" "expect=block: started" "expect=net: started" || exit 1
run_one "rach2.nonet" net \
    "expect=mouse: started" "expect=audio: started" "expect=block: started" || exit 1
run_one "rach2.nodisk" block \
    "expect=mouse: started" "expect=audio: started" "expect=net: started" || exit 1
run_one "rach2.nousb" usb \
    "expect=mouse: started" "expect=audio: started" "expect=net: started" || exit 1

# All five at once, which is what a person types when the boot stops and they
# do not yet know which probe stopped it. Every probe must be skipped and the
# machine must still reach a shell. The five are named in the order the boot
# prints them, the USB stack after the root, because each `expect` reads on
# from where the last one matched.
if ! "$HARNESS" --kernel "$KERNEL" --initrd "$IMAGE" \
        --qemu-arg=-cpu --qemu-arg=max \
        --append "rach2.noaudio rach2.nomouse rach2.nonet rach2.nodisk rach2.nousb" \
        --timeout 90 --ok "Type 'help'" \
        --never "audio: started" --never "mouse: started" \
        --never "net: started" --never "block: started" --never "usb: started" \
        "expect=mouse: skipped: rach2.nomouse" \
        "expect=audio: skipped: rach2.noaudio" \
        "expect=block: skipped: rach2.nodisk" \
        "expect=usb: skipped: rach2.nousb" \
        "expect=net: skipped: rach2.nonet" \
        "expect=Type 'help'" >/dev/null 2>&1; then
    echo "subset-test: FAIL: with every device switched off the machine did" >&2
    echo "subset-test: not skip all five and reach a shell" >&2
    exit 1
fi
echo "subset-test: all five at once: OK"

# And with none of them named, every probe runs. Without this the checks above
# would pass on a kernel that skipped every device whatever it was told.
if ! "$HARNESS" --kernel "$KERNEL" --initrd "$IMAGE" \
        --qemu-arg=-cpu --qemu-arg=max \
        --timeout 90 --ok "Type 'help'" \
        --never "skipped: rach2.no" \
        "expect=mouse: started" "expect=audio: started" "expect=block: started" \
        "expect=usb: started" "expect=net: started" \
        "expect=Type 'help'" >/dev/null 2>&1; then
    echo "subset-test: FAIL: an ordinary boot did not run all five probes" >&2
    exit 1
fi
echo "subset-test: an ordinary boot runs all five: OK"
echo "subset-test: PASS"
