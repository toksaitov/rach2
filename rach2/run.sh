#!/bin/sh

# Start the kernel in QEMU, call `make` to generate the kernel
# -serial stdio redirects COM1 output to terminal for debugging
#
# Usage: ./run.sh [audio_device]
#   audio_device: sb16, hda, or speaker (default: speaker)
# Audio and networking devices are attached (see qemu_devices.sh).

AUDIO_DEVICE="${1:-speaker}"
. "$(dirname "$0")/qemu_devices.sh"

qemu-system-i386 -kernel kernel -serial stdio $AUDIO_OPTS $NET_OPTS
