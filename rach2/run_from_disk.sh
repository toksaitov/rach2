#!/bin/sh

# Boot the bootable Rach2 disk image in QEMU, BIOS -> MBR -> GRUB -> kernel,
# with no -kernel or -initrd: the kernel and GRUB come off the disk's boot
# partition and KRFFS off its second partition (rach2.root=disk). Build the
# disk first with `make os-real-grub-disk`.
#
# -serial stdio puts the kernel's COM1 on the terminal; audio and networking
# come from qemu_devices.sh. SSH is forwarded to host port 2222 so the capstone
# (ssh -i krffs/rach2_demo_ed25519 -p 2222 root@localhost) reaches dropbear.
#
# Usage: ./run_from_disk.sh [audio_device]

AUDIO_DEVICE="${1:-speaker}"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

# This image carries the SSH capstone, so forward host port 2222 to guest port
# 22 by default. NET_HOSTFWD must be set before sourcing qemu_devices.sh, whose
# own default (8080 -> 80) would otherwise win; an exported NET_HOSTFWD still
# overrides.
NET_HOSTFWD="${NET_HOSTFWD:-hostfwd=tcp::2222-:22}"
. "$SCRIPT_DIR/qemu_devices.sh"

DISK="$SCRIPT_DIR/../build/images/os-real-grub.disk"
KERNEL="$SCRIPT_DIR/../build/rach2/kernel"

if [ ! -f "$DISK" ]; then
    echo "run: disk image '$DISK' not found; build it with 'make os-real-grub-disk'" >&2
    exit 1
fi

# GRUB does not reload a fresh kernel: the disk holds a copy of the built
# kernel. Warn if it is newer than the disk, so a stale-kernel boot is caught.
if [ -f "$KERNEL" ] && [ "$KERNEL" -nt "$DISK" ]; then
    echo "run: warning: $KERNEL is newer than the disk; rebuild with 'make os-real-grub-disk'" >&2
fi

# -cpu max exposes RDSEED/RDRAND so the CSPRNG seeds from trusted hardware
# entropy (dropbear key generation needs it).
CPU="${CPU:-max}"

qemu-system-i386 -cpu "$CPU" \
    -drive "file=$DISK,format=raw,if=ide,index=0,media=disk" \
    -serial stdio $AUDIO_OPTS $NET_OPTS
