#!/bin/sh
# exit_mutate.sh [--resume WORK | --texts]: show that exitprobe's normal and
# signal rows catch two faults in each of the kernel's exit callers, the exit
# system call in rach2/sys/syscall_proc.c and the death by a signal in
# rach2/kernel.c, on the kernel, probe and emulator of the architecture ARCH
# names.
#
# `remove` takes the caller's call of proc_exit_cleanup out, and `wait` makes
# the process sleep for two ticks after the cleanup has published its death,
# which the exit contract forbids. Each fault must fail its row with
# `EXITPROBE FAIL ROW: reap or release`, or, in the signal row, with the
# panic of a dying process scheduled again. The restored kernel must match
# the first build byte for byte and pass; the first build also runs the exec
# row. --resume continues in the directory WORK, skipping the rows its
# markers record as passed. --texts checks that each fault is once in its
# source.
set -eu
repo=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
cd "$repo"

# apply_fault ROW FAULT: put FAULT into ROW's caller; with
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

row, fault = sys.argv[1:]
path, call, removed, waited = {
    'normal': ('rach2/sys/syscall_proc.c', 'if (!proc_exit_cleanup(code)) {', 'if (1) {',
               'if (!exit_proof_waited(proc_exit_cleanup(code))) {'),
    'signal': ('rach2/kernel.c', 'last = proc_exit_cleanup(signo);', 'last = 0;',
               'last = exit_proof_waited(proc_exit_cleanup(signo));'),
}[row]
path = Path(path)
source = path.read_text()
if source.count(call) != 1:
    raise SystemExit('exit-integration: caller is not unique')
replacement = removed
if fault == 'wait':
    helper = Path('rach2/sys/proc/exit.c')
    helper.write_text(helper.read_text() + '''

#include "wait_queue.h"
#include "../lib/kirq.h"
extern volatile uint32_t kernel_ticks;
void exit_proof_wait(void);
int exit_proof_waited(int last);

/** Complete the wait after the real timer wakes this process. */
static int exit_proof_awake(void *arg)
{
    (void)arg;
    return current_proc->wakeup_tick == 0;
}

/** Exercise a finite wait after publication, violating the exit contract. */
void exit_proof_wait(void)
{
    wait_queue_t queue;
    uint32_t flags, target;
    flags = kirq_save();
    wait_queue_init(&queue);
    target = kernel_ticks + 2;
    current_proc->wakeup_tick = target ? target : 1;
    wq_wait_event_nointr(&queue, exit_proof_awake, 0);
    kirq_restore(flags);
}

/** Wait after the cleanup has published the death, and pass on its result. */
int exit_proof_waited(int last)
{
    exit_proof_wait();
    return last;
}
''')
    source = 'int exit_proof_waited(int last);\n' + source
    replacement = waited
path.write_text(source.replace(call, replacement))
PY
}

if [ "${1:-}" = --texts ]; then
    for fault in remove wait; do
        for row in normal signal; do
            RACH2_FAULT_TEXTS=1 apply_fault "$row" "$fault" >/dev/null || exit 1
        done
    done
    echo 'exit-integration: texts: every fault is once in its source'
    exit 0
fi

. "$repo/tests/system/runlock.sh"
runlock_hold "$repo" exit-integration "$repo/tests/system/exit_mutate.sh" "$@"

RACH2_ARCH=${RACH2_ARCH:-${ARCH:-i686}}
REPO=$repo
. "$repo/mk/arch.sh"
RACH2_QEMU=$ARCH_QEMU
export RACH2_QEMU ARCH_BUILD
tree=build$ARCH_SUFFIX
mkdir -p build
sources='rach2/sys/syscall_proc.c rach2/kernel.c rach2/sys/proc/exit.c'
if [ "$#" -eq 0 ]; then
    work=$(mktemp -d "$repo/build/exit-proof.XXXXXX")
    for source in $sources; do
        cp "$source" "$work/$(basename "$source").saved"
    done
elif [ "$#" -eq 2 ] && [ "$1" = --resume ]; then
    work=$(CDPATH='' cd -- "$2" && pwd)
    for source in $sources; do
        cmp "$source" "$work/$(basename "$source").saved"
    done
else
    echo 'exit-integration: usage: exit_mutate.sh [--resume work-directory | --texts]' >&2; exit 1
fi
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

# Remove the objects of the three sources and the kernel, and make the
# kernel again.
rebuild()
{
    rm -f "$tree"/rach2/sys/syscall_proc.o "$tree"/rach2/kernel.o \
        "$tree"/rach2/sys/proc/exit.o "$tree"/rach2/kernel
    make -s rach2 "ARCH=$RACH2_ARCH"
}

