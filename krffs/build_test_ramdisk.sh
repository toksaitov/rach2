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
            tlsprobe errnoprobe vmprobe vmfault vmro forkro cowprobe dirprobe \
            timeprobe sigprobe ofdprobe waitprobe pipeprobe pollprobe devprobe \
            ttyprobe jobprobe miscprobe; do
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

# The Phase 6 directory/file navigation program, installed as "mcatfile".
if [ -f "../tests/musl/catfile" ]; then
    cp "../tests/musl/catfile" "$MOUNT_POINT/mcatfile"
    echo "  mcatfile (musl)"
fi

# The Phase 7 time/sleep/random program, installed as "mtimemusl".
if [ -f "../tests/musl/timemusl" ]; then
    cp "../tests/musl/timemusl" "$MOUNT_POINT/mtimemusl"
    echo "  mtimemusl (musl)"
fi

# The Phase 8 signals program, installed as "msigmusl".
if [ -f "../tests/musl/sigmusl" ]; then
    cp "../tests/musl/sigmusl" "$MOUNT_POINT/msigmusl"
    echo "  msigmusl (musl)"
fi

# The Phase 8 Ctrl-C interrupt program, installed as "mctrlc".
if [ -f "../tests/musl/ctrlcmusl" ]; then
    cp "../tests/musl/ctrlcmusl" "$MOUNT_POINT/mctrlc"
    echo "  mctrlc (musl)"
fi

# The Phase 10 TCP send()/recv() round-trip program, installed as "mtcpsr".
if [ -f "../tests/musl/tcpsendrecv" ]; then
    cp "../tests/musl/tcpsendrecv" "$MOUNT_POINT/mtcpsr"
    echo "  mtcpsr (musl)"
fi

# The Phase 11 shared-open-file-offset program, installed as "mofd".
if [ -f "../tests/musl/ofdmusl" ]; then
    cp "../tests/musl/ofdmusl" "$MOUNT_POINT/mofd"
    echo "  mofd (musl)"
fi

# The Phase 13 pipe+fork round-trip program, installed as "mpipe".
if [ -f "../tests/musl/pipemusl" ]; then
    cp "../tests/musl/pipemusl" "$MOUNT_POINT/mpipe"
    echo "  mpipe (musl)"
fi

# The Phase 14 poll()/select() program, installed as "mpoll".
if [ -f "../tests/musl/pollmusl" ]; then
    cp "../tests/musl/pollmusl" "$MOUNT_POINT/mpoll"
    echo "  mpoll (musl)"
fi

# The Phase 15 /dev + /proc + ttyname program, installed as "mdevtest".
if [ -f "../tests/musl/devtest" ]; then
    cp "../tests/musl/devtest" "$MOUNT_POINT/mdevtest"
    echo "  mdevtest (musl)"
fi

# The Phase 16 termios program (tcgetattr/cfmakeraw/raw getch), as "mtty".
if [ -f "../tests/musl/ttymusl" ]; then
    cp "../tests/musl/ttymusl" "$MOUNT_POINT/mtty"
    echo "  mtty (musl)"
fi

# The Phase 9 musl-linked Unix utilities, under their real names so the combined
# boot can exercise them (e.g. "exec cat hello.txt", "exec ls", "exec echo hi").
for util in true false echo yes cat pwd touch mkdir rm mv cp ls ping nc httpd; do
    if [ -f "../utils/$util" ]; then
        cp "../utils/$util" "$MOUNT_POINT/"
        echo "  $util (musl util)"
    fi
done

# The Phase 18 busybox capstone (static musl, ~190 KB), if built.
if [ -f "../tests/busybox" ]; then
    cp "../tests/busybox" "$MOUNT_POINT/busybox"
    echo "  busybox (Phase 18)"
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

# Stage a real directory tree (/etc) directly into the unmounted image, so the
# guest boots with populated directories (not just a flat root). This writes v2
# hierarchical nodes the kernel resolver reads -- proving the host tools speak the
# directory format, the integration half of Phase 6.
echo ""
echo "=== Staging directory tree (/etc) ==="
./stagetree.krffs "$FS_FILE"

echo ""
echo "=== Done ==="
echo "RAM disk image: $FS_FILE"
echo ""
echo "To use with QEMU:"
echo "  qemu-system-i386 -kernel ../rach2/kernel -initrd $FS_FILE -serial stdio"
