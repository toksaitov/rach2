#!/bin/sh
# disks.sh: two disks of different sizes on one AHCI controller, both listed
# by /proc/disk, both with nodes, and a partition of each mounted and written.
#
# Usage: disks.sh KERNEL IMAGE HARNESS DISK0 DISK1
set -u
KERNEL=$1; IMAGE=$2; HARNESS=$3; DISK0=$4; DISK1=$5
REPO=$(cd "$(dirname "$0")/../../.." && pwd)
fail() { echo "disks-test: FAIL: $1" >&2; exit 1; }

# Disk 0: 64 MiB with one KRFFS partition, as the disk suites build it.
sh "$REPO/krffs/mkdisk.sh" "$REPO/krffs/manifests/disk-test.manifest" "$DISK0" 64 48 >/dev/null 2>&1 \
    || fail "mkdisk"
# Disk 1: 20 MiB with one FAT32 partition from sector 2048.
rm -f "$DISK1"
dd if=/dev/zero of="$DISK1" bs=1048576 count=20 2>/dev/null || fail "the second disk"
"$REPO/build/tools/image/mkmbr" --lba "$DISK1" 2048:36864:0c >/dev/null || fail "mkmbr"
mformat -i "$DISK1"@@1048576 -F -T 36864 -v DISKONE :: || fail "mformat"

"$HARNESS" --kernel "$KERNEL" --initrd "$IMAGE" \
    --qemu-arg=-cpu --qemu-arg=max \
    --drive "file=$DISK0,format=raw,if=none,id=drv0" \
    --drive "file=$DISK1,format=raw,if=none,id=drv1" \
    --qemu-arg=-device --qemu-arg=ich9-ahci,id=ahci0 \
    --qemu-arg=-device --qemu-arg=ide-hd,drive=drv0,bus=ahci0.0 \
    --qemu-arg=-device --qemu-arg=ide-hd,drive=drv1,bus=ahci0.1 \
    --append "rach2.disk=ahci rach2.diskdiag rach2.rescue" \
    --timeout 90 --steptmo 60 --ok "DISKSPROBE OK" --never "DISKSPROBE FAIL" \
    --log "$DISK1.log" \
    "expect=Type 'help'" "send=exec /disksprobe" "expect=DISKSPROBE" > /dev/null 2>&1 \
    || fail "the disksprobe boot"
echo "disks-test: two AHCI disks listed, their nodes present, a partition of each written: OK"

mtype -i "$DISK1"@@1048576 ::/one.txt 2>/dev/null | grep -q "disk one says hello" \
    || fail "the file written through /dev/disk1p1 is not on the second disk"
echo "disks-test: the second disk's file is on the second disk: OK"
