#!/usr/bin/env sh
#
# subset.sh: the kernel with each optional device switched off.
#
# On a machine with no serial port, a boot that stops has no diagnostic but the
# screen, and the only way to find which probe stopped it is to switch them off
# one at a time from the GRUB command line. That is worth having only if each
# switch works, so each is booted here.
#
# Reaching a shell is not enough to show that, and used to be all this asked: a
# kernel that ignored every one of these options would still reach one. So each
# probe says `started` when it begins and `skipped: rach2.noX` when it is told
# not to, and a boot with the option requires the second record and forbids the
# first. The last boot leaves out all four at once, which is the case the
# option exists for.
#
# Usage: subset.sh <kernel> <image> <harness>
#
set -u

KERNEL="$1"
IMAGE="$2"
HARNESS="$3"

# Each option, and the subsystem whose records it decides.
run_one() {
    opt="$1"
    subsystem="$2"

    if ! "$HARNESS" --kernel "$KERNEL" --initrd "$IMAGE" \
            --qemu-arg=-cpu --qemu-arg=max --append "$opt" \
            --timeout 90 --ok "$subsystem: skipped: $opt" \
            --never "$subsystem: started" \
            "expect=Type 'help'" >/dev/null 2>&1; then
        echo "subset-test: FAIL: with [$opt] the machine did not skip $subsystem" >&2
        return 1
    fi
    echo "subset-test: $opt: $subsystem was skipped: OK"
}

run_one "rach2.noaudio" audio || exit 1
run_one "rach2.nomouse" mouse || exit 1
run_one "rach2.nonet" net || exit 1
run_one "rach2.nodisk" block || exit 1

# All four at once, which is what a person types when the boot stops and they
# do not yet know which probe did it. Every probe must be skipped and the
# machine must still reach a shell. The four are named in the order the boot
# prints them, because each `expect` reads on from where the last one matched.
if ! "$HARNESS" --kernel "$KERNEL" --initrd "$IMAGE" \
        --qemu-arg=-cpu --qemu-arg=max \
        --append "rach2.noaudio rach2.nomouse rach2.nonet rach2.nodisk" \
        --timeout 90 --ok "Type 'help'" \
        --never "audio: started" --never "mouse: started" \
        --never "net: started" --never "block: started" \
        "expect=mouse: skipped" "expect=audio: skipped" \
        "expect=block: skipped" "expect=net: skipped" \
        "expect=Type 'help'" >/dev/null 2>&1; then
    echo "subset-test: FAIL: with every device switched off the machine did" >&2
    echo "subset-test: not skip all four and reach a shell" >&2
    exit 1
fi
echo "subset-test: all four at once: OK"

# And with none of them named, every probe runs. Without this the checks above
# would pass on a kernel that skipped every device whatever it was told.
if ! "$HARNESS" --kernel "$KERNEL" --initrd "$IMAGE" \
        --qemu-arg=-cpu --qemu-arg=max \
        --timeout 90 --ok "Type 'help'" \
        --never "skipped: rach2.no" \
        "expect=mouse: started" "expect=audio: started" \
        "expect=block: started" "expect=net: started" \
        "expect=Type 'help'" >/dev/null 2>&1; then
    echo "subset-test: FAIL: an ordinary boot did not run all four probes" >&2
    exit 1
fi
echo "subset-test: an ordinary boot runs all four: OK"
echo "subset-test: PASS"
