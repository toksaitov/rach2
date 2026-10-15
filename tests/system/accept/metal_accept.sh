#!/usr/bin/env sh

#
# metal_accept.sh: the scripted half of the acceptance of a real machine,
# run from the development host over SSH.
#
# The acceptance has two halves. One needs a person at the machine: a cable
# pulled and put back, a cold power cycle, a sound heard, a screen
# photographed. The other is every fact the machine can report about
# itself, which this script reads and records.
#
# It is run against the Lenovo once SSH answers, and the metal suite runs it
# against QEMU, so that the script is not debugged on the machine it
# measures. Nearly everything it does is a read, and it issues no command
# the read-only build's allowlist would refuse; it writes files only on the
# root and on the stick named by its serial.
#
# Its output is a record of notes and FAIL lines. Where a baseline exists
# (dev-log/24-metal/baseline.md, what Windows measured on the Lenovo before
# Rach2 was installed) it compares and says so; where none exists it
# reports the number so that the record has it. It prints
# `metal-accept: PASS` and exits 0, or `metal-accept: FAIL (N)` and exits 1;
# a usage error exits 2.
#
# The key is the demonstration one the RAM images carry, unless
# `RACH2_ACCEPT_KEY` names another, and the account is root unless
# `RACH2_ACCEPT_USER` names another. On an installed machine, root may have no
# key. RACH2_ACCEPT_SU=1 authenticates the account by its key
# and uses su for root commands. The owner sets RACH2_ACCEPT_PASSWORD in the
# environment; it is never put in a command argument or printed.
#
# Usage: metal_accept.sh <host> [port] [stick-serial]
#

set -e

HOST="$1"
PORT="${2:-22}"
# The stick this script may write, by its serial as `/proc/disk` reports it;
# with none given, no stick is written.
STICK_SERIAL="${3:-}"

if [ -z "$HOST" ]; then
    echo "usage: metal_accept.sh <host> [port] [stick-serial]" >&2
    exit 2
fi

REPO="$(cd "$(dirname "$0")/../../.." && pwd)"
KEY="${RACH2_ACCEPT_KEY:-$REPO/krffs/rach2_demo_ed25519}"
USER_NAME="${RACH2_ACCEPT_USER:-root}"
USE_SU="${RACH2_ACCEPT_SU:-0}"
if [ "${RACH2_ACCEPT_BOOTLOG+x}" = x ] || [ "${RACH2_ACCEPT_DD+x}" = x ]; then
    echo 'metal-accept: RACH2_ACCEPT_BOOTLOG and RACH2_ACCEPT_DD are no longer supported' >&2
    exit 2
fi
if [ "$USE_SU" = 1 ] && [ -z "${RACH2_ACCEPT_PASSWORD:-}" ]; then
    echo 'metal-accept: RACH2_ACCEPT_SU needs RACH2_ACCEPT_PASSWORD in the environment' >&2
    exit 2
fi
if [ ! -f "$KEY" ]; then
    echo "metal_accept.sh: $KEY is not a file: name the machine's own key in" \
         "RACH2_ACCEPT_KEY" >&2
    exit 2
fi
KNOWN="$REPO/build/images/metal-accept.known"
umask 077
LOCAL_TMP=$(mktemp -d "${TMPDIR:-/tmp}/rach2-accept.XXXXXX")

# The MBR's SHA-256 as Windows measured it, from dev-log/24-metal/baseline.md.
# It is kept here and not read from the document, so that this script is one
# file to copy to a development host, and it is stated in both places so that
# a change to one is visible against the other.
BASELINE_MBR_SHA256="652ad40fe72061185cf3a69c1a9a45394f68374f409153e082b7c1fa04c107d3"
BASELINE_SECTORS="1953525168"
BASELINE_MODEL="Samsung SSD 870 EVO 1TB"

# On exit, remove what the script left on the machine: the stick's file, its
# mount, the loop device, and the temporary directories, each mount released
# before its directory is removed. A failed unmount leaves its directory.
MACHINE_TMP=""
MACHINE_MOUNTED=""
MACHINE_LOOP=""
TRANSFER_TMP=""
STICK_FILE=""
cleanup() {
    if [ -n "$MACHINE_MOUNTED" ]; then
        if [ -n "$STICK_FILE" ]; then
            run "rm -f $STICK_FILE" > /dev/null 2>&1 || true
        fi
        if run "umount $MACHINE_MOUNTED && rmdir $MACHINE_MOUNTED" > /dev/null 2>&1; then
            MACHINE_MOUNTED=""
        fi
    fi
    if [ -n "$MACHINE_LOOP" ]; then
        run "loopctl detach $MACHINE_LOOP" > /dev/null 2>&1 || true
    fi
    if [ -n "$MACHINE_TMP" ] && [ -z "$MACHINE_MOUNTED" ]; then
        run "rm -rf $MACHINE_TMP" > /dev/null 2>&1 || true
    fi
    if [ -n "$TRANSFER_TMP" ]; then
        account "rm -rf $TRANSFER_TMP" > /dev/null 2>&1 || true
    fi
    rm -f "$KNOWN"
    rm -rf "$LOCAL_TMP"
    return 0
}
trap 'cleanup' EXIT
trap 'cleanup; exit 130' INT
trap 'cleanup; exit 143' TERM
trap 'cleanup; exit 129' HUP

fails=0
# Record $1 as a note, or as a failure that is counted.
note() {
    echo "  $1"
}
bad() {
    fails=$((fails + 1))
    echo "  FAIL: $1"
}

# Run $1 on the machine as the account, over SSH, with its status.
#
# A connection the machine refused, or that ended before the two sides had
# exchanged their identification, is tried again, three times a second
# apart: a machine slow to answer, under a loaded emulator in the
# rehearsal, says nothing about what the command would have printed. A
# reset after that point is not tried again, since the command may have run
# and read its input.
account() {
    tries=0
    while :; do
        if ssh -i "$KEY" -p "$PORT" \
            -o StrictHostKeyChecking=no -o UserKnownHostsFile="$KNOWN" \
            -o ConnectTimeout=10 -o BatchMode=yes \
            "$USER_NAME@$HOST" "$1" 2>"$LOCAL_TMP/ssh.err"; then
            status=0
        else
            status=$?
        fi
        if [ "$status" -ne 255 ] || [ "$tries" -ge 3 ] ||
            ! grep -q 'kex_exchange_identification\|ssh_exchange_identification\|Connection refused\|Connection timed out' "$LOCAL_TMP/ssh.err"; then
            return "$status"
        fi
        tries=$((tries + 1))
        # interval: a reset before the session is tried again after a second
        sleep 1
    done
}

