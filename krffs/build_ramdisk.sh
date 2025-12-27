#!/usr/bin/env sh

#
# build_ramdisk.sh
#
# Builds the ISH shell and Unix utilities for Rach2, creates a KRFFS RAM disk
# image, and copies the binaries onto it.
#
# Usage:
#     ./build_ramdisk.sh [size in megabytes]
#
# Without the size parameter, the file system size is 10 megabytes.
#

set -e

# Prevent macOS from creating ._* AppleDouble files
export COPYFILE_DISABLE=1

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$SCRIPT_DIR"

SIZE=${1:-10}
MOUNT_POINT="mount_point"
FS_FILE="file_system.krffs"

echo "=== Building ISH shell ==="
make -C ../ish

echo ""
echo "=== Building Unix utilities ==="
make -C ../utils

echo ""
echo "=== Building Doom (optional) ==="
if [ -d "../apps/doom" ]; then
    make -C ../apps/doom || echo "Doom build failed or skipped"
fi

echo ""
echo "=== Creating KRFFS file system ($SIZE MB) ==="
rm -f "$FS_FILE"
dd if=/dev/zero of="$FS_FILE" bs=1048576 count=$SIZE
./mkfs.krffs "$FS_FILE"

echo ""
echo "=== Mounting file system ==="
mkdir -p "$MOUNT_POINT"
# Clear mount point if not empty
rm -rf "${MOUNT_POINT:?}"/*
./krffs "$FS_FILE" "$MOUNT_POINT"

echo ""
echo "=== Copying binaries ==="

# Copy ISH shell
cp ../ish/ish "$MOUNT_POINT/"
echo "  ish"

# Copy utilities
for util in true false echo yes cat pwd touch mkdir rm mv cp ls; do
    if [ -f "../utils/$util" ]; then
        cp "../utils/$util" "$MOUNT_POINT/"
        echo "  $util"
    fi
done

# Copy Doom if it was built
if [ -f "../apps/doom/doom" ]; then
    cp "../apps/doom/doom" "$MOUNT_POINT/"
    echo "  doom"
fi

# Copy doom1.wad if it exists (user must provide this)
if [ -f "../apps/doom/doom1.wad" ]; then
    cp "../apps/doom/doom1.wad" "$MOUNT_POINT/"
    echo "  doom1.wad"
fi

# Create a test file for cat and other file-processing utilities
echo "hello, world" > "$MOUNT_POINT/hello.txt"
echo "  hello.txt (test file)"

# Remove macOS AppleDouble files (._*) if any were created
rm -f "$MOUNT_POINT"/._*

echo ""
echo "=== Unmounting file system ==="
# Try fusermount first, fall back to umount
if command -v fusermount >/dev/null 2>&1; then
    fusermount -u "$MOUNT_POINT"
elif command -v umount >/dev/null 2>&1; then
    umount "$MOUNT_POINT"
else
    echo "Error: Neither fusermount nor umount available"
    exit 1
fi

echo ""
echo "=== Done ==="
echo "RAM disk image: $FS_FILE"
echo ""
echo "To use with QEMU:"
echo "  qemu-system-i386 -kernel ../rach2/kernel -initrd $FS_FILE -serial stdio"
