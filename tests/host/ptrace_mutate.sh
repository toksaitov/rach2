#!/bin/sh
# Prove native ptrace tests against faults in private source copies.
# Usage: ptrace_mutate.sh [ptrace|debugreg [--check]] | --texts
# With no argument, run both tests and their faults. --check compiles and
# runs the unmodified test alone; --texts checks that every fault's text is
# once in the tree's source and builds nothing. Every fault must fail its
# named row; a restored, touched and rebuilt source must pass.
set -eu
repo=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
cd "$repo"

# apply_fault FILE FAULT: put FAULT into FILE and print its row's message;
# with RACH2_FAULT_TEXTS set, only check that its text is once in FILE.
apply_fault()
{
    python3 - "$1" "$2" <<'PY'
from pathlib import Path
import os
import sys

path, fault = Path(sys.argv[1]), sys.argv[2]
changes = {
    'regset': ('    if (!set) {\n        t = ptrace_tracee(pid);\n',
               '    if (!set) {\n', 'regset revalidates after sleeping copy'),
    'listen': ('        } else if (t->ptrace & PT_LISTEN) {\n            ptrace_restop(t);\n',
               '', 'interrupt releases listening tracee'),
    'return-stops': ('    while ((self->ptrace & PT_TRACED) && (self->ptrace & (PT_START | PT_INTERRUPT)) &&',
                     '    if ((self->ptrace & PT_TRACED) && (self->ptrace & (PT_START | PT_INTERRUPT)) &&',
                     'listening interrupts stop before user return'),
    'return-kill': ('           !kill_pending()) {',
                    '           1) {', 'kill ends return stop sequence'),
    'options': ('        if (data & ~(unsigned long)PTRACE_O_MASK) {',
                '        if (data & ~PTRACE_O_MASK) {', 'setoptions refuses upper bits'),
    'seize': ('static int ptrace_attach(process_t *t, int seize, unsigned long opts)',
              'static int ptrace_attach(process_t *t, int seize, uint32_t opts)',
              'seize refuses upper bits without attaching'),
    'step': ('    int stepping = leaving && (self->ptrace & PT_STEP);',
             '    int stepping = 0;', 'syscall single step reports TRAP_TRACE'),
    'switch': ('    } else if (dr7_live[cpu]) {',
               '    } else if (old && (old->debugreg[7] & DR7_ENABLES)) {',
               'detach then switch clears hardware enables'),
    'stale': ('    if ((dr6 & DR6_HITS) && current_proc && !(current_proc->debugreg[7] & DR7_ENABLES)) {',
              '    if (0) {', 'stale hit sends no trap'),
    'migration': ('        old->debug_pending |= deferred[cpu];',
                  '        old->debug_pending |= 0;', 'deferred trap follows thread'),
    'kernel-step': ('    if (info.code == TRAP_HWBKPT && (current_proc->debugreg[7] & DR7_ENABLES)) {',
                    '    if ((info.code == TRAP_HWBKPT || info.code == TRAP_TRACE) && (current_proc->debugreg[7] & DR7_ENABLES)) {',
                    'kernel single step does not duplicate syscall exit stop'),
    'delivery': ('        signal_post_thread(current_proc, SIGTRAP, &info);',
                 '        (void)info;', 'deferred trap follows thread'),
    'pending': ('    dr6 = deferred[cpu] | current_proc->debug_pending;',
                '    dr6 = 0;', 'deferred hit is posted before a wait'),
    'pending-saved': ('    dr6 = deferred[cpu] | current_proc->debug_pending;',
                      '    dr6 = deferred[cpu];', 'saved hit is posted after migration'),
    'pending-stale': ('    if (info.code == TRAP_HWBKPT && (current_proc->debugreg[7] & DR7_ENABLES)) {',
                      '    if (dr6 & DR6_HITS) {', 'detached hit posts no signal'),
    'pending-step': ('    if (info.code == TRAP_HWBKPT && (current_proc->debugreg[7] & DR7_ENABLES)) {',
                     '    if ((info.code == TRAP_HWBKPT || info.code == TRAP_TRACE) && (current_proc->debugreg[7] & DR7_ENABLES)) {',
                     'kernel step posts no signal'),
    'dequeue': ('    debugreg_deliver();', '    return;',
                'deferred hit is posted for dequeue with siginfo'),
    'user-force': ('            signal_force(current_proc, SIGTRAP, &info);',
                   '            signal_post_thread(current_proc, SIGTRAP, &info);',
                   'delayed user hit is deliverable at syscall return'),
    'user-entry': ('    if (pc != (uintptr_t)&DEBUG_SYSCALL_ENTRY) {',
                   '    if (pc < (uintptr_t)&DEBUG_SYSCALL_ENTRY - 1 || pc > (uintptr_t)&DEBUG_SYSCALL_ENTRY + 1) {',
                   'adjacent gate addresses leave status-less exception unchanged'),
    'user-origin': ('        deferred[cpu] |= DEFER_USER;', '        deferred[cpu] |= 0;',
                    'exact gate entry forces blocked trap'),
    'user-status': ('    info.code = debugreg_code(dr6 & ~DEFER_USER);', '    info.code = debugreg_code(dr6);',
                  'delayed user hit keeps status and syscall frame'),
    'user-no-status-origin': ('    if (current_proc && (current_proc->debugreg[7] & DR7_ENABLES)) {',
                              '    if (deferred[cpu] & DR6_HITS) {',
                              'status-less user hit is deliverable at syscall return'),
    'user-no-status-code': ('    if (dr6 & DEFER_USER) {\n        info.code = TRAP_HWBKPT;\n    }',
                            '', 'status-less user hit is deliverable at syscall return'),
    'user-no-status-dr6': ('    info.code = debugreg_code(dr6 & ~DEFER_USER);',
                           '    info.code = debugreg_code((dr6 & ~DEFER_USER) | ((dr6 & DEFER_USER) ? 1ul : 0ul));',
                           'status-less user hit keeps status and syscall frame'),
    'user-no-status-capture': ('    if (current_proc && (current_proc->debugreg[7] & DR7_ENABLES)) {',
                               '    if (current_proc) {',
                               'stale status-less entry records no user hit'),
    'user-no-status-stale': ('    if (info.code == TRAP_HWBKPT && (current_proc->debugreg[7] & DR7_ENABLES)) {',
                             '    if (info.code == TRAP_HWBKPT) {',
                             'detach cancels status-less user hit without changing mask'),
    'user-no-status-repeat-cpu': ('    dr6 = deferred[cpu] | current_proc->debug_pending;\n    deferred[cpu] = 0;',
                                  '    dr6 = deferred[cpu] | current_proc->debug_pending;\n    deferred[cpu] &= DEFER_USER;',
                                  'status-less user hit checked during wait is forced only once'),
    'user-no-status-repeat-saved': ('    current_proc->debug_pending = 0;',
                                    '    current_proc->debug_pending &= DEFER_USER;',
                                    'status-less user hit is forced only once'),
    'user-no-status-wait': ('    return ((deferred[arch_cpu_id()] | current_proc->debug_pending) & DEFER_USER) != 0;',
                            '    return ((deferred[arch_cpu_id()] | current_proc->debug_pending) & DEFER_USER) != 0 &&\n        ((deferred[arch_cpu_id()] | current_proc->debug_pending) & DR6_HITS) != 0;',
                            'status-less user hit interrupts wait regardless of mask and disposition'),
    'user-wait-post': ('    if (!arch_debug_pending()) {', '    if (1) {',
                       'pending check keeps delayed user hit out of signal waits'),
    'user-wait-cpu': ('    return ((deferred[arch_cpu_id()] | current_proc->debug_pending) & DEFER_USER) != 0;',
                      '    return (current_proc->debug_pending & DEFER_USER) != 0;',
                      'delayed user hit interrupts wait regardless of mask and disposition'),
    'user-wait-saved': ('    return ((deferred[arch_cpu_id()] | current_proc->debug_pending) & DEFER_USER) != 0;',
                        '    return (deferred[arch_cpu_id()] & DEFER_USER) != 0;',
                        'delayed user hit interrupts wait regardless of mask and disposition'),
    'user-wait-stale': ('    if (!current_proc || !(current_proc->debugreg[7] & DR7_ENABLES)) {',
                        '    if (!current_proc) {', 'detached user hit does not interrupt wait'),
    'user-wait-kernel': ('    return ((deferred[arch_cpu_id()] | current_proc->debug_pending) & DEFER_USER) != 0;',
                         '    return (deferred[arch_cpu_id()] | current_proc->debug_pending) != 0;',
                         'kernel hit does not force a wait interruption'),
    'user-wait-no-thread': ('    if (!current_proc || !(current_proc->debugreg[7] & DR7_ENABLES)) {\n        return 0;',
                            '    if (!current_proc || !(current_proc->debugreg[7] & DR7_ENABLES)) {\n        return 1;',
                            'no current thread means no forced wait interruption'),
    'dequeue-repeat': ('    current_proc->debug_pending = 0;',
                       '    current_proc->debug_pending |= 0;',
                       'dequeued hit is not posted again at user return'),
}
old, new, message = changes[fault]
s = path.read_text()
if s.count(old) != 1:
    sys.exit('ptrace-proof: fault text must occur once: ' + fault)
if os.environ.get('RACH2_FAULT_TEXTS'):
    sys.exit(0)
path.write_text(s.replace(old, new))
print(message)
PY
}

