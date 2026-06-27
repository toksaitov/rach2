#!/bin/sh

# Start the kernel in QEMU, call `make` to generate the kernel
# -serial stdio redirects COM1 output to terminal for debugging
#
# Usage: ./run.sh [audio_device]
#   audio_device: sb16, hda, or speaker (default: speaker)
# Audio and networking devices are attached (see qemu_devices.sh).

AUDIO_DEVICE="${1:-speaker}"
. "$(dirname "$0")/qemu_devices.sh"

# -cpu max exposes RDSEED/RDRAND so the kernel CSPRNG seeds from trusted hardware
# entropy; without it getrandom + /dev/[u]random fail closed and SSH
# key generation (dropbear) cannot run. Override with CPU=... to test fail-closed.
CPU="${CPU:-max}"

qemu-system-i386 -cpu "$CPU" -kernel ../build/rach2/kernel -serial stdio $AUDIO_OPTS $NET_OPTS
