#!/usr/bin/env sh

#
# shutdown.sh: test that userland can stop the machine, and that only root
# can.
#
# Boots one disk fixture repeatedly and checks two things a token cannot
# show: whether the QEMU process ended, and what offline fsck says about the
# image afterwards. A token shows only that the kernel reached the line before
# the write, so every phase that expects a stop waits on the process.
#
# The console echoes what is typed, so a token that appears in a command is
# written split, or it would match its own echo; a feed written all at once is
# eaten by anything that flushes its input, so every feed is spaced; and the
# logs of an earlier run hold every token this run waits for, so they are
# removed before the first boot as well as in the trap.
#
# The serial input is a FIFO held open past each wait, because EOF on it stops
# QEMU by itself and would make every outcome look like a success.
#
# Usage: shutdown.sh <kernel> <disk> <fsck.krffs> <password>
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
PASSWORD="$4"

if [ -z "$KERNEL" ] || [ -z "$DISK" ] || [ -z "$FSCK" ] || [ -z "$PASSWORD" ]; then
    echo "usage: shutdown.sh <kernel> <disk> <fsck.krffs> <password>" >&2
    exit 2
fi

# The KRFFS partition geometry, which must match the disk the Makefile builds
# with `mkdisk.sh ... 64 48`: the partition sits at LBA 2048 and is 48 MiB.
OFFSET=$((2048 * 512))
SIZE=$((48 * 1024 * 1024))

WORK="$DISK.work"
LOG="$DISK.work.log"
FEED="$DISK.work.feed"
rm -f "$WORK" "$LOG" "$FEED"

QPID=""
FPID=""

# fsck_says_clean: return 0 when offline fsck calls the working image clean.
# Its status is read beside its output: a check that only asked whether the
# dirty line was absent would read a corrupt or unreadable image, which prints
# something else, as a clean one, since in a pipeline it is grep's status that
# survives.
fsck_says_clean() {
    out=$("$FSCK" "$WORK" --offset "$OFFSET" --size "$SIZE" 2>&1)
    rc=$?
    if [ "$rc" -ne 0 ]; then
        echo "shutdown-test: fsck.krffs exited $rc:" >&2
        echo "$out" >&2
        return 1
    fi
    case "$out" in
        *"not cleanly unmounted"*) return 1 ;;
    esac
    return 0
}

# fsck_says_dirty: return 0 when offline fsck runs to the end and calls the
# working image dirty. Its status is read for the same reason: fsck prints the
# dirty line before it finishes walking the chain.
fsck_says_dirty() {
    out=$("$FSCK" "$WORK" --offset "$OFFSET" --size "$SIZE" 2>&1)
    rc=$?
    if [ "$rc" -ne 0 ]; then
        echo "shutdown-test: fsck.krffs exited $rc:" >&2
        echo "$out" >&2
        return 1
    fi
    case "$out" in
        *"not cleanly unmounted"*) return 0 ;;
    esac
    return 1
}

# require_clean_halt NAME: check the halt of phase NAME and fail if it was not
# clean. A halt does not end QEMU, so the suite kills the machine itself. The
# routes a halt must not take are checked too, since `power_stop` prints the
# halt token after a power-off or a reset that did not take, and a halt that
# fell back through one of those would otherwise look like one asked for.
require_clean_halt() {
    if ! wait_token "halt: system halted" 120; then
        fail "$1: never reported a halt"
    fi
    if ! grep -q "shutdown: clean unmount: dirty-mount flag cleared" "$LOG"; then
        fail "$1: did not report a clean unmount"
    fi
    if grep -q "halting instead" "$LOG"; then
        fail "$1: fell back to a halt from another route"
    fi
    if grep -qE "poweroff: powering off|reboot: restarting" "$LOG"; then
        fail "$1: took a platform route a halt must not take"
    fi
    if ! kill -0 "$QPID" 2>/dev/null; then
        fail "$1: QEMU ended by itself, so this was not a halt"
    fi
    cleanup_boot
    if ! fsck_says_clean; then
        fail "$1: the image is not clean"
    fi
    echo "shutdown-test: $1: OK"
}

