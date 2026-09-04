#!/usr/bin/env sh
#
# loopdev.sh: a file system in a file. Two boots on a disk whose one partition
# is a FAT32 volume carrying two images, one FAT32 and one ext2, each with a
# file of known bytes in it. The probe attaches an image to a loop device,
# mounts it, reads it, writes it, unmounts and detaches, attaches again and
# reads back what it wrote, and asks for everything an attachment refuses.
#
# Usage: loopdev.sh KERNEL IMAGE HARNESS DISK
set -u
KERNEL=$1; IMAGE=$2; HARNESS=$3; DISK=$4
REPO=$(cd "$(dirname "$0")/../../.." && pwd)
PART=2048
SECTORS=61440

fail() { echo "loopdev-test: FAIL: $1" >&2; exit 1; }

# The disk: a FAT32 partition of 30 MiB, and nothing else.
dd if=/dev/zero of="$DISK" bs=1048576 count=32 2>/dev/null || fail "the disk"
"$REPO/build/tools/image/mkmbr" --lba "$DISK" "$PART:$SECTORS:0c" >/dev/null \
    || fail "the partition table"
mformat -i "$DISK"@@$((PART * 512)) -F -T $SECTORS -v LOOPTEST :: || fail "mformat"

# The image inside the image: a FAT32 volume of 4 MiB with one file in it.
dd if=/dev/zero of="$DISK.fat" bs=1048576 count=4 2>/dev/null || fail "the fat image"
mformat -i "$DISK.fat" -F -T 8192 -v INNERFAT :: || fail "mformat inner"
printf 'hello from inside an image' > "$DISK.hello"
mcopy -i "$DISK.fat" "$DISK.hello" ::/hello.txt || fail "mcopy inner"
# A file of whole sectors inside the image, for the row that asks for a device
# standing on a device.
dd if=/dev/zero of="$DISK.inner" bs=512 count=8 2>/dev/null || fail "the inner file"
mcopy -i "$DISK.fat" "$DISK.inner" ::/inner.bin || fail "mcopy inner.bin"

# And an ext2 one, the same size, with the same file.
rm -rf "$DISK.tree"
mkdir -p "$DISK.tree" || fail "the tree"
cp "$DISK.hello" "$DISK.tree/hello.txt" || fail "the tree's file"
cp "$DISK.inner" "$DISK.tree/inner.bin" || fail "the tree's whole-sector file"
# The features this kernel writes: the fixture suite's set, so the volume
# mounts read-write rather than read-only for a feature the writer would
# leave stale.
# Emptied first: mke2fs writes into a file that is already there without
# shortening it, and a volume with a longer file around it is not the volume
# the row asks for.
: > "$DISK.ext2"
E2FSPROGS_FAKE_TIME=1 mke2fs -q -F -t ext2 -b 4096 -I 128 \
    -O sparse_super,large_file,filetype,^dir_index,^resize_inode,^ext_attr,^has_journal,^extent,^64bit,^flex_bg,^metadata_csum,^huge_file,^dir_nlink,^extra_isize \
    -d "$DISK.tree" "$DISK.ext2" 4M >/dev/null 2>&1 || fail "mke2fs"

# A file whose length is not whole sectors, which no device may stand on.
dd if=/dev/zero of="$DISK.odd" bs=1 count=100 2>/dev/null || fail "the odd file"

# And a KRFFS one, which is this project's own file system in a file.
mkdir -p "$REPO/build/tests/system/loopimg"
printf 'hello from inside an image' > "$REPO/build/tests/system/loopimg/hello.txt"
cp "$DISK.inner" "$REPO/build/tests/system/loopimg/inner.bin" || fail "the krffs tree"
sh "$REPO/krffs/mkimage.sh" "$REPO/krffs/manifests/loopimg-test.manifest" \
    "$DISK.krffs" 4 >/dev/null 2>&1 || fail "the krffs image"

mcopy -i "$DISK"@@$((PART * 512)) "$DISK.fat" ::/fat.img || fail "mcopy fat.img"
mcopy -i "$DISK"@@$((PART * 512)) "$DISK.krffs" ::/krffs.img || fail "mcopy krffs.img"
mcopy -i "$DISK"@@$((PART * 512)) "$DISK.ext2" ::/ext2.img || fail "mcopy ext2.img"
mcopy -i "$DISK"@@$((PART * 512)) "$DISK.odd" ::/odd.bin || fail "mcopy odd.bin"

run() {
    "$HARNESS" --kernel "$KERNEL" --initrd "$IMAGE" \
        --drive "file=$DISK,format=raw,if=ide,index=0,media=disk" "$@"
}

for type in vfat ext2 krffs; do
    if run --append "rach2.rescue" --timeout 120 --steptmo 90 \
        --ok "LOOPPROBE OK ($type)" --never "LOOPPROBE FAIL" \
        --log "$DISK.$type.log" "expect=Type 'help'" \
        "send=exec /loopdevprobe $type" "expect=LOOPPROBE" > /dev/null 2>&1; then
        echo "loopdev-test: an image of type $type on a volume is attached, mounted, written and read back: OK"
    else
        fail "the $type boot"
    fi
    grep -aq "loop: loop0: /m/" "$DISK.$type.log" \
        || fail "the kernel did not say what it attached"
done

# The KRFFS image the machine wrote, judged by the host's own checker: what a
# volume that was mounted, written and unmounted owes is a clean image.
mcopy -i "$DISK"@@$((PART * 512)) ::/krffs.img "$DISK.krffs.after" || fail "mcopy krffs back"
"$REPO/build/krffs/fsck.krffs" "$DISK.krffs.after" --strict > "$DISK.fsck" 2>&1 \
    || { cat "$DISK.fsck" >&2; fail "the checker finds the image faulty after the boots"; }
echo "loopdev-test: the host's checker finds the KRFFS image clean after it was written: OK"

# What the machine wrote inside the image is in the image file the host reads,
# which is what says the writes went through the file and not past it.
mcopy -i "$DISK"@@$((PART * 512)) ::/fat.img "$DISK.fat.after" || fail "mcopy back"
mtype -i "$DISK.fat.after" ::/made.txt > "$DISK.made" 2>/dev/null || fail "mtype made.txt"
grep -q "written through a loop device" "$DISK.made" \
    || fail "the host does not find what the machine wrote inside the image"
echo "loopdev-test: and the host finds those bytes inside the image file: OK"

echo "loopdev-test: PASS"
