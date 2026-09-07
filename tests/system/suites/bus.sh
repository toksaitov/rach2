#!/bin/sh
# bus.sh: the drivers on every position they serve.
#
# Four IDE drives of four sizes, one on each position of the two channels,
# each mounted and written; an AHCI disk and an IDE disk together, the root
# named on each in turn; a sparse disk of 130 GiB on the primary master
# with a partition past what the 28-bit commands address, written and read
# back.
#
# Usage: bus.sh KERNEL IMAGE HARNESS MKMBR DIR
set -u
KERNEL=$1; IMAGE=$2; HARNESS=$3; MKMBR=$4; DIR=$5
REPO=$(cd "$(dirname "$0")/../../.." && pwd)
fail() { echo "bus-test: FAIL: $1" >&2; exit 1; }
mkdir -p "$DIR"

# `fat_disk FILE MIB ID`: a disk of MIB MiB with one FAT32 partition from
# sector 2048 to the end, and the MBR id ID.
fat_disk() {
    rm -f "$1"
    dd if=/dev/zero of="$1" bs=1048576 count="$2" 2>/dev/null || fail "the disk $1"
    "$MKMBR" --lba --id "$3" "$1" 2048:$(($2 * 2048 - 2048)):0c >/dev/null || fail "mkmbr $1"
    mformat -i "$1"@@1048576 -F -T $(($2 * 2048 - 2048)) -v "DISK$3" :: || fail "mformat $1"
}
fat_disk "$DIR/pm.disk" 16 11110000
fat_disk "$DIR/ps.disk" 20 11110001
fat_disk "$DIR/sm.disk" 24 11110002
fat_disk "$DIR/ss.disk" 28 11110003

"$HARNESS" --kernel "$KERNEL" --initrd "$IMAGE" --qemu-arg=-cpu --qemu-arg=max \
    --drive "file=$DIR/pm.disk,format=raw,if=ide,index=0,media=disk" \
    --drive "file=$DIR/ps.disk,format=raw,if=ide,index=1,media=disk" \
    --drive "file=$DIR/sm.disk,format=raw,if=ide,index=2,media=disk" \
    --drive "file=$DIR/ss.disk,format=raw,if=ide,index=3,media=disk" \
    --append "rach2.rescue" --timeout 120 --steptmo 60 --log "$DIR/four.log" \
    --ok "BUSPROBE OK" --never "BUSPROBE FAIL" \
    "expect=Type 'help'" "send=exec /busprobe four" "expect=BUSPROBE" > /dev/null 2>&1 \
    || fail "the four IDE drives"
for d in pm:"primary master" ps:"primary slave" sm:"secondary master" ss:"secondary slave"; do
    mtype -i "$DIR/${d%%:*}.disk"@@1048576 ::/drive.txt 2>/dev/null | grep -q "^${d#*:}" \
        || fail "the file on the ${d#*:}"
done
grep -aq 'ata: secondary slave: 57344 sectors' "$DIR/four.log" || fail "the secondary slave's record"
echo "bus-test: four IDE drives on both channels, each mounted and written: OK"

# An AHCI disk and an IDE disk, each a KRFFS disk with its own id.
sh "$REPO/krffs/mkdisk.sh" "$REPO/krffs/manifests/disk-test.manifest" "$DIR/sata.disk" 64 48 \
    >/dev/null 2>&1 || fail "mkdisk sata"
sh "$REPO/krffs/mkdisk.sh" "$REPO/krffs/manifests/disk-test.manifest" "$DIR/ide.disk" 64 48 \
    >/dev/null 2>&1 || fail "mkdisk ide"
"$MKMBR" --lba --id aaaa0001 "$DIR/sata.disk" 2048:98304:7f >/dev/null || fail "mkmbr sata"
"$MKMBR" --lba --id bbbb0002 "$DIR/ide.disk" 2048:98304:7f >/dev/null || fail "mkmbr ide"
both() {
    "$HARNESS" --kernel "$KERNEL" --qemu-arg=-cpu --qemu-arg=max \
        --drive "file=$DIR/sata.disk,format=raw,if=none,id=drv0" \
        --qemu-arg=-device --qemu-arg=ich9-ahci,id=ahci0 \
        --qemu-arg=-device --qemu-arg=ide-hd,drive=drv0,bus=ahci0.0 \
        --drive "file=$DIR/ide.disk,format=raw,if=ide,index=0,media=disk" \
        --append "rach2.root=$1 rach2.rootfstype=krffs rach2.rescue" --timeout 90 --steptmo 60 \
        --log "$DIR/$2.log" --ok "$3" "expect=Type 'help'" "send=exec /blockprobe" \
        "expect=BLOCKPROBE OK" > /dev/null 2>&1 || fail "$2"
    echo "bus-test: $2: OK"
}
both "MBRID=bbbb0002,1" "the IDE disk as the root beside the AHCI one" \
    "root: MBRID=bbbb0002,1: /dev/disk1p1, KRFFS"
both "MBRID=aaaa0001,1" "the AHCI disk as the root beside the IDE one" \
    "root: MBRID=aaaa0001,1: /dev/disk0p1, KRFFS"

# The disk past 128 GiB: sparse, its one partition beyond sector 2^28.
BIG="$DIR/big.disk"
rm -f "$BIG"
dd if=/dev/zero of="$BIG" bs=1048576 count=1 seek=$((130 * 1024 - 1)) 2>/dev/null || fail "the big disk"
"$MKMBR" --lba --id 12345678 "$BIG" 268437504:65536:0c >/dev/null || fail "mkmbr big"
mformat -i "$BIG"@@$((268437504 * 512)) -F -T 65536 -v FARAWAY :: || fail "mformat big"
"$HARNESS" --kernel "$KERNEL" --initrd "$IMAGE" --qemu-arg=-cpu --qemu-arg=max \
    --drive "file=$BIG,format=raw,if=ide,index=0,media=disk" \
    --append "rach2.rescue" --timeout 120 --steptmo 60 --log "$DIR/big.log" \
    --ok "BUSPROBE OK" --never "BUSPROBE FAIL" \
    "expect=Type 'help'" "send=exec /busprobe big" "expect=BUSPROBE" > /dev/null 2>&1 \
    || fail "the 130 GiB disk"
mtype -i "$BIG"@@$((268437504 * 512)) ::/far.txt 2>/dev/null | grep -q "written past 128 GiB" \
    || fail "the file past 128 GiB"
grep -aq 'ata: primary master: 272629760 sectors, 133120 MiB, LBA48' "$DIR/big.log" \
    || fail "the big disk's record"
echo "bus-test: a partition past 2^28 sectors written through the 48-bit commands: OK"
rm -f "$BIG"

