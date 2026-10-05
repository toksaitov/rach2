#!/bin/sh
# Prove sigprobe's timer rows with faults compiled into the kernel, on the
# kernel, probe and emulator of the architecture ARCH names.
#
# Usage: sh tests/system/itimer_mutate.sh
#
# sigprobe arms the real-time timer of setitimer(2) and requires a blocked
# single alarm not to end a sleep and its handler to run once unblocked, an
# interval timer to end three sleeps, and a timer other than the three to be
# refused (entries 212 and 215). The kernel is built as it is, where the
# probe must pass; with the tick's SIGALRM taken out of proc_wake_sleepers,
# where the single alarm's handler row must fail; with the timer left
# disarmed after it fires, where the interval's row must fail; with
# setitimer's check of `which` removed, where the row of a timer other than
# the three must fail; with the blocked mask taken out of signal_pending,
# where the blocked alarm's row must fail; and restored after each, where
# the probe must pass and the kernel must be the first one byte for byte.
# The probe boots alone from a 2 MiB image, which is checked to hold the
# built probe. The trap stops the probe's machine before it restores the
# sources.
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
    echo 'itimer-mutation: another suite owns this tree' >&2; exit 1;
}
mkdir -p build
work=$(mktemp -d "$repo/build/itimer-mutation.XXXXXX")
cp rach2/sys/proc/process.c "$work/process.saved"
cp rach2/sys/syscall.c "$work/syscall.saved"
cp rach2/sys/syscall_proc.c "$work/syscall_proc.saved"
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
    rm -f "$tree"/rach2/sys/proc/process.o "$tree"/rach2/sys/syscall.o \
        "$tree"/rach2/sys/syscall_proc.o \
        "$tree"/rach2/kernel
    make -s rach2 "ARCH=$RACH2_ARCH"
}

restore()
{
    cp "$work/process.saved" rach2/sys/proc/process.c
    cp "$work/syscall.saved" rach2/sys/syscall.c
    cp "$work/syscall_proc.saved" rach2/sys/syscall_proc.c
    touch rach2/sys/proc/process.c rach2/sys/syscall.c rach2/sys/syscall_proc.c
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
    build/tests/system/runutil group build/tests/system/harness \
        --kernel "$tree"/rach2/kernel --initrd "$work/test.krffs" \
        --net hubport,id=net0,hubid=0 --qemu-arg=-cpu --qemu-arg=max \
        --append rach2.rescue --timeout 120 --steptmo 90 \
        --ok "SIGPROBE OK" --never "SIGPROBE FAIL" \
        --log "$work/$phase.serial.log" \
        "expect=Type 'help'" "send=exec /sigprobe" "expect=SIGPROBE" \
        'expect=returning to kernel shell' >"$work/$phase.log" 2>&1 &
    child=$!
    status=0
    wait "$child" || status=$?
    stop_child
    cat "$work/$phase.log" "$work/$phase.serial.log"
    if [ "$expected" = pass ]; then
        [ "$status" -eq 0 ] || exit 1
    else
        [ "$status" -ne 0 ] || {
            echo "itimer-mutation: the probe passed with its fault: $phase" >&2; exit 1;
        }
        grep -F "FAIL: $detail" "$work/$phase.serial.log" >/dev/null || exit 1
    fi
    echo "itimer-mutation: result: $phase status $status"
}

run_phase()
{
    phase=$1
    expected=$2
    rebuild >"$work/$phase.build.log" 2>&1
    cat "$work/$phase.build.log"
    cp "$tree"/rach2/kernel "$work/$phase.kernel"
    cksum rach2/sys/proc/process.c rach2/sys/syscall.c rach2/sys/syscall_proc.c "$tree"/rach2/kernel \
        "$work/test.krffs" >"$work/$phase.artifacts"
    run_probe "$expected"
}

