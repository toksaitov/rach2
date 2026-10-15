#!/bin/sh
# framebuffer_mutate.sh [--texts]: run devprobe and loopprobe, and show that
# devprobe's framebuffer rows catch two compiled faults in the framebuffer's
# ioctl in rach2/sys/syscall.c, on the kernel, probes and emulator of the
# architecture ARCH names.
#
# `owner` takes out the check that only the display's owner changes its mode,
# and `reentry` keeps a request for graphics mode from entering it again.
# Each must fail devprobe with its row's words, and the restored kernel must
# match the first build and pass both probes. --texts checks that each fault
# is once in its source.
set -eu
repo=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
cd "$repo"

# apply_fault SOURCE FAULT: put FAULT into SOURCE; with
# RACH2_FAULT_TEXTS set, only check its text and write nothing.
apply_fault()
{
    python3 - "$1" "$2" <<'PY'
from pathlib import Path
import sys
import os
import pathlib
if os.environ.get('RACH2_FAULT_TEXTS'):
    pathlib.Path.write_text = lambda self, *args, **kwargs: 0
path = Path(sys.argv[1])
old, new = {
    'owner': ('if (fb_owner_pid != current_proc->tgid &&',
              'if (0 && fb_owner_pid != current_proc->tgid &&'),
    'reentry': ('if (!gfx_is_active()) {', 'if (0) {'),
}[sys.argv[2]]
text = path.read_text()
if text.count(old) != 1:
    raise SystemExit('framebuffer-guest: target is not unique')
path.write_text(text.replace(old, new))
PY
}

faults='owner reentry'
if [ "${1:-}" = --texts ]; then
    for mutation in $faults; do
        RACH2_FAULT_TEXTS=1 apply_fault rach2/sys/syscall.c "$mutation" >/dev/null || exit 1
    done
    echo 'framebuffer-guest: texts: every fault is once in its source'
    exit 0
fi

RACH2_ARCH=${RACH2_ARCH:-${ARCH:-i686}}
REPO=$repo
. "$repo/mk/arch.sh"
RACH2_QEMU=$ARCH_QEMU
export RACH2_QEMU ARCH_BUILD
tree=build$ARCH_SUFFIX
. "$repo/tests/system/runlock.sh"
runlock_hold "$repo" framebuffer-guest "$repo/tests/system/framebuffer_mutate.sh" "$@"
mkdir -p build
work=$(mktemp -d "$repo/build/framebuffer-guest.XXXXXX")
source=rach2/sys/syscall.c
cp "$source" "$work/syscall.saved"
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

# Remove syscall.c's object and the kernel, and make the kernel again.
rebuild()
{
    rm -f "$tree"/rach2/sys/syscall.o "$tree"/rach2/kernel
    make -s -C rach2 "ARCH=$RACH2_ARCH"
}

# Put back the saved syscall.c and touch it.
restore()
{
    cp "$work/syscall.saved" "$source"
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

# run_probe PROBE TOKEN EXPECTED: boot the kernel of the phase named by
# `phase` and run PROBE. With EXPECTED `pass` it must print `TOKEN OK`;
# otherwise it must fail with the words in `detail`.
run_probe()
{
    probe=$1
    token=$2
    expected=$3
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
            echo "framebuffer-guest: mutation passed: $phase" >&2; exit 1;
        }
        grep -F "$detail" "$work/$phase.$probe.serial.log" >/dev/null || exit 1
    fi
    echo "framebuffer-guest: result: $phase $probe status $status"
}

# run_phase PHASE EXPECTED: build the kernel and run devprobe, and loopprobe
# too when EXPECTED is `pass`.
run_phase()
{
    phase=$1
    expected=$2
    rebuild >"$work/$phase.build.log" 2>&1
    cat "$work/$phase.build.log"
    cp "$tree"/rach2/kernel "$work/$phase.kernel"
    cksum "$source" "$tree"/rach2/kernel "$work/test.krffs" >"$work/$phase.artifacts"
    run_probe devprobe DEVPROBE "$expected"
    if [ "$expected" = pass ]; then
        run_probe loopprobe LOOPPROBE pass
    fi
}

echo "framebuffer-guest: logs: $work"
make -s -C tests ../"$tree"/tests/devprobe ../"$tree"/tests/loopprobe "ARCH=$RACH2_ARCH" >"$work/build.log" 2>&1
# These three image tools do not use mkhash's vendored crypt source.
make -s -C krffs -o libc-check ../build/krffs/mkfs.krffs \
    ../build/krffs/stageimage.krffs ../build/krffs/readfile.krffs "ARCH=$RACH2_ARCH" >>"$work/build.log" 2>&1
make -s build/tests/system/harness build/tests/system/runutil "ARCH=$RACH2_ARCH" >>"$work/build.log" 2>&1
cat >"$work/test.manifest" <<MANIFEST
file 0755 /devprobe $tree/tests/devprobe
file 0755 /loopprobe $tree/tests/loopprobe
MANIFEST
{
    dd if=/dev/zero of="$work/test.krffs" bs=1048576 count=2
    build/krffs/mkfs.krffs "$work/test.krffs"
    build/krffs/stageimage.krffs "$work/test.manifest" "$work/test.krffs"
} >>"$work/build.log" 2>&1
cat "$work/build.log"
for probe in devprobe loopprobe; do
    build/krffs/readfile.krffs "$work/test.krffs" "/$probe" "$work/$probe.staged"
    cmp "$tree/tests/$probe" "$work/$probe.staged"
done
run_phase fixed pass
for mutation in $faults; do
    changed=1
    apply_fault "$source" "$mutation"
    case "$mutation" in
        owner) detail="stale descriptor cannot release second owner's display" ;;
        reentry) detail='graphics reentry restores writable display' ;;
    esac
    run_phase "$mutation" fail
    if cmp -s "$work/fixed.kernel" "$work/$mutation.kernel"; then
        echo 'framebuffer-guest: mutation did not change kernel' >&2; exit 1;
    fi
    restore
    run_phase "$mutation-restored" pass
    cmp "$work/fixed.kernel" "$work/$mutation-restored.kernel"
done
echo 'framebuffer-guest: result: compiled faults failed, restored kernels matched'
