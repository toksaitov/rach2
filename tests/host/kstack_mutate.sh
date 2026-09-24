#!/bin/sh
# Prove that fork's kernel stack comes back clear, entry 26 of milestone 50.
# `kstack_alloc` and `kstack_free` are extracted unchanged from a private
# copy of rach2/sys/syscall_proc.c, with KERNEL_STACK_SIZE from the same
# file and PAGE_SIZE from rach2/sys/mm/paging.h, and compiled under ASan and
# UBSan in tests/host/fixtures/kstack.c, whose heap hands out pages full of
# code bytes.
#
# Usage: kstack_mutate.sh [--check]
# With --check it compiles and runs the source as it is, which is what the
# host suite does; without, it also compiles each fault in, requires the
# row to fail, and requires the restored source to pass again. The two
# faults are the clearing removed, and the clearing of the top page alone,
# the part fork writes anyway.
set -eu
repo=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
cd "$repo"
[ "$#" -eq 0 ] || [ "$*" = --check ] || {
    echo 'kstack-proof: usage: kstack_mutate.sh [--check]' >&2
    exit 2
}
mkdir -p build/tests
work=$(mktemp -d "$repo/build/tests/kstack-proof.XXXXXX")
child=
cp rach2/sys/syscall_proc.c "$work/syscall_proc.c"
cp rach2/sys/mm/paging.h "$work/paging.h"
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
paging = (work / 'paging.h').read_text()
parts = [define(source, 'KERNEL_STACK_SIZE'), define(paging, 'PAGE_SIZE'),
         function(source, 'kstack_alloc'), function(source, 'kstack_free')]
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
for fault in none top; do
    python3 - "$work/syscall_proc.c" "$fault" <<'PY'
from pathlib import Path
import sys

path = Path(sys.argv[1])
text = path.read_text()
clear = '        k_memset(base, 0, KERNEL_STACK_SIZE);\n'
changes = {
    'none': (clear, ''),
    'top': (clear, '        k_memset((char *)base + KERNEL_STACK_SIZE - PAGE_SIZE, 0, PAGE_SIZE);\n'),
}
old, new = changes[sys.argv[2]]
start = text.index('static uintptr_t kstack_alloc(void)\n{')
end = text.index('\n}\n', start)
body = text[start:end]
if body.count(old) != 1:
    raise SystemExit('kstack-proof: the clearing is not unique in kstack_alloc')
path.write_text(text[:start] + body.replace(old, new) + text[end:])
PY
    touch "$work/syscall_proc.c"
    run_phase "$fault-mutated" 1 'kstack-proof: fail: every byte of a new kernel stack is zero'
    restore
    run_phase "$fault-restored" 0 'kstack-proof: result: pass:'
    cmp "$work/syscall_proc.saved" "$work/syscall_proc.c"
done
echo 'kstack-proof: result: pass: both faults failed the row and the restored source passed'