# Run $1 on the machine as root: through su_command.sh when
# RACH2_ACCEPT_SU is 1, and otherwise as the account, which must be root.
try_run() {
    if [ "$USE_SU" = 1 ]; then
        if run_output=$(sh "$REPO/tests/system/accept/su_command.sh" \
            "$KEY" "$HOST" "$PORT" "$USER_NAME" "$KNOWN" "$1"); then
            printf '%s\n' "$run_output"
        else
            return "$?"
        fi
    else
        if run_output=$(account "$1"); then
            printf '%s\n' "$run_output" | tr -d '\r'
        else
            return "$?"
        fi
    fi
}

# Run $1 as root, a command the acceptance requires. A failure is recorded in
# a file, which keeps it across subshells and pipelines, so that the run
# fails even when a caller tests only the command's output.
run() {
    if try_run "$1"; then
        return 0
    else
        run_status=$?
        : > "$LOCAL_TMP/command-failed"
        echo "metal-accept: a required remote command failed (status $run_status)" >&2
        return "$run_status"
    fi
}

# Make a temporary directory on the machine for a transfer through su, in
# TRANSFER_TMP, and check that its name is a plain one.
transfer_directory() {
    TRANSFER_TMP=$(account 'mktemp -d /tmp/accept-transfer.XXXXXX') || return 1
    case "$TRANSFER_TMP" in
        /tmp/accept-transfer.*) ;;
        *) return 1 ;;
    esac
    case "$TRANSFER_TMP" in
        *[!A-Za-z0-9_./-]*) return 1 ;;
    esac
}

# put LOCAL REMOTE and get REMOTE LOCAL: copy a file to or from the machine.
#
# The machine has no sftp server, so a file goes over as the standard input of
# a command that writes it, and comes back as the standard output of one that
# reads it. Through su, the file passes through a directory the account owns
# and is moved as root. Both temporary directories are on /tmp's file system,
# so that the move takes no second copy of the image on a small RAM root,
# where KRFFS has no hard links.
put() {
    if [ "$USE_SU" != 1 ]; then
        account "cat > $2" < "$1"
        return
    fi
    transfer_directory || return 1
    transfer_status=0
    if ! account "cat > $TRANSFER_TMP/data" < "$1" ||
        ! run "mv $TRANSFER_TMP/data $2"; then
        transfer_status=1
    fi
    account "rm -rf $TRANSFER_TMP" || transfer_status=1
    TRANSFER_TMP=""
    return "$transfer_status"
}
get() {
    if [ "$USE_SU" != 1 ]; then
        account "cat $1" > "$2"
        return
    fi
    transfer_directory || return 1
    transfer_status=0
    if ! run "mv $1 $TRANSFER_TMP/data && chmod 644 $TRANSFER_TMP/data" ||
        ! account "cat $TRANSFER_TMP/data" > "$2"; then
        transfer_status=1
    fi
    run "if [ -f $TRANSFER_TMP/data ]; then mv $TRANSFER_TMP/data $1; fi" || transfer_status=1
    account "rm -rf $TRANSFER_TMP" || transfer_status=1
    TRANSFER_TMP=""
    return "$transfer_status"
}

# Write a file with a unique text to the stick's volume, $VOLNODE, and
# compare it with the text after an unmount and a mount. When
# RACH2_ACCEPT_FAT_IMAGE names the volume's image, as the metal suite's
# rehearsal does, mtools also reads the file from it while the volume is
# unmounted. The file is removed afterwards; a failure returns 1.
stick_witness() {
    printf 'fat acceptance %s\n' "${LOCAL_TMP##*/}" > "$LOCAL_TMP/stick.expected" || return 1
    MACHINE_TMP=$(run 'mktemp -d /tmp/accept.XXXXXX') || return 1
    case "$MACHINE_TMP" in
        /tmp/accept.*) ;;
        *) return 1 ;;
    esac
    case "$MACHINE_TMP" in
        *[!A-Za-z0-9_./-]*) return 1 ;;
    esac
    put "$LOCAL_TMP/stick.expected" "$MACHINE_TMP/expected" || return 1
    STICK_MOUNT="$MACHINE_TMP/mnt"
    MACHINE_MOUNTED="$STICK_MOUNT"
    run "mkdir $STICK_MOUNT && mount $VOLNODE $STICK_MOUNT" || return 1
    STICK_FILE=$(run "mktemp $STICK_MOUNT/accept.XXXXXX") || return 1
    case "$STICK_FILE" in
        "$STICK_MOUNT"/accept.*) ;;
        *) return 1 ;;
    esac
    case "${STICK_FILE#"$STICK_MOUNT"/}" in
        *[!A-Za-z0-9.]*) return 1 ;;
    esac
    run "cp $MACHINE_TMP/expected $STICK_FILE && cmp $MACHINE_TMP/expected $STICK_FILE" || return 1
    note "stick: cached bytes match"
    if ! run "umount $STICK_MOUNT"; then
        bad "stick: first unmount failed"
        return 1
    fi
    run "mount $VOLNODE $STICK_MOUNT" || return 1
    if ! run "cmp $MACHINE_TMP/expected $STICK_FILE"; then
        bad "stick: remount bytes differ"
        return 1
    fi
    run "umount $STICK_MOUNT" || return 1
    MACHINE_MOUNTED=""
    note "stick: remount bytes match after successful unmounts"
    if [ -n "${RACH2_ACCEPT_FAT_IMAGE:-}" ]; then
        if ! mcopy -i "$RACH2_ACCEPT_FAT_IMAGE" "::/${STICK_FILE##*/}" "$LOCAL_TMP/stick.host" ||
            ! cmp -s "$LOCAL_TMP/stick.expected" "$LOCAL_TMP/stick.host"; then
            bad "stick: host FAT bytes differ"
            return 1
        fi
        note "stick: host FAT bytes match while unmounted"
    else
        note "stick: no host FAT image supplied; no offline byte check"
    fi
    MACHINE_MOUNTED="$STICK_MOUNT"
    run "mount $VOLNODE $STICK_MOUNT" || return 1
    run "rm $STICK_FILE && umount $STICK_MOUNT" || return 1
    STICK_FILE=""
    MACHINE_MOUNTED=""
    run "rm -rf $MACHINE_TMP" || return 1
    MACHINE_TMP=""
}

