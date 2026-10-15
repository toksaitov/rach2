#!/bin/sh
# init_refusal_mutate.sh: show that the row of init_refusal.sh catches a
# compiled fault, with kernels built from a private copy for the
# architecture ARCH names, on its emulator.
#
# Usage: sh tests/system/init_refusal_mutate.sh [--texts]
#
# tests/system/init_refusal.sh boots a root whose /sbin/init is a symbolic
# link to itself and requires the kernel to refuse it and halt. The kernel
# is built three times from a private copy of rach2/ and mk/, never from the
# main tree's sources or into its build/: as it is, where the row must pass;
# with the lookup's test of -ENOENT widened to any failure, which takes any
# failure for an absent init and opens the debug shell, where the row must
# fail and the log must show that shell; and restored, touched and rebuilt,
# where it must pass again with a kernel identical to the first. The trap
# stops the row's process group before it restores the private source.
set -eu
repo=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
cd "$repo"

# apply_fault SOURCE: put the fault into SOURCE; with
# RACH2_FAULT_TEXTS set, only check its text and write nothing.
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
old = "        if (rc == -ENOENT) {\n"
if text.count(old) != 1:
    raise SystemExit("init-refusal-mutation: the init lookup's test is not unique")
path.write_text(text.replace(old, "        if (rc != 0) {\n"))
PY
}

if [ "${1:-}" = --texts ]; then
    RACH2_FAULT_TEXTS=1
    export RACH2_FAULT_TEXTS
    apply_fault rach2/kernel.c >/dev/null
    echo 'init-refusal-mutation: texts: every fault is once in its source'
    exit 0
fi


RACH2_ARCH=${RACH2_ARCH:-${ARCH:-i686}}
REPO=$repo
. "$repo/mk/arch.sh"
RACH2_QEMU=$ARCH_QEMU
export RACH2_QEMU ARCH_BUILD
. "$repo/tests/system/runlock.sh"
runlock_hold "$repo" init-refusal-mutation "$repo/tests/system/init_refusal_mutate.sh" "$@"
mkdir -p build
work=$(mktemp -d "$repo/build/init-refusal-proof.XXXXXX")
tree="$work/tree"
source="$tree/rach2/kernel.c"
child=
changed=

# Stop the running boot's process group and wait until it is gone.
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

# Put back the saved kernel.c of the private tree and touch it.
restore()
{
    cp "$work/kernel.saved" "$source"
    touch "$source"
    changed=
}

# Stop the row and, when a fault is in the private source, restore it.
cleanup()
{
    stop_child
    if [ -n "$changed" ]; then
        restore
    fi
}
trap cleanup EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

# run_phase PHASE EXPECTED: build the kernel in the private tree and run the
# row on it. With EXPECTED `pass` the row must pass; otherwise it must fail
# and its log must show the debug shell. The object is removed first, since
# a source restored within the second its object was built reads as up to
# date to a make that compares whole seconds.
run_phase()
{
    phase=$1
    expected=$2
    rm -f "$tree/build$ARCH_SUFFIX/rach2/kernel.o" "$tree/build$ARCH_SUFFIX/rach2/kernel"
    make -s -j8 -C "$tree/rach2" "ARCH=$RACH2_ARCH" >"$work/$phase.build.log" 2>&1 || {
        cat "$work/$phase.build.log" >&2; exit 1;
    }
    cp "$tree/build$ARCH_SUFFIX/rach2/kernel" "$work/$phase.kernel"
    cksum "$source" "$work/$phase.kernel" >"$work/$phase.artifacts"
    build/tests/system/runutil group sh tests/system/init_refusal.sh "$work/$phase.kernel" \
        build/tests/system/harness build/tools/image/mkmbr "$work/$phase.row" \
        >"$work/$phase.out" 2>&1 &
    child=$!
    status=0
    wait "$child" || status=$?
    stop_child
    cat "$work/$phase.out"
    if [ "$expected" = pass ]; then
        [ "$status" -eq 0 ] || exit 1
    else
        [ "$status" -ne 0 ] || {
            echo "init-refusal-mutation: the kernel that takes any lookup failure for an absent init passed: $phase" >&2; exit 1;
        }
        grep -aq 'no /sbin/init in the root; falling back to the debug shell' \
            "$work/$phase.row.log" || exit 1
        grep -aq 'Kernel debug shell active' "$work/$phase.row.log" || exit 1
    fi
    echo "init-refusal-mutation: result: $phase status $status"
}

echo "init-refusal-mutation: logs: $work"
make -s build/tests/system/harness build/tests/system/runutil build/tools/image/mkmbr \
    build/tools/inspect/elfcheck "ARCH=$RACH2_ARCH" >"$work/prepare.log" 2>&1 || {
    cat "$work/prepare.log" >&2; exit 1;
}
# The private tree has what the kernel's own Makefile reads: rach2/ and mk/.
# Its link asks the directory above for the ELF checker, so that directory
# gets a copy of the checker and a Makefile with nothing to rebuild.
mkdir -p "$tree/build/tools/inspect"
cp -R rach2 mk "$tree/"
cp build/tools/inspect/elfcheck "$tree/build/tools/inspect/elfcheck"
printf 'build/tools/inspect/elfcheck: ;\n' >"$tree/Makefile"
cp "$source" "$work/kernel.saved"
run_phase fixed pass
changed=1
apply_fault "$source"
touch "$source"
run_phase fallthrough fail
if cmp -s "$work/fixed.kernel" "$work/fallthrough.kernel"; then
    echo 'init-refusal-mutation: the mutation did not change the kernel' >&2; exit 1
fi
restore
run_phase restored pass
cmp "$work/fixed.kernel" "$work/restored.kernel"
echo 'init-refusal-mutation: result: pass: the kernel as it is passed, the mutated kernel failed, the restored kernel passed and matched'
