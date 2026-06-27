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

IMAGE="$1"
AUDIO_DEVICE="${2:-speaker}"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
. "$SCRIPT_DIR/qemu_devices.sh"

# Pass the kernel a rach2.rescue command line when RESCUE is set, so it boots the
# debug shell rather than the persistent init.
APPEND=""
if [ -n "$RESCUE" ]; then
    APPEND="-append rach2.rescue"
fi

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

qemu-system-i386 -cpu "$CPU" -kernel "$KERNEL" $APPEND "$@" -serial stdio $AUDIO_OPTS $NET_OPTS
