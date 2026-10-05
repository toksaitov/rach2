#!/bin/sh
# Prove syscall stepping and x86-64 debug entry with one small guest image.
# QEMU TCG does not defer a MOV SS watchpoint through the next instruction.
# For the entry proof, inject int $1 before syscall_entry changes GS or RSP;
# this exercises that entry state without depending on the emulator's delay.
# The fixed entry must preserve a syscall on an invalid user stack. Removing
# its interrupt stack or its GS check must fail. The original kernel is
# restored, rebuilt and required to match and pass at the end. It holds the
# record lock on build/.run.guard and the run lock, as tests/run does, so that
# no suite builds or boots the tree, and no other proof saves a source, while
# a fault is in it.
set -eu
repo=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
cd "$repo"
mkdir -p build
if [ "${RACH2_DEBUG_LOCK:-}" != "$$" ]; then
    helper=$(mktemp -d "${TMPDIR:-/tmp}/rach2-debug.XXXXXX")
    cc -O2 -Wall -Wextra -o "$helper/runutil" tests/system/runner/runutil.c || {
        rm -rf "$helper"; exit 1;
    }
    RACH2_DEBUG_LOCK=$$; RACH2_RUN_WORK=$helper
    export RACH2_DEBUG_LOCK RACH2_RUN_WORK
    exec "$helper/runutil" lock build/.run.guard build/.run.lock/owner \
        sh "$repo/tests/system/ptrace_debug_mutate.sh" "$@"
fi
rm -rf "$RACH2_RUN_WORK"
unset RACH2_DEBUG_LOCK RACH2_RUN_WORK
mkdir build/.run.lock 2>/dev/null || {
    echo 'ptrace-debug: another run owns this tree' >&2; exit 1;
}
trap 'rm -rf build/.run.lock' EXIT
RACH2_ARCH=${RACH2_ARCH:-${ARCH:-i686}}
# shellcheck disable=SC2034
REPO=$repo
# shellcheck source=mk/arch.sh
. "$repo/mk/arch.sh"
RACH2_QEMU=$ARCH_QEMU
export RACH2_QEMU
mkdir -p build/tests
work=$(mktemp -d "$repo/build/tests/ptrace-debug.XXXXXX")
printf 'pid %s\npgid %s\nargs ptrace_debug_mutate.sh\nsince %s\nwork %s\n' "$$" \
    "$(ps -o pgid= -p $$ | tr -d ' ')" "$(date '+%Y-%m-%d %H:%M:%S')" "$work" \
    > build/.run.lock/owner.new
mv build/.run.lock/owner.new build/.run.lock/owner
sources='rach2/sys/syscall.c rach2/sys/arch/x86_64/idt.c rach2/sys/arch/x86_64/isr_handler.s'
for file in $sources; do
    mkdir -p "$work/saved/$(dirname "$file")"
    cp "$file" "$work/saved/$file"
done
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
    rm -rf build/.run.lock
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
    else
        if [ "$status" -eq 0 ] || ! grep -Fq "$detail" "$work/$phase.serial.log"; then
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
if [ "$RACH2_ARCH" = x86_64 ]; then
    mutate inject
    run_phase injected entry pass
    mutate stack
    run_phase no-stack entry fail 'kernel exception: double fault'
    restore
    mutate inject
    run_phase stack-restored entry pass
    cmp "$work/injected.kernel" "$work/stack-restored.kernel"
    mutate gs
    run_phase wrong-gs entry fail 'kernel exception: double fault'
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
