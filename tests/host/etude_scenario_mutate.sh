#!/bin/sh
# Prove etude_scenario_hosttest.c with faults in private copies of
# libetude-case, the QEMU launcher and the executor's keeper: a private copy
# made again for each boot, a closed console waited on as if it could still
# show the text, an exit without QEMU's event taken for a shutdown, a step's
# bound never kept, a stop signal ignored, a copy of an artifact that is no
# disk, a lost etude-guest session never noticed, a damaged artifact taken, a
# service taken as ready before its text, a service that died unseen
# accepted, no diagnostics before the stop, a stopped service's output not
# kept, a fixed port in place of the plan's, a second machine refused while
# the first runs, a service's output that wakes no wait, a keeper that does
# not end its case when its controller is killed, a machine's memory
# profile left out of the launcher's profile, a boot of a case whose
# machines together need more memory than it reserves, a boundary
# machine that takes any QMP command, and a refused text at a boundary
# console taken for a lost machine must each be found.
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
    'memory': ('    p->memory_profile = jtext(qp, \"memory_profile\");\n',
               '    p->memory_profile = NULL;\n'),
    'share': ('    if (own > reserved || others > reserved - own) {\n',
              '    if ((own > reserved || others > reserved - own) && 0) {\n'),
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
    'ready': ('    if (st == ETUDE_OK && t.at >= 0) {\n        *sp = s;\n',
              '    if (st == ETUDE_OK && (t.at >= 0 || 1)) {\n        *sp = s;\n'),
    'unseen': ('        if (s->finished && !s->seen && !stopped_by_signal(s)) {\n',
               '        if (s->finished && !s->seen && !stopped_by_signal(s) && 0) {\n'),
    'capture': ('    if (failing)\n        capture(c, ', '    if (failing && 0)\n        capture(c, '),
    'evidence': ('            c->unseen = 1;\n        }\n        keep_service(s);\n',
                 '            c->unseen = 1;\n        }\n'),
    'port': ('    return c->ports[i];\n', '    return 7777 + i;\n'),
    'single': ('    if (d->now != NULL && mres(d->now)->state != ETUDE_MACHINE_ENDED) {\n',
               '    if (c->machines != NULL && mres(c->machines)->state != ETUDE_MACHINE_ENDED) {\n'),
    'wake': ('    r->output = r->p == NULL && !own && !r->ready && (soonest == 0 || posix_now_us() < soonest);\n',
             '    r->output = r->p == NULL && !own && !r->ready && 0;\n'),
    'controller': ('        kp->controller = 0;\n        m.type = KEEP_CANCEL;\n',
                   '        kp->controller = 0;\n        m.type = KEEP_GO;\n'),
    'qmp': ('    if (m->boundary && !boundary_command(execute, arguments)) {\n',
            '    if (m->boundary && !boundary_command(execute, arguments) && 0) {\n'),
    'typed': ('        if (errno != EPERM)\n            return ETUDE_LOST;\n',
              '        if (errno != EPERM || 1)\n            return ETUDE_LOST;\n'),
}
old, new = faults[sys.argv[2]]
if source.count(old) != 1:
    raise SystemExit('etude-scenario-mutation: fault text is not unique')
if not os.environ.get('RACH2_FAULT_TEXTS'):
    path.write_text(source.replace(old, new))
PY
}

# Give the source, under tools/etude, that `fault` goes into.
fault_file()
{
    case $1 in
    wake|qmp) echo posix/qemu.c ;;
    controller) echo posix/keeper.c ;;
    *) echo posix/scenario.c ;;
    esac
}

faults='state gone event bound stop disk session damaged ready unseen capture evidence port single wake controller memory share qmp typed'
if [ "${1:-}" = --texts ]; then
    RACH2_FAULT_TEXTS=1
    export RACH2_FAULT_TEXTS
    for fault in $faults; do
        apply_fault "tools/etude/$(fault_file "$fault")" "$fault"
    done
    echo 'etude-scenario-mutation: texts: all twenty faults found once'
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
cp tools/etude/serial.h tools/etude/history_json.h tools/etude/capture.h "$work/"

# Copy the sources the faults go into afresh.
restore()
{
    cp tools/etude/posix/scenario.c tools/etude/posix/qemu.c tools/etude/posix/keeper.c \
        "$work/posix/"
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
        "$work/posix/qemu.c" tools/etude/qemu_profile.c tools/etude/posix/exec.c \
        "$work/posix/keeper.c" tools/etude/processor.c \
        tools/etude/posix/process.c tools/etude/posix/store.c tools/etude/capture.c \
        tools/etude/history_json.c tools/etude/guest/guest.c tools/etude/guest/fixture.c \
        tools/etude/guest/wire.c utils/sha256.c -o "$work/$phase" >"$work/$phase.compile.log" 2>&1 || {
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
    apply_fault "$work/$(fault_file "$1")" "$1"
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
prove ready 'unready: no .assert early-failed pass.'
prove unseen 'unseen: o->status == r->want_status'
prove capture 'stop-pair: evidence beta.1.capture lacks'
prove evidence 'stop-pair: evidence peer.1.out lacks'
prove port 'no .assert service-ready pass.'
prove single 'pair: no .assert beta-reached pass.'
prove wake 'pair: the attempt did not end within'
prove controller 'kill-pair: [0-9]* of its processes are left'
prove memory 'memory: no .assert refused pass.'
prove share 'memory-pair: no .assert refused pass.'
prove qmp 'memory-qmp: no .assert qmp-refused pass.'
prove typed 'memory-qmp: no .assert type-refused pass.'
restore
run_phase restored 0
