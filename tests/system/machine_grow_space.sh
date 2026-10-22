#!/bin/sh
# machine_grow_space.sh, or HARNESS FIXTURE space: check that
# tools/dev/machine_grow.sh keeps its work and its logs on the store, so that
# save, grow and restore all run with the recovery system's RAM root full.
#
# Run with no operands, it runs grow_machine.sh on the images `tests/run grow`
# builds, which prepares the fixture and runs this script again with HARNESS,
# the fixture directory and `space`. Each round trip boots the recovery
# stick, fills the 5 MiB RAM root, saves, has the host check the backup,
# grows and restores; the RAM root must be as full after as before, and the
# target must have every original byte and boot. A save that fails must leave
# the target as it was, and the console is printed. The full-log round trip
# also fills the store while the restore log grows, which must fail the
# restore with a message, and the retry must finish. GROW_SPACE_SCRIPT, when
# set, names the helper the guest runs in place of the tree's, and
# GROW_SPACE_DIR the directory the fixture is made in in place of the
# suite's. The host and the guest open only disposable image files.
# shellcheck disable=SC2016
set -eu

repo=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
# The build tree and the emulator of the architecture tests/system/suites.mk
# names, or, run directly, of ARCH, which machine_grow_plan.sh also reads.
if [ -z "${ARCH_BUILD:-}" ]; then
    RACH2_ARCH=${RACH2_ARCH:-${ARCH:-i686}}
    REPO=$repo
    . "$repo/mk/arch.sh"
    RACH2_QEMU=${RACH2_QEMU:-$ARCH_QEMU}
    export RACH2_QEMU
fi
QEMU=${RACH2_QEMU:-qemu-system-i386}
helper=${GROW_SPACE_SCRIPT:-$repo/tools/dev/machine_grow.sh}
. "$repo/tests/system/runlock.sh"
runlock_hold "$repo" grow-space "$repo/tests/system/machine_grow_space.sh" "$@"
cd "$repo"

if [ "$#" = 0 ]; then
    # The images are not built here: `tests/run grow` builds them, and a
    # direct run uses the same ones, the MBR disk among them, whose boot
    # partition the grow saves as the suite's run does. The fixture's
    # directory is the suite's unless GROW_SPACE_DIR names one, and a tree
    # that has not run the suite lacks it.
    if [ -f krffs/recovery_authorized_keys ]; then tag=-with-key; else tag=; fi
    grow=${GROW_SPACE_DIR:-$ARCH_BUILD/images/grow}
    mkdir -p "$grow"
    exec sh tests/system/grow_machine.sh build/tests/system/harness \
        "$ARCH_BUILD/images/rach2-recovery$tag.iso" "$ARCH_BUILD"/images/installer-test.disk \
        "$ARCH_BUILD"/images/installer-test-mbr.disk "$grow" space
fi
[ "$#" = 3 ] && [ "$3" = space ] ||
    { echo 'grow-space: usage: machine_grow_space.sh [HARNESS FIXTURE space]' >&2; exit 2; }
harness=$1
fixture=$2
fixture=$(CDPATH='' cd -- "$fixture" && pwd)
proof=
qpid=
DRIVE_FPID=
REPO=$repo
# shellcheck source=tests/system/drive.sh
. "$repo/tests/system/drive.sh"
# Stop the machine and the feeder.
stop()
{
    if [ -n "$qpid" ]; then kill "$qpid" 2>/dev/null || :; wait "$qpid" 2>/dev/null || :; qpid=; fi
    drive_stop
}
# On exit: stop, then remove the scratch directory. A failure, but not a
# signal, keeps it, since the run printed its path.
cleanup()
{
    status=$?
    stop
    [ -n "$proof" ] || return 0
    case $status in
        0|129|130|143) rm -rf "$proof" ;;
    esac
}
# Report $* with the console's log and exit 1.
fail() { echo "grow-space: failed: $*; console: ${DRIVE_LOG:-none}" >&2; exit 1; }
trap cleanup EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM
proof=$(mktemp -d "$fixture/space.XXXXXX")
echo "grow-space: logs: $proof"
echo "$$" > "$proof/driver.pid"
mkexfat="$repo/build/tests/host/mkexfatfixture"
mkdir "$proof/scripts"
cp "$helper" "$proof/scripts/fixed.sh"
# full-log.sh fills the store's exFAT volume after the first restored range,
# with the log ending on a cluster boundary, so that the next completion
# record needs a new cluster.
awk '
    /run_logged restore.log restore_range/ {
        print "            if [ \"$row\" = 2 ]; then"
        print "                dd if=/dev/zero of=\"$work/restore.log\" bs=1048576 count=1 || fail fixture"
        print "                capture=$(wc -c < \"$store/failed.capture\")"
        print "                dd if=/dev/zero bs=1048576 count=\"$((1048576 - capture % 1048576))\" iflag=count_bytes 2>/dev/null || fail fixture"
        print "                rm -f \"$work/command.status\" \"$work/restore-read.status\" || fail fixture"
        print "                dd if=/dev/zero of=\"$store/full.fixture\" bs=1048576 2>/dev/null || :"
        print "                dd if=/dev/zero bs=512 2>/dev/null >>\"$store/full.fixture\" || :"
        print "                df -k \"$store\" | awk \"NR == 2 {exit !(\\\$4 == 0)}\" || fail fixture-space"
        print "            fi"
    }
    { print }
