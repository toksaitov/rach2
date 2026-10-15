#!/bin/sh
# Prove that fork's kernel stack comes back clear, with a guard page below it
# that an overrun is known by, and that the boot stack, which is no heap's, is
# never given back.
# `kstack_alloc`, `kstack_free` and `kstack_overrun_pid` are extracted
# unchanged from a private copy of rach2/sys/syscall_proc.c, with
# KERNEL_STACK_SIZE from rach2/sys/proc/process.h and PAGE_SIZE from
# rach2/sys/arch/i686/cpu.h, and compiled under ASan and UBSan in
# tests/host/fixtures/kstack.c, whose heap hands out pages full of code
# bytes.
#
# Usage: kstack_mutate.sh [--check | --texts]
# With --check it compiles and runs the source as it is, which is what the
# host suite does; without, it also compiles each fault in, requires its
# row to fail, and requires the restored source to pass again. The faults
# are the clearing removed; the clearing of the top page alone, the part
# fork writes anyway; the stack taken from the heap without a guard; the
# guard looked for in the stack's own lowest page; a thread with no stack
# looked at like any other, its guard wrapped below zero; and the boot stack
# given back like any other.
set -eu
repo=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
cd "$repo"

# apply_fault FILE FAULT: put FAULT into FILE, a copy of syscall_proc.c; with
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
text = path.read_text()
clear = '        k_memset(base, 0, KERNEL_STACK_SIZE);\n'
changes = {
    'none': ('static uintptr_t kstack_alloc(void)\n{', clear, ''),
    'top': ('static uintptr_t kstack_alloc(void)\n{', clear,
            '        k_memset((char *)base + KERNEL_STACK_SIZE - PAGE_SIZE, 0, PAGE_SIZE);\n'),
    'unguarded': ('static uintptr_t kstack_alloc(void)\n{',
                  '    void *base = kalloc_guarded(KERNEL_STACK_SIZE / PAGE_SIZE);\n',
                  '    void *base = kalloc_pages(KERNEL_STACK_SIZE / PAGE_SIZE, NULL);\n'),
    'guard-in-stack': ('int kstack_overrun_pid(uintptr_t addr)\n{',
                       '    guard = current_proc->kernel_stack - KERNEL_STACK_SIZE - PAGE_SIZE;\n',
                       '    guard = current_proc->kernel_stack - KERNEL_STACK_SIZE;\n'),
    'empty': ('int kstack_overrun_pid(uintptr_t addr)\n{',
              '    if (!current_proc || current_proc->kernel_stack < KERNEL_STACK_SIZE + PAGE_SIZE) {\n',
              '    if (!current_proc) {\n'),
    'boot-freed': ('static void kstack_free(uintptr_t top)\n{',
                   '    if (top != 0 && top != (uintptr_t)stack + KERNEL_STACK_SIZE) {\n',
                   '    if (top != 0) {\n'),
}
head, old, new = changes[sys.argv[2]]
start = text.index(head)
end = text.index('\n}\n', start)
body = text[start:end]
if body.count(old) != 1:
    raise SystemExit('kstack-proof: the fault is not unique in its function: ' + sys.argv[2])
path.write_text(text[:start] + body.replace(old, new) + text[end:])
PY
}

faults='none top unguarded guard-in-stack empty boot-freed'
if [ "${1:-}" = --texts ]; then
    for fault in $faults; do
        RACH2_FAULT_TEXTS=1 apply_fault rach2/sys/syscall_proc.c "$fault" >/dev/null || exit 1
    done
    echo 'kstack-proof: texts: every fault is once in its source'
    exit 0
fi

. "$repo/tests/system/runlock.sh"
runlock_hold "$repo" kstack-proof "$repo/tests/host/kstack_mutate.sh" "$@"

[ "$#" -eq 0 ] || [ "$*" = --check ] || {
    echo 'kstack-proof: usage: kstack_mutate.sh [--check]' >&2
    exit 2
}
mkdir -p build/tests
work=$(mktemp -d "$repo/build/tests/kstack-proof.XXXXXX")
child=
cp rach2/sys/syscall_proc.c "$work/syscall_proc.c"
cp rach2/sys/arch/i686/cpu.h "$work/cpu.h"
cp rach2/sys/proc/process.h "$work/process.h"
cp "$work/syscall_proc.c" "$work/syscall_proc.saved"

