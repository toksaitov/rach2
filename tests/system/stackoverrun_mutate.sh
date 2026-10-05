#!/bin/sh
# Prove the memory suite's two stack-overrun rows with faults compiled into
# private kernels.
#
# Usage: sh tests/system/stackoverrun_mutate.sh
#
# The rows (tests/system/stackoverrun_row.sh) boot `rach2.stackoverrun=fork`
# and `rach2.stackoverrun=boot`, which descend a kernel stack until it runs
# out, and require the record that names the thread whose stack ran out and
# the panic `a kernel stack ran out` (entry 248 of milestone 50). A private
# copy of rach2/ and mk/ is built as it is, where both rows must pass; then
# with each of three faults in turn, where each row the fault breaks must
# fail with `the overrun was not reported`, after the descent's announcement,
# and the other row must pass: `guard-mapped`, kalloc_guarded mapping the
# guard page below a stack like the pages above it, breaks the fork row;
# `boot-guard-mapped`, guard_boot_stack leaving the page below the boot stack
# mapped, breaks the boot row; and `interrupt-gate`, vector 8 given an
# interrupt gate that runs on the stack in use, breaks both, since the double
# fault then faults a third time and QEMU, started with -no-reboot, ends. A
# fault that does not change the kernel stops the proof. After each fault the
# copy is restored and rebuilt, both rows must pass, and the kernel must be
# the first one byte for byte.
#
# A fault that leaves the guard page mapped leaves the rows no address to
# name the thread by but a mapped one, where nothing faults, so no layout of
# the heap can let the fork row pass under it; the thread's record lies above
# its stack, where the descent does not reach it. The proof needs the test
# image `tests/run memory` boots, test.krffs under the images of the
# architecture ARCH names, of which it boots a copy, and its kernels are built
# for that architecture. It changes nothing in the tree; its kernels, consoles
# and logs stay under the build/stackoverrun-proof.* directory it prints.
set -eu
repo=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
cd "$repo"
RACH2_ARCH=${RACH2_ARCH:-${ARCH:-i686}}
REPO=$repo
. "$repo/mk/arch.sh"
RACH2_QEMU=$ARCH_QEMU
export RACH2_QEMU ARCH_BUILD
tree=build$ARCH_SUFFIX
[ ! -d build/.run.lock ] || {
    echo 'stackoverrun-proof: another suite owns this tree' >&2; exit 1;
}
[ -f "$tree"/images/test.krffs ] || {
    echo "stackoverrun-proof: no $tree/images/test.krffs; make test-image first" >&2; exit 1;
}
make -s build/tools/inspect/elfcheck build/tests/system/harness build/tests/system/runutil "ARCH=$RACH2_ARCH"
mkdir -p build
work=$(mktemp -d "$repo/build/stackoverrun-proof.XXXXXX")
cp -R rach2 mk "$work/"
mkdir -p "$work/build/tools/inspect"
cp build/tools/inspect/elfcheck "$work/build/tools/inspect/"
cat >"$work/Makefile" <<'MAKE'
build/tools/inspect/elfcheck:
	@test -x "$@"
MAKE
cp "$tree"/images/test.krffs "$work/test.krffs"
child=
file=

stop_child()
{
    if [ -n "$child" ]; then
        build/tests/system/runutil killgroup "$child" 15 2>/dev/null || :
        wait "$child" 2>/dev/null || :
        child=
    fi
}
trap stop_child EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

# run_row PHASE WHERE: the row for the stack WHERE on PHASE's kernel, in a
# process group of its own, with its line in $row and its status in $status.
run_row()
{
    # shellcheck disable=SC2016 # the dollars are the inner shell's
    build/tests/system/runutil group sh -c '. "$1"; shift; stackoverrun_row "$@"' row \
        tests/system/stackoverrun_row.sh build/tests/system/harness "$work/$1.kernel" \
        "$work/test.krffs" "$2" "$work/$1-$2.console.log" >"$work/$1-$2.row" 2>&1 &
    child=$!
    status=0
    wait "$child" || status=$?
    stop_child
    row=$(cat "$work/$1-$2.row")
    echo "$row"
}

