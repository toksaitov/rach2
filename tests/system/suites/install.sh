#!/usr/bin/env sh

#
# install.sh: the machine builds and installs its own kernel, and the trial,
# the commit, the fallback and the repair are checked on the booted disk.
#
# The disk is the GRUB one (mkdisk_grub.sh) and QEMU has no -kernel, so what
# boots is what GRUB reads from the boot partition, and a reboot after an
# install boots the installed kernel. The loop runs twice in one invocation,
# with a distinct banner marker per pass, so the second pass starts from the
# disk the first changed and shows that the flow can be repeated.
#
# One pass: log in as root and check rach2.slot=candidate; edit the pass's
# marker into the kernel's serial banner in /src/rach2 (the slot token alone
# is menu text, and a hash of the slot shows only the bytes on the disk, so
# the marker shows which kernel runs); build the kernel in place; install it;
# check that a commit is refused while the trial is armed; reboot -f into the
# trial and check the marker and the slot token; commit; cold-boot and check
# that the saved default held. Then the recovery half: corrupt the
# candidate's first sector through /dev/bootpart at the extent bootctl status
# names, reboot into the fallback (the marker must be absent from that boot's
# part of the log), reinstall from inside, reboot into the repaired
# candidate, and commit so the repair is the saved default.
#
# After the two passes, the interruption matrix's determinate states are
# booted from environment blocks the host writes: a destroyed header and a
# complete old value select the fallback, a complete new value the candidate,
# told apart by the marker with no login (the banner prints before one).
#
# The feeder paces on log patterns with occurrence counts, because a reboot
# brings a second "login:" and a plain grep would match the first. The log is
# checked after each session on the byte offsets of its tokens, each boot's
# claims only inside its own part of the log. The disk is assembled on the
# host with mtools and the pinned GRUB images, so the suite runs anywhere the
# other suites do.
#
# Usage: install.sh <grub-disk> <root password>
#

# The emulator of the architecture tests/system/suites.mk names.
QEMU=${RACH2_QEMU:-qemu-system-i386}

# The runner helper, whose `until` and `gone` are the suite's waits.
RUNUTIL="$(cd "$(dirname "$0")/../../.." && pwd)/build/tests/system/runutil"
set -e

REPO="$(cd "$(dirname "$0")/../../.." && pwd)"

DISK="$1"
ROOTPASSWORD="$2"

# Both entries boot the same root, so the kernel built under the candidate is
# on the fallback's root as well and the repair is done where it stands.
REPAIR_CD="cd /src/rach2"

if [ -z "$DISK" ] || [ -z "$ROOTPASSWORD" ]; then
    echo "usage: install_test.sh <grub-disk> <root password>" >&2
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

# cleanup_boot: stop the machine and its feeder and remove the FIFO; safe to
# call twice, since each pid is cleared before it is signalled.
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

# cleanup: stop the boot and remove the working disk and the logs.
cleanup() {
    cleanup_boot
    rm -f "$WORK" "$FEED" "$STEPS" "$DISK".log*
}

trap 'cleanup' EXIT
trap 'cleanup; exit 130' INT
trap 'cleanup; exit 143' TERM
trap 'cleanup; exit 129' HUP

# fail REASON: keep the failing boot's log as $DISK.fail.log, stop the boot,
# print REASON and exit 1.
fail() {
    if [ -n "$LOG" ] && [ -f "$LOG" ]; then
        cp "$LOG" "$DISK.fail.log" 2>/dev/null || true
        echo "install-test: the failing boot's console log: $DISK.fail.log" >&2
    fi
    cleanup_boot
    echo "install-test: FAIL: $1" >&2
    exit 1
}

# wait_count PATTERN COUNT SECONDS: wait up to SECONDS until the log has at
# least COUNT occurrences of PATTERN; return 1 if it does not. The wait is cut
# to what is left of the run's deadline, and passing that deadline fails.
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

# boot_feed: boot $WORK, logged to $LOG, and feed it the lines of $STEPS in the
# background. Login prompts and shell markers alike are patterns with counts,
# so a session that reboots in place keeps feeding correctly; the feeder stops
# at a pattern that does not come within ten minutes.
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

# boot_watch PATTERN: boot $WORK with no input, logged to $LOG, wait up to 75
# seconds for PATTERN, and stop the machine; fail if it does not come.
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

# The environment block's offset in the image, for the matrix states.
ENVOFF="$("$REPO/build/tools/inspect/binfind" --offset \
    '# GRUB Environment Block' "$WORK" || echo -1)"
if [ "$ENVOFF" -lt 0 ]; then
    fail "the disk has no environment block"
fi

# token_at LOG TOKEN: print the byte offset of TOKEN's first occurrence in
# LOG, or nothing when it is absent. The console writes the tokens as they
# happen, so comparing these offsets checks their order without a parser.
token_at() {
    grep -abo -m1 -e "$2" "$1" 2>/dev/null | head -1 | cut -d: -f1
}

# token_between FROM TO TOKEN LOG: return 0 when TOKEN appears in LOG between
# the byte offsets FROM and TO.
token_between() {
    dd if="$4" bs=1 skip="$1" count=$(($2 - $1)) status=none 2>/dev/null |
        grep -aq -e "$3"
}

