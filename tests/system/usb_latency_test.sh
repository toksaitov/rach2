#!/bin/sh
# usb_latency_test.sh: check checkers/usblatency.awk on the records of
# tests/host/usbstamp_hosttest.c, a host test built with the kernel's USB
# stamp recorder and scheduler.
#
# The host test runs in six modes. The checker must accept the spin, timer
# and random records and refuse the pump, work and undispatched ones, and it
# must refuse a record with a malformed field, one with a field twice, one
# with a field missing, and an empty one. USBSTAMP_TEST names a host test
# built elsewhere to run in place of the private build;
# tests/proofs/usb-latency.json runs the rows with builds whose faults make
# the checker refuse every record.
set -eu
[ "$#" -eq 0 ] || { echo 'usb-latency-test: usage: usb_latency_test.sh' >&2; exit 2; }
repo=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
. "$repo/tests/system/runlock.sh"
runlock_hold "$repo" usb-latency-test "$repo/tests/system/usb_latency_test.sh" "$@"
cd "$repo"

work=
child=
keep=

# Stop the running test, then remove the work directory, unless the test
# failed after printing its path, which sets `keep`; a signal clears `keep`.
cleanup()
{
    exit_status=$?
    if [ -n "$child" ]; then
        kill -TERM "$child" 2>/dev/null || :
        wait "$child" 2>/dev/null || :
        child=
    fi
    if [ "$exit_status" -eq 0 ] || [ -z "$keep" ]; then
        rm -rf ${work:+"$work"}
    fi
}
trap cleanup EXIT
trap 'keep=; exit 129' HUP
trap 'keep=; exit 130' INT
trap 'keep=; exit 143' TERM
work=$(mktemp -d "${TMPDIR:-/tmp}/rach2-usb-latency-test.XXXXXX")

fails=0
# bad TEXT: report a failed row and count it in `fails`.
bad()
{
    echo "usb-latency-test: fail: $1" >&2
    fails=$((fails + 1))
}

if [ -n "${USBSTAMP_TEST:-}" ]; then
    cp "$USBSTAMP_TEST" "$work/test"
else
    "${CC:-cc}" -std=c99 -DRACH2_HOSTTEST -g -O1 -ffreestanding \
        -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=all \
        -I rach2/sys -I rach2/sys/lib -I rach2/sys/proc \
        tests/host/usbstamp_hosttest.c -o "$work/test" >"$work/build.log" 2>&1 || {
            cat "$work/build.log" >&2; exit 1;
        }
fi

echo "usb-latency-test: logs: $work"
keep=1

# Run the host test, which must exit 0, then run it in each mode and judge
# each record with the checker.
status=0
"$work/test" >"$work/test.log" 2>&1 &
child=$!
wait "$child" || status=$?
child=
cat "$work/test.log"
[ "$status" -eq 0 ] || bad "the host test exits $status"
for mode in spin timer random pump work undispatched; do
    status=0
    "$work/test" "$mode" >"$work/$mode.log" 2>&1 &
    child=$!
    wait "$child" || status=$?
    child=
    [ "$status" -eq 0 ] || bad "$mode: the host test exits $status"
    want=0
    case $mode in pump|work|undispatched) want=1 ;; esac
    random=0
    case $mode in random|work) random=1 ;; esac
    status=0
    awk -v random="$random" -f tests/system/checkers/usblatency.awk \
        "$work/$mode.log" || status=$?
    [ "$status" -eq "$want" ] || {
        verdict=accepts
        [ "$status" -eq 0 ] || verdict=refuses
        bad "verdict: $mode: the checker $verdict the record"
        cat "$work/$mode.log" >&2
    }
done
[ "$fails" -eq 0 ] || exit 1

sed 's/ tracked 1,/ tracked x,/' "$work/spin.log" >"$work/malformed.log"
sed 's/ dispatch 1,/ dispatch 1, dispatch 1,/' "$work/spin.log" >"$work/duplicate.log"
sed 's/ pumps 0,//' "$work/spin.log" >"$work/missing.log"
: >"$work/empty.log"
for kind in malformed duplicate missing empty; do
    if awk -f tests/system/checkers/usblatency.awk "$work/$kind.log"; then
        echo "usb-latency-test: checker accepted $kind record" >&2; exit 1
    fi
done
echo "usb-latency-test: result: pass"
