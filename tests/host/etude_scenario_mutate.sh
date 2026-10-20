#!/bin/sh
# Prove etude_scenario_hosttest.c with faults in a private copy of
# libetude-case: a private copy made again for each boot, a closed console
# waited on as if it could still show the text, an exit without QEMU's event
# taken for a shutdown, a step's bound never kept, a stop signal ignored, a
# copy of an artifact that is no disk, a lost etude-guest session never
# noticed and a damaged artifact taken must each be found.
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
    'state': ('    if (a->copy != NULL)\n        return a->copy;\n',
              '    if (a->copy != NULL)\n        free(a->copy);\n    a->copy = NULL;\n'
              '    if (a->copy != NULL)\n        return a->copy;\n'),
    'gone': ('        r->state == ETUDE_MACHINE_ENDED || r->console.closed || r->console.total > r->console.kept;\n',
             '        r->console.total > r->console.kept;\n'),
    'event': ('    end->end = r->end;\n    end->value = -1;\n    end->console_offset = r->end_console;\n',
              '    end->end = r->end == ETUDE_END_QEMU_EXITED ? ETUDE_END_SHUTDOWN : r->end;\n'
              '    end->value = -1;\n    end->console_offset = r->end_console;\n'),
    'bound': ('        if (now >= until)\n            return ETUDE_BOUND;\n',
              '        if (now >= until && until != c->deadline)\n            return ETUDE_BOUND;\n'),
    'stop': ('    stop_asked = 1;\n', '    stop_asked = 0;\n'),
    'disk': ('    if (a == NULL || !disk_fixture(c, name)) {\n',
             '    if (a == NULL || !(disk_fixture(c, name) || 1)) {\n'),
    'session': ('        if (r->console.closed || r->state == ETUDE_MACHINE_ENDED || r->console.total > m->fed) {\n',
                '        if (r->console.total > m->fed) {\n'),
    'damaged': ('        if (etude_store_check(c->store, a->digest, NULL) != 0) {\n',
                '        if (etude_store_check(c->store, a->digest, NULL) != 0 && 0) {\n'),
}
old, new = faults[sys.argv[2]]
if source.count(old) != 1:
    raise SystemExit('etude-scenario-mutation: fault text is not unique')
if not os.environ.get('RACH2_FAULT_TEXTS'):
    path.write_text(source.replace(old, new))
PY
}

faults='state gone event bound stop disk session damaged'
if [ "${1:-}" = --texts ]; then
    RACH2_FAULT_TEXTS=1
    export RACH2_FAULT_TEXTS
    for fault in $faults; do
        apply_fault tools/etude/posix/scenario.c "$fault"
    done
    echo 'etude-scenario-mutation: texts: all eight faults found once'
    exit 0
fi
. "$repo/tests/system/runlock.sh"
runlock_hold "$repo" etude-scenario-mutation "$repo/tests/host/etude_scenario_mutate.sh" "$@"
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
        echo "etude-scenario-mutation: evidence: $work" >&2
    fi
}
trap cleanup EXIT
trap 'stopped=1; exit 129' HUP
trap 'stopped=1; exit 130' INT
trap 'stopped=1; exit 143' TERM
work=$(mktemp -d "${TMPDIR:-/tmp}/rach2-etude-scenario-proof.XXXXXX")
mkdir "$work/posix"
cp tools/etude/serial.h tools/etude/history_json.h "$work/"

# Copy the source the faults go into afresh.
restore()
{
    cp tools/etude/posix/scenario.c "$work/posix/scenario.c"
}

# Build and run the real host test against the private source under the
# sanitizers, with its fixtures in the scratch, and require `expected` as its
# status. The rows of the real QEMU run in the unmodified and restored runs
# alone, since every fault is found by a stand-in's row.
run_phase()
{
    phase=$1
    expected=$2
    "${CC:-cc}" -std=c99 -D_DEFAULT_SOURCE -D_FORTIFY_SOURCE=0 -DRACH2_HOSTTEST \
        -Wall -Wextra -Werror -g -O1 -fsanitize=address,undefined \
        -fno-sanitize-recover=all -I tests/host -I tools/etude/include -I tools/etude \
        -I tools/etude/posix -I tools/etude/guest -I utils tests/host/etude_scenario_hosttest.c \
        "$work/posix/scenario.c" tools/etude/posix/case_main.c tools/etude/serial.c \
        tools/etude/posix/qemu.c tools/etude/qemu_profile.c tools/etude/posix/exec.c \
        tools/etude/posix/keeper.c tools/etude/processor.c \
        tools/etude/posix/process.c tools/etude/posix/store.c tools/etude/capture.c \
        tools/etude/history_json.c tools/etude/guest/guest.c tools/etude/guest/wire.c \
        utils/sha256.c -o "$work/$phase" >"$work/$phase.compile.log" 2>&1 || {
            cat "$work/$phase.compile.log" >&2; exit 1;
        }
    status=0
    TMPDIR=$work ETUDE_TEST_NO_REAL=${3:-} perl -e 'alarm 280; exec @ARGV or die "etude-scenario-mutation: exec: $!\n"' \
        "$work/$phase" >"$work/$phase.log" 2>&1 &
    child=$!
    wait "$child" || status=$?
    child=
    cat "$work/$phase.log"
    [ "$status" -eq "$expected" ] || exit 1
    echo "etude-scenario-mutation: result: $phase status $status"
}

# Plant `fault` in the copy, require the test to fail with a line that has
# `text`, and restore the copy.
prove()
{
    restore
    apply_fault "$work/posix/scenario.c" "$1"
    run_phase "$1-mutated" 1 1
    grep -q "$2" "$work/$1-mutated.log"
}

restore
run_phase fixed 0
prove state 'reboot: no .assert readback pass.'
prove gone 'lost: no .assert lost-wait pass.'
prove event 'lost: no .assert not-shutdown pass.'
prove bound 'bound: the attempt did not end within'
prove stop 'stop: the attempt did not end within'
prove disk 'misuse: no .assert no-copy pass.'
prove session 'guest: no .assert guest-lost pass.'
prove damaged 'damaged: no .assert refused pass.'
restore
run_phase restored 0