# judge PHASE WHERE EXPECT: run the row, which with EXPECT `pass` must pass
# and with `fail` must fail with the words of an overrun not reported, which
# it says only once the descent was announced.
judge()
{
    run_row "$1" "$2"
    case $3:$status:$row in
        "pass:0:stackoverrun: $2: pass") ;;
        "fail:1:stackoverrun: $2: fail: the overrun was not reported"*) ;;
        *) echo "stackoverrun-proof: $1: the $2 row did not $3 as it must" >&2; exit 1 ;;
    esac
    echo "stackoverrun-proof: result: $1: the $2 row: $3"
}

# build PHASE: build the copy as it stands and keep its kernel. The kernel
# and the object of the file a fault changes are removed first, since a make
# that keeps whole seconds would take either for current when the source
# changed within the second it was made.
build()
{
    rm -f "$work/build$ARCH_SUFFIX/rach2/kernel"
    if [ -n "$file" ]; then
        rm -f "$work/build/${file%.c}.o"
    fi
    if ! make -C "$work/rach2" "ARCH=$RACH2_ARCH" >"$work/$1.build.log" 2>&1; then
        cat "$work/$1.build.log" >&2
        exit 1
    fi
    cp "$work/build$ARCH_SUFFIX/rach2/kernel" "$work/$1.kernel"
}

# fault NAME: put the fault NAME into the private copy, the file it changes
# saved first; the lines it changes must occur once there.
fault()
{
    case $1 in
        guard-mapped) file=rach2/sys/mm/kheap.c ;;
        boot-guard-mapped) file=rach2/kernel.c ;;
        interrupt-gate) file=rach2/sys/arch/i686/idt.c ;;
    esac
    cp "$work/$file" "$work/saved"
    python3 - "$work/$file" "$1" <<'PY'
from pathlib import Path
import sys

path = Path(sys.argv[1])
text = path.read_text()
old, new = {
    'guard-mapped': (
        '    if (guard >= heap_pages || map_run(guard + 1, count) != 0) {\n',
        '    if (guard >= heap_pages || map_run(guard, count + 1) != 0) {\n'),
    'boot-guard-mapped': (
        '    unmap_kernel_frame((uintptr_t)stack_guard);\n',
        ''),
    'interrupt-gate': (
        '    _idt_set_desciptor(8, 0, DOUBLE_FAULT_TSS_SELECTOR, 0x85);\n',
        '    _idt_set_desciptor(8, (uintptr_t)&isr10, 0x08, 0x8E);\n'),
}[sys.argv[2]]
if text.count(old) != 1:
    raise SystemExit('stackoverrun-proof: the lines to change are not unique: ' + sys.argv[2])
path.write_text(text.replace(old, new))
PY
}

restore()
{
    cp "$work/saved" "$work/$file"
    touch "$work/$file"
}

echo "stackoverrun-proof: logs: $work"
build fixed
judge fixed fork pass
judge fixed boot pass
for name in guard-mapped boot-guard-mapped interrupt-gate; do
    fault "$name"
    build "$name"
    if cmp -s "$work/fixed.kernel" "$work/$name.kernel"; then
        echo "stackoverrun-proof: the $name fault did not change the kernel" >&2
        exit 1
    fi
    case $name in
        guard-mapped) fork=fail boot=pass ;;
        boot-guard-mapped) fork=pass boot=fail ;;
        interrupt-gate) fork=fail boot=fail ;;
    esac
    judge "$name" fork "$fork"
    judge "$name" boot "$boot"
    restore
    build "$name-restored"
    cmp "$work/fixed.kernel" "$work/$name-restored.kernel"
    judge "$name-restored" fork pass
    judge "$name-restored" boot pass
done
echo 'stackoverrun-proof: result: pass: each fault failed its rows; the restored kernels match the first and pass'
