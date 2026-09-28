#!/bin/sh
# Prove that an init which ran under the debug shell waits, before it hands
# the machine back, for a process asleep in the call that ends it, which has
# no address space since the end gives it up, and waits for no kernel thread
# and no record another reaper has. `proc_end_others` is extracted unchanged
# from a private copy of rach2/sys/syscall_proc.c and compiled under ASan and
# UBSan in tests/host/fixtures/endothers.c.
#
# Usage: endothers_mutate.sh [--check]
# With --check it compiles and runs the source as it is, which is what the
# host suite does; without, it also compiles each fault in, requires its row
# to fail, and requires the restored source to pass again. The faults are
# every record without an address space passed by, as a kernel thread is;
# the record another reaper has waited for; and the kernel thread waited for.
set -eu
repo=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
cd "$repo"
[ "$#" -eq 0 ] || [ "$*" = --check ] || {
    echo 'endothers-proof: usage: endothers_mutate.sh [--check]' >&2
    exit 2
}
mkdir -p build/tests
work=$(mktemp -d "$repo/build/tests/endothers-proof.XXXXXX")
child=
cp rach2/sys/syscall_proc.c "$work/syscall_proc.c"
cp "$work/syscall_proc.c" "$work/syscall_proc.saved"

restore()
{
    cp "$work/syscall_proc.saved" "$work/syscall_proc.c"
    touch "$work/syscall_proc.c"
}
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


def function(text, name):
    matches = list(re.finditer(r'^[A-Za-z_][\w *]*\b' + name + r'\([^;]*?\n\{', text, re.M))
    if len(matches) != 1:
        raise SystemExit('endothers-proof: the function is not unique: ' + name)
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
    raise SystemExit('endothers-proof: the function does not end: ' + name)


source = (work / 'syscall_proc.c').read_text()
(work / 'endothers.inc').write_text(function(source, 'proc_end_others') + '\n')
PY
    if ! "${CC:-cc}" -std=c99 -g -O1 -Wall -Wextra -Werror \
        -fsanitize=address,undefined -fno-sanitize-recover=all \
        -I "$work" tests/host/fixtures/endothers.c -o "$binary" >"$binary.compile.log" 2>&1; then
        cat "$binary.compile.log" >&2
        exit 1
    fi
    cksum "$work/syscall_proc.c" "$work/endothers.inc" "$binary" >"$binary.cksum"
    perl -e 'alarm 20; exec @ARGV or die "endothers-proof: exec: $!\n"' \
        "$binary" >"$binary.log" 2>&1 &
    child=$!
    status=0
    wait "$child" || status=$?
    child=
    cat "$binary.log"
    [ "$status" -eq "$expected" ] && grep -Fq "$message" "$binary.log" || exit 1
    echo "endothers-proof: result: $phase status $status"
}

echo "endothers-proof: logs: $work"
run_phase fixed 0 'endothers-proof: result: pass:'
[ "$#" -eq 0 ] || exit 0
for fault in ending-passed unused-waited kthread-waited; do
    python3 - "$work/syscall_proc.c" "$fault" <<'PY'
from pathlib import Path
import sys

path = Path(sys.argv[1])
text = path.read_text()
count = '                alive += p->in_syscall != 0 && p->state != PROC_UNUSED;\n'
changes = {
    'ending-passed': '',
    'unused-waited': '                alive += p->in_syscall != 0;\n',
    'kthread-waited': '                alive += p->state != PROC_UNUSED;\n',
}
start = text.index('static void proc_end_others(void)\n{')
end = text.index('\n}\n', start)
body = text[start:end]
if body.count(count) != 1:
    raise SystemExit('endothers-proof: the fault is not unique in its function: ' + sys.argv[2])
path.write_text(text[:start] + body.replace(count, changes[sys.argv[2]]) + text[end:])
PY
    touch "$work/syscall_proc.c"
    case $fault in
        ending-passed)
            message='endothers-proof: fail: a process in the call that ends it is waited for until it is a zombie and reaped'
            ;;
        unused-waited | kthread-waited)
            message='endothers-proof: fail: the walk waits for a record that never ends'
            ;;
    esac
    run_phase "$fault-mutated" 1 "$message"
    restore
    run_phase "$fault-restored" 0 'endothers-proof: result: pass:'
    cmp "$work/syscall_proc.saved" "$work/syscall_proc.c"
done
echo 'endothers-proof: result: pass: the three faults failed their rows and the restored source passed'
