#!/bin/sh
# xsave_mutate.sh [--texts]: show that xsaveprobe and the two signal-frame
# host tests catch eight compiled faults in the XSAVE code of
# rach2/sys/arch/x86/fpu.c, on the kernel, test image and emulator of the
# architecture ARCH names.
#
# Each fault is built into the main tree's kernel in turn and must fail the
# probe, the host tests or both, with the words given for it; the source is
# restored after each, and the restored kernel must match the first. --texts
# checks that each fault is once in fpu.c.
set -eu

repo=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
cd "$repo"
texts=
[ "${1:-}" != --texts ] || texts=1
# mutate OLD NEW: mark the source changed and replace OLD, which must occur
# once, with NEW in it; with RACH2_FAULT_TEXTS set, only check the text.
mutate()
{
    changed=1
    python3 - "$source" "$1" "$2" <<'PY'
from pathlib import Path
import os
import sys

path = Path(sys.argv[1])
text = path.read_text()
assert text.count(sys.argv[2]) == 1, sys.argv[2]
if not os.environ.get('RACH2_FAULT_TEXTS'):
    path.write_text(text.replace(sys.argv[2], sys.argv[3]))
PY
}

# fpu_fault NAME: put the fault whose failing phase is NAME into its source.
fpu_fault()
{
    case $1 in
        switch)
            mutate '"D"(area), "a"(0xFFFFFFFFu), "d"(0xFFFFFFFFu) : "memory");
    } else if (use_fxsr) {
        __asm__ __volatile__(FXSAVE' '"D"(area), "a"(3u), "d"(0u) : "memory");
    } else if (use_fxsr) {
        __asm__ __volatile__(FXSAVE'
            ;;
        xcr0)
            mutate '        mask |= XFEATURE_YMM;' ''
            ;;
        magic1)
            mutate '    put32(sw, FP_XSTATE_MAGIC1);' '    put32(sw, 0);'
            ;;
        ucflags)
            mutate '    return use_xsave ? UC_FP_XSTATE : 0u;' '    return 0u;'
            ;;
        sigreturn)
            mutate '        bv = get64(a + XSAVE_HDR) & rfbm;' \
                '        bv = get64(a + XSAVE_HDR) & rfbm & ~(uint64_t)XFEATURE_YMM;'
            ;;
        view)
            mutate '    put64(copy + FX_SW, xfeatures);' ''
            ;;
        magic2)
            mutate '    if (size != 0 && magic2 == FP_XSTATE_MAGIC2) {' '    if (size != 0) {'
            ;;
        header)
            mutate '        if (img[i] != 0) {' '        if (0) {'
            ;;
    esac
}

if [ -n "$texts" ]; then
    source=rach2/sys/arch/x86/fpu.c
    RACH2_FAULT_TEXTS=1
    export RACH2_FAULT_TEXTS
    for name in switch xcr0 magic1 ucflags sigreturn view magic2 header; do
        fpu_fault "$name"
    done
    echo 'xsave-mutation: texts: every fault is once in its source'
    exit 0
fi

