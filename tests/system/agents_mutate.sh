#!/bin/sh
# agents_mutate.sh | --texts: show that `tests/run agents` catches a fault in
# the agents layer's install of gh.
#
# Usage: sh tests/system/agents_mutate.sh | --texts
# The fault points gh's link in /usr/bin, which the recipe
# vendor/gnu/agents/packages/gh.sh makes, at a file the package does not
# have, so that the path finds no gh. The suite must pass, fail on the gh
# row with the fault in the recipe and the root built again, and pass again
# with the recipe restored. The suite is x86-64 only. --texts checks that the
# fault is once in its source.
set -eu

repo=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
cd "$repo"
recipe=vendor/gnu/agents/packages/gh.sh

# apply_fault SOURCE: put the fault into SOURCE; with RACH2_FAULT_TEXTS set,
# only check its text and write nothing.
apply_fault()
{
    python3 - "$1" <<'PY'
from pathlib import Path
import sys
import os
import pathlib
if os.environ.get('RACH2_FAULT_TEXTS'):
    pathlib.Path.write_text = lambda self, *args, **kwargs: 0
path = Path(sys.argv[1])
text = path.read_text()
old = 'ln -s ../../opt/gh/bin/gh "$1/usr/bin/gh"'
if text.count(old) != 1:
    raise SystemExit('agents-mutation: the link of gh is not once in the recipe')
path.write_text(text.replace(old, 'ln -s ../../opt/gh/bin/gh.missing "$1/usr/bin/gh"'))
PY
}

if [ "${1:-}" = --texts ]; then
    RACH2_FAULT_TEXTS=1 apply_fault "$recipe" >/dev/null
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

# Put back the saved recipe and touch it, so that make stages gh again.
restore()
{
    if [ -n "$changed" ]; then
        cp "$work/gh.sh.saved" "$recipe"
        touch "$recipe"
        changed=
    fi
}

# Stop the suite and restore the recipe. Then remove the scratch directory. A
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
cp "$recipe" "$work/gh.sh.saved"
cc -O2 -Wall -Wextra -Werror tests/system/runner/runutil.c -o "$work/runutil"

# run_phase PHASE EXPECTED: run the suite, which builds the root again when
# the recipe changed. With EXPECTED `pass` it must pass; otherwise it must
# fail with EXPECTED in its output.
run_phase()
{
    phase=$1
    expected=$2
    ARCH=x86_64 "$work/runutil" group tests/run agents >"$work/$phase.log" 2>&1 &
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
        grep -F "$expected" "$work/$phase.log" >/dev/null || {
            echo "agents-mutation: wrong failure: $phase" >&2; exit 1;
        }
    fi
    echo "agents-mutation: result: $phase status $status"
}

echo "agents-mutation: logs: $work"
logged=1
run_phase fixed pass
changed=$recipe
apply_fault "$recipe"
touch "$recipe"
run_phase link 'AGENTROW FAIL gh: gh --version: status 127'
restore
run_phase restored pass
cmp "$recipe" "$work/gh.sh.saved"
echo "agents-mutation: pass: the fault was rejected and the suite passed with the recipe restored"
