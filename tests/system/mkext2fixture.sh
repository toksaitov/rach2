#!/usr/bin/env sh
#
# mkext2fixture.sh: the ext2 fixture disk the ext2 suite boots.
#
# A tree with the shapes the walker must answer, a volume from it on the
# profile mkext2.sh writes, and a disk with that volume as its first MBR
# partition, type 0x83, at sector 2048. The host's mke2fs writes the
# volume, so the driver is judged against the reference writer and not
# against itself.
#
# Usage: mkext2fixture.sh OUTPUT.disk MKMBR
#
set -eu

DISK=$1
MKMBR=$2
REPO=$(cd "$(dirname "$0")/../.." && pwd)
TREE="$DISK.tree"
VOL="$DISK.vol"

. "$REPO/tools/setup/e2fsprogs_path.sh"

rm -rf "$TREE" "$VOL" "$DISK"
mkdir -p "$TREE/sub"
printf 'hello\n' > "$TREE/hello.txt"
printf 'deep\n' > "$TREE/sub/deep.txt"
ln "$TREE/hello.txt" "$TREE/hello.link"
ln -s hello.txt "$TREE/fast"
ln -s /e2/hello.txt "$TREE/abs"
ln -s sub "$TREE/dirlink"
ln -s nowhere "$TREE/dangling"
ln -s "$(printf 'x%.0s' $(seq 1 300))" "$TREE/long300"
# A chain of forty links ending at a file, and one of forty-one.
i=1
while [ "$i" -lt 40 ]; do ln -s "c$((i + 1))" "$TREE/c$i"; i=$((i + 1)); done
ln -s hello.txt "$TREE/c40"
i=1
while [ "$i" -lt 41 ]; do ln -s "d$((i + 1))" "$TREE/d$i"; i=$((i + 1)); done
ln -s hello.txt "$TREE/d41"

: > "$VOL"
E2FSPROGS_FAKE_TIME=1 mke2fs -q -F -t ext2 -b 4096 -I 128 \
    -O sparse_super,large_file,filetype,^dir_index,^resize_inode,^ext_attr,^has_journal,^extent,^64bit,^flex_bg,^metadata_csum,^huge_file,^dir_nlink,^extra_isize \
    -L e2fixture -d "$TREE" "$VOL" 16M 2>&1 | grep -v '128-byte inodes' || true
e2fsck -fn "$VOL" > /dev/null 2>&1 || { echo "mkext2fixture: e2fsck finds the fixture faulty" >&2; exit 1; }

dd if=/dev/zero of="$DISK" bs=1048576 count=1 2>/dev/null
cat "$VOL" >> "$DISK"
dd if=/dev/zero bs=1048576 count=1 >> "$DISK" 2>/dev/null
"$MKMBR" --lba "$DISK" 2048:32768:83 > /dev/null
rm -rf "$TREE" "$VOL"
echo "mkext2fixture: wrote $DISK"
