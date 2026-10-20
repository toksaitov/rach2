#!/bin/sh
# Check that fork's kernel stack comes back clear, with a guard page below it
# that an overrun is known by, and that the boot stack, which is no heap's, is
# never given back. `kstack_alloc`, `kstack_free` and `kstack_overrun_pid` are
# extracted unchanged from rach2/sys/syscall_proc.c, with KERNEL_STACK_SIZE
# from rach2/sys/proc/process.h and PAGE_SIZE from rach2/sys/arch/i686/cpu.h,
# and compiled under ASan and UBSan in tests/host/fixtures/kstack.c, whose
# heap hands out pages full of code bytes. The host suite runs the check, and
# tests/proofs/kstack.json builds its faults with it.
#
# Usage: sh tests/host/kstack_check.sh [SRC DIR]
# With no operand it builds from the tree in a scratch directory, runs the
# fixture for at most 20 seconds and exits with its status. With SRC and DIR
# it reads SRC/rach2/sys/syscall_proc.c, writes DIR/kstack.inc and the fixture
# DIR/kstack, and runs nothing.
set -eu
repo=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
cd "$repo"

# build SRC DIR: extract the definitions and functions into DIR/kstack.inc
# and compile the fixture over it as DIR/kstack.
build()
{
    python3 - "$1/rach2/sys/syscall_proc.c" "$2/kstack.inc" <<'PY'
from pathlib import Path
import re
import sys


def define(text, name):
    found = re.findall(r'^#define ' + name + r'\s+\S+', text, re.M)
    if len(found) != 1:
        raise SystemExit('kstack-check: the definition is not unique: ' + name)
    return found[0]


def function(text, name):
    matches = list(re.finditer(r'^[A-Za-z_][\w *]*\b' + name + r'\([^;]*?\n\{', text, re.M))
    if len(matches) != 1:
        raise SystemExit('kstack-check: the function is not unique: ' + name)
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
    raise SystemExit('kstack-check: the function does not end: ' + name)


source = Path(sys.argv[1]).read_text()
cpu = Path('rach2/sys/arch/i686/cpu.h').read_text()
process = Path('rach2/sys/proc/process.h').read_text()
parts = [define(process, 'KERNEL_STACK_SIZE'), define(cpu, 'PAGE_SIZE'),
         function(source, 'kstack_alloc'), function(source, 'kstack_free'),
         function(source, 'kstack_overrun_pid')]
Path(sys.argv[2]).write_text('\n\n'.join(parts) + '\n')
PY
    "${CC:-cc}" -std=c99 -g -O1 -Wall -Wextra -Werror \
        -fsanitize=address,undefined -fno-sanitize-recover=all \
        -I "$2" tests/host/fixtures/kstack.c -o "$2/kstack"
}

case $# in
    2)
        build "$1" "$2"
        exit 0
        ;;
    0) ;;
    *)
        echo 'kstack-check: usage: kstack_check.sh [SRC DIR]' >&2
        exit 2
        ;;
esac
mkdir -p build/tests
work=
trap '[ -z "$work" ] || rm -rf "$work"' EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM
work=$(mktemp -d "$repo/build/tests/kstack-check.XXXXXX")
build "$repo" "$work"
perl -e 'alarm 20; exec @ARGV or die "kstack-check: exec: $!\n"' "$work/kstack"
