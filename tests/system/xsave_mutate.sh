#!/bin/sh
# xsave_mutate.sh [--texts]: show that xsaveprobe and the two signal-frame
# host tests catch eight compiled faults in the XSAVE code of
# rach2/sys/arch/x86/fpu.c, on the kernel, test image and emulator of the
# architecture ARCH names.
#
# The probe boots under the processor profile max, which declares XSAVE and
# AVX, and its report records are judged by tools/etude/casecheck as the case
# guest.default.xsaveprobe of tools/etude/schemas/case.guest.example.json,
# placed qemu. Each fault is built into the main tree's kernel in turn and
# must fail the report, with the assertion and words given for it, the host
# tests, or both; the source is restored after each, and the restored kernel
# must match the first. The unchanged kernel then boots under the profile
# qemu64, which has neither: the probe passes by its branches fxsave and
# none, which the judge refuses for the case of the profile max, as it
# refuses any processor that lacks a feature its profile declares. --texts
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
casecheck=build/tools/etude/casecheck
manifest=tools/etude/schemas/case.guest.example.json
work=
source=rach2/sys/arch/x86/fpu.c
child=
changed=
keep=

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
# The work directory is removed too, unless the proof failed after printing
# its path, which sets `keep`; a signal clears `keep`.
cleanup()
{
    exit_status=$?
    stop_child
    if [ -n "$changed" ]; then
        restore
        rebuild >"$work/cleanup.build.log" 2>&1
    fi
    if [ "$exit_status" -eq 0 ] || [ -z "$keep" ]; then
        rm -rf ${work:+"$work"}
    fi
}
trap cleanup EXIT
trap 'keep=; exit 129' HUP
trap 'keep=; exit 130' INT
trap 'keep=; exit 143' TERM
work=$(mktemp -d "$repo/build/xsave-proof.XXXXXX")
cp "$source" "$work/source.saved"

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

# boot PHASE PROFILE: boot the tree's kernel under the processor profile
# PROFILE, run xsaveprobe, and judge its records into $work/PHASE.judged,
# with the status casecheck gave in `status`.
boot()
{
    build/tests/system/runutil group build/tests/system/harness \
        --kernel "$tree/rach2/kernel" --initrd "$tree/images/test.krffs" \
        --qemu-arg=-cpu --qemu-arg="$("$casecheck" processor "$2")" --append rach2.rescue \
        --timeout 60 --steptmo 30 --ok '@etude 1 end ' \
        --log "$work/$1.serial.log" \
        "expect=Type 'help'" 'send=exec /xsaveprobe' 'expect=@etude 1 end ' \
        >"$work/$1.log" 2>&1 &
    child=$!
    wait "$child" || :
    stop_child
    grep -a 'xsaveprobe\|XSAVEPROBE\|HARNESS' "$work/$1.serial.log" "$work/$1.log" || :
    tr -d '\r' < "$work/$1.serial.log" | grep -a '^@etude 1 ' > "$work/$1.records" || :
    status=0
    "$casecheck" report "$manifest" guest.default.xsaveprobe "$RACH2_ARCH" qemu \
        "$work/$1.records" >"$work/$1.judged" 2>&1 || status=$?
    cat "$work/$1.judged"
}

# run_phase PHASE PROBE MATCH HOST HOSTMATCH: build the kernel and run
# xsaveprobe, whose report must pass or fail as PROBE says, failing with a
# line that matches the extended expression MATCH when it is not empty; then
# run the host tests, which must pass or fail as HOST says, failing with
# words that contain HOSTMATCH.
run_phase()
{
    phase=$1
    rebuild >"$work/$phase.build.log" 2>&1
    cat "$work/$phase.build.log"
    cp "$tree/rach2/kernel" "$work/$phase.kernel"
    boot "$phase" max
    if [ "$2" = pass ]; then
        [ "$status" -eq 0 ] || { echo "xsave-mutation: $phase: the report failed" >&2; exit 1; }
    else
        [ "$status" -eq 1 ] || { echo "xsave-mutation: $phase: the fault passed the report" >&2; exit 1; }
        [ -z "$3" ] || grep -E "$3" "$work/$phase.judged" >/dev/null || {
            echo "xsave-mutation: $phase: the report failed for another reason" >&2; exit 1;
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
    echo "xsave-mutation: result: $phase: report status $status, host tests status $hstatus"
}

echo "xsave-mutation: $RACH2_ARCH: logs: $work"
keep=1
make -s ARCH="$RACH2_ARCH" rach2 tests krffs test-image "$casecheck" \
    build/tests/system/harness build/tests/system/runutil >"$work/build.log" 2>&1
cat "$work/build.log"
run_phase fixed pass '' pass ''

fpu_fault switch
run_phase switch fail 'assert switches fail: .*the upper half' pass ''
restore

fpu_fault xcr0
run_phase xcr0 fail 'assert xcr0 fail: XCR0 is not x87, SSE, and AVX' pass ''
restore

fpu_fault magic1
run_phase magic1 fail 'assert signals fail: .*lack FP_XSTATE_MAGIC1' fail 'FP_XSTATE_MAGIC1'
restore

fpu_fault ucflags
run_phase ucflags fail 'assert signals fail: .*lack UC_FP_XSTATE' pass ''
restore

fpu_fault sigreturn
run_phase sigreturn fail 'assert signals fail: .*clobbered the registers: ymm0: the upper half' \
    fail 'XSTATE_BV comes back'
restore

fpu_fault view
run_phase view fail 'assert ptrace fail: .*XCR0 at byte 464' fail 'the view has XCR0 at byte 464'
restore

fpu_fault magic2
run_phase magic2 pass '' fail 'without FP_XSTATE_MAGIC2'
restore

fpu_fault header
run_phase header fail 'assert header (fail: |missing)' fail 'XCOMP_BV set is refused'
restore

run_phase restored pass '' pass ''
cmp "$work/fixed.kernel" "$work/restored.kernel"
boot noavx qemu64
if [ "$status" -ne 1 ] ||
    ! grep -F 'the program took none where the processor gives avx' "$work/noavx.judged" >/dev/null; then
    echo "xsave-mutation: noavx: the report passed without AVX under the profile max" >&2
    exit 1
fi
"$casecheck" report -f xsave=absent -f avx=absent "$manifest" guest.default.xsaveprobe \
    "$RACH2_ARCH" native "$work/noavx.records" ||
    { echo "xsave-mutation: noavx: the report fails where the processor has neither" >&2; exit 1; }
echo "xsave-mutation: $RACH2_ARCH: result: pass, every fault failed, the restored kernel matched, and a processor without AVX failed the profile max"
