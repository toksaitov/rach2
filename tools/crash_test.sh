#!/usr/bin/env sh

#
# crash_test.sh: prove KRFFS's dirty-mount flag detects an unclean shutdown.
#
# Boots a disk-backed KRFFS three times against scratch copies. The first boot
# is killed after the mount (which sets the dirty bit write-through to the
# disk), simulating a crash; offline fsck must then report "not cleanly
# unmounted". The second boot halts cleanly (clearing the bit); offline fsck
# must find it clean. The third runs dirtyprobe, which attacks the root node
# with every operation that could clear a mode bit, and the flag must survive:
# KRFFS_MODE_DIRTY and KRFFS_MODE_SUID are the same bit, so a set-id clear that
# reached the root would report an unclean image as cleanly unmounted. That
# claim needs a disk, since the RAM mount never sets the flag.
#
# Usage: crash_test.sh <kernel> <disk> <fsck.krffs>
#

set -e

KERNEL="$1"
DISK="$2"
FSCK="$3"
# The KRFFS partition geometry, which must match the disk the Makefile
# crash-test target builds with `mkdisk.sh ... 64 48`: krffs/mkdisk.sh places
# the partition at LBA 2048 (1 MiB) and sizes it at 48 MiB. fsck.krffs reads the
# partition at this offset/size; a drift here silently misreads the dirty byte.
OFFSET=$((2048 * 512))
SIZE=$((48 * 1024 * 1024))

if [ -z "$KERNEL" ] || [ -z "$DISK" ] || [ -z "$FSCK" ]; then
    echo "usage: crash_test.sh <kernel> <disk> <fsck.krffs>" >&2
    exit 2
fi

CRASH="$DISK.crash"
CLEAN="$DISK.clean"
DIRTY="$DISK.dirty"
OFF="$DISK.off"
cp "$DISK" "$CRASH"
cp "$DISK" "$CLEAN"
cp "$DISK" "$DIRTY"
cp "$DISK" "$OFF"
# Removed here and not only in the trap. An uncatchable kill leaves a previous
# run's logs, and each has the token its phase waits for; the redirect that
# would truncate one is performed by the backgrounded child, which the parent's
# grep is not ordered after. The clean phase is the one that would then lie: a
# stale halt token ends its wait before the boot, and its fresh copy of the
# image is clean, so the fsck below agrees. The window is narrow (the parent's
# grep costs a fork and an exec while the child's truncate costs an open, and it
# was not won in 600 trials of both background forms used here), which is a
# reason to spend one line closing it rather than a reason to argue about it.
rm -f "$CRASH.log" "$CLEAN.log" "$DIRTY.log" "$OFF.log"

QPID=""
FPID=""

# Whatever machine a phase left running, stopped. Every phase ends its own, so
# this is for the ways out that no phase writes: a cancelled run that had only
# removed the images would leave QEMU alive on a file nothing can now name. The
# pid is forgotten as it is killed, since a pid this script no longer owns is a
# pid the host may since have given to somebody else.
stop() {
    qpid="$QPID"
    fpid="$FPID"
    QPID=""
    FPID=""
    if [ -n "$qpid" ]; then
        kill "$qpid" 2>/dev/null || true
        wait "$qpid" 2>/dev/null || true
    fi
    if [ -n "$fpid" ]; then
        kill "$fpid" 2>/dev/null || true
        wait "$fpid" 2>/dev/null || true
    fi
}

cleanup() {
    stop
    rm -f "$CRASH" "$CLEAN" "$DIRTY" "$OFF" "$CRASH.log" "$CLEAN.log" \
        "$DIRTY.log" "$OFF.log" "$DISK.feed"
}

trap 'cleanup' EXIT
trap 'cleanup; exit 130' INT
trap 'cleanup; exit 143' TERM
trap 'cleanup; exit 129' HUP

# Whether an offline fsck calls $1 dirty. Its status is read as well, because
# fsck prints the dirty line before it finishes walking the chain: a pipeline
# would read grep's status, so an image that is dirty AND corrupt would pass a
# phase that only asked whether the line appeared.
fsck_says_dirty() {
    out=$("$FSCK" "$1" --offset "$OFFSET" --size "$SIZE" 2>&1)
    rc=$?
    if [ "$rc" -ne 0 ]; then
        echo "crash-test: fsck.krffs exited $rc on $1:" >&2
        echo "$out" >&2
        return 1
    fi
    case "$out" in
        *"not cleanly unmounted"*) return 0 ;;
    esac
    return 1
}

