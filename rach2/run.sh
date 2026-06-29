#!/bin/sh

# Start a Rach2 RAM-disk image in QEMU.
# -serial stdio redirects COM1 output to the terminal.
#
# Usage: ./run.sh [image] [audio_device]
#   image:        test | os-simple | os-real  (boots build/images/<image>.krffs as
#                 the RAM disk). Omit it to boot the kernel alone, into the kernel
#                 debug shell.
#   audio_device: sb16, hda, or speaker (default: speaker)
# Audio and networking devices are attached (see qemu_devices.sh).
#
# Examples:
#   ./run.sh os-simple        # the ish shell + our simple Unix tools
#   ./run.sh os-real sb16     # the busybox/dropbear environment, Sound Blaster 16
#   ./run.sh test             # the automated test image (kernel debug shell)
#
# Networking (RTL8139 on QEMU's SLIRP user network) is enabled by default:
#   NET=off ./run.sh os-real                              # disable it
#   NET_HOSTFWD="hostfwd=tcp::2222-:22" ./run.sh os-real  # forward SSH to :2222
#
# RESCUE=1 boots into the kernel debug shell instead of the image's /sbin/init,
# for developing and debugging (a normal boot runs init and never drops there):
#   RESCUE=1 ./run.sh os-real
#
# DISK=<file> attaches a raw image as the primary master ATA disk (if=ide,
# index=0), so the kernel debug shell's `disk` command can probe it:
#   make os-real-disk
#   DISK=../build/images/os-real.disk RESCUE=1 ./run.sh
#
# DISKBUS=ahci attaches that same DISK on an ich9-ahci SATA HBA instead, and
# appends rach2.disk=ahci (select the AHCI backend) + rach2.fsdisk (mount KRFFS
# from the disk), so the root file system rides the AHCI driver:
#   DISK=../build/images/os-real.disk DISKBUS=ahci ./run.sh os-real

IMAGE="$1"
AUDIO_DEVICE="${2:-speaker}"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
. "$SCRIPT_DIR/qemu_devices.sh"

# -cpu max exposes RDSEED/RDRAND so the kernel CSPRNG seeds from trusted hardware
# entropy; without it getrandom + /dev/[u]random fail closed and SSH key
# generation (dropbear, in the os-real image) cannot run. Override CPU=... to test.
CPU="${CPU:-max}"

KERNEL="$SCRIPT_DIR/../build/rach2/kernel"

if [ -n "$IMAGE" ]; then
    INITRD="$SCRIPT_DIR/../build/images/$IMAGE.krffs"
    if [ ! -f "$INITRD" ]; then
        echo "run: image '$INITRD' not found; build it with 'make $IMAGE-image'" >&2
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

# Assemble the kernel command line. RESCUE drops to the debug shell; DISKBUS=ahci
# selects the AHCI backend and mounts KRFFS from the disk (the two flags are
# orthogonal: the selector alone boots but does not mount).
CMDLINE=""
if [ -n "$RESCUE" ]; then
    CMDLINE="$CMDLINE rach2.rescue"
fi
if [ -n "$DISK" ] && [ "$DISKBUS" = ahci ]; then
    CMDLINE="$CMDLINE rach2.disk=ahci rach2.fsdisk"
fi
CMDLINE="${CMDLINE# }"
if [ -n "$CMDLINE" ]; then
    set -- "$@" -append "$CMDLINE"
fi

qemu-system-i386 -cpu "$CPU" -kernel "$KERNEL" "$@" -serial stdio $AUDIO_OPTS $NET_OPTS
