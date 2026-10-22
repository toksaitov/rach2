#!/bin/sh
# Check that an init which ran under the debug shell waits, before it hands
# the machine back, for a process asleep in the call that ends it, which has
# no address space since the end gives it up, and waits for no kernel thread
# and no record another reaper has. `proc_end_others` is extracted unchanged
# from rach2/sys/syscall_proc.c and compiled under ASan and UBSan in
# tests/host/fixtures/endothers.c. The host suite runs the check.
#
# Usage: sh tests/host/endothers_check.sh
# The script builds from the tree in a scratch directory under build/tests,
# runs the fixture for at most 20 seconds and exits with its status.
set -eu
repo=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
cd "$repo"

# build DIR: extract proc_end_others into DIR/endothers.inc and compile
# the fixture over it as DIR/endothers.
build()
{
    python3 - rach2/sys/syscall_proc.c "$1/endothers.inc" <<'PY'
from pathlib import Path
import re
import sys


def function(text, name):
    matches = list(re.finditer(r'^[A-Za-z_][\w *]*\b' + name + r'\([^;]*?\n\{', text, re.M))
    if len(matches) != 1:
        raise SystemExit('endothers-check: the function is not unique: ' + name)
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
    raise SystemExit('endothers-check: the function does not end: ' + name)


source = Path(sys.argv[1]).read_text()
Path(sys.argv[2]).write_text(function(source, 'proc_end_others') + '\n')
PY
    "${CC:-cc}" -std=c99 -g -O1 -Wall -Wextra -Werror \
        -fsanitize=address,undefined -fno-sanitize-recover=all \
        -I "$1" tests/host/fixtures/endothers.c -o "$1/endothers"
}

[ "$#" -eq 0 ] || {
    echo 'endothers-check: usage: endothers_check.sh' >&2
    exit 2
}
mkdir -p build/tests
work=
trap '[ -z "$work" ] || rm -rf "$work"' EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM
work=$(mktemp -d "$repo/build/tests/endothers-check.XXXXXX")
build "$work"
perl -e 'alarm 20; exec @ARGV or die "endothers-check: exec: $!\n"' "$work/endothers"
