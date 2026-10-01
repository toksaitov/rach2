#!/bin/sh
# Prove mapmusl's rows of a shared mapping of a file on ext2 with rebuilt
# kernel faults: a mapping released without writing its pages back, a read
# that does not write them back first, and a write not copied into them.
# Each faulted kernel must fail its row on an ext2 root (probe_roots.sh), and
# the restored kernel must match the first and pass.
set -eu

repo=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
cd "$repo"
[ ! -d build/.run.lock ] || {
    echo 'ext2-map-mutation: another suite owns this tree' >&2; exit 1;
}
mkdir -p build
work=$(mktemp -d "$repo/build/ext2-map-proof.XXXXXX")
for f in rach2/sys/mm/filemap.c rach2/sys/syscall_fs.c; do
    cp "$f" "$work/$(basename "$f").saved"
done
child=
changed=

stop_child()
{
    if [ -n "$child" ]; then
        kill -TERM "-$child" 2>/dev/null || :
        wait "$child" 2>/dev/null || :
        child=
    fi
}

restore()
{
    for f in rach2/sys/mm/filemap.c rach2/sys/syscall_fs.c; do
        cp "$work/$(basename "$f").saved" "$f"
        touch "$f"
    done
    changed=
}

cleanup()
{
    stop_child
    if [ -n "$changed" ]; then
        restore
        make -s rach2 >"$work/cleanup.build.log" 2>&1
    fi
}
trap cleanup EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

# Run mapmusl on an ext2 root under the kernel built now; $2 is pass, or
# the words the row prints when it fails.
run_phase()
{
    phase=$1
    expected=$2
    make -s rach2 >"$work/$phase.build.log" 2>&1
    cp build/rach2/kernel "$work/$phase.kernel"
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
    build/tools/image/mkmbr >"$work/build.log" 2>&1
run_phase fixed pass
for fault in release read write; do
    changed=1
    python3 - "$fault" <<'PY'
from pathlib import Path
import sys

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
