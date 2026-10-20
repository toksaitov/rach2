#!/bin/sh
# Prove etude_artifact_hosttest.c with faults in private copies of the
# artifact store and the preparation graph: a recipe's failed exit taken for
# success, a dependent of a failed node run, a live writer's partial file
# swept, a damaged object reused, the key's lock never contended and a
# digest other than the one expected accepted must each be found.
#
# Usage: sh tests/host/etude_artifact_mutate.sh [--texts]
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
    'exit': ('    if (r->end == ETUDE_END_EXITED && r->value != 0)\n',
             '    if (0 && r->value != 0)\n'),
    'blocked': ('            *bad = 1;\n', '            *bad = 0;\n'),
    'sweep': ('if (lock_try(fd, F_RDLCK) == 0 && unlink(path) == 0)',
              'if (unlink(path) == 0)'),
    'verify': ('    if (etude_store_check(s, digest, &found) != 0)\n        return -1;\n',
               '    found = *size;\n'),
    'lock': ('    if (lock_try(fd, F_WRLCK) != 0) {\n        e = errno;\n',
             '    if (0) {\n        e = errno;\n'),
    'expect': ('    if (expect != NULL && strcmp(expect, digest) != 0) {\n',
               '    if (0) {\n'),
}
old, new = faults[sys.argv[2]]
if source.count(old) != 1:
    raise SystemExit('etude-artifact-mutation: fault text is not unique')
if not os.environ.get('RACH2_FAULT_TEXTS'):
    path.write_text(source.replace(old, new))
PY
}

if [ "${1:-}" = --texts ]; then
    RACH2_FAULT_TEXTS=1
    export RACH2_FAULT_TEXTS
    for fault in exit blocked; do
        apply_fault tools/etude/posix/prepare.c "$fault"
    done
    for fault in sweep verify lock expect; do
        apply_fault tools/etude/posix/store.c "$fault"
    done
    echo 'etude-artifact-mutation: texts: all six faults found once'
    exit 0
fi
. "$repo/tests/system/runlock.sh"
runlock_hold "$repo" etude-artifact-mutation "$repo/tests/host/etude_artifact_mutate.sh" "$@"
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
        echo "etude-artifact-mutation: evidence: $work" >&2
    fi
}
trap cleanup EXIT
trap 'stopped=1; exit 129' HUP
trap 'stopped=1; exit 130' INT
trap 'stopped=1; exit 143' TERM
work=$(mktemp -d "${TMPDIR:-/tmp}/rach2-etude-artifact-proof.XXXXXX")

# Copy the sources the faults go into afresh.
restore()
{
    cp tools/etude/posix/store.c "$work/store.c"
    cp tools/etude/posix/prepare.c "$work/prepare.c"
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
        -fno-sanitize-recover=all -I tests/host -I utils -I tools/etude/include \
        -I tools/etude/posix tests/host/etude_artifact_hosttest.c "$work/store.c" \
        "$work/prepare.c" tools/etude/capture.c tools/etude/posix/exec.c \
        tools/etude/posix/keeper.c tools/etude/posix/process.c utils/sha256.c \
        -o "$work/$phase" >"$work/$phase.compile.log" 2>&1 || {
            cat "$work/$phase.compile.log" >&2; exit 1;
        }
    status=0
    TMPDIR=$work perl -e 'alarm 240; exec @ARGV or die "etude-artifact-mutation: exec: $!\n"' \
        "$work/$phase" >"$work/$phase.log" 2>&1 &
    child=$!
    wait "$child" || status=$?
    child=
    cat "$work/$phase.log"
    [ "$status" -eq "$expected" ] || exit 1
    echo "etude-artifact-mutation: result: $phase status $status"
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
prove prepare.c exit 'failures: bad-exit'
prove prepare.c blocked 'failures: blocked'
prove store.c sweep 'interrupt: the writer that runs keeps its file'
prove store.c verify 'damage: object: made again'
prove store.c lock 'race: the recipe ran once'
prove store.c expect 'put: expected'
restore
run_phase restored 0
