#!/usr/bin/env sh

#
# uefi.sh: the machine booted by UEFI firmware, from its installed disk and
# from the recovery stick's image, and the installed disk's BIOS core
# replaced under either firmware.
#
# Every other suite boots through SeaBIOS or with the kernel given to the
# emulator. This one gives an x86-64 machine QEMU's UEFI firmware, OVMF, and
# nothing else: the firmware starts GRUB's EFI image from the disk's EFI
# system partition, or from the stick's EFI boot image, and the image opens
# the menu the BIOS core opens. Four boots by OVMF, a fifth part that boots
# the GPT disk by SeaBIOS as well, and an install by OVMF:
#
# 1. The installed disk, a GPT disk as the disk builder makes one and an
#    install writes it: the firmware boots it, the kernel finds its root by
#    the disk's GUID, and root logs in over SSH with the demonstration key;
#    `bootctl` on the running machine reads the EFI system partition's slots
#    and names the candidate as the booted one.
# 2. The same disk's recovery entry, armed for one boot on the host: the
#    recovery system comes up from partition 2 and answers SSH, and its
#    `bootctl` reads the boot partition by its node.
# 3. The install stick's image as a USB device: the firmware boots its EFI
#    boot image, an MBR partition of type 0xEF, and the recovery system
#    answers SSH.
# 4. The recovery ISO as a CD: the firmware boots its second El Torito
#    entry, and the recovery system answers SSH.
# 5. The installed disk's BIOS core replaced from the running machine. The
#    disk boots by SeaBIOS, whose path is the BIOS core in the BIOS boot
#    partition; `bootctl grub` writes GRUB's GPT core into the partition's
#    tail not in use, and the entry sector, the partition's first, is left
#    naming it. With the other tail zeroed on the host, the disk boots by
#    SeaBIOS again, so the new tail is the one that ran; a second
#    `bootctl grub` goes back to the first tail and is proved the same way;
#    and the disk still boots by OVMF, whose path does not use the core. The
#    table's sectors before the partition and the backup table at the disk's
#    end are the same bytes after both installs.
# 6. An install onto a 4 TiB NVMe disk, past the 2 TiB an MBR addresses. The
#    stick boots by OVMF beside the disk, `rach2-install` is answered over
#    SSH and grows the root into the whole disk, and the disk then boots by
#    OVMF alone, the first account logs in with the demonstration key the
#    install took as its authorized key, and `df` finds the root on the NVMe
#    disk and larger than 2 TiB. The disk is a sparse file, and QEMU turns the zeroed blocks
#    the grow writes, its new groups' inode tables, back into holes.
#
# In the first boot the kernel's console is on the firmware's framebuffer,
# and the screen QEMU dumps through its monitor, once the login prompt is
# on the serial line, must show `rach2 login:` too, as the kernel's font
# draws it: build/tests/system/screentext reads the console's cells off the
# dump against that font.
#
# The kernel is the one ARCH names, i686 unless it is set, and the machine is
# x86-64 QEMU for either.
#
# Usage: uefi.sh <disk> <install-stick> <recovery-iso> <work-dir> <ssh-port>
#            <demo-key>
#

REPO=$(cd "$(dirname "$0")/../../.." && pwd)
RUNUTIL="$REPO/build/tests/system/runutil"
SCREENTEXT="$REPO/build/tests/system/screentext"
# shellcheck source=tests/system/deadline.sh
. "$REPO/tests/system/deadline.sh"
# shellcheck source=tools/setup/ovmf_path.sh
. "$REPO/tools/setup/ovmf_path.sh"
set -e

DISK="$1"
STICK="$2"
ISO="$3"
DIR="$4"
PORT="$5"
DEMO_KEY="$6"
ARCH="${ARCH:-i686}"

if [ -z "$DEMO_KEY" ]; then
    echo "usage: uefi.sh <disk> <install-stick> <recovery-iso> <work-dir> <ssh-port> <demo-key>" >&2
    exit 2
fi
case "$ARCH" in
    i686 | x86_64) ;;
    *)
        echo "uefi-test: ARCH is i686 or x86_64, not $ARCH" >&2
        exit 2
        ;;
