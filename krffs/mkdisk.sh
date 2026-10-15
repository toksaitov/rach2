#!/usr/bin/env sh

#
# mkdisk.sh: build a disk image with one KRFFS partition, staged from a
# manifest.
#
# Where mkimage.sh writes a KRFFS that fills its file, the form of a RAM
# root, this writes a disk: a 512-byte MBR with one primary partition, and in
# it a KRFFS that mkfs.krffs and stageimage.krffs write through their
# --offset and --size options.
#
# Usage: mkdisk.sh MANIFEST OUTPUT [DISK_MIB [PARTITION_MIB]]
#
# DISK_MIB is 64 and PARTITION_MIB 48 by default. The script runs from any
# directory: it changes to the repository root, against which the
# manifest's host paths are read. The KRFFS tools must be built in
# build/krffs/.
#

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$SCRIPT_DIR/.." && pwd)"
TOOLS="$REPO/build/krffs"

MANIFEST="$1"
OUT="$2"
DISK_MB="${3:-64}"
PART_MB="${4:-48}"

# The partition starts at LBA 2048, 1 MiB in: the usual alignment, and a
# multiple of every host's mmap granularity, which the region options need.
PART_LBA=2048
SECTOR=512
PART_OFFSET=$((PART_LBA * SECTOR))
PART_SIZE=$((PART_MB * 1048576))
DISK_SIZE=$((DISK_MB * 1048576))

# The kernel maps a KRFFS partition through its page-cache window, which is
# at most 512 MiB (PC_WINDOW_CAP in rach2/sys/mm/pagecache.c); a larger
# partition cannot be mounted from disk.
WINDOW_MB=512

if [ -z "$MANIFEST" ] || [ -z "$OUT" ]; then
    echo "usage: mkdisk.sh <manifest> <output.disk> [disk_mb] [partition_mb]" >&2
    exit 2
fi
case "$DISK_MB$PART_MB" in
    *[!0-9]*|'') echo "mkdisk: disk_mb and partition_mb must be positive integers" >&2; exit 2 ;;
esac
if [ ! -f "$TOOLS/mkfs.krffs" ] || [ ! -f "$TOOLS/stageimage.krffs" ]; then
    echo "mkdisk: the KRFFS host tools are missing; run 'make -C krffs' first" >&2
    exit 1
fi
if [ $((PART_OFFSET + PART_SIZE)) -gt "$DISK_SIZE" ]; then
    echo "mkdisk: a ${PART_MB} MiB partition at 1 MiB does not fit a ${DISK_MB} MiB disk" >&2
    exit 1
fi
if [ "$PART_MB" -gt "$WINDOW_MB" ]; then
    echo "mkdisk: a ${PART_MB} MiB partition exceeds the ${WINDOW_MB} MiB mount window" >&2
    exit 1
fi

cd "$REPO"
mkdir -p "$(dirname "$OUT")"

# The disk is built in a temporary file and renamed only on success, so a
# failure leaves any previous disk as it was.
TMP="$OUT.new.$$"   # unique, so two parallel suites building one disk do not collide
trap 'rm -f "$TMP"' EXIT INT TERM
dd if=/dev/zero of="$TMP" bs=1048576 count="$DISK_MB" 2>/dev/null

# The MBR has no boot code, one primary partition entry over the KRFFS
# region and the 0x55AA signature. The type, 0x7f, is the one reserved for
# experimental use, which a host does not mount by itself. mkmbr's --lba
# form writes in the CHS fields the value that means "use the LBA".
MKMBR="$REPO/build/tools/image/mkmbr"
make -s -C "$REPO" build/tools/image/mkmbr || exit 1
"$MKMBR" --lba "$TMP" "$PART_LBA:$((PART_SIZE / SECTOR)):7f"

"$TOOLS/mkfs.krffs" "$TMP" --offset "$PART_OFFSET" --size "$PART_SIZE" >/dev/null
"$TOOLS/stageimage.krffs" "$MANIFEST" "$TMP" --offset "$PART_OFFSET" --size "$PART_SIZE"

# Both halves of the set-id guard, as mkimage.sh runs them. The inventory
# reads the partition, so it is given the region's offset and size.
SETIDCHECK="$REPO/build/tools/inspect/setidcheck"
make -s -C "$REPO" build/tools/inspect/setidcheck || exit 1
"$SETIDCHECK" "$MANIFEST"
"$SETIDCHECK" --image "$TMP" --manifest "$MANIFEST" \
    --offset "$PART_OFFSET" --size "$PART_SIZE"
mv "$TMP" "$OUT"
