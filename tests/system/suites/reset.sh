#!/usr/bin/env sh

#
# reset.sh: each restart mechanism, on its own.
#
# The kernel asks the machine to restart up to four ways (`power_reset`,
# rach2/sys/arch/x86/power.c): the FADT's reset register, where the FADT has
# one, then the firmware's own SMI handler, then the chipset reset register,
# then the keyboard controller's pulse. QEMU's FADT has none, and the suite
# boots the other three with the first N left out (`rach2.resetskip=N`), so
# that each must restart the machine on its own. A later mechanism that does
# not work would otherwise be found only on a machine where the earlier ones
# fail, and a restart that does not happen looks over SSH like a kernel that
# died.
#
# The harness runs QEMU with -no-reboot, so a machine that resets exits in
# place of booting again, and a restart is judged by three things together.
# The machine must log the restart. It must not print the fallback, which
# `power_reset` prints when no mechanism it tried acted. And it must end by
# itself: `--must-exit` fails a run killed at its deadline, as a kernel that
# logged the restart and then hung would be. A panic, which also ends the
# machine, is refused by `--never`.
#
# The kernel sends the firmware's command only to Insyde's firmware, and both
# of its answers are booted: SeaBIOS, which QEMU boots by default, must log
# that the command was not sent, and a machine given
# `-smbios type=0,vendor=Insyde Corp.`, which makes QEMU publish that vendor's
# name, must log that it was.
#
# A triple fault also resets the machine, and with -no-reboot QEMU exits for
# it as it does for a reset the kernel asked for. The kernel prints a `CRIT`
# record for a kernel exception, which `--never` refuses, so the case the
# suite cannot separate is a triple fault that printed nothing.
#
# Usage: reset.sh <kernel> <image>
#

set -e

# The build tree of the architecture tests/system/suites.mk names.
ARCH_BUILD=${ARCH_BUILD:-build}

KERNEL="$1"
IMAGE="$2"

if [ -z "$KERNEL" ] || [ -z "$IMAGE" ]; then
    echo "usage: reset.sh <kernel> <image>" >&2
    exit 2
fi

REPO="$(cd "$(dirname "$0")/../../.." && pwd)"
LOG="$REPO/$ARCH_BUILD/images/reset-test.log"

# Remove the log and the harness's output.
cleanup() {
    rm -f "$LOG" "$LOG.harness"
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
    "$REPO/build/tests/system/harness" --kernel "$KERNEL" --initrd "$IMAGE" \
        --qemu-arg=-cpu --qemu-arg=max \
        --append "rach2.rescue rach2.resetskip=$skip" \
        --timeout 90 --log "$LOG" --ok "reboot: restarting" \
        --must-exit --never "<2>" \
        "expect=Type 'help'" "send=reboot" until=10 >"$LOG.harness" 2>&1 && rc=0 || rc=$?

    if [ "$rc" -ne 0 ]; then
        echo "reset-test: FAIL: with $what, the machine did not restart:" >&2
        sed -n 's/^HARNESS: /reset-test: /p' "$LOG.harness" >&2
        rm -f "$LOG.harness"
        exit 1
    fi
    rm -f "$LOG.harness"
    if ! grep -q "reboot: restarting (skipping $skip)" "$LOG"; then
        echo "reset-test: FAIL: the machine never reached the restart with skip=$skip" >&2
        exit 1
    fi
    if grep -q "the platform did not reset" "$LOG"; then
        echo "reset-test: FAIL: with $what, nothing restarted the machine" >&2
        exit 1
    fi
    if [ "$skip" = "0" ] && ! grep -q "not Insyde: its reset is not sent" "$LOG"; then
        echo "reset-test: FAIL: SeaBIOS was sent another firmware's reset command" >&2
        exit 1
    fi
    echo "reset-test: $what: OK"
done

# The firmware's command on a machine whose SMBIOS names Insyde's firmware.
# The command's write does nothing on QEMU, and the chipset register restarts
# the machine after it.
rm -f "$LOG"
"$REPO/build/tests/system/harness" --kernel "$KERNEL" --initrd "$IMAGE"     --qemu-arg=-cpu --qemu-arg=max     --qemu-arg=-smbios --qemu-arg="type=0,vendor=Insyde Corp."     --append "rach2.rescue rach2.resetskip=0"     --timeout 90 --log "$LOG" --ok "reboot: restarting"     --must-exit --never "<2>"     "expect=Type 'help'" "send=reboot" until=10 >"$LOG.harness" 2>&1 && rc=0 || rc=$?

if [ "$rc" -ne 0 ]; then
    echo "reset-test: FAIL: the machine naming an Insyde firmware did not restart:" >&2
    sed -n 's/^HARNESS: /reset-test: /p' "$LOG.harness" >&2
    rm -f "$LOG.harness"
    exit 1
fi
rm -f "$LOG.harness"
if ! grep -q "the firmware is Insyde Corp.: sending its own reset" "$LOG"; then
    echo "reset-test: FAIL: the firmware that owns the reset command was not sent it" >&2
    exit 1
fi
if grep -q "its reset is not sent" "$LOG"; then
    echo "reset-test: FAIL: the machine both sent and did not send the command" >&2
    exit 1
fi
echo "reset-test: the firmware's own reset, sent only to that firmware: OK"

echo "reset-test: PASS"
