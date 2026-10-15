#!/bin/sh

# Start a Rach2 RAM-disk image in QEMU.
# -serial stdio redirects COM1 output to the terminal.
#
# Usage: ./run.sh [image] [audio_device]
#   image:        test | os-simple | os-real, which boots
#                 build/images/<image>.krffs as the RAM disk, or
#                 build/x86_64/images/<image>.krffs under the x86-64 kernel
#                 when ARCH=x86_64. Without it the kernel boots alone and,
#                 with no root to mount, halts, unless RESCUE=1 asks for the
#                 kernel debug shell or DISK gives it a root.
#   audio_device: sb16, hda, or speaker (default: speaker)
# Audio and networking devices are attached (see qemu_devices.sh).
#
# Examples:
#   ./run.sh os-simple        # the project's login, the ish shell, utils/
#   ./run.sh os-real sb16     # the busybox/dropbear environment, Sound Blaster
#   ./run.sh test             # the automated test image (kernel debug shell)
#   ARCH=x86_64 ./run.sh os-real  # the same system on the x86-64 kernel
#
# Networking (on QEMU's SLIRP user network) is enabled by default:
#   NET=off ./run.sh os-real                              # no network card
#   NET_HOSTFWD="hostfwd=tcp::2222-:22" ./run.sh os-real  # forward SSH to :2222
#   NIC=e1000 ./run.sh os-real                            # the other card
#
# With no X or Wayland display, as in an ssh session on Linux, QEMU opens no
# window and the console is the terminal alone; DISPLAY_MODE=none, gtk, sdl or
# cocoa chooses instead (see qemu_devices.sh).
#
# Both os-simple and os-real start at a login prompt. `make` makes the demo
# passwords when they are absent: krffs/rach2_demo_password is root's, and
# krffs/rach2_test_password the unprivileged test account's; neither is
# committed. The same /etc/shadow is staged into both images.
#
# RESCUE=1 boots into the kernel debug shell instead of the image's /sbin/init,
# for developing and debugging (a normal boot of an image with an init runs it
# and does not reach the shell):
#   RESCUE=1 ./run.sh os-real
#
# DISK=<file> attaches a raw image as the primary master ATA disk (if=ide,
# index=0) and mounts KRFFS from it (rach2.fsdisk), so what boots is the
# writable Rach2, not the RAM module. This is the persistent machine:
#   make os-real-disk
#   DISK=../build/images/os-real.disk ./run.sh
#
# The page cache is write-back, so an edit is durable after a `sync` (or after
# `passwd`, which flushes for you) and not before. Stop the machine with `halt
# -f`, `poweroff -f` or `reboot -f`, which reach reboot(2) and flush before they
# clear the dirty-mount flag; a boot that is simply killed leaves the flag set
# and the next one prints the fsck advice.
#
# DISKNOMOUNT=1 attaches the disk without mounting it: the kernel mounts the
# RAM module, if one is given, and the debug shell's `disk` command reads the
# disk as an unmounted device:
#   DISK=../build/images/os-real.disk DISKNOMOUNT=1 RESCUE=1 ./run.sh
#
# DISKBUS=ahci attaches that same DISK to an ich9-ahci SATA controller instead,
# so the root is read through the AHCI driver, and appends rach2.disk=ahci,
# which the kernel accepts and ignores, since its probe tries AHCI first:
#   DISK=../build/images/os-real.disk DISKBUS=ahci ./run.sh os-real

IMAGE="$1"
AUDIO_DEVICE="${2:-speaker}"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
. "$SCRIPT_DIR/qemu_devices.sh"

# -cpu max exposes RDSEED/RDRAND so the kernel CSPRNG seeds from trusted
# hardware entropy; without it getrandom and /dev/[u]random fail closed and SSH
# key generation (dropbear, in the os-real image) cannot run. Override CPU=...
# to test.
CPU="${CPU:-max}"

KERNEL="$BUILD_DIR/rach2/kernel"

if [ -n "$IMAGE" ]; then
    INITRD="$BUILD_DIR/images/$IMAGE.krffs"
    if [ ! -f "$INITRD" ]; then
        echo "run: image '$INITRD' not found; build it with 'make $MAKE_ARCH$IMAGE-image'" >&2
        exit 1
    fi
    set -- -initrd "$INITRD"
else
    set --
fi

# The disk: if=ide with index=0 and media=disk puts it on the primary ATA
# channel at port 0x1F0, for the ATA driver; DISKBUS=ahci attaches the same
# image to an ich9-ahci controller instead (if=none and ide-hd), for the AHCI
# driver.
if [ -n "$DISK" ]; then
    if [ ! -f "$DISK" ]; then
        echo "run: disk image '$DISK' not found; build it with 'make os-real-disk'" >&2
        exit 1
    fi
    if [ "$DISKBUS" = ahci ]; then
        set -- "$@" -drive "file=$DISK,format=raw,if=none,id=drv0" \
                    -device ich9-ahci,id=ahci0 \
                    -device ide-hd,drive=drv0,bus=ahci0.0
    else
        set -- "$@" -drive "file=$DISK,format=raw,if=ide,index=0,media=disk"
    fi
fi

# The kernel command line. RESCUE asks for the debug shell; DISK mounts KRFFS
# from the disk, on either bus, unless DISKNOMOUNT is set.
CMDLINE=""
if [ -n "$RESCUE" ]; then
    CMDLINE="$CMDLINE rach2.rescue"
fi
if [ -n "$DISK" ]; then
    if [ "$DISKBUS" = ahci ]; then
        CMDLINE="$CMDLINE rach2.disk=ahci"
    fi
    if [ -z "$DISKNOMOUNT" ]; then
        CMDLINE="$CMDLINE rach2.fsdisk"
    fi
fi
CMDLINE="${CMDLINE# }"
if [ -n "$CMDLINE" ]; then
    set -- "$@" -append "$CMDLINE"
fi

"$QEMU" -cpu "$CPU" -kernel "$KERNEL" "$@" -serial stdio $AUDIO_OPTS $NET_OPTS $DISPLAY_OPTS
