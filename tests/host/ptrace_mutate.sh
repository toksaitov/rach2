#!/bin/sh
# Prove native ptrace tests against faults in private source copies.
# Usage: ptrace_mutate.sh [ptrace|debugreg [--check]]
# With no argument, run both tests and their faults.
# --check compiles and runs the unmodified test alone. Every fault must
# fail its named row; a restored, touched and rebuilt source must pass.
set -eu
repo=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
cd "$repo"
if [ "$#" -eq 0 ]; then
    sh "$repo/tests/host/ptrace_mutate.sh" ptrace
    sh "$repo/tests/host/ptrace_mutate.sh" debugreg
    exit 0
fi
kind=${1:-}
case "$kind" in
    ptrace) source=proc/ptrace.c; faults='regset listen return-stops return-kill options seize step' ;;
    debugreg) source=arch/x86/debugreg.c; faults='switch stale migration delivery kernel-step pending pending-saved pending-stale pending-step dequeue dequeue-repeat' ;;
    *) echo 'ptrace-proof: usage: ptrace_mutate.sh [ptrace|debugreg [--check]]' >&2; exit 2 ;;
esac
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
    message=$(python3 - "$file" "$fault" <<'PY'
from pathlib import Path
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
    'dequeue-repeat': ('    current_proc->debug_pending = 0;',
                       '    current_proc->debug_pending |= 0;',
                       'dequeued hit is not posted again at user return'),
}
old, new, message = changes[fault]
s = path.read_text()
if s.count(old) != 1:
    sys.exit('ptrace-proof: fault text must occur once: ' + fault)
path.write_text(s.replace(old, new))
print(message)
PY
    )
    run_phase "$fault" 1 "$message"
    cp "$work/source.saved" "$file"
    touch "$file"
    run_phase "$fault-restored" 0 "$kind-hosttest: result: pass"
done
