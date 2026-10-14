#!/bin/sh

# Boot a Rach2 ISO image in QEMU (build it first with `make <image>-iso`).
# -cdrom boots the ISO through GRUB; -serial stdio shows the console.
#
# Usage: ./run_from_iso.sh <image> [audio_device]
#   image:        test | os-simple | os-real  (boots build/images/<image>.iso,
#                 or build/x86_64/images/<image>.iso when ARCH=x86_64)
#   audio_device: sb16, hda, or speaker (default: speaker)
#
# Examples:
#   ./run_from_iso.sh os-real        # the busybox/dropbear environment
#   ./run_from_iso.sh os-simple sb16 # our shell + tools, Sound Blaster 16
#
# Networking (on QEMU's SLIRP user network) is enabled by default:
#   NET=off ./run_from_iso.sh os-real                              # disable it
#   NET_HOSTFWD="hostfwd=tcp::2222-:22" ./run_from_iso.sh os-real  # forward SSH
#   NIC=e1000 ./run_from_iso.sh os-real                 # the other card

IMAGE="$1"
AUDIO_DEVICE="${2:-speaker}"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
. "$SCRIPT_DIR/qemu_devices.sh"

# -cpu max for trusted entropy (the os-real SSH login needs it; see run.sh).
CPU="${CPU:-max}"

if [ -z "$IMAGE" ]; then
    echo "usage: ./run_from_iso.sh <test|os-simple|os-real> [audio]" >&2
    exit 2
fi
ISO="$BUILD_DIR/images/$IMAGE.iso"
if [ ! -f "$ISO" ]; then
    echo "run_from_iso: $ISO not found; build it with 'make $MAKE_ARCH$IMAGE-iso'" >&2
    exit 1
fi

"$QEMU" -cpu "$CPU" -cdrom "$ISO" -serial stdio $AUDIO_OPTS $NET_OPTS $DISPLAY_OPTS