esac
if [ -z "$OVMF_ARGS" ]; then
    echo "uefi-test: no UEFI firmware for x86-64; tools/setup/doctor.sh says where to get it" >&2
    exit 1
fi
mkdir -p "$DIR"
KNOWN="$DIR/known_hosts"

fail=0
note() { echo "uefi-test: $1"; }
bad() {
    echo "uefi-test: FAIL: $1" >&2
    fail=1
}
note "the kernel is $ARCH's, the firmware $OVMF_PATH"

# A machine left running by an earlier run holds the images this one writes.
for left in $(pgrep -f "$DIR/" 2>/dev/null || true); do
    kill "$left" 2>/dev/null || true
    "$RUNUTIL" gone "$left" 30 || true
done

QPID=""
stop_machine() {
    [ -n "$QPID" ] || return 0
    kill "$QPID" 2>/dev/null || true
    "$RUNUTIL" gone "$QPID" 60 || true
    wait "$QPID" 2>/dev/null || true
    QPID=""
}
trap stop_machine EXIT
trap 'stop_machine; exit 130' INT
trap 'stop_machine; exit 143' TERM
trap 'stop_machine; exit 129' HUP

# boot_uefi LOG QEMU-ARG...: start the machine by UEFI, its serial line in
# LOG and its SSH port forwarded to PORT. The firmware's arguments are words
# with no space in them.
boot_uefi() {
    log=$1
    shift
    rm -f "$log" "$KNOWN"
    # shellcheck disable=SC2086 # the firmware's arguments are separate words
    qemu-system-x86_64 -m 512 -cpu max -display none $OVMF_ARGS \
        -serial "file:$log" -no-reboot \
        -netdev "user,id=net0,hostfwd=tcp::$PORT-:22" -device rtl8139,netdev=net0 \
        "$@" > "$log.qemu" 2>&1 &
    QPID=$!
}

# ssh to the machine as root with the demonstration key and nothing of the
# runner's own, once, with standard input from FILE.
key_ssh_input() { # key_ssh_input FILE COMMAND
    ssh -o StrictHostKeyChecking=no -o "UserKnownHostsFile=$KNOWN" \
        -o "ConnectTimeout=$(scaled 20)" -o BatchMode=yes -o IdentitiesOnly=yes \
        -o PreferredAuthentications=publickey -o PasswordAuthentication=no \
        -i "$DEMO_KEY" -p "$PORT" root@localhost "$2" < "$1"
}

# ssh to the machine as root with the demonstration key and nothing of the
# runner's own, tried until the server answers or the time is up.
key_ssh() { # key_ssh COMMAND
    key_ssh_as root "$1"
}

# The same as USER.
key_ssh_as() { # key_ssh_as USER COMMAND
    tries=0
    while :; do
        if out=$(ssh -o StrictHostKeyChecking=no -o "UserKnownHostsFile=$KNOWN" \
            -o "ConnectTimeout=$(scaled 20)" -o BatchMode=yes -o IdentitiesOnly=yes \
            -o PreferredAuthentications=publickey -o PasswordAuthentication=no \
            -i "$DEMO_KEY" -p "$PORT" "$1@localhost" "$2" 2>&1); then
            printf '%s\n' "$out"
            return 0
        fi
        tries=$((tries + 1))
        [ "$tries" -lt 10 ] || { printf '%s\n' "$out"; return 1; }
        sleep 3 # interval: the server starts after the login prompt
    done
}

# boot_bios LOG QEMU-ARG...: start the machine by SeaBIOS, QEMU's own
# firmware, which boots the disk through the BIOS core.
boot_bios() {
    log=$1
    shift
    rm -f "$log" "$KNOWN"
    qemu-system-x86_64 -m 512 -cpu max -display none \
        -serial "file:$log" -no-reboot \
        -netdev "user,id=net0,hostfwd=tcp::$PORT-:22" -device rtl8139,netdev=net0 \
        "$@" > "$log.qemu" 2>&1 &
    QPID=$!
}

# The firmware's own record of what it booted, on the serial line.
booted_by_uefi() { # booted_by_uefi LOG WHAT
    grep -aq "BdsDxe: starting Boot.*$2" "$1"
}

