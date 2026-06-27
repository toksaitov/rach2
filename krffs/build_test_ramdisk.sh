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

# Everything builds out of tree under build/ at the repo root (mk/build.mk): the host
# krffs tools, the probe and musl binaries, the staged capstones, and the image this
# script writes. The demo SSH keypair is a host-side test credential, not a build
# product, so it stays beside this script in krffs/ (the documented
# `ssh -i krffs/rach2_demo_ed25519` path).
BUILD="$(cd "$SCRIPT_DIR/.." && pwd)/build"
TOOLS="$BUILD/krffs"

SIZE=${1:-10}
MOUNT_POINT="$BUILD/krffs/mount_point"
FS_FILE="$BUILD/krffs/file_system.krffs"
DEMO_KEY="rach2_demo_ed25519"

# Provision a demo SSH keypair for the dropbear login if the operator
# has not supplied one. Only the PUBLIC half is staged (into
# /root/.ssh/authorized_keys by stagetree below); the private half stays on the
# host; log in with `ssh -i krffs/rach2_demo_ed25519 -p 2222 root@localhost`.
# Both are git-ignored test credentials and are never committed.
if [ ! -f authorized_keys ]; then
    if command -v ssh-keygen >/dev/null 2>&1; then
        echo "ramdisk: generating the demo ssh key ($DEMO_KEY)"
        rm -f "$DEMO_KEY" "$DEMO_KEY.pub"
        ssh-keygen -t ed25519 -N "" -C "rach2-demo" -f "$DEMO_KEY" >/dev/null
        cp "$DEMO_KEY.pub" authorized_keys
        echo "  private key: krffs/$DEMO_KEY"
        echo "  log in with: ssh -i krffs/$DEMO_KEY -p 2222 root@localhost"
    else
        echo "ramdisk: no authorized_keys and no ssh-keygen; dropbear SSH login will lack a key" >&2
    fi
fi

# Build the test programs, unless Make already built them (SKIP_BUILD=1, set by the
# `make test-ramdisk` target so nothing is built twice); a standalone run still
# builds them for convenience.
if [ "${SKIP_BUILD:-0}" != 1 ]; then
    echo "ramdisk: building the test programs"
    make -C ../tests
fi

echo "ramdisk: creating a $SIZE MB image"
mkdir -p "$BUILD/krffs"
rm -f "$FS_FILE"
dd if=/dev/zero of="$FS_FILE" bs=1048576 count=$SIZE
"$TOOLS/mkfs.krffs" "$FS_FILE"

