#!/usr/bin/env sh

#
# mkdisk.sh: build one bootable-layout KRFFS disk image out of tree.
#
# Where mkimage.sh writes a bare KRFFS that fills a file (the RAM-disk module),
# this writes a real disk image: a 512-byte MBR with one primary KRFFS
# partition, then a fresh KRFFS laid down inside that partition and staged from
# a manifest. The partition starts at LBA 2048 (a 1 MiB offset), which is both
# sector- and page-aligned, so the host mmap the KRFFS tools use is always
# valid. mkfs.krffs and stageimage.krffs are pointed at the partition with their
# --offset/--size region arguments; the KRFFS bytes are byte-identical to a
# module image.
#
# Usage: ./mkdisk.sh <manifest> <output.disk> [disk_mb] [partition_mb]
#
# disk_mb defaults to 64 and partition_mb to 48, matching `make os-real-disk`.
# Run from anywhere; the manifest's `build/...` host paths resolve because this
# cd's to the repo root first. The KRFFS host tools come from build/krffs.
#

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$SCRIPT_DIR/.." && pwd)"
TOOLS="$REPO/build/krffs"

MANIFEST="$1"
OUT="$2"
DISK_MB="${3:-64}"
PART_MB="${4:-48}"

# LBA 2048 == a 1 MiB offset: the standard 1 MiB alignment, also the coarsest
# mmap granularity across hosts, so the partition byte offset is always valid.
PART_LBA=2048
SECTOR=512
PART_OFFSET=$((PART_LBA * SECTOR))
PART_SIZE=$((PART_MB * 1048576))
DISK_SIZE=$((DISK_MB * 1048576))

# The kernel maps the KRFFS partition through a fixed virtual window; a
# partition larger than that window cannot be mounted from disk. The window is
# capped at 512 MiB (PC_WINDOW_CAP in sys/mm/pagecache.c), so cap the partition
# to match. This said 256 until milestone 25: the cap was raised when the window
# moved above the region real firmware uses for MMIO, and this copy of the
# number was left behind, which put a 500 MiB root out of reach of its own
# builder.
WINDOW_MB=512

if [ -z "$MANIFEST" ] || [ -z "$OUT" ]; then
    echo "usage: mkdisk.sh <manifest> <output.disk> [disk_mb] [partition_mb]" >&2
    exit 2
fi
case "$DISK_MB$PART_MB" in
    *[!0-9]*|'') echo "mkdisk: disk_mb and partition_mb must be positive integers" >&2; exit 2 ;;
esac
if ! command -v python3 >/dev/null 2>&1; then
    echo "mkdisk: python3 is required to write the MBR" >&2
    exit 1
fi
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

# Build into a temp file and rename only on success, so a staging failure leaves
# any previous disk intact, not a half-written one.
TMP="$OUT.new"
trap 'rm -f "$TMP"' EXIT INT TERM
dd if=/dev/zero of="$TMP" bs=1048576 count="$DISK_MB" 2>/dev/null

# Write a minimal MBR: the boot code stays zero (GRUB is installed later), one
# primary partition entry points at the KRFFS region, and the 0x55AA signature
# marks the sector valid. Type 0x7F is the reserved "individual/experimental"
# code, fitting for a custom file system and ignored by host auto-mounters.
python3 - "$TMP" "$PART_LBA" "$((PART_SIZE / SECTOR))" <<'PY'
import struct, sys
path, lba, count = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])
mbr = bytearray(512)
entry = struct.pack(
    "<BBBBBBBBII",
    0x00,                    # not bootable yet (GRUB phase sets this)
    0xFE, 0xFF, 0xFF,        # CHS start: sentinel meaning "use LBA"
    0x7F,                    # partition type: experimental/custom (KRFFS)
    0xFE, 0xFF, 0xFF,        # CHS end: same sentinel
    lba,                     # LBA of the first sector
    count,                   # sector count
)
mbr[446:446 + 16] = entry
mbr[510] = 0x55
mbr[511] = 0xAA
with open(path, "r+b") as f:
    f.write(mbr)
PY

"$TOOLS/mkfs.krffs" "$TMP" --offset "$PART_OFFSET" --size "$PART_SIZE" >/dev/null
"$TOOLS/stageimage.krffs" "$MANIFEST" "$TMP" --offset "$PART_OFFSET" --size "$PART_SIZE"

# Both halves, as mkimage.sh runs them, the inventory over the partition rather
# than the whole file: the region is the partition, so the walk needs both
# bounds or it runs off the wrong end.
python3 "$REPO/tools/inspect/check_setid.py" "$MANIFEST"
python3 "$REPO/tools/inspect/check_setid.py" --image "$TMP" --manifest "$MANIFEST" \
    --offset "$PART_OFFSET" --size "$PART_SIZE"
mv "$TMP" "$OUT"
