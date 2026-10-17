#!/usr/bin/env sh
#
# clock.sh: the guest's clock against the host's while the host stops the
# machine thirty times.
#
# tests/system/clockload.py boots the test image, runs musl/clockloadmusl
# (tests/musl/clockloadmusl.c), which prints CLOCK_MONOTONIC and
# CLOCK_REALTIME once a second for 45 s while it keeps the machine busy, and
# stops the emulator with SIGSTOP for 0.3 s after each 0.7 s it runs, thirty
# times, as a loaded host stops a machine's processors. Each clock's time
# between the first and the last line must be the host's between their
# arrivals within 250 ms. The machine boots twice: under the accelerator the
# run chose, and under TCG, where the emulator's timers deliver one interrupt
# for every tick that fell due while the machine was stopped; a run that
# chose TCG boots once.
#
# The bound was measured on the testing host, under TCG with two processors,
# with six machines of the suite at once: the kernel that counts the
# reference's ticks read 1 to 3 ms from the host over the 45 s, 9 s of them
# stopped, and the kernel that counted one tick for each interrupt, the rule
# before the reference kept the clock, read 8749 ms behind.
#
# Usage: clock.sh KERNEL IMAGE
#
set -u

KERNEL=${1:-}
IMAGE=${2:-}
REPO=$(cd "$(dirname "$0")/../../.." && pwd)

if [ -z "$KERNEL" ] || [ -z "$IMAGE" ]; then
    echo "usage: clock.sh KERNEL IMAGE" >&2
    exit 2
fi

# What this suite writes under the images directory carries its name.
OUT="$(dirname "$IMAGE")/clock-test"
rm -f "$OUT.log" "$OUT.tcg.log"

fail=0
python3 "$REPO/tests/system/clockload.py" "$KERNEL" "$IMAGE" "$OUT.log" || fail=1
if [ "${RACH2_QEMU_ACCEL:-tcg}" != tcg ]; then
    python3 "$REPO/tests/system/clockload.py" "$KERNEL" "$IMAGE" "$OUT.tcg.log" tcg || fail=1
fi
if [ "$fail" -ne 0 ]; then
    echo "clock-test: FAIL: a clock lost or gained time over the host's stops" >&2
    exit 1
fi
echo "clock-test: ok: both clocks kept the host's time across the stops"
