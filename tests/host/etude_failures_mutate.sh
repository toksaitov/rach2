#!/bin/sh
# Prove that history_failures keeps a failure that a passing retry or the run
# record would hide, refuses an incomplete or doubled record, and bounds its
# listing, with faults in a private copy of tools/etude/failures.c.
#
# Usage: sh tests/host/etude_failures_mutate.sh [--texts]
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
    'last': ('        if (rank > j->worst)\n            j->worst = rank;\n',
             '        j->worst = rank;\n'),
    'summary': ('    if (!j->attempts || j->worst)\n        return 0;\n', ''),
    'partial': ('    if (status == 1) {\n', '    if (0) {\n'),
    'contents': ('            status = -1;\n', ''),
    'bound': ('        if (out->lines + item.lines < FL_LINES) {\n', '        if (1) {\n'),
}
old, new = faults[sys.argv[2]]
if source.count(old) != 1:
    raise SystemExit('etude-failures-mutation: fault text is not unique: ' + sys.argv[2])
if not os.environ.get('RACH2_FAULT_TEXTS'):
    path.write_text(source.replace(old, new))
PY
}

if [ "${1:-}" = --texts ]; then
    RACH2_FAULT_TEXTS=1
    export RACH2_FAULT_TEXTS
    for fault in last summary partial contents bound; do
        apply_fault tools/etude/failures.c "$fault"
    done
    echo 'etude-failures-mutation: texts: all five faults found once'
    exit 0
fi
. "$repo/tests/system/runlock.sh"
runlock_hold "$repo" etude-failures-mutation "$repo/tests/host/etude_failures_mutate.sh" "$@"
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
        echo "etude-failures-mutation: evidence: $work" >&2
    fi
}
trap cleanup EXIT
trap 'stopped=1; exit 129' HUP
trap 'stopped=1; exit 130' INT
trap 'stopped=1; exit 143' TERM
work=$(mktemp -d "${TMPDIR:-/tmp}/rach2-etude-failures-proof.XXXXXX")
cp tools/etude/failures.c "$work/"

# Build and run the real host test against the private source under sanitizers.
run_phase()
{
    phase=$1
    expected=$2
    "${CC:-cc}" -std=c99 -D_DEFAULT_SOURCE -D_FORTIFY_SOURCE=0 \
        -Wall -Wextra -Werror -g -O1 -fsanitize=address,undefined \
        -fno-sanitize-recover=all -I tools/etude -I tools/etude/include \
        tests/host/etude_failures_hosttest.c "$work/failures.c" tools/etude/history.c \
        tools/etude/history_json.c tools/etude/history_collect.c utils/sha256.c \
        -o "$work/$phase" >"$work/$phase.compile.log" 2>&1 || {
            cat "$work/$phase.compile.log" >&2; exit 1;
        }
    status=0
    perl -e 'alarm 60; exec @ARGV or die "etude-failures-mutation: exec: $!\n"' \
        "$work/$phase" >"$work/$phase.log" 2>&1 &
    child=$!
    wait "$child" || status=$?
    child=
    [ "$status" -eq "$expected" ] || { cat "$work/$phase.log"; exit 1; }
    echo "etude-failures-mutation: result: $phase status $status"
}

# Put one fault in the private source, require its check to fail, restore it.
prove()
{
    fault=$1
    message=$2
    apply_fault "$work/failures.c" "$fault"
    run_phase "$fault-mutated" 1
    grep -qF "failed: $message" "$work/$fault-mutated.log" || {
        cat "$work/$fault-mutated.log"; exit 1;
    }
    cp tools/etude/failures.c "$work/failures.c"
}
run_phase fixed 0
prove last 'a failure that a passing retry follows is listed as possibly flaky'
prove summary 'the failure stays listed when the run record calls the item passed'
prove partial 'a stream whose last record is incomplete is refused'
prove contents 'a record with two contents is refused'
prove bound 'the listing stops at 80 lines'
run_phase restored 0
