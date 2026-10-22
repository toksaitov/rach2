#!/bin/sh
# Prove etude_legacy_hosttest.c with faults in a private copy of the
# preparation adapter: a failed step taken for a pass, a missing artifact
# passed over, a tree held by another process taken, a request of no goal
# and a goal outside the tree taken, the kernel left out of the record,
# make's flags passed to the steps, a changed digest not found and a stop
# signal ignored must each be found.
#
# Usage: sh tests/host/etude_legacy_mutate.sh [--texts]
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
    'exit': ('ended == ENDED_ITSELF && r->end == ETUDE_END_EXITED && r->value == 0 &&',
             'ended == ENDED_ITSELF && r->end == ETUDE_END_EXITED &&'),
    'missing': ('        } else if (errno != EISDIR) {\n',
                '        } else if (0) {\n'),
    'held': ('    return ok && (kill((pid_t)pid, 0) == 0 || errno == EPERM);\n',
             '    return ok || (kill((pid_t)pid, 0) == 0 || errno == EPERM);\n'),
    'nogoal': ('    if (o->ngoals == 0)\n', '    if (0)\n'),
    'goal': ('        if (!goal_name(o->goals[i]))\n', '        if (!goal_name(o->goals[i]) && 0)\n'),
    'kernel': ('    if (list_add(&p->required, p->kernel) != 0)\n', '    if (0)\n'),
    'flags': ('    "MAKEFLAGS=", "MFLAGS=",', '    "MFLAGS=",'),
    'digest': ('        changed = changed || strcmp(hex, e->digest) != 0;\n',
               '        changed = changed || strcmp(hex, hex) != 0;\n'),
    'stop': ('        if (ended == ENDED_ITSELF && stopped(p)) {\n',
             '        if (0 && stopped(p)) {\n'),
}
old, new = faults[sys.argv[2]]
if source.count(old) != 1:
    raise SystemExit('etude-legacy-mutation: fault text is not unique')
if not os.environ.get('RACH2_FAULT_TEXTS'):
    path.write_text(source.replace(old, new))
PY
}

if [ "${1:-}" = --texts ]; then
    RACH2_FAULT_TEXTS=1
    export RACH2_FAULT_TEXTS
    for fault in exit missing held nogoal goal kernel flags digest stop; do
        apply_fault tools/etude/posix/legacy.c "$fault"
    done
    echo 'etude-legacy-mutation: texts: all nine faults found once'
    exit 0
fi
. "$repo/tests/system/runlock.sh"
runlock_hold "$repo" etude-legacy-mutation "$repo/tests/host/etude_legacy_mutate.sh" "$@"
work=
child=

# Stop a test before removing its private source and evidence.
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
        echo "etude-legacy-mutation: evidence: $work" >&2
    fi
}
trap cleanup EXIT
trap 'stopped=1; exit 129' HUP
trap 'stopped=1; exit 130' INT
trap 'stopped=1; exit 143' TERM
work=$(mktemp -d "${TMPDIR:-/tmp}/rach2-etude-legacy-proof.XXXXXX")

# Copy the source the faults go into afresh.
restore()
{
    cp tools/etude/posix/legacy.c "$work/legacy.c"
}

# Build and run the real host test against the private source under the
# sanitizers, with its fixtures in the scratch, and require `expected` as its
# status.
run_phase()
{
    phase=$1
    expected=$2
    "${CC:-cc}" -std=c99 -D_DEFAULT_SOURCE -D_FORTIFY_SOURCE=0 \
        -Wall -Wextra -Werror -g -O1 -fsanitize=address,undefined \
        -fno-sanitize-recover=all -I tests/host -I utils -I tools/etude/include \
        -I tools/etude/posix tests/host/etude_legacy_hosttest.c "$work/legacy.c" \
        tools/etude/posix/store.c tools/etude/posix/prepare.c tools/etude/capture.c \
        tools/etude/posix/exec.c tools/etude/posix/keeper.c tools/etude/posix/process.c \
        utils/sha256.c -o "$work/$phase" >"$work/$phase.compile.log" 2>&1 || {
            cat "$work/$phase.compile.log" >&2; exit 1;
        }
    status=0
    TMPDIR=$work perl -e 'alarm 240; exec @ARGV or die "etude-legacy-mutation: exec: $!\n"' \
        "$work/$phase" >"$work/$phase.log" 2>&1 &
    child=$!
    wait "$child" || status=$?
    child=
    cat "$work/$phase.log"
    [ "$status" -eq "$expected" ] || exit 1
    echo "etude-legacy-mutation: result: $phase status $status"
}

# Plant `fault` in the copy, and require the test to fail with a failure of
# the row `row`.
prove()
{
    restore
    apply_fault "$work/legacy.c" "$1"
    run_phase "$1-mutated" 1
    grep -q "^etude-legacy-test: fail: .*: $2: " "$work/$1-mutated.log" || {
        echo "etude-legacy-mutation: $1: the row '$2' did not fail" >&2; exit 1;
    }
}

restore
run_phase fixed 0
prove exit 'failed: failed'
prove missing 'missing: failed'
prove held 'held'
prove nogoal 'none'
prove goal 'absolute'
prove kernel "alone: the record's files"
prove flags 'alone: prepared'
prove digest 'check: a changed content found by digest'
prove stop 'stop: stopped'
restore
run_phase restored 0
