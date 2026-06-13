#!/bin/sh

# Start the kernel in QEMU with RAM disk
# -serial stdio redirects COM1 output to terminal for debugging
# -initrd loads a RAM disk as a multiboot module
#
# Usage: ./run_with_ram_disk.sh [audio_device]
#   audio_device: sb16, hda, or speaker (default: speaker)
#
# Examples:
#   ./run_with_ram_disk.sh      # PC speaker only (default)
#   ./run_with_ram_disk.sh sb16 # Sound Blaster 16
#   ./run_with_ram_disk.sh hda  # Intel HD Audio
#
# Networking (RTL8139 on QEMU's SLIRP user network) is enabled by default:
#   NET=off ./run_with_ram_disk.sh                              # disable it
#   NET_HOSTFWD="hostfwd=tcp::8080-:80" ./run_with_ram_disk.sh  # custom forwards

AUDIO_DEVICE="${1:-speaker}"
. "$(dirname "$0")/qemu_devices.sh"

qemu-system-i386 -kernel kernel -initrd ../krffs/file_system.krffs -serial stdio $AUDIO_OPTS $NET_OPTS
