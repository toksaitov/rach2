#!/bin/sh

# Start QEMU with GDB server (-s) paused at first instruction (-S).
# Audio and networking devices are attached so their drivers can be debugged.
# NIC=e1000 selects the other card; see qemu_devices.sh.
# GDB reads rach2/.gdbinit through -x with the automatic load of a local
# .gdbinit turned off, so the file is read once whatever GDB's auto-load safe
# path allows. Run the script from rach2/.
#
# Usage: ./debug.sh [audio_device]
#   audio_device: sb16, hda, or speaker (default: speaker)
#
# ARCH=x86_64 debugs the x86-64 kernel on qemu-system-x86_64.

AUDIO_DEVICE="${1:-speaker}"
. "$(dirname "$0")/qemu_devices.sh"

"$QEMU" -s -S -kernel "$BUILD_DIR/rach2/kernel" $AUDIO_OPTS $NET_OPTS &
QEMU_PID=$!

# Ensure QEMU is killed when this script exits
trap 'kill $QEMU_PID 2>/dev/null' EXIT INT TERM

sleep 1

# Find and run appropriate GDB
if command -v x86_64-elf-gdb >/dev/null 2>&1; then
    GDB=x86_64-elf-gdb
elif command -v gdb-multiarch >/dev/null 2>&1; then
    GDB=gdb-multiarch
elif command -v gdb >/dev/null 2>&1; then
    GDB=gdb
else
    echo "debug: install a GDB that can debug x86 binaries (try x86_64-elf-gdb)" >&2
    exit 1
fi
"$GDB" -iex 'set auto-load local-gdbinit off' -x .gdbinit
