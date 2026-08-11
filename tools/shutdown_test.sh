#!/usr/bin/env sh

#
# shutdown_test.sh: prove that userland can stop the machine, and that only
# root can.
#
# Boots one disk fixture repeatedly and asserts on two things a token cannot
# give: whether the QEMU process ended, and what offline fsck says about the
# image afterwards. A token proves only that the kernel reached the line before
# the write, so every phase that claims a stop waits on the process.
#
# Three traps this repo has already paid for, and which every phase here obeys:
# the console echoes what is typed, so a token that appears in a command would
# be matched from its own echo and each is written split; a feed written all at
# once is eaten by anything that flushes its input, so every feed is spaced; and
# a previous run's logs hold every token this run waits for, so they go before
# the first boot rather than only in the trap.
#
# The serial input is a FIFO held open past each wait, because EOF on it stops
# QEMU by itself and would make every outcome look like a success.
#
# Usage: shutdown_test.sh <kernel> <disk> <fsck.krffs> <password>
#

set -e

KERNEL="$1"
DISK="$2"
FSCK="$3"
PASSWORD="$4"

if [ -z "$KERNEL" ] || [ -z "$DISK" ] || [ -z "$FSCK" ] || [ -z "$PASSWORD" ]; then
    echo "usage: shutdown_test.sh <kernel> <disk> <fsck.krffs> <password>" >&2
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
trap 'rm -f "$WORK" "$LOG" "$FEED"' EXIT INT TERM

QPID=""
FPID=""

# Whether offline fsck calls the working image clean. Its status is read beside
# its output: a check that only asked whether the dirty line was absent would
# read a corrupt or unreadable image, which prints something else, as a clean
# one, since in a pipeline it is grep's status that survives.
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

# Start a boot on the working image, with $1 as the shell line typed at the
# console. The copy is a separate step, because the phase that reads a file back
# has to boot the image the previous phase WROTE, not a pristine one. The fixture's init drops straight to a root shell, so there is no
# login to get past. Set EXTRA_QEMU beforehand for a flag a phase needs; it is
# deliberately unquoted, and empty by default, because a quoted empty expansion
# would hand QEMU an empty argument and QEMU refuses one.
EXTRA_QEMU=""
fresh_image() {
    rm -f "$WORK"
    cp "$DISK" "$WORK"
}

start_boot() {
    rm -f "$LOG" "$FEED"
    mkfifo "$FEED"
    ( sleep 10; printf '%s\n' "$1"; sleep 180 ) > "$FEED" &
    FPID=$!
    # shellcheck disable=SC2086
    qemu-system-i386 -cpu max -display none $EXTRA_QEMU -kernel "$KERNEL" \
        -append "rach2.fsdisk" \
        -drive "file=$WORK,format=raw,if=ide,index=0,media=disk" \
        -serial stdio < "$FEED" > "$LOG" 2>&1 &
    QPID=$!
}

# Wait up to $1 seconds for QEMU to end by itself. Returns 1 if it did not.
wait_stop() {
    i=0
    while kill -0 "$QPID" 2>/dev/null && [ "$i" -lt "$1" ]; do
        sleep 1
        i=$((i + 1))
    done
    if kill -0 "$QPID" 2>/dev/null; then
        return 1
    fi
    return 0
}

# Wait up to $2 seconds for the token $1 to appear in the log.
wait_token() {
    i=0
    while [ "$i" -lt "$2" ]; do
        if grep -q "$1" "$LOG" 2>/dev/null; then
            return 0
        fi
        sleep 1
        i=$((i + 1))
    done
    return 1
}

cleanup_boot() {
    kill -9 "$QPID" 2>/dev/null || true
    wait "$QPID" 2>/dev/null || true
    kill "$FPID" 2>/dev/null || true
    rm -f "$FEED"
}

fail() {
    cleanup_boot
    echo "shutdown-test: FAIL: $1" >&2
    exit 1
}