# Ask the machine's monitor, on the pipes $DIR/monitor.in and .out, for a dump
# of its screen into $1, and wait for the whole file: the PPM header and three
# bytes a pixel.
screen_dump() {
    rm -f "$1"
    printf 'screendump %s\n' "$1" > "$DIR/monitor.in"
    tries=0
    while :; do
        if [ -s "$1" ]; then
            size=$(wc -c < "$1" | tr -d ' ')
            sleep 1 # interval: the dump is written whole by then, or still growing
            [ "$(wc -c < "$1" | tr -d ' ')" = "$size" ] && return 0
        fi
        tries=$((tries + 1))
        [ "$tries" -lt "$(scaled 30)" ] || return 1
        sleep 1 # interval: the monitor takes the command between the guest's work
    done
}

# ---- 1: the installed disk ----
cp "$DISK" "$DIR/disk.img"
LOG1="$DIR/disk.log"
rm -f "$DIR/monitor.in" "$DIR/monitor.out"
mkfifo "$DIR/monitor.in" "$DIR/monitor.out"
boot_uefi "$LOG1" -monitor "pipe:$DIR/monitor" \
    -drive "file=$DIR/disk.img,format=raw,if=ide,index=0,media=disk"
cat "$DIR/monitor.out" > /dev/null &
MONITOR_READER=$!
if "$RUNUTIL" until "$LOG1" "rach2 login:" 240; then
    if screen_dump "$DIR/screen.ppm" && "$SCREENTEXT" "$DIR/screen.ppm" > "$DIR/screen.txt" &&
        grep -q '^rach2 login:' "$DIR/screen.txt"; then
        note "and the framebuffer console shows the login prompt in the kernel's font: OK"
    else
        bad "the screen did not show 'rach2 login:' in the kernel's font; see $DIR/screen.txt"
    fi
    if booted_by_uefi "$LOG1" "HARDDISK"; then
        note "the firmware boots the installed disk from its EFI system partition: OK"
    else
        bad "the firmware did not say it booted the disk"
    fi
    if grep -aq "root: GPTID=[0-9a-f-]*,3: /dev/disk0p3, ext2" "$LOG1"; then
        note "and the kernel finds its root by the disk's GUID: OK"
    else
        bad "the kernel did not name its root by GPTID"
    fi
    OUT=$(key_ssh 'id -u; bootctl status; echo UEFI""LOGIN' || true)
    case "$OUT" in
        *UEFILOGIN*) note "and root logs in over SSH: OK" ;;
        *) bad "root's SSH login to the disk booted by UEFI: $OUT" ;;
    esac
    case "$OUT" in
        *"bootctl: booted: candidate"*) note "and bootctl reads the slots and names the candidate: OK" ;;
        *) bad "bootctl on the disk booted by UEFI: $OUT" ;;
    esac
else
    bad "the installed disk never reached a login by UEFI; see $LOG1"
fi
stop_machine
kill "$MONITOR_READER" 2>/dev/null || true
wait "$MONITOR_READER" 2>/dev/null || true
rm -f "$DIR/monitor.in" "$DIR/monitor.out"

# ---- 2: the same disk's recovery entry ----
# Armed the way grub-editenv arms it: next_entry set and every other line of
# the block kept.
ENV_AT=$(grep -abo '# GRUB Environment Block' "$DIR/disk.img" | head -1 | cut -d: -f1)
if [ -n "$ENV_AT" ]; then
    {
        printf '# GRUB Environment Block\n'
        printf '# WARNING: Do not edit this file by tools other than grub-editenv!!!\n'
        dd if="$DIR/disk.img" bs=1 skip="$ENV_AT" count=1024 2>/dev/null |
            sed 's/#*$//' | grep -a -v -e '^#' -e '^$' -e '^next_entry='
        printf 'next_entry=recovery\n'
    } > "$DIR/env"
    n=$(wc -c < "$DIR/env" | tr -d ' ')
    dd if=/dev/zero bs=1 count=$((1024 - n)) 2>/dev/null | tr '\0' '#' >> "$DIR/env"
    dd if="$DIR/env" of="$DIR/disk.img" bs=1 seek="$ENV_AT" conv=notrunc 2>/dev/null
