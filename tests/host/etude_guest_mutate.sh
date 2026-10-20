#!/bin/sh
# Prove etude_guest_hosttest.c with faults in private copies of etude-guest:
# a repeated command run again, a cancellation reported as a signal, an
# acknowledgement that releases nothing, and an exec that runs the program as
# a child. Each must fail the test, and the restored copy must pass it.
set -eu
repo=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
cd "$repo"

# Change the unique fault text in a private source, or just locate it.
apply_fault()
{
    python3 - "$1" "$2" <<'PY'
from pathlib import Path
import os
import sys
path = Path(sys.argv[1])
source = path.read_text()
faults = {
    'repeat': ('    if (read_file(path, buf, sizeof(buf)) < 0) {\n        char line0',
               '    if (1) {\n        char line0'),
    'stopped': ('    if (a->stopped)\n        snprintf(line[0]',
                '    if (0)\n        snprintf(line[0]'),
    'ack': ('    while (queue_head && queue_head->sequenced && queue_head->seq <= ack &&',
            '    while (0 && queue_head && queue_head->sequenced && queue_head->seq <= ack &&'),
    'exec': ('    execve(exec_next.prog, exec_next.argv, exec_next.envp);\n    _exit(127);',
             '    if (fork() == 0)\n        execve(exec_next.prog, exec_next.argv, exec_next.envp);\n'
             '    _exit(0);'),
}
old, new = faults[sys.argv[2]]
if source.count(old) != 1:
    raise SystemExit('etude-guest-mutation: fault text is not unique')
if not os.environ.get('RACH2_FAULT_TEXTS'):
    path.write_text(source.replace(old, new))
PY
}

if [ "${1:-}" = --texts ]; then
    RACH2_FAULT_TEXTS=1
    export RACH2_FAULT_TEXTS
    for fault in repeat stopped ack exec; do
        apply_fault tools/etude/guest/guest.c "$fault"
    done
    echo 'etude-guest-mutation: texts: every fault found once'
    exit 0
fi
. "$repo/tests/system/runlock.sh"
runlock_hold "$repo" etude-guest-mutation "$repo/tests/host/etude_guest_mutate.sh" "$@"
work=
child=

# Stop a test before removing its private sources and evidence.
cleanup()
{
    status=$?
    if [ -n "$child" ]; then
        kill -TERM "$child" 2>/dev/null || :
        wait "$child" 2>/dev/null || :
    fi
    if [ "$status" -eq 0 ] || [ -n "${stopped:-}" ]; then
        [ -z "$work" ] || rm -rf "$work"
    else
        echo "etude-guest-mutation: evidence: $work" >&2
    fi
}
trap cleanup EXIT
trap 'stopped=1; exit 129' HUP
trap 'stopped=1; exit 130' INT
trap 'stopped=1; exit 143' TERM
work=$(mktemp -d "${TMPDIR:-/tmp}/rach2-etude-guest-proof.XXXXXX")
cp tools/etude/guest/guest.c "$work/guest.c"

# Build the real host test against the private guest.c and run it under the
# sanitizers; its status must be `expected`.
run_phase()
{
    phase=$1
    expected=$2
    "${CC:-cc}" -std=c99 -D_DEFAULT_SOURCE -D_FORTIFY_SOURCE=0 -DRACH2_HOSTTEST \
        -g -O1 -fsanitize=address,undefined -fno-sanitize-recover=all \
        -I tools/etude/include -I tools/etude -I tools/etude/guest -I utils \
        tests/host/etude_guest_hosttest.c "$work/guest.c" tools/etude/guest/wire.c \
        tools/etude/history_json.c utils/sha256.c \
        -o "$work/$phase" >"$work/$phase.compile.log" 2>&1 || {
            cat "$work/$phase.compile.log" >&2; exit 1;
        }
    status=0
    perl -e 'alarm 120; exec @ARGV or die "etude-guest-mutation: exec: $!\n"' \
        "$work/$phase" >"$work/$phase.log" 2>&1 &
    child=$!
    wait "$child" || status=$?
    child=
    cat "$work/$phase.log"
    [ "$status" -eq "$expected" ] || exit 1
    echo "etude-guest-mutation: result: $phase status $status"
}

# fault NAME TEXT: the test fails with the fault and names the broken
# contract in its log.
fault()
{
    cp tools/etude/guest/guest.c "$work/guest.c"
    apply_fault "$work/guest.c" "$1"
    run_phase "$1-mutated" 1
    grep -q "$2" "$work/$1-mutated.log"
}
run_phase fixed 0
fault repeat 'a repeated run is answered by the same events'
fault stopped 'a cancelled attempt is stopped and cleaned'
fault ack 'an acknowledged frame is not sent again'
fault exec "the program's exit is the process's"
cp tools/etude/guest/guest.c "$work/guest.c"
run_phase restored 0
