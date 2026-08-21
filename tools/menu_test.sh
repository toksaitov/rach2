#!/usr/bin/env sh

#
# menu_test.sh: the boot menu that can be rewritten, and the stub that cannot.
#
# GRUB reads two files on this disk. /boot/grub/grub.cfg is written once when
# the disk is built and never again; it defines every entry the machine needs
# to boot and then sources /boot/boot.cfg, the rewritable half that bootctl
# overwrites in place. The order is the whole safety property: the stub's
# entries are defined first, so they hold the low indices whatever the second
# file contains, and `set fallback` takes a number.
#
# Four boots of one disk prove it. The first stages a menu defining two entries
# the stub does not have, round-trips a grubenv key through `bootctl env`, and
# corrupts the candidate kernel so the next boot has to take the fallback. The
# second boot proves both staged entries reached the menu and that the fallback
# NUMBER still named the stub's own entry rather than a staged one, which is the
# property the ordering exists for. The third truncates the staged file
# mid-entry from the host and the machine must still boot through the stub. The
# fourth reads the slot's extent back, which says bootctl overwrote bytes in
# place rather than rewriting a file.
#
# What no boot here does is let the staged file choose the default. It cannot:
# a slot overwritten in place is not written atomically, so a file in a torn
# state must not be able to point the machine at anything.
#
# Usage: menu_test.sh <grub-disk> <root password>
#

set -e

DISK="$1"
ROOTPASSWORD="$2"

if [ -z "$DISK" ] || [ -z "$ROOTPASSWORD" ]; then
    echo "usage: menu_test.sh <grub-disk> <root password>" >&2
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
        echo "menu-test: the failing boot's console log: $DISK.fail.log" >&2
    fi
    cleanup_boot
    echo "menu-test: FAIL: $1" >&2
    exit 1
}

# Wait up to $3 seconds until the log has at least $2 occurrences of $1.
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
# Cancelling removes the key rather than emptying it: an empty next_entry is the
# marker a trial boot leaves, and a commit requires it.
grep -a "ENVB=" "$LOG" | grep -aq "absent" || \
    fail "bootctl cancel did not remove the trial key, so it left the consumed marker behind"

# The candidate is now unbootable, so GRUB takes the fallback NUMBER. That
# number indexes the merged list, which is why the stub's entries have to be
# defined before the staged file is sourced: index 1 must still be the stub's
# own fallback and not whatever a staged menu put there.
LOG="$DISK.log.fallback"
boot_watch "rach2 login:"
grep -aq "stagedone" "$LOG" || fail "the staged menu's entries are not in the menu at all"
grep -aq "stagedtwo" "$LOG" || fail "the staged menu's second entry is missing"
grep -aq "Falling back to .Rach2 (fallback kernel)" "$LOG" || \
    fail "the fallback number did not select the stub's own fallback entry"

# The whole list, in order, because every index is a contract and only one of
# them is checked by the fallback above. The menu is drawn on the serial console
# with one title per line, so the order they appear in is the order GRUB has
# them in.
python3 -c "
import re, sys
want = ['Rach2', 'Rach2 (fallback kernel)', 'Rach2 (static address)',
        'Rach2 (fallback kernel, static address)', 'Rach2 (kernel debug shell)',
        'Rach2 (recovery, not yet built)',
        'Rach2 (recovery, static address, not yet built)', 'stagedone', 'stagedtwo']
text = open('$LOG', 'rb').read().decode('latin-1')
# GRUB draws one title per screen row, positioned with a cursor escape, and
# marks the highlighted one with a leading asterisk.
seen = []
for row, body in re.findall(r'\\[(\\d+);3H([^\\x1b]{0,60})', text):
    t = body.strip().lstrip('*').strip()
    if t and t not in seen:
        seen.append(t)
if seen != want:
    sys.exit('menu-test: the entry order is %r, not %r' % (seen, want))
" || fail "the six stub entries are not in the order their numbers assume, or the staged two are not after them"
if grep -a "Falling back to" "$LOG" | grep -aq "staged"; then
    fail "the fallback number selected a staged entry, so the stub's entries no longer hold the low indices"
fi

# The staged menu, truncated mid-entry from the host: the file keeps its size,
# so nothing about the FAT changes, but its last menuentry has no closing brace.
# GRUB must report the parse error and the machine must still boot, because the
# stub defined its own entries before it read this one.
python3 -c "
import sys
work = '$WORK'
d = open(work, 'rb').read()
i = d.find(b'menuentry stagedone --id stagedone')
if i < 0:
    sys.exit('menu-test: the staged menu is not on the disk')
