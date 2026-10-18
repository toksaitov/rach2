#!/bin/sh
# exe_mutate.sh [--texts]: show that the gnu suite's bash row catches a
# kernel that names a program by the path it was executed by, links and
# all, on x86-64.
#
# Usage: sh tests/system/exe_mutate.sh [--texts]
#
# The gnu suite runs its bash row alone three times: with the kernel as the
# tree has it, where it must pass; with execve naming the new image by the
# path it was given in place of the path its walk took, where /proc/PID/exe
# of a bash run through two links must not be the file's path; and with the
# source restored and the kernel rebuilt, where it must pass again. The
# suite builds the kernel before it boots. --texts checks that the fault's
# text is once in its source.
set -eu
repo=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
cd "$repo"
source=rach2/sys/syscall_proc.c

# apply_fault: put the fault into the source; with RACH2_FAULT_TEXTS set,
# only check that its text is once there.
apply_fault()
{
    python3 - "$source" <<'PY'
import os
import pathlib
import sys

path = pathlib.Path(sys.argv[1])
old = "    const char *name = real[0] != '\\0' ? real : path;\n"
new = "    const char *name = path;\n"
text = path.read_text()
if text.count(old) != 1:
    raise SystemExit(f"exe-mutation: not once in {path}: {old}")
if not os.environ.get("RACH2_FAULT_TEXTS"):
    path.write_text(text.replace(old, new))
PY
}

if [ "${1:-}" = --texts ]; then
    RACH2_FAULT_TEXTS=1 apply_fault
    echo 'exe-mutation: texts: every fault is once in its source'
    exit 0
fi

. "$repo/tests/system/runlock.sh"
runlock_hold "$repo" exe-mutation "$repo/tests/system/exe_mutate.sh" "$@"
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

# Put back the saved source and touch it, so that the kernel is built again.
restore()
{
    cp "$work/syscall_proc.c.saved" "$source"
    touch "$source"
    changed=
}

# Stop the suite and, when the fault is in the source, restore it and
# rebuild the kernel. The work directory is removed too, unless the proof
# failed after printing its path, which sets `keep`; a signal clears `keep`.
cleanup()
{
    exit_status=$?
    stop_child
    if [ -n "$changed" ]; then
        restore
        make -s ARCH=x86_64 rach2 >"$work/cleanup.build.log" 2>&1 || :
    fi
    if [ "$exit_status" -eq 0 ] || [ -z "$keep" ]; then
        rm -rf ${work:+"$work"}
    fi
}
trap cleanup EXIT
trap 'keep=; exit 129' HUP
trap 'keep=; exit 130' INT
trap 'keep=; exit 143' TERM
work=$(mktemp -d "$repo/build/exe-proof.XXXXXX")
cp "$source" "$work/syscall_proc.c.saved"

# run_phase PHASE EXPECTED MESSAGE: run the suite's bash row. With EXPECTED
# `pass` it must pass, otherwise fail, and either way its output must have
# MESSAGE.
run_phase()
{
    phase=$1
    expected=$2
    message=$3
    ARCH=x86_64 RACH2_GNU_ROWS=bash build/tests/system/runutil group tests/run gnu \
        >"$work/$phase.out" 2>&1 &
    child=$!
    status=0
    wait "$child" || status=$?
    stop_child
    cat "$work/$phase.out"
    for log in build/x86_64/images/gnu-test.disk.*; do
        [ ! -f "$log" ] || cp "$log" "$work/$phase.${log##*.disk.}"
    done
    if [ "$expected" = pass ]; then
        [ "$status" -eq 0 ] || exit 1
    else
        [ "$status" -ne 0 ] || {
            echo "exe-mutation: the fault passed: $phase" >&2; exit 1;
        }
    fi
    grep -Fq "$message" "$work/$phase.out" || {
        echo "exe-mutation: $phase did not end with: $message" >&2; exit 1;
    }
    echo "exe-mutation: result: $phase status $status"
}

echo "exe-mutation: logs: $work"
keep=1
make -s build/tests/system/runutil >"$work/prepare.log" 2>&1 || {
    cat "$work/prepare.log" >&2; exit 1;
}
run_phase fixed pass 'gnu-test: bash: OK'

changed=1
apply_fault
touch "$source"
run_phase unresolved fail "gnu-test: bash: FAIL after"
grep -Fq "/proc/PID/exe through links: got [" "$work/unresolved.out" || {
    echo 'exe-mutation: unresolved did not fail on /proc/PID/exe' >&2; exit 1;
}
restore

run_phase restored pass 'gnu-test: bash: OK'
cmp "$work/syscall_proc.c.saved" "$source"
echo 'exe-mutation: result: pass: the fault failed the row; the restored kernel passed'