' "$proof/scripts/fixed.sh" > "$proof/scripts/full-log.sh"
for script in "$proof/scripts/"*.sh; do sh -n "$script"; done
tar -C "$proof/scripts" -cf "$proof/space-tools.tar" fixed.sh full-log.sh
# Add the proof scripts to a new store. No saved backup is modified.
"$mkexfat" --make "$proof/store.vol" --sectors 262144 --offset 2048 \
    --add "seed.ext2=$fixture/seed.ext2" --add "grow-tools.tar=$fixture/grow-tools.tar" \
    --add "space-tools.tar=$proof/space-tools.tar" > "$proof/store.make.log"
dd if=/dev/zero of="$proof/store.base" bs=1048576 count=1 2>"$proof/store-prefix.log"
cat "$proof/store.vol" >> "$proof/store.base"
"$repo/build/tools/image/mkmbr" --lba "$proof/store.base" 2048:262144:07 > "$proof/store-mbr.log"

# command_try COMMAND WHAT: type COMMAND at the guest's shell and succeed
# when it succeeds; WHAT names it when it does not return.
command_try()
{
    drive_type "{ $1; } && echo SPACE_\"\"STEP || echo SPACE_\"\"FAILED"
    command_start=$DRIVE_POS
    drive_expect 'SPACE_(STEP|FAILED)' 300 || fail "$2"
    ! tail -c +"$((command_start + 1))" "$DRIVE_LOG" | tr -d '\r' | grep -q '^SPACE_FAILED$'
}
# command_ok COMMAND WHAT: type COMMAND at the guest's shell and require it
# to succeed; WHAT names it in a failure.
command_ok()
{
    command_try "$1" "$2" || fail "$2"
}
# start PHASE: boot the recovery stick with a new overlay of the target and a
# copy of the store, unpack the tools, and fill the RAM root until the file
# system refuses a write, which leaves the heap alone and changes nothing in
# the image the stick boots.
start()
{
    phase=$1
    cp "$proof/store.base" "$proof/store.img"
    rm -f "$proof/target.qcow2"
    qemu-img create -q -f qcow2 -F raw -b "$fixture/base.raw" "$proof/target.qcow2"
    DRIVE_LOG="$proof/$phase.console"
    DRIVE_FIFO="$proof/$phase.input"
    mkfifo "$DRIVE_FIFO"
    "$QEMU" -m 512 -cpu max -display none -serial stdio -monitor none -no-reboot \
        -net none -device qemu-xhci,id=xhci \
        -drive "if=none,id=stick,format=raw,file=$fixture/stick.img" \
        -device usb-storage,bus=xhci.0,drive=stick,bootindex=0 \
        -drive "if=none,id=target,format=qcow2,file=$proof/target.qcow2" \
        -device ide-hd,drive=target,bus=ide.0,unit=0,serial=GROWTEST \
        -drive "file=$proof/store.img,format=raw,if=ide,index=1,media=disk" \
        < "$DRIVE_FIFO" > "$DRIVE_LOG" 2>&1 &
    qpid=$!
    echo "$qpid" > "$proof/qemu.pid"
    drive_feed "$qpid"
    drive_expect 'recovery image' 90 || fail 'recovery boot'
    drive_prompt 30 || fail 'recovery prompt'
    command_ok 'mkdir -p /mnt/grow /tmp/grow-plan && mount -n -t exfat /dev/disk1p1 /mnt/grow && tar -C /tmp/grow-plan -xf /mnt/grow/grow-tools.tar && tar -C /tmp/grow-plan -xf /mnt/grow/space-tools.tar' 'preparing the guest'
    command_ok 'df -k / > /mnt/grow/root.before && awk "NR == 2 {exit !(\$2 <= 5120 && \$2 >= 4900)}" /mnt/grow/root.before' 'the 5 MiB root'
    command_ok 'dd if=/dev/zero of=/tmp/occupied bs=4096 2>/mnt/grow/fill.log; test "$?" != 0' 'filling RAM scratch'
    command_ok 'dd if=/dev/zero bs=512 2>/mnt/grow/fill-tail.log >> /tmp/occupied; test "$?" != 0' 'filling the last RAM fragments'
    command_ok 'i=0; while [ "$i" -lt 1024 ] && touch "/tmp/empty.$i" 2>/mnt/grow/fill-names.log; do i=$((i+1)); done; test "$i" -lt 1024' 'consuming the remaining name records'
    command_ok 'df -k / > /mnt/grow/root.full && awk "NR == 2 {exit !(\$4 == 0)}" /mnt/grow/root.full' 'RAM scratch really full'
}

