#!/bin/sh
set -eu

repo=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
cd "$repo"
[ ! -d build/.run.lock ] || {
    echo 'install-stick-mutation: another suite owns this tree' >&2; exit 1;
}
mkdir -p build
work=$(mktemp -d "$repo/build/install-stick-proof.XXXXXX")
source="$work/images.mk"
cp mk/images.mk "$source"
cp "$source" "$work/images.saved"
child=

stop_child()
{
    if [ -n "$child" ]; then
        kill -TERM -- "-$child" 2>/dev/null || :
        wait "$child" 2>/dev/null || :
        while kill -0 -- "-$child" 2>/dev/null; do sleep 1; done
        child=
    fi
}

restore()
{
    cp "$work/images.saved" "$source"
    touch "$source"
}

cleanup()
{
    stop_child
    restore
}
trap cleanup EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

run_phase()
{
    phase=$1
    expected=$2
    message=$3
    rm -f "$stick"
    make -s -f "$work/Makefile" rach2-install-stick >"$work/$phase.build.log" 2>&1
    cat "$work/$phase.build.log"
    shasum -a 256 "$source" "$stick" >"$work/$phase.artifacts"
    cp "$stick" "$work/$phase.img"
    build/tests/system/runutil group sh tests/system/install_stick_test.sh \
        "$stick" "$disk" "$root" "$work/$phase" >"$work/$phase.log" 2>&1 &
    child=$!
    status=0
    wait "$child" || status=$?
    stop_child
    cat "$work/$phase.log"
    if [ "$status" -ne "$expected" ] || ! grep -Fq "$message" "$work/$phase.log"; then
        echo "install-stick-mutation: wrong result: $phase status $status" >&2; exit 1
    fi
    echo "install-stick-mutation: result: $phase status $status"
}

echo "install-stick-mutation: logs: $work"
make -s -j8 rach2 build/images/installer-root.ext2 rach2-recovery-image \
    build/tests/system/harness build/tests/system/runutil \
    build/tools/image/mkmbr build/tools/image/fatchains build/tools/image/grubembed \
    build/tools/image/stickcheck build/tests/host/mkexfatfixture >"$work/build.log" 2>&1
tag=
[ ! -f krffs/recovery_authorized_keys ] || tag=-with-key
iso="build/images/rach2-recovery$tag.iso"
make -s "$iso" >>"$work/build.log" 2>&1
cat "$work/build.log"
mkdir "$work/input"
root="$work/input/rach2-metal-disk.ext2"
disk="$work/input/rach2-sealed.disk"
stick="$work/rach2-install-stick$tag.img"
cp build/images/installer-root.ext2 "$root"
RACH2_DISK_BOOT_MB=64 RACH2_DISK_CLUSTER=8 RACH2_DISK_RECOVERY_MB=64 \
    RACH2_DISK_RECOVERY_KERNEL=build/rach2-recovery/kernel \
    RACH2_DISK_RECOVERY_IMAGE="build/images/rach2-recovery$tag.krffs" \
    RACH2_DISK_EXT2_MB=64 RACH2_DISK_EXT2_IMAGE="$root" \
    sh tools/image/mkdisk_grub.sh build/rach2/kernel "$disk" >"$work/disk.build.log" 2>&1
cat >"$work/Makefile" <<EOF
IMAGES_DIR = $work
RECOVERY_KEY_TAG = $tag
RECOVERY_ISO = $iso
SEALED_DISK = $disk
SEALED_EXT2 = $root
MKEXFATFIXTURE = build/tests/host/mkexfatfixture
MKMBR = build/tools/image/mkmbr
STICKCHECK = build/tools/image/stickcheck
FATCHAINS = build/tools/image/fatchains
GRUBEMBED = build/tools/image/grubembed
include $source
EOF
run_phase fixed 0 'install-stick-test: result: pass:'
for fault in root reserve; do
    python3 - "$source" "$fault" <<'PY'
from pathlib import Path
import sys

path = Path(sys.argv[1])
source = path.read_text()
old = '$(STICKCHECK) $(SEALED_EXT2) 256'
assert source.count(old) == 1
new = '$(STICKCHECK) "" 256' if sys.argv[2] == 'root' else '$(STICKCHECK) $(SEALED_EXT2) 4'
path.write_text(source.replace(old, new))
PY
    message='install-stick-test: missing root image'
    [ "$fault" != reserve ] || message='STICKFAIL: space'
    run_phase "$fault-mutated" 1 "$message"
    if cmp -s "$work/fixed.img" "$work/$fault-mutated.img"; then
        echo 'install-stick-mutation: image did not change' >&2; exit 1
    fi
    restore
    run_phase "$fault-restored" 0 'install-stick-test: result: pass:'
    cmp "$work/fixed.img" "$work/$fault-restored.img"
done
echo 'install-stick-mutation: result: pass: both faults failed; restored images matched'
