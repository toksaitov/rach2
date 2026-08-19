#!/usr/bin/env sh

#
# grub_test.sh: replacing the boot loader from the running machine.
#
# The post-MBR gap holds two core-image tails and LBA 1 selects between them.
# `bootctl grub` writes the tail that is not in use, reads it back, and only
# then rewrites LBA 1. Sector 0 is never written, which is the whole reason the
# layout is shaped this way: it carries the partition table, and a disk writes
# whole sectors.
#
# Three boots of one disk. The first proves which core is running and installs
# the other, the second proves the machine came up on it, installs the first
# back and checks that the window admits one opener at a time, and the third
# proves the swap goes both ways. Each core image carries a configuration that
# prints its own marker, so what the log holds identifies the image that ran.
# Reading the pointer back would prove only bytes at rest, and a tool that wrote
# the pointer and nothing else would pass that.
#
# Usage: grub_test.sh <grub-disk> <root password>
#

set -e

DISK="$1"
ROOTPASSWORD="$2"

if [ -z "$DISK" ] || [ -z "$ROOTPASSWORD" ]; then
    echo "usage: grub_test.sh <grub-disk> <root password>" >&2
    exit 2
fi

DEADLINE=2400
START="$(date +%s)"

WORK="$DISK.work"
FEED="$DISK.feed"
STEPS="$DISK.steps"
LOG=""
QPID=""
FPID=""

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

cleanup() {
    cleanup_boot
    rm -f "$WORK" "$FEED" "$STEPS" "$DISK".log*
}

trap 'cleanup' EXIT
trap 'cleanup; exit 130' INT
trap 'cleanup; exit 143' TERM
trap 'cleanup; exit 129' HUP

fail() {
    if [ -n "$LOG" ] && [ -f "$LOG" ]; then
        cp "$LOG" "$DISK.fail.log" 2>/dev/null || true
        echo "grub-test: the failing boot's console log: $DISK.fail.log" >&2
    fi
    cleanup_boot
    echo "grub-test: FAIL: $1" >&2
    exit 1
}

# Wait up to $3 seconds until the log holds at least $2 occurrences of $1.
wait_count() {
    i=0
    while [ "$i" -lt "$3" ]; do
        n="$(grep -ac "$1" "$LOG" 2>/dev/null)" || true
        if [ "${n:-0}" -ge "$2" ]; then
            return 0
        fi
        if [ "$(($(date +%s) - START))" -gt "$DEADLINE" ]; then
            fail "the run passed its ${DEADLINE}s deadline waiting for $1"
        fi
        sleep 1
        i=$((i + 1))
    done
    return 1
}

# step <pattern> <count> <command>: the feeder sends <command> once the log
# holds at least <count> occurrences of <pattern>.
step() {
    printf '%s\t%s\t%s\n' "$2" "$1" "$3" >> "$STEPS"
}

# Boot $WORK on the current $STEPS; $LOG must be set by the caller. The
# feeder is pattern-paced: login prompts and shell markers alike are patterns
# with counts, so a session that reboots in place keeps feeding correctly.
boot_feed() {
    rm -f "$FEED"
    mkfifo "$FEED"
    qemu-system-i386 -cpu max -display none -rtc base=utc \
        -drive "file=$WORK,format=raw,if=ide,index=0,media=disk" \
        -serial stdio < "$FEED" > "$LOG" 2>&1 &
    QPID=$!
    (
        while IFS='	' read -r count pattern command; do
            waited=0
            while [ "$waited" -lt 600 ]; do
                seen="$(grep -ac "$pattern" "$LOG" 2>/dev/null)" || true
                if [ "${seen:-0}" -ge "$count" ]; then
                    break
                fi
                if [ "$(($(date +%s) - START))" -gt "$DEADLINE" ]; then
                    break
                fi
                sleep 1
                waited=$((waited + 1))
            done
            seen="$(grep -ac "$pattern" "$LOG" 2>/dev/null)" || true
            if [ "${seen:-0}" -lt "$count" ]; then
                break
            fi
            printf '%s\n' "$command"
        done < "$STEPS"
        held=0
        while [ "$held" -lt 120 ] && kill -0 "$QPID" 2>/dev/null; do
            sleep 1
            held=$((held + 1))
        done
    ) > "$FEED" &
    FPID=$!
}

