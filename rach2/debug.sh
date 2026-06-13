#!/bin/sh

# Start QEMU with GDB server (-s) paused at first instruction (-S).
# Audio and networking devices are attached so their drivers can be debugged.
#
# Usage: ./debug.sh [audio_device]
#   audio_device: sb16, hda, or speaker (default: speaker)

AUDIO_DEVICE="${1:-speaker}"
. "$(dirname "$0")/qemu_devices.sh"

qemu-system-i386 -s -S -kernel kernel $AUDIO_OPTS $NET_OPTS &
QEMU_PID=$!

# Ensure QEMU is killed when this script exits
trap "kill $QEMU_PID 2>/dev/null" EXIT INT TERM

sleep 1

# Find and run appropriate GDB
if command -v x86_64-elf-gdb >/dev/null 2>&1; then
    x86_64-elf-gdb
elif command -v gdb-multiarch >/dev/null 2>&1; then
    gdb-multiarch
elif command -v gdb >/dev/null 2>&1; then
    gdb
else
    echo "Please install a GDB that can debug x86 binaries (e.g., x86_64-elf-gdb)."
    exit 1
fi
