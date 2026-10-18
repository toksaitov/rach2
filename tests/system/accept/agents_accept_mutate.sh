#!/bin/sh
# agents_accept_mutate.sh [--texts]: show that the QEMU mode of
# agents_accept.sh, `tests/run agentsaccept`, catches a kernel that does not
# tell a terminal's programs that its window changed size.
#
# Usage: sh tests/system/accept/agents_accept_mutate.sh [--texts]
#
# The suite runs three times, with Claude Code alone: as the tree has it,
# where it must pass; with rach2/sys/tty/pty.c sending no SIGWINCH when a
# pseudo-terminal's master sets the window size, as sshd does when the
# client's window changes, where the tmux client on the machine never learns
# the new size, and the suite must fail on the resized terminal; and with
# the source restored and the kernel built again, where it must pass again.
# The suite builds the kernel from the source it boots. --texts checks that
# the fault's text is once in its source.
set -eu
repo=$(CDPATH='' cd -- "$(dirname -- "$0")/../../.." && pwd)
cd "$repo"
source=rach2/sys/tty/pty.c

# fault: put the fault into its source; with RACH2_FAULT_TEXTS set, only
# check that its text is once there.
fault()
{
    python3 - "$source" <<'PY'
import os
import pathlib
import sys

path = pathlib.Path(sys.argv[1])
old = ("        if (p->slave.fg_pgid != 0) {\n"
       "            tty_signal_group(p->slave.fg_pgid, SIGWINCH);\n")
new = ("        if (0) {\n"
       "            tty_signal_group(p->slave.fg_pgid, SIGWINCH);\n")
text = path.read_text()
if text.count(old) != 1:
    raise SystemExit(f"agents-accept-mutation: the fault's text is not once in {path}")
if not os.environ.get("RACH2_FAULT_TEXTS"):
    path.write_text(text.replace(old, new))
PY
}

if [ "${1:-}" = --texts ]; then
    RACH2_FAULT_TEXTS=1 fault
    echo 'agents-accept-mutation: texts: the fault is once in its source'
    exit 0
fi

. "$repo/tests/system/runlock.sh"
runlock_hold "$repo" agents-accept-mutation "$repo/tests/system/accept/agents_accept_mutate.sh" "$@"
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

# Put back the saved source and touch it, so that make builds the kernel
# again.
restore()
{
    cp "$work/pty.c.saved" "$source"
    touch "$source"
    changed=
}

# Stop the suite and, when the fault is in the source, restore it. The work
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
work=$(mktemp -d "$repo/build/agents-accept-proof.XXXXXX")
cp "$source" "$work/pty.c.saved"

# run_phase PHASE EXPECTED MESSAGE: run the suite with Claude Code alone.
# With EXPECTED `pass` it must pass, otherwise fail, and either way the
# acceptance's log must have MESSAGE.
run_phase()
{
    phase=$1
    expected=$2
    message=$3
    ARCH=x86_64 RACH2_ACCEPT_AGENTS=claude build/tests/system/runutil group \
        tests/run agentsaccept >"$work/$phase.out" 2>&1 &
    child=$!
    status=0
    wait "$child" || status=$?
    stop_child
    cat "$work/$phase.out"
    for log in build/x86_64/images/agentsaccept-test.*; do
        [ ! -f "$log" ] || cp "$log" "$work/$phase.${log##*/agentsaccept-test.}"
    done
    if [ "$expected" = pass ]; then
        [ "$status" -eq 0 ] || exit 1
    else
        [ "$status" -ne 0 ] || {
            echo "agents-accept-mutation: the fault passed: $phase" >&2; exit 1;
        }
    fi
    grep -Fq "$message" "$work/$phase.disk.log" || {
        echo "agents-accept-mutation: $phase did not end with: $message" >&2; exit 1;
    }
    echo "agents-accept-mutation: result: $phase status $status"
}

echo "agents-accept-mutation: logs: $work"
keep=1
make -s build/tests/system/runutil >"$work/prepare.log" 2>&1 || {
    cat "$work/prepare.log" >&2; exit 1;
}
run_phase fixed pass 'agents-accept: PASS'

changed=1
fault
touch "$source"
run_phase winch fail 'agents-accept: FAIL: claude: the resized terminal: got [120x39 120x39 1]'
restore

run_phase restored pass 'agents-accept: PASS'
cmp "$work/pty.c.saved" "$source"
echo 'agents-accept-mutation: result: pass: the fault failed the suite; the restored kernel passed'
