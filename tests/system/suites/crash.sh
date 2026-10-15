#!/usr/bin/env sh

#
# crash.sh: KRFFS's dirty-mount flag detects an unclean shutdown.
#
# Four boots of a disk-backed KRFFS, each on a scratch copy of the disk. The
# first is killed after the mount, which sets the dirty bit write-through to
# the disk, as a crash would; an offline fsck must then report "not cleanly
# unmounted". The second halts cleanly, which clears the bit, and the third
# powers off, which must also stop QEMU; fsck must find both images clean.
# The fourth runs dirtyprobe, which attacks the root node with every
# operation that could clear a mode bit, and the flag must survive:
# KRFFS_MODE_DIRTY and KRFFS_MODE_SUID are the same bit, so a set-id clear
# that reached the root would report an unclean image as cleanly unmounted.
# That needs a disk, since the RAM mount never sets the flag.
#
# Usage: crash.sh <kernel> <disk> <fsck.krffs>
#

# The emulator of the architecture tests/system/suites.mk names.
QEMU=${RACH2_QEMU:-qemu-system-i386}

# shellcheck source=tests/system/deadline.sh
# The runner helper, whose `until` and `gone` are the suite's waits.
RUNUTIL="$(cd "$(dirname "$0")/../../.." && pwd)/build/tests/system/runutil"
. "$(cd "$(dirname "$0")/../../.." && pwd)/tests/system/deadline.sh"
set -e

KERNEL="$1"
DISK="$2"
FSCK="$3"
# The KRFFS partition geometry, which must match the disk that the run-crash
# target of tests/system/suites.mk builds with `mkdisk.sh ... 64 48`:
# krffs/mkdisk.sh places the partition at LBA 2048 (1 MiB) and sizes it at
# 48 MiB. fsck.krffs reads the partition at this offset and size, and a
# difference here would misread the dirty byte.
OFFSET=$((2048 * 512))
SIZE=$((48 * 1024 * 1024))

if [ -z "$KERNEL" ] || [ -z "$DISK" ] || [ -z "$FSCK" ]; then
    echo "usage: crash.sh <kernel> <disk> <fsck.krffs>" >&2
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
# would truncate one is performed by the backgrounded child, which the
# parent's grep is not ordered after. A stale halt token would end the clean
# phase's wait before its boot, and the phase's fresh copy of the image is
# clean, so the fsck below would agree.
rm -f "$CRASH.log" "$CLEAN.log" "$DIRTY.log" "$OFF.log"

QPID=""
FPID=""

# Stop the machine and the feeder a phase left running. Every phase stops its
# own, so this is for the ways out that no phase writes: a cancelled run that
# only removed the images would leave QEMU running on a file nothing can name.
# The pid is cleared as it is killed, since a pid this script no longer owns
# may since have been given to another process.
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

# Stop the machine and remove the images, the logs and the feed.
cleanup() {
    stop
    rm -f "$CRASH" "$CLEAN" "$DIRTY" "$OFF" "$CRASH.log" "$CLEAN.log" \
        "$DIRTY.log" "$OFF.log" "$DISK.feed"
}

trap 'cleanup' EXIT
trap 'cleanup; exit 130' INT
trap 'cleanup; exit 143' TERM
trap 'cleanup; exit 129' HUP

# Return 0 when an offline fsck calls $1 dirty. fsck's status is read as well
# as its output, since it prints the dirty line before it finishes walking the
# chain, and a pipeline would keep only grep's status: an image that is dirty
# and corrupt would pass a check of the line alone.
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

# Return 0 when an offline fsck calls $1 clean. fsck's status is read as well
# as its output, since a corrupt or unreadable image prints something other
# than the dirty line and would pass a check that the line is absent; in a
# pipeline only grep's status would survive.
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

# qemu_disk DISK: run the machine on DISK with the rescue shell. The function
# execs QEMU, so that a backgrounded call is the machine and not its parent:
# without the exec the shell forks a subshell, runs QEMU as its child and
# reports the subshell's pid in `$!`, and `stop` would kill the subshell and
# leave QEMU running under init.
qemu_disk() {
    exec "$QEMU" -cpu max -display none -kernel "$KERNEL" \
        -append "rach2.fsdisk rach2.rescue" \
        -drive "file=$1,format=raw,if=ide,index=0,media=disk" -serial stdio
}

# Wait until the serial log $1 contains the token $2, then return 0; give up
# after 60 seconds and return 1. A wait for the token the kernel prints, and
# not for a fixed time, keeps a loaded or slow host from stopping QEMU before
# the phase's disk state is written.
wait_serial() {
    "$RUNUTIL" until "$1" "$2" 60
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

# Power off, the one phase that requires the machine to stop. `halt` parks the
# processor and leaves QEMU running, so no halt phase can tell a stop from a
# hang; `poweroff` asks the PIIX4 to remove power, and QEMU exits. The wait is
# therefore on the process and not on a token, since a token shows only that
# the kernel reached the line before the write. Standard input is a FIFO held
# open by a writer that outlives the wait, because EOF on the serial input
# stops QEMU by itself and would make every outcome look like a success.
FEED="$DISK.feed"
rm -f "$FEED"
mkfifo "$FEED"
"$QEMU" -cpu max -display none -kernel "$KERNEL" \
    -append "rach2.fsdisk rach2.rescue" \
    -drive "file=$OFF,format=raw,if=ide,index=0,media=disk" \
    -serial stdio < "$FEED" > "$OFF.log" 2>&1 &
QPID=$!
# `poweroff` is typed once the debug shell's prompt has been printed, through
# tests/system/drive.sh, whose feeder holds the fifo open while the machine
# runs.
REPO=$(cd "$(dirname "$0")/../../.." && pwd)
# shellcheck source=tests/system/drive.sh
. "$REPO/tests/system/drive.sh"
DRIVE_LOG="$OFF.log"
DRIVE_FIFO=$FEED
drive_feed "$QPID"
FPID=$DRIVE_FPID
if drive_expect "Type 'help'" "$(scaled 60)"; then
    drive_type poweroff
fi
# The wait's own verdict decides: it reports the machine ended as soon as its
# process is a zombie, before this shell has reaped it, and a zombie can still
# be signalled, so a signal test here would call a stopped machine running.
if ! "$RUNUTIL" gone "$QPID" 60; then
    kill -9 "$QPID" 2>/dev/null || true
    stop
    rm -f "$FEED"
    echo "crash-test: FAIL: poweroff did not stop the machine" >&2
    exit 1
fi
# The status is read before the pid is cleared, since this phase is about the
# machine stopping by itself.
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
# The probe syncs after every attempt, so a spurious clear reaches the disk
# instead of staying in a write-back frame where this check could not see it.
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