# select_kind KIND: the source and the faults of KIND.
select_kind()
{
    case "$1" in
        ptrace) source=proc/ptrace.c; faults='regset listen return-stops return-kill options seize step' ;;
        debugreg) source=arch/x86/debugreg.c; faults='switch stale migration delivery kernel-step pending pending-saved pending-stale pending-step dequeue dequeue-repeat user-force user-entry user-origin user-status user-wait-post user-wait-cpu user-wait-saved user-wait-stale user-wait-kernel user-wait-no-thread user-no-status-origin user-no-status-code user-no-status-dr6 user-no-status-capture user-no-status-stale user-no-status-repeat-cpu user-no-status-repeat-saved user-no-status-wait' ;;
        *) echo 'ptrace-proof: usage: ptrace_mutate.sh [ptrace|debugreg [--check]]' >&2; exit 2 ;;
    esac
}

if [ "${1:-}" = --texts ]; then
    for kind in ptrace debugreg; do
        select_kind "$kind"
        for fault in $faults; do
            RACH2_FAULT_TEXTS=1 apply_fault "rach2/sys/$source" "$fault" >/dev/null || exit 1
        done
    done
    echo 'ptrace-proof: texts: every fault is once in its source'
    exit 0
fi

. "$repo/tests/system/runlock.sh"
runlock_hold "$repo" ptrace-proof "$repo/tests/host/ptrace_mutate.sh" "$@"
if [ "$#" -eq 0 ]; then
    # A group signal reaches the child too; these run once it has restored.
    trap 'exit 129' HUP
    trap 'exit 130' INT
    trap 'exit 143' TERM
    sh "$repo/tests/host/ptrace_mutate.sh" ptrace
    sh "$repo/tests/host/ptrace_mutate.sh" debugreg
    exit 0