# run_pass P OLDMARK MARK TOKEN: pass P of the loop, in two sessions, with the
# banner's OLDMARK edited to MARK, whose TOKEN the installed kernel prints. The
# first session checks the slot, edits the marker, builds, installs, checks
# the refused commit, reboots into the trial, checks it, commits and powers
# off. The second cold-boots, corrupts the candidate, reboots into the
# fallback, reinstalls, reboots into the repair and commits it. Each session's
# log is checked after it ends; any fault fails the suite.
run_pass() {
    P="$1"
    OLDMARK="$2"
    MARK="$3"
    TOKEN="$4"

    : > "$STEPS"
    step "rach2 login:" 1 "root"
    step "Password:" 1 "$ROOTPASSWORD"
    step "Password:" 1 "busybox grep -o 'rach2.slot=[a-z]*' /proc/cmdline | busybox sed s/slot/slotA${P}/"
    step "rach2.slotA${P}=candidate" 1 "cd /src/rach2 && busybox sed -i 's/${OLDMARK}/${MARK}/' sys/arch/x86/boot.c && busybox grep -c '${MARK}' sys/arch/x86/boot.c | busybox sed s/^/SED${P}N/"
    step "SED${P}N1" 1 "make -f guest.mk > /tmp/ik.out 2>&1; echo BUILD${P}RC=\$?"
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
    wait_count "COMMIT${P}A" 1 1800 || fail "pass $P: the trial boot never committed"
    "$RUNUTIL" gone "$QPID" 60 || true
    cleanup_boot

    # On the byte offsets: the marker must appear between the reboot and the
    # second slot token (the trial boot's part of the log), and not before
    # the reboot, where the running kernel had no marker or the last pass's.
    # The echoed commands have the marker text and are not counted.
    ARMED_AT="$(token_at "$LOG" "ARMED${P}RC=1")"
    RB_AT="$(token_at "$LOG" "PRERB${P}A")"
    SLOT2_AT="$(token_at "$LOG" "rach2.slotB${P}=candidate")"
    LOGEND="$(wc -c < "$LOG" | tr -d ' ')"
    [ -n "$ARMED_AT" ] || fail "pass $P: the armed-trial commit was not refused, so the trial was never armed or the gate is gone"
    if [ -z "$RB_AT" ] || [ -z "$SLOT2_AT" ] ||
       [ "$SLOT2_AT" -lt "$RB_AT" ] || [ "$ARMED_AT" -gt "$RB_AT" ]; then
        fail "pass $P: the session tokens are missing or out of order"
    fi
    token_between "$RB_AT" "$SLOT2_AT" "${TOKEN}" "$LOG" || \
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
    step "rach2.slotD${P}=fallback" 1 "$REPAIR_CD && bootctl kernel kernel && echo REPAIR\"\"${P}OK"
    step "REPAIR${P}OK" 1 "echo PRERB${P}C; reboot -f"
    step "rach2 login:" 3 "root"
    step "Password:" 3 "$ROOTPASSWORD"
    step "Password:" 3 "busybox grep -o 'rach2.slot=[a-z]*' /proc/cmdline | busybox sed s/slot/slotE${P}/"
    step "rach2.slotE${P}=candidate" 1 "bootctl commit && echo COMMIT\"\"${P}B"
    step "COMMIT${P}B" 1 "poweroff -f"

    LOG="$DISK.log.p${P}b"
    boot_feed
    wait_count "COMMIT${P}B" 1 1800 || fail "pass $P: the repair was never committed"
    "$RUNUTIL" gone "$QPID" 60 || true
    cleanup_boot

    COLD_AT="$(token_at "$LOG" "${TOKEN}")"
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
    if token_between "$RB_B" "$SLOT_D" "${TOKEN}" "$LOG"; then
        fail "pass $P: the fallback boot has the marker"
    fi
    token_between "$RB_C" "$SLOT_E" "${TOKEN}" "$LOG" || \
        fail "pass $P: the repaired candidate did not print its marker"
    token_between "$SLOT_E" "$LOGEND" "bootctl: committed" "$LOG" || \
        fail "pass $P: the repair commit did not report success"
}

run_pass 1 "Rach2 Booting" "Rach2 Bt IKMA" "IKMA"
run_pass 2 "IKMA" "IKMB" "IKMB"

# The matrix's determinate states, crafted on the block and booted with no
# login: the marked candidate prints IKMB before any prompt, so presence
# tells the entries apart. A destroyed header and a complete old value select
# the fallback; a complete new value selects the candidate once.
# craft_env LINES: write the environment block in place at its offset: 1024
# bytes, LINES and a '#' fill, the way grub-editenv writes one.
craft_env() {
    printf '%s' "$1" > "$WORK.env"
    n="$(wc -c < "$WORK.env" | tr -d ' ')"
    dd if=/dev/zero bs=1 count=$((1024 - n)) 2>/dev/null | tr '\0' '#' \
        >> "$WORK.env"
    dd if="$WORK.env" of="$WORK" bs=1 seek="$ENVOFF" conv=notrunc \
        status=none
    rm -f "$WORK.env"
}

# smash_env: overwrite the environment block with 512 bytes of 0xDE and 512 of
# 0xAD, so nothing in it reads as a header or a variable.
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
if grep -aq "IKMB" "$LOG"; then
    fail "a destroyed environment header still booted the candidate"
fi

LOG="$DISK.log.m2"
craft_env "# GRUB Environment Block
# WARNING: Do not edit this file by tools other than grub-editenv!!!
saved_entry=fallback
"
boot_watch "rach2 login:"
if grep -aq "IKMB" "$LOG"; then
    fail "a complete old value still booted the candidate"
fi

LOG="$DISK.log.m3"
craft_env "# GRUB Environment Block
# WARNING: Do not edit this file by tools other than grub-editenv!!!
saved_entry=fallback
next_entry=candidate
"
boot_watch "IKMB"

echo "install-test: two passes installed, trialed, committed, recovered and re-committed a machine-built kernel on the booted disk, and the matrix states each booted the kernel the plan predicts: OK"
echo "install-test: PASS"
