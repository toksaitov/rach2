#!/usr/bin/env sh

#
# crash_test.sh: prove KRFFS's dirty-mount flag detects an unclean shutdown.
#
# Boots a disk-backed KRFFS three times against scratch copies. The first boot is
# killed after the mount (which sets the dirty bit write-through to the disk),
# simulating a crash; offline fsck must then report "not cleanly unmounted". The
# second boot halts cleanly (clearing the bit); offline fsck must find it clean.
# The third runs dirtyprobe, which attacks the root node with every operation
# that could clear a mode bit, and the flag must survive: KRFFS_MODE_DIRTY and
# KRFFS_MODE_SUID are the same bit, so a set-id clear that reached the root would
# report an unclean image as cleanly unmounted. That claim needs a disk, since
# the RAM mount never sets the flag.
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
DIRTY="$DISK.dirty"
cp "$DISK" "$CRASH"
cp "$DISK" "$CLEAN"
cp "$DISK" "$DIRTY"
# Removed here and not only in the trap. An uncatchable kill leaves a previous
# run's logs, and each holds the token its phase waits for; the redirect that
# would truncate one is performed by the backgrounded child, which the parent's
# grep is not ordered after. The clean phase is the one that would then lie: a
# stale halt token ends its wait before the boot, and its fresh copy of the image
# is clean, so the fsck below agrees. The window is narrow (the parent's grep
# costs a fork and an exec while the child's truncate costs an open, and it was
# not won in 600 trials of both background forms used here), which is a reason to
# spend one line closing it rather than a reason to argue about it.
rm -f "$CRASH.log" "$CLEAN.log" "$DIRTY.log"
trap 'rm -f "$CRASH" "$CLEAN" "$DIRTY" "$CRASH.log" "$CLEAN.log" "$DIRTY.log"' EXIT INT TERM

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

# The root node under attack: boot dirty, run dirtyprobe, kill without a halt.
# The probe syncs after every attempt, so a spurious clear reaches the platter
# rather than sitting in a write-back frame where this check could not see it.
printf '\nexec /dirtyprobe\n' | qemu_disk "$DIRTY" >"$DIRTY.log" 2>&1 &
QPID=$!
if ! wait_serial "$DIRTY.log" "DIRTYPROBE OK"; then
    kill "$QPID" 2>/dev/null || true
    wait "$QPID" 2>/dev/null || true
    echo "crash-test: FAIL: dirtyprobe never reported OK" >&2
    exit 1
fi
kill "$QPID" 2>/dev/null || true
wait "$QPID" 2>/dev/null || true
if "$FSCK" "$DIRTY" --offset "$OFFSET" --size "$SIZE" 2>&1 \
        | grep -q "not cleanly unmounted"; then
    echo "crash-test: root node survived the set-id clears: OK"
else
    echo "crash-test: FAIL: the dirty flag was cleared off the root node" >&2
    exit 1
fi