echo "metal-accept: $HOST:$PORT"

# ---- that it is reachable at all, and as root ----
if [ "$(run 'echo ALIVE')" != "ALIVE" ]; then
    echo "metal-accept: FAIL: no SSH answer" >&2
    exit 1
fi
note "SSH answers"
if [ "$(run 'id -u')" != 0 ]; then
    echo 'metal-accept: root commands are required; use a root key or RACH2_ACCEPT_SU=1' >&2
    exit 1
fi
CMDLINE=$(run 'cat /proc/cmdline')
[ -n "$CMDLINE" ] || { echo 'metal-accept: the command line is empty' >&2; exit 1; }
MOUNTS=$(run 'cat /proc/mounts')
ROOT_TYPE=$(printf '%s\n' "$MOUNTS" | awk '$2 == "/" { print $3 }')
case "$ROOT_TYPE" in
    ext2|krffs) ;;
    *) echo 'metal-accept: the mount table has no supported root type' >&2; exit 1 ;;
esac
case " $CMDLINE " in
    *' rach2.rootfstype=ext2 '*|*' rach2.root=ext2 '*)
        [ "$ROOT_TYPE" = ext2 ] || {
            echo 'metal-accept: the requested ext2 root is not mounted' >&2
            exit 1
        }
        ;;
esac
INSTALLED=0
case " $CMDLINE " in
    *' rach2.fsdisk '*|*' rach2.root=disk '*|*' rach2.root=ext2 '*|\
    *' rach2.root=MBRID='*|*' rach2.root=GPTID='*|*' rach2.root=PARTUUID='*|\
    *' rach2.root=UUID='*|*' rach2.root=LABEL='*|*' rach2.root=/dev/disk'*)
        INSTALLED=1
        ;;
esac

# ---- what it says it is ----
note "uname: $(run 'uname -a')"
note "uptime ticks: $(run 'cat /proc/uptime')"
note "boot line: $CMDLINE"
note "identity: $(run 'id')"

# ---- the network: the address the boot line gives. That this session
#      exists is the evidence that the network works. ----
note "address given: $(printf '%s\n' "$CMDLINE" | tr ' ' '\n' | grep '^rach2.ip=' || echo none)"

# ---- entropy, which dropbear's host key depends on ----
if run 'dd if=/dev/urandom bs=16 count=1 2>/dev/null | wc -c' | grep -q '^ *16$'; then
    note "the random pool is trusted (16 bytes read from /dev/urandom)"
else
    bad "the random pool refused a read, so this CPU has no hardware entropy and dropbear could not have minted a host key"
fi

# ---- the disk, read and compared against the baseline ----
DISK="$(run 'cat /proc/disk 2>/dev/null')"
if [ -z "$DISK" ]; then
    bad "/proc/disk gave no disk report"
else
    policy="$(echo "$DISK" | awk '/^policy /{print $2}')"
    disk0="$(echo "$DISK" | awk '$1 == "disk" && $2 == 0')"
    identity0="$(echo "$DISK" | awk '$1 == "identity" && $2 == 0')"
    sectors="$(echo "$disk0" | awk '{print $5}')"
    model="$(echo "$disk0" | sed 's/^disk 0 [^ ]* [^ ]* [^ ]* [^ ]* //')"
    mounted="$(echo "$identity0" | awk '{print $5}')"

    note "disk: policy=$policy"
    # Two kernels answer here. The one a stick boots is built read-only,
    # because the disk under it may hold another system, and it mounts
    # nothing from that disk. The installed one, whose boot line names a root
    # on a disk (rach2.root=disk, rach2.root=ext2, or a partition by
    # identity, as the installer names it: rach2.root=MBRID=ID,N on an MBR
    # disk and rach2.root=GPTID=GUID,N on a GUID table), writes that root and
    # is built read-write; a read-only kernel there could not take a file.
    if [ "$INSTALLED" = 1 ]; then
        [ "$policy" = "read-write" ] || bad "the installed kernel is not the read-write build"
    else
        [ "$policy" = "read-only" ] || bad "the running kernel is not the read-only build"
    fi

    # The registry in /proc/disk says whether disk0 is mounted; the boot
    # diagnostic, which is optional, is not read.
    if [ -z "$disk0" ]; then
        if [ "$INSTALLED" = 1 ]; then
            bad "disk: the installed root has no disk0 in the registry"
        else
            note "disk: no disk0 in the registry"
        fi
    else
        note "disk: $disk0"
        note "disk: $identity0"
        case "$mounted" in
            mounted|free) ;;
            *) bad "disk: disk0 has no valid identity record" ;;
        esac
        if [ "$INSTALLED" = 1 ]; then
            [ "$mounted" = mounted ] || bad "disk: the installed root's disk is not mounted"
            note "disk: the installed system's root is on it"
        else
            [ "$mounted" = free ] || bad "the disk is mounted before the acceptance's mount rows"
        fi
    fi

    if [ -n "$disk0" ]; then
        mbr=""
        window="$(run 'if [ -b /dev/disk0 ]; then echo published; else echo absent; fi')"
        case "$window" in
            published)
                # A hash of a pipe can succeed after dd fails or reads nothing,
                # so the sector is kept in a file until its length is checked.
                # shellcheck disable=SC2016
                sector="$(run 'f=$(mktemp /dev/shm/accept-mbr.XXXXXX) || exit 1; trap '"'"'rm -f "$f"'"'"' EXIT; /bin/busybox dd if=/dev/disk0 of="$f" bs=512 count=1 >/dev/null 2>&1 && [ "$(wc -c < "$f")" = 512 ] && sha256sum "$f"')" || sector=""
                mbr="$(printf '%s\n' "$sector" | awk '{print $1}')"
                case "$mbr" in
                    ''|*[!0-9a-f]*) mbr="" ;;
                esac
                if [ "${#mbr}" -eq 64 ]; then
                    note "disk: the MBR hashes to $mbr"
                else
                    mbr=""
                    bad "disk: the published whole-disk window did not supply one complete sector"
                fi
                ;;
            absent)
                note "disk: /dev/disk0 is not published; no MBR read on this boot"
                ;;
            *) bad "disk: the whole-disk window check gave no answer" ;;
        esac
        echo "$DISK" | awk '$1 == "part" && $2 == 0' | while read -r line; do note "disk: $line"; done

        # The baseline describes one drive. Against that drive every number
        # must agree, and a disagreement is a failure. Against any other drive,
        # including the one QEMU provides in the rehearsal, there is nothing to
        # compare, and the note says so; a script that failed there could not
        # be rehearsed.
        if [ "$model" = "$BASELINE_MODEL" ]; then
            note "disk: this is the drive the baseline describes; comparing"
            [ "$sectors" = "$BASELINE_SECTORS" ] \
                || bad "the sector count is $sectors and the baseline says $BASELINE_SECTORS"
            if [ -z "$mbr" ]; then
                note "disk: no MBR comparison without a complete sector read"
            elif [ "$mbr" = "$BASELINE_MBR_SHA256" ]; then
                note "disk: the MBR is what Windows measured before Rach2 ran here"
            else
                bad "the MBR does not match the baseline: a wrong sector was read, or something has written it"
            fi
        else
            note "disk: not the drive the baseline describes, so nothing is compared"
        fi
    fi