# Put back the saved copies of the three sources and touch them.
restore()
{
    for source in $sources; do
        cp "$work/$(basename "$source").saved" "$source"
        touch "$source"
    done
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

# Build the kernel for the phase named by `phase`, keep a copy, and record
# the checksums of the sources and of what the boot reads.
build_phase()
{
    rebuild >"$work/$phase.build.log" 2>&1
    cat "$work/$phase.build.log"
    cp "$tree"/rach2/kernel "$work/$phase.kernel"
    cksum rach2/sys/syscall_proc.c rach2/kernel.c rach2/sys/proc/exit.c \
        "$work/$phase.kernel" "$work/test.krffs" "$tree"/tests/exitprobe \
        >"$work/$phase.artifacts"
}

# run_row ROW EXPECTED: boot the phase's kernel and run exitprobe's ROW.
# With EXPECTED `pass` the row must pass and the program return to the shell;
# otherwise it must fail with one of the row's two failures and never pass.
run_row()
{
    row=$1
    expected=$2
    if [ "$expected" = pass ]; then
        set -- "expect=EXITPROBE OK $row" 'expect=returning to kernel shell'
    else
        case $row in
            normal) set -- "expect=EXITPROBE FAIL $row: reap or release" ;;
            signal) set -- 'expect=panic: stopped: a dying process was scheduled again' ;;
        esac
    fi
    build/tests/system/runutil group build/tests/system/harness \
        --kernel "$work/$phase.kernel" --initrd "$work/test.krffs" \
        --net hubport,id=net0,hubid=0 --qemu-arg=-cpu --qemu-arg=max \
        --append rach2.rescue --timeout 30 --steptmo 15 \
        --ok "EXITPROBE OK $row" --never 'EXITPROBE FAIL' --never 'panic: stopped:' \
        --log "$work/$phase.$row.serial.log" \
        "expect=Type 'help'" "send=exec /exitprobe $row $timeout" \
        "expect=exitprobe: waiting: $row" "$@" >"$work/$phase.$row.log" 2>&1 &
    child=$!
    status=0
    wait "$child" || status=$?
    stop_child
    cat "$work/$phase.$row.log" "$work/$phase.$row.serial.log"
    grep -F "exitprobe: waiting: $row" "$work/$phase.$row.serial.log" >/dev/null
    if [ "$expected" = pass ]; then
        [ "$status" -eq 0 ] || exit 1
        grep -F 'exited with status 0, returning to kernel shell' \
            "$work/$phase.$row.serial.log" >/dev/null
    else
        [ "$status" -ne 0 ] || {
            echo "exit-integration: fault passed: $phase" >&2; exit 1;
        }
        if grep -F "EXITPROBE FAIL $row: reap or release" \
            "$work/$phase.$row.serial.log" >/dev/null; then
            detail='reap or release did not complete'
        elif [ "$row" = signal ] && grep -F 'panic: stopped: a dying process was scheduled again' \
            "$work/$phase.$row.serial.log" >/dev/null; then
            detail='dying process resumed'
        else
            echo "exit-integration: unexpected failure: $phase $row" >&2; exit 1
        fi
        if grep -F "EXITPROBE OK $row" "$work/$phase.$row.serial.log" >/dev/null; then
            echo "exit-integration: failed row also passed: $phase $row" >&2; exit 1
        fi
        echo "exit-integration: fail: $row: $detail"
    fi
    echo "exit-integration: result: $phase $row status $status"
}

echo "exit-integration: logs: $work"
timeout=$(python3 - <<'PY'
import math
import os

factor = float(os.environ.get('RACH2_TIMEOUT_FACTOR', '1'))
if not 1 <= factor <= 64:
    raise SystemExit('exit-integration: timeout factor must be from 1 through 64')
print(math.ceil(5000 * factor))
PY
)
RACH2_DEADLINES=scaled
RACH2_DEADLINE_FACTOR=${RACH2_TIMEOUT_FACTOR:-1}
export RACH2_DEADLINES RACH2_DEADLINE_FACTOR
make -s -C tests ../"$tree"/tests/exitprobe "ARCH=$RACH2_ARCH" >"$work/build.log" 2>&1
if [ -f "$work/exitprobe.staged" ]; then
    cmp "$tree"/tests/exitprobe "$work/exitprobe.staged"
fi
make -s -C krffs -o libc-check ../build/krffs/mkfs.krffs \
    ../build/krffs/stageimage.krffs ../build/krffs/readfile.krffs "ARCH=$RACH2_ARCH" >>"$work/build.log" 2>&1
make -s build/tests/system/harness build/tests/system/runutil "ARCH=$RACH2_ARCH" >>"$work/build.log" 2>&1
cat >"$work/test.manifest" <<MANIFEST
file 0755 /exitprobe $tree/tests/exitprobe
MANIFEST
{
    dd if=/dev/zero of="$work/test.krffs" bs=1048576 count=2
    build/krffs/mkfs.krffs "$work/test.krffs"
    build/krffs/stageimage.krffs "$work/test.manifest" "$work/test.krffs"
} >>"$work/build.log" 2>&1
cat "$work/build.log"
build/krffs/readfile.krffs "$work/test.krffs" /exitprobe "$work/exitprobe.staged"
cmp "$tree"/tests/exitprobe "$work/exitprobe.staged"
phase=fixed
if [ -f "$work/fixed.kernel" ]; then
    rebuild >"$work/resume.build.log" 2>&1
    cat "$work/resume.build.log"
    cmp "$work/fixed.kernel" "$tree"/rach2/kernel
else
    build_phase
fi
for row in normal signal exec; do
    [ ! -f "$work/fixed.$row.passed" ] || continue
    run_row "$row" pass
    : >"$work/fixed.$row.passed"
done
for fault in remove wait; do
    for row in normal signal; do
        [ ! -f "$work/$row-$fault.passed" ] || continue
        changed=1
        apply_fault "$row" "$fault"
        phase="$row-$fault"
        build_phase
        if cmp -s "$work/fixed.kernel" "$work/$phase.kernel"; then
            echo "exit-integration: kernel did not change: $phase" >&2; exit 1;
        fi
        run_row "$row" fail
        restore
        phase="$row-$fault-restored"
        build_phase
        cmp "$work/fixed.kernel" "$work/$phase.kernel"
        run_row "$row" pass
        : >"$work/$row-$fault.passed"
    done
done
echo 'exit-integration: result: pass, four compiled faults failed their rows; four restored kernels matched and passed'