# Read the saved backup from the store on the host, check its SHA256SUMS,
# and write the host's acknowledgement, host.sha256, through the guest.
host_backup()
{
    mkdir "$proof/$phase-backup"
    for name in backup.bin boot.img grow.plan ranges.txt SHA256SUMS boot.status; do
        "$mkexfat" --check "$proof/store.img" 2048 --read "saved/$name" \
            > "$proof/$phase-backup/$name" || fail "host read saved/$name"
    done
    (cd "$proof/$phase-backup" && shasum -a 256 -c SHA256SUMS) || fail 'host backup hashes'
    ack="printf '%s\\n'"
    while read -r sum name; do ack="$ack '$sum  $name'"; done < "$proof/$phase-backup/SHA256SUMS"
    command_ok "mount -n -t exfat /dev/disk1p1 /mnt/grow && $ack > /mnt/grow/saved/host.sha256" 'host acknowledgement'
}

# Require the target to have every original byte.
check_target()
{
    qemu-img convert -f qcow2 -O raw "$proof/target.qcow2" "$proof/$phase.raw"
    cmp "$fixture/base.raw" "$proof/$phase.raw" || fail "$phase changed an original byte"
}
# Boot the restored target alone; recovery must be the saved entry and the
# root must have the owner file.
boot_target()
{
    "$harness" --bios --qemu-arg=-cpu --qemu-arg=max --qemu-arg=-m --qemu-arg=512 \
        --drive "file=$proof/target.qcow2,format=qcow2,if=ide,index=0,media=disk" \
        --timeout 90 --steptmo 60 --ok SPACE_DISK_BOOTS --log "$proof/$phase-boot.console" \
        'expect=recovery image' 'expect=# ' \
        'send=bootctl --target /dev/disk0p1 status' \
        'expect=bootctl: saved_entry: recovery' 'expect=bootctl: next_entry: absent' \
        'send=mkdir -p /mnt/root && mount -t ext2 -o ro /dev/disk0p3 /mnt/root && cat /mnt/root/owner.txt && umount /mnt/root && echo SPACE_DISK_""BOOTS' \
        'expect=preserve the installed root' 'expect=SPACE_DISK_BOOTS' || fail 'booting the restored target'
}

# round_trip PHASE: save, grow and restore with the RAM root full. The grow's
# report is kept in a shell variable until the grow has unmounted and
# mounted the store again, since a file held open on the store would keep
# the measurement from unmounting it.
round_trip()
{
    start "$1"
    if ! command_try 'sh /tmp/grow-plan/fixed.sh save /dev/disk0 /mnt/grow /tmp/grow-plan > /mnt/grow/save.capture 2>&1' 'save with full RAM scratch'; then
        tail -c +"$((command_start + 1))" "$DRIVE_LOG" | tr -d '\r' >&2
        command_ok 'umount -n /mnt/grow' 'unmounting after a failed save'
        stop
        check_target
        fail "$1: the save with full RAM scratch failed; the target is as it was"
    fi
    command_ok 'umount -n /mnt/grow' 'unmounting for host verification'
    host_backup
    command_ok 'report=$(sh /tmp/grow-plan/fixed.sh grow /dev/disk0 /mnt/grow /tmp/grow-plan 2>&1); rc=$?; printf "%s\n" "$report" > /mnt/grow/grow.capture && test "$rc" = 0' 'grow with accumulated work'
    if [ "$1" = full-log ]; then
        drive_type 'sh /tmp/grow-plan/full-log.sh restore /dev/disk0 /mnt/grow /tmp/grow-plan > /mnt/grow/failed.capture 2>&1; rc=$?; echo SPACE_LOG_""DONE_$rc'
        drive_expect 'SPACE_LOG_DONE_[0-9]+' 300 || fail 'log-write failure did not return'
        grep -q 'machine-grow: failed: writing .*restore.log' "$DRIVE_LOG" || fail 'log-write failure was silent'
        grep -q '^SPACE_LOG_DONE_[1-9]' "$DRIVE_LOG" || fail 'log-write failure status'
        command_ok 'rm /mnt/grow/full.fixture' 'releasing space for retry'
    fi
    command_ok 'sh /tmp/grow-plan/fixed.sh restore /dev/disk0 /mnt/grow /tmp/grow-plan > /mnt/grow/restore.capture 2>&1' 'restore with accumulated work'
    command_ok 'grep -q "every saved range matches and the restored root checks clean" /mnt/grow/restore.capture && df -k / > /mnt/grow/root.after && cmp /mnt/grow/root.full /mnt/grow/root.after' 'restoration without RAM allocation'
    command_ok 'test ! -e /tmp/machine-grow.* && test "$(ls -d /mnt/grow/machine-grow.* | wc -l)" -ge 3 && umount -n /mnt/grow' 'persistent work directories'
    stop
    check_target
    boot_target
    echo "grow-space: $phase: same 5 MiB root, accumulated work, guest logs, original bytes and boot passed"
}
round_trip fixed
round_trip full-log
