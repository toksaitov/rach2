#!/bin/sh
# loopdev_disks.sh KEPT DISKS: make the images and the disks of the loop
# device cases, tests/cases/machine.loopdev.json.
#
# In KEPT go the three images, each of 4 MiB with hello.txt, whose bytes the
# probe knows, and inner.bin, a file of whole sectors: fat.img, a FAT32
# volume; ext2.img, an ext2 one; and krffs.img, a KRFFS one. The native cases
# of tests/native/probes.cases take them from there (tests/native/maps).
#
# In DISKS go the three disks. disk.img is 32 MiB with a FAT32 volume of
# 30 MiB at sector 2048 that holds the three images, a second copy of the
# FAT32 image as odd2.img, and odd.bin, a file of 100 bytes, which no loop
# device may stand on. ext2back.img has an ext2 volume of 16 MiB at sector
# 2048 that holds the KRFFS image. stick.img is laid out as disk.img and
# holds the FAT32 image alone.
#
# The ext2 volumes have the features the kernel writes, so that each mounts
# read-write, and E2FSPROGS_FAKE_TIME fixes their times, so that mke2fs
# writes the same bytes each time. Each file is written under a temporary
# name and renamed once all of them are made.
set -u
[ "$#" -eq 2 ] || { echo 'usage: loopdev_disks.sh KEPT DISKS' >&2; exit 2; }
KEPT=$1
DISKS=$2
REPO=$(cd "$(dirname "$0")/../.." && pwd)
MKMBR="$REPO/build/tools/image/mkmbr"
PART=2048
SECTORS=61440
FEATURES=sparse_super,large_file,filetype,^dir_index,resize_inode,^ext_attr,^has_journal,^extent,^64bit,^flex_bg,^metadata_csum,^huge_file,^dir_nlink,^extra_isize
# shellcheck source=tools/setup/e2fsprogs_path.sh
. "$REPO/tools/setup/e2fsprogs_path.sh"

work=
trap '[ -z "$work" ] || rm -rf "$work"' EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

# fail REASON prints REASON and exits 1.
fail() { echo "loopdev-disks: FAIL: $1" >&2; exit 1; }

mkdir -p "$KEPT" "$DISKS" || fail "mkdir"
work=$(mktemp -d "$DISKS/rach2-loopdev.XXXXXX") || fail "mktemp"

# mkext2 TREE IMAGE SIZE makes an ext2 volume of SIZE with TREE's files.
mkext2() {
    : > "$2" || return 1
    E2FSPROGS_FAKE_TIME=1 mke2fs -q -F -t ext2 -b 4096 -I 128 -O "$FEATURES" \
        -d "$1" "$2" "$3" > /dev/null 2>&1
}

# fatdisk DISK LABEL makes DISK a disk of 32 MiB whose one partition, at
# sector 2048, is an empty FAT32 volume labelled LABEL.
fatdisk() {
    dd if=/dev/zero of="$1" bs=1048576 count=32 2>/dev/null || return 1
    "$MKMBR" --lba "$1" "$PART:$SECTORS:0c" > /dev/null || return 1
    mformat -i "$1"@@$((PART * 512)) -F -T $SECTORS -v "$2" ::
}

# The two files of each image.
printf 'hello from inside an image' > "$work/hello.txt" || fail "hello.txt"
dd if=/dev/zero of="$work/inner.bin" bs=512 count=8 2>/dev/null || fail "inner.bin"

# The FAT32 image.
dd if=/dev/zero of="$work/fat.img" bs=1048576 count=4 2>/dev/null || fail "the FAT32 image"
mformat -i "$work/fat.img" -F -T 8192 -v INNERFAT :: || fail "mformat for the FAT32 image"
mcopy -i "$work/fat.img" "$work/hello.txt" ::/hello.txt || fail "mcopy hello.txt"
mcopy -i "$work/fat.img" "$work/inner.bin" ::/inner.bin || fail "mcopy inner.bin"

# The ext2 image.
mkdir "$work/tree" || fail "the ext2 image's tree"
cp "$work/hello.txt" "$work/inner.bin" "$work/tree/" || fail "the ext2 image's files"
mkext2 "$work/tree" "$work/ext2.img" 4M || fail "mke2fs for the ext2 image"

# The KRFFS image, from the files its manifest names.
mkdir -p "$REPO/build/tests/system/loopimg" || fail "the KRFFS image's tree"
cp "$work/hello.txt" "$work/inner.bin" "$REPO/build/tests/system/loopimg/" ||
    fail "the KRFFS image's files"
sh "$REPO/krffs/mkimage.sh" "$REPO/krffs/manifests/loopimg-test.manifest" \
    "$work/krffs.img" 4 > /dev/null 2>&1 || fail "the KRFFS image"
rm -f "$work/krffs.img.d" "$work/krffs.img.programs"

# The file of 100 bytes.
dd if=/dev/zero of="$work/odd.bin" bs=1 count=100 2>/dev/null || fail "odd.bin"

# The disk of the FAT32 volume with the images.
fatdisk "$work/disk.img" LOOPTEST || fail "the disk"
for f in fat.img:fat.img fat.img:odd2.img krffs.img:krffs.img ext2.img:ext2.img odd.bin:odd.bin; do
    mcopy -i "$work/disk.img"@@$((PART * 512)) "$work/${f%%:*}" "::/${f#*:}" ||
        fail "mcopy ${f#*:} to the disk"
done

# The disk of the ext2 volume with the KRFFS image.
mkdir "$work/tree2" || fail "the ext2 volume's tree"
cp "$work/krffs.img" "$work/tree2/krffs.img" || fail "the ext2 volume's image"
mkext2 "$work/tree2" "$work/ext2out" 16M || fail "mke2fs for the ext2 volume"
{ dd if=/dev/zero bs=512 count=$PART 2>/dev/null; cat "$work/ext2out"; } > "$work/ext2back.img" ||
    fail "the ext2 disk"
"$MKMBR" --lba "$work/ext2back.img" "$PART:$(($(wc -c < "$work/ext2out") / 512)):83" > /dev/null ||
    fail "the ext2 disk's partition table"

# The stick.
fatdisk "$work/stick.img" LOOPSTICK || fail "the stick"
mcopy -i "$work/stick.img"@@$((PART * 512)) "$work/fat.img" ::/fat.img || fail "mcopy to the stick"

for f in fat.img ext2.img krffs.img; do
    mv "$work/$f" "$KEPT/$f" || fail "keeping $f"
done
for f in disk.img ext2back.img stick.img; do
    mv "$work/$f" "$DISKS/$f" || fail "placing $f"
done
