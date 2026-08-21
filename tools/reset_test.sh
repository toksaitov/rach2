#!/usr/bin/env sh

#
# reset_test.sh: each restart mechanism, on its own.
#
# The kernel asks the machine to restart three ways, most of the machine first:
# the firmware's own SMI handler, then the chipset reset register, then the
# keyboard controller pulse. That order was chosen for a laptop whose chipset
# register may not act, and the risk in choosing it is plain: if only the first
# mechanism is ever exercised, nobody finds out the later ones do not work
# until the machine they were written for refuses to restart, which over SSH
# looks exactly like a kernel that died.
#
# So each is booted with the ones before it left out (`rach2.resetskip=N`), and
# each must restart the machine on its own.
#
# What "restarted" means here needs saying, because the obvious check does not
# work: the harness runs QEMU with -no-reboot, so a machine that resets exits
# instead of booting again, and waiting for a second boot banner would fail on
# success. The signal is the other way round. `power_reset` falls through to a
# halt and says so when every mechanism it tried did nothing, so a run that
# reached the restart and never printed that fallback is a run where one of
# them acted.
#
# Usage: reset_test.sh <kernel> <image>
#

set -e

KERNEL="$1"
IMAGE="$2"

if [ -z "$KERNEL" ] || [ -z "$IMAGE" ]; then
    echo "usage: reset_test.sh <kernel> <image>" >&2
    exit 2
fi

REPO="$(cd "$(dirname "$0")/.." && pwd)"
LOG="$REPO/build/images/reset-test.log"

cleanup() {
    rm -f "$LOG"
}
trap 'cleanup' EXIT
trap 'cleanup; exit 130' INT
trap 'cleanup; exit 143' TERM
trap 'cleanup; exit 129' HUP

for skip in 0 1 2; do
    case "$skip" in
        0) what="all three, the firmware handler first" ;;
        1) what="the chipset register and the keyboard controller" ;;
        2) what="the keyboard controller alone" ;;
        *) what="" ;;
    esac
    rm -f "$LOG"
    python3 "$REPO/tools/harness.py" --kernel "$KERNEL" --initrd "$IMAGE" \
        --qemu-arg=-cpu --qemu-arg=max \
        --append "rach2.rescue rach2.resetskip=$skip" \
        --timeout 90 --log "$LOG" --ok "reboot: restarting" \
        "expect=Type 'help'" "send=reboot" wait=6 >/dev/null 2>&1 || true

    if ! grep -q "reboot: restarting (skipping $skip)" "$LOG"; then
        echo "reset-test: FAIL: the machine never reached the restart with skip=$skip" >&2
        exit 1
    fi
    if grep -q "the platform did not reset" "$LOG"; then
        echo "reset-test: FAIL: with $what, nothing restarted the machine" >&2
        exit 1
    fi
    echo "reset-test: $what: OK"
done

echo "reset-test: PASS"