echo "ramdisk: mounting"
mkdir -p "$MOUNT_POINT"
# Clear mount point if not empty
rm -rf "${MOUNT_POINT:?}"/*
"$TOOLS/krffs" "$FS_FILE" "$MOUNT_POINT"

# Always unmount on exit, so a crash between here and the explicit unmount below never
# leaves a live FUSE mount for `make clean` (which removes build/krffs) to recurse into.
trap 'fusermount -u "$MOUNT_POINT" 2>/dev/null || umount "$MOUNT_POINT" 2>/dev/null || true' EXIT INT TERM

echo "ramdisk: staging test binaries"

# Copy all test programs
for prog in hello filetest forktest shelltest dirtest stattest \
            touchtest rmtest mvtest lstest stdintest dup2test \
            pingtest udptest tcptest sockettest dnstest auxvprobe \
            tlsprobe errnoprobe vmprobe vmfault vmro forkro cowprobe dirprobe \
            timeprobe sigprobe ofdprobe waitprobe pipeprobe pollprobe devprobe \
            ttyprobe jobprobe miscprobe ptyprobe rngprobe; do
    if [ -f "$BUILD/tests/$prog" ]; then
        cp "$BUILD/tests/$prog" "$MOUNT_POINT/"
        echo "  $prog"
    fi
done

# Copy the statically-linked musl program if built. It lives under tests/musl and is
# linked against the ported libc. It is
# installed as "mhello" so it does not collide with the small freestanding test
# named "hello" (the boot self-test's test_elf() reads a hardcoded "hello" into
# an 8 KB scratch buffer that a real musl binary, ~20 KB, would not fit).
if [ -f "$BUILD/tests/musl/hello" ]; then
    cp "$BUILD/tests/musl/hello" "$MOUNT_POINT/mhello"
    echo "  mhello (musl)"
fi

# The malloc/qsort program, installed as "mqsort".
if [ -f "$BUILD/tests/musl/mallocqsort" ]; then
    cp "$BUILD/tests/musl/mallocqsort" "$MOUNT_POINT/mqsort"
    echo "  mqsort (musl)"
fi

# The fork/wait program, installed as "mcowfork".
if [ -f "$BUILD/tests/musl/cowfork" ]; then
    cp "$BUILD/tests/musl/cowfork" "$MOUNT_POINT/mcowfork"
    echo "  mcowfork (musl)"
fi

# The directory/file navigation program, installed as "mcatfile".
if [ -f "$BUILD/tests/musl/catfile" ]; then
    cp "$BUILD/tests/musl/catfile" "$MOUNT_POINT/mcatfile"
    echo "  mcatfile (musl)"
fi

# The time/sleep/random program, installed as "mtimemusl".
if [ -f "$BUILD/tests/musl/timemusl" ]; then
    cp "$BUILD/tests/musl/timemusl" "$MOUNT_POINT/mtimemusl"
    echo "  mtimemusl (musl)"
fi

# The signals program, installed as "msigmusl".
if [ -f "$BUILD/tests/musl/sigmusl" ]; then
    cp "$BUILD/tests/musl/sigmusl" "$MOUNT_POINT/msigmusl"
    echo "  msigmusl (musl)"
fi

# The Ctrl-C interrupt program, installed as "mctrlc".
if [ -f "$BUILD/tests/musl/ctrlcmusl" ]; then
    cp "$BUILD/tests/musl/ctrlcmusl" "$MOUNT_POINT/mctrlc"
    echo "  mctrlc (musl)"
fi

# The TCP send()/recv() round-trip program, installed as "mtcpsr".
if [ -f "$BUILD/tests/musl/tcpsendrecv" ]; then
    cp "$BUILD/tests/musl/tcpsendrecv" "$MOUNT_POINT/mtcpsr"
    echo "  mtcpsr (musl)"
fi

# The shared-open-file-offset program, installed as "mofd".
if [ -f "$BUILD/tests/musl/ofdmusl" ]; then
    cp "$BUILD/tests/musl/ofdmusl" "$MOUNT_POINT/mofd"
    echo "  mofd (musl)"
fi

# The pipe+fork round-trip program, installed as "mpipe".
if [ -f "$BUILD/tests/musl/pipemusl" ]; then
    cp "$BUILD/tests/musl/pipemusl" "$MOUNT_POINT/mpipe"
    echo "  mpipe (musl)"
fi

# The poll()/select() program, installed as "mpoll".
if [ -f "$BUILD/tests/musl/pollmusl" ]; then
    cp "$BUILD/tests/musl/pollmusl" "$MOUNT_POINT/mpoll"
    echo "  mpoll (musl)"
fi

# The /dev + /proc + ttyname program, installed as "mdevtest".
if [ -f "$BUILD/tests/musl/devtest" ]; then
    cp "$BUILD/tests/musl/devtest" "$MOUNT_POINT/mdevtest"
    echo "  mdevtest (musl)"
fi

# The termios program (tcgetattr/cfmakeraw/raw getch), as "mtty".
if [ -f "$BUILD/tests/musl/ttymusl" ]; then
    cp "$BUILD/tests/musl/ttymusl" "$MOUNT_POINT/mtty"
    echo "  mtty (musl)"
fi

# The openpty()+fork pty round-trip program, installed as "mpty".
if [ -f "$BUILD/tests/musl/ptymusl" ]; then
    cp "$BUILD/tests/musl/ptymusl" "$MOUNT_POINT/mpty"
    echo "  mpty (musl)"
fi

# The live one-session budget probe, installed as "mbudget".
if [ -f "$BUILD/tests/musl/budgetmusl" ]; then
    cp "$BUILD/tests/musl/budgetmusl" "$MOUNT_POINT/mbudget"
    echo "  mbudget (musl)"
fi

# The getrandom + /dev/urandom keygen-shape program, as "mrng".
if [ -f "$BUILD/tests/musl/rngmusl" ]; then
    cp "$BUILD/tests/musl/rngmusl" "$MOUNT_POINT/mrng"
    echo "  mrng (musl)"
fi

# The socket server-side fill-out probe, installed as "msockfill".
if [ -f "$BUILD/tests/musl/sockfillmusl" ]; then
    cp "$BUILD/tests/musl/sockfillmusl" "$MOUNT_POINT/msockfill"
    echo "  msockfill (musl)"
fi

# The auth/identity + tail-syscall probe, installed as "mauth".
if [ -f "$BUILD/tests/musl/authmusl" ]; then
    cp "$BUILD/tests/musl/authmusl" "$MOUNT_POINT/mauth"
    echo "  mauth (musl)"
fi

# The permission/identity probe (fsync/link/chmod/chown/seteuid),
# installed as "mperm".
if [ -f "$BUILD/tests/musl/permmusl" ]; then
    cp "$BUILD/tests/musl/permmusl" "$MOUNT_POINT/mperm"
    echo "  mperm (musl)"
fi

# The musl-linked Unix utilities, under their real names so the combined
# boot can exercise them (e.g. "exec cat hello.txt", "exec ls", "exec echo hi").
for util in true false echo yes cat pwd touch mkdir rm mv cp ls ping nc httpd; do
    if [ -f "$BUILD/utils/$util" ]; then
        cp "$BUILD/utils/$util" "$MOUNT_POINT/"
        echo "  $util (musl util)"
    fi
done

# The busybox capstone (static musl, ~190 KB), if built.
if [ -f "$BUILD/vendor/busybox/busybox" ]; then
    cp "$BUILD/vendor/busybox/busybox" "$MOUNT_POINT/busybox"
    echo "  busybox"
    # Also install it as the flat-root login shell /sh: dropbear execs
    # the /etc/passwd shell, and busybox invoked with basename "sh" runs ash. A
    # flat-root copy works through the krffs FUSE mount (which cannot mkdir).
    cp "$BUILD/vendor/busybox/busybox" "$MOUNT_POINT/sh"
    echo "  sh (busybox login shell for dropbear)"
fi

# The dropbear SSH server (static musl), if built.
if [ -f "$BUILD/vendor/dropbear/dropbearmulti" ]; then
    cp "$BUILD/vendor/dropbear/dropbearmulti" "$MOUNT_POINT/dropbearmulti"
    echo "  dropbearmulti"
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

echo "ramdisk: unmounting"
# Try fusermount first, fall back to umount
if command -v fusermount >/dev/null 2>&1; then
    fusermount -u "$MOUNT_POINT"
elif command -v umount >/dev/null 2>&1; then
    umount "$MOUNT_POINT"
else
    echo "ramdisk: no fusermount or umount available" >&2
    exit 1
fi

# Stage a real directory tree (/etc) directly into the unmounted image, so the
# guest boots with populated directories (not just a flat root). This writes v2
# hierarchical nodes the kernel resolver reads, proving the host tools speak the
# directory format.
echo "ramdisk: staging the /etc directory tree"
"$TOOLS/stagetree.krffs" "$FS_FILE"

echo "ramdisk: wrote $FS_FILE"
echo "ramdisk: boot with qemu-system-i386 -kernel $BUILD/rach2/kernel -initrd $FS_FILE -serial stdio"
