#!/bin/sh
# muse_session_mutate.sh | --texts: prove the Muse session row against a
# commit made with the wrong author. The Model API exchange still succeeds;
# the host must reject the returned history. The suite passes before and
# after the fault. The first pass also checks the package rows and the short
# loop test, which exercises the shared tree setup. The trap restores the
# script and stops its children.
set -eu

repo=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
cd "$repo"
source=tests/system/fakeapi/muse.json

# apply_fault: change the author of the commit the fake API asks Muse to make;
# with RACH2_FAULT_TEXTS set, check the text without writing it.
apply_fault()
{
    python3 - "$source" <<'EOF'
from pathlib import Path
import os
import sys
path = Path(sys.argv[1])
text = path.read_text()
old = "git commit -q -am 'Greet Rach2'"
if text.count(old) != 1:
    raise SystemExit('muse-session-mutation: the commit command is not once in the script')
if not os.environ.get('RACH2_FAULT_TEXTS'):
    path.write_text(text.replace(old, "git -c user.name=Impostor commit -q -am 'Greet Rach2'"))
EOF
}

if [ "${1:-}" = --texts ]; then
    RACH2_FAULT_TEXTS=1 apply_fault
    echo 'muse-session-mutation: texts: the fault is once in its source'
    exit 0
fi

. "$repo/tests/system/runlock.sh"
runlock_hold "$repo" muse-session-mutation "$repo/tests/system/muse_session_mutate.sh" "$@"

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
work=$(mktemp -d "$repo/build/muse-session-proof.XXXXXX")
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
    rows='muse muse-tmux'
    [ "$phase" != fixed ] || rows='claude gh limited_test muse muse-tmux'
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
            echo "muse-session-mutation: fault passed: $phase" >&2; exit 1;
        }
        for line in "$@"; do
            grep -F "$line" "$work/$phase.log" >/dev/null || {
                echo "muse-session-mutation: wrong failure: $phase: no $line" >&2; exit 1;
            }
        done
    fi
    echo "muse-session-mutation: result: $phase status $status"
}

echo "muse-session-mutation: logs: $work"
logged=1
run_phase fixed pass
changed=1
apply_fault
run_phase author 'muse-verify: fail: the history or commit author' \
    'agents-test: muse: FAIL' 'agents-test: muse-tmux: FAIL'
restore
run_phase restored pass
cmp "$source" "$work/script.saved"
echo "muse-session-mutation: pass: the wrong author was rejected and the restored script passed"
