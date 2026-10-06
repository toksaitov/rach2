#!/bin/sh
# Prove syscall stepping and x86-64 debug entry with one small guest image.
# QEMU TCG does not defer a MOV SS watchpoint through the next instruction.
# For the entry proof, inject int $1 before syscall_entry changes GS or RSP;
# this exercises that entry state without depending on the emulator's delay.
# The fixed entry must preserve a syscall on an invalid user stack. Removing
# its interrupt stack must end in a double fault. Choosing GS by the saved CS
# must reset the machine, a triple fault, before any handler prints. On
# x86-64, which alone defers a kernel hit, removing its post before the
# pending-signal check must fail the sigwait row. Where /dev/kvm is usable,
# the delayed and delayed-poll rows also run under KVM, which delays a watched
# MOV SS's hit into the system-call entry as hardware does. Posting that hit
# without forcing it must fail delayed; ignoring it in signal_pending must
# fail delayed-poll. Elsewhere the script says those rows did not run.
# The original kernel is restored, rebuilt and required to match and pass at
# the end. The script holds the tree through runlock.sh, as tests/run does,
# so no suite builds or boots the tree and no other proof saves a source
# while a fault is in it.
set -eu
repo=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
cd "$repo"
. "$repo/tests/system/runlock.sh"
runlock_hold "$repo" ptrace-debug "$repo/tests/system/ptrace_debug_mutate.sh" "$@"
RACH2_ARCH=${RACH2_ARCH:-${ARCH:-i686}}
# shellcheck disable=SC2034
REPO=$repo
# shellcheck source=mk/arch.sh
. "$repo/mk/arch.sh"
RACH2_QEMU=$ARCH_QEMU
export RACH2_QEMU
mkdir -p build/tests
work=$(mktemp -d "$repo/build/tests/ptrace-debug.XXXXXX")
sources='rach2/sys/syscall.c rach2/sys/syscall_proc.c rach2/sys/arch/x86/debugreg.c
    rach2/sys/arch/x86_64/idt.c rach2/sys/arch/x86_64/isr_handler.s'
for file in $sources; do
    mkdir -p "$work/saved/$(dirname "$file")"
    cp "$file" "$work/saved/$file"
done
child=
changed=
kvm=

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

restore()
{
    for file in $sources; do
        cp "$work/saved/$file" "$file"
        touch "$file"
    done
}

rebuild()
{
    make -s ARCH="$RACH2_ARCH" rach2
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

mutate()
{
    changed=1
    python3 - "$1" <<'PY'
from pathlib import Path
import sys

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
    'inject': ('rach2/sys/arch/x86_64/isr_handler.s',
               'syscall_entry:\n    swapgs', 'syscall_entry:\n    int $1\n    swapgs'),
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

run_phase()
{
    phase=$1
    row=$2
    expected=$3
    detail=${4:-}
    set --
    [ -z "$kvm" ] || set -- --qemu-arg=-accel --qemu-arg=kvm
    rebuild >"$work/$phase.build.log" 2>&1
    cat "$work/$phase.build.log"
    cp "$ARCH_BUILD/rach2/kernel" "$work/$phase.kernel"
    cksum "$work/$phase.kernel" "$work/test.krffs" "$work/ptracemusl" \
        >"$work/$phase.artifacts"
    build/tests/system/runutil group build/tests/system/harness \
        --kernel "$work/$phase.kernel" --initrd "$work/test.krffs" \
        --net hubport,id=net0,hubid=0 --qemu-arg=-cpu --qemu-arg=max "$@" \
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
        if [ -n "$kvm" ] && grep -Fq 'not run: debug exception preceded' "$work/$phase.serial.log"; then
            echo "ptrace-debug: fail: $phase: KVM did not delay the watchpoint" >&2
            exit 1
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
make -s ARCH="$RACH2_ARCH" musl build/tests/system/harness build/tests/system/runutil \
    >"$work/build.log" 2>&1
vendor/musl/muslgcc -O2 -Wall -Wextra -Werror -Wl,-z,noexecstack \
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
run_phase delayed delayed pass
run_phase delayed-poll delayed-poll pass
run_phase sigwait sigwait pass
kvm=
if [ -r /dev/kvm ] && [ -w /dev/kvm ]; then
    kvm=1
    run_phase delayed-kvm delayed pass
    mutate unforced
    run_phase unforced delayed fail 'delayed debug exception lost its frame or result'
    restore
    run_phase unforced-restored delayed pass
    cmp "$work/fixed.kernel" "$work/unforced-restored.kernel"
    run_phase delayed-poll-kvm delayed-poll pass
    mutate pending
    run_phase no-pending delayed-poll fail 'delayed infinite poll did not trap'
    restore
    run_phase pending-restored delayed-poll pass
    cmp "$work/fixed.kernel" "$work/pending-restored.kernel"
    kvm=
else
    echo 'ptrace-debug: not run: the delayed-watchpoint rows need KVM, which this host does not offer'
fi
if [ "$RACH2_ARCH" = x86_64 ]; then
    mutate sigwait
    run_phase no-sigwait sigwait fail 'watched sigwait did not take SIGTRAP with TRAP_HWBKPT'
    restore
    run_phase sigwait-restored sigwait pass
    cmp "$work/fixed.kernel" "$work/sigwait-restored.kernel"
    mutate inject
    run_phase injected entry pass
    mutate stack
    run_phase no-stack entry fail 'kernel exception: double fault'
    restore
    mutate inject
    run_phase stack-restored entry pass
    cmp "$work/injected.kernel" "$work/stack-restored.kernel"
    mutate gs
    run_phase wrong-gs entry fail 'qemu had already exited'
    restore
    mutate inject
    run_phase gs-restored entry pass
    cmp "$work/injected.kernel" "$work/gs-restored.kernel"
fi
restore
run_phase restored entry pass
cmp "$work/fixed.kernel" "$work/restored.kernel"
changed=
echo 'ptrace-debug: result: pass'