# restore: copy the saved syscall_proc.c back over the copy under test.
restore()
{
    cp "$work/syscall_proc.saved" "$work/syscall_proc.c"
    touch "$work/syscall_proc.c"
}
# cleanup: the exit trap; stop the running test and restore the source.
cleanup()
{
    if [ -n "$child" ]; then
        kill -TERM "$child" 2>/dev/null || :
        wait "$child" 2>/dev/null || :
        child=
    fi
    restore
}
trap cleanup EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

# run_phase PHASE STATUS MESSAGE: extract the definitions and functions from
# the copies into $work/kstack.inc, compile fixtures/kstack.c over it as
# $work/PHASE, run it for at most 20 seconds, and exit unless it ends with
# STATUS and its log contains MESSAGE.
run_phase()
{
    phase=$1
    expected=$2
    message=$3
    binary="$work/$phase"
    python3 - "$work" <<'PY'
from pathlib import Path
import re
import sys

work = Path(sys.argv[1])


def define(text, name):
    found = re.findall(r'^#define ' + name + r'\s+\S+', text, re.M)
    if len(found) != 1:
        raise SystemExit('kstack-proof: the definition is not unique: ' + name)
    return found[0]


def function(text, name):
    matches = list(re.finditer(r'^[A-Za-z_][\w *]*\b' + name + r'\([^;]*?\n\{', text, re.M))
    if len(matches) != 1:
        raise SystemExit('kstack-proof: the function is not unique: ' + name)
    match = matches[0]
    depth = 0
    tokens = r'/\*.*?\*/|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|[{}]'
    for token in re.finditer(tokens, text[match.end() - 1:], re.S):
        if token.group() == '{':
            depth += 1
        elif token.group() == '}':
            depth -= 1
            if depth == 0:
                return text[match.start():match.end() - 1 + token.end()]
    raise SystemExit('kstack-proof: the function does not end: ' + name)


source = (work / 'syscall_proc.c').read_text()
cpu = (work / 'cpu.h').read_text()
process = (work / 'process.h').read_text()
parts = [define(process, 'KERNEL_STACK_SIZE'), define(cpu, 'PAGE_SIZE'),
         function(source, 'kstack_alloc'), function(source, 'kstack_free'),
         function(source, 'kstack_overrun_pid')]
(work / 'kstack.inc').write_text('\n\n'.join(parts) + '\n')
PY
    if ! "${CC:-cc}" -std=c99 -g -O1 -Wall -Wextra -Werror \
        -fsanitize=address,undefined -fno-sanitize-recover=all \
        -I "$work" tests/host/fixtures/kstack.c -o "$binary" >"$binary.compile.log" 2>&1; then
        cat "$binary.compile.log" >&2
        exit 1
    fi
    cksum "$work/syscall_proc.c" "$work/kstack.inc" "$binary" >"$binary.cksum"
    perl -e 'alarm 20; exec @ARGV or die "kstack-proof: exec: $!\n"' \
        "$binary" >"$binary.log" 2>&1 &
    child=$!
    status=0
    wait "$child" || status=$?
    child=
    cat "$binary.log"
    [ "$status" -eq "$expected" ] && grep -Fq "$message" "$binary.log" || exit 1
    echo "kstack-proof: result: $phase status $status"
}

echo "kstack-proof: logs: $work"
run_phase fixed 0 'kstack-proof: result: pass:'
[ "$#" -eq 0 ] || exit 0
for fault in $faults; do
    apply_fault "$work/syscall_proc.c" "$fault"
    touch "$work/syscall_proc.c"
    case $fault in
        none | top)
            message='kstack-proof: fail: every byte of a new kernel stack is zero'
            ;;
        unguarded)
            message='kstack-proof: fail: a kernel stack is four pages from the heap with a guard page below them'
            ;;
        guard-in-stack)
            message='kstack-proof: fail: an address in the guard page below the running thread'"'"'s stack gives its pid'
            ;;
        empty)
            message='kstack-proof: fail: a thread with no stack gives none'
            ;;
        boot-freed)
            message='kstack-proof: fail: the boot stack'"'"'s top gives nothing back'
            ;;
    esac
    run_phase "$fault-mutated" 1 "$message"
    restore
    run_phase "$fault-restored" 0 'kstack-proof: result: pass:'
    cmp "$work/syscall_proc.saved" "$work/syscall_proc.c"
done
echo 'kstack-proof: result: pass: the six faults failed their rows and the restored source passed'
