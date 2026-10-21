#!/bin/sh
# metal_sticks.sh DIR: make the sticks of the metal case that boots the
# metal image, tests/cases/machine.metal.json, in DIR: stick.img, the stick
# the acceptance script writes, and rstick1.img and rstick2.img, two
# recovery sticks it must refuse.
#
# stick.img is 80 MiB with an MBR: a FAT32 volume labelled ACCEPT, 48 MiB
# at sector 2048, which the script finds by that label and writes, and a
# KRFFS volume of 16 MiB at sector 100352, which it mounts through the
# mass-storage driver and writes, and which the case then checks with the
# host's tools. Each recovery stick is 20 MiB, with the ISO half's entry,
# type 17, and a FAT volume labelled ACCEPT like the real stick's, so that
# only its table tells it from the stick to write; the ISO half is a few
# empty sectors, since the table is what the script reads.
set -u
[ "$#" -eq 1 ] || { echo 'usage: metal_sticks.sh DIR' >&2; exit 2; }
DIR=$1
REPO=$(cd "$(dirname "$0")/../.." && pwd)
MKMBR="$REPO/build/tools/image/mkmbr"
# fail REASON prints REASON and exits 1.
fail() { echo "metal-sticks: FAIL: $1" >&2; exit 1; }

mkdir -p "$DIR/volume" || fail "mkdir"
DIR=$(cd "$DIR" && pwd) || fail "cd $DIR"
STICK="$DIR/stick.img"
dd if=/dev/zero of="$STICK" bs=1048576 count=80 2>/dev/null || fail "the stick image"
"$MKMBR" --lba "$STICK" 2048:98304:0c 100352:32768:83 > /dev/null || fail "mkmbr for the stick"
mformat -i "$STICK"@@1048576 -F -T 98304 -v ACCEPT :: || fail "mformat for the stick"

# The KRFFS volume has the files of the loop device suite's image, written
# here into a directory of the sticks' own and named by a manifest of their
# own, so that no other build writes them meanwhile.
printf 'hello from inside an image' > "$DIR/volume/hello.txt" || fail "the volume's text"
dd if=/dev/zero of="$DIR/volume/inner.bin" bs=512 count=8 2>/dev/null || fail "the volume's sectors"
printf 'file 0644 /hello.txt %s\nfile 0644 /inner.bin %s\n' \
    "$DIR/volume/hello.txt" "$DIR/volume/inner.bin" > "$DIR/loopimg-test.manifest" || fail "the manifest"
sh "$REPO/krffs/mkimage.sh" "$DIR/loopimg-test.manifest" "$DIR/volume.krffs" 16 > /dev/null ||
    fail "the KRFFS volume"
dd if="$DIR/volume.krffs" of="$STICK" bs=512 seek=100352 conv=notrunc 2>/dev/null ||
    fail "putting the KRFFS volume on the stick"

for r in 1 2; do
    RSTICK="$DIR/rstick$r.img"
    dd if=/dev/zero of="$RSTICK" bs=1048576 count=20 2>/dev/null || fail "the recovery stick $r"
    "$MKMBR" --lba "$RSTICK" 1:2047:17 2048:32768:0c > /dev/null || fail "mkmbr for the recovery stick $r"
    mformat -i "$RSTICK"@@1048576 -F -T 32768 -v ACCEPT :: || fail "mformat for the recovery stick $r"
done
