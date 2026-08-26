#!/usr/bin/env sh

#
# install_test.sh: the machine installs its own kernel, and the trial, the
# commit, the fallback and the repair are all proved on the booted disk.
#
# The disk is the GRUB one (mkdisk_grub.sh): no -kernel anywhere, so what
# boots is what GRUB reads from the boot partition, which is what makes a
# reboot after an install mean something. The whole loop runs twice in one
# invocation, with a distinct banner marker per pass, so the second pass
# starts from the disk the first modified and proves the flow idempotent.
#
# One pass: log in as root and prove rach2.slot=candidate; edit the pass's
# marker into the kernel's serial banner in /src/rach2 (the slot token alone
# is menu text, and a hash of the slot proves only bytes at rest, so the
# marker is what proves which bytes RUN); build the kernel in place; install
# it; prove a commit is refused while the trial is armed; reboot -f into the
# trial and prove the marker and the slot token; commit; cold-boot and prove
# the saved default held. Then the recovery half: corrupt the candidate's
# first sector through /dev/bootpart at the extent bootctl status
# names, reboot into the fallback (the marker must be absent from that boot's
# log interval), reinstall from inside, reboot into the repaired candidate,
# and commit so the repair is the saved default.
#
# After the two passes, the interruption matrix's determinate states are
# booted from host-crafted environment blocks: a destroyed header and a
# complete old value select the fallback, a complete new value the candidate,
# told apart by the marker with no login (the banner prints before one).
#
# The feeder paces on log patterns with occurrence counts, because a reboot
# brings a second "login:" a plain grep would match the first of; assertions
# over the log are made post-hoc on token byte offsets, each boot's claims
# checked only inside its own interval. The disk is assembled natively with
# mtools and the pinned GRUB artifacts, so the whole harness runs anywhere the
# rest of the suites do.
#
# Usage: install_test.sh <grub-disk> <root password>
#

set -e

REPO="$(cd "$(dirname "$0")/../../.." && pwd)"

DISK="$1"
ROOTPASSWORD="$2"

if [ -z "$DISK" ] || [ -z "$ROOTPASSWORD" ]; then
    echo "usage: install_test.sh <grub-disk> <root password>" >&2
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
        echo "install-test: the failing boot's console log: $DISK.fail.log" >&2
    fi
    cleanup_boot
    echo "install-test: FAIL: $1" >&2
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

rm -f "$WORK"
cp "$DISK" "$WORK"

# The environment block's offset in the image, for the matrix states.
ENVOFF="$("$REPO/build/tools/inspect/binfind" --offset \
    '# GRUB Environment Block' "$WORK" || echo -1)"
if [ "$ENVOFF" -lt 0 ]; then
    fail "the disk has no environment block"
fi

# One pass of the install loop. $1 is the pass number, $2 the sed pattern the
# marker edit replaces, $3 the marker this pass plants.
# The byte offset of a token's first occurrence in the log, or empty when
# it is absent. `grep -ab` numbers each matching line by its byte offset,
# which is what puts the console tokens in order without a parser: the
# console writes them as they happen, so an ordering assertion is an
# assertion about these numbers.
token_at() {
    grep -abo -m1 -e "$2" "$1" 2>/dev/null | head -1 | cut -d: -f1
}

# Whether `$3` appears in the log between the byte offsets $1 and $2.
token_between() {
    dd if="$4" bs=1 skip="$1" count=$(($2 - $1)) status=none 2>/dev/null |
        grep -aq -e "$3"
}