# fault FILE OLD NEW: replace the lines OLD of FILE, which must occur once,
# with NEW. The mark is set first, so a trap that runs while the file is
# being written still restores it.
fault()
{
    changed=1
    python3 - "$1" "$2" "$3" <<'PYTHON'
from pathlib import Path
import sys

path = Path(sys.argv[1])
text = path.read_text()
old, new = sys.argv[2] + '\n', sys.argv[3] + '\n'
if text.count(old) != 1:
    raise SystemExit('itimer-mutation: the lines to change are not unique: ' + sys.argv[2])
path.write_text(text.replace(old, new))
PYTHON
}

echo "itimer-mutation: logs: $work"
make -s -C tests ../"$tree"/tests/sigprobe "ARCH=$RACH2_ARCH" >"$work/build.log" 2>&1
# These three image tools do not use mkhash's vendored crypt source.
make -s -C krffs -o libc-check ../build/krffs/mkfs.krffs \
    ../build/krffs/stageimage.krffs ../build/krffs/readfile.krffs "ARCH=$RACH2_ARCH" >>"$work/build.log" 2>&1
make -s build/tests/system/harness build/tests/system/runutil "ARCH=$RACH2_ARCH" >>"$work/build.log" 2>&1
cat >"$work/test.manifest" <<MANIFEST
file 0755 /sigprobe $tree/tests/sigprobe
MANIFEST
{
    dd if=/dev/zero of="$work/test.krffs" bs=1048576 count=2
    build/krffs/mkfs.krffs "$work/test.krffs"
    build/krffs/stageimage.krffs "$work/test.manifest" "$work/test.krffs"
} >>"$work/build.log" 2>&1
cat "$work/build.log"
build/krffs/readfile.krffs "$work/test.krffs" /sigprobe "$work/sigprobe.staged"
cmp "$tree"/tests/sigprobe "$work/sigprobe.staged"
run_phase fixed pass

fault rach2/sys/proc/process.c '            signal_post(p, SIGALRM);' '            (void)0;'
detail='and its handler runs once SIGALRM is unblocked'
run_phase unposted fail
if cmp -s "$work/fixed.kernel" "$work/unposted.kernel"; then
    echo 'itimer-mutation: the first fault did not change the kernel' >&2; exit 1
fi
restore
run_phase posted pass
cmp "$work/fixed.kernel" "$work/posted.kernel"

fault rach2/sys/proc/process.c \
    '            p->alarm_tick = p->alarm_interval != 0 ? proc_alarm_at(now, p->alarm_interval) : 0;' \
    '            p->alarm_tick = 0;'
detail='the interval timer ends three sleeps in turn'
run_phase unrearmed fail
if cmp -s "$work/fixed.kernel" "$work/unrearmed.kernel"; then
    echo 'itimer-mutation: the second fault did not change the kernel' >&2; exit 1
fi
restore
run_phase rearmed pass
cmp "$work/fixed.kernel" "$work/rearmed.kernel"

fault rach2/sys/syscall.c '    if (which < ITIMER_REAL || which > ITIMER_PROF) {
        return -EINVAL;
    }
    if (!unew || copy_from_user(&set, unew, sizeof(set)) != 0) {' \
    '    if (!unew || copy_from_user(&set, unew, sizeof(set)) != 0) {'
detail='a timer other than the three is refused and arms nothing'
run_phase unchecked fail
if cmp -s "$work/fixed.kernel" "$work/unchecked.kernel"; then
    echo 'itimer-mutation: the third fault did not change the kernel' >&2; exit 1
fi
restore
run_phase checked pass
cmp "$work/fixed.kernel" "$work/checked.kernel"

fault rach2/sys/syscall_proc.c '    deliverable = sig_pending_now() & ~current_proc->sig_blocked;
    sig_irq_restore(flags);' \
    '    deliverable = sig_pending_now();
    sig_irq_restore(flags);'
detail='a blocked alarm does not end a sleep and leaves the timer disarmed'
run_phase unmasked fail
if cmp -s "$work/fixed.kernel" "$work/unmasked.kernel"; then
    echo 'itimer-mutation: the fourth fault did not change the kernel' >&2; exit 1
fi
restore
run_phase masked pass
cmp "$work/fixed.kernel" "$work/masked.kernel"
echo 'itimer-mutation: result: pass: each fault failed its row; the restored kernels passed and match the first'