# Every phase that stops the machine asserts the same three things, so they are
# asserted in one place: the process ended, the kernel said it cleared the flag,
# and the image agrees offline. $2 is the line that says WHICH stop happened,
# and it is not optional for a reason: under -no-reboot a reset and a power-off
# both end QEMU with a clean image, so without it a `reboot` that powered the
# machine off instead would pass. $3 is the path that must NOT appear.
require_clean_stop() {
    if ! wait_stop 90; then
        fail "$1: the machine did not stop"
    fi
    if ! wait "$QPID"; then
        kill "$FPID" 2>/dev/null || true
        fail "$1: QEMU stopped, but not with the status a power-off gives"
    fi
    kill "$FPID" 2>/dev/null || true
    rm -f "$FEED"
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

# And the marker is really there. Measured: the same write is LOST when the
# machine is killed instead of stopped, so this is a real durability assertion
# and not a tautology, and removing both flushes from the shutdown fails the
# phase above. What it does not prove is that the shutdown's flush is the only
# thing that could have put the page there: the cache also writes a frame back
# when it evicts one, so on a busier boot the page might arrive that way. The
# assertion is that a write made immediately before a stop survives it.
start_boot 'cat /root/marker; echo READ""BACK'
if ! wait_token "READBACK" 90; then
    fail "the second boot never reported"
fi
if ! grep -q "^marker" "$LOG"; then
    fail "the file written immediately before the power-off did not survive it"
fi
cleanup_boot
echo "shutdown-test: the write before the power-off reached the platter: OK"

# The forced halt, which parks the CPU rather than stopping QEMU, so the runner
# does the killing. It still has to have flushed and cleared the flag.
fresh_image
start_boot 'halt -n -f'
if ! wait_token "halt: system halted" 90; then
    fail "halt -n -f never reported a halt"
fi
if ! grep -q "shutdown: clean unmount: dirty-mount flag cleared" "$LOG"; then
    fail "halt -n -f did not report a clean unmount"
fi
cleanup_boot
if ! fsck_says_clean; then
    fail "the image is not clean after halt -n -f"
fi
echo "shutdown-test: halt -n -f flushes and parks the CPU: OK"

# The signal contract, before the pid is: `$PPID` from the console shell is
# init by construction, so this phase names no number and does not depend on
# init being pid 1. It sends the signal exactly ONCE, because busybox sends its
# own twice and a shipped path that came to depend on the retry would be a path
# that breaks the day this system serves a /proc/meminfo.
fresh_image
# $PPID is the GUEST shell's, so it must reach the guest unexpanded.
# shellcheck disable=SC2016
start_boot 'kill -USR2 $PPID'
require_clean_stop "a SIGUSR2 to init powers the machine off" \
    "poweroff: powering off" "reboot: restarting"

# And now the bare forms, which need init to actually BE pid 1, since that is
# who busybox signals. One phase per signal arm, because the arms do not map
# one-to-one onto these phases: removing SIGUSR2 fails both the phase above and
# the bare poweroff here, removing SIGUSR1 fails bare halt alone, and removing
# SIGTERM fails bare reboot alone.
fresh_image
start_boot 'poweroff'
require_clean_stop "a bare poweroff stops the machine" \
    "poweroff: powering off" "reboot: restarting"

# Bare halt parks the CPU, so the runner does the killing; what it must still
# have done is flush and clear the flag.
fresh_image
start_boot 'halt'
if ! wait_token "halt: system halted" 120; then
    fail "a bare halt never reported a halt"
fi
if ! grep -q "shutdown: clean unmount: dirty-mount flag cleared" "$LOG"; then
    fail "a bare halt did not report a clean unmount"
fi
cleanup_boot
if ! fsck_says_clean; then
    fail "the image is not clean after a bare halt"
fi
echo "shutdown-test: a bare halt flushes and parks the CPU: OK"

# Bare reboot exercises SIGTERM and RB_AUTOBOOT. -no-reboot makes the reset end
# QEMU rather than start the machine again.
fresh_image
EXTRA_QEMU="-no-reboot"
start_boot 'reboot'
require_clean_stop "a bare reboot restarts the machine" \
    "reboot: restarting" "poweroff: powering off"
EXTRA_QEMU=""

# The refusal, which is the only committed test of the kernel's permission check
# on a signal to init: an unprivileged caller must be turned away, and the
# machine must still be there afterwards to say so.
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
if fsck_says_clean; then
    fail "the image is clean, so the refused halt stopped the machine anyway"
fi
echo "shutdown-test: an unprivileged bare halt is refused: OK"

echo "shutdown-test: PASS"