run_pass() {
    P="$1"
    OLDMARK="$2"
    MARK="$3"
    TOKEN="$4"

    # Session A: prove the slot, plant the marker, build, install, prove the
    # armed-commit refusal, reboot into the trial, prove it, commit, stop.
    : > "$STEPS"
    step "rach2 login:" 1 "root"
    step "Password:" 1 "$ROOTPASSWORD"
    step "Password:" 1 "busybox grep -o 'rach2.slot=[a-z]*' /proc/cmdline | busybox sed s/slot/slotA${P}/"
    step "rach2.slotA${P}=candidate" 1 "cd /src/rach2 && busybox sed -i 's/${OLDMARK}/${MARK}/' kernel.c && busybox grep -c '${MARK}' kernel.c | busybox sed s/^/SED${P}N/"
    step "SED${P}N2" 1 "make -f guest.mk > /tmp/ik.out 2>&1; echo BUILD${P}RC=\$?"
    step "BUILD${P}RC=0" 1 "bootctl kernel kernel && echo INST\"\"${P}A"
    step "INST${P}A" 1 "bootctl commit > /dev/null 2>&1; echo ARMED${P}RC=\$?"
    step "ARMED${P}RC=1" 1 "echo PRERB${P}A; reboot -f"
    step "rach2 login:" 2 "root"
    step "Password:" 2 "$ROOTPASSWORD"
    step "Password:" 2 "busybox grep -o 'rach2.slot=[a-z]*' /proc/cmdline | busybox sed s/slot/slotB${P}/"
    step "rach2.slotB${P}=candidate" 1 "bootctl commit && echo COMMIT\"\"${P}A"
    step "COMMIT${P}A" 1 "poweroff -f"

    LOG="$DISK.log.p${P}a"
    boot_feed
    wait_count "COMMIT${P}A" 1 900 || fail "pass $P: the trial boot never committed"
    waited=0
    while [ "$waited" -lt 60 ] && kill -0 "$QPID" 2>/dev/null; do
        sleep 1
        waited=$((waited + 1))
    done
    cleanup_boot

    # Post-hoc, on ordered byte offsets: the marker must appear between the
    # reboot and the second slot token (the trial boot's own interval) and
    # not before the reboot (the running kernel was unmarked or previously
    # marked; the token echoes carry the marker text and do not count).
    ARMED_AT="$(token_at "$LOG" "ARMED${P}RC=1")"
    RB_AT="$(token_at "$LOG" "PRERB${P}A")"
    SLOT2_AT="$(token_at "$LOG" "rach2.slotB${P}=candidate")"
    LOGEND="$(wc -c < "$LOG" | tr -d ' ')"
    [ -n "$ARMED_AT" ] || fail "pass $P: the armed-trial commit was not refused, so the trial was never armed or the gate is gone"
    if [ -z "$RB_AT" ] || [ -z "$SLOT2_AT" ] ||
       [ "$SLOT2_AT" -lt "$RB_AT" ] || [ "$ARMED_AT" -gt "$RB_AT" ]; then
        fail "pass $P: the session tokens are missing or out of order"
    fi
    token_between "$RB_AT" "$SLOT2_AT" "Booting... ${TOKEN}" "$LOG" || \
        fail "pass $P: the installed kernel did not print its marker"
    token_between "$SLOT2_AT" "$LOGEND" "bootctl: committed" "$LOG" || \
        fail "pass $P: the trial commit did not report success"

    # Session B: cold boot; the saved default must be the marked candidate.
    # Then the recovery half: corrupt the candidate at the extent the
    # installer names, reboot into the fallback, reinstall, reboot into the
    # repaired candidate, commit.
    : > "$STEPS"
    step "rach2 login:" 1 "root"
    step "Password:" 1 "$ROOTPASSWORD"
    step "Password:" 1 "busybox grep -o 'rach2.slot=[a-z]*' /proc/cmdline | busybox sed s/slot/slotC${P}/"
    step "rach2.slotC${P}=candidate" 1 "S=\$(bootctl status | busybox awk '/^bootctl: candidate: first sector/{print \$5}' | busybox tr -d ,); echo SECTOR${P}=\$S; busybox dd if=/dev/zero of=/dev/bootpart bs=512 seek=\$S count=1 2>/dev/null && echo CORRUPT\"\"${P}OK"
    step "CORRUPT${P}OK" 1 "echo PRERB${P}B; reboot -f"
    step "rach2 login:" 2 "root"
    step "Password:" 2 "$ROOTPASSWORD"
    step "Password:" 2 "busybox grep -o 'rach2.slot=[a-z]*' /proc/cmdline | busybox sed s/slot/slotD${P}/"
    step "rach2.slotD${P}=fallback" 1 "cd /src/rach2 && bootctl kernel kernel && echo REPAIR\"\"${P}OK"
    step "REPAIR${P}OK" 1 "echo PRERB${P}C; reboot -f"
    step "rach2 login:" 3 "root"
    step "Password:" 3 "$ROOTPASSWORD"
    step "Password:" 3 "busybox grep -o 'rach2.slot=[a-z]*' /proc/cmdline | busybox sed s/slot/slotE${P}/"
    step "rach2.slotE${P}=candidate" 1 "bootctl commit && echo COMMIT\"\"${P}B"
    step "COMMIT${P}B" 1 "poweroff -f"

    LOG="$DISK.log.p${P}b"
    boot_feed
    wait_count "COMMIT${P}B" 1 900 || fail "pass $P: the repair was never committed"
    waited=0
    while [ "$waited" -lt 60 ] && kill -0 "$QPID" 2>/dev/null; do
        sleep 1
        waited=$((waited + 1))
    done
    cleanup_boot

    COLD_AT="$(token_at "$LOG" "Booting... ${TOKEN}")"
    RB_B="$(token_at "$LOG" "PRERB${P}B")"
    SLOT_D="$(token_at "$LOG" "rach2.slotD${P}=fallback")"
    RB_C="$(token_at "$LOG" "PRERB${P}C")"
    SLOT_E="$(token_at "$LOG" "rach2.slotE${P}=candidate")"
    LOGEND="$(wc -c < "$LOG" | tr -d ' ')"
    if [ -z "$COLD_AT" ] || [ -z "$RB_B" ] || [ -z "$SLOT_D" ] ||
       [ -z "$RB_C" ] || [ -z "$SLOT_E" ]; then
        fail "pass $P: the recovery tokens are missing"
    fi
    if [ "$COLD_AT" -ge "$RB_B" ] || [ "$RB_B" -ge "$SLOT_D" ] ||
       [ "$SLOT_D" -ge "$RB_C" ] || [ "$RB_C" -ge "$SLOT_E" ]; then
        fail "pass $P: the recovery tokens are out of order"
    fi
    if token_between "$RB_B" "$SLOT_D" "Booting... ${TOKEN}" "$LOG"; then
        fail "pass $P: the fallback boot has the marker"
    fi
    token_between "$RB_C" "$SLOT_E" "Booting... ${TOKEN}" "$LOG" || \
        fail "pass $P: the repaired candidate did not print its marker"
    token_between "$SLOT_E" "$LOGEND" "bootctl: committed" "$LOG" || \
        fail "pass $P: the repair commit did not report success"
}

