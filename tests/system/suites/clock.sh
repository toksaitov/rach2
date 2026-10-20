#!/usr/bin/env sh
#
# clock.sh: the guest's clock against the host's while the host stops the
# machine thirty times.
#
# clockload.py supplies a UDP host reference before and after thirty
# SIGSTOP intervals, each 0.3 s after 0.7 s running. The guest keeps busy
# until the host ends the stops. Both clocks must agree with the host's
# monotonic interval to 250 ms, including the uncertainty of both samples.
# Each exchange encloses the host sample in guest reads at most 100 ms
# apart. Unavailable or ambiguous references fail as measurements. Serial
# latency cannot alter the clock comparison. The selected accelerator runs
# once, with a second TCG boot when the selected accelerator is not TCG.
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
    echo "clock-test: FAIL: a clock comparison failed" >&2
    exit 1
fi
echo "clock-test: ok: both clocks kept the host's time across the stops"
