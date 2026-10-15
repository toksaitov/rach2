#!/usr/bin/env sh

#
# menu.sh: the boot menu that can be rewritten, and the stub that cannot.
#
# GRUB reads two files on this disk. /boot/grub/grub.cfg is written once when
# the disk is built and never again; it defines every entry the machine needs
# to boot and then sources /boot/boot.cfg, the rewritable half that bootctl
# overwrites in place. The order is what makes the menu safe: the stub's
# entries are defined first, so they keep the low indices whatever the second
# file contains, and `set fallback` takes a number.
#
# Four boots of one disk check it. The first stages a menu defining two
# entries the stub does not have, round-trips a grubenv key through `bootctl
# env`, and corrupts the candidate kernel so that the next boot must take the
# fallback. The second requires both staged entries in the menu after the
# stub's, and the fallback number to name the stub's own entry and not a
# staged one. In the third, a staged file truncated mid-entry from the host
# must still leave the machine booting through the stub. The last boot reads
# the slot's extent back, which shows that bootctl overwrote bytes in place
# and did not rewrite the file.
#
# No boot here lets the staged file choose the default, and it cannot: a slot
# overwritten in place is not written atomically, so a file in a torn state
# must not be able to point the machine at anything.
#
# Usage: menu.sh <grub-disk> <root password>
#

# The emulator of the architecture tests/system/suites.mk names.
QEMU=${RACH2_QEMU:-qemu-system-i386}

# The runner helper, whose `until` and `gone` are the suite's waits.
RUNUTIL="$(cd "$(dirname "$0")/../../.." && pwd)/build/tests/system/runutil"
set -e

REPO="$(cd "$(dirname "$0")/../../.." && pwd)"

DISK="$1"
ROOTPASSWORD="$2"

if [ -z "$DISK" ] || [ -z "$ROOTPASSWORD" ]; then
    echo "usage: menu_test.sh <grub-disk> <root password>" >&2
    exit 2
fi

# shellcheck source=tests/system/deadline.sh
. "$REPO/tests/system/deadline.sh"
DEADLINE=$(scaled 2400)
START="$(date +%s)"

WORK="$DISK.work"
FEED="$DISK.feed"
STEPS="$DISK.steps"
LOG=""
QPID=""
FPID=""

# Stop the machine and the feeder of this boot, and remove the feed.
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

# Stop the boot, and remove the disk's copy, the feed, the steps and the logs.
cleanup() {
    cleanup_boot
    rm -f "$WORK" "$FEED" "$STEPS" "$DISK".log*
}

trap 'cleanup' EXIT
trap 'cleanup; exit 130' INT
trap 'cleanup; exit 143' TERM
trap 'cleanup; exit 129' HUP

# fail REASON: keep the failing boot's log as DISK.fail.log, stop the boot,
# print REASON and exit 1.
fail() {
    if [ -n "$LOG" ] && [ -f "$LOG" ]; then
        cp "$LOG" "$DISK.fail.log" 2>/dev/null || true
        echo "menu-test: the failing boot's console log: $DISK.fail.log" >&2
    fi
    cleanup_boot
    echo "menu-test: FAIL: $1" >&2
    exit 1
}

# Wait up to $3 seconds until the log has at least $2 occurrences of $1, and
# return 1 if it does not. The bound is $3 or what is left of the run's
# budget, whichever is less, so that a run cannot pass its deadline inside a
# wait; a run past its deadline fails.
wait_count() {
    remaining=$((START + DEADLINE - $(date +%s)))
    if [ "$remaining" -le 0 ]; then
        fail "the run passed its ${DEADLINE}s deadline waiting for $1"
    fi
    bound=$3
    [ "$bound" -le "$remaining" ] || bound=$remaining
    "$RUNUTIL" until "$LOG" "$1" "$bound" "$2" && return 0
    if [ "$(($(date +%s) - START))" -gt "$DEADLINE" ]; then
        fail "the run passed its ${DEADLINE}s deadline waiting for $1"
    fi
    return 1
}

# step <pattern> <count> <command>: the feeder sends <command> once the log
# has at least <count> occurrences of <pattern>.
step() {
    printf '%s\t%s\t%s\n' "$2" "$1" "$3" >> "$STEPS"
}

