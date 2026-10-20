#!/bin/sh
# ext2_cost_rows.sh KERNEL OUT: run ext2probe's `cost` and `cost-append` rows
# on KERNEL with the test image, each beside a fresh copy of the ext2 fixture
# disk, on the architecture ARCH names, i686 by default.
#
# The rows count the disk commands of ext2's write-back batch on a `sync`
# mount (tests/ext2probe.c). A row passes when the probe passes, the host's
# e2fsck accepts the disk it left, and /cost-rewrite and /cost-append have
# the bytes the probe wrote. Both rows run whatever the first gives; the
# script fails if either does. OUT is a path prefix for the disks and logs.
# tests/proofs/ext2-batch-probe.json runs it.
set -eu
repo=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
cd "$repo"
[ "$#" -eq 2 ] || { echo 'usage: ext2_cost_rows.sh KERNEL OUT' >&2; exit 2; }
kernel=$1
out=$2
RACH2_ARCH=${RACH2_ARCH:-${ARCH:-i686}}
REPO=$repo
. "$repo/mk/arch.sh"
RACH2_QEMU=$ARCH_QEMU
export RACH2_QEMU
# shellcheck source=tools/setup/e2fsprogs_path.sh
. "$repo/tools/setup/e2fsprogs_path.sh"
mkdir -p "$(dirname -- "$out")"
sh tests/system/mkext2fixture.sh "$out.base.disk" build/tools/image/mkmbr \
    >"$out.fixture.log" 2>&1 || {
    cat "$out.fixture.log" >&2
    echo 'ext2-cost: fail: the fixture disk' >&2
    exit 1
}

# check_disk ROW: copy the ext2 volume out of ROW's disk, require e2fsck to
# accept it, and require the bytes of /cost-rewrite and /cost-append.
check_disk()
{
    dd if="$out.$1.disk" of="$out.$1.ext2" bs=1048576 skip=1 count=16 2>/dev/null
    if ! e2fsck -fn "$out.$1.ext2" >"$out.$1.fsck.log" 2>&1; then
        cat "$out.$1.fsck.log"
        echo "ext2-cost: fail: $1: e2fsck rejects the disk"
        return 1
    fi
    debugfs -R "dump /cost-rewrite $out.$1.rewrite" "$out.$1.ext2" >"$out.$1.debugfs.log" 2>&1
    debugfs -R "dump /cost-append $out.$1.append" "$out.$1.ext2" >>"$out.$1.debugfs.log" 2>&1
    python3 - "$out.$1" "$1" <<'PY'
from pathlib import Path
import sys

prefix, row = sys.argv[1:]
if Path(prefix + '.rewrite').read_bytes() != bytes([16]) * 4096:
    raise SystemExit('ext2-cost: fail: %s: wrong rewrite bytes on disk' % row)
if Path(prefix + '.append').read_bytes() != b''.join(bytes([n]) * 4096 for n in (64, 65, 66)):
    raise SystemExit('ext2-cost: fail: %s: wrong append bytes on disk' % row)
print('ext2-cost: %s: disk bytes and file lengths match' % row)
PY
}

failed=0
for mode in cost cost-append; do
    cp "$out.base.disk" "$out.$mode.disk"
    status=0
    build/tests/system/harness --kernel "$kernel" --initrd "$ARCH_BUILD/images/test.krffs" \
        --net hubport,id=net0,hubid=0 --qemu-arg=-cpu --qemu-arg=max \
        --drive "file=$out.$mode.disk,format=raw,if=ide,index=0,media=disk" \
        --append rach2.rescue --timeout 90 --steptmo 60 \
        --ok "EXT2PROBE OK ($mode)" --never 'EXT2PROBE FAIL' \
        --log "$out.$mode.serial.log" \
        "expect=Type 'help'" "send=exec /ext2probe $mode" \
        'expect=EXT2PROBE' 'expect=returning to kernel shell' \
        >"$out.$mode.log" 2>&1 || status=$?
    cat "$out.$mode.log" "$out.$mode.serial.log"
    if [ "$status" -ne 0 ]; then
        echo "ext2-cost: fail: $mode: the probe's row failed, status $status"
        failed=1
    elif check_disk "$mode"; then
        echo "ext2-cost: result: $mode: pass"
    else
        failed=1
    fi
done
exit "$failed"