. "$repo/tests/system/runlock.sh"
runlock_hold "$repo" xsave-mutation "$repo/tests/system/xsave_mutate.sh" "$@"
# The architecture, from ARCH as mk/arch.sh reads it: its kernel, its build
# tree and its emulator.
RACH2_ARCH=${RACH2_ARCH:-${ARCH:-i686}}
REPO=$repo
. "$repo/mk/arch.sh"
RACH2_QEMU=$ARCH_QEMU
export RACH2_QEMU
tree=${ARCH_BUILD#"$repo"/}
mkdir -p build
work=$(mktemp -d "$repo/build/xsave-proof.XXXXXX")
source=rach2/sys/arch/x86/fpu.c
cp "$source" "$work/source.saved"
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

# Remove fpu.c's object and the kernel, and make the kernel again.
rebuild()
{
    rm -f "$tree/rach2/sys/arch/x86/fpu.o" "$tree/rach2/kernel"
    make -s ARCH="$RACH2_ARCH" rach2
}

# Put back the saved fpu.c, touch it, and mark the source unchanged.
restore()
{
    cp "$work/source.saved" "$source"
    touch "$source"
    changed=
}

# Stop the boot and, when a fault is in the source, restore it and rebuild.
cleanup()
{
    stop_child
    if [ -n "$changed" ]; then
        restore
        rebuild >"$work/cleanup.build.log" 2>&1
    fi
}
trap cleanup EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

# Compile and run the two frame host tests as tests/system/suites/host.sh
# compiles them, and return 0 when both pass. The caller's `||` turns errexit
# off in here, so a compile that fails exits explicitly, and the last phase's
# binary is removed first, so that it cannot run in this one's place.
host_tests()
{
    failed=0
    for t in fpuframe_hosttest fpuframe64_hosttest; do
        rm -f "$work/$t"
        ${CC:-cc} -std=c99 -D_DEFAULT_SOURCE -D_FORTIFY_SOURCE=0 -DRACH2_HOSTTEST \
            -fsanitize=address,undefined -fno-sanitize-recover=all -g -O1 \
            -Wno-empty-body -ffreestanding -I rach2/sys -I rach2/sys/arch/x86 \
            -I rach2/sys/arch/i686 -I rach2/sys/lib -I rach2/sys/proc \
            -Wno-int-to-pointer-cast -Wno-pointer-to-int-cast -o "$work/$t" \
            "tests/host/$t.c" rach2/sys/lib/kstring.c rach2/sys/lib/kctypes.c ||
            { echo "xsave-mutation: $t did not compile in phase $phase" >&2; exit 1; }
        "$work/$t" >"$work/$phase.$t.log" 2>&1 || failed=1
        cat "$work/$phase.$t.log"
    done
    return "$failed"
}

# run_phase PHASE PROBE MATCH HOST HOSTMATCH: build the kernel and run
# xsaveprobe, which must pass or fail as PROBE says, failing with words that
# match MATCH when it is not empty; then run the host tests, which must pass
# or fail as HOST says, failing with words that contain HOSTMATCH.
run_phase()
{
    phase=$1
    rebuild >"$work/$phase.build.log" 2>&1
    cat "$work/$phase.build.log"
    cp "$tree/rach2/kernel" "$work/$phase.kernel"
    build/tests/system/runutil group build/tests/system/harness \
        --kernel "$tree/rach2/kernel" --initrd "$tree/images/test.krffs" \
        --qemu-arg=-cpu --qemu-arg=max --append rach2.rescue \
        --timeout 60 --steptmo 30 --ok 'XSAVEPROBE OK' --never 'XSAVEPROBE FAIL' \
        --log "$work/$phase.serial.log" \
        "expect=Type 'help'" 'send=exec /xsaveprobe' 'expect=XSAVEPROBE' \
        >"$work/$phase.log" 2>&1 &
    child=$!
    status=0
    wait "$child" || status=$?
    stop_child
    grep -a 'xsaveprobe\|XSAVEPROBE\|HARNESS' "$work/$phase.serial.log" "$work/$phase.log" || :
    if [ "$2" = pass ]; then
        [ "$status" -eq 0 ] || { echo "xsave-mutation: $phase: the probe failed" >&2; exit 1; }
    else
        [ "$status" -ne 0 ] || { echo "xsave-mutation: $phase: the fault passed the probe" >&2; exit 1; }
        [ -z "$3" ] || grep -aE "XSAVEPROBE FAIL: .*$3" "$work/$phase.serial.log" >/dev/null || {
            echo "xsave-mutation: $phase: the probe failed for another reason" >&2; exit 1;
        }
    fi
    hstatus=0
    host_tests || hstatus=$?
    if [ "$4" = pass ]; then
        [ "$hstatus" -eq 0 ] || { echo "xsave-mutation: $phase: the host tests failed" >&2; exit 1; }
    else
        [ "$hstatus" -ne 0 ] || { echo "xsave-mutation: $phase: the fault passed the host tests" >&2; exit 1; }
        cat "$work/$phase".fpuframe*.log | grep 'fail:' | grep -F "$5" >/dev/null || {
            echo "xsave-mutation: $phase: the host tests failed for another reason" >&2; exit 1;
        }
    fi
    echo "xsave-mutation: result: $phase: probe status $status, host tests status $hstatus"
}

echo "xsave-mutation: $RACH2_ARCH: logs: $work"
make -s ARCH="$RACH2_ARCH" rach2 tests krffs test-image \
    build/tests/system/harness build/tests/system/runutil >"$work/build.log" 2>&1
cat "$work/build.log"
run_phase fixed pass '' pass ''

fpu_fault switch
run_phase switch fail 'the upper half' pass ''
restore

fpu_fault xcr0
run_phase xcr0 fail 'XCR0 is not x87, SSE, and AVX' pass ''
restore

fpu_fault magic1
run_phase magic1 fail 'lack FP_XSTATE_MAGIC1' fail 'FP_XSTATE_MAGIC1'
restore

fpu_fault ucflags
run_phase ucflags fail 'lack UC_FP_XSTATE' pass ''
restore

fpu_fault sigreturn
run_phase sigreturn fail 'clobbered the registers: ymm0: the upper half' fail 'XSTATE_BV comes back'
restore

fpu_fault view
run_phase view fail 'XCR0 at byte 464' fail 'the view has XCR0 at byte 464'
restore

fpu_fault magic2
run_phase magic2 pass '' fail 'without FP_XSTATE_MAGIC2'
restore

fpu_fault header
run_phase header fail '' fail 'XCOMP_BV set is refused'
restore

run_phase restored pass '' pass ''
cmp "$work/fixed.kernel" "$work/restored.kernel"
echo "xsave-mutation: $RACH2_ARCH: result: pass, every fault failed and the restored kernel matched"