fi

# Each invocation reads the running machine, including after rehearsal faults.
BOOTLOG_OUT="$REPO/build/images/metal-bootlog.txt"
if run 'cat /dev/bootlog' > "$BOOTLOG_OUT" 2>/dev/null && [ -s "$BOOTLOG_OUT" ]; then
    note "boot log: $(wc -l < "$BOOTLOG_OUT" | tr -d ' ') lines saved to $BOOTLOG_OUT"
else
    bad "/dev/bootlog gave nothing; this kernel does not provide it, or the log is empty"
fi
if [ -s "$BOOTLOG_OUT" ]; then
    if grep -q "refused command" "$BOOTLOG_OUT"; then
        bad "the boot log records a refused disk command; something strayed off the read path"
    fi
fi

# ---- the USB hardware, read out of the boot log saved above. The kernel
#      takes the inventory from configuration space at boot, so there is
#      nothing more to ask the machine for. On the Lenovo the routing line
#      matters: its firmware decides whether the USB2 pins belong to the xHCI
#      or to the companion EHCI, and a controller with no pins looks like a
#      machine with nothing plugged in. ----
if grep -q 'usb: controller:' "$BOOTLOG_OUT" 2>/dev/null; then
    for pattern in 'usb: controller:' 'usb: routing:' 'usb: ready:' 'usb: port:' \
                   'usb: device:' 'usb: absent:' 'usb: refused:'; do
        grep "$pattern" "$BOOTLOG_OUT" | sed 's/^.*usb: /usb: /' | while read -r line; do
            note "$line"
        done
    done
    # Whether the stack reached a device is said here rather than left to a
    # reader of the lines above. It is a note and not a failure: a machine
    # with nothing plugged into it is not broken, and the person who plugged
    # something in knows which it was.
    if grep -q 'usb: refused:' "$BOOTLOG_OUT"; then
        bad "the USB controller was refused; the line above says why"
    elif grep -q 'usb: device: port' "$BOOTLOG_OUT"; then
        note "usb: a device was enumerated on this machine"
    elif grep -q 'usb: ready:' "$BOOTLOG_OUT"; then
        note "usb: the controller came up and nothing was enumerated on it"
    elif grep -q 'usb: controller: none' "$BOOTLOG_OUT"; then
        note "usb: this machine has no USB controller"
    else
        bad "a USB controller was found and the stack did not come up on it"
    fi
else
    bad "the boot log has no usb inventory; this kernel does not provide it"
fi

# ---- which interrupts this machine takes, and by which path: whether the
#      local APIC came up, whether the USB and audio controllers took message
#      vectors or stayed on their legacy lines, and whether the timer ticks.
#      Almost every line is a note: a machine whose firmware leaves it on
#      legacy lines is not broken, and what it did is the record. The
#      failures are the timer's vector, 32, having taken nothing, a boot log
#      with no line about the local APIC, and the kernel's own
#      `lapic: timer:` line, which it logs when no tick arrives. ----
INTR="$(run 'cat /proc/interrupts 2>/dev/null')"
if [ -z "$INTR" ]; then
    bad "/proc/interrupts is not there; this kernel does not provide it"
