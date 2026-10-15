#!/usr/bin/env sh
#
# loopdev.sh: a file system in a file.
#
# The disk's one partition is a FAT32 volume that holds a FAT32, an ext2 and
# a KRFFS image, each with a file of known bytes, a second FAT32 image, and a
# file whose length is not whole sectors. For each of the three types a boot
# runs the probe, which attaches the image to a loop device, mounts it, reads
# it, writes it, unmounts and detaches, attaches again and reads back what it
# wrote, and asks for everything an attachment refuses. Further boots require
# a loop device's write to reach the medium when it returns, an image on an
# ext2 volume whose second name is refused a device of its own, the FAT32
# sequence over a disk that sleeps its caller, and an error when a USB stick
# is pulled from under a mounted image. Last, the host reads the images the
# machine wrote.
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
# The features this kernel writes, the fixture suite's set, so that the
# volume mounts read-write and not read-only for a feature the writer would
# leave stale. The file is emptied first: mke2fs writes into a file that is
# already there without shortening it.
: > "$DISK.ext2"
E2FSPROGS_FAKE_TIME=1 mke2fs -q -F -t ext2 -b 4096 -I 128 \
    -O sparse_super,large_file,filetype,^dir_index,resize_inode,^ext_attr,^has_journal,^extent,^64bit,^flex_bg,^metadata_csum,^huge_file,^dir_nlink,^extra_isize \
    -d "$DISK.tree" "$DISK.ext2" 4M >/dev/null 2>&1 || fail "mke2fs"

# A file whose length is not whole sectors, which no device may stand on.
dd if=/dev/zero of="$DISK.odd" bs=1 count=100 2>/dev/null || fail "the odd file"

# And a KRFFS image with the same two files.
mkdir -p "$REPO/build/tests/system/loopimg"
printf 'hello from inside an image' > "$REPO/build/tests/system/loopimg/hello.txt"
cp "$DISK.inner" "$REPO/build/tests/system/loopimg/inner.bin" || fail "the krffs tree"
sh "$REPO/krffs/mkimage.sh" "$REPO/krffs/manifests/loopimg-test.manifest" \
    "$DISK.krffs" 4 >/dev/null 2>&1 || fail "the krffs image"

# A second FAT32 image, for the rows that ask what a taken device and a
# device that will not write answer.
cp "$DISK.fat" "$DISK.fat2" || fail "the second image"

mcopy -i "$DISK"@@$((PART * 512)) "$DISK.fat" ::/fat.img || fail "mcopy fat.img"
mcopy -i "$DISK"@@$((PART * 512)) "$DISK.fat2" ::/odd2.img || fail "mcopy odd2.img"
mcopy -i "$DISK"@@$((PART * 512)) "$DISK.krffs" ::/krffs.img || fail "mcopy krffs.img"
mcopy -i "$DISK"@@$((PART * 512)) "$DISK.ext2" ::/ext2.img || fail "mcopy ext2.img"
mcopy -i "$DISK"@@$((PART * 512)) "$DISK.odd" ::/odd.bin || fail "mcopy odd.bin"

# run ARG...: boot the test image on the disk with the harness's ARGs.
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

# A loop device's write has written and synced the backing file when it
# returns. The row fills the cache through a mounted image so that eviction
# alone writes the image's blocks back, with no sync and no unmount, and the
# drivers' own flush count must grow.
if run --append "rach2.rescue" --timeout 150 --steptmo 120 \
    --ok "LOOPPROBE OK (durable)" --never "LOOPPROBE FAIL" \
    --log "$DISK.durable.log" "expect=Type 'help'" \
    "send=exec /loopdevprobe durable" "expect=LOOPPROBE" > /dev/null 2>&1; then
    echo "loopdev-test: a write through a loop device reaches the medium when it returns, not when something later flushes: OK"
else
    grep -a 'LOOPPROBE FAIL' "$DISK.durable.log" >&2 || true
    fail "the durability boot"
fi

# The outer volume as ext2 rather than FAT32: a file is a file whatever holds
# it, and ext2 has hard links, so the row that says one file is one device can
# be asked with two names for it.
rm -rf "$DISK.tree2"
mkdir -p "$DISK.tree2" || fail "the ext2 tree"
cp "$DISK.krffs" "$DISK.tree2/krffs.img" || fail "the image in the ext2 tree"
: > "$DISK.ext2out"
E2FSPROGS_FAKE_TIME=1 mke2fs -q -F -t ext2 -b 4096 -I 128 \
    -O sparse_super,large_file,filetype,^dir_index,resize_inode,^ext_attr,^has_journal,^extent,^64bit,^flex_bg,^metadata_csum,^huge_file,^dir_nlink,^extra_isize \
    -d "$DISK.tree2" "$DISK.ext2out" 16M >/dev/null 2>&1 || fail "mke2fs for the outer volume"
E2SECTORS=$(($(wc -c < "$DISK.ext2out") / 512))
{ dd if=/dev/zero bs=512 count=$PART 2>/dev/null; cat "$DISK.ext2out"; } > "$DISK.e2"
"$REPO/build/tools/image/mkmbr" --lba "$DISK.e2" "$PART:$E2SECTORS:83" >/dev/null \
    || fail "the ext2 disk's partition table"
