#!/bin/sh
# ptrace_debug_mutate.sh [--texts]: show that ptracemusl's rows catch faults
# in system-call stepping, in the delivery of debug exceptions and in the
# x86-64 debug entry, with one small guest image for the architecture ARCH
# names.
#
# The ordinary rows exercise real user and kernel watchpoints. The delayed
# rows use a fixture: int $1 at the first instruction of the system-call
# entry, before x86-64 changes GS or RSP. debugreg_defer_user recognizes the
# saved PC just after that two-byte instruction. The processor builds the
# debug frame and runs the real entry and return code on either architecture;
# no MOV SS delay or accelerator-specific behavior is required. DR7 is armed,
# while SIGTRAP is blocked, so a deferred user hit must force its signal.
#
# Each fixture passes before a fault, fails with the fault, and passes again
# after restoration. Missing injection and a wrong entry PC must fail too.
# x86-64 additionally checks its interrupt stack and GS selection. The fixture
# does not test a processor's MOV SS inhibition; it tests the kernel state that
# a delayed hit reaches. The original kernel is rebuilt and compared at exit.
# --texts checks that each replacement occurs once without writing a source.
set -eu
repo=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
cd "$repo"

# apply_fault FAULT: put FAULT into its source; with
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

changes = {
    'step': ('rach2/sys/syscall.c',
             '    if (current_proc && (current_proc->ptrace & (PT_SYSCALL | PT_STEP))) {',
             '    if (current_proc && (current_proc->ptrace & PT_SYSCALL)) {'),
    'unforced': ('rach2/sys/arch/x86/debugreg.c',
                 '            signal_force(current_proc, SIGTRAP, &info);',
                 '            signal_post_thread(current_proc, SIGTRAP, &info);'),
    'pending': ('rach2/sys/syscall_proc.c',
                '    debug_pending = arch_debug_pending();',
                '    debug_pending = 0;'),
    'sigwait': ('rach2/sys/syscall_proc.c',
                '    arch_debug_deliver();\n    set = current_proc->sig_pending',
                '    set = current_proc->sig_pending'),
    'inject': ('rach2/sys/arch/' + os.environ.get('RACH2_ARCH', 'i686') + '/isr_handler.s',
               'syscall_entry:\n    swapgs' if os.environ.get('RACH2_ARCH') == 'x86_64' else 'isr128:\n    cli',
               'syscall_entry:\n    int $1\n    swapgs' if os.environ.get('RACH2_ARCH') == 'x86_64' else 'isr128:\n    int $1\n    cli'),
    'entry-pc': ('rach2/sys/arch/x86/debugreg.c',
                 '    if (pc != (uintptr_t)&DEBUG_SYSCALL_ENTRY) {',
                 '    if (pc != (uintptr_t)&DEBUG_SYSCALL_ENTRY + 2) {'),
    'stack': ('rach2/sys/arch/x86_64/idt.c',
              '    set_gate(1, isr1, GATE_KERNEL, IST_DEBUG);',
              '    set_gate(1, isr1, GATE_KERNEL, 0);'),
    'gs': ('rach2/sys/arch/x86_64/isr_handler.s',
           '    js .debug_kernel_gs',
           '    testb $3, F_CS(%rsp)\n    jz .debug_kernel_gs'),
}
file, old, new = changes[sys.argv[1]]
p = Path(file)
s = p.read_text()
if s.count(old) != 1:
    sys.exit('ptrace-debug: fault text must occur once: ' + sys.argv[1])
p.write_text(s.replace(old, new))
PY
}

if [ "${1:-}" = --texts ]; then
    RACH2_FAULT_TEXTS=1
    export RACH2_FAULT_TEXTS
    for RACH2_ARCH in i686 x86_64; do
        export RACH2_ARCH
        for fault in step unforced pending sigwait inject entry-pc stack gs; do
            apply_fault "$fault" >/dev/null
        done
    done
    echo 'ptrace-debug: texts: every fault is once in its source'
    exit 0