# A flag a phase needs from QEMU, set before start_boot. It is expanded
# unquoted, and empty by default, because a quoted empty expansion would give
# QEMU an empty argument, which QEMU refuses.
EXTRA_QEMU=""
# fresh_image: replace the working image with a copy of the fixture. The copy
# is a step apart from the boot, so the phase that reads a file back boots the
# image the phase before it wrote.
fresh_image() {
    rm -f "$WORK"
    cp "$DISK" "$WORK"
}

# The console is driven through tests/system/drive.sh.
REPO=$(cd "$(dirname "$0")/../../.." && pwd)
# shellcheck source=tests/system/drive.sh
. "$REPO/tests/system/drive.sh"
# start_boot LINE: boot the working image and type LINE once the shell's
# prompt is printed. The fixture's init starts a root shell with no login. The
# feeder holds the console open while the machine runs, since EOF on the
# serial input stops QEMU by itself.
start_boot() {
    rm -f "$LOG" "$FEED"
    mkfifo "$FEED"
    # shellcheck disable=SC2086
    "$QEMU" -cpu max -display none $EXTRA_QEMU -kernel "$KERNEL" \
        -append "rach2.fsdisk" \
        -drive "file=$WORK,format=raw,if=ide,index=0,media=disk" \
        -serial stdio < "$FEED" > "$LOG" 2>&1 &
    QPID=$!
    DRIVE_LOG=$LOG
    DRIVE_FIFO=$FEED
    drive_feed "$QPID"
    FPID=$DRIVE_FPID
    drive_prompt "$(scaled 90)" || fail "the machine printed no prompt to type '$1' at"
    drive_type "$1"
}

# wait_stop SECONDS: wait up to SECONDS for QEMU to end by itself; return 1 if
# it did not. A process that has ended counts, reaped or not: the caller reaps
# the machine and reads its status, which is what the phase tests.
wait_stop() {
    "$RUNUTIL" gone "$QPID" "$1"
}

# wait_token TOKEN SECONDS: wait up to SECONDS for TOKEN to appear in the log.
wait_token() {
    "$RUNUTIL" until "$LOG" "$1" "$2"
}

# cleanup_boot: stop the machine and its feeder and remove the FIFO. It is safe
# to call twice, since the ordinary paths call it and the trap calls it again;
# without the trap a cancelled build would leave QEMU running on a disk copy
# whose name has been removed.
#
# Each pid is cleared before it is signalled, so a trap that fires between the
# kill and the wait finds nothing left to kill: a pid this script no longer
# owns may since have been given to another process.
cleanup_boot() {
    qpid="$QPID"
    fpid="$FPID"
    QPID=""
    FPID=""
    if [ -n "$qpid" ]; then
        kill -9 "$qpid" 2>/dev/null || true
        wait "$qpid" 2>/dev/null || true
    fi
    if [ -n "$fpid" ]; then
        kill -9 "$fpid" 2>/dev/null || true
        wait "$fpid" 2>/dev/null || true
    fi
    rm -f "$FEED"
}

# cleanup: stop the boot and remove the working image, its log and the FIFO.
cleanup() {
    cleanup_boot
    rm -f "$WORK" "$LOG" "$FEED"
}

trap 'cleanup' EXIT
trap 'cleanup; exit 130' INT
trap 'cleanup; exit 143' TERM
trap 'cleanup; exit 129' HUP

# fail REASON: stop the boot, print REASON and exit 1.
fail() {
    cleanup_boot
    echo "shutdown-test: FAIL: $1" >&2
    exit 1
}

# require_clean_stop NAME SAID NOTSAID: check the stop of phase NAME and fail
# if it was not clean: the process ended, the kernel said it cleared the flag,
# and the image is clean offline. SAID is the line that says which stop
# happened, since under -no-reboot a reset and a power-off both end QEMU with a
# clean image, and a `reboot` that powered the machine off would otherwise
# pass. NOTSAID is the line of the other stop, which must not appear.
require_clean_stop() {
    if ! wait_stop 90; then
        fail "$1: the machine did not stop"
    fi
    # Read the status before the pid is cleared, since the host may reuse it.
    if ! wait "$QPID"; then
        QPID=""
        fail "$1: QEMU stopped, but not with the status a power-off gives"
    fi
    QPID=""
    cleanup_boot
    if ! grep -q "shutdown: clean unmount: dirty-mount flag cleared" "$LOG"; then
        fail "$1: the kernel never reported a clean unmount"
    fi
    if grep -q "halting instead" "$LOG"; then
        fail "$1: the stop fell back to a halt"
    fi
    if ! grep -q "$2" "$LOG"; then
        fail "$1: the kernel never said '$2', so this was a different stop"
    fi
    if grep -q "$3" "$LOG"; then
        fail "$1: the kernel said '$3', which is not the stop this asked for"
    fi
    if ! fsck_says_clean; then
        fail "$1: the image is not clean"
    fi
    echo "shutdown-test: $1: OK"
}