fi
LOG2="$DIR/recovery.log"
boot_uefi "$LOG2" -drive "file=$DIR/disk.img,format=raw,if=ide,index=0,media=disk"
if "$RUNUTIL" until "$LOG2" "SSH is on" 240; then
    OUT=$(key_ssh 'grep -o rach2.slot=recovery /proc/cmdline; bootctl --target /dev/disk0p1 status; echo RECOVERY""LOGIN' || true)
    case "$OUT" in
        *"rach2.slot=recovery"*RECOVERYLOGIN*) note "the disk's recovery entry boots by UEFI and answers SSH: OK" ;;
        *) bad "the recovery system of the disk booted by UEFI: $OUT" ;;
    esac
    case "$OUT" in
        *"bootctl: next_entry: empty"*) note "and the trial GRUB armed was consumed: OK" ;;
        *) bad "the recovery entry's trial was not consumed: $OUT" ;;
    esac
else
    bad "the disk's recovery entry never came up by UEFI; see $LOG2"
fi
stop_machine

# ---- 3: the install stick's image as a USB device ----
cp "$STICK" "$DIR/stick.img"
LOG3="$DIR/stick.log"
boot_uefi "$LOG3" -device qemu-xhci,id=xhci \
    -drive "if=none,id=stick,format=raw,file=$DIR/stick.img" \
    -device usb-storage,bus=xhci.0,drive=stick,bootindex=0
if "$RUNUTIL" until "$LOG3" "SSH is on" 240; then
    if booted_by_uefi "$LOG3" "USB HARDDRIVE"; then
        note "the firmware boots the stick from its EFI boot image: OK"
    else
        bad "the firmware did not say it booted the stick"
    fi
    OUT=$(key_ssh 'grep -c "^part " /proc/disk; echo STICK""LOGIN' || true)
    case "$OUT" in
        *STICKLOGIN*) note "and the stick's recovery system answers SSH: OK" ;;
        *) bad "the stick's recovery system over SSH: $OUT" ;;
    esac
else
    bad "the stick never came up by UEFI; see $LOG3"
fi
stop_machine

# ---- 4: the recovery ISO as a CD ----
cp "$ISO" "$DIR/recovery.iso"
LOG4="$DIR/cd.log"
boot_uefi "$LOG4" -cdrom "$DIR/recovery.iso"
if "$RUNUTIL" until "$LOG4" "SSH is on" 240; then
    if booted_by_uefi "$LOG4" "DVD-ROM"; then
        note "the firmware boots the ISO from its EFI El Torito entry: OK"
    else
        bad "the firmware did not say it booted the CD"
    fi
    OUT=$(key_ssh 'echo CD""LOGIN' || true)
    case "$OUT" in
        *CDLOGIN*) note "and the recovery system answers SSH: OK" ;;
        *) bad "the CD's recovery system over SSH: $OUT" ;;
    esac
else
    bad "the ISO never came up by UEFI; see $LOG4"
fi
stop_machine

# ---- 5: the BIOS core replaced, booted by BIOS and then by UEFI ----
# The window is the BIOS boot partition as the table gives it, and its tails
# are what `grubslot.h` lays out: T = min((N - 1) / 2, 1023) sectors at W + 1
# and W + 1 + T for a partition of N sectors from LBA W.
GDISK="$DIR/grub.img"
CORE="$DIR/gpt-core.img"
cp "$DISK" "$GDISK"
cp "$REPO/vendor/grub/gpt-core.img" "$CORE.new"
if ! sh "$REPO/vendor/grub/verify_grub.sh" "gpt-core.img=$CORE.new" > /dev/null; then
    rm -f "$CORE.new"
    bad "the pinned GPT core did not verify"
    echo "uefi-test: result: FAIL" >&2
    exit 1