fi


. "$repo/tests/system/runlock.sh"
runlock_hold "$repo" ptrace-debug "$repo/tests/system/ptrace_debug_mutate.sh" "$@"
RACH2_ARCH=${RACH2_ARCH:-${ARCH:-i686}}
export RACH2_ARCH
# shellcheck disable=SC2034
REPO=$repo
# shellcheck source=mk/arch.sh
. "$repo/mk/arch.sh"
RACH2_QEMU=$ARCH_QEMU
export RACH2_QEMU
mkdir -p build/tests
work=
sources='rach2/sys/syscall.c rach2/sys/syscall_proc.c rach2/sys/arch/x86/debugreg.c
    rach2/sys/arch/x86_64/idt.c rach2/sys/arch/x86_64/isr_handler.s
    rach2/sys/arch/i686/isr_handler.s'
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

# Put back the saved sources and touch them.
restore()
{
    for file in $sources; do
        cp "$work/saved/$file" "$file"
        touch "$file"
    done
}

# Make the kernel again.
rebuild()
{
    make -s ARCH="$RACH2_ARCH" rach2
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
        [ -z "$work" ] || rm -rf "$work"
    fi
}
trap cleanup EXIT
trap 'keep=; exit 129' HUP
trap 'keep=; exit 130' INT
trap 'keep=; exit 143' TERM
work=$(mktemp -d "$repo/build/tests/ptrace-debug.XXXXXX")
for file in $sources; do
    mkdir -p "$work/saved/$(dirname "$file")"
    cp "$file" "$work/saved/$file"
done

# mutate FAULT: mark the sources changed and put FAULT in.
mutate()
{
    changed=1
    apply_fault "$1"
}

# run_phase PHASE ROW EXPECTED [DETAIL]: build the kernel and run ptracemusl's
# ROW. With EXPECTED `pass` it must pass and print DETAIL when given;
# otherwise it must fail and print DETAIL. The kernel's exit record and the
# success token must both agree.
run_phase()
{
    phase=$1
    row=$2
    expected=$3
    detail=${4:-}
    rebuild >"$work/$phase.build.log" 2>&1
    cat "$work/$phase.build.log"
    cp "$ARCH_BUILD/rach2/kernel" "$work/$phase.kernel"
    cksum "$work/$phase.kernel" "$work/test.krffs" "$work/ptracemusl" \
        >"$work/$phase.artifacts"
    build/tests/system/runutil group build/tests/system/harness \
        --kernel "$work/$phase.kernel" --initrd "$work/test.krffs" \
        --net hubport,id=net0,hubid=0 --qemu-arg=-cpu --qemu-arg=max \
        --append rach2.rescue --timeout 40 --steptmo 15 \
        --ok 'PTRACEMUSL OK' --never 'PTRACEMUSL FAIL' \
        --log "$work/$phase.serial.log" \
        "expect=Type 'help'" "send=exec /ptracemusl $row" \
        'expect=PTRACEMUSL' 'expect=returning to kernel shell' \
        >"$work/$phase.log" 2>&1 &
    child=$!
    status=0
    wait "$child" || status=$?
    stop_child
    cat "$work/$phase.log" "$work/$phase.serial.log"
    if [ "$expected" = pass ]; then
        [ "$status" -eq 0 ] || exit 1
        grep -Eq '^(<[0-7]>\[[0-9.]+\] )?sys_exit: init process [0-9]+ exited with status 0, returning to kernel shell' "$work/$phase.serial.log" || exit 1
        if [ -n "$detail" ]; then
            grep -Fq "$detail" "$work/$phase.serial.log" || exit 1
        fi
    else
        if [ "$status" -eq 0 ] ||
            ! cat "$work/$phase.log" "$work/$phase.serial.log" | grep -Fq "$detail"; then
            echo "ptrace-debug: fail: $phase did not produce its expected failure" >&2
            exit 1
        fi
    fi
    echo "ptrace-debug: result: $phase status $status"
}

