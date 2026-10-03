#!/usr/bin/env sh

#
# uefi.sh: the machine booted by UEFI firmware, from its installed disk and
# from the recovery stick's image.
#
# Every other suite boots through SeaBIOS or with the kernel given to the
# emulator. This one gives an x86-64 machine QEMU's UEFI firmware, OVMF, and
# nothing else: the firmware starts GRUB's EFI image from the disk's EFI
# system partition, or from the stick's EFI boot image, and the image opens
# the menu the BIOS core opens. Four boots:
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
#
# The kernel is the one ARCH names, i686 unless it is set, and the machine is
# x86-64 QEMU for either. The row that reads one line of the framebuffer
# console off QEMU's screen dump against the font is the framebuffer
# console's, which comes with it.
#
# Usage: uefi.sh <disk> <install-stick> <recovery-iso> <work-dir> <ssh-port>
#            <demo-key>
#

REPO=$(cd "$(dirname "$0")/../../.." && pwd)
RUNUTIL="$REPO/build/tests/system/runutil"
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
# runner's own, tried until the server answers or the time is up.
key_ssh() { # key_ssh COMMAND
    tries=0
    while :; do
        if out=$(ssh -o StrictHostKeyChecking=no -o "UserKnownHostsFile=$KNOWN" \
            -o "ConnectTimeout=$(scaled 20)" -o BatchMode=yes -o IdentitiesOnly=yes \
            -o PreferredAuthentications=publickey -o PasswordAuthentication=no \
            -i "$DEMO_KEY" -p "$PORT" root@localhost "$1" 2>&1); then
            printf '%s\n' "$out"
            return 0
        fi
        tries=$((tries + 1))
        [ "$tries" -lt 10 ] || { printf '%s\n' "$out"; return 1; }
        sleep 3 # interval: the server starts after the login prompt
    done
}

# The firmware's own record of what it booted, on the serial line.
booted_by_uefi() { # booted_by_uefi LOG WHAT
    grep -aq "BdsDxe: starting Boot.*$2" "$1"
}

# ---- 1: the installed disk ----
cp "$DISK" "$DIR/disk.img"
LOG1="$DIR/disk.log"
boot_uefi "$LOG1" -drive "file=$DIR/disk.img,format=raw,if=ide,index=0,media=disk"
if "$RUNUTIL" until "$LOG1" "rach2 login:" 240; then
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

if [ "$fail" -ne 0 ]; then
    echo "uefi-test: result: FAIL" >&2
    exit 1
fi
echo "uefi-test: result: the installed disk, its recovery entry, the stick and the CD boot by UEFI"