# Boot $WORK on the current $STEPS; $LOG must be set by the caller. The
# feeder is pattern-paced: login prompts and shell markers alike are patterns
# with counts, so a session that reboots in place keeps feeding correctly.
boot_feed() {
    rm -f "$FEED"
    mkfifo "$FEED"
    "$QEMU" -cpu max -display none -rtc base=utc \
        -drive "file=$WORK,format=raw,if=ide,index=0,media=disk" \
        -serial stdio < "$FEED" > "$LOG" 2>&1 &
    QPID=$!
    (
        while IFS='	' read -r count pattern command; do
            "$RUNUTIL" until "$LOG" "$pattern" 600 "$count" || true
            seen="$(grep -ac "$pattern" "$LOG" 2>/dev/null)" || true
            if [ "${seen:-0}" -lt "$count" ]; then
                break
            fi
            printf '%s\n' "$command"
        done < "$STEPS"
        "$RUNUTIL" gone "$QPID" 120 || true
    ) > "$FEED" &
    FPID=$!
}

# A boot with nothing typed: wait up to 75 seconds for $1, then stop it.
boot_watch() {
    rm -f "$FEED"
    mkfifo "$FEED"
    "$QEMU" -cpu max -display none -rtc base=utc \
        -drive "file=$WORK,format=raw,if=ide,index=0,media=disk" \
        -serial stdio < "$FEED" > "$LOG" 2>&1 &
    QPID=$!
    (sleep "$(scaled 90)") > "$FEED" & # interval: a holder keeps the console's feed open while the machine runs
    FPID=$!
    wait_count "$1" 1 75 || fail "a matrix boot never reached $1"
    cleanup_boot
}

rm -f "$WORK"
cp "$DISK" "$WORK"

# Two entries, and no attempt to choose a default. A file overwritten in place
# cannot be written atomically, so one in a torn state must not be able to point
# the machine anywhere; what it may do is add entries a person at the console
# can select. Written in two pieces because the console's line discipline has a
# buffer a single line holding both would overrun.
MENU1='menuentry stagedone --id stagedone {\nmultiboot /boot/fallback rach2.root=disk rach2.menu=stagedone\nboot\n}\n'
MENU2='menuentry stagedtwo --id stagedtwo {\nmultiboot /boot/fallback rach2.root=disk rach2.menu=stagedtwo\nboot\n}\n'

LOG="$DISK.log.stage"
: > "$STEPS"
step "rach2 login:" 1 "root"
step "Password:" 1 "$ROOTPASSWORD"
step "Password:" 1 "bootctl status | busybox grep 'boot.cfg' | busybox sed s/^/SLOTA\"\"=/"
step "SLOTA=" 1 "busybox printf '%b' '$MENU1' > /tmp/m.cfg; echo PART\"\"ONE"
step "PARTONE" 1 "busybox printf '%b' '$MENU2' >> /tmp/m.cfg; bootctl config /tmp/m.cfg && echo STAGED\"\"OK"
step "STAGEDOK" 1 "bootctl env next_entry candidate > /dev/null; bootctl env next_entry | busybox sed s/^/ENVA\"\"=/"
step "ENVA=" 1 "bootctl cancel > /dev/null && bootctl env next_entry | busybox sed s/^/ENVB\"\"=/"
step "ENVB=" 1 "S=\$(bootctl status | busybox awk '/^bootctl: candidate: first sector/{print \$5}' | busybox tr -d ,); busybox dd if=/dev/zero of=/dev/bootpart bs=512 seek=\$S count=1 2>/dev/null && echo CORRUPT\"\"OK"
step "CORRUPTOK" 1 "poweroff -f"
boot_feed
wait_count "CORRUPTOK" 1 900 || fail "the staging boot never finished"
cleanup_boot

grep -aq "STAGEDOK" "$LOG" || fail "the menu did not stage"
# The two grubenv keys this tool writes are the trial state machine, so a round
# trip through `bootctl env` is what says the block took the write.
grep -a "ENVA=" "$LOG" | grep -aq "candidate" || fail "bootctl env did not read back the armed value"
# Cancelling removes the key instead of emptying it: an empty next_entry is the
# marker a trial boot leaves, and a commit requires it.
grep -a "ENVB=" "$LOG" | grep -aq "absent" || \
    fail "bootctl cancel did not remove the trial key, so it left the consumed marker behind"

