#!/bin/sh
# etude_lines_mutate.sh: prove etude_lines_hosttest.c with faults put into
# private copies of the compatible formatter, the status writer and the reader.
#
# Usage: sh tests/host/etude_lines_mutate.sh [--texts]
#
# It builds and runs the host test on the unmodified sources, then with each
# fault below in a private copy of its source, requiring the test to fail
# with the check named for the fault, then on the restored copies. It changes
# no source in the working tree. The faults of the status writer and the
# reader are caught only where the reader runs, on Linux, so the proof runs
# there. --texts checks that each fault's text is once in its source, and
# builds nothing.
set -eu
repo=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
cd "$repo"

# fault MODE ROOT [NAME]: with MODE names, list the faults; with texts, check
# that each fault's text is once in its source under ROOT; with apply, put
# fault NAME into its source under ROOT and print the check that must fail.
fault() {
    python3 - "$@" <<'PY'
import sys
from pathlib import Path

faults = {
    'disrupted': ('tools/etude/lines.c',
                  '    if (outcome == R_FAILURE || (outcome == R_INCOMPLETE && !disrupted))\n',
                  '    if (outcome == R_FAILURE || outcome == R_INCOMPLETE)\n',
                  "a disrupted attempt is the runner's, not its suite's"),
    'unnamed': ('tools/etude/lines.c',
                '        snprintf(names + used, size - used, "%srunner", used ? " " : "");\n',
                '        ;\n',
                "a disrupted attempt is the runner's, not its suite's"),
    'retry': ('tools/etude/lines.c',
              '    if (!it->attempts++ || outcome > it->worst) {\n',
              '    if (it->attempts++ || 1) {\n',
              'a retry that passes keeps the failure'),
    'stalled': ('tools/etude/lines.c',
                '    if (worst == R_FAILURE || l->nextra)\n',
                '    if (worst >= R_INCOMPLETE || l->nextra)\n',
                "a stalled case is its suite's, and leaves bisection unresolved"),
    'unfinished': ('tools/etude/lines.c',
                   '    if (runs != 1) {\n',
                   '    if (runs != 1) { if (!runs) return;\n',
                   "a run without its run record is the runner's fault"),
    'cut': ('tools/etude/lines.c',
            '        if (rc == 1)\n',
            '        if (rc == 2)\n',
            "a stream cut short is the runner's fault"),
    'orphan': ('tools/etude/lines.c',
               '        fault(l, "%s is in no suite of the map", label(it));\n',
               '        (void)l;\n',
               "a case in no suite of the map is the runner's fault"),
    'runner': ('tools/etude/lines.c',
               '    if (l->nfaults && runner_suite(l, si))\n',
               '    if (0)\n',
               "the runner's own suite is a suite of the map"),
    'named': ('tools/etude/lines.c',
              '    if (worst == R_FAILURE || l->nextra)\n',
              '    if (worst == R_FAILURE)\n',
              'a failure named outside the suites fails a run whose suites passed'),
    'prepared': ('tools/etude/lines.c',
                 '    if (worst == R_PASS && strcmp(outcome, "pass"))\n',
                 '    if (0)\n',
                 "a run that failed after its cases passed is the runner's fault"),
    'expired': ('tools/etude/posix/status.c',
                '    a->expired = expired != NULL;\n',
                '    a->expired = 0;\n',
                'an attempt whose watchdog ran out is stalled and being ended'),
    'deadline': ('tools/flow/test_status.py',
                 '    elif now <= int(deadline):\n',
                 '    elif True:\n',
                 'an attempt past its deadline is stalled'),
    'protected': ('tools/flow/test_status.py',
                  '    elif claim.get("state") == "blocked":\n',
                  '    elif False:\n',
                  'the protected request is named'),
    'live': ('tools/flow/test_status.py',
             '    if "alive" in states:\n',
             '    if False:\n',
             'a claim whose owner ended while its process runs is live'),
}
mode, root = sys.argv[1], Path(sys.argv[2])
if mode == 'names':
    print('\n'.join(faults))
    sys.exit(0)
for name, (source, old, new, check) in faults.items():
    if mode == 'apply' and name != sys.argv[3]:
        continue
    text = (root / source).read_text()
    if text.count(old) != 1:
        sys.exit(f'etude-lines-mutation: {name}: its text is {text.count(old)} times in {source}')
    if mode == 'apply':
        (root / source).write_text(text.replace(old, new))
        print(check)
PY
}

