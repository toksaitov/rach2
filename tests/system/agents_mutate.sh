#!/bin/sh
# agents_mutate.sh | --texts: show that `tests/run agents` catches a fault in
# the agents layer's install of gh and one in its install of muse.
#
# Usage: sh tests/system/agents_mutate.sh | --texts
# Each fault points a program's link in /usr/bin, which its recipe in
# vendor/gnu/agents/packages/ makes, at a file the package does not have, so
# that the path finds no such program. The suite must pass, fail on the gh
# row and on the muse row with both faults in the recipes and the root built
# again, and pass again with the recipes restored. The suite is x86-64 only.
# --texts checks that each fault is once in its source.
set -eu

repo=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
cd "$repo"
packages="gh muse"

# apply_fault NAME: put the fault into the recipe of the package NAME, whose
# program is NAME too; with RACH2_FAULT_TEXTS set, only check its text and
# write nothing.
apply_fault()
{
    python3 - "vendor/gnu/agents/packages/$1.sh" "$1" <<'PY'
from pathlib import Path
import sys
import os
import pathlib
if os.environ.get('RACH2_FAULT_TEXTS'):
    pathlib.Path.write_text = lambda self, *args, **kwargs: 0
path = Path(sys.argv[1])
name = sys.argv[2]
text = path.read_text()
old = f'ln -s ../../opt/{name}/bin/{name} "$1/usr/bin/{name}"'
if text.count(old) != 1:
    raise SystemExit(f'agents-mutation: the link of {name} is not once in its recipe')
path.write_text(text.replace(old, f'ln -s ../../opt/{name}/bin/{name}.missing "$1/usr/bin/{name}"'))
PY
}

if [ "${1:-}" = --texts ]; then
    for name in $packages; do
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

# Put back the saved recipes and touch them, so that make stages the packages
# again.
restore()
{
    if [ -n "$changed" ]; then
        for name in $packages; do
            cp "$work/$name.sh.saved" "vendor/gnu/agents/packages/$name.sh"
            touch "vendor/gnu/agents/packages/$name.sh"
        done
        changed=
    fi
}

# Stop the suite and restore the recipes. Then remove the scratch directory. A
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
for name in $packages; do
    cp "vendor/gnu/agents/packages/$name.sh" "$work/$name.sh.saved"
done
cc -O2 -Wall -Wextra -Werror tests/system/runner/runutil.c -o "$work/runutil"

# run_phase PHASE EXPECTED...: run the suite, which builds the root again
# when a recipe changed. With EXPECTED `pass` it must pass; otherwise it must
# fail with each EXPECTED in its output.
run_phase()
{
    phase=$1
    shift
    expected=$1
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
for name in $packages; do
    apply_fault "$name"
    touch "vendor/gnu/agents/packages/$name.sh"
done
run_phase links 'AGENTROW FAIL gh: gh --version: status 127' \
    'AGENTROW FAIL muse: muse --version: status 127'
restore
run_phase restored pass
for name in $packages; do
    cmp "vendor/gnu/agents/packages/$name.sh" "$work/$name.sh.saved"
done
echo "agents-mutation: pass: the faults were rejected and the suite passed with the recipes restored"
