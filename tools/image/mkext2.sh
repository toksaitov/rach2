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
# Usage: ./mkext2.sh [--overlay PROGRAM] <manifest> <output.ext2> [size_mb]
#                    [blocks_per_group] [bytes_per_inode]
#
# The default size is 1024 MiB, the size of the sealed disk's third partition;
# the file is sparse on the host. The group size and the inode ratio are
# mke2fs's for the size unless given. A grow keeps the number of inodes in a
# group and writes a zeroed inode table for every group it adds, so the ratio
# sets what a grow to a large disk writes. Run from anywhere; the manifest's
# `build/...` host paths resolve because this cd's to the repo root first, and
# so does PROGRAM's.
#
# --overlay runs PROGRAM with the staged directory as its one argument, after
# the manifest is staged and before the volume is written, for a system that
# a manifest cannot describe, such as one with links (vendor/gnu's
# install_root.sh). What it adds is root's, with the fixed staged instant as
# its times and the mode it has in the directory less the group's and
# others' write bits, so that only root may write it. PROGRAM may copy what
# an earlier build made under that build's umask, and a umask of 002 leaves
# a directory writable by its group; the volume is the same whatever write
# bits the umask left, though a read or search bit the umask took away is
# not put back. A set-id bit the program leaves would be root's, and the
# set-id guard, which reads the finished volume, refuses it. A node the
# manifest staged keeps the owner and mode the manifest gives it, so only
# the manifest makes a node that others may write, and one the program
# removed fails the build.
#

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$SCRIPT_DIR/../.." && pwd)"
STAGEIMAGE="$REPO/build/krffs/stageimage.krffs"
SETIDCHECK="$REPO/build/tools/inspect/setidcheck"

OVERLAY=
if [ "$1" = --overlay ]; then
    OVERLAY="$2"
    shift 2
fi
MANIFEST="$1"
OUT="$2"
SIZE="${3:-1024}"
GROUP_BLOCKS="${4:-}"
INODE_BYTES="${5:-}"

if [ -z "$MANIFEST" ] || [ -z "$OUT" ]; then
    echo "usage: mkext2.sh [--overlay PROGRAM] <manifest> <output.ext2> [size_mb] [blocks_per_group] [bytes_per_inode]" >&2
    exit 2
fi
set --
if [ -n "$GROUP_BLOCKS" ]; then
    case "$GROUP_BLOCKS" in
        *[!0-9]*) echo "mkext2: invalid blocks per group: $GROUP_BLOCKS" >&2; exit 2 ;;
    esac
    GROUP_BLOCKS=$(awk -v n="$GROUP_BLOCKS" 'BEGIN {
        if (n < 256 || n > 32768 || n % 8) exit 1
        printf "%.0f", n
    }') || {
        echo "mkext2: blocks per group must be a multiple of 8 from 256 to 32768" >&2
        exit 2
    }
    set -- -g "$GROUP_BLOCKS"
fi
if [ -n "$INODE_BYTES" ]; then
    case "$INODE_BYTES" in
        *[!0-9]*) echo "mkext2: invalid bytes per inode: $INODE_BYTES" >&2; exit 2 ;;
    esac
    INODE_BYTES=$(awk -v n="$INODE_BYTES" 'BEGIN {
        if (n < 4096 || n > 67108864 || n % 4096) exit 1
        printf "%.0f", n
    }') || {
        echo "mkext2: bytes per inode must be a multiple of 4096 from 4096 to 67108864" >&2
        exit 2
    }
    set -- "$@" -i "$INODE_BYTES"
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
if [ -n "$OVERLAY" ] && [ ! -x "$OVERLAY" ]; then
    echo "mkext2: the overlay $OVERLAY is not an executable file" >&2
    exit 1
fi
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
ADDED="$WORK/added"
trap 'rm -rf "$WORK"' EXIT INT TERM

"$STAGEIMAGE" "$MANIFEST" --tree "$TREE" > "$NODES"
: > "$ADDED"
if [ -n "$OVERLAY" ]; then
    "$OVERLAY" "$TREE"
    # The nodes the program added are the paths of the directory the node
    # list does not have; the list has a directory twice when a line
    # makes it as a parent before its own line. debugfs splits a command's
    # words at spaces outside double quotes, so each name added is given to
    # it quoted, and a name with a quote, a backslash or a tab, which the
    # quotes do not carry, is refused.
    awk '$1 == "node" { print $5 }' "$NODES" | LC_ALL=C sort -u > "$WORK/staged"
    (cd "$TREE" && find . -mindepth 1) | sed 's|^\.||' | LC_ALL=C sort > "$WORK/paths"
    LC_ALL=C comm -23 "$WORK/staged" "$WORK/paths" > "$WORK/removed"
    if [ -s "$WORK/removed" ]; then
        echo "mkext2: $OVERLAY removed what the manifest staged:" >&2
        head -n 5 "$WORK/removed" >&2
        exit 1
    fi
    LC_ALL=C comm -13 "$WORK/staged" "$WORK/paths" > "$ADDED"
    BAD="[\"\\\\$(printf '\t')]"
    if grep -q "$BAD" "$ADDED"; then
        echo "mkext2: $OVERLAY added a name with a quote, a backslash or a tab:" >&2
        grep "$BAD" "$ADDED" | head -n 3 >&2
        exit 1
    fi
    # The group's and others' write bits come off every node but a link,
    # since chmod follows a link to its target. debugfs gives each staged
    # node the manifest's mode below, so this changes only what the program
    # added.
    find "$TREE" ! -type l \( -perm -020 -o -perm -002 \) -exec chmod go-w {} +