if [ "${1:-}" = --texts ]; then
    fault texts "$repo"
    echo "etude-lines-mutation: texts: every fault's text is once in its source"
    exit 0
fi

work=
child=
stopped=

# Stop a running test before removing the private sources, which a failure
# keeps as its evidence.
cleanup() {
    status=$?
    if [ -n "$child" ]; then
        kill -TERM "$child" 2>/dev/null || :
        wait "$child" 2>/dev/null || :
    fi
    if [ "$status" -eq 0 ] || [ -n "$stopped" ]; then
        [ -z "$work" ] || rm -rf "$work"
    else
        echo "etude-lines-mutation: evidence: $work" >&2
    fi
}
trap cleanup EXIT
trap 'stopped=1; exit 129' HUP
trap 'stopped=1; exit 130' INT
trap 'stopped=1; exit 143' TERM
work=$(mktemp -d "${TMPDIR:-/tmp}/rach2-etude-lines-proof.XXXXXX")

# fresh: copy the faulted sources to $work/src.
fresh() {
    rm -rf "$work/src"
    mkdir -p "$work/src/tools/etude/posix" "$work/src/tools/flow"
    cp tools/etude/lines.c "$work/src/tools/etude/"
    cp tools/etude/posix/status.c "$work/src/tools/etude/posix/"
    cp tools/flow/test_status.py "$work/src/tools/flow/"
}

# run NAME [WARN]: build the host test against the copies in $work/src, with
# warnings as errors unless WARN is given, and run it with the copy of the
# reader, leaving its output in $work/NAME.log; the status is the test's, or
# 2 when it does not build.
run() {
    s=$work/src/tools
    strict=-Werror
    [ $# -lt 2 ] || strict=
    # shellcheck disable=SC2086
    "${CC:-cc}" -std=c99 -D_DEFAULT_SOURCE -D_FORTIFY_SOURCE=0 -DRACH2_HOSTTEST \
        -Wall -Wextra $strict -g -O1 -fsanitize=address,undefined \
        -fno-sanitize-recover=all -I tools/etude/include -I tools/etude -I utils \
        -I tests/host/compat tests/host/etude_lines_hosttest.c "$s/etude/lines.c" \
        tools/etude/history.c tools/etude/history_json.c tools/etude/history_collect.c \
        utils/sha256.c "$s/etude/posix/status.c" tools/etude/watchdog.c tools/etude/ledger.c \
        tools/etude/posix/process.c -o "$work/$1" >"$work/$1.compile.log" 2>&1 || {
        cat "$work/$1.compile.log" >&2
        return 2
    }
    TMPDIR=$work ETUDE_LINES_READER=$s/flow/test_status.py \
        perl -e 'alarm 120; exec @ARGV or die "etude-lines-mutation: exec: $!\n"' \
        "$work/$1" >"$work/$1.log" 2>&1 &
    child=$!
    st=0
    wait "$child" || st=$?
    child=
    return "$st"
}

fresh
if ! run unmodified; then
    cat "$work/unmodified.log" >&2
    echo "etude-lines-mutation: result: fail: the unmodified sources fail" >&2
    exit 1
fi
echo "etude-lines-mutation: the unmodified sources pass"
bad=0
fault names "$repo" >"$work/faults"
while IFS= read -r name; do
    fresh
    check=$(fault apply "$work/src" "$name")
    st=0
    run "$name" warn || st=$?
    if [ "$st" -ne 1 ]; then
        echo "etude-lines-mutation: $name: the test gave status $st with the fault in, not 1" >&2
        bad=1
    elif ! grep -qxF "etude_lines_hosttest: failed: $check" "$work/$name.log"; then
        echo "etude-lines-mutation: $name: the check \"$check\" did not fail" >&2
        cat "$work/$name.log" >&2
        bad=1
    else
        echo "etude-lines-mutation: $name: caught by \"$check\""
    fi
done <"$work/faults"
fresh
if ! run restored; then
    cat "$work/restored.log" >&2
    echo "etude-lines-mutation: result: fail: the restored sources fail" >&2
    exit 1
fi
if [ "$bad" -ne 0 ]; then
    echo "etude-lines-mutation: result: fail: a fault was not caught" >&2
    exit 1
fi
echo "etude-lines-mutation: result: pass: every fault caught, and the restored sources pass"
