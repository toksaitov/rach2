#!/bin/sh
# bootorder.sh: a Rach2 disk that is not the first disk on its bus.
#
# The disk is built as the GRUB suites build it and attached on the second
# AHCI port behind a blank disk, with the firmware told to boot it. The
# kernel numbers it disk 1, so no stage of the boot may assume the first
# disk: the core image finds the boot partition by its label, the menu finds
# the kernel's partition by its serial, and the kernel finds the root and the
# boot partition by the identity the menu gives it. SeaBIOS boots one hard
# disk only, and the disk it boots is always its first; the order that
# changes here is the bus's, by which the kernel numbers its disks.
#
# Usage: bootorder.sh KERNEL HARNESS DISK
set -u

# The build tree of the architecture tests/system/suites.mk names.
ARCH_BUILD=${ARCH_BUILD:-build}

KERNEL=$1; HARNESS=$2; DISK=$3
REPO=$(cd "$(dirname "$0")/../../.." && pwd)
fail() { echo "bootorder-test: FAIL: $1" >&2; exit 1; }
BLANK="$DISK.blank"

# The root this disk boots is the grub-test tree as an ext2 partition, which
# mkdisk_grub.sh requires of every disk.
RACH2_DISK_TABLE=mbr \
RACH2_DISK_APPEND="rach2.disk=ahci" \
RACH2_DISK_RECOVERY_MB=8 \
RACH2_DISK_EXT2_MB=48 \
RACH2_DISK_EXT2_IMAGE="$REPO/$ARCH_BUILD/images/grub-test.ext2" \
    sh "$REPO/tools/image/mkdisk_grub.sh" "$KERNEL" "$DISK" > "$DISK.mkdisk.log" 2>&1 \
    || fail "mkdisk_grub"
rm -f "$BLANK"
dd if=/dev/zero of="$BLANK" bs=1048576 count=8 2>/dev/null || fail "the blank disk"

"$HARNESS" --bios --qemu-arg=-cpu --qemu-arg=max \
    --drive "file=$BLANK,format=raw,if=none,id=drv0" \
    --drive "file=$DISK,format=raw,if=none,id=drv1" \
    --qemu-arg=-device --qemu-arg=ich9-ahci,id=ahci0 \
    --qemu-arg=-device --qemu-arg=ide-hd,drive=drv0,bus=ahci0.0 \
    --qemu-arg=-device --qemu-arg=ide-hd,drive=drv1,bus=ahci0.1,bootindex=0 \
    --timeout 150 --steptmo 120 --log "$DISK.log" \
    --ok "root: MBRID=1acef00d,3: /dev/disk1p3, ext2" \
    "expect=login:" > /dev/null 2>&1 || fail "the boot behind another disk"
grep -aq 'disk: disk1: ahci 1: ' "$DISK.log" || fail "the Rach2 disk is not disk 1"
echo "bootorder-test: the disk behind another disk boots, and its root is found as disk 1: OK"
