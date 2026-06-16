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

# Build the guest programs, unless Make already built them (SKIP_BUILD=1, set by the
# `make ramdisk` target so nothing is built twice); a standalone run still builds
# them for convenience.
if [ "${SKIP_BUILD:-0}" != 1 ]; then
    echo "=== Building ISH shell ==="
    make -C ../ish

    echo ""
    echo "=== Building Unix utilities ==="
    make -C ../utils
fi

# Doom is a capstone (ports/), not part of the base teaching image, so this base build
# neither builds nor stages it by default. The staging script deliberately does NOT
# build Doom; it only STAGES an already-built ../ports/doom/doom when
# RAMDISK_WITH_DOOM=1, so it can never stage a stale binary from a failed build. That
# opt-in is for a STANDALONE `./build_ramdisk.sh` run after building the binary (`make
# -C ../ports/doom`); the Make `ramdisk` target forces RAMDISK_WITH_DOOM=0, so `make`/
# `make full` stay capstone-free and the staging never races the sibling ports/doom
# build. A Make-wired bootable Doom capstone image is the next phase.
RAMDISK_WITH_DOOM=${RAMDISK_WITH_DOOM:-0}

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
for util in true false echo yes cat pwd touch mkdir rm mv cp ls ping nc httpd; do
    if [ -f "../utils/$util" ]; then
        cp "../utils/$util" "$MOUNT_POINT/"
        echo "  $util"
    fi
done

# Stage Doom + its WAD only when requested and already built (RAMDISK_WITH_DOOM=1).
# The script never builds Doom, so it cannot stage a stale binary from a failed build;
# the WAD is not vendored, so the operator supplies ../ports/doom/doom1.wad.
if [ "$RAMDISK_WITH_DOOM" = 1 ]; then
    if [ -f "../ports/doom/doom" ]; then
        cp "../ports/doom/doom" "$MOUNT_POINT/"
        echo "  doom"
        if [ -f "../ports/doom/doom1.wad" ]; then
            cp "../ports/doom/doom1.wad" "$MOUNT_POINT/"
            echo "  doom1.wad"
        fi
    else
        echo "note: RAMDISK_WITH_DOOM=1 but ../ports/doom/doom is not built (run 'make full' first); skipping" >&2
    fi
fi

# Create a test file for cat and other file-processing utilities
echo "hello, world" > "$MOUNT_POINT/hello.txt"
echo "  hello.txt (test file)"

# Name-resolution config for the network utilities. KRFFS has no directories,
# so these live in the flat root; the resolver opens them as /resolv.conf and
# /hosts. nameserver 10.0.2.3 is QEMU SLIRP's built-in DNS forwarder.
printf 'nameserver 10.0.2.3\n' > "$MOUNT_POINT/resolv.conf"
echo "  resolv.conf"
printf '127.0.0.1   localhost\n10.0.2.2    gateway\n10.0.2.3    dns\n10.0.2.15   rach2\n' > "$MOUNT_POINT/hosts"
echo "  hosts"

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