head = b'menuentry stagedone --id stagedone {\n'
if i + 8192 > len(d):
    sys.exit('menu-test: the menu slot runs past the image')
f = open(work, 'r+b')
f.seek(i)
f.write(head + b'#' * (8192 - len(head)))
f.close()
"

LOG="$DISK.log.torn"
boot_watch "rach2 login:"
grep -aqi "error" "$LOG" || \
    fail "a truncated menu produced no loader error, so GRUB did not read it at all"
if grep -a "Falling back to" "$LOG" | grep -aq "staged"; then
    fail "a truncated menu still reached one of its own entries"
fi

# A trial of the static-address candidate must fall back to the static-address
# fallback, entry 3, and not to the ordinary one. The two carry different
# command lines on a machine whose network has no DHCP server, which is the only
# reason the static entries exist, so falling back to entry 1 there is a machine
# that boots and cannot be reached. The candidate is still corrupt from above,
# so arming the trial is enough to reach the fallback.
LOG="$DISK.log.armstatic"
: > "$STEPS"
step "rach2 login:" 1 "root"
step "Password:" 1 "$ROOTPASSWORD"
step "Password:" 1 "bootctl trial candidate-static > /dev/null && echo ARMED\"\"OK"
step "ARMEDOK" 1 "poweroff -f"
boot_feed
wait_count "ARMEDOK" 1 600 || fail "the static trial was never armed"
cleanup_boot

LOG="$DISK.log.staticfallback"
boot_watch "rach2 login:"
grep -aq "Falling back to .Rach2 (fallback kernel, static address)" "$LOG" || \
    fail "a static-address trial fell back to the ordinary fallback entry"

# The other way a static entry is chosen: not one boot but every boot, which is
# a machine left on a network with no DHCP server. The saved default has to
# name it, and its fallback has to be the static one for the same reason.
LOG="$DISK.log.savestatic"
: > "$STEPS"
step "rach2 login:" 1 "root"
step "Password:" 1 "$ROOTPASSWORD"
step "Password:" 1 "bootctl env saved_entry candidate-static > /dev/null && echo SAVED\"\"OK"
step "SAVEDOK" 1 "poweroff -f"
boot_feed
wait_count "SAVEDOK" 1 600 || fail "the static default was never saved"
cleanup_boot

LOG="$DISK.log.savedstaticboot"
boot_watch "rach2 login:"
grep -aq "Falling back to .Rach2 (fallback kernel, static address)" "$LOG" || \
    fail "a saved static default did not select the static entries"

LOG="$DISK.log.savedback"
: > "$STEPS"
step "rach2 login:" 1 "root"
step "Password:" 1 "$ROOTPASSWORD"
step "Password:" 1 "bootctl env saved_entry candidate > /dev/null && echo BACK\"\"OK"
step "BACKOK" 1 "poweroff -f"
boot_feed
wait_count "BACKOK" 1 600 || fail "the saved default was never put back"
cleanup_boot

# The slot's extent, read back after all of it: bootctl overwrote bytes in place
# rather than rewriting a file, so the chain found when the volume opened is the
# chain still there.
LOG="$DISK.log.after"
: > "$STEPS"
step "rach2 login:" 1 "root"
step "Password:" 1 "$ROOTPASSWORD"
step "Password:" 1 "bootctl status | busybox grep 'boot.cfg' | busybox sed s/^/SLOTB\"\"=/"
step "SLOTB=" 1 "poweroff -f"
boot_feed
wait_count "SLOTB=" 1 600 || fail "the closing boot never reported the slot back"
cleanup_boot

SLOTA="$(grep -a 'SLOTA=' "$DISK.log.stage" | head -1 | sed 's/.*first sector //')"
SLOTB="$(grep -a 'SLOTB=' "$LOG" | head -1 | sed 's/.*first sector //')"
if [ -z "$SLOTA" ] || [ "$SLOTA" != "$SLOTB" ]; then
    fail "the menu slot's extent changed across a staged write: [$SLOTA] then [$SLOTB]"
fi

echo "menu-test: a staged menu's entries reached the menu, the fallback number still named the stub's own entry, a static trial and a saved static default both fell back to the static fallback, a truncated menu left the machine booting, and the slot never moved: OK"
echo "menu-test: PASS"
