#!/bin/sh
# Keep the recovery session and its work through save, grow and restore.
# Only disposable image files are opened by the host or guest.
# shellcheck disable=SC2016
set -eu

# The build tree and the emulator of the architecture
# tests/system/suites.mk names.
ARCH_BUILD=${ARCH_BUILD:-build}
QEMU=${RACH2_QEMU:-qemu-system-i386}

repo=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
cd "$repo"

# space_mutant FAULT SCRIPT: print SCRIPT, the machine's grow helper, with its
# work directory (ram-work) or its log (ram-log) moved to the RAM disk.
space_mutant()
{
    case $1 in
        ram-work) sed 's|mktemp -d "$store/machine-grow.XXXXXX"|mktemp -d /tmp/machine-grow.XXXXXX|' "$2" ;;
        ram-log) sed 's|tee -a "$work/$log_name"|tee -a "/tmp/$log_name"|' "$2" ;;
    esac
}

if [ "${1:-}" = --texts ]; then
    for fault in ram-work ram-log; do
        if space_mutant "$fault" tools/dev/machine_grow.sh | cmp -s - tools/dev/machine_grow.sh; then
            echo "grow-space: the $fault fault is not in tools/dev/machine_grow.sh" >&2
            exit 1
        fi
    done
    echo 'grow-space: texts: every fault is once in its source'
    exit 0
fi
if [ "$#" -le 1 ]; then
    mode=space
    [ "$#" = 0 ] || { [ "$1" = --mutate ] && mode=space-mutate; } \
        || { echo 'grow-space: usage: machine_grow_space.sh [--mutate | --texts]' >&2; exit 1; }
    # Image targets are deliberately not built here. tests/run grow supplies
    # them; a direct proof runs against the same prepared artifacts, the MBR
    # disk among them, whose boot partition the legacy grow saves as the
    # suite does.
    if [ -f krffs/recovery_authorized_keys ]; then tag=-with-key; else tag=; fi
    exec sh tests/system/grow_machine.sh build/tests/system/harness \
        "$ARCH_BUILD/images/rach2-recovery$tag.iso" "$ARCH_BUILD"/images/installer-test.disk \
        "$ARCH_BUILD"/images/installer-test-mbr.disk "$ARCH_BUILD"/images/grow "$mode"
fi
[ "$#" = 3 ] || exit 2
harness=$1
fixture=$2
mode=$3
fixture=$(CDPATH='' cd -- "$fixture" && pwd)
proof=$(mktemp -d "$fixture/space.XXXXXX")
echo "grow-space: logs: $proof"
echo "$$" > "$proof/driver.pid"
qpid=
DRIVE_FPID=
REPO=$repo
# shellcheck source=tests/system/drive.sh
. "$repo/tests/system/drive.sh"
stop()
{
    if [ -n "$qpid" ]; then kill "$qpid" 2>/dev/null || :; wait "$qpid" 2>/dev/null || :; qpid=; fi
    drive_stop
}
fail() { echo "grow-space: failed: $*; console: ${DRIVE_LOG:-none}" >&2; exit 1; }
trap stop EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM
mkexfat="$repo/build/tests/host/mkexfatfixture"
mkdir "$proof/scripts"
cp "$repo/tools/dev/machine_grow.sh" "$proof/scripts/fixed.sh"
space_mutant ram-work "$proof/scripts/fixed.sh" > "$proof/scripts/ram-work.sh"
space_mutant ram-log "$proof/scripts/fixed.sh" > "$proof/scripts/ram-log.sh"
# Fill the actual exFAT volume after the first restored range. The log ends
# on a cluster boundary, so its next completion must allocate more space.
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
tar -C "$proof/scripts" -cf "$proof/space-tools.tar" fixed.sh ram-work.sh ram-log.sh full-log.sh
# Add the proof scripts to a new store. No saved backup is modified.
"$mkexfat" --make "$proof/store.vol" --sectors 262144 --offset 2048 \
    --add "seed.ext2=$fixture/seed.ext2" --add "grow-tools.tar=$fixture/grow-tools.tar" \
    --add "space-tools.tar=$proof/space-tools.tar" > "$proof/store.make.log"
