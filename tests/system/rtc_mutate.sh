#!/bin/sh
# rtc_mutate.sh [--host | --guest] | --texts: show that the host test of the
# clock, and with --guest the STATTIME row of stattest, catch a fault in
# rach2/sys/clock.c.
#
# Usage: sh tests/system/rtc_mutate.sh [--host|--guest] | --texts
# The fault takes the PIT's position out of the time between ticks, in
# rach2/sys/clock.c: the host proof compiles rtc_hosttest.c with a private
# copy of it, as tests/run host compiles the test, and the guest proof runs
# STATTIME on the KRFFS test root in all three phases, on the kernel, image
# and emulator of the architecture ARCH names. --host is the default. --texts
# checks that the fault is once in its source.
set -eu
repo=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
cd "$repo"

# apply_fault SOURCE: put the fault into SOURCE; with
# RACH2_FAULT_TEXTS set, only check its text and write nothing.
apply_fault()
{
    python3 - "$1" <<'PY'
from pathlib import Path
import sys
import os
import pathlib
if os.environ.get('RACH2_FAULT_TEXTS'):
    pathlib.Path.write_text = lambda self, *args, **kwargs: 0
path = Path(sys.argv[1])
text = path.read_text()
old = '*nanoseconds += phase_ns;'
if text.count(old) != 1:
    raise SystemExit('rtc-mutation: sub-tick expression is not unique')
path.write_text(text.replace(old, '*nanoseconds += phase_ns * 0u;'))
PY
}

if [ "${1:-}" = --texts ]; then
    RACH2_FAULT_TEXTS=1
    export RACH2_FAULT_TEXTS
    apply_fault rach2/sys/clock.c >/dev/null
    echo 'rtc-mutation: texts: every fault is once in its source'
    exit 0
fi


RACH2_ARCH=${RACH2_ARCH:-${ARCH:-i686}}
REPO=$repo
. "$repo/mk/arch.sh"
RACH2_QEMU=$ARCH_QEMU
export RACH2_QEMU ARCH_BUILD
tree=build$ARCH_SUFFIX
mode=${1:---host}
case "$mode" in
    --host|--guest) ;;
    *) echo 'rtc-mutation: use --host or --guest' >&2; exit 2 ;;
esac
. "$repo/tests/system/runlock.sh"
runlock_hold "$repo" rtc-mutation "$repo/tests/system/rtc_mutate.sh" "$@"
mkdir -p build/clock-proof
work=$(mktemp -d "$repo/build/clock-proof/mutation.XXXXXX")
cp rach2/sys/clock.c "$work/source.saved"
source="$work/clock.c"
[ "$mode" != --guest ] || source=rach2/sys/clock.c
cp "$work/source.saved" "$source"
child=
changed=

# Stop the running boot's process group and wait until it is gone.
stop_child()
{
    if [ -n "$child" ]; then
        kill -TERM "-$child" 2>/dev/null || :
        wait "$child" 2>/dev/null || :
        while kill -0 "-$child" 2>/dev/null; do
            sleep 1
        done
        child=
    fi
}

# Put back the saved clock.c and touch it.
restore()
{
    cp "$work/source.saved" "$source"
    touch "$source"
    changed=
}

# Stop the boot and, when the fault is in the source, restore it, and with
# --guest rebuild the kernel.
cleanup()
{
    stop_child
    if [ -n "$changed" ]; then
        restore
        if [ "$mode" = --guest ]; then
            make -s rach2 "ARCH=$RACH2_ARCH" >"$work/cleanup.build.log" 2>&1
        fi
    fi
}
trap cleanup EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

# run_phase PHASE EXPECTED: compile and run the host test, and with --guest
# build the kernel and run stattest on the KRFFS root. With EXPECTED `pass`
# both must pass; otherwise both must fail.
run_phase()
{
    phase=$1
    expected=$2
    "${CC:-cc}" -std=c99 -D_DEFAULT_SOURCE -D_FORTIFY_SOURCE=0 -DRACH2_HOSTTEST -g -O1 \
        -ffreestanding -Wno-empty-body -fsanitize=address,undefined -fno-sanitize-recover=all \
        -DRTC_HOST_TEST -DRACH2_HOST_CPU -I rach2/sys -I rach2/sys/arch/x86 \
        -I rach2/sys/arch/i686 -I rach2/sys/lib -I rach2/sys/proc -I tests/host/compat \
        tests/host/rtc_hosttest.c "$source" rach2/sys/arch/x86/cmos.c rach2/sys/arch/x86/pit.c \
        tests/host/compat/spinlock_host.c -o "$work/$phase.host" >"$work/$phase.compile.log" 2>&1
    cat "$work/$phase.compile.log"
    status=0
    "$work/$phase.host" >"$work/$phase.host.log" 2>&1 || status=$?
    cat "$work/$phase.host.log"
    [ ! -s "$work/$phase.host.log" ]
    if [ "$expected" = pass ]; then
        [ "$status" -eq 0 ]
    else
        [ "$status" -gt 0 ] && [ "$status" -le 100 ]
    fi
    echo "rtc-mutation: result: $phase host status $status"
    [ "$mode" = --guest ] || return 0
    make -s rach2 "ARCH=$RACH2_ARCH" >"$work/$phase.build.log" 2>&1
    cat "$work/$phase.build.log"
    cp "$tree"/rach2/kernel "$work/$phase.kernel"
    cksum "$source" "$work/$phase.kernel" >"$work/$phase.artifacts"
    build/tests/system/runutil group sh tests/system/probe_roots.sh \
        "$work/$phase.kernel" "$work/$phase" krffs stattest \
        >"$work/$phase.guest.log" 2>&1 &
    child=$!
    status=0
    wait "$child" || status=$?
    stop_child
    cat "$work/$phase.guest.log"
    if [ "$expected" = pass ]; then
        [ "$status" -eq 0 ]
        grep -F 'STATTIME OK:' "$work/$phase.stattest.serial.log" >/dev/null
    else
        [ "$status" -ne 0 ]
        grep -F 'STATTIME FAIL:' "$work/$phase.stattest.serial.log" >/dev/null
        grep -Eq 'sys_exit: init process [0-9]+ exited with status 1, returning to kernel shell' \
            "$work/$phase.stattest.serial.log"
    fi
    echo "rtc-mutation: result: $phase guest status $status"
}

echo "rtc-mutation: logs: $work"
if [ "$mode" = --guest ]; then
    make -s test-image build/tests/system/harness build/tests/system/runutil \
        "ARCH=$RACH2_ARCH" >"$work/prepare.log" 2>&1
    cat "$work/prepare.log"
fi
run_phase fixed pass
changed=1
apply_fault "$source"
run_phase coarse fail
restore
run_phase restored pass
if [ "$mode" = --guest ]; then
    if cmp -s "$work/fixed.kernel" "$work/coarse.kernel"; then
        echo 'rtc-mutation: kernel did not change' >&2; exit 1
    fi
    cmp "$work/fixed.kernel" "$work/restored.kernel"
fi
echo 'rtc-mutation: result: pass, fixed passed, coarse failed, restored passed'
