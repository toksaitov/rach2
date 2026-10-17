#!/bin/sh
# museapi_mutate.sh [--texts]: show that `tests/run museapi` catches two
# faults in the fake API server's Responses protocol, on x86-64, the suite's
# one architecture.
#
# Usage: sh tests/system/museapi_mutate.sh [--texts]
#
# The suite runs four times: as the tree has it, where it must pass; with
# proto_responses.py taking any output of a tool call as the one a turn
# expects, where the wrong tool result is accepted, and the suite must fail
# for want of a refusal; with a namespace's functions looked for among the
# request's top-level tools, where the server finds none of Muse's tools and
# refuses its first turn, and the suite must fail on Muse's status; and
# restored, where it must pass again. Each fault is put into the tree's
# source, which the trap restores, and the server is Python, so the suite
# runs the changed source with nothing to build. --texts checks that each
# fault's text is once in its source.
set -eu
repo=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
cd "$repo"
texts=
[ "${1:-}" != --texts ] || texts=1
source=tests/system/fakeapi/proto_responses.py

# mutate FILE OLD NEW: replace OLD, which must be once in FILE, with NEW;
# with RACH2_FAULT_TEXTS set, only check that OLD is once in FILE.
mutate()
{
    python3 - "$1" "$2" "$3" <<'PY'
import os
import pathlib
import sys

path, old, new = pathlib.Path(sys.argv[1]), sys.argv[2], sys.argv[3]
text = path.read_text()
if text.count(old) != 1:
    raise SystemExit(f"museapi-mutation: not once in {path}: {old}")
if not os.environ.get("RACH2_FAULT_TEXTS"):
    path.write_text(text.replace(old, new))
PY
}

# fault NAME: put the fault NAME into its source.
fault()
{
    case $1 in
        result-accepted)
            mutate "$source" \
                '        if all(w in text for w in wanted):' \
                '        if True:'
            ;;
        namespace-unsearched)
            mutate "$source" \
                '        tools = [t for space in spaces for t in space.get("tools") or []]' \
                '        tools = list(tools)'
            ;;
    esac
}

if [ -n "$texts" ]; then
    RACH2_FAULT_TEXTS=1
    export RACH2_FAULT_TEXTS
    for name in result-accepted namespace-unsearched; do
        fault "$name"
    done
    echo 'museapi-mutation: texts: every fault is once in its source'
    exit 0
fi

. "$repo/tests/system/runlock.sh"
runlock_hold "$repo" museapi-mutation "$repo/tests/system/museapi_mutate.sh" "$@"
mkdir -p build
work=
child=
changed=
keep=

# Stop the running suite's process group and wait until it is gone.
stop_child()
{
    if [ -n "$child" ]; then
        kill -TERM "-$child" 2>/dev/null || :
        wait "$child" 2>/dev/null || :
        while kill -0 "-$child" 2>/dev/null; do
            sleep 1
        done
        child=
    fi
}

# Put back the saved source and mark it unchanged.
restore()
{
    cp "$work/proto_responses.py.saved" "$source"
    changed=
}

# Stop the suite and, when a fault is in the source, restore it. The work
# directory is removed too, unless the proof failed after printing its path,
# which sets `keep`; a signal clears `keep`.
cleanup()
{
    exit_status=$?
    stop_child
    if [ -n "$changed" ]; then
        restore
    fi
    if [ "$exit_status" -eq 0 ] || [ -z "$keep" ]; then
        rm -rf ${work:+"$work"}
    fi
}
trap cleanup EXIT
trap 'keep=; exit 129' HUP
trap 'keep=; exit 130' INT
trap 'keep=; exit 143' TERM
work=$(mktemp -d "$repo/build/museapi-proof.XXXXXX")
cp "$source" "$work/proto_responses.py.saved"

# run_phase PHASE EXPECTED MESSAGE: run the suite. With EXPECTED `pass` it
# must pass, otherwise fail, and either way its output must have MESSAGE.
run_phase()
{
    phase=$1
    expected=$2
    message=$3
    ARCH=x86_64 build/tests/system/runutil group tests/run museapi >"$work/$phase.out" 2>&1 &
    child=$!
    status=0
    wait "$child" || status=$?
    stop_child
    cat "$work/$phase.out"
    for log in build/x86_64/images/museapi-test.*; do
        [ ! -f "$log" ] || cp "$log" "$work/$phase.${log##*/museapi-test.}"
    done
    if [ "$expected" = pass ]; then
        [ "$status" -eq 0 ] || exit 1
    else
        [ "$status" -ne 0 ] || {
            echo "museapi-mutation: the fault passed: $phase" >&2; exit 1;
        }
    fi
    grep -Fq "$message" "$work/$phase.out" || {
        echo "museapi-mutation: $phase did not end with: $message" >&2; exit 1;
    }
    echo "museapi-mutation: result: $phase status $status"
}

echo "museapi-mutation: logs: $work"
keep=1
make -s build/tests/system/runutil >"$work/prepare.log" 2>&1 || {
    cat "$work/prepare.log" >&2; exit 1;
}
passed='museapi-test: PASS'
run_phase fixed pass "$passed"

changed=1
fault result-accepted
run_phase result-accepted fail 'museapi-test: FAIL: the server did not refuse a wrong tool result'
restore

changed=1
fault namespace-unsearched
run_phase namespace-unsearched fail 'museapi-test: FAIL: muse exec ended with status'
restore

run_phase restored pass "$passed"
cmp "$work/proto_responses.py.saved" "$source"
echo 'museapi-mutation: result: pass: both faults failed the suite; the restored server passed'