fi
mv "$CORE.new" "$CORE"
BB=$("$REPO/build/tools/image/mkgpt" --list "$GDISK" | awk '$2 == "bios" { print $3 / 512, $4 / 512 }')
BB_FIRST=${BB% *}
BB_SECTORS=${BB#* }
TAIL=$(((BB_SECTORS - 1) / 2))
[ "$TAIL" -le 1023 ] || TAIL=1023
TAIL_A=$((BB_FIRST + 1))
TAIL_B=$((TAIL_A + TAIL))
DISK_SECTORS=$(($(wc -c < "$GDISK") / 512))
note "the BIOS boot partition is LBA $BB_FIRST, $BB_SECTORS sectors: tails at $TAIL_A and $TAIL_B"

# The LBA the entry sector's blocklist names, as GRUB's boot code reads it.
blocklist() {
    dd if="$GDISK" bs=512 skip="$BB_FIRST" count=1 2>/dev/null | od -A n -t u4 -j 500 -N 4 |
        tr -d ' '
}
# The table's own sectors: the protective MBR, the primary header and array,
# and the backup array and header at the end.
table_sectors() {
    dd if="$GDISK" bs=512 count="$BB_FIRST" 2>/dev/null
    dd if="$GDISK" bs=512 skip=$((DISK_SECTORS - 33)) 2>/dev/null
}
table_sectors > "$DIR/table.before"
[ "$(blocklist)" = "$TAIL_A" ] || bad "the disk was not built with its core in tail A"

# swap_core LOG WANT OTHER: on the machine booted into LOG, install the core,
# which must land in the tail at WANT; stop the machine, check the selector and
# the table's sectors on the host, zero the tail at OTHER, and boot by SeaBIOS
# into LOG.next, which only the tail at WANT can bring up. Returns 1 when the
# machine is not up on LOG.next afterwards. Its names are its own, since
# boot_bios sets `log`.
swap_core() {
    was=$1
    want=$2
    other=$3
    next="$was.next"
    grep -aq "devfs: /dev/grubcore: BIOS boot partition 4, $BB_SECTORS sectors at LBA $BB_FIRST" \
        "$was" || bad "the kernel did not publish the BIOS boot partition as /dev/grubcore"
    if ! key_ssh 'echo up' > /dev/null; then
        bad "the machine booted by SeaBIOS did not answer SSH"
        return 1
    fi
    OUT=$(key_ssh_input "$CORE" \
        'cat > /tmp/core.img && bootctl grub /tmp/core.img && sync && echo GRUB""DONE' \
        2>&1 || true)
    case "$OUT" in
        *"in the tail at LBA $want; the entry sector at LBA $BB_FIRST now points there"*GRUBDONE*)
            note "bootctl grub writes the tail at LBA $want and selects it: OK" ;;
        *) bad "bootctl grub on the GPT disk: $OUT" ;;
    esac
    stop_machine
    [ "$(blocklist)" = "$want" ] || bad "the entry sector names LBA $(blocklist), not $want"
    table_sectors | cmp -s - "$DIR/table.before" ||
        bad "the install changed a sector of the GUID table"
    dd if=/dev/zero of="$GDISK" bs=512 seek="$other" count="$TAIL" conv=notrunc 2>/dev/null
    boot_bios "$next" -drive "file=$GDISK,format=raw,if=ide,index=0,media=disk"
    if ! "$RUNUTIL" until "$next" "rach2 login:" 240; then
        bad "the disk did not boot by SeaBIOS through the tail at LBA $want; see $next"
        return 1
    fi
    note "with the tail at LBA $other zeroed, the disk boots by SeaBIOS through LBA $want: OK"
}

LOG5="$DIR/bios.log"
boot_bios "$LOG5" -drive "file=$GDISK,format=raw,if=ide,index=0,media=disk"
if "$RUNUTIL" until "$LOG5" "rach2 login:" 240; then
    if grep -aq "BdsDxe" "$LOG5"; then
        bad "the BIOS boot was made by UEFI firmware"
    fi
    if swap_core "$LOG5" "$TAIL_B" "$TAIL_A"; then
        swap_core "$LOG5.next" "$TAIL_A" "$TAIL_B" || true
    fi
    stop_machine
    LOG6="$DIR/after.log"
    boot_uefi "$LOG6" -drive "file=$GDISK,format=raw,if=ide,index=0,media=disk"
    if "$RUNUTIL" until "$LOG6" "rach2 login:" 240 && booted_by_uefi "$LOG6" "HARDDISK"; then
        note "and after both installs the disk still boots by UEFI: OK"
    else
        bad "the disk did not boot by UEFI after the core was replaced; see $LOG6"
    fi
    stop_machine
else
    bad "the installed disk never reached a login by SeaBIOS; see $LOG5"
fi
stop_machine