# Whether an offline fsck calls $1 clean. Its exit status is read as well as its
# output, because a check that only asked whether the dirty line was absent
# would read a corrupt or unreadable image, which prints something else
# entirely, as a clean one: in a pipeline it is grep's status that survives, not
# fsck's.
fsck_says_clean() {
    out=$("$FSCK" "$1" --offset "$OFFSET" --size "$SIZE" 2>&1)
    rc=$?
    if [ "$rc" -ne 0 ]; then
        echo "crash-test: fsck.krffs exited $rc on $1:" >&2
        echo "$out" >&2
        return 1
    fi
    case "$out" in
        *"not cleanly unmounted"*) return 1 ;;
    esac
    return 0
}

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
    stop
    echo "crash-test: FAIL: disk mount never reported on serial" >&2
    exit 1
fi
stop
if ! fsck_says_dirty "$CRASH"; then
    echo "crash-test: FAIL: crashed image did not report dirty" >&2
    exit 1
fi
echo "crash-test: crashed image reports not-cleanly-unmounted: OK"

# Clean: boot and halt from the debug shell, which clears the bit. The kernel
# prints "halt: system halted" on serial only after the clean-unmount flush.
printf '\nhalt\n' | qemu_disk "$CLEAN" >"$CLEAN.log" 2>&1 &
QPID=$!
if ! wait_serial "$CLEAN.log" "halt: system halted"; then
    stop
    echo "crash-test: FAIL: clean halt never reported on serial" >&2
    exit 1
fi
stop
if ! fsck_says_clean "$CLEAN"; then
    echo "crash-test: FAIL: cleanly-halted image is not clean" >&2
    exit 1
fi
echo "crash-test: cleanly-halted image is clean: OK"

# Power off, which is the only phase that asserts the machine STOPPED. `halt`
# parks the CPU and leaves QEMU running by design, so no halt phase can tell a
# stop from a hang; `poweroff` asks the PIIX4 to remove power, and QEMU exits.
# The wait is therefore on the process and not on a token: a token proves only
# that the kernel reached the line before the write. stdin is a FIFO held open
# by a writer that outlives the wait, because EOF on the serial input stops QEMU
# on its own and would make every outcome here look like a success.
FEED="$DISK.feed"
rm -f "$FEED"
mkfifo "$FEED"
( sleep 6; printf 'poweroff\n'; sleep 120 ) > "$FEED" &
FPID=$!
qemu-system-i386 -cpu max -display none -kernel "$KERNEL" \
    -append "rach2.fsdisk rach2.rescue" \
    -drive "file=$OFF,format=raw,if=ide,index=0,media=disk" \
    -serial stdio < "$FEED" > "$OFF.log" 2>&1 &
QPID=$!
i=0
while kill -0 "$QPID" 2>/dev/null && [ "$i" -lt 60 ]; do
    sleep 1
    i=$((i + 1))
done
if kill -0 "$QPID" 2>/dev/null; then
    kill -9 "$QPID" 2>/dev/null || true
    stop
    rm -f "$FEED"
    echo "crash-test: FAIL: poweroff did not stop the machine" >&2
    exit 1
fi
# The status is read before the pid is forgotten, because it is the machine
# stopping ITSELF that this phase is about.
if ! wait "$QPID"; then
    QPID=""
    stop
    rm -f "$FEED"
    echo "crash-test: FAIL: QEMU stopped, but not with the status a power-off gives" >&2
    exit 1
fi
QPID=""
stop
rm -f "$FEED"
if ! grep -q "shutdown: clean unmount: dirty-mount flag cleared" "$OFF.log"; then
    echo "crash-test: FAIL: poweroff did not report a clean unmount" >&2
    exit 1
fi
if grep -q "halting instead" "$OFF.log"; then
    echo "crash-test: FAIL: the power-off fell back to a halt" >&2
    exit 1
fi
if ! fsck_says_clean "$OFF"; then
    echo "crash-test: FAIL: powered-off image is not clean" >&2
    exit 1
fi
echo "crash-test: powered-off image is clean and QEMU stopped itself: OK"

# The root node under attack: boot dirty, run dirtyprobe, kill without a halt.
# The probe syncs after every attempt, so a spurious clear reaches the platter
# rather than sitting in a write-back frame where this check could not see it.
printf '\nexec /dirtyprobe\n' | qemu_disk "$DIRTY" >"$DIRTY.log" 2>&1 &
QPID=$!
if ! wait_serial "$DIRTY.log" "DIRTYPROBE OK"; then
    stop
    echo "crash-test: FAIL: dirtyprobe never reported OK" >&2
    exit 1
fi
stop
if ! fsck_says_dirty "$DIRTY"; then
    echo "crash-test: FAIL: the dirty flag was cleared off the root node" >&2
    exit 1
fi
echo "crash-test: root node survived the set-id clears: OK"
