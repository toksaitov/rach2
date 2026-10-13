#!/bin/sh
# Prove the rows of tests/host/klog_hosttest.c that require a record raised
# inside a line of the debug shell to follow the line, with two faults in a
# private copy of klog.c, each of which must fail the checks named for it and
# no other:
#
#   puts-open    writes a `console_puts` line outside `console_write_begin`
#                and `console_write_end`, holding only the screen's cursor.
#   printf-open  does the same to a `console_printf` call.
#
# Usage: klog_mutate.sh [--texts]
# With --texts it checks that each fault's text is once in the tree's source,
# and builds and runs nothing.
set -eu
repo=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
cd "$repo"
texts=
[ "${1:-}" != --texts ] || texts=1
if [ -z "$texts" ]; then
    . "$repo/tests/system/runlock.sh"
    runlock_hold "$repo" klog-proof "$repo/tests/host/klog_mutate.sh" "$@"
fi
[ "$#" -eq 0 ] || [ "$*" = --texts ] || {
    echo 'klog-proof: usage: klog_mutate.sh [--texts]' >&2
    exit 2
}
child=
if [ -n "$texts" ]; then
    work=
    source=rach2/sys/klog.c
else
    mkdir -p build/tests
    work=$(mktemp -d "$repo/build/tests/klog-proof.XXXXXX")
    # The test includes klog.c by a path relative to itself.
    mkdir -p "$work/rach2" "$work/tests/host"
    cp -R rach2/sys "$work/rach2/"
    cp tests/host/klog_hosttest.c "$work/tests/host/"
    source="$work/rach2/sys/klog.c"
    cp "$source" "$work/klog.c.saved"
fi

restore()
{
    cp "$work/klog.c.saved" "$source"
    touch "$source"
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

test=tests/host/klog_hosttest.c
deps=$(grep -oE 'host-deps:[^*]*' "$test" | sed 's/host-deps://' | tr '\n' ' ')

# run_phase NAME: compile the test against the private tree and run it;
# `status` is its exit status and NAME.log what it printed.
run_phase()
{
    phase=$1
    binary="$work/$phase"
    # shellcheck disable=SC2086 # the dependency list is one path a word
    if ! (cd "$work" && "${CC:-cc}" -std=c99 -D_DEFAULT_SOURCE -D_FORTIFY_SOURCE=0 \
        -DRACH2_HOSTTEST -fsanitize=address,undefined -fno-sanitize-recover=all -g -O1 \
        -Wno-empty-body -ffreestanding -I rach2/sys -I rach2/sys/arch/x86 -I rach2/sys/arch/i686 \
        -I rach2/sys/lib -I rach2/sys/proc -o "$binary" "$test" $deps) \
        >"$binary.compile.log" 2>&1; then
        cat "$binary.compile.log" >&2
        exit 1
    fi
    cksum "$source" "$binary" >"$binary.cksum"
    perl -e 'alarm 60; exec @ARGV or die "klog-proof: exec: $!\n"' \
        "$binary" >"$binary.log" 2>&1 &
    child=$!
    status=0
    wait "$child" || status=$?
    child=
    cat "$binary.log"
    echo "klog-proof: result: $phase status $status"
}

# mutate OLD NEW: replace the unique OLD in the private klog.c by NEW.
mutate()
{
    python3 - "$source" "$1" "$2" <<'PY'
from pathlib import Path
import os
import sys

path = Path(sys.argv[1])
source = path.read_text()
if source.count(sys.argv[2]) != 1:
    raise SystemExit('klog-proof: the pattern is not unique: ' + sys.argv[2])
if not os.environ.get('RACH2_FAULT_TEXTS'):
    path.write_text(source.replace(sys.argv[2], sys.argv[3]))
PY
    [ -n "$texts" ] || touch "$source"
}

# fault NAME CHECK...: run the mutated tree and require it to fail exactly
# the checks named, in the order the test runs them.
fault()
{
    name=$1
    shift
    run_phase "$name"
    : >"$work/$name.want"
    for check in "$@"; do
        printf '  FAIL: %s\n' "$check" >>"$work/$name.want"
    done
    if [ "$status" -eq 0 ] || [ "$status" -ge 128 ] || ! cmp -s "$work/$name.want" "$work/$name.log"; then
        echo "klog-proof: $name: the failures are not the ones expected:" >&2
        cat "$work/$name.want" >&2
        exit 1
    fi
    restore
}

if [ -n "$texts" ]; then
    trap - EXIT
    run_phase() { :; }
    restore() { :; }
    fault() { :; }
    RACH2_FAULT_TEXTS=1
    export RACH2_FAULT_TEXTS
else
    echo "klog-proof: logs: $work"
    run_phase fixed
    [ "$status" -eq 0 ] && grep -q '^klog_hosttest: PASS$' "$work/fixed.log" || exit 1
fi

mutate "    console_write_begin();
    while (*str != '\\0') {
        console_putc(*str++);
    }
    console_putc('\\n');
    console_write_end();" "    textcon_hold();
    while (*str != '\\0') {
        console_putc(*str++);
    }
    console_putc('\\n');
    textcon_release();"
fault puts-open 'a record raised inside a line of the shell follows the line on the port' \
    'and on the screen'

mutate '    console_write_begin();
    k_vprintf(console_putc, format, ap);
    console_write_end();' '    textcon_hold();
    k_vprintf(console_putc, format, ap);
    textcon_release();'
fault printf-open 'a record raised inside a formatted line of the shell follows it'

if [ -n "$texts" ]; then
    echo 'klog-proof: texts: every fault is once in its source'
    exit 0
fi
run_phase restored
[ "$status" -eq 0 ] && grep -q '^klog_hosttest: PASS$' "$work/restored.log" || exit 1
cmp "$work/klog.c.saved" "$source"
echo 'klog-proof: result: pass: each fault failed its checks alone and the restored source passed'
