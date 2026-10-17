#!/bin/sh
# agents_mutate.sh | --texts: show that `tests/run agents` catches a fault in
# each of the agents layer's installs of gh, Claude Code and muse, and one
# in the project's loops.
#
# Usage: sh tests/system/agents_mutate.sh | --texts
# Three faults are in recipes of vendor/gnu/agents/packages/. Those of gh
# and muse point the program's link in /usr/bin, which the recipe makes, at
# a file the package does not have, so that the path finds no such program.
# That of claude gives Claude Code's managed settings DISABLE_AUTOUPDATER in
# place of DISABLE_UPDATES, which stops only the background update, so that
# `claude update` tries to fetch a release. The fourth makes `limited` in
# tools/flow/lib.sh take a usage limit from any line of a session's output,
# which the limited_test row, running tests/system/limited_test.sh on the
# machine, must catch. The suite runs only the rows of these four, through
# RACH2_AGENTS_ROWS: it must pass, fail on each of them with the four faults
# in their sources and the root built again, and pass again with the sources
# restored. The suite is x86-64 only. --texts checks that each fault is once
# in its source.
set -eu

repo=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
cd "$repo"
faults="gh claude muse limited"
rows="gh claude muse limited_test"

# fault_file NAME: the file that fault NAME goes into: tools/flow/lib.sh for
# `limited`, and otherwise the recipe of the package NAME.
fault_file()
{
    if [ "$1" = limited ]; then
        echo tools/flow/lib.sh
    else
        echo "vendor/gnu/agents/packages/$1.sh"
    fi
}

# apply_fault NAME: put fault NAME into its source: for a package, whose
# program is NAME too, claude's in its managed settings and the others' in
# the program's link; with RACH2_FAULT_TEXTS set, only check its text and
# write nothing.
apply_fault()
{
    python3 - "$(fault_file "$1")" "$1" <<'PY'
from pathlib import Path
import sys
import os
import pathlib
if os.environ.get('RACH2_FAULT_TEXTS'):
    pathlib.Path.write_text = lambda self, *args, **kwargs: 0
path = Path(sys.argv[1])
name = sys.argv[2]
text = path.read_text()
if name == 'claude':
    old, new = '"DISABLE_UPDATES": "1"', '"DISABLE_AUTOUPDATER": "1"'
elif name == 'limited':
    old = '''        grep -E '"type":"result".*"is_error":true|"is_error":true.*"type":"result"|"type":"(error|turn\\.failed)"'\n'''
    new = '        cat\n'
else:
    old = f'ln -s ../../opt/{name}/bin/{name} "$1/usr/bin/{name}"'
    new = f'ln -s ../../opt/{name}/bin/{name}.missing "$1/usr/bin/{name}"'
if text.count(old) != 1:
    raise SystemExit(f'agents-mutation: the fault of {name} is not once in {path}')
path.write_text(text.replace(old, new))
PY
}

if [ "${1:-}" = --texts ]; then
    for name in $faults; do
        RACH2_FAULT_TEXTS=1 apply_fault "$name" >/dev/null
    done
    echo 'agents-mutation: texts: every fault is once in its source'
    exit 0
fi

. "$repo/tests/system/runlock.sh"
runlock_hold "$repo" agents-mutation "$repo/tests/system/agents_mutate.sh" "$@"

mkdir -p build
work=
logged=
child=
changed=

# Stop the running suite's process group and wait until it is gone.
stop_child()
{
    if [ -n "$child" ]; then
        "$work/runutil" killgroup "$child" 15 2>/dev/null || :
        wait "$child" 2>/dev/null || :
        while kill -0 "-$child" 2>/dev/null; do
            sleep 1
        done
        child=
    fi
}

# Put back the saved sources and touch them, so that make stages the packages
# again.
restore()
{
    if [ -n "$changed" ]; then
        for name in $faults; do
            cp "$work/$name.saved" "$(fault_file "$name")"
            touch "$(fault_file "$name")"
        done
        changed=
    fi
}

# Stop the suite and restore the sources. Then remove the scratch directory. A
# failure after its path was printed keeps it as evidence; a signal does not.
cleanup()
{
    status=$?
    [ -n "$work" ] || return 0
    stop_child
    restore
    case $status in
        0|129|130|143) ;;
        *) [ -z "$logged" ] || return 0 ;;
    esac
    rm -rf "$work"
}
trap cleanup EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM
work=$(mktemp -d "$repo/build/agents-proof.XXXXXX")
for name in $faults; do
    cp "$(fault_file "$name")" "$work/$name.saved"
done
cc -O2 -Wall -Wextra -Werror tests/system/runner/runutil.c -o "$work/runutil"

# run_phase PHASE EXPECTED...: run the suite's rows of the faults, which
# builds the root again when a recipe changed. With EXPECTED `pass` it must
# pass; otherwise it must fail with each EXPECTED in its output.
run_phase()
{
    phase=$1
    shift
    expected=$1
    ARCH=x86_64 RACH2_AGENTS_ROWS=$rows "$work/runutil" group tests/run agents \
        >"$work/$phase.log" 2>&1 &
    child=$!
    status=0
    wait "$child" || status=$?
    child=
    cat "$work/$phase.log"
    if [ "$expected" = pass ]; then
        [ "$status" -eq 0 ] || exit 1
    else
        [ "$status" -ne 0 ] || {
            echo "agents-mutation: fault passed: $phase" >&2; exit 1;
        }
        for line in "$@"; do
            grep -F "$line" "$work/$phase.log" >/dev/null || {
                echo "agents-mutation: wrong failure: $phase: no $line" >&2; exit 1;
            }
        done
    fi
    echo "agents-mutation: result: $phase status $status"
}

echo "agents-mutation: logs: $work"
logged=1
run_phase fixed pass
changed=1
for name in $faults; do
    apply_fault "$name"
    touch "$(fault_file "$name")"
done
run_phase faults 'AGENTROW FAIL gh: gh --version: status 127' \
    'AGENTROW FAIL claude: claude update: status 1' \
    'AGENTROW FAIL muse: muse --version: status 127' \
    'AGENTROW FAIL limited_test: limited_test.sh --mutate: status'
restore
run_phase restored pass
for name in $faults; do
    cmp "$(fault_file "$name")" "$work/$name.saved"
done
echo "agents-mutation: pass: the faults were rejected and the suite passed with the sources restored"
