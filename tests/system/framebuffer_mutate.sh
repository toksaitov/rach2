#!/bin/sh
# Run the two entry 117 probes and prove the framebuffer rows in QEMU, on the
# kernel, probes and emulator of the architecture ARCH names.
set -eu
repo=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
cd "$repo"
RACH2_ARCH=${RACH2_ARCH:-${ARCH:-i686}}
REPO=$repo
. "$repo/mk/arch.sh"
RACH2_QEMU=$ARCH_QEMU
export RACH2_QEMU ARCH_BUILD
tree=build$ARCH_SUFFIX
[ ! -d build/.run.lock ] || {
    echo 'framebuffer-guest: another suite owns this tree' >&2; exit 1;
}
mkdir -p build
work=$(mktemp -d "$repo/build/framebuffer-guest.XXXXXX")
source=rach2/sys/syscall.c
cp "$source" "$work/syscall.saved"
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
    rm -f "$tree"/rach2/sys/syscall.o "$tree"/rach2/kernel
    make -s -C rach2 "ARCH=$RACH2_ARCH"
}

restore()
{
    cp "$work/syscall.saved" "$source"
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
for mutation in owner reentry; do
    changed=1
    python3 - "$source" "$mutation" <<'PY'
from pathlib import Path
import sys
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
