#!/bin/sh
# Real AHCI transfers and root faults. Controlled host models check request
# deadlines, independent ports, pool waits and held flush completions.
#
# Usage: queue.sh KERNEL IMAGE HARNESS MKMBR DIR
set -u
KERNEL=$1; IMAGE=$2; HARNESS=$3; MKMBR=$4; DIR=$5
REPO=$(cd "$(dirname "$0")/../../.." && pwd)
fail() { echo "queue-test: FAIL: $1" >&2; exit 1; }
mkdir -p "$DIR"

# fat_disk FILE: an 8 MiB disk with one FAT32 partition from sector 2048.
fat_disk() {
    rm -f "$1"
    dd if=/dev/zero of="$1" bs=1048576 count=8 2>/dev/null || fail "the disk $1"
    "$MKMBR" --lba "$1" 2048:14336:0c >/dev/null || fail "mkmbr $1"
    mformat -i "$1"@@1048576 -F -T 14336 :: || fail "mformat $1"
}
fat_disk "$DIR/slow.disk"
fat_disk "$DIR/fast.disk"
"$HARNESS" --kernel "$KERNEL" --initrd "$IMAGE" --qemu-arg=-cpu --qemu-arg=max \
    --drive "file=$DIR/slow.disk,format=raw,if=none,id=drv0" \
    --drive "file=$DIR/fast.disk,format=raw,if=none,id=drv1" \
    --qemu-arg=-device --qemu-arg=ich9-ahci,id=ahci0 \
    --qemu-arg=-device --qemu-arg=ide-hd,drive=drv0,bus=ahci0.0 \
    --qemu-arg=-device --qemu-arg=ide-hd,drive=drv1,bus=ahci0.1 \
    --append "rach2.rescue" --timeout 120 --steptmo 60 --log "$DIR/slow.log" \
    --ok "AHCIPROBE OK" --never "AHCIPROBE FAIL" \
    "expect=Type 'help'" "send=exec /ahciprobe read" "expect=AHCIPROBE" > /dev/null 2>&1 \
    || fail "the two disks"
grep -aq 'ahci: interrupts: messages on vector' "$DIR/slow.log" || fail "the driver did not switch to its interrupts"
echo "queue-test: both disks complete reads through the interrupt-driven driver: $(grep -a 'AHCIPROBE READ' "$DIR/slow.log" | tr -d '\r' | tr '\n' ' ')OK"

# The fallback root on AHCI, with a FAT32 second partition the raw reader reads.
sh "$REPO/krffs/mkdisk.sh" "$REPO/krffs/manifests/disk-test.manifest" "$DIR/root.disk" 64 48 \
    >/dev/null 2>&1 || fail "mkdisk"
"$MKMBR" --lba "$DIR/root.disk" 2048:98304:7f 100352:28672:0c >/dev/null || fail "mkmbr root"
mformat -i "$DIR/root.disk"@@51380224 -F -T 28672 :: || fail "mformat root"
"$HARNESS" --kernel "$KERNEL" --qemu-arg=-cpu --qemu-arg=max \
    --drive "file=$DIR/root.disk,format=raw,if=none,id=drv0" \
    --qemu-arg=-device --qemu-arg=ich9-ahci,id=ahci0 \
    --qemu-arg=-device --qemu-arg=ide-hd,drive=drv0,bus=ahci0.0 \
    --append "rach2.root=disk rach2.rescue" --timeout 180 --steptmo 120 --log "$DIR/fault.log" \
    --ok "AHCIPROBE OK" --never "AHCIPROBE FAIL" \
    "expect=Type 'help'" "send=exec /ahciprobe fault" "expect=AHCIPROBE" > /dev/null 2>&1 \
    || fail "the fallback root beside the queue"
grep -aq 'BLOCKPROBE OK' "$DIR/fault.log" || fail "blockprobe on the root"
echo "queue-test: the root's faults share the port with the queue: $(grep -a 'AHCIPROBE READ' "$DIR/fault.log" | tr -d '\r' | tr '\n' ' ')OK"

# The real write, flush and ext2 batch path, with every byte checked after
# sync. Its deadline transitions are exercised by request_hosttest instead
# of racing a throttle or the host's fdatasync against guest ticks.
rm -f "$DIR/late.vol"; : > "$DIR/late.vol"
E2FSPROGS_FAKE_TIME=1 mke2fs -q -F -t ext2 -b 4096 -I 128 \
    -O sparse_super,large_file,filetype,^dir_index,resize_inode,^ext_attr,^has_journal,^extent,^64bit,^flex_bg,^metadata_csum,^huge_file,^dir_nlink,^extra_isize \
    "$DIR/late.vol" 6M > /dev/null 2>&1 || fail "the late volume"
{
    dd if=/dev/zero bs=1048576 count=1 2>/dev/null
    cat "$DIR/late.vol"
    dd if=/dev/zero bs=1048576 count=1 2>/dev/null
} > "$DIR/late.disk"
"$MKMBR" --lba "$DIR/late.disk" 2048:12288:83 > /dev/null || fail "the late disk's table"
"$HARNESS" --kernel "$KERNEL" --initrd "$IMAGE" --qemu-arg=-cpu --qemu-arg=max \
    --drive "file=$DIR/late.disk,format=raw,if=none,id=drv0" \
    --qemu-arg=-device --qemu-arg=ich9-ahci,id=ahci0 \
    --qemu-arg=-device --qemu-arg=ide-hd,drive=drv0,bus=ahci0.0 \
    --append "rach2.rescue" --timeout 240 --steptmo 180 --log "$DIR/late.log" \
    --ok "AHCIPROBE OK" --never "AHCIPROBE FAIL" \
    "expect=Type 'help'" "send=exec /ahciprobe write" "expect=AHCIPROBE" > /dev/null 2>&1 \
    || fail "the ext2 batch: $(grep -a 'AHCIPROBE FAIL\|read-only\|stalled' "$DIR/late.log" | head -2 | tr -d '\r' | tr '\n' ' ')"
if grep -aq 'vfs: read-only' "$DIR/late.log"; then fail "the mount went read-only"; fi
echo "queue-test: the ext2 batch reads back and the mount keeps writing: OK"
