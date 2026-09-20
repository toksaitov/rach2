#!/bin/sh
# Prove the three exit callers and their prohibition on waits after publication.
set -eu
repo=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
cd "$repo"
[ ! -d build/.run.lock ] || {
    echo 'exit-integration: another suite owns this tree' >&2; exit 1;
}
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
    echo 'exit-integration: usage: exit_mutate.sh [--resume work-directory]' >&2; exit 1
fi
child=
changed=

stop_child()
{
    if [ -n "$child" ]; then
        kill -TERM -- "-$child" 2>/dev/null || :
        wait "$child" 2>/dev/null || :
        while kill -0 -- "-$child" 2>/dev/null; do
            sleep 1
        done
        child=
    fi
}

rebuild()
{
    rm -f build/rach2/sys/syscall_proc.o build/rach2/kernel.o \
        build/rach2/sys/proc/exit.o build/rach2/kernel
    make -s rach2
}

restore()
{
    for source in $sources; do
        cp "$work/$(basename "$source").saved" "$source"
        touch "$source"
    done
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

build_phase()
{
    rebuild >"$work/$phase.build.log" 2>&1
    cat "$work/$phase.build.log"
    cp build/rach2/kernel "$work/$phase.kernel"
    cksum rach2/sys/syscall_proc.c rach2/kernel.c rach2/sys/proc/exit.c \
        "$work/$phase.kernel" "$work/test.krffs" build/tests/exitprobe \
        >"$work/$phase.artifacts"
}

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
            exec) set -- "expect=Type 'help' for available commands" ;;
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
    if [ "$row" = exec ]; then
        grep -F 'the image was destroyed and step 1 failed' \
            "$work/$phase.$row.serial.log" >/dev/null
    fi
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
        elif [ "$row" = exec ] && \
            [ "$(grep -Fc "Type 'help' for available commands" "$work/$phase.$row.serial.log")" -eq 2 ]; then
            detail='returned to shell before reap'
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
make -s -C tests ../build/tests/exitprobe >"$work/build.log" 2>&1
if [ -f "$work/exitprobe.staged" ]; then
    cmp build/tests/exitprobe "$work/exitprobe.staged"
fi
make -s -C krffs -o libc-check ../build/krffs/mkfs.krffs \
    ../build/krffs/stageimage.krffs ../build/krffs/readfile.krffs >>"$work/build.log" 2>&1
make -s build/tests/system/harness build/tests/system/runutil >>"$work/build.log" 2>&1
cat >"$work/test.manifest" <<'MANIFEST'
file 0755 /exitprobe build/tests/exitprobe
MANIFEST
{
    dd if=/dev/zero of="$work/test.krffs" bs=1048576 count=2
    build/krffs/mkfs.krffs "$work/test.krffs"
    build/krffs/stageimage.krffs "$work/test.manifest" "$work/test.krffs"
} >>"$work/build.log" 2>&1
cat "$work/build.log"
build/krffs/readfile.krffs "$work/test.krffs" /exitprobe "$work/exitprobe.staged"
cmp build/tests/exitprobe "$work/exitprobe.staged"
phase=fixed
if [ -f "$work/fixed.kernel" ]; then
    rebuild >"$work/resume.build.log" 2>&1
    cat "$work/resume.build.log"
    cmp "$work/fixed.kernel" build/rach2/kernel
else
    build_phase
fi
for row in normal signal exec; do
    [ ! -f "$work/fixed.$row.passed" ] || continue
    run_row "$row" pass
    : >"$work/fixed.$row.passed"
done
for fault in remove wait; do
    for row in normal signal exec; do
        [ ! -f "$work/$row-$fault.passed" ] || continue
        changed=1
        python3 - "$row" "$fault" <<'PY'
from pathlib import Path
import sys

row, fault = sys.argv[1:]
path, call = {
    'normal': ('rach2/sys/syscall_proc.c', 'proc_exit_cleanup((status & 0xFF) << 8);'),
    'signal': ('rach2/kernel.c', 'proc_exit_cleanup(signo & 0x7F);'),
    'exec': ('rach2/sys/syscall_proc.c', 'proc_exit_cleanup(SIGKILL & 0x7F);'),
}[row]
path = Path(path)
source = path.read_text()
if source.count(call) != 1:
    raise SystemExit('exit-integration: caller is not unique')
replacement = '(void)0;'
if fault == 'wait':
    helper = Path('rach2/sys/proc/exit.c')
    helper.write_text(helper.read_text() + '''

#include "wait_queue.h"
extern volatile uint32_t kernel_ticks;
void exit_proof_wait(void);

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
    __asm__ __volatile__("pushfl; popl %0; cli" : "=r"(flags) : : "memory");
    wait_queue_init(&queue);
    target = kernel_ticks + 2;
    current_proc->wakeup_tick = target ? target : 1;
    wq_wait_event_nointr(&queue, exit_proof_awake, 0);
    __asm__ __volatile__("pushl %0; popfl" : : "r"(flags) : "memory", "cc");
}
''')
    source = 'void exit_proof_wait(void);\n' + source
    replacement = call + '\n    exit_proof_wait();'
path.write_text(source.replace(call, replacement))
PY
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
echo 'exit-integration: result: pass, six compiled faults failed their rows; six restored kernels matched and passed'