# The candidate is now unbootable, so GRUB takes the fallback number. That
# number indexes the merged list, which is why the stub's entries have to be
# defined before the staged file is sourced: index 1 must still be the stub's
# own fallback and not whatever a staged menu put there.
LOG="$DISK.log.fallback"
boot_watch "rach2 login:"
grep -aq "stagedone" "$LOG" || fail "the staged menu's entries are not in the menu at all"
grep -aq "stagedtwo" "$LOG" || fail "the staged menu's second entry is missing"
# The closing quote is part of the pattern, so a title that begins with these
# same words cannot satisfy it.
grep -aq "Falling back to .Start Rach2 with the previous kernel'" "$LOG" || \
    fail "the fallback number did not select the stub's own fallback entry"

# The whole list, in order, since every index is a contract and the fallback
# above checks only one. GRUB draws one title per screen row of the serial
# console, positioned with a cursor escape, and marks the highlighted one with
# a leading asterisk; the titles are cut out of the log in the order they were
# drawn, each kept once.
SEEN="$(tr '\033' '\n' < "$LOG" | sed -n 's/^\[[0-9][0-9]*;3H//p' |
    sed 's/^[ *]*//; s/[ ]*$//' | grep -a -v '^$' |
    awk '!seen[$0]++')"
WANT="Start Rach2
Start Rach2 with the previous kernel
Start the Rach2 recovery system (not yet built)
stagedone
stagedtwo"
[ "$SEEN" = "$WANT" ] || \
    fail "the three stub entries are not in the order their numbers assume, or the staged two are not after them"
if grep -a "Falling back to" "$LOG" | grep -aq "staged"; then
    fail "the fallback number selected a staged entry, so the stub's entries no longer hold the low indices"
fi

# The staged menu, truncated mid-entry from the host: the file keeps its size,
# so nothing about the FAT changes, but its last menuentry has no closing brace.
# GRUB must report the parse error and the machine must still boot, because the
# stub defined its own entries before it read this one.
MENU_AT="$("$REPO/build/tools/inspect/binfind" --offset \
    'menuentry stagedone --id stagedone' "$WORK")" || \
    fail "the staged menu is not on the disk"
[ $((MENU_AT + 8192)) -le "$(wc -c < "$WORK" | tr -d ' ')" ] || \
    fail "the menu slot runs past the image"
printf 'menuentry stagedone --id stagedone {\n' > "$WORK.menu"
n="$(wc -c < "$WORK.menu" | tr -d ' ')"
dd if=/dev/zero bs=1 count=$((8192 - n)) 2>/dev/null | tr '\0' '#' \
    >> "$WORK.menu"
dd if="$WORK.menu" of="$WORK" bs=1 seek="$MENU_AT" conv=notrunc status=none
rm -f "$WORK.menu"

LOG="$DISK.log.torn"
boot_watch "rach2 login:"
grep -aqi "error" "$LOG" || \
    fail "a truncated menu produced no loader error, so GRUB did not read it at all"
if grep -a "Falling back to" "$LOG" | grep -aq "staged"; then
    fail "a truncated menu still reached one of its own entries"
fi

# The slot's extent, read back after all of it: bootctl overwrote bytes in place
# instead of rewriting a file, so the chain found when the volume opened is the
# chain still there.
LOG="$DISK.log.after"
: > "$STEPS"
step "rach2 login:" 1 "root"
step "Password:" 1 "$ROOTPASSWORD"
step "Password:" 1 "bootctl status | busybox grep 'boot.cfg' | busybox sed s/^/SLOTB\"\"=/; echo SLOTB\"\"DONE"
step "SLOTBDONE" 1 "poweroff -f"
boot_feed
# The token is the one after the line, since a wait that ends at the line's
# start stops the machine before a slow guest has written the rest of it.
wait_count "SLOTBDONE" 1 600 || fail "the closing boot never reported the slot back"
cleanup_boot

SLOTA="$(grep -a 'SLOTA=' "$DISK.log.stage" | head -1 | sed 's/.*first sector //')"
SLOTB="$(grep -a 'SLOTB=' "$LOG" | head -1 | sed 's/.*first sector //')"
if [ -z "$SLOTA" ] || [ "$SLOTA" != "$SLOTB" ]; then
    fail "the menu slot's extent changed across a staged write: [$SLOTA] then [$SLOTB]"
fi

echo "menu-test: a staged menu's entries reached the menu, the fallback number still named the stub's own entry, a truncated menu left the machine booting, and the slot never moved: OK"
echo "menu-test: PASS"
