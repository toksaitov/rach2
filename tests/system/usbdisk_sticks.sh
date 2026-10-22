#!/bin/sh
# usbdisk_sticks.sh DIR MKEXFATFIXTURE: make the sticks of the USB cases in
# DIR: stick-mbr.img, an MBR with a FAT32 and an exFAT volume; stick-gpt.img,
# a GUID table with the same two, its first used entry at index 6;
# stick-super.img, an exFAT volume from sector zero with no table;
# stick-hub.img, an MBR with the FAT32 volume alone, the stick the USB cases
# put behind a hub; and far.vol, a FAT32 volume of 32 MiB labelled FARAWAY,
# which the case of a stick past two terabytes places past sector 2^32.
#
# The FAT32 volume is 48 MiB at sector 2048, with a text file and a pattern
# file the hold boot reads in a loop. Forty-eight and not thirty-two because
# a FAT32 volume must have at least 65525 clusters, and mformat will write
# one with fewer; the kernel mounts such a volume, but BusyBox's blkid judges
# by the cluster count, takes it for FAT16 and reads the label from the
# FAT16 field, which is empty. The exFAT volume is the fixture's own 16 MiB,
# placed at sector 100352. The Makefile makes the sticks of the cases here.
set -u
[ "$#" -eq 2 ] || { echo 'usage: usbdisk_sticks.sh DIR MKEXFATFIXTURE' >&2; exit 2; }
DIR=$1
MKFIX=$2
REPO=$(cd "$(dirname "$0")/../.." && pwd)
MKMBR="$REPO/build/tools/image/mkmbr"
MKGPT="$REPO/build/tools/image/mkgpt"
MBR="$DIR/stick-mbr.img"
GPT="$DIR/stick-gpt.img"
SUPER="$DIR/stick-super.img"
HUB="$DIR/stick-hub.img"
FAR="$DIR/far.vol"
# fail REASON prints REASON and exits 1.
fail() { echo "usbdisk-sticks: FAIL: $1" >&2; exit 1; }

mkdir -p "$DIR" || fail "mkdir"
"$MKFIX" "$DIR/exfat.vol" > /dev/null || fail "the exFAT volume"
echo "hello from the stick" > "$DIR/hello.txt"
awk 'BEGIN { for (i = 0; i < 128; i++) for (j = 0; j < 128; j++) printf "%03d.", i }' \
    > "$DIR/pattern.bin" || fail "the pattern"

# fill_fat IMAGE: make the FAT32 volume at sector 2048 of IMAGE with its two
# files. mformat's -T limits the volume to the partition's 98304 sectors,
# since the driver refuses a volume that claims more than the partition has.
fill_fat() {
    mformat -i "$1"@@1048576 -F -T 98304 -v STICKFAT :: || fail "mformat $1"
    mcopy -i "$1"@@1048576 "$DIR/hello.txt" ::/hello.txt || fail "mcopy hello $1"
    mcopy -i "$1"@@1048576 "$DIR/pattern.bin" ::/pattern.bin || fail "mcopy pattern $1"
}

# fill_volumes IMAGE: make the FAT32 volume of IMAGE, and place and check the
# exFAT volume at sector 100352.
fill_volumes() {
    fill_fat "$1"
    dd if="$DIR/exfat.vol" of="$1" bs=512 seek=100352 conv=notrunc 2>/dev/null || fail "placing exFAT on $1"
    "$MKFIX" --check "$1" 100352 > /dev/null 2>&1 || fail "the exFAT volume on $1 does not check"
}

dd if=/dev/zero of="$MBR" bs=1048576 count=80 2>/dev/null || fail "the MBR image"
"$MKMBR" --lba "$MBR" 2048:98304:0c 100352:32768:07 > /dev/null || fail "mkmbr"
fill_volumes "$MBR"

dd if=/dev/zero of="$GPT" bs=1048576 count=80 2>/dev/null || fail "the GUID image"
"$MKGPT" "$GPT" 2048:98304:fat@6 100352:32768:data@9 > /dev/null || fail "mkgpt"
fill_volumes "$GPT"

cp "$DIR/exfat.vol" "$SUPER" || fail "the superfloppy"

dd if=/dev/zero of="$HUB" bs=1048576 count=80 2>/dev/null || fail "the hub stick's image"
"$MKMBR" --lba "$HUB" 2048:98304:0c > /dev/null || fail "mkmbr hub"
fill_fat "$HUB"

dd if=/dev/zero of="$FAR" bs=512 count=65536 2>/dev/null || fail "the far volume"
mformat -i "$FAR" -F -T 65536 -v FARAWAY :: || fail "mformat far"