else
    echo "$INTR" | while read -r line; do note "interrupts: $line"; done
    timer="$(echo "$INTR" | awk -F'\t' '$1 == "32:" { print $2 }')"
    # By the device's name and not by a number: a vector is given to a device
    # when it enables its messages, so the number is the next one free.
    usb_vector="$(echo "$INTR" | awk -F'\t' '$4 ~ /xhci/ { print $2 }')"
    audio_vector="$(echo "$INTR" | awk -F'\t' '$4 ~ /hda/ { print $2 }')"
    fallback="$(echo "$INTR" | awk -F'\t' '$1 == "FBK:" { print $2 }')"
    spurious="$(echo "$INTR" | awk -F'\t' '$1 == "SPU:" { print $2 }')"

    # A vector that has taken nothing has no line at all, so an empty answer
    # here is the failure and there is no zero to test for.
    case "$timer" in
        ''|*[!0-9]*) bad "the timer's vector has no line, so no timer interrupt has arrived" ;;
        *) note "interrupts: the timer has taken $timer, so its interrupts arrive" ;;
    esac
    note "interrupts: usb vector ${usb_vector:-none}, audio vector ${audio_vector:-none}"
    note "interrupts: spurious ${spurious:-none}, usb fallback ${fallback:-none}"
    # The fallback count is a note and not a failure. Under the emulator it
    # is zero, because the processor has latched a message by the time the
    # timer's pump looks. On hardware the event is in memory while the
    # message is still on its way, and a count that is small beside the
    # vector's own is that race and not a lost edge.
    if [ -n "$usb_vector" ] && [ "${fallback:-0}" != "0" ]; then
        note "interrupts: the timer's pump found the work of $fallback of $usb_vector messages"
    fi
    if grep -q 'lapic: refused:' "$BOOTLOG_OUT" 2>/dev/null; then
        note "interrupts: $(grep -m1 'lapic: refused:' "$BOOTLOG_OUT" | sed 's/^.*lapic: /lapic: /')"
    elif grep -q 'lapic: ready:' "$BOOTLOG_OUT" 2>/dev/null; then
        note "interrupts: $(grep -m1 'lapic: ready:' "$BOOTLOG_OUT" | sed 's/^.*lapic: /lapic: /')"
    else
        bad "the boot log says nothing about the local APIC; this kernel does not provide it"
    fi
    if grep -q 'lapic: timer:' "$BOOTLOG_OUT" 2>/dev/null; then
        bad "the timer stopped when the APIC was enabled: $(grep -m1 'lapic: timer:' "$BOOTLOG_OUT")"
    fi
fi

# ---- the boot log: the rows above read the one copy saved from it ----
note "boot log: inventory checks used the snapshot above"

# ---- the partitions as mounts: the partition nodes the table gives this
#      machine, the boot partition mounted read-only through the shell with
#      its files the sizes bootctl reports for its slots, and, when the boot
#      line has rach2.blocklag=N, the lag check through the shell: a read
#      through the lag device sleeps its reader while a loop in another
#      shell keeps counting, which is the scheduler's part of the block path
#      and what no host test can show. A machine with no slot report has no
#      installed disk and this is a note there; a machine whose bootctl
#      answers has the installed layout and must have its three
#      partitions. ----
NODES="$(run 'ls /dev 2>/dev/null' | grep '^disk0p[1-4]$' | tr '\n' ' ')"
if [ "$INSTALLED" = 1 ]; then
    SLOTS=$(run 'bootctl status 2>/dev/null')
else
    SLOTS=$(run 'bootctl status 2>/dev/null || :')
fi
if [ -z "$NODES" ]; then
    if [ -n "$SLOTS" ]; then
        bad "partitions: bootctl reports slots and there are no partition nodes"
    else
        note "partitions: no partition nodes (no disk, or an empty table)"
    fi
else
    note "partitions: $NODES"
    if [ -n "$SLOTS" ]; then
        for want in disk0p1 disk0p2 disk0p3; do
            case " $NODES " in
                *" $want "*) ;;
                *) bad "partitions: the sealed layout has $want and this machine does not" ;;
            esac
        done
    fi
    # A RAM boot may have a data stick at disk0. An installed boot requires
    # its boot files, and the cleanup in the remote command keeps a failure
    # to read them.
    # shellcheck disable=SC2016 # The remote shell preserves its read status.
    BOOT_COMMAND='mkdir -p /mnt/accept-boot && mount -r -t vfat /dev/disk0p1 /mnt/accept-boot && grep "^/dev/disk0p1 /mnt/accept-boot vfat ro" /proc/mounts && ls -l /mnt/accept-boot/boot /mnt/accept-boot/boot/grub; status=$?; umount /mnt/accept-boot || status=1; rmdir /mnt/accept-boot || status=1; exit "$status"'
    if [ "$INSTALLED" = 1 ] || [ -n "$SLOTS" ]; then
        OUT=$(run "$BOOT_COMMAND")
    else
        OUT=$(run "( $BOOT_COMMAND ) || :")
    fi
    if [ -z "$OUT" ]; then
        note "partitions: /dev/disk0p1 is not a FAT volume this kernel mounts"
    else
        case "$OUT" in
            "/dev/disk0p1 /mnt/accept-boot vfat ro"*) note "partitions: /dev/disk0p1 mounted read-only, as asked" ;;
            *) bad "partitions: /dev/disk0p1 mounted, but /proc/mounts does not say read-only" ;;
        esac
        # The volume must be the boot partition before its files are judged.
        # A machine with no hard disk has a stick as disk0, and its first
        # partition is a FAT volume that mounts and holds none of this.
        case "$OUT" in
            *" kernel"*) ;;
            *)
                if [ "$INSTALLED" = 1 ] || [ -n "$SLOTS" ]; then
                    bad "partitions: the boot partition has no kernel"
                else
                    note "partitions: /dev/disk0p1 is a FAT volume without a kernel, so not this machine's boot partition"
                fi
                OUT="" ;;
        esac
    fi
    if [ -n "$OUT" ]; then
        # The listing's sizes against the slot report: the same bytes read
        # through two paths, the FAT driver and bootslot's own walk.
        for name in kernel fallback boot.cfg grubenv; do
            size="$(echo "$OUT" | awk -v n="$name" '$NF == n { print $5 }')"
            case "$name" in
                kernel) key=candidate ;;
                *) key="$name" ;;
            esac
            said="$(echo "$SLOTS" | awk -F'[ ,]+' -v n="$key" '$2 == n":" { for (i = 1; i <= NF; i++) if ($(i+1) == "bytes") print $i }')"
            if [ -n "$size" ] && [ "$size" = "$said" ]; then
                note "partitions: /boot/$name is $size bytes on the mount and in the slot report"
            else
                bad "partitions: /boot/$name is [$size] bytes on the mount and [$said] in the slot report"
            fi
        done
    fi
    LAGN="$(printf '%s\n' "$CMDLINE" | tr ' ' '\n' | sed -n 's/^rach2.blocklag=//p' | head -1)"
    case "$LAGN" in
        ''|*[!0-9]*)
            note "partitions: the lag check needs rach2.blocklag=N on the boot line"
            ;;
        *)
            # 64 KiB in eight-sector requests is sixteen sleeps of N ticks
            # each, so the read takes at least 16N ticks by the machine's own
            # clock; a loop that counted nothing meanwhile means a reader that
            # never slept, and a read that failed is reported and not counted.
            # shellcheck disable=SC2016
            OUT="$(run 'rm -f /tmp/accept-lag-ok /tmp/accept-lag-bad; t0=$(cut -d" " -f1 /proc/uptime); (if dd if=/dev/disk0p1 of=/dev/null bs=65536 count=1 2>/dev/null; then touch /tmp/accept-lag-ok; else touch /tmp/accept-lag-bad; fi) & i=0; while [ ! -f /tmp/accept-lag-ok ] && [ ! -f /tmp/accept-lag-bad ]; do i=$((i + 1)); done; t1=$(cut -d" " -f1 /proc/uptime); if [ -f /tmp/accept-lag-ok ]; then echo "ok $i $t0 $t1"; else echo "bad $i $t0 $t1"; fi; rm -f /tmp/accept-lag-ok /tmp/accept-lag-bad')"
            # shellcheck disable=SC2086
            set -- $OUT
            if [ "$1" != "ok" ]; then
                bad "partitions: the read through the lag failed or gave no answer: $OUT"
            else
                count="$2"
                ticks="$(awk -v a="$3" -v b="$4" 'BEGIN { printf "%d", (b - a) * 100 + 0.5 }')"
                if [ "$count" = "0" ]; then
                    bad "partitions: the shell counted nothing while the read slept through the lag"
                elif [ "$ticks" -lt $((16 * LAGN)) ]; then
                    bad "partitions: the 64 KiB read took $ticks ticks, under the $((16 * LAGN)) the lag of $LAGN requires"
                else
                    note "partitions: the shell counted to $count while a 64 KiB read slept $ticks ticks through the lag of $LAGN"
                fi
            fi
            ;;
    esac
