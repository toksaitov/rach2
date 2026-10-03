#!/bin/sh

# Start a Rach2 RAM-disk image in QEMU.
# -serial stdio redirects COM1 output to the terminal.
#
# Usage: ./run.sh [image] [audio_device]
#   image:        test | os-simple | os-real  (boots build/images/<image>.krffs
# as the RAM disk, or build/x86_64/images/<image>.krffs under the x86-64
# kernel when ARCH=x86_64). Omit it to boot the kernel alone, into the kernel debug
# shell. audio_device: sb16, hda, or speaker (default: speaker) Audio and
# networking devices are attached (see qemu_devices.sh).
#
# Examples:
#   ./run.sh os-simple        # our login, the ish shell, and our Unix tools
#   ./run.sh os-real sb16     # the busybox/dropbear environment, Sound Blaster
#   ./run.sh test             # the automated test image (kernel debug shell)
#   ARCH=x86_64 ./run.sh os-real  # the same system on the x86-64 kernel
#
# Networking (on QEMU's SLIRP user network) is enabled by default:
#   NET=off ./run.sh os-real                              # disable it
#   NET_HOSTFWD="hostfwd=tcp::2222-:22" ./run.sh os-real  # forward SSH to :2222
#   NIC=e1000 ./run.sh os-real                            # the other card
#
# Both os-simple and os-real ask who is there. `make` mints the demo passwords
# when they are absent: krffs/rach2_demo_password is root's, and
# krffs/rach2_test_password the unprivileged test account's; neither is
# committed. The same /etc/shadow is staged into both images.
#
# RESCUE=1 boots into the kernel debug shell instead of the image's /sbin/init,
# for developing and debugging (a normal boot runs init and never drops there):
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
# DISKNOMOUNT=1 attaches the disk without mounting it, which is the older
# recipe: the kernel boots the RAM module and the debug shell's `disk` command
# probes the platter as an unmounted device. It is kept for exactly that, and
# for nothing else:
#   DISK=../build/images/os-real.disk DISKNOMOUNT=1 RESCUE=1 ./run.sh
#
# DISKBUS=ahci attaches that same DISK on an ich9-ahci SATA HBA instead, and
# appends rach2.disk=ahci (select the AHCI backend) beside the mount, so the
# root file system rides the AHCI driver:
#   DISK=../build/images/os-real.disk DISKBUS=ahci ./run.sh os-real

IMAGE="$1"
AUDIO_DEVICE="${2:-speaker}"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
. "$SCRIPT_DIR/qemu_devices.sh"

# -cpu max exposes RDSEED/RDRAND so the kernel CSPRNG seeds from trusted
# hardware entropy; without it getrandom + /dev/[u]random fail closed and SSH
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

# A real disk: index=0,media=disk on if=ide lands a plain READ/WRITE SECTORS
# drive at 0x1F0 for the polled ATA driver; DISKBUS=ahci instead hangs the same
# image off an ich9-ahci HBA (if=none + ide-hd) for the polled AHCI driver.
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

# Assemble the kernel command line. RESCUE drops to the debug shell; DISK mounts
# KRFFS from the platter and DISKBUS=ahci selects the AHCI backend under it.
CMDLINE=""
if [ -n "$RESCUE" ]; then
    CMDLINE="$CMDLINE rach2.rescue"
fi
# A disk that is attached is a disk that is mounted, on either bus: the two
# flags are separate because the selector alone boots without mounting, and
# DISKNOMOUNT is the way to ask for that on purpose, not by forgetting.
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

"$QEMU" -cpu "$CPU" -kernel "$KERNEL" "$@" -serial stdio $AUDIO_OPTS $NET_OPTS
