#!/bin/sh
# etudeexec_mutate.sh [--texts]: show that the etudeexec suite catches three
# faults in etude's Rach2 adapter and recipe, on the kernel, disk and emulator
# of the architecture ARCH names.
#
# Usage: sh tests/system/etudeexec_mutate.sh [--texts]
#
# The suite archives the files Rach2 builds from a private copy of them,
# which ETUDEEXEC_TREE names: as they are; with the POSIX adapter's Linux
# interfaces not chosen on Rach2, so that a process has no known start; with
# the keeper never made a subreaper there; and with the recipe's flags
# lacking -D__rach2__. The unchanged copy must pass, each fault must fail
# the rows it breaks, and the copy restored must pass. Nothing in the tree is
# changed. --texts checks that each fault is once in its source.
set -eu
repo=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
cd "$repo"

# apply_fault FILE NAME: put the fault NAME into the copy FILE; with
# RACH2_FAULT_TEXTS set, only check that its text is once in FILE.
apply_fault()
{
    python3 - "$1" "$2" <<'PY'
from pathlib import Path
import os
import sys
path = Path(sys.argv[1])
source = path.read_text()
faults = {
    'adapter': ('#if defined(__linux__) || defined(__rach2__)\n#define ETUDE_PROC_LINUX 1',
                '#if defined(__linux__)\n#define ETUDE_PROC_LINUX 1'),
    'subreaper': ('    return prctl(PR_SET_CHILD_SUBREAPER, 1, 0, 0, 0);',
                  '    return -1;'),
    'recipe': ("            flags='-std=c99 -static -D__rach2__' ;;",
               "            flags='-std=c99 -static' ;;"),
}
old, new = faults[sys.argv[2]]
if source.count(old) != 1:
    raise SystemExit('etudeexec-mutation: fault text is not unique: ' + sys.argv[2])
if not os.environ.get('RACH2_FAULT_TEXTS'):
    path.write_text(source.replace(old, new))
PY
}

if [ "${1:-}" = --texts ]; then
    RACH2_FAULT_TEXTS=1
    export RACH2_FAULT_TEXTS
    apply_fault tools/etude/posix/process.c adapter
    apply_fault tools/etude/posix/process.c subreaper
    apply_fault tools/etude/bootstrap.sh recipe
    echo 'etudeexec-mutation: texts: every fault is once in its source'
    exit 0
fi

. "$repo/tests/system/runlock.sh"
runlock_hold "$repo" etudeexec-mutation "$repo/tests/system/etudeexec_mutate.sh" "$@"
RACH2_ARCH=${RACH2_ARCH:-${ARCH:-i686}}
work=
logged=
child=

# Stop the suite and its machine, and wait until they are gone.
stop_child()
{
    if [ -n "$child" ]; then
        kill -TERM "$child" 2>/dev/null || :
        wait "$child" 2>/dev/null || :
        child=
    fi
}

# Stop the suite, then remove the scratch directory. A failure after its
# path was printed keeps it as evidence; a signal does not.
cleanup()
{
    status=$?
    [ -n "$work" ] || return 0
    stop_child
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
mkdir -p build/tests
work=$(mktemp -d "$repo/build/tests/rach2-etudeexec-proof.XXXXXX")
echo "etudeexec-mutation: logs: $work"
logged=1
files="tools/etude utils/sha256.c utils/sha256.h tests/host/etude_exec_hosttest.c
tests/host/compat/fixdir_host.c tests/host/compat/fixdir_host.h tests/auxvprobe.c
tests/system/etudeexec_guest.sh"

# fresh: make the private copy the tree's files as they are.
fresh()
{
    rm -rf "${work:?}/tree"
    mkdir "$work/tree"
    # shellcheck disable=SC2086 # files is a list of paths without spaces
    tar -cf - $files | tar -xf - -C "$work/tree"
}

# run_phase PHASE EXPECTED [ROWS]: run the suite on the private copy. With
# EXPECTED `pass` it must pass; otherwise it must fail with exactly ROWS, the
# rows that did not pass.
run_phase()
{
    phase=$1
    expected=$2
    status=0
    make -s run-etudeexec "ARCH=$RACH2_ARCH" "ETUDEEXEC_TREE=$work/tree" \
        >"$work/$phase.log" 2>&1 &
    child=$!
    wait "$child" || status=$?
    child=
    grep '^etudeexec' "$work/$phase.log" || :
    if [ "$expected" = pass ]; then
        [ "$status" -eq 0 ] || exit 1
    else
        [ "$status" -ne 0 ] || {
            echo "etudeexec-mutation: the suite passed with its fault: $phase" >&2; exit 1;
        }
        grep -qx "etudeexec-test: FAIL: rows did not pass: $3" "$work/$phase.log" || exit 1
    fi
    echo "etudeexec-mutation: result: $phase status $status"
}

fresh
run_phase fixed pass
apply_fault "$work/tree/tools/etude/posix/process.c" adapter
run_phase adapter fail executor
fresh
apply_fault "$work/tree/tools/etude/posix/process.c" subreaper
run_phase subreaper fail executor
fresh
apply_fault "$work/tree/tools/etude/bootstrap.sh" recipe
run_phase recipe fail 'recipe executor'
fresh
run_phase restored pass
echo 'etudeexec-mutation: result: pass: each fault failed its rows; the restored copy passed'