fi

# ---- the ext2 root, once the installer has made it: the mount table has it
#      writable on the third partition, the boot came from the candidate
#      slot, the boot log shows either no check or a clean one, and the
#      volume takes a write that survives a sync. A boot after a power cut
#      checks the root and records `ext2: check: clean`; a check that found
#      lost blocks, orphans or a fault is the failure. A krffs root in the
#      mount table skips this row. ----
if [ "$ROOT_TYPE" = ext2 ]; then
    OUT="$MOUNTS"
    case "$OUT" in
        *"/dev/disk0p3 / ext2 rw,relatime"*) note "the ext2 root is mounted writable from /dev/disk0p3" ;;
        *) bad "the ext2 root is not the mounted root: $OUT" ;;
    esac
    OUT="$(run 'bootctl status')"
    case "$OUT" in
        *"booted: candidate"*) note "the ext2 root boot came from the candidate slot" ;;
        *) bad "the ext2 root boot did not come from the candidate slot: $OUT" ;;
    esac
    if [ -s "$BOOTLOG_OUT" ] && grep -q "ext2: check" "$BOOTLOG_OUT"; then
        if ! grep 'ext2: check' "$BOOTLOG_OUT" | grep -v 'ext2: check: clean$' >/dev/null; then
            note "the ext2 root was checked at this boot and found clean; it was not unmounted the time before"
        else
            bad "the ext2 root's check found a fault: $(grep 'ext2: check' "$BOOTLOG_OUT" | head -1)"
        fi
    fi
    # The file is read back through the same cache; that the bytes reached
    # the disk is shown by the reboot after a write in the procedure. The
    # remote shell removes its file on its own exit, so a connection cut
    # after the mktemp leaves nothing behind.
    # shellcheck disable=SC2016 # the machine expands these, not this script
    OUT="$(run 'f=$(mktemp /root/accept.XXXXXX) && trap '"'"'rm -f "$f"'"'"' EXIT && echo accept > "$f" && sync && cat "$f"')"
    if [ "$OUT" = "accept" ]; then
        note "the ext2 root takes a file and gives it back"
    else
        bad "the ext2 root did not take a write: $OUT"
    fi
else
    note "the mount table identifies a krffs root; no ext2 root row"
fi

note "scratch: the image row uses a temporary file on the root"

# ---- a file system in a file: an image on the root, attached to a loop
#      device, mounted, written, read back, unmounted, detached, and judged
#      here by the host's own checker. The image is built here and taken away
#      again, so the machine is left as it was found. ----
FILESYSTEMS=$(run 'cat /proc/filesystems')
if [ -z "$FILESYSTEMS" ]; then
    bad "the machine gave no file-system list"
elif ! printf '%s\n' "$FILESYSTEMS" | grep -qw krffs; then
    note "this kernel does not take the krffs type; no image row"
elif [ ! -x "$REPO/build/krffs/mkfs.krffs" ]; then
    bad "the host KRFFS tools required for the image row are not built"