# A watch-only boot (the matrix states): no feed, a bounded wait for $1.
boot_watch() {
    rm -f "$FEED"
    mkfifo "$FEED"
    qemu-system-i386 -cpu max -display none -rtc base=utc \
        -drive "file=$WORK,format=raw,if=ide,index=0,media=disk" \
        -serial stdio < "$FEED" > "$LOG" 2>&1 &
    QPID=$!
    (sleep 90) > "$FEED" &
    FPID=$!
    wait_count "$1" 1 75 || fail "a matrix boot never reached $1"
    cleanup_boot
}

if ! command -v docker > /dev/null 2>&1; then
    fail "Docker is not installed, and this harness is the milestone's proof"
fi
if ! docker info > /dev/null 2>&1; then
    fail "the Docker daemon is not answering, and this harness is the milestone's proof"
fi

rm -f "$WORK"
cp "$DISK" "$WORK"

LOG="$DISK.log.first"
: > "$STEPS"
step "rach2 login:" 1 "root"
step "Password:" 1 "$ROOTPASSWORD"
step "Password:" 1 "bootctl grub /core-b.img && echo GRUBB\"\"OK"
step "GRUBBOK" 1 "echo PRERB; reboot -f"
step "rach2 login:" 2 "root"
step "Password:" 2 "$ROOTPASSWORD"
step "Password:" 2 "bootctl grub /core-a.img && echo GRUBA\"\"OK"
step "GRUBAOK" 1 "( exec 7</dev/grubcore; exec 8</dev/grubcore ) 2>&1 | grep -qi busy && echo SECOND\"\"BUSY"
step "SECONDBUSY" 1 "( exec 7</dev/grubcore ) && echo AGAIN\"\"OK"
step "AGAINOK" 1 "poweroff -f"
boot_feed
wait_count "AGAINOK" 1 900 || fail "the boot loader swap never completed"
cleanup_boot

grep -aq "GRUBBOK" "$LOG" || fail "installing core B failed"
grep -aq "GRUBAOK" "$LOG" || fail "installing core A back failed"

# Each install must have used the tail the other one did not. A tool that wrote
# the tail the machine is running from would leave the disk unbootable if the
# write were cut short, and the markers alone would not catch it: overwriting
# one tail twice still boots the image most recently written there.
TAILS="$(grep -ao 'in the tail at LBA [0-9]*' "$LOG" | sort -u | wc -l | tr -d ' ')"
if [ "$TAILS" -ne 2 ]; then
    fail "the two installs used $TAILS distinct tail(s), not 2"
fi

# Only one open of the window at a time. Two installs running at once could each
# verify their own tail and then leave the selector naming one image with the
# other one's length, and nothing recovers from that remotely. The second open
# must be refused, and the refusal must not outlive the process that held it.
grep -aq "SECONDBUSY" "$LOG" || fail "a second open of /dev/grubcore was not refused"
grep -aq "AGAINOK" "$LOG" || fail "/dev/grubcore stayed locked after its holder exited"

# The disk is built with an unmarked core, so the first boot says nothing and
# the first marker in this log belongs to the second boot, which must be B
# because the first boot installed it. The marker comes from inside the running
# image, so it names the image that ran and not the pointer that selected it.
if [ "$(grep -ac 'GRUBCORE=' "$LOG")" -ne 1 ]; then
    fail "expected exactly one marked boot in this log, saw $(grep -ac 'GRUBCORE=' "$LOG")"
fi
SECOND="$(grep -ao 'GRUBCORE=[AB]' "$LOG" | head -1)"
if [ "$SECOND" != "GRUBCORE=B" ]; then
    fail "the boot after installing core B ran [$SECOND], not B"
fi

# And back again: the second install put A in the tail B is not in, so this boot
# must run A.
LOG="$DISK.log.back"
boot_watch "rach2 login:"
THIRD="$(grep -ao 'GRUBCORE=[AB]' "$LOG" | head -1)"
if [ "$THIRD" != "GRUBCORE=A" ]; then
    fail "the boot after installing core A ran [$THIRD], not A"
fi

echo "grub-test: the machine replaced its own boot loader, twice, and each boot ran the image it had just installed: OK"
echo "grub-test: PASS"
