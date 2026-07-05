#!/usr/bin/env sh

#
# crash_test.sh: prove KRFFS's dirty-mount flag detects an unclean shutdown.
#
# Boots a disk-backed KRFFS twice against scratch copies. The first boot is
# killed after the mount (which sets the dirty bit write-through to the disk),
# simulating a crash; offline fsck must then report "not cleanly unmounted". The
# second boot halts cleanly (clearing the bit); offline fsck must find it clean.
#
# Usage: crash_test.sh <kernel> <disk> <fsck.krffs>
#

set -e

KERNEL="$1"
DISK="$2"
FSCK="$3"
# The KRFFS partition geometry, which must match the disk the Makefile crash-test
# target builds with `mkdisk.sh ... 64 48`: krffs/mkdisk.sh places the partition at
# LBA 2048 (1 MiB) and sizes it at 48 MiB. fsck.krffs reads the partition at this
# offset/size; a drift here silently misreads the dirty byte.
OFFSET=$((2048 * 512))
SIZE=$((48 * 1024 * 1024))

if [ -z "$KERNEL" ] || [ -z "$DISK" ] || [ -z "$FSCK" ]; then
    echo "usage: crash_test.sh <kernel> <disk> <fsck.krffs>" >&2
    exit 2
fi

CRASH="$DISK.crash"
CLEAN="$DISK.clean"
cp "$DISK" "$CRASH"
cp "$DISK" "$CLEAN"
trap 'rm -f "$CRASH" "$CLEAN" "$CRASH.log" "$CLEAN.log"' EXIT INT TERM

qemu_disk() {
    qemu-system-i386 -cpu max -display none -kernel "$KERNEL" \
        -append "rach2.fsdisk rach2.rescue" \
        -drive "file=$1,format=raw,if=ide,index=0,media=disk" -serial stdio
}

# Wait until the serial log $1 contains the token $2, then return 0; give up
# after 60 seconds and return 1. Gating each phase on the token the kernel
# prints, rather than on a fixed sleep, keeps a loaded or slow host from
# killing QEMU before the phase's disk state has actually been written.
wait_serial() {
    i=0
    while [ "$i" -lt 60 ]; do
        if grep -q "$2" "$1" 2>/dev/null; then
            return 0
        fi
        sleep 1
        i=$((i + 1))
    done
    return 1
}

# Crash: boot, wait for the mount (which sets the dirty bit write-through
# before the kernel reports the file system), then kill before any clean halt.
qemu_disk "$CRASH" >"$CRASH.log" 2>&1 &
QPID=$!
if ! wait_serial "$CRASH.log" "(disk)"; then
    kill "$QPID" 2>/dev/null || true
    wait "$QPID" 2>/dev/null || true
    echo "crash-test: FAIL: disk mount never reported on serial" >&2
    exit 1
fi
kill "$QPID" 2>/dev/null || true
wait "$QPID" 2>/dev/null || true
if "$FSCK" "$CRASH" --offset "$OFFSET" --size "$SIZE" 2>&1 \
        | grep -q "not cleanly unmounted"; then
    echo "crash-test: crashed image reports not-cleanly-unmounted: OK"
else
    echo "crash-test: FAIL: crashed image did not report dirty" >&2
    exit 1
fi

# Clean: boot and halt from the debug shell, which clears the bit. The kernel
# prints "halt: system halted" on serial only after the clean-unmount flush.
printf '\nhalt\n' | qemu_disk "$CLEAN" >"$CLEAN.log" 2>&1 &
QPID=$!
if ! wait_serial "$CLEAN.log" "halt: system halted"; then
    kill "$QPID" 2>/dev/null || true
    wait "$QPID" 2>/dev/null || true
    echo "crash-test: FAIL: clean halt never reported on serial" >&2
    exit 1
fi
kill "$QPID" 2>/dev/null || true
wait "$QPID" 2>/dev/null || true
if "$FSCK" "$CLEAN" --offset "$OFFSET" --size "$SIZE" 2>&1 \
        | grep -q "not cleanly unmounted"; then
    echo "crash-test: FAIL: cleanly-halted image still dirty" >&2
    exit 1
else
    echo "crash-test: cleanly-halted image is clean: OK"
fi
