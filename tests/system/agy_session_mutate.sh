#!/bin/sh
# agy_session_mutate.sh | --texts: prove the agy-print and agy-screen rows of
# the agents suite against a script that has Antigravity CLI write
# answer=afterward where the fixture should have answer=after. The program
# edits and commits as it is told, and every function response still passes
# the server's checks; the host must reject the returned file. The suite
# passes before and after the fault, and its first pass also runs the agy
# row, whose names the session rows give the fake server. The trap restores
# the script and stops its children.
set -eu

repo=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
cd "$repo"
source=tests/system/fakeapi/agy-session.json

# apply_fault: change the text the fake API has the program write in place
# of answer=before; with RACH2_FAULT_TEXTS set, check the text without
# writing it.
apply_fault()
{
    python3 - "$source" <<'EOF'
from pathlib import Path
import os
import sys
path = Path(sys.argv[1])
text = path.read_text()
old = '"ReplacementContent": "answer=after"'
if text.count(old) != 1:
    raise SystemExit('agy-session-mutation: the replacement is not once in the script')
if not os.environ.get('RACH2_FAULT_TEXTS'):
    path.write_text(text.replace(old, '"ReplacementContent": "answer=afterward"'))
EOF
}

if [ "${1:-}" = --texts ]; then
    RACH2_FAULT_TEXTS=1 apply_fault
    echo 'agy-session-mutation: texts: the fault is once in its source'
    exit 0
fi

. "$repo/tests/system/runlock.sh"
runlock_hold "$repo" agy-session-mutation "$repo/tests/system/agy_session_mutate.sh" "$@"

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

# Restore the saved script and touch it.
restore()
{
    if [ -n "$changed" ]; then
        cp "$work/script.saved" "$source"
        touch "$source"
        changed=
    fi
}

# Stop the suite and restore the script. Then remove the scratch directory. A
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
work=$(mktemp -d "$repo/build/rach2-agy-session-proof.XXXXXX")
cp "$source" "$work/script.saved"
make -s build/tests/system/runutil
cp build/tests/system/runutil "$work/runutil"

# run_phase PHASE EXPECTED...: run the suite, which must pass for EXPECTED
# `pass`; otherwise it must fail with each EXPECTED in its output.
run_phase()
{
    phase=$1
    shift
    expected=$1
    rows='agy-print agy-screen'
    [ "$phase" != fixed ] || rows='agy agy-print agy-screen'
    RACH2_AGENTS_ROWS="$rows" ARCH=x86_64 "$work/runutil" group tests/run agents >"$work/$phase.log" 2>&1 &
    child=$!
    status=0
    wait "$child" || status=$?
    child=
    cat "$work/$phase.log"
    if [ "$expected" = pass ]; then
        [ "$status" -eq 0 ] || exit 1
    else
        [ "$status" -ne 0 ] || {
            echo "agy-session-mutation: fault passed: $phase" >&2; exit 1;
        }
        for line in "$@"; do
            grep -F "$line" "$work/$phase.log" >/dev/null || {
                echo "agy-session-mutation: wrong failure: $phase: no $line" >&2; exit 1;
            }
        done
    fi
    echo "agy-session-mutation: result: $phase status $status"
}

echo "agy-session-mutation: logs: $work"
logged=1
run_phase fixed pass
changed=1
apply_fault
run_phase replacement 'agy-session-verify: fail: wrong edited fixture' \
    'agents-test: agy-print: FAIL' 'agents-test: agy-screen: FAIL'
restore
run_phase restored pass
cmp "$source" "$work/script.saved"
echo "agy-session-mutation: pass: the wrong file was rejected and the restored script passed"
