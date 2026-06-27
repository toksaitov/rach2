#!/bin/sh

# Start the kernel in QEMU from the ISO image
# -serial stdio redirects COM1 output to terminal for debugging
# -cdrom loads an ISO disk and attempts to boot from it
#
# Call `make iso` to generate the ISO image first
#
# Usage: ./run_from_iso.sh [audio_device]
#   audio_device: sb16, hda, or speaker (default: speaker)
#
# Examples:
#   ./run_from_iso.sh      # PC speaker only (default)
#   ./run_from_iso.sh sb16 # Sound Blaster 16
#   ./run_from_iso.sh hda  # Intel HD Audio
#
# Networking (RTL8139 on QEMU's SLIRP user network) is enabled by default:
#   NET=off ./run_from_iso.sh                              # disable it
#   NET_HOSTFWD="hostfwd=tcp::8080-:80" ./run_from_iso.sh  # custom forwards

AUDIO_DEVICE="${1:-speaker}"
. "$(dirname "$0")/qemu_devices.sh"

qemu-system-i386 -cdrom ../build/rach2/bin/boot.iso -serial stdio $AUDIO_OPTS $NET_OPTS
