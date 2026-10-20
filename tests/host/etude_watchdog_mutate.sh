#!/bin/sh
# Prove etude_watchdog_hosttest.c with faults in private copies of the
# watchdog and its supervisor: a heartbeat taken for progress, quiet work
# never announced, a busy loop never stopped, a capture that waits on a hung
# QMP forever, and a lost transport taken for a confirmed cleanup must each
# be found.
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
    'heartbeat': ('        w->p.repeats++;\n',
                  '        w->p.repeats++;\n        w->p.last_us = now_us;\n'),
    'quiet': ('if (wb->kind == ETUDE_WITNESS_QUIET && ws->announced_us == 0)',
              'if (0 && ws->announced_us == 0)'),
    'busy': ('        a->r.asked = 1;\n        etude_exec_cancel(a->proc);\n',
             '        a->r.asked = 1;\n'),
    'hung': ('    a->until = now + capture_bound(a);\n',
             '    a->until = UINT64_MAX - capture_bound(a);\n'),
    'lost': ('        a->r.end = ETUDE_END_LOST;\n        a->r.cleanup = ETUDE_CLEANUP_UNCONFIRMED;\n',
             '        a->r.end = ETUDE_END_LOST;\n        a->r.cleanup = ETUDE_CLEANUP_CONFIRMED;\n'),
}
old, new = faults[sys.argv[2]]
if source.count(old) != 1:
    raise SystemExit('etude-watchdog-mutation: fault text is not unique')
if not os.environ.get('RACH2_FAULT_TEXTS'):
    path.write_text(source.replace(old, new))
PY
}

if [ "${1:-}" = --texts ]; then
    RACH2_FAULT_TEXTS=1
    export RACH2_FAULT_TEXTS
    for fault in heartbeat quiet; do
        apply_fault tools/etude/watchdog.c "$fault"
    done
    for fault in busy hung lost; do
        apply_fault tools/etude/posix/supervise.c "$fault"
    done
    echo 'etude-watchdog-mutation: texts: all five faults found once'
    exit 0
fi
. "$repo/tests/system/runlock.sh"
runlock_hold "$repo" etude-watchdog-mutation "$repo/tests/host/etude_watchdog_mutate.sh" "$@"
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
        echo "etude-watchdog-mutation: evidence: $work" >&2
    fi
}
trap cleanup EXIT
trap 'stopped=1; exit 129' HUP
trap 'stopped=1; exit 130' INT
trap 'stopped=1; exit 143' TERM
work=$(mktemp -d "${TMPDIR:-/tmp}/rach2-etude-watchdog-proof.XXXXXX")

# Copy the sources the faults go into afresh.
restore()
{
    cp tools/etude/watchdog.c "$work/watchdog.c"
    cp tools/etude/posix/supervise.c "$work/supervise.c"
}

# Build and run the real host test against the private sources under the
# sanitizers, with its fixtures in the scratch, and require `expected` as its
# status.
run_phase()
{
    phase=$1
    expected=$2
    "${CC:-cc}" -std=c99 -D_DEFAULT_SOURCE -D_FORTIFY_SOURCE=0 \
        -Wall -Wextra -Werror -g -O1 -fsanitize=address,undefined \
        -fno-sanitize-recover=all -I tests/host -I tools/etude/include \
        -I tools/etude/posix tests/host/etude_watchdog_hosttest.c "$work/watchdog.c" \
        "$work/supervise.c" tools/etude/capture.c tools/etude/history_json.c \
        tools/etude/qemu_profile.c tools/etude/posix/exec.c tools/etude/posix/keeper.c \
        tools/etude/posix/process.c tools/etude/posix/qemu.c tools/etude/processor.c \
        -o "$work/$phase" >"$work/$phase.compile.log" 2>&1 || {
            cat "$work/$phase.compile.log" >&2; exit 1;
        }
    status=0
    TMPDIR=$work perl -e 'alarm 240; exec @ARGV or die "etude-watchdog-mutation: exec: $!\n"' \
        "$work/$phase" >"$work/$phase.log" 2>&1 &
    child=$!
    wait "$child" || status=$?
    child=
    cat "$work/$phase.log"
    [ "$status" -eq "$expected" ] || exit 1
    echo "etude-watchdog-mutation: result: $phase status $status"
}

# Plant `fault` in `file` of the copy, require the test to fail with a line
# that has `text`, and restore the copy.
prove()
{
    restore
    apply_fault "$work/$1" "$2"
    run_phase "$2-mutated" 1
    grep -q "$3" "$work/$2-mutated.log"
}

restore
run_phase fixed 0
prove watchdog.c heartbeat 'heartbeat: ended stopped after expiry case, not stopped after gap'
prove watchdog.c quiet 'quiet: ended stopped after expiry gap, not exited after none'
prove supervise.c busy 'busy: did not finish within'
prove supervise.c hung 'hung: did not finish within'
prove supervise.c lost 'lost: the claims stay'
restore
run_phase restored 0
