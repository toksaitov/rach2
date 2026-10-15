#!/bin/sh
# usb_latency_mutate.sh [--texts]: show that the two USB wake-to-dispatch rows
# of tests/system/usb_latency.sh catch two faults, on harness kernels built with
# each. Without the preemption that follows a device interrupt, a reader the
# keyboard woke waits beside the spinner for the timer, whose tick pumps the
# controller first, and the spin row must fail its count of pumps. Without the
# service point in a large random read, the reader waits for the whole
# eight-mebibyte call, and the random row must fail its count of bytes. The
# source restored, touched and rebuilt must pass both rows with a kernel equal
# to the first byte for byte.
#
# The kernels are built from a private copy of rach2/ and mk/, so no kernel
# under build/ is replaced; the test image, the harness and the runner helper
# are the ones `tests/run usb` built, and must be current. ARCH chooses the
# architecture, as it does for `tests/run`. --texts checks that each fault is
# once in its source.
#
# Usage: sh tests/system/usb_latency_mutate.sh [--texts]
set -eu
repo=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
cd "$repo"

# apply_fault SYS FAULT: put FAULT into the sources under SYS; with
# RACH2_FAULT_TEXTS set, only check its text and write nothing.
apply_fault()
{
    python3 - "$1" "$2" <<'PY'
import pathlib
import sys
import os
if os.environ.get('RACH2_FAULT_TEXTS'):
    pathlib.Path.write_text = lambda self, *args, **kwargs: 0

root = pathlib.Path(sys.argv[1])
changes = {
    'preempt': ('arch/x86/isr.c', [
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

if [ "${1:-}" = --texts ]; then
    RACH2_FAULT_TEXTS=1
    export RACH2_FAULT_TEXTS
    apply_fault rach2/sys preempt >/dev/null
    apply_fault rach2/sys service >/dev/null
    echo 'usb-latency-mutation: texts: every fault is once in its source'
    exit 0
fi


. "$repo/tests/system/runlock.sh"
runlock_hold "$repo" usb-latency-mutation "$repo/tests/system/usb_latency_mutate.sh" "$@"
RACH2_ARCH=${RACH2_ARCH:-${ARCH:-i686}}
REPO=$repo
. "$repo/mk/arch.sh"
RACH2_QEMU=$ARCH_QEMU
export RACH2_QEMU
harness=$repo/build/tests/system/harness
runner=$repo/build/tests/system/runutil
image=$repo/build$ARCH_SUFFIX/images/test.krffs
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
isr=$work/rach2/sys/arch/x86/isr.c
syscall=$work/rach2/sys/syscall.c
cp "$isr" "$work/isr.c.saved"
cp "$syscall" "$work/syscall.c.saved"
child=

# Stop the running boot's process group and wait until it is gone.
stop_child()
{
    if [ -n "$child" ]; then
        kill -TERM "-$child" 2>/dev/null || :
        wait "$child" 2>/dev/null || :
        while kill -0 "-$child" 2>/dev/null; do sleep 1; done
        child=
    fi
}

# Put back the saved isr.c and syscall.c and touch them.
restore()
{
    cp "$work/isr.c.saved" "$isr"
    cp "$work/syscall.c.saved" "$syscall"
    touch "$isr" "$syscall"
}

# Stop the rows and restore the sources.
cleanup()
{
    stop_child
    restore
}
trap cleanup EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

# build PHASE: build the harness kernel from the private tree and keep a copy.
build()
{
    phase=$1
    # A source changed within the object's timestamp unit must still compile.
    objects=$work/build$ARCH_SUFFIX/rach2-harness
    rm -f "$objects/sys/arch/x86/isr.o" "$objects/sys/syscall.o" "$objects/kernel"
    if ! make -s -C "$work/rach2" -j4 "ARCH=$RACH2_ARCH" COMPONENT=rach2-harness HARNESS=1 \
        "ELFCHECK=$checker" >"$work/$phase.build.log" 2>&1; then
        cat "$work/$phase.build.log" >&2
        exit 1
    fi
    cp "$objects/kernel" "$work/$phase.kernel"
    cksum "$isr" "$syscall" "$work/$phase.kernel" >"$work/$phase.cksum"
}

# run_rows PHASE FAILING: build the kernel and run both rows on it. With
# FAILING empty both must pass; otherwise the FAILING row must fail with an
# invalid dispatch record.
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

# mutate FAULT: put FAULT into the private sources.
mutate()
{
    apply_fault "$work/rach2/sys" "$1"
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
