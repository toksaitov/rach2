#!/bin/sh
# Prove that textbusy detects missing boot-root bind holder normalization.
set -eu
repo=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
cd "$repo"
. "$repo/tests/system/runlock.sh"
runlock_hold "$repo" bindhold-faults "$repo/tests/system/bindhold_mutate.sh" "$@"
fault()
{
    python3 - "$1" "$2" <<'PY'
from pathlib import Path
import sys

faults = {
    'descriptor': ('rach2/sys/syscall_fs.c',
                   'vfs_mount_base(ofd->vmnt) == vfs_root_mount()',
                   'ofd->vmnt == vfs_root_mount()'),
    'image': ('rach2/sys/mm/mm.c',
              'vfs_mount_base(m->exe_file->vmnt) == vfs_mount_base(mnt)',
              'm->exe_file->vmnt == mnt'),
    'copied': ('rach2/sys/syscall_proc.c',
               'if (type != 0100000u || !mmap_fault_readable(ref->mnt)) {',
               'if (type != 0100000u || !mmap_fault_readable(ref->mnt) ||\n'
               '        vfs_mount_base(ref->mnt) == vfs_root_mount()) {'),
}
path, old, new = faults[sys.argv[1]]
p = Path(path)
s = p.read_text()
assert s.count(old) == 1, (path, old)
if sys.argv[2] == 'write':
    p.write_text(s.replace(old, new))
PY
}
if [ "${1:-}" = --texts ]; then
    fault descriptor check
    fault image check
    fault copied check
    echo 'bindhold-faults: checked: normalization and copied-segment texts'
    exit
fi
[ "$#" -eq 0 ] || exit 2
work=
child=
changed=
finished=
restore_sources()
{
    cp "$work/syscall_fs.c" rach2/sys/syscall_fs.c
    cp "$work/mm.c" rach2/sys/mm/mm.c
    cp "$work/syscall_proc.c" rach2/sys/syscall_proc.c
    touch rach2/sys/syscall_fs.c rach2/sys/mm/mm.c rach2/sys/syscall_proc.c
}
restore()
{
    if [ -n "$child" ]; then
        kill -TERM "-$child" 2>/dev/null || :
        wait "$child" 2>/dev/null || :
        child=
    fi
    if [ -n "$changed" ]; then
        restore_sources
        changed=
        make -C rach2 ARCH=x86_64
    fi
    [ -z "$work" ] || rm -rf "$work"
}
trap 'restore; [ -n "$finished" ]' EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM
work=$(mktemp -d "${TMPDIR:-/tmp}/rach2-bindhold-faults.XXXXXX")
cp rach2/sys/syscall_fs.c "$work/syscall_fs.c"
cp rach2/sys/mm/mm.c "$work/mm.c"
cp rach2/sys/syscall_proc.c "$work/syscall_proc.c"
ARCH=x86_64 RACH2_WORKERS=4 tests/run textbusy
echo 'bindhold-faults: passed: unmodified boot-root bind holders'
changed=1
for kind in descriptor image; do
    restore_sources
    if [ "$kind" = image ]; then
        fault copied write
        ARCH=x86_64 RACH2_WORKERS=4 tests/run textbusy
        echo 'bindhold-faults: passed: copied segments with executable holders'
    fi
    fault "$kind" write
    setsid env ARCH=x86_64 RACH2_WORKERS=4 tests/run textbusy > "$work/$kind.log" 2>&1 &
    child=$!
    rc=0
    wait "$child" || rc=$?
    child=
    cat "$work/$kind.log"
    [ "$rc" -ne 0 ]
    grep -q 'BINDHOLDMUSL FAIL' build/x86_64/images/textbusy-test.log
    echo "bindhold-faults: caught: $kind normalization removed"
done
restore
work=
ARCH=x86_64 RACH2_WORKERS=4 tests/run textbusy
echo 'bindhold-faults: passed: restored boot-root bind normalization'
finished=1
