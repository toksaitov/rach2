#!/usr/bin/env sh
#
# mount.sh: the block path under the mount milestone, phase 2's boots.
#
# The disk is the KRFFS test disk with a FAT32 partition added after it, so
# the partition nodes have two real entries to publish. The lag boots prove
# the sleeping path: a reader that sleeps while another process runs, and a
# device that never answers in time being quarantined and freed late.
#
# Usage: mount.sh KERNEL IMAGE HARNESS DISK
set -u
KERNEL=$1; IMAGE=$2; HARNESS=$3; DISK=$4
REPO=$(cd "$(dirname "$0")/../../.." && pwd)
fail() { echo "mount-test: FAIL: $1" >&2; exit 1; }

# The two-partition disk: 48 MiB KRFFS at LBA 2048, 14 MiB FAT32 at LBA 100352.
sh "$REPO/krffs/mkdisk.sh" "$REPO/krffs/manifests/disk-test.manifest" "$DISK" 64 48 >/dev/null 2>&1 \
    || fail "mkdisk"
"$REPO/build/tools/image/mkmbr" --lba "$DISK" 2048:98304:7f 100352:28672:0c >/dev/null || fail "mkmbr"
mformat -i "$DISK"@@51380224 -F -v M36FAT :: || fail "mformat"
echo "hello from the mount suite" > "${DISK}.hello"
mcopy -i "$DISK"@@51380224 "${DISK}.hello" ::/hello.txt || fail "mcopy"

run() {
    "$HARNESS" --kernel "$KERNEL" --initrd "$IMAGE" \
        --qemu-arg=-cpu --qemu-arg=max \
        --drive "file=$DISK,format=raw,if=ide,index=0,media=disk" "$@"
}

# ---- 1: the partition nodes appear with the sizes the MBR gives, and a read
#         through one, on the lag, sleeps the reader while its sibling runs ----
if run --append "rach2.blocklag=3 rach2.rescue" \
    --timeout 90 --ok "LAGPROBE OK" --never "LAGPROBE FAIL" \
    "expect=Type 'help'" "send=exec /lagprobe sleep" \
    "expect=LAGPROBE OK" > /dev/null 2>&1; then
    echo "mount-test: the lag reader sleeps while its sibling counts: OK"
else
    fail "the lag sleep boot"
fi

# ---- 2: a device that never answers in time: the deadline gives EIO, the
#         late completion frees the quarantined slot, and the pool serves the
#         next request ----
if run --append "rach2.blocklag=700 rach2.rescue" \
    --timeout 120 --steptmo 60 --ok "LAGPROBE OK" --never "LAGPROBE FAIL" \
    "expect=Type 'help'" "send=exec /lagprobe deadline" \
    "expect=quarantined" "expect=late completion" \
    "expect=LAGPROBE OK" > /dev/null 2>&1; then
    echo "mount-test: the deadline quarantines and the late answer frees: OK"
else
    fail "the deadline boot"
fi

# ---- 3: the root mounts through the real device while the partitions are on
#         the lag: the boot reaches its shell at all is the proof ----
if run --append "rach2.blocklag=3 rach2.fsdisk rach2.rescue" \
    --timeout 90 --ok "Type 'help'" > /dev/null 2>&1; then
    echo "mount-test: the root is on the real device beside the lag: OK"
else
    fail "the root-beside-lag boot"
fi

# ---- 4: a write through the lag: the kernel copies the caller's bytes into
#         the request page before the device sleeps, and the sector reads back
#         through the same sleeping path. /dev/bootpart is the one writable
#         raw window, so the disk is the boot-fixture one ----
BOOTDISK="${DISK%.disk}-boot.disk"
sh "$REPO/krffs/mkdisk.sh" "$REPO/krffs/manifests/disk-test.manifest" "$BOOTDISK" 64 48 >/dev/null 2>&1 \
    || fail "mkdisk (boot fixture)"
"$REPO/build/tests/system/mkfixture" boot "$BOOTDISK" >/dev/null || fail "mkfixture boot"
if "$HARNESS" --kernel "$KERNEL" --initrd "$IMAGE" \
    --qemu-arg=-cpu --qemu-arg=max \
    --drive "file=$BOOTDISK,format=raw,if=ide,index=0,media=disk" \
    --append "rach2.blocklag=3 rach2.rescue" \
    --timeout 90 --ok "LAGPROBE OK" --never "LAGPROBE FAIL" \
    "expect=Type 'help'" "send=exec /lagprobe write" \
    "expect=LAGPROBE OK" > /dev/null 2>&1; then
    echo "mount-test: a sector written through the lag reads back: OK"
else
    fail "the bootpart write boot"
fi

echo "mount-test: PASS"
