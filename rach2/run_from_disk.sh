#!/bin/sh

# Boot the bootable Rach2 disk image in QEMU, BIOS -> MBR -> GRUB -> kernel,
# with no -kernel or -initrd: the kernel and GRUB come off the disk's boot
# partition and KRFFS off its second partition (rach2.root=disk). Build the
# disk first with `make os-real-grub-disk`, which names it
# os-real-grub-with-key.disk in a tree with a recovery key; the script boots
# whichever of the two names exists, the plain one first.
#
# -serial stdio puts the kernel's COM1 on the terminal; audio and networking
# come from qemu_devices.sh. SSH is forwarded to host port 2222 so the capstone
# (ssh -i krffs/rach2_demo_ed25519 -p 2222 root@localhost) reaches dropbear.
#
# Usage: ./run_from_disk.sh [audio_device]
#
# ARCH=x86_64 boots the x86-64 build's disk on qemu-system-x86_64.

AUDIO_DEVICE="${1:-speaker}"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

# This image has the SSH capstone, so forward host port 2222 to guest port
# 22 by default. NET_HOSTFWD must be set before sourcing qemu_devices.sh, whose
# own default (8080 -> 80) would otherwise win; an exported NET_HOSTFWD still
# overrides.
NET_HOSTFWD="${NET_HOSTFWD:-hostfwd=tcp::2222-:22}"
. "$SCRIPT_DIR/qemu_devices.sh"

DISK="$BUILD_DIR/images/os-real-grub.disk"
[ -f "$DISK" ] || DISK="$BUILD_DIR/images/os-real-grub-with-key.disk"

if [ ! -f "$DISK" ]; then
    echo "run: no disk image os-real-grub.disk or os-real-grub-with-key.disk in $BUILD_DIR/images; build one with 'make ${MAKE_ARCH}os-real-grub-disk'" >&2
    exit 1
fi

# The disk owns its kernel state: /boot/kernel is the candidate slot the
# machine itself rewrites with bootctl, /boot/fallback the untouched
# copy, and grubenv selects between them. A host kernel newer than the disk
# is therefore not a reason to rebuild it; `make os-real-grub-disk` recreates
# the disk from the host build and DISCARDS anything installed on it.

# -cpu max exposes RDSEED/RDRAND so the CSPRNG seeds from trusted hardware
# entropy (dropbear key generation needs it).
CPU="${CPU:-max}"

"$QEMU" -cpu "$CPU" \
    -drive "file=$DISK,format=raw,if=ide,index=0,media=disk" \
    -serial stdio $AUDIO_OPTS $NET_OPTS $DISPLAY_OPTS