# The forced power-off, with -n so that busybox does not sync for the kernel.
# Without it the marker below would already be on the platter through sync(2)
# and the phase would pass with the kernel's own flush deleted.
fresh_image
start_boot 'echo marker > /root/marker; poweroff -n -f'
require_clean_stop "poweroff -n -f stops the machine and flushes it" \
    "poweroff: powering off" "reboot: restarting"

# The marker must be on the disk. The same write is lost when the machine is
# killed instead of stopped, and removing both flushes from the shutdown fails
# the phase above. The cache also writes a frame back when it evicts one, so
# on a busier boot the page might arrive that way: the phase shows that a
# write made just before a stop survives it, and not that the shutdown's flush
# alone put it there.
start_boot 'cat /root/marker; echo READ""BACK'
if ! wait_token "READBACK" 90; then
    fail "the second boot never reported"
fi
if ! grep -q "^marker" "$LOG"; then
    fail "the file written immediately before the power-off did not survive it"
fi
cleanup_boot
echo "shutdown-test: the write before the power-off reached the platter: OK"

# The forced halt, which parks the CPU instead of stopping QEMU, so the suite
# kills the machine. It must still have flushed and cleared the flag.
fresh_image
start_boot 'halt -n -f'
require_clean_halt "halt -n -f flushes and parks the CPU"

# The signal alone, apart from init's pid: `$PPID` of the console shell is
# init, so this phase names no number and does not depend on init being pid 1.
# The shell's kill sends the signal once, where busybox's poweroff sends its
# own twice, so init must act on the first.
fresh_image
# $PPID is the guest shell's, so it must reach the guest unexpanded.
# shellcheck disable=SC2016
start_boot 'kill -USR2 $PPID'
require_clean_stop "a SIGUSR2 to init powers the machine off" \
    "poweroff: powering off" "reboot: restarting"

# The bare forms, which need init to be pid 1, since busybox signals pid 1.
# Each signal init handles has a phase: removing SIGUSR2 fails both the phase
# above and the bare poweroff here, removing SIGUSR1 fails bare halt alone,
# and removing SIGTERM fails bare reboot alone.
fresh_image
start_boot 'poweroff'
require_clean_stop "a bare poweroff stops the machine" \
    "poweroff: powering off" "reboot: restarting"

# Bare halt parks the CPU, so the suite kills the machine; it must still have
# flushed and cleared the flag.
fresh_image
start_boot 'halt'
require_clean_halt "a bare halt flushes and parks the CPU"

# Bare reboot exercises SIGTERM and RB_AUTOBOOT. -no-reboot makes the reset end
# QEMU, not start the machine again.
fresh_image
EXTRA_QEMU="-no-reboot"
start_boot 'reboot'
require_clean_stop "a bare reboot restarts the machine" \
    "reboot: restarting" "poweroff: powering off"
EXTRA_QEMU=""

# The refusal tests the kernel's permission check on a signal to init: an
# unprivileged caller is turned away, and the machine still runs afterwards.
fresh_image
start_boot 'su test -c halt; echo STILL""HERE'
if ! wait_token "STILLHERE" 120; then
    fail "the machine did not survive an unprivileged halt"
fi
if ! grep -q "Operation not permitted" "$LOG"; then
    fail "an unprivileged bare halt was not refused"
fi
if grep -q "shutdown:" "$LOG"; then
    fail "an unprivileged bare halt reached the kernel's shutdown"
fi
cleanup_boot
if ! fsck_says_dirty; then
    fail "the image is not dirty, so the refused halt stopped the machine anyway"
fi
echo "shutdown-test: an unprivileged bare halt is refused: OK"

echo "shutdown-test: PASS"