run_pass 1 "Rach2 Booting\\.\\.\\." "Rach2 Booting... IKMARKA" "IKMARKA"
run_pass 2 "IKMARKA" "IKMARKB" "IKMARKB"

# The matrix's determinate states, crafted on the block and booted with no
# login: the marked candidate prints IKMARKB before any prompt, so presence
# tells the entries apart. A destroyed header and a complete old value select
# the fallback; a complete new value selects the candidate once.
# The block is built whole and written in place at the recorded offset:
# 1024 bytes, the lines given here and a '#' fill, the way grub-editenv
# writes one; `craft_env` takes the lines.
craft_env() {
    printf '%s' "$1" > "$WORK.env"
    n="$(wc -c < "$WORK.env" | tr -d ' ')"
    dd if=/dev/zero bs=1 count=$((1024 - n)) 2>/dev/null | tr '\0' '#' \
        >> "$WORK.env"
    dd if="$WORK.env" of="$WORK" bs=1 seek="$ENVOFF" conv=notrunc \
        status=none
    rm -f "$WORK.env"
}

# The destroyed header: 1024 bytes of one repeated pattern, so nothing in
# the block reads as a variable at all.
smash_env() {
    dd if=/dev/zero bs=1 count=512 2>/dev/null | tr '\0' '\336' > "$WORK.env"
    dd if=/dev/zero bs=1 count=512 2>/dev/null | tr '\0' '\255' \
        >> "$WORK.env"
    dd if="$WORK.env" of="$WORK" bs=1 seek="$ENVOFF" conv=notrunc \
        status=none
    rm -f "$WORK.env"
}

LOG="$DISK.log.m1"
smash_env
boot_watch "rach2 login:"
if grep -aq "Booting... IKMARKB" "$LOG"; then
    fail "a destroyed environment header still booted the candidate"
fi

LOG="$DISK.log.m2"
craft_env "# GRUB Environment Block
# WARNING: Do not edit this file by tools other than grub-editenv!!!
saved_entry=fallback
"
boot_watch "rach2 login:"
if grep -aq "Booting... IKMARKB" "$LOG"; then
    fail "a complete old value still booted the candidate"
fi

LOG="$DISK.log.m3"
craft_env "# GRUB Environment Block
# WARNING: Do not edit this file by tools other than grub-editenv!!!
saved_entry=fallback
next_entry=candidate
"
boot_watch "Booting... IKMARKB"

echo "install-test: two passes installed, trialed, committed, recovered and re-committed a machine-built kernel on the booted disk, and the matrix states each booted the kernel the plan predicts: OK"
echo "install-test: PASS"