fi

# The profile: revision 1, 4 KiB blocks, 128-byte inodes, the three
# features the driver writes and the resize inode that keeps room for the
# descriptor table a grow needs, with every other feature mke2fs's
# configuration file might add named and switched off. The times, the
# identifier and the directory hash seed, which mke2fs draws at random even
# with dir_index off, are fixed, so that one manifest makes one image.
PROFILE="sparse_super,large_file,filetype,^dir_index,resize_inode,^ext_attr,^has_journal,^extent,^64bit,^flex_bg,^metadata_csum,^huge_file,^dir_nlink,^extra_isize"
: > "$TMP"
E2FSPROGS_FAKE_TIME=1 mke2fs -q -F -t ext2 -b 4096 -I 128 -O "$PROFILE" "$@" \
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
if [ -n "$GROUP_BLOCKS" ] && [ "$(sb 32)" != "$GROUP_BLOCKS" ]; then
    echo "mkext2: blocks per group $(sb 32), expected $GROUP_BLOCKS" >&2
    exit 1
fi
# mke2fs divides the bytes by the ratio, dropping the remainder, and rounds
# each group's inodes up to whole blocks of the table, 32 inodes of 128 bytes
# to a 4 KiB block, so the count may pass the quotient by less than a block's
# worth a group.
if [ -n "$INODE_BYTES" ] && ! awk -v inodes="$(sb 0)" -v blocks="$(sb 4)" -v bpg="$(sb 32)" \
    -v ratio="$INODE_BYTES" 'BEGIN {
        want = int(blocks * 4096 / ratio)
        groups = int((blocks + bpg - 1) / bpg)
        exit !(inodes >= want && inodes < want + groups * 32)
    }'; then
    echo "mkext2: $(sb 0) inodes in $(sb 4) blocks, not one for each $INODE_BYTES bytes" >&2
    exit 1
fi
[ "$(sb16 88)" = "128" ] || { echo "mkext2: inode size $(sb16 88), not 128" >&2; exit 1; }
[ "$(sb 92)" = "16" ] || { echo "mkext2: compatible features $(sb 92), not resize_inode alone" >&2; exit 1; }
[ "$(sb 96)" = "2" ] || { echo "mkext2: incompatible features $(sb 96), not filetype alone" >&2; exit 1; }
[ "$(sb 100)" = "3" ] || { echo "mkext2: read-only features $(sb 100), not sparse_super and large_file" >&2; exit 1; }

# The owners, modes and set-id bits from the manifest, one debugfs command
# each, with the type bits kept: `mode` is the whole i_mode. The change
# time as well, which mke2fs copies from the staged tree and no builder can
# set there, so that one manifest makes one image. Where an overlay ran, it
# may have changed the times of a staged directory by adding to it, so every
# node has the staged instant as all three times, and what it added is
# root's.
awk -v overlay="$OVERLAY" '$1 == "node" {
    printf "set_inode_field %s mode %s\n", $5, $2;
    printf "set_inode_field %s uid %s\n", $5, $3;
    printf "set_inode_field %s gid %s\n", $5, $4;
    printf "set_inode_field %s ctime @1\n", $5;
    if (overlay != "") {
        printf "set_inode_field %s atime @1\n", $5;
        printf "set_inode_field %s mtime @1\n", $5;
    }
}' "$NODES" > "$SCRIPT"
awk '{
    printf "set_inode_field \"%s\" uid 0\n", $0;
    printf "set_inode_field \"%s\" gid 0\n", $0;
    printf "set_inode_field \"%s\" atime @1\n", $0;
    printf "set_inode_field \"%s\" mtime @1\n", $0;
    printf "set_inode_field \"%s\" ctime @1\n", $0;
}' "$ADDED" >> "$SCRIPT"
if [ -n "$OVERLAY" ]; then
    printf 'set_inode_field / %s @1\n' atime mtime ctime >> "$SCRIPT"
fi
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
ADDED_NOTE=
[ -z "$OVERLAY" ] || ADDED_NOTE=" and $(wc -l < "$ADDED" | tr -d ' ') from $OVERLAY"
echo "mkext2: wrote $OUT from $MANIFEST ($(grep -c '^node ' "$NODES") nodes$ADDED_NOTE, ${SIZE} MiB)"
