#!/bin/sh
# ext2_map_mutate.sh [--texts]: show that mapmusl's rows of a shared mapping
# of a file on ext2 catch three compiled kernel faults: a mapping released
# without writing its pages back, a read that does not write them back first,
# and a write not copied into them.
#
# mapmusl runs on an ext2 root through probe_roots.sh, on the kernel and the
# emulator of the architecture ARCH names. Each faulty kernel must fail its
# row with the row's words, and the restored kernel must match the first
# build and pass. --texts checks that each fault is once in its source.
set -eu

repo=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
cd "$repo"

# apply_fault FAULT: put FAULT into its source; with
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

faults = {
    'release': ('rach2/sys/mm/filemap.c',
                '        rc = dshare_write_back(m->ds, m, 0, m->ds->sh.npages);',
                '        rc = 0;'),
    'read': ('rach2/sys/syscall_fs.c',
             '    if (filemap_file_reading(ofd->vmnt, ofd->ino, at ? *at : ofd->offset, count) != 0) {',
             '    if (0) {'),
    'write': ('rach2/sys/syscall_fs.c',
              '        filemap_file_written(entry, pos, (uint32_t)written);',
              ''),
}
name, old, new = faults[sys.argv[1]]
path = Path(name)
text = path.read_text()
assert text.count(old) == 1, old
path.write_text(text.replace(old, new))
PY
}

faults='release read write'
if [ "${1:-}" = --texts ]; then
    for fault in $faults; do
        RACH2_FAULT_TEXTS=1 apply_fault "$fault" >/dev/null || exit 1
    done
    echo 'ext2-map-mutation: texts: every fault is once in its source'
    exit 0
fi

RACH2_ARCH=${RACH2_ARCH:-${ARCH:-i686}}
REPO=$repo
. "$repo/mk/arch.sh"
RACH2_QEMU=$ARCH_QEMU
export RACH2_QEMU ARCH_BUILD
tree=build$ARCH_SUFFIX
. "$repo/tests/system/runlock.sh"
runlock_hold "$repo" ext2-map-mutation "$repo/tests/system/ext2_map_mutate.sh" "$@"
mkdir -p build
work=$(mktemp -d "$repo/build/ext2-map-proof.XXXXXX")
for f in rach2/sys/mm/filemap.c rach2/sys/syscall_fs.c; do
    cp "$f" "$work/$(basename "$f").saved"
done
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

# Put back the saved copies of the two sources and touch them.
restore()
{
    for f in rach2/sys/mm/filemap.c rach2/sys/syscall_fs.c; do
        cp "$work/$(basename "$f").saved" "$f"
        touch "$f"
    done
    changed=
}

# Stop the boot and, when a fault is in a source, restore it and rebuild.
cleanup()
{
    stop_child
    if [ -n "$changed" ]; then
        restore
        make -s rach2 "ARCH=$RACH2_ARCH" >"$work/cleanup.build.log" 2>&1
    fi
}
trap cleanup EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

# run_phase PHASE EXPECTED: build the kernel and run mapmusl on an ext2 root
# under it. EXPECTED is `pass`, or the words the row prints when it fails.
run_phase()
{
    phase=$1
    expected=$2
    make -s rach2 "ARCH=$RACH2_ARCH" >"$work/$phase.build.log" 2>&1
    cp "$tree"/rach2/kernel "$work/$phase.kernel"
    status=0
    build/tests/system/runutil group sh tests/system/probe_roots.sh \
        "$work/$phase.kernel" "$work/$phase" ext2 musl/mapmusl >"$work/$phase.log" 2>&1 &
    child=$!
    wait "$child" || status=$?
    child=
    if [ "$expected" = pass ]; then
        [ "$status" -eq 0 ] || { cat "$work/$phase.log"; exit 1; }
    else
        [ "$status" -ne 0 ] || {
            echo "ext2-map-mutation: fault passed: $phase" >&2; exit 1;
        }
        grep -F "MAPMUSL FAIL ($expected" "$work/$phase.musl-mapmusl.serial.log" >/dev/null || {
            echo "ext2-map-mutation: $phase failed otherwise" >&2; exit 1;
        }
    fi
    echo "ext2-map-mutation: result: $phase status $status"
}

echo "ext2-map-mutation: logs: $work"
make -s test-image build/tests/system/harness build/tests/system/runutil \
    build/tools/image/mkmbr "ARCH=$RACH2_ARCH" >"$work/build.log" 2>&1
run_phase fixed pass
for fault in $faults; do
    changed=1
    apply_fault "$fault"
    case "$fault" in
        release) detail='the stores of a shared mapping of a file on the root did not reach it' ;;
        read) detail='a store in a shared mapping of a file on the root did not reach read' ;;
        write) detail='a write to a file on the root was not seen through its shared mapping' ;;
    esac
    run_phase "$fault" "$detail"
    if cmp -s "$work/fixed.kernel" "$work/$fault.kernel"; then
        echo 'ext2-map-mutation: kernel did not change' >&2; exit 1;
    fi
    restore
    run_phase "$fault-restored" pass
    cmp "$work/fixed.kernel" "$work/$fault-restored.kernel"
done
echo 'ext2-map-mutation: result: pass, compiled faults failed and restored kernels matched'
