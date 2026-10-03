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
# success. So three things are required together. The machine must reach the
# restart, which its own record says. It must not print the fallback, which
# `power_reset` prints when every mechanism it tried did nothing. And the
# machine must have ended by itself: `--must-exit` fails a run that was still
# there at the deadline and had to be killed, which is what a kernel that
# announced the restart and then hung looks like, and which the first two
# checks alone accept. A panic that also ends the machine is refused by
# `--never`, since a crash is not a reset.
#
# The first of the three is sent only to the firmware it belongs to, and that
# decision is made in the kernel and not in a test's copy of it, so both of its
# answers are booted: a machine whose firmware is SeaBIOS, which is every
# machine QEMU boots by default, must say the command was not sent; and a
# machine given `-smbios type=0,vendor=Insyde Corp.`, which is how QEMU is
# asked to publish another firmware's name, must say it was.
#
# What is left uncheckable from here: a triple fault also resets the machine,
# and with `-no-reboot` QEMU exits for it exactly as it does for a reset the
# kernel asked for. The kernel prints a `CRIT` record for a kernel exception,
# which `--never` refuses, so the case this cannot separate is a triple fault
# that printed nothing at all.
#
# Usage: reset_test.sh <kernel> <image>
#

set -e

# The build tree of the architecture tests/system/suites.mk names.
ARCH_BUILD=${ARCH_BUILD:-build}

KERNEL="$1"
IMAGE="$2"

if [ -z "$KERNEL" ] || [ -z "$IMAGE" ]; then
    echo "usage: reset_test.sh <kernel> <image>" >&2
    exit 2
fi

REPO="$(cd "$(dirname "$0")/../../.." && pwd)"
LOG="$REPO/$ARCH_BUILD/images/reset-test.log"

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

# The same first mechanism on a machine that says it runs the firmware the
# command belongs to. QEMU publishes the name; the write itself is inert there,
# and the machine still restarts through the chipset register after it.
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