else
    IMG="$REPO/build/images/metal-loop.krffs"
    TREE="$REPO/build/tests/system/loopimg"
    mkdir -p "$TREE"
    printf 'hello from inside an image' > "$TREE/hello.txt"
    dd if=/dev/zero of="$TREE/inner.bin" bs=512 count=8 2>/dev/null
    if ! sh "$REPO/krffs/mkimage.sh" "$REPO/krffs/manifests/loopimg-test.manifest" \
        "$IMG" 1 >/dev/null 2>&1; then
        bad "the image could not be built here"
    elif ! MACHINE_TMP="$(run 'mktemp -d /tmp/accept.XXXXXX')" || [ -z "$MACHINE_TMP" ]; then
        bad "the machine gave no temporary directory for the image row"
    elif ! put "$IMG" "$MACHINE_TMP/loop.img" ||
        [ "$(run "wc -c < $MACHINE_TMP/loop.img")" != "1048576" ]; then
        bad "image: the transfer failed or did not produce 1048576 bytes"
        run "df -k /tmp; ls -ld $MACHINE_TMP; ls -l $MACHINE_TMP/loop.img" || true
        run "rm -rf $MACHINE_TMP"; MACHINE_TMP=""
    else
        MACHINE_LOOP=0
        OUT="$(run "loopctl attach 0 $MACHINE_TMP/loop.img && grep -c '^loop0 ' /proc/loop")"
        if [ "$OUT" = "1" ]; then
            note "an image on the root is attached to /dev/loop0"
        else
            bad "the image was not attached: $OUT"
        fi
        MACHINE_MOUNTED=/mnt/loop
        OUT="$(run 'mkdir -p /mnt/loop && mount -t krffs /dev/loop0 /mnt/loop && cat /mnt/loop/hello.txt')"
        if [ "$OUT" = "hello from inside an image" ]; then
            note "the file system in that file mounts and reads"
        else
            bad "the file system in that file did not read: $OUT"
        fi
        OUT="$(run 'echo written-on-the-machine > /mnt/loop/accept.txt && cat /mnt/loop/accept.txt')"
        if [ "$OUT" = "written-on-the-machine" ]; then
            note "and takes a write"
        else
            bad "the file system in that file did not take a write: $OUT"
        fi
        OUT="$(run 'umount /mnt/loop && rmdir /mnt/loop && loopctl detach 0 && wc -c < /proc/loop')"
        if [ "$OUT" = "0" ]; then
            note "and unmounts and detaches, leaving no device attached"
            MACHINE_MOUNTED=""; MACHINE_LOOP=""
        else
            bad "the unmount or the detach left something behind: $OUT"
        fi
        if ! get "$MACHINE_TMP/loop.img" "$IMG.after"; then
            bad "the image the machine wrote could not be downloaded"
        else
            if "$REPO/build/krffs/fsck.krffs" "$IMG.after" --strict >/dev/null 2>&1; then
                note "the host's checker finds the image the machine wrote clean"
            else
                bad "the host's checker finds the image the machine wrote faulty"
            fi
            if "$REPO/build/krffs/readfile.krffs" "$IMG.after" /accept.txt \
                "$IMG.accept" >/dev/null 2>&1 &&
                grep -q written-on-the-machine "$IMG.accept"; then
                note "with the file the machine wrote inside it"
            else
                bad "the file the machine wrote is not in the image"
            fi
        fi
        run "rm -rf $MACHINE_TMP"; MACHINE_TMP=""
    fi
fi

# ---- a flash drive, when one is plugged in: the kernel names it, reports
#      it as a block device, BusyBox finds its volume, and the volume takes a
#      file and gives it back after an unmount that flushed the stick's own
#      cache. A machine with nothing plugged in is a note, not a failure. The
#      pull from under a mount and the boot from a Rach2 stick need a person
#      at the machine and are in dev-log/40-usbdisk/hardware.md.
#      The stick is whichever disk slot holds one: the kernel reclaims a slot
#      only when its next stick is found, so the machine's first stick may be
#      disk1 for a while. A stick whose table has a Rach2 recovery ISO in it,
#      entry type 17, is a recovery stick and is refused whatever else is on
#      it, since the acceptance must not write over the stick that boots the
#      recovery system. A serial is required; the stick is the one that
#      reports it and no other, and two that report it stop the acceptance.
#      The volume is the first FAT or exFAT partition blkid finds on the
#      chosen stick, mounted by its node and not by a label another stick
#      could carry. ----
USBLINES="$(run 'cat /proc/usb 2>/dev/null' | grep '^disk[0-9]* ' || true)"
STICK=""
STICK_DUP=""
USBLINE=""
if [ -z "$USBLINES" ]; then
    note "no flash drive is plugged in (/proc/usb names no usb disk)"
else
    echo "$USBLINES" | while read -r line; do note "stick: $line"; done
    PARTS="$(run 'cat /proc/disk 2>/dev/null' || true)"
    for cand in $(echo "$USBLINES" | awk '{print $1}'); do
        n="${cand#disk}"
        serial="$(echo "$PARTS" | grep "^identity $n " | head -1 | cut -d' ' -f6-)"
        if echo "$PARTS" | grep -q "^part $n [0-9]* [0-9]* [0-9]* mbr:17\\( \\|$\\)"; then
            note "stick: /dev/$cand ($serial) carries a Rach2 recovery ISO in its table and is not written"
            continue
        fi
        if [ -z "$STICK_SERIAL" ]; then
            note "stick: /dev/$cand ($serial) was not named by the owner and is not written"
            continue
        fi
        if [ "$serial" != "$STICK_SERIAL" ]; then
            note "stick: /dev/$cand ($serial) is not the stick named, $STICK_SERIAL, and is not written"
            continue
        fi
        if [ -n "$STICK_DUP" ]; then
            continue
        fi
        if [ -n "$STICK" ]; then
            bad "stick: /dev/$cand and /dev/$STICK both report the serial $STICK_SERIAL; neither is written"
            STICK=""
            STICK_DUP=1
            continue
        fi
        STICK="$cand"
        USBLINE="$(echo "$USBLINES" | grep "^$cand " | head -1)"
        note "stick: /dev/$cand ($serial) is the one written"
    done
fi
if [ -z "$STICK" ] && [ -n "$STICK_SERIAL" ]; then
    bad "stick: no flash drive reports the serial $STICK_SERIAL that is not a recovery stick"