# ---- 6: an install onto a sparse 4 TiB NVMe disk ----
BIG="$DIR/big.img"
BIG_DRIVE="file=$BIG,if=none,id=big,format=raw,discard=unmap,detect-zeroes=unmap"
rm -f "$BIG"
dd if=/dev/zero of="$BIG" bs=1048576 seek=4194304 count=0 2>/dev/null
cp "$STICK" "$DIR/big-stick.img"
# The answers, in the order rach2-install asks: the disk, root's password
# twice, the first account and its password twice, the demonstration key the
# recovery system was reached with as that account's authorized key, no key
# for the recovery system, the grow it offers, and the disk's name again.
cat > "$DIR/big-install.sh" <<'GUEST'
n=$(awk '$1 == "disk" && $3 == "nvme" { print $2 }' /proc/disk)
[ -n "$n" ] || { echo "no NVMe disk"; exit 1; }
printf '%s\n' "/dev/disk$n" uefi-big uefi-big owner owner-big owner-big \
    /root/.ssh/authorized_keys "" "" "/dev/disk$n" | rach2-install
echo "INSTALL STATUS $?"
GUEST
LOG7="$DIR/big-install.log"
boot_uefi "$LOG7" -device qemu-xhci,id=xhci \
    -drive "if=none,id=stick,format=raw,file=$DIR/big-stick.img" \
    -device usb-storage,bus=xhci.0,drive=stick,bootindex=0 \
    -drive "$BIG_DRIVE" -device nvme,drive=big,serial=rach2big
if "$RUNUTIL" until "$LOG7" "SSH is on" 240 && key_ssh 'echo up' > /dev/null; then
    key_ssh_input "$DIR/big-install.sh" 'sh -s' > "$DIR/big-install.out" 2>&1 &
    INSTALLER=$!
    # The grow writes for minutes; its last line is printed each minute, so
    # the run sees the suite is alive.
    waited=0
    while ! "$RUNUTIL" gone "$INSTALLER" 60; do
        waited=$((waited + 60))
        note "installing for ${waited}s: $(tail -n 1 "$DIR/big-install.out")"
        if [ "$waited" -ge 1800 ]; then
            kill "$INSTALLER" 2>/dev/null || true
            break
        fi
    done
    wait "$INSTALLER" 2>/dev/null || true
    if grep -q "growing the root into the rest of the disk" "$DIR/big-install.out" &&
        grep -q "^INSTALL STATUS 0$" "$DIR/big-install.out"; then
        note "the stick installs onto the 4 TiB NVMe disk and grows its root: OK"
    else
        bad "the install onto the 4 TiB disk; see $DIR/big-install.out"
    fi
else
    bad "the stick beside the 4 TiB disk never answered SSH; see $LOG7"
fi
stop_machine
LOG8="$DIR/big.log"
boot_uefi "$LOG8" -drive "$BIG_DRIVE" -device nvme,drive=big,serial=rach2big
if "$RUNUTIL" until "$LOG8" "rach2 login:" 240; then
    if booted_by_uefi "$LOG8" "NVMe"; then
        note "the firmware boots the NVMe disk from its EFI system partition: OK"
    else
        bad "the firmware did not say it booted the NVMe disk"
    fi
    OUT=$(key_ssh_as owner 'df -k / | tail -n 1; echo BIG""LOGIN' || true)
    if printf '%s\n' "$OUT" |
        awk '$1 ~ /^\/dev\/disk[0-9]+p3$/ && $2 > 2147483648 { found = 1 } END { exit !found }'; then
        note "and the first account finds the root on the NVMe disk, larger than 2 TiB: OK"
    else
        bad "the root of the 4 TiB disk: $OUT"
    fi
    note "the disk's file holds $(du -k "$BIG" | cut -f1) KiB on the host"
else
    bad "the 4 TiB disk never reached a login by UEFI; see $LOG8"
fi
stop_machine
rm -f "$BIG"

if [ "$fail" -ne 0 ]; then
    echo "uefi-test: result: FAIL" >&2
    exit 1
fi
echo "uefi-test: result: the installed disk, its recovery entry, the stick and the CD boot by UEFI; the disk's BIOS core is replaced twice and boots by either firmware; an install grows its root past 2 TiB on a 4 TiB NVMe disk"