dd if=/dev/zero of="$proof/store.base" bs=1048576 count=1 2>"$proof/store-prefix.log"
cat "$proof/store.vol" >> "$proof/store.base"
"$repo/build/tools/image/mkmbr" --lba "$proof/store.base" 2048:262144:07 > "$proof/store-mbr.log"

command_ok()
{
    drive_type "{ $1; } && echo SPACE_\"\"STEP || echo SPACE_\"\"FAILED"
    command_start=$DRIVE_POS
    drive_expect 'SPACE_(STEP|FAILED)' 300 || fail "$2"
    if tail -c +"$((command_start + 1))" "$DRIVE_LOG" | tr -d '\r' | grep -q '^SPACE_FAILED$'; then
        fail "$2"
    fi
}
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
    # Allocate until the RAM file system itself refuses. This is distinct
    # from starving heap memory and does not change the shipped image.
    command_ok 'df -k / > /mnt/grow/root.before && awk "NR == 2 {exit !(\$2 <= 5120 && \$2 >= 4900)}" /mnt/grow/root.before' 'the 5 MiB root'
    command_ok 'dd if=/dev/zero of=/tmp/occupied bs=4096 2>/mnt/grow/fill.log; test "$?" != 0' 'filling RAM scratch'
    command_ok 'dd if=/dev/zero bs=512 2>/mnt/grow/fill-tail.log >> /tmp/occupied; test "$?" != 0' 'filling the last RAM fragments'
    command_ok 'i=0; while [ "$i" -lt 1024 ] && touch "/tmp/empty.$i" 2>/mnt/grow/fill-names.log; do i=$((i+1)); done; test "$i" -lt 1024' 'consuming the remaining name records'
    command_ok 'df -k / > /mnt/grow/root.full && awk "NR == 2 {exit !(\$4 == 0)}" /mnt/grow/root.full' 'RAM scratch really full'
}

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

check_target()
{
    qemu-img convert -f qcow2 -O raw "$proof/target.qcow2" "$proof/$phase.raw"
    cmp "$fixture/base.raw" "$proof/$phase.raw" || fail "$phase changed an original byte"
}
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

round_trip()
{
    start "$1"
    command_ok 'sh /tmp/grow-plan/fixed.sh save /dev/disk0 /mnt/grow /tmp/grow-plan > /mnt/grow/save.capture 2>&1' 'save with full RAM scratch'
    command_ok 'umount -n /mnt/grow' 'unmounting for host verification'
    host_backup
    # The helper saves command logs itself. Keeping an external file open on
    # the store would prevent its measurement unmount, so collect the short
    # console report in the guest shell until that mount has returned.
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
if [ "$mode" = space-mutate ]; then
    for mutation in ram-work ram-log; do
        start "$mutation"
        drive_type "sh /tmp/grow-plan/$mutation.sh save /dev/disk0 /mnt/grow /tmp/grow-plan > /mnt/grow/mutation.capture 2>&1; test \"\$?\" != 0 && echo SPACE_MUTANT_\"\"FAILED"
        drive_expect SPACE_MUTANT_FAILED 300 || fail "$mutation mutation passed"
        case "$mutation" in
            ram-work) diagnostic='creating work directory on' ;;
            ram-log) diagnostic='writing .*original-check.log' ;;
        esac
        grep -q "$diagnostic" "$DRIVE_LOG" || fail "$mutation wrong failure"
        command_ok 'umount -n /mnt/grow' 'unmounting after refusal'
        stop
        check_target
        echo "grow-space: mutation: $mutation detected before a target write"
        round_trip "$mutation-restored"
    done
fi
