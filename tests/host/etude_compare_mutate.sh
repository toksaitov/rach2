#!/bin/sh
# Prove the comparison's noise floor, minimum history, disruption rule and
# accepted baseline with faults in private copies of the sources.
#
# Usage: sh tests/host/etude_compare_mutate.sh [--texts]
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
    'floor': ('    percent = (s->median4 + 399) / 400;\n',
              '    percent = 0;\n'),
    'minimum': ('    if (r->baseline_count < CMP_MINIMUM) {\n',
                '    if (r->baseline_count < 3) {\n'),
    'disrupted': ('    if (disruptions && disruptions->child)\n',
                  '    if (0 && disruptions)\n'),
    'accepted': ('    if (!match || options->new_baseline || !use_accepted(h, match, reference, r, room)) {\n',
                 '    if (!match || !options->new_baseline || !use_accepted(h, match, reference, r, room)) {\n'),
}
old, new = faults[sys.argv[2]]
if source.count(old) != 1:
    raise SystemExit('etude-compare-mutation: fault text is not unique: ' + sys.argv[2])
if not os.environ.get('RACH2_FAULT_TEXTS'):
    path.write_text(source.replace(old, new))
PY
}

if [ "${1:-}" = --texts ]; then
    RACH2_FAULT_TEXTS=1
    export RACH2_FAULT_TEXTS
    apply_fault tools/etude/compare.c floor
    apply_fault tools/etude/baseline.c minimum
    apply_fault tools/etude/compare.c disrupted
    apply_fault tools/etude/baseline.c accepted
    echo 'etude-compare-mutation: texts: all four faults found once'
    exit 0
fi
. "$repo/tests/system/runlock.sh"
runlock_hold "$repo" etude-compare-mutation "$repo/tests/host/etude_compare_mutate.sh" "$@"
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
        echo "etude-compare-mutation: evidence: $work" >&2
    fi
}
trap cleanup EXIT
trap 'stopped=1; exit 129' HUP
trap 'stopped=1; exit 130' INT
trap 'stopped=1; exit 143' TERM
work=$(mktemp -d "${TMPDIR:-/tmp}/rach2-etude-compare-proof.XXXXXX")
cp tools/etude/compare.c tools/etude/baseline.c "$work/"

# Build and run the real host test against private sources under sanitizers.
run_phase()
{
    phase=$1
    expected=$2
    "${CC:-cc}" -std=c99 -D_DEFAULT_SOURCE -D_FORTIFY_SOURCE=0 \
        -Wall -Wextra -Werror -g -O1 -fsanitize=address,undefined \
        -fno-sanitize-recover=all -I tools/etude -I tools/etude/include \
        tests/host/etude_compare_hosttest.c "$work/compare.c" "$work/baseline.c" \
        tools/etude/compare_report.c tools/etude/compare_run.c tools/etude/history.c \
        tools/etude/history_json.c tools/etude/history_collect.c utils/sha256.c \
        -o "$work/$phase" >"$work/$phase.compile.log" 2>&1 || {
            cat "$work/$phase.compile.log" >&2; exit 1;
        }
    status=0
    perl -e 'alarm 60; exec @ARGV or die "etude-compare-mutation: exec: $!\n"' \
        "$work/$phase" >"$work/$phase.log" 2>&1 &
    child=$!
    wait "$child" || status=$?
    child=
    [ "$status" -eq "$expected" ] || { cat "$work/$phase.log"; exit 1; }
    echo "etude-compare-mutation: result: $phase status $status"
}

# Put one fault in a private source, require its check to fail, and restore it.
prove()
{
    fault=$1
    file=$2
    message=$3
    apply_fault "$work/$file" "$fault"
    run_phase "$fault-mutated" 1
    grep -qF "failed: $message" "$work/$fault-mutated.log" || {
        cat "$work/$fault-mutated.log"; exit 1;
    }
    cp "tools/etude/$file" "$work/$file"
}
run_phase fixed 0
prove floor compare.c '101 s against [99,100,100,100,101] is noise'
prove minimum baseline.c 'three equal samples are too few'
prove disrupted compare.c 'a disrupted run is left out'
prove accepted baseline.c 'five slow runs do not move the accepted baseline'
run_phase restored 0