printf 'ptrace-debug: logs: %s\n' "$work"
keep=1
make -s ARCH="$RACH2_ARCH" musl build/tests/system/harness build/tests/system/runutil \
    >"$work/build.log" 2>&1
vendor/musl/muslgcc -DPTRACE_DEBUG_ENTRY -O2 -Wall -Wextra -Werror -Wl,-z,noexecstack \
    -o "$work/ptracemusl" tests/musl/ptracemusl.c >>"$work/build.log" 2>&1
make -s -C krffs -o libc-check ../build/krffs/mkfs.krffs \
    ../build/krffs/stageimage.krffs ../build/krffs/readfile.krffs "ARCH=$RACH2_ARCH" >>"$work/build.log" 2>&1
printf 'file 0755 /ptracemusl %s/ptracemusl\n' "$work" >"$work/test.manifest"
{
    dd if=/dev/zero of="$work/test.krffs" bs=1048576 count=2
    build/krffs/mkfs.krffs "$work/test.krffs"
    build/krffs/stageimage.krffs "$work/test.manifest" "$work/test.krffs"
} >>"$work/build.log" 2>&1
cat "$work/build.log"
build/krffs/readfile.krffs "$work/test.krffs" /ptracemusl "$work/staged"
cmp "$work/ptracemusl" "$work/staged"
run_phase fixed step pass
mutate step
run_phase no-step step fail 'syscall step ran the next instruction or lost its result'
restore
run_phase step-restored step pass
cmp "$work/fixed.kernel" "$work/step-restored.kernel"
run_phase copy copy pass
run_phase poll poll pass
run_phase sigwait sigwait pass
mutate entry-pc
run_phase missing-injection delayed fail 'delayed debug exception lost its status'
mutate inject
run_phase injected delayed pass 'ptracemusl: result: delayed: kernel entry'
run_phase injected-poll delayed-poll pass 'ptracemusl: result: delayed-poll: kernel entry'
mutate unforced
run_phase unforced delayed fail 'delayed debug exception lost its frame or result'
restore
mutate entry-pc
mutate inject
run_phase unforced-restored delayed pass 'ptracemusl: result: delayed: kernel entry'
cmp "$work/injected.kernel" "$work/unforced-restored.kernel"
mutate pending
run_phase no-pending delayed-poll fail 'delayed infinite poll did not trap'
restore
mutate entry-pc
mutate inject
run_phase pending-restored delayed-poll pass 'ptracemusl: result: delayed-poll: kernel entry'
cmp "$work/injected.kernel" "$work/pending-restored.kernel"
restore
mutate inject
run_phase wrong-entry-pc delayed fail 'delayed debug exception lost its status'
restore
if [ "$RACH2_ARCH" = x86_64 ]; then
    mutate sigwait
    run_phase no-sigwait sigwait fail 'watched sigwait did not take SIGTRAP with TRAP_HWBKPT'
    restore
    run_phase sigwait-restored sigwait pass
    cmp "$work/fixed.kernel" "$work/sigwait-restored.kernel"
    mutate entry-pc
    mutate inject
    run_phase entry-injected entry pass
    mutate stack
    run_phase no-stack entry fail 'kernel exception: double fault'
    restore
    mutate entry-pc
    mutate inject
    run_phase stack-restored entry pass
    cmp "$work/entry-injected.kernel" "$work/stack-restored.kernel"
    mutate gs
    run_phase wrong-gs entry fail 'qemu had already exited'
    restore
    mutate entry-pc
    mutate inject
    run_phase gs-restored entry pass
    cmp "$work/entry-injected.kernel" "$work/gs-restored.kernel"
fi
restore
run_phase restored entry pass
cmp "$work/fixed.kernel" "$work/restored.kernel"
changed=
echo 'ptrace-debug: result: pass'