fi
if [ -n "$STICK" ]; then
    case "$(run "ls -l /dev/$STICK 2>/dev/null")" in
        b*) note "stick: /dev/$STICK is a block device" ;;
        *)  bad "stick: /dev/$STICK is not a block device, so a label scan cannot find it" ;;
    esac
    BLKID="$(run 'blkid 2>/dev/null' | grep -E "^/dev/$STICK(p[0-9]+)?:")"
    echo "$BLKID" | while read -r line; do note "stick: $line"; done
    if echo "$BLKID" | grep -q 'TYPE="iso9660"'; then
        note "stick: the ISO half of a Rach2 stick is on it and is left alone"
    fi
    VOLNODE="$(echo "$BLKID" | grep 'TYPE="\(vfat\|exfat\)"' | sed -n 's/^\(\/dev\/[a-z0-9]*\):.*/\1/p' | head -1 || true)"
    LABEL="$(echo "$BLKID" | grep "^$VOLNODE:" | sed -n 's/.*LABEL="\([^"]*\)".*/\1/p' | head -1 || true)"
    # The label is the stick's own text and goes into the record, so only a
    # label of letters, digits, `_` and `-` is used.
    case "$LABEL" in
        *[!A-Za-z0-9_-]*) bad "stick: the label has a character this script will not put in a record: $LABEL"; VOLNODE="" ;;
    esac
    if [ -z "$VOLNODE" ]; then
        bad "stick: no FAT or exFAT volume with a plain label was found on it"
    else
        before="$(echo "$USBLINE" | sed -n 's/.*flushes \([0-9]*\).*/\1/p')"
        if stick_witness; then
            note "stick: the volume $LABEL on $VOLNODE mounts, writes and reads after remount"
        else
            bad "stick: the volume $LABEL failed its persistence check"
            exit 1
        fi
        after="$(run 'cat /proc/usb 2>/dev/null' | grep "^$STICK " | sed -n 's/.*flushes \([0-9]*\).*/\1/p' || true)"
        if [ -n "$before" ] && [ -n "$after" ] && [ "$after" -gt "$before" ]; then
            note "stick: flushes during writes and unmounts ($before to $after)"
        else
            bad "stick: writes and unmounts did not increase flushes (${before:-?} to ${after:-?})"
        fi
    fi
    # A KRFFS volume of the stick: a partition blkid does not name is tried as
    # one, and the first that mounts is read, written and unmounted. A stick
    # with no such volume is a note.
    KVOL=""
    for part in 1 2 3 4; do
        node="/dev/${STICK}p${part}"
        if [ "$(run "if [ -b $node ]; then echo PRESENT; fi")" = PRESENT ] &&
            ! echo "$BLKID" | grep -q "^$node:" &&
            [ "$(run "mkdir -p /mnt/k || exit 1; if mount -t krffs $node /mnt/k; then echo MOUNTED; fi")" \
                = "MOUNTED" ]; then
            KVOL="$node"
            break
        fi
    done
    if [ -z "$KVOL" ]; then
        note "stick: no KRFFS volume on it (no partition of it mounts as krffs)"
    else
        before="$(run 'cat /proc/usb 2>/dev/null' | grep "^$STICK " | sed -n 's/.*flushes \([0-9]*\).*/\1/p')"
        OUT="$(run 'cat /mnt/k/hello.txt; echo written-on-the-stick > /mnt/k/accept.txt; cat /mnt/k/accept.txt')"
        after="$(run 'cat /proc/usb 2>/dev/null' | grep "^$STICK " | sed -n 's/.*flushes \([0-9]*\).*/\1/p')"
        if [ "$OUT" = "hello from inside an imagewritten-on-the-stick" ]; then
            note "stick: the KRFFS volume on $KVOL mounts, reads and takes a write"
        else
            bad "stick: the KRFFS volume on $KVOL did not read and write: $OUT"
        fi
        # KRFFS writes each field that publishes data after the data has
        # reached the medium, which on a stick takes a flush, not only a write
        # the device has taken.
        if [ -n "$before" ] && [ -n "$after" ] && [ "$after" -gt "$before" ]; then
            note "stick: writing it put the device's own cache on the medium ($before to $after)"
        else
            bad "stick: writing it never flushed the device (${before:-?} to ${after:-?})"
        fi
        OUT="$(run 'umount /mnt/k && rmdir /mnt/k && echo UNMOUNTED')"
        if [ "$OUT" = "UNMOUNTED" ]; then
            note "stick: and it unmounts"
        else
            bad "stick: the KRFFS volume did not unmount: $OUT"
        fi
    fi
fi

# ---- a hub, when one is plugged in: what the hub said it is, which devices
#      came up behind it and at what path, a low-speed device behind a
#      high-speed hub, the translator per port if the hub has them, and a
#      SuperSpeed hub's two halves. Each is a line in the record; a machine
#      with no hub is a note. ----
HUBLINES="$(grep 'usb: hub:' "$BOOTLOG_OUT" 2>/dev/null | sed 's/^.*usb: hub: /hub: /' || true)"
HUBSEEN="$(grep 'usb: device: .*, a hub' "$BOOTLOG_OUT" 2>/dev/null || true)"
USBLIST="$(run 'cat /proc/usb 2>/dev/null')"
if [ -z "$HUBLINES" ] && [ -z "$HUBSEEN" ] && ! echo "$USBLIST" | grep -q 'class 9 '; then
    note "no hub is plugged in"
elif [ -n "$HUBSEEN" ] && ! echo "$USBLIST" | grep -q 'class 9 '; then
    bad "a hub was seen and then went away: $HUBSEEN"
else
    echo "$HUBLINES" | while read -r line; do note "$line"; done
    echo "$USBLIST" | grep 'class 9 ' | while read -r line; do note "hub: $line"; done
    if echo "$USBLIST" | grep -q '^port [0-9]*\.[0-9]'; then
        echo "$USBLIST" | grep '^port [0-9]*\.[0-9]' | while read -r line; do note "behind a hub: $line"; done
    else
        bad "a hub is there and nothing behind it came up"
    fi
    if echo "$USBLIST" | grep '^port [0-9]*\.[0-9]' | grep -q ' low '; then
        note "hub: a low-speed device behind a hub is driven, which is the translator's case"
    else
        note "hub: no low-speed device behind a hub; the translator is untested on this run"
    fi
    if grep -q 'refused a translator per port' "$BOOTLOG_OUT" 2>/dev/null; then
        note "hub: one translator per port was refused; one is used"
    elif echo "$HUBLINES" | grep -q 'think time'; then
        note "hub: the think time is in the record above; a translator per port is selected when the hub offers one"
    fi
    if grep -q 'SuperSpeed hub' "$BOOTLOG_OUT" 2>/dev/null; then
        note "hub: a SuperSpeed hub was refused by name; its high-speed half is the one driven"
        if echo "$USBLIST" | grep '^port [0-9]*\.[0-9]' | grep -q ' super '; then
            bad "hub: a SuperSpeed device appeared behind a hub, which this kernel does not drive there"
        else
            note "hub: no SuperSpeed device appeared behind it"
        fi
    fi
fi

if [ -f "$LOCAL_TMP/command-failed" ]; then
    bad "a required remote command failed; its output is not acceptance evidence"
fi
if [ "$fails" -gt 0 ]; then
    echo "metal-accept: FAIL ($fails)" >&2
    exit 1
fi
echo "metal-accept: PASS"