fi
kind=${1:-}
select_kind "$kind"
shift
[ "$#" -eq 0 ] || [ "$*" = --check ] || exit 2
mkdir -p build/tests
work=$(mktemp -d "$repo/build/tests/$kind-proof.XXXXXX")
mkdir "$work/rach2"
cp -R rach2/sys "$work/rach2/"
file="$work/rach2/sys/$source"
cp "$file" "$work/source.saved"
child=
extra=
[ "$kind" != debugreg ] || extra=-DRACH2_HOST_CPU

cleanup()
{
    if [ -n "$child" ]; then
        kill -TERM "$child" 2>/dev/null || :
        wait "$child" 2>/dev/null || :
        child=
    fi
    cp "$work/source.saved" "$file"
    touch "$file"
}
trap cleanup EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

run_phase()
{
    phase=$1
    expected=$2
    message=$3
    binary="$work/$phase"
    # shellcheck disable=SC2086
    if ! "${CC:-cc}" -std=c99 -DRACH2_HOSTTEST $extra \
        -fsanitize=address,undefined -fno-sanitize-recover=all -g -O1 -ffreestanding \
        -I "$work/rach2/sys" -I "$work/rach2/sys/lib" -I "$work/rach2/sys/proc" \
        -o "$binary" "tests/host/${kind}_hosttest.c" \
        "$work/rach2/sys/lib/kstring.c" "$work/rach2/sys/lib/kctypes.c" \
        >"$binary.compile.log" 2>&1; then
        cat "$binary.compile.log" >&2
        exit 1
    fi
    cksum "$file" "$binary" >"$binary.cksum"
    perl -e 'alarm 10; exec @ARGV or die "ptrace-proof: exec: $!\n"' \
        "$binary" >"$binary.log" 2>&1 &
    child=$!
    status=0
    wait "$child" || status=$?
    child=
    cat "$binary.log"
    if [ "$status" -ne "$expected" ] || ! grep -Fq "$message" "$binary.log"; then
        echo "ptrace-proof: fail: $phase status $status" >&2
        exit 1
    fi
    echo "ptrace-proof: result: $kind $phase status $status"
}

printf 'ptrace-proof: logs: %s\n' "$work"
run_phase fixed 0 "$kind-hosttest: result: pass"
[ "$#" -eq 0 ] || exit 0
for fault in $faults; do
    message=$(apply_fault "$file" "$fault")
    run_phase "$fault" 1 "$message"
    cp "$work/source.saved" "$file"
    touch "$file"
    run_phase "$fault-restored" 0 "$kind-hosttest: result: pass"
done
