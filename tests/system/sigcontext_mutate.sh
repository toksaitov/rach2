#!/bin/sh
# sigcontext_mutate.sh [--texts]: show that sigmusl catches a signal frame
# whose ucontext lacks the saved registers.
#
# The fault takes the call of fill_sigcontext out of the i686 sigframe.c, and
# sigmusl must then fail with the ucontext's cs, esp and fpregs zero, both
# with FXSR and with rach2.nofxsr; the kernel must differ from the first. The
# unmodified and restored kernels must pass both, and the restored kernel
# must match the first. The fault and the paths are i686's, whatever ARCH the
# run that starts it is for, in the environment or in the make flags
# tests/run gives it. --texts checks that the fault is once in its source.
set -eu
unset ARCH MAKEFLAGS MFLAGS

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
old = '        fill_sigcontext(&frame.rt.uc.mc, tf, fp_addr, oldmask);\n'
assert text.count(old) == 1
path.write_text(text.replace(old, ''))
PY
}

if [ "${1:-}" = --texts ]; then
    RACH2_FAULT_TEXTS=1
    export RACH2_FAULT_TEXTS
    apply_fault rach2/sys/arch/i686/sigframe.c >/dev/null
    echo 'sigcontext-mutation: texts: every fault is once in its source'
    exit 0
fi


. "$repo/tests/system/runlock.sh"
runlock_hold "$repo" sigcontext-mutation "$repo/tests/system/sigcontext_mutate.sh" "$@"
mkdir -p build
work=$(mktemp -d "$repo/build/sigcontext-proof.XXXXXX")
source=rach2/sys/arch/i686/sigframe.c
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

# Remove sigframe.c's object and the kernel, and make the kernel again.
rebuild()
{
    rm -f build/rach2/sys/arch/i686/sigframe.o build/rach2/kernel
    make -s -C rach2
}

# Put back the saved sigframe.c, touch it, and mark the source unchanged.
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

# run_phase PHASE EXPECTED: build the kernel and run sigmusl on it with FXSR
# and with rach2.nofxsr. With EXPECTED `pass` both must pass; otherwise both
# must fail with the ucontext's registers zero.
run_phase()
{
    phase=$1
    expected=$2
    rebuild >"$work/$phase.build.log" 2>&1
    cat "$work/$phase.build.log"
    cp build/rach2/kernel "$work/$phase.kernel"
    cksum build/rach2/kernel build/images/test.krffs build/tests/musl/sigmusl \
        >"$work/$phase.artifacts"
    for mode in fxsr x87; do
        append=rach2.rescue
        [ "$mode" != x87 ] || append="$append rach2.nofxsr"
        build/tests/system/runutil group build/tests/system/harness \
            --kernel build/rach2/kernel --initrd build/images/test.krffs \
            --net hubport,id=net0,hubid=0 --qemu-arg=-cpu --qemu-arg=max \
            --append "$append" --timeout 30 --steptmo 10 \
            --ok 'SIGMUSL OK' --never 'SIGMUSL FAIL' \
            --log "$work/$phase.$mode.serial.log" \
            "expect=Type 'help'" 'send=exec /musl/sigmusl' \
            'expect=SIGMUSL' 'expect=returning to kernel shell' \
            >"$work/$phase.$mode.log" 2>&1 &
        child=$!
        status=0
        wait "$child" || status=$?
        stop_child
        cat "$work/$phase.$mode.log" "$work/$phase.$mode.serial.log"
        if [ "$expected" = pass ]; then
            [ "$status" -eq 0 ] || exit 1
        else
            [ "$status" -ne 0 ] || {
                echo "sigcontext-mutation: fault passed: $mode" >&2; exit 1;
            }
            grep -F 'SIGMUSL FAIL: ucontext cs=0x0 esp=0x0 fpregs=0x0' \
                "$work/$phase.$mode.serial.log" >/dev/null || {
                echo "sigcontext-mutation: wrong failure: $mode" >&2; exit 1;
            }
        fi
        echo "sigcontext-mutation: result: $phase $mode status $status"
    done
}

echo "sigcontext-mutation: logs: $work"
make -s rach2 tests musl-programs utils ish krffs \
    build/tests/system/harness build/tests/system/runutil >"$work/build.log" 2>&1
make -s test-image >>"$work/build.log" 2>&1
cat "$work/build.log"
build/krffs/readfile.krffs build/images/test.krffs /musl/sigmusl "$work/sigmusl.staged"
cmp build/tests/musl/sigmusl "$work/sigmusl.staged"
run_phase fixed pass
changed=1
apply_fault "$source"
run_phase short fail
if cmp -s "$work/fixed.kernel" "$work/short.kernel"; then
    echo "sigcontext-mutation: kernel did not change" >&2; exit 1
fi
restore
run_phase restored pass
cmp "$work/fixed.kernel" "$work/restored.kernel"
echo "sigcontext-mutation: result: pass, compiled fault failed and restored kernel matched"
