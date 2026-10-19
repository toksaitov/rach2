#!/bin/sh
# agents_mutate.sh [FAULT...] | --texts: show that `tests/run agents` catches
# a fault in each of the agents layer's installs of gh, Claude Code, Codex,
# Antigravity CLI and muse, one in the project's loops, one in the kernel
# that Claude Code's sessions meet, and one in Codex's settings that its
# sessions meet.
#
# Usage: sh tests/system/agents_mutate.sh [FAULT...] | --texts
# Five faults are in recipes of vendor/gnu/agents/packages/. Those of gh and
# muse point the program's link in /usr/bin, which the recipe makes, at a
# file the package does not have, so that the path finds no such program.
# That of claude gives Claude Code's managed settings DISABLE_AUTOUPDATER in
# place of DISABLE_UPDATES, which stops only the background update, so that
# `claude update` tries to fetch a release. That of codex turns on Codex's
# daemon_auto_start in /etc/codex/config.toml, so that Codex starts its
# background server, which outlives the screen of the codex-screen row too.
# That of agy makes /etc/profile.d/agy.sh set
# AGY_CLI_DISABLE_AUTO_UPDATE to false, so that a login shell leaves
# Antigravity CLI's updater on. The sixth, in rach2/sys/fs/procfs.c, stops
# a path from going on past /proc/PID/fd/N of a file or a directory, with
# ENOTDIR, as Rach2 did before it followed the link; Claude Code's Bash tool
# makes its files through such a path, so the claude-print and claude-screen
# rows must fail. The seventh makes tools/flow/worker.sh give its session
# the prompt with `@ISSUE@` in place of the issue's number, which the
# worker-session row's verifier must find in the first request of the session
# on #901.
# The eighth sets sandbox_mode to workspace-write in Codex's
# /etc/codex/config.toml, so that Codex runs the model's commands in its
# sandbox, which Rach2 cannot make, and the codex-exec and codex-screen rows
# must fail; it is one of the faults in `alone`, which run one at a time,
# since the fault of codex fails codex-screen first. The suite runs only the
# rows of these eight, through RACH2_AGENTS_ROWS: it must pass, fail on each
# of them with the first seven faults in their sources and the root and the
# kernel built again, fail on the rows of each fault of `alone` with it alone
# in its source, and pass again with the sources restored. FAULT, one of the
# names in `faults` below, proves only the rows of the faults named. The
# suite is x86-64 only. --texts checks that each fault is once in its
# source.
set -eu

repo=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
cd "$repo"
faults="gh claude codex agy muse procfs worker sandbox"
alone="sandbox"

# fault_rows NAME: the rows that fault NAME must fail.
fault_rows()
{
    case $1 in
        codex) echo codex codex-screen ;;
        procfs) echo claude-print claude-screen ;;
        worker) echo worker-session ;;
        sandbox) echo codex-exec codex-screen ;;
        *) echo "$1" ;;
    esac
}

# fault_failure NAME: print, a line each, what the suite's output must have
# with fault NAME in its source.
fault_failure()
{
    case $1 in
        gh) echo 'AGENTROW FAIL gh: gh --version: status 127' ;;
        claude) echo 'AGENTROW FAIL claude: claude update: status 1' ;;
        codex) printf '%s\n' 'AGENTROW FAIL codex: the processes: got [' \
            "AGENTROW FAIL codex-screen: codex's screen: the processes left: got [" ;;
        agy) echo "AGENTROW FAIL agy: the updater's setting in a login shell: got [false], want [true]" ;;
        muse) echo 'AGENTROW FAIL muse: muse --version: status 127' ;;
        procfs) printf '%s\n' 'agents-test: claude-print: FAIL' 'agents-test: claude-screen: FAIL' ;;
        worker) echo "the session has no worker's prompt for #901" ;;
        sandbox) printf '%s\n' 'agents-test: codex-exec: FAIL' 'agents-test: codex-screen: FAIL' ;;
    esac
}

