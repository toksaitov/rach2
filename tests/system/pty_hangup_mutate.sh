#!/bin/sh
# Prove that master closure releases both blocked canonical slave readers, on
# the kernel, probe and emulator of the architecture ARCH names.
set -eu
repo=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
cd "$repo"

# apply_fault ARGS...: put one fault into its source; with
# RACH2_FAULT_TEXTS set, only check its text and write nothing.
apply_fault()
{
    python3 - "$1" <<'PYTHON'
from pathlib import Path
import sys
import os
import pathlib
if os.environ.get('RACH2_FAULT_TEXTS'):
    pathlib.Path.write_text = lambda self, *args, **kwargs: 0

path = Path(sys.argv[1])
text = path.read_text()
old = 'return !p->master_open || n_tty_readable(&p->slave.ldisc);'
new = 'return n_tty_readable(&p->slave.ldisc);'
if text.count(old) != 1:
    raise SystemExit('pty-hangup: wait predicate is not unique')
path.write_text(text.replace(old, new))
PYTHON
}

if [ "${1:-}" = --texts ]; then
    RACH2_FAULT_TEXTS=1
    export RACH2_FAULT_TEXTS
    apply_fault rach2/sys/tty/pty.c >/dev/null
    echo 'pty-hangup: texts: every fault is once in its source'
    exit 0
fi


RACH2_ARCH=${RACH2_ARCH:-${ARCH:-i686}}
REPO=$repo
. "$repo/mk/arch.sh"
RACH2_QEMU=$ARCH_QEMU
export RACH2_QEMU ARCH_BUILD
tree=build$ARCH_SUFFIX
. "$repo/tests/system/runlock.sh"
runlock_hold "$repo" pty-hangup "$repo/tests/system/pty_hangup_mutate.sh" "$@"
mkdir -p build
work=$(mktemp -d "$repo/build/pty-hangup.XXXXXX")
source=rach2/sys/tty/pty.c
cp "$source" "$work/pty.saved"
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
    rm -f "$tree"/rach2/sys/tty/pty.o "$tree"/rach2/kernel
    make -s rach2 "ARCH=$RACH2_ARCH"
}

restore()
{
    cp "$work/pty.saved" "$source"
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

run_probe()
{
    expected=$1
    probe=ptyprobe
    token=PTYPROBE
    build/tests/system/runutil group build/tests/system/harness \
        --kernel "$tree"/rach2/kernel --initrd "$work/test.krffs" \
        --net hubport,id=net0,hubid=0 --qemu-arg=-cpu --qemu-arg=max \
        --append rach2.rescue --timeout 40 --steptmo 15 \
        --ok "$token OK" --never "$token FAIL" \
        --log "$work/$phase.$probe.serial.log" \
        "expect=Type 'help'" "send=exec /$probe" "expect=$token" \
        'expect=returning to kernel shell' >"$work/$phase.$probe.log" 2>&1 &
    child=$!
    status=0
    wait "$child" || status=$?
    stop_child
    cat "$work/$phase.$probe.log" "$work/$phase.$probe.serial.log"
    if [ "$expected" = pass ]; then
        [ "$status" -eq 0 ] || exit 1
    else
        [ "$status" -ne 0 ] || {
            echo "pty-hangup: mutation passed: $phase" >&2; exit 1;
        }
        grep -F "$detail" "$work/$phase.$probe.serial.log" >/dev/null || exit 1
    fi
    echo "pty-hangup: result: $phase $probe status $status"
}

run_phase()
{
    phase=$1
    expected=$2
    rebuild >"$work/$phase.build.log" 2>&1
    cat "$work/$phase.build.log"
    cp "$tree"/rach2/kernel "$work/$phase.kernel"
    cksum "$source" "$tree"/rach2/kernel "$work/test.krffs" >"$work/$phase.artifacts"
    run_probe "$expected"
}

echo "pty-hangup: logs: $work"
make -s -C tests ../"$tree"/tests/ptyprobe "ARCH=$RACH2_ARCH" >"$work/build.log" 2>&1
# These three image tools do not use mkhash's vendored crypt source.
make -s -C krffs -o libc-check ../build/krffs/mkfs.krffs \
    ../build/krffs/stageimage.krffs ../build/krffs/readfile.krffs "ARCH=$RACH2_ARCH" >>"$work/build.log" 2>&1
make -s build/tests/system/harness build/tests/system/runutil "ARCH=$RACH2_ARCH" >>"$work/build.log" 2>&1
cat >"$work/test.manifest" <<MANIFEST
file 0755 /ptyprobe $tree/tests/ptyprobe
MANIFEST
{
    dd if=/dev/zero of="$work/test.krffs" bs=1048576 count=2
    build/krffs/mkfs.krffs "$work/test.krffs"
    build/krffs/stageimage.krffs "$work/test.manifest" "$work/test.krffs"
} >>"$work/build.log" 2>&1
cat "$work/build.log"
build/krffs/readfile.krffs "$work/test.krffs" /ptyprobe "$work/ptyprobe.staged"
cmp "$tree"/tests/ptyprobe "$work/ptyprobe.staged"
run_phase fixed pass
changed=1
apply_fault "$source"
detail='PTYPROBE FAIL (hangup releases both blocked readers)'
run_phase mutated fail
if cmp -s "$work/fixed.kernel" "$work/mutated.kernel"; then
    echo 'pty-hangup: mutation did not change kernel' >&2; exit 1;
fi
restore
run_phase restored pass
cmp "$work/fixed.kernel" "$work/restored.kernel"
echo 'pty-hangup: result: compiled fault failed, restored kernel matched'
