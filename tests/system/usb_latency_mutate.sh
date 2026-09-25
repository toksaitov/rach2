#!/bin/sh
# Prove the two USB wake-to-dispatch rows of tests/system/usb_latency.sh on
# harness kernels built with each fault. Without the preemption that follows
# a device interrupt, a reader the keyboard woke waits beside the spinner for
# the timer, whose tick pumps the controller first, and the spin row must
# fail its count of pumps. Without the service point in a large random read,
# the reader waits for the whole eight-mebibyte call, and the random row must
# fail its count of bytes. The source restored, touched and rebuilt must pass
# both rows with a kernel equal to the first byte for byte.
#
# The kernels are built from a private copy of rach2/ and mk/, so no kernel
# under build/ is replaced; the test image, the harness and the runner helper
# are the ones `tests/run usb` built, and must be current.
#
# Usage: sh tests/system/usb_latency_mutate.sh
set -eu
repo=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
cd "$repo"
harness=$repo/build/tests/system/harness
runner=$repo/build/tests/system/runutil
image=$repo/build/images/test.krffs
checker=$repo/build/tools/inspect/elfcheck
for artifact in "$harness" "$runner" "$image" "$checker"; do
    if [ ! -f "$artifact" ]; then
        echo "usb-latency-mutation: missing $artifact; run tests/run usb first" >&2
        exit 1
    fi
done
work=$(mktemp -d "${TMPDIR:-/tmp}/rach2-usb-latency.XXXXXX")
cp -R rach2 mk "$work/"
# The kernel's link asks the directory above rach2/ for the ELF checker; the
# private tree's has a rule with nothing to do, and ELFCHECK names the built
# one.
printf 'build/tools/inspect/elfcheck: ;\n' >"$work/Makefile"
isr=$work/rach2/sys/isr.c
syscall=$work/rach2/sys/syscall.c
cp "$isr" "$work/isr.c.saved"
cp "$syscall" "$work/syscall.c.saved"
child=

stop_child()
{
    if [ -n "$child" ]; then
        kill -TERM "-$child" 2>/dev/null || :
        wait "$child" 2>/dev/null || :
        while kill -0 "-$child" 2>/dev/null; do sleep 1; done
        child=
    fi
}

restore()
{
    cp "$work/isr.c.saved" "$isr"
    cp "$work/syscall.c.saved" "$syscall"
    touch "$isr" "$syscall"
}

cleanup()
{
    stop_child
    restore
}
trap cleanup EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

build()
{
    phase=$1
    # A source changed within the object's timestamp unit must still compile.
    rm -f "$work/build/rach2-harness/sys/isr.o" "$work/build/rach2-harness/sys/syscall.o" \
        "$work/build/rach2-harness/kernel"
    if ! make -s -C "$work/rach2" -j4 COMPONENT=rach2-harness HARNESS=1 "ELFCHECK=$checker" \
        >"$work/$phase.build.log" 2>&1; then
        cat "$work/$phase.build.log" >&2
        exit 1
    fi
    cp "$work/build/rach2-harness/kernel" "$work/$phase.kernel"
    cksum "$isr" "$syscall" "$work/$phase.kernel" >"$work/$phase.cksum"
}

run_rows()
{
    phase=$1
    failing=$2
    build "$phase"
    status=0
    RACH2_USB_LOG_PREFIX="$work/$phase" "$runner" group sh tests/system/usb_latency.sh \
        "$work/$phase.kernel" "$image" "$harness" >"$work/$phase.log" 2>&1 &
    child=$!
    wait "$child" || status=$?
    stop_child
    cat "$work/$phase.log"
    if [ -z "$failing" ]; then
        if [ "$status" -ne 0 ]; then
            echo "usb-latency-mutation: $phase failed its rows" >&2
            exit 1
        fi
    elif [ "$status" -eq 0 ] ||
        ! grep -Fq "usb-test: invalid dispatch record: $failing" "$work/$phase.log"; then
        echo "usb-latency-mutation: $phase did not fail the $failing row" >&2
        exit 1
    fi
    echo "usb-latency-mutation: result: $phase status $status"
}

mutate()
{
    python3 - "$work/rach2/sys" "$1" <<'PY'
import pathlib
import sys

root = pathlib.Path(sys.argv[1])
changes = {
    'preempt': ('isr.c', [
        ('        sched_preempt(); /* a woken reader runs now, or at the call\'s exit */\n', ''),
        ('    if (params->vector != 32) {\n        sched_preempt();\n    }\n', '')]),
    'service': ('syscall.c', [
        ('        if ((done & 4095u) == 0) {\n            sched_service_point();\n        }\n',
         '')]),
}
name, edits = changes[sys.argv[2]]
path = root / name
source = path.read_text()
for old, new in edits:
    if source.count(old) != 1:
        raise SystemExit('usb-latency-mutation: source pattern is not unique')
    source = source.replace(old, new)
path.write_text(source)
PY
}

echo "usb-latency-mutation: logs: $work"
run_rows fixed ''
mutate preempt
run_rows preempt-mutated spin
restore
run_rows preempt-restored ''
cmp "$work/fixed.kernel" "$work/preempt-restored.kernel"
mutate service
run_rows service-mutated random
restore
run_rows service-restored ''
cmp "$work/fixed.kernel" "$work/service-restored.kernel"
echo 'usb-latency-mutation: result: pass: each fault failed its row; the restored kernels passed and equal the first'