# fault_file NAME: the file that fault NAME goes into: rach2/sys/fs/procfs.c
# for `procfs`, tools/flow/worker.sh for `worker`, Codex's recipe for
# `sandbox`, and otherwise the recipe of the package NAME.
fault_file()
{
    case $1 in
        procfs) echo rach2/sys/fs/procfs.c ;;
        worker) echo tools/flow/worker.sh ;;
        sandbox) echo vendor/gnu/agents/packages/codex.sh ;;
        *) echo "vendor/gnu/agents/packages/$1.sh" ;;
    esac
}

# apply_fault NAME: put fault NAME into its source: for a package, whose
# program is NAME too, claude's in its managed settings, codex's in its
# system config, agy's in its profile script and the others' in the
# program's link; with
# RACH2_FAULT_TEXTS set, only check its text and write nothing.
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
elif name == 'codex':
    old, new = '\ndaemon_auto_start = false\n', '\ndaemon_auto_start = true\n'
elif name == 'sandbox':
    old = '\nsandbox_mode = "danger-full-access"\n'
    new = '\nsandbox_mode = "workspace-write"\n'
elif name == 'agy':
    old = 'export AGY_CLI_DISABLE_AUTO_UPDATE=true'
    new = 'export AGY_CLI_DISABLE_AUTO_UPDATE=false'
elif name == 'procfs':
    old = 'if ((e->type == FD_TYPE_FILE || e->type == FD_TYPE_DIR) && e->ofd && e->ofd->vmnt) {'
    new = 'if (0) {'
elif name == 'worker':
    old = '    prompt=$(sed "s/@ISSUE@/$1/g" "$state/run/worker.txt")\n'
    new = '    prompt=$(cat "$state/run/worker.txt")\n'
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

if [ "$#" -gt 0 ]; then
    for name; do
        case " $faults " in
            *" $name "*) ;;
            *) echo "agents-mutation: no fault named $name" >&2; exit 2 ;;
        esac
    done
    faults=$*
fi

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
# and builds the kernel again. The saved copies of `codex` and `sandbox` are
# the same file.
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

# run_phase PHASE pass|fail NAME...: run the suite's rows of the faults
# NAME, which builds the root and the kernel again when a source changed.
# With `pass` it must pass; with `fail` it must fail with what fault_failure
# prints for each fault NAME in its output.
run_phase()
{
    phase=$1
    expected=$2
    shift 2
    rows=
    for name; do
        rows="$rows $(fault_rows "$name")"
    done
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
        for name; do
            fault_failure "$name"
        done > "$work/expected"
        while IFS= read -r line; do
            grep -F "$line" "$work/$phase.log" >/dev/null || {
                echo "agents-mutation: wrong failure: $phase: no $line" >&2; exit 1;
            }
        done < "$work/expected"
    fi
    echo "agents-mutation: result: $phase status $status"
}

echo "agents-mutation: logs: $work"
logged=1
together=
separate=
for name in $faults; do
    case " $alone " in
        *" $name "*) separate="$separate $name" ;;
        *) together="$together $name" ;;
    esac
done

# shellcheck disable=SC2086 # the lists are names, a word each
run_phase fixed pass $faults
if [ -n "$together" ]; then
    changed=1
    for name in $together; do
        apply_fault "$name"
        touch "$(fault_file "$name")"
    done
    # shellcheck disable=SC2086
    run_phase faults fail $together
    restore
fi
for name in $separate; do
    changed=1
    apply_fault "$name"
    touch "$(fault_file "$name")"
    run_phase "$name" fail "$name"
    restore
done
# shellcheck disable=SC2086
run_phase restored pass $faults
for name in $faults; do
    cmp "$(fault_file "$name")" "$work/$name.saved"
done
echo "agents-mutation: pass: the faults were rejected and the suite passed with the sources restored"