if "$HARNESS" --kernel "$KERNEL" --initrd "$IMAGE" \
    --drive "file=$DISK.e2,format=raw,if=ide,index=0,media=disk" \
    --append "rach2.rescue" --timeout 150 --steptmo 120 \
    --ok "LOOPPROBE OK (ext2back)" --never "LOOPPROBE FAIL" \
    --log "$DISK.ext2back.log" "expect=Type 'help'" \
    "send=exec /loopdevprobe ext2back" "expect=LOOPPROBE" > /dev/null 2>&1; then
    echo "loopdev-test: an image on an ext2 volume is attached, mounted and written, and its second name is refused a device of its own: OK"
else
    grep -a 'LOOPPROBE FAIL' "$DISK.ext2back.log" >&2 || true
    fail "the ext2-backing boot"
fi

# The same sequence over a disk that sleeps its caller: the device under the
# loop device takes a slot of the request pool for every transfer, and the
# loop device must take none, or the pool of sixteen would run out with every
# holder waiting for another.
if run --append "rach2.rescue rach2.blocklag=1" --timeout 180 --steptmo 150 \
    --ok "LOOPPROBE OK (vfat)" --never "LOOPPROBE FAIL" --log "$DISK.lag.log" \
    "expect=Type 'help'" "send=exec /loopdevprobe vfat" "expect=LOOPPROBE" \
    > /dev/null 2>&1; then
    echo "loopdev-test: and all of it again over a disk that sleeps its caller: OK"
else
    fail "the boot over a sleeping disk"
fi

# A stick pulled from under an image mounted on a loop device. The stick's
# volume holds the same FAT32 image; the harness pulls the device while the
# probe is reading through it, and the machine must then return an error,
# unmount both volumes and release the file the attachment held.
dd if=/dev/zero of="$DISK.stick" bs=1048576 count=32 2>/dev/null || fail "the stick"
"$REPO/build/tools/image/mkmbr" --lba "$DISK.stick" "$PART:$SECTORS:0c" >/dev/null \
    || fail "the stick's partition table"
mformat -i "$DISK.stick"@@$((PART * 512)) -F -T $SECTORS -v LOOPSTICK :: \
    || fail "mformat the stick"
mcopy -i "$DISK.stick"@@$((PART * 512)) "$DISK.fat" ::/fat.img || fail "mcopy to the stick"

if "$HARNESS" --kernel "$KERNEL" --initrd "$IMAGE" \
    --qemu-arg=-device --qemu-arg=qemu-xhci,id=xhci \
    --qemu-arg=-drive --qemu-arg="file=$DISK.stick,format=raw,if=none,id=stick" \
    --qemu-arg=-device --qemu-arg=usb-storage,bus=xhci.0,drive=stick,id=st0 \
    --append "rach2.rescue" --timeout 150 --steptmo 120 \
    --ok "LOOPPROBE OK (pull)" --never "LOOPPROBE FAIL" --log "$DISK.pull.log" \
    "expect=Type 'help'" "send=exec /loopdevprobe pull" \
    "expect=LOOPPROBE HOLDING" "usb=del:st0" "expect=LOOPPROBE OK" \
    > /dev/null 2>&1; then
    echo "loopdev-test: a stick pulled from under a mounted image takes the device with it: OK"
else
    fail "the stick-pull boot"
fi
grep -aq "loop: loop0: the device under its file has gone" "$DISK.pull.log" \
    || fail "the kernel did not say the device under the file had gone"

# The KRFFS image the machine wrote, judged by the host's own checker: a
# volume that was mounted, written and unmounted must be clean.
mcopy -n -i "$DISK"@@$((PART * 512)) ::/krffs.img "$DISK.krffs.after" || fail "mcopy krffs back"
"$REPO/build/krffs/fsck.krffs" "$DISK.krffs.after" --strict > "$DISK.fsck" 2>&1 \
    || { cat "$DISK.fsck" >&2; fail "the checker finds the image faulty after the boots"; }
# And the file the machine wrote inside it, read out of the image by the host's
# own reader: a clean image says nothing about what reached it.
"$REPO/build/krffs/readfile.krffs" "$DISK.krffs.after" /made.txt "$DISK.krffs.made" \
    >/dev/null 2>&1 || fail "the host cannot read what the machine wrote"
grep -q "written through a loop device" "$DISK.krffs.made" \
    || fail "the host does not find what the machine wrote inside the KRFFS image"
echo "loopdev-test: the host's checker finds the KRFFS image clean, with the file the machine wrote in it: OK"

# What the machine wrote inside the image is in the image file the host reads,
# which is what says the writes went through the file and not past it.
mcopy -n -i "$DISK"@@$((PART * 512)) ::/fat.img "$DISK.fat.after" || fail "mcopy back"
mtype -i "$DISK.fat.after" ::/made.txt > "$DISK.made" 2>/dev/null || fail "mtype made.txt"
grep -q "written through a loop device" "$DISK.made" \
    || fail "the host does not find what the machine wrote inside the image"
echo "loopdev-test: and the host finds those bytes inside the image file: OK"

echo "loopdev-test: PASS"
