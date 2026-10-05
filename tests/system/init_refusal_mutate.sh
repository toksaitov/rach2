#!/bin/sh
# Prove the init refusal row with kernels built from a private copy, for the
# architecture ARCH names, on its emulator.
#
# Usage: sh tests/system/init_refusal_mutate.sh
#
# tests/system/init_refusal.sh boots a root whose /sbin/init is a symbolic
# link to itself and requires the kernel to refuse it and halt. The kernel
# is built three times from a private copy of rach2/ and mk/, never from the
# main tree's sources or into its build/: as it is, where the row must pass;
# with the lookup's refusal put back to the old fall-through, which takes any
# failure for an absent init and opens the debug shell, where the row must
# fail and the log must show that shell; and restored, touched and rebuilt,
# where it must pass again with a kernel identical to the first. The trap
# stops the row's process group before it restores the private source.
set -eu
repo=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
cd "$repo"
RACH2_ARCH=${RACH2_ARCH:-${ARCH:-i686}}
REPO=$repo
. "$repo/mk/arch.sh"
RACH2_QEMU=$ARCH_QEMU
export RACH2_QEMU ARCH_BUILD
[ ! -d build/.run.lock ] || {
    echo 'init-refusal-mutation: another suite owns this tree' >&2; exit 1;
}
mkdir -p build
work=$(mktemp -d "$repo/build/init-refusal-proof.XXXXXX")
tree="$work/tree"
source="$tree/rach2/kernel.c"
child=
changed=

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

restore()
{
    cp "$work/kernel.saved" "$source"
    touch "$source"
    changed=
}

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

run_phase()
{
    phase=$1
    expected=$2
    # The object goes first: a source restored within the second its object
    # was built reads as up to date to a make that compares whole seconds.
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
            echo "init-refusal-mutation: the old fall-through passed: $phase" >&2; exit 1;
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
python3 - "$source" <<'PY'
from pathlib import Path
import sys

path = Path(sys.argv[1])
text = path.read_text()
old = "        if (rc == -ENOENT) {\n"
if text.count(old) != 1:
    raise SystemExit("init-refusal-mutation: the init lookup's test is not unique")
path.write_text(text.replace(old, "        if (rc != 0) {\n"))
PY
touch "$source"
run_phase fallthrough fail
if cmp -s "$work/fixed.kernel" "$work/fallthrough.kernel"; then
    echo 'init-refusal-mutation: the mutation did not change the kernel' >&2; exit 1
fi
restore
run_phase restored pass
cmp "$work/fixed.kernel" "$work/restored.kernel"
echo 'init-refusal-mutation: result: pass: the fixed kernel passed, the old fall-through failed, the restored kernel passed and matched'
