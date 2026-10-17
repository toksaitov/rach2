#!/bin/sh
# fakeapi_mutate.sh [--texts]: show that `tests/run fakeapi` catches two
# faults in the fake API server, on x86-64, the suite's one architecture.
#
# Usage: sh tests/system/fakeapi_mutate.sh [--texts]
#
# The suite runs four times: as the tree has it, where it must pass; with
# proto_plain.py's check of the path taken out, where the request the script
# does not expect plays the last step, and the suite must fail on its
# answer; with fakeapi.py no longer counting an unexpected request as a
# failure, where the server's verdict passes the run and the suite must fail
# on it; and restored, where it must pass again. Each fault is put into the
# tree's source, which the trap restores, and the server is Python, so the
# suite runs the changed source with nothing to build. --texts checks that
# each fault's text is once in its source.
set -eu
repo=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
cd "$repo"
texts=
[ "${1:-}" != --texts ] || texts=1

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
    raise SystemExit(f"fakeapi-mutation: not once in {path}: {old}")
if not os.environ.get("RACH2_FAULT_TEXTS"):
    path.write_text(text.replace(old, new))
PY
}

# fault NAME: put the fault NAME into its source.
fault()
{
    case $1 in
        path-ignored)
            mutate tests/system/fakeapi/proto_plain.py \
                '    if request.path != expected["path"]:' \
                '    if False and request.path != expected["path"]:'
            ;;
        unexpected-uncounted)
            mutate tests/system/fakeapi/fakeapi.py \
                '                self.failures.append(f"request {n}: {request.describe()}: {reason}")' \
                '                pass'
            ;;
    esac
}

if [ -n "$texts" ]; then
    RACH2_FAULT_TEXTS=1
    export RACH2_FAULT_TEXTS
    for name in path-ignored unexpected-uncounted; do
        fault "$name"
    done
    echo 'fakeapi-mutation: texts: every fault is once in its source'
    exit 0
fi

. "$repo/tests/system/runlock.sh"
runlock_hold "$repo" fakeapi-mutation "$repo/tests/system/fakeapi_mutate.sh" "$@"
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

# Put back the saved sources and mark them unchanged.
restore()
{
    cp "$work/fakeapi.py.saved" tests/system/fakeapi/fakeapi.py
    cp "$work/proto_plain.py.saved" tests/system/fakeapi/proto_plain.py
    changed=
}

# Stop the suite and, when a fault is in a source, restore it. The work
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
work=$(mktemp -d "$repo/build/fakeapi-proof.XXXXXX")
cp tests/system/fakeapi/fakeapi.py "$work/fakeapi.py.saved"
cp tests/system/fakeapi/proto_plain.py "$work/proto_plain.py.saved"

# run_phase PHASE EXPECTED MESSAGE: run the suite. With EXPECTED `pass` it
# must pass, otherwise fail, and either way its output must have MESSAGE.
run_phase()
{
    phase=$1
    expected=$2
    message=$3
    ARCH=x86_64 build/tests/system/runutil group tests/run fakeapi >"$work/$phase.out" 2>&1 &
    child=$!
    status=0
    wait "$child" || status=$?
    stop_child
    cat "$work/$phase.out"
    for log in build/x86_64/images/fakeapi-test.disk.*; do
        [ ! -f "$log" ] || cp "$log" "$work/$phase.${log##*.disk.}"
    done
    if [ "$expected" = pass ]; then
        [ "$status" -eq 0 ] || exit 1
    else
        [ "$status" -ne 0 ] || {
            echo "fakeapi-mutation: the fault passed: $phase" >&2; exit 1;
        }
    fi
    grep -Fq "$message" "$work/$phase.out" || {
        echo "fakeapi-mutation: $phase did not end with: $message" >&2; exit 1;
    }
    echo "fakeapi-mutation: result: $phase status $status"
}

echo "fakeapi-mutation: logs: $work"
keep=1
make -s build/tests/system/runutil >"$work/prepare.log" 2>&1 || {
    cat "$work/prepare.log" >&2; exit 1;
}
passed='fakeapi-test: PASS'
run_phase fixed pass "$passed"

changed=1
fault path-ignored
run_phase path-ignored fail 'fakeapi-test: FAIL: the server answered a request its script does not expect'
restore

changed=1
fault unexpected-uncounted
run_phase unexpected-uncounted fail \
    "fakeapi-test: FAIL: the server's verdict, status 0, did not fail the run for the unexpected request"
restore

run_phase restored pass "$passed"
cmp "$work/fakeapi.py.saved" tests/system/fakeapi/fakeapi.py
cmp "$work/proto_plain.py.saved" tests/system/fakeapi/proto_plain.py
echo 'fakeapi-mutation: result: pass: both faults failed the suite; the restored server passed'
