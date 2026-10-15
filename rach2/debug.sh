#!/bin/sh

# Debug the kernel under QEMU with GDB: QEMU boots the kernel, stopped at its
# first instruction with a GDB server on port 1234 (-s -S), and GDB attaches
# to it through rach2/.gdbinit.
# Audio and networking devices are attached so their drivers can be debugged.
# NIC=e1000 selects the other card; see qemu_devices.sh.
# GDB loads the symbols of the kernel it debugs, the architecture's
# kernel.debug, and reads rach2/.gdbinit through -x with the automatic load
# of a local .gdbinit turned off, so the file is read once whatever GDB's
# auto-load safe path allows. Run the script from rach2/.
#
# Usage: ./debug.sh [image] [audio_device]
#   image:        test | os-simple | os-real, which boots
#                 build/images/<image>.krffs as the RAM disk, as run.sh does.
#                 Without it the kernel boots with rach2.rescue and no root,
#                 and so reaches the kernel debug shell instead of halting.
#   audio_device: sb16, hda, or speaker (default: speaker)
#
# RESCUE=1 adds rach2.rescue to a boot with an image, and CPU chooses the
# processor model, max by default, as in run.sh.
#
# ARCH=x86_64 debugs the x86-64 kernel on qemu-system-x86_64.

IMAGE="$1"
AUDIO_DEVICE="${2:-speaker}"
. "$(dirname "$0")/qemu_devices.sh"

CPU="${CPU:-max}"

if [ -n "$IMAGE" ]; then
    INITRD="$BUILD_DIR/images/$IMAGE.krffs"
    if [ ! -f "$INITRD" ]; then
        echo "debug: image '$INITRD' not found; build it with 'make $MAKE_ARCH$IMAGE-image'" >&2
        exit 1
    fi
    set -- -initrd "$INITRD"
    if [ -n "$RESCUE" ]; then
        set -- "$@" -append rach2.rescue
    fi
else
    set -- -append rach2.rescue
fi

"$QEMU" -s -S -cpu "$CPU" -kernel "$BUILD_DIR/rach2/kernel" "$@" $AUDIO_OPTS $NET_OPTS $DISPLAY_OPTS &
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
"$GDB" -iex 'set auto-load local-gdbinit off' -ex "file $BUILD_DIR/rach2/kernel.debug" -x .gdbinit
