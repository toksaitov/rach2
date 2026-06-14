#!/usr/bin/env sh

#
# build_test_ramdisk.sh
#
# Builds the Rach2 syscall test programs and creates a KRFFS RAM disk image
# containing all test binaries.
#
# Usage:
#     ./build_test_ramdisk.sh [size in megabytes]
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

echo "=== Building test programs ==="
make -C ../tests

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
echo "=== Copying test binaries ==="

# Copy all test programs
for prog in hello filetest forktest shelltest dirtest stattest \
            touchtest rmtest mvtest lstest stdintest dup2test \
            pingtest udptest tcptest sockettest dnstest auxvprobe \
            tlsprobe errnoprobe vmprobe vmfault vmro forkro cowprobe; do
    if [ -f "../tests/$prog" ]; then
        cp "../tests/$prog" "$MOUNT_POINT/"
        echo "  $prog"
    fi
done

# Copy the statically-linked musl program (Phase 3 integration milestone), if
# built. It lives under tests/musl and is linked against the ported libc. It is
# installed as "mhello" so it does not collide with the small freestanding test
# named "hello" (the boot self-test's test_elf() reads a hardcoded "hello" into
# an 8 KB scratch buffer that a real musl binary, ~20 KB, would not fit).
if [ -f "../tests/musl/hello" ]; then
    cp "../tests/musl/hello" "$MOUNT_POINT/mhello"
    echo "  mhello (musl)"
fi

# The Phase 4 malloc/qsort integration program, installed as "mqsort".
if [ -f "../tests/musl/mallocqsort" ]; then
    cp "../tests/musl/mallocqsort" "$MOUNT_POINT/mqsort"
    echo "  mqsort (musl)"
fi

# The Phase 5 fork/wait integration program, installed as "mcowfork".
if [ -f "../tests/musl/cowfork" ]; then
    cp "../tests/musl/cowfork" "$MOUNT_POINT/mcowfork"
    echo "  mcowfork (musl)"
fi

# Also copy hello.txt for file tests
echo "hello, world" > "$MOUNT_POINT/hello.txt"
echo "  hello.txt (test data)"

# Name-resolution config for the network tests (dnstest). KRFFS has no
# directories, so these live in the flat root; the resolver opens them as
# /resolv.conf and /hosts. nameserver 10.0.2.3 is SLIRP's DNS forwarder.
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
