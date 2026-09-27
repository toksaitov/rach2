#!/bin/sh
# Prove sigmusl catches a signal frame whose ucontext lacks the saved registers.
set -eu

repo=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
cd "$repo"
[ ! -d build/.run.lock ] || {
    echo "sigcontext-mutation: another suite owns this tree" >&2; exit 1;
}
mkdir -p build
work=$(mktemp -d "$repo/build/sigcontext-proof.XXXXXX")
source=rach2/sys/proc/sigframe.c
cp "$source" "$work/source.saved"
child=
changed=

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

rebuild()
{
    rm -f build/rach2/sys/proc/sigframe.o build/rach2/kernel
    make -s -C rach2
}

restore()
{
    cp "$work/source.saved" "$source"
    touch "$source"
    changed=
}

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
python3 - "$source" <<'PY'
from pathlib import Path
import sys

path = Path(sys.argv[1])
text = path.read_text()
old = '        fill_sigcontext(&frame.rt.uc.mc, tf, fp_addr, oldmask);\n'
assert text.count(old) == 1
path.write_text(text.replace(old, ''))
PY
run_phase short fail
if cmp -s "$work/fixed.kernel" "$work/short.kernel"; then
    echo "sigcontext-mutation: kernel did not change" >&2; exit 1
fi
restore
run_phase restored pass
cmp "$work/fixed.kernel" "$work/restored.kernel"
echo "sigcontext-mutation: result: pass, compiled fault failed and restored kernel matched"
