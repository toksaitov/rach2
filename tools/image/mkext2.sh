#!/usr/bin/env sh

#
# mkext2.sh: build one ext2 image out of tree from a manifest.
#
# stageimage.krffs materialises the manifest into a fresh directory and lists
# every node with its mode and owner; mke2fs writes the volume from that
# directory with the one feature profile this kernel's driver writes; debugfs
# then sets each node's owner, mode and set-id bits, because a build that is
# not root cannot make a root-owned set-id file on the host and mke2fs takes
# what the host directory says; e2fsck must find the volume clean; and the
# set-id guard reads the finished image with its own reader. The image is
# built under a temporary name and renamed into place only after every step
# passes, so no unchecked image ever exists under the target's name.
#
# Usage: ./mkext2.sh <manifest> <output.ext2> [size_in_megabytes]
#
# The default size is 1024 MiB, the size of the sealed disk's fourth partition;
# the file is sparse on the host. Run from anywhere; the manifest's `build/...`
# host paths resolve because this cd's to the repo root first.
#

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$SCRIPT_DIR/../.." && pwd)"
STAGEIMAGE="$REPO/build/krffs/stageimage.krffs"
SETIDCHECK="$REPO/build/tools/inspect/setidcheck"

MANIFEST="$1"
OUT="$2"
SIZE="${3:-1024}"

if [ -z "$MANIFEST" ] || [ -z "$OUT" ]; then
    echo "usage: mkext2.sh <manifest> <output.ext2> [size_mb]" >&2
    exit 2
fi
if [ ! -x "$STAGEIMAGE" ]; then
    echo "mkext2: $STAGEIMAGE is missing; run 'make -C krffs' first" >&2
    exit 1
fi

# e2fsprogs, on the path or where a package manager keeps it.
. "$REPO/tools/setup/e2fsprogs_path.sh"
for t in mke2fs debugfs e2fsck; do
    command -v "$t" >/dev/null 2>&1 || { echo "mkext2: $t is missing; install e2fsprogs" >&2; exit 1; }
done

cd "$REPO"
mkdir -p "$(dirname "$OUT")"

# Every intermediate lives in a directory of this run's own, so that two
# builds of one output never share a path and nothing here follows a name
# somebody else made; the image reaches its name by one rename at the end.
WORK=$(mktemp -d "$(dirname "$OUT")/.mkext2.XXXXXX") || exit 1
TMP="$WORK/image"
TREE="$WORK/tree"
NODES="$WORK/nodes"
SCRIPT="$WORK/debugfs"
FSCK="$WORK/fsck.log"
MKLOG="$WORK/mke2fs.log"
trap 'rm -rf "$WORK"' EXIT INT TERM

"$STAGEIMAGE" "$MANIFEST" --tree "$TREE" > "$NODES"

# The profile: revision 1, 4 KiB blocks, 128-byte inodes, and exactly the
# three features the driver writes, with every other feature mke2fs's
# configuration file might add named and switched off. The times, the
# identifier and the directory hash seed, which mke2fs draws at random even
# with dir_index off, are fixed, so that one manifest makes one image.
PROFILE="sparse_super,large_file,filetype,^dir_index,^resize_inode,^ext_attr,^has_journal,^extent,^64bit,^flex_bg,^metadata_csum,^huge_file,^dir_nlink,^extra_isize"
: > "$TMP"
E2FSPROGS_FAKE_TIME=1 mke2fs -q -F -t ext2 -b 4096 -I 128 -O "$PROFILE" \
    -E hash_seed=52616368-3200-4578-7432-000000000002 \
    -U 52616368-3200-4578-7432-000000000001 -L rach2 -d "$TREE" "$TMP" "${SIZE}M" \
    > "$MKLOG" 2>&1
MKSTATUS=$?
grep -v '128-byte inodes cannot handle dates beyond 2038' "$MKLOG" >&2 || true
[ "$MKSTATUS" -eq 0 ] || { echo "mkext2: mke2fs failed with status $MKSTATUS" >&2; exit 1; }

# Read the superblock back: the magic, the revision, the block size, the
# inode size and the three feature words must be the profile's, whatever
# mke2fs's configuration said.
sb() { od -A n -t u4 -j "$((1024 + $1))" -N 4 "$TMP" | tr -d ' '; }
sb16() { od -A n -t u2 -j "$((1024 + $1))" -N 2 "$TMP" | tr -d ' '; }
[ "$(sb16 56)" = "61267" ] || { echo "mkext2: no ext2 magic in the image" >&2; exit 1; }
[ "$(sb 76)" = "1" ] || { echo "mkext2: revision $(sb 76), not 1" >&2; exit 1; }
[ "$(sb 24)" = "2" ] || { echo "mkext2: block size log $(sb 24), not 2 (4096)" >&2; exit 1; }
[ "$(sb16 88)" = "128" ] || { echo "mkext2: inode size $(sb16 88), not 128" >&2; exit 1; }
[ "$(sb 92)" = "0" ] || { echo "mkext2: compatible features $(sb 92), not 0" >&2; exit 1; }
[ "$(sb 96)" = "2" ] || { echo "mkext2: incompatible features $(sb 96), not filetype alone" >&2; exit 1; }
[ "$(sb 100)" = "3" ] || { echo "mkext2: read-only features $(sb 100), not sparse_super and large_file" >&2; exit 1; }

# The owners, modes and set-id bits from the manifest, one debugfs command
# each, with the type bits kept: `mode` is the whole i_mode. The change
# time as well, which mke2fs copies from the staged tree and no builder can
# set there, so that one manifest makes one image.
awk '$1 == "node" {
    printf "set_inode_field %s mode %s\n", $5, $2;
    printf "set_inode_field %s uid %s\n", $5, $3;
    printf "set_inode_field %s gid %s\n", $5, $4;
    printf "set_inode_field %s ctime @1\n", $5;
}' "$NODES" > "$SCRIPT"
debugfs -w -f "$SCRIPT" "$TMP" > "$WORK/debugfs.log" 2>&1
DBSTATUS=$?
if [ "$DBSTATUS" -ne 0 ] || grep -v '^debugfs' "$WORK/debugfs.log" | grep -q .; then
    echo "mkext2: debugfs refused a command (status $DBSTATUS):" >&2
    grep -v '^debugfs' "$WORK/debugfs.log" | head -3 >&2
    exit 1
fi

# The two gates: structure, then policy.
e2fsck -fn "$TMP" > "$FSCK" 2>&1 || { cat "$FSCK" >&2; exit 1; }
if [ ! -x "$SETIDCHECK" ] || [ "$REPO/tools/inspect/setidcheck.c" -nt "$SETIDCHECK" ]; then
    mkdir -p "${SETIDCHECK%/*}"
    cc -O2 -o "$SETIDCHECK" "$REPO/tools/inspect/setidcheck.c" || exit 1
fi
"$SETIDCHECK" "$MANIFEST"
"$SETIDCHECK" --ext2 "$TMP" --manifest "$MANIFEST"

mv "$TMP" "$OUT"
echo "mkext2: wrote $OUT from $MANIFEST ($(grep -c '^node ' "$NODES") nodes, ${SIZE} MiB)"
