#!/usr/bin/env sh

#
# metal_accept.sh: the scripted half of the metal acceptance, run over SSH.
#
# The acceptance for this milestone has two halves and they cannot be merged.
# One half needs a person in the room: a cable pulled and put back, a cold
# power cycle, a sound heard, a screen photographed, Windows booting with the
# stick out. The other half is every fact a machine can report about itself,
# and there is no reason for a person to read those off a screen and copy them
# down. This is that half.
#
# It is written to be run against the laptop from the development host once SSH
# answers, and it is run against QEMU first so that the script itself is not
# being debugged on the machine it is meant to measure. Nearly everything it
# does is a read, and it issues no command the read-only build's allowlist
# would refuse; the two exceptions, a file written to the scratch partition
# and one to a plugged-in stick, are each read back, removed and unmounted.
#
# Its output is a record, not a verdict on everything. Where a baseline exists
# (dev-log/24-metal/baseline.md, what Windows measured before Rach2 existed on
# that machine) it compares and says so; where none exists it reports the
# number so the record has it.
#
# The key is the demonstration one the RAM images carry, unless
# `RACH2_ACCEPT_KEY` names another, and the account is root unless
# `RACH2_ACCEPT_USER` names another. A machine installed by `rach2-install` has
# no demonstration key on it and root's keys are whatever its owner put there:
# the acceptance of such a machine is run with that account's name and key in
# the environment rather than with this script edited. The account must be able
# to run the acceptance's commands, which read and write the whole disk, so it
# is root's key on an installed machine or the account's on one where root's
# keys were left in place.
#
# Usage: metal_accept.sh <host> [port]
#

set -e

HOST="$1"
PORT="${2:-22}"
# The stick this script may write, by its serial as `/proc/disk` reports it;
# with none given, the first flash drive that is not a Rach2 recovery stick.
STICK_SERIAL="${3:-}"

if [ -z "$HOST" ]; then
    echo "usage: metal_accept.sh <host> [port] [stick-serial]" >&2
    exit 2
fi

REPO="$(cd "$(dirname "$0")/../../.." && pwd)"
KEY="${RACH2_ACCEPT_KEY:-$REPO/krffs/rach2_demo_ed25519}"
USER_NAME="${RACH2_ACCEPT_USER:-root}"
if [ ! -f "$KEY" ]; then
    echo "metal_accept.sh: $KEY is not a file: name the machine's own key in" \
         "RACH2_ACCEPT_KEY" >&2
    exit 2
fi
KNOWN="$REPO/build/images/metal-accept.known"

# The MBR's SHA-256 as Windows measured it, from dev-log/24-metal/baseline.md.
# Kept here, not read from the document so this script is one file to
# copy to a development host, and stated in both places so a change to one is
# visible against the other.
BASELINE_MBR_SHA256="652ad40fe72061185cf3a69c1a9a45394f68374f409153e082b7c1fa04c107d3"
BASELINE_SECTORS="1953525168"
BASELINE_MODEL="Samsung SSD 870 EVO 1TB"

# What the rows leave on the machine is removed here as well as in the rows,
# so an interrupted run leaves nothing but the machine as it was found: the
# image's mount and its loop device first, then the directory that holds the
# image, and the known-hosts file last, after the last connection. Each
# variable is set when its resource exists and cleared when the row let it
# go, and the function keeps the status the script is exiting with.
MACHINE_TMP=""
MACHINE_MOUNTED=""
MACHINE_LOOP=""
cleanup() {
    if [ -n "$MACHINE_MOUNTED" ]; then
        run "umount $MACHINE_MOUNTED; rmdir $MACHINE_MOUNTED" > /dev/null 2>&1 || true
    fi
    if [ -n "$MACHINE_LOOP" ]; then
        run "loopctl detach $MACHINE_LOOP" > /dev/null 2>&1 || true
    fi
    if [ -n "$MACHINE_TMP" ]; then
        run "rm -rf $MACHINE_TMP" > /dev/null 2>&1 || true
    fi
    rm -f "$KNOWN"
    return 0
}
trap 'cleanup' EXIT
trap 'cleanup; exit 130' INT
trap 'cleanup; exit 143' TERM
trap 'cleanup; exit 129' HUP

if [ ! -f "$KEY" ]; then
    echo "metal-accept: no key at $KEY" >&2
    exit 2
fi

fails=0
note() {
    echo "  $1"
}
bad() {
    fails=$((fails + 1))
    echo "  FAIL: $1"
}

run() {
    ssh -i "$KEY" -p "$PORT" \
        -o StrictHostKeyChecking=no -o UserKnownHostsFile="$KNOWN" \
        -o ConnectTimeout=10 -o BatchMode=yes \
        "$USER_NAME@$HOST" "$1" 2>/dev/null | tr -d '\r'
}

# The machine has no sftp server, so a file goes over as the standard input of
# a command that writes it, and comes back as the standard output of one that
# reads it.
put() {
    ssh -i "$KEY" -p "$PORT" \
        -o StrictHostKeyChecking=no -o UserKnownHostsFile="$KNOWN" \
        -o ConnectTimeout=10 -o BatchMode=yes \
        "$USER_NAME@$HOST" "cat > $2" < "$1" 2>/dev/null
}
get() {
    ssh -i "$KEY" -p "$PORT" \
        -o StrictHostKeyChecking=no -o UserKnownHostsFile="$KNOWN" \
        -o ConnectTimeout=10 -o BatchMode=yes \
        "$USER_NAME@$HOST" "cat $1" > "$2" 2>/dev/null
}

echo "metal-accept: $HOST:$PORT"

# ---- that it is reachable at all, which is the milestone's whole point ----
if [ "$(run 'echo ALIVE')" != "ALIVE" ]; then
    echo "metal-accept: FAIL: no SSH answer" >&2
    exit 1
fi
note "SSH answers"

# ---- what it says it is ----
note "uname: $(run 'uname -a')"
note "uptime ticks: $(run 'cat /proc/uptime 2>/dev/null || echo unavailable')"
note "boot line: $(run 'cat /proc/cmdline')"
note "identity: $(run 'id')"

# ---- the network, which is the thing that was ported. The address is on the
#      boot line, since this kernel publishes no interface file, and the fact
#      that this session exists at all is the stronger evidence anyway. ----
note "address given: $(run 'cat /proc/cmdline' | tr ' ' '\n' | grep '^rach2.ip=' || echo none)"

# ---- entropy, which dropbear's host key depends on ----
if run 'dd if=/dev/urandom bs=16 count=1 2>/dev/null | wc -c' | grep -q '^ *16$'; then
    note "the random pool is trusted (16 bytes read from /dev/urandom)"
else
    bad "the random pool refused a read, so this CPU has no hardware entropy and dropbear could not have minted a host key"
fi

# ---- the disk, read and compared against the baseline ----
DISK="$(run 'cat /proc/disk 2>/dev/null')"
if [ -z "$DISK" ]; then
    bad "/proc/disk is not there; the boot line needs rach2.diskdiag"
else
    state="$(echo "$DISK" | awk '/^state /{print $2}')"
    policy="$(echo "$DISK" | awk '/^policy /{print $2}')"
    mounted="$(echo "$DISK" | awk '/^mounted /{print $2}')"
    sectors="$(echo "$DISK" | awk '/^sectors /{print $2}')"
    model="$(echo "$DISK" | sed -n 's/^model //p')"

    note "disk: state=$state policy=$policy"
    # Two kernels answer here. The stick's, milestone 25's, is built
    # read-only because the disk under it may be somebody's Windows, and it
    # mounts nothing from that disk. The installed one, whose boot line names
    # its root with rach2.root=disk, rach2.root=ext2 or, since the installer
    # names the disk by its table's id, rach2.root=MBRID=ID,N, writes that
    # root and is built read-write; a read-only kernel there would be a
    # system that cannot take a file.
    if run 'cat /proc/cmdline' | grep -q 'rach2.root=\(disk\|ext2\|MBRID=\)'; then
        INSTALLED=1
        [ "$policy" = "read-write" ] || bad "the installed kernel is not the read-write build"
    else
        INSTALLED=0
        [ "$policy" = "read-only" ] || bad "the running kernel is not the read-only build"
    fi

    # A machine with no disk reports that and stops there, which is a complete
    # answer and not a failure: this script is rehearsed against exactly such a
    # machine. What would be a failure is a disk that is present AND mounted
    # under the stick's kernel.
    if [ "$state" != "present" ]; then
        note "disk: none answered, so there is nothing further to read"
    else
        note "disk: model [$model], $sectors sectors, mounted=$mounted"
        if [ "$INSTALLED" = 1 ]; then
            note "disk: the installed system's root is on it"
        else
            [ "$mounted" = "0" ] || bad "the disk is mounted, which the stick's kernel forbids"
        fi
    fi

    if [ "$state" = "present" ]; then
        mbr="$(run 'dd if=/dev/disk0 bs=512 count=1 2>/dev/null | sha256sum' | awk '{print $1}')"
        note "disk: the MBR hashes to $mbr"
        echo "$DISK" | grep '^part' | while read -r line; do note "disk: $line"; done

        # The baseline describes ONE drive. Against that drive every number
        # must agree, and a disagreement is the finding this script exists for.
        # Against any other drive, including the one QEMU provides when this
        # script is rehearsed, there is nothing to compare and saying so is the
        # honest answer; a script that failed there would be a script nobody
        # could rehearse.
        if [ "$model" = "$BASELINE_MODEL" ]; then
            note "disk: this is the drive the baseline describes; comparing"
            [ "$sectors" = "$BASELINE_SECTORS" ] \
                || bad "the sector count is $sectors and the baseline says $BASELINE_SECTORS"
            if [ "$mbr" = "$BASELINE_MBR_SHA256" ]; then
                note "disk: the MBR is what Windows measured before Rach2 ran here"
            else
                bad "the MBR does not match the baseline: a wrong sector was read, or something has written it"
            fi
        else
            note "disk: not the drive the baseline describes, so nothing is compared"
        fi
    fi
fi

# ---- the machine's own boot log, saved, not read ----
BOOTLOG_OUT="$REPO/build/images/metal-bootlog.txt"
if run 'cat /dev/bootlog' > "$BOOTLOG_OUT" 2>/dev/null && [ -s "$BOOTLOG_OUT" ]; then
    note "boot log: $(wc -l < "$BOOTLOG_OUT" | tr -d ' ') lines saved to $BOOTLOG_OUT"
    if grep -q "refused command" "$BOOTLOG_OUT"; then
        bad "the boot log records a refused disk command; something strayed off the read path"
    fi
else
    bad "/dev/bootlog gave nothing; this kernel predates it, or the log is empty"
fi

# ---- the USB hardware, read out of the log this machine already wrote. The
#      kernel takes the inventory from configuration space at boot, so there is
#      nothing to ask for here that is not already saved above. What matters on
#      this notebook is the routing line: its firmware decides whether the USB2
#      pins belong to the xHCI or to the companion EHCI, and a controller with
#      no pins looks exactly like a machine with nothing plugged in. ----
if grep -q 'usb: controller:' "$BOOTLOG_OUT" 2>/dev/null; then
    for pattern in 'usb: controller:' 'usb: routing:' 'usb: ready:' 'usb: port:' \
                   'usb: device:' 'usb: absent:' 'usb: refused:'; do
        grep "$pattern" "$BOOTLOG_OUT" | sed 's/^.*usb: /usb: /' | while read -r line; do
            note "$line"
        done
    done
    # Whether the stack got as far as a device is the question this machine
    # exists to answer, so it is asked here rather than left to a reader of the
    # lines above. It is a note and not a failure: a machine with nothing
    # plugged into it is not broken, and which it was is for the person who
    # plugged something in.
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
    bad "the boot log has no usb inventory; this kernel predates it"
fi

# ---- which interrupts this machine takes, and by which road. Written here
#      and run in milestone 43: on this notebook the question is whether the
#      APIC came up at all, whether the two controllers took their message
#      vectors or stayed on their legacy lines, and whether the 8259's own
#      interrupts still arrive with the APIC between them and the processor.
#      Almost every line is a note: a machine whose firmware leaves it on
#      legacy lines is not broken, and what it did is the record this
#      milestone is for. One thing is a failure, and it is the kernel's own
#      claim: a timer vector that has taken nothing, which would mean the
#      8259's interrupts stopped when the APIC came up. ----
INTR="$(run 'cat /proc/interrupts 2>/dev/null')"
if [ -z "$INTR" ]; then
    bad "/proc/interrupts is not there; this kernel predates it"
else
    echo "$INTR" | while read -r line; do note "interrupts: $line"; done
    timer="$(echo "$INTR" | awk -F'\t' '$1 == "32:" { print $2 }')"
    # By the device's name and not by a number: a vector is given to a device
    # when it enables its messages, so the number is whatever came next.
    usb_vector="$(echo "$INTR" | awk -F'\t' '$4 ~ /xhci/ { print $2 }')"
    audio_vector="$(echo "$INTR" | awk -F'\t' '$4 ~ /hda/ { print $2 }')"
    fallback="$(echo "$INTR" | awk -F'\t' '$1 == "FBK:" { print $2 }')"
    spurious="$(echo "$INTR" | awk -F'\t' '$1 == "SPU:" { print $2 }')"

    # A vector that has taken nothing has no line at all, so an empty answer
    # here is the failure and there is no zero to test for.
    case "$timer" in
        ''|*[!0-9]*) bad "the timer's vector has no line, so the 8259's interrupts are not arriving" ;;
        *) note "interrupts: the timer has taken $timer, so the 8259's road is open" ;;
    esac
    note "interrupts: usb vector ${usb_vector:-none}, audio vector ${audio_vector:-none}"
    note "interrupts: spurious ${spurious:-none}, usb fallback ${fallback:-none}"
    # The fallback count is a note and not a verdict on this machine. Under
    # the emulator it is zero, because a message sent is a message the
    # processor has already latched by the time the timer's pump looks. On
    # hardware the event is in memory while the message is still on its way,
    # and a count that is small beside the vector's own is that race and not a
    # lost edge. What milestone 43 does with the number is read it.
    if [ -n "$usb_vector" ] && [ "${fallback:-0}" != "0" ]; then
        note "interrupts: the timer's pump found the work of $fallback of $usb_vector messages"
    fi
    if grep -q 'lapic: refused:' "$BOOTLOG_OUT" 2>/dev/null; then
        note "interrupts: $(grep -m1 'lapic: refused:' "$BOOTLOG_OUT" | sed 's/^.*lapic: /lapic: /')"
    elif grep -q 'lapic: ready:' "$BOOTLOG_OUT" 2>/dev/null; then
        note "interrupts: $(grep -m1 'lapic: ready:' "$BOOTLOG_OUT" | sed 's/^.*lapic: /lapic: /')"
    else
        bad "the boot log says nothing about the local APIC; this kernel predates it"
    fi
    if grep -q 'lapic: timer:' "$BOOTLOG_OUT" 2>/dev/null; then
        bad "the timer stopped when the APIC was enabled: $(grep -m1 'lapic: timer:' "$BOOTLOG_OUT")"
    fi
fi

# ---- and that nothing on this machine has been refused a command ----
note "(the saved boot log above is the whole of what this machine printed)"

# ---- the partitions as mounts. Written for milestone 36 and run in milestone
#      43: the partition nodes the MBR gives this machine, the boot partition
#      mounted read-only through the shell with its files the sizes bootctl
#      reports for its slots, and, when the boot line has rach2.blocklag=N,
#      the lag check through the shell: a read through the lag device sleeps
#      its reader while a loop in another shell keeps counting, which is the
#      scheduler's part of the block path and the thing no host test can
#      show. A machine with no slot report has no sealed disk and this is a
#      note there; a machine whose bootctl answers has the sealed layout and
#      owes its three partitions. ----
NODES="$(run 'ls /dev 2>/dev/null' | grep '^disk0p[1-4]$' | tr '\n' ' ')"
SLOTS="$(run 'bootctl status 2>/dev/null')"
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
    OUT="$(run 'mkdir -p /mnt/accept-boot && mount -r -t vfat /dev/disk0p1 /mnt/accept-boot && grep "^/dev/disk0p1 /mnt/accept-boot vfat ro" /proc/mounts && ls -l /mnt/accept-boot/boot /mnt/accept-boot/boot/grub; umount /mnt/accept-boot; rmdir /mnt/accept-boot')"
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
                note "partitions: /dev/disk0p1 is a FAT volume without a kernel, so not this machine's boot partition"
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
    LAGN="$(run 'cat /proc/cmdline' | tr ' ' '\n' | sed -n 's/^rach2.blocklag=//p' | head -1)"
    case "$LAGN" in
        ''|*[!0-9]*)
            note "partitions: the lag check needs rach2.blocklag=N on the boot line"
            ;;
        *)
            # 64 KiB in eight-sector requests is sixteen sleeps of N ticks
            # each, so the read takes at least 16N ticks by the machine's own
            # clock; a loop that counted nothing meanwhile is a reader that
            # never slept, and a read that failed is said so and not counted.
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
                    bad "partitions: the 64 KiB read took $ticks ticks, under the $((16 * LAGN)) the lag of $LAGN owes"
                else
                    note "partitions: the shell counted to $count while a 64 KiB read slept $ticks ticks through the lag of $LAGN"
                fi
            fi
            ;;
    esac
fi

# ---- the ext2 root, once milestone 43 has made it: the candidate's command
#      line names it, the mount table has it writable on the fourth partition,
#      the boot came from the candidate slot, the boot log shows either no
#      check or a clean one, and the volume takes a write that survives a
#      sync. A boot after a power cut checks the root and records
#      `ext2: check: clean`; a check that found lost blocks, orphans or a fault
#      is the failure. Before the ext2 root the command line says
#      rach2.rootfstype=krffs and this is a note, not a failure. ----
if run 'cat /proc/cmdline' | grep -q 'rach2.rootfstype=ext2'; then
    OUT="$(run 'cat /proc/mounts')"
    case "$OUT" in
        *"/dev/disk0p4 / ext2 rw,relatime"*) note "the ext2 root is mounted writable from /dev/disk0p4" ;;
        *) bad "the ext2 root is not the mounted root: $OUT" ;;
    esac
    OUT="$(run 'bootctl status')"
    case "$OUT" in
        *"booted: candidate"*) note "the ext2 root boot came from the candidate slot" ;;
        *) bad "the ext2 root boot did not come from the candidate slot: $OUT" ;;
    esac
    if [ -s "$BOOTLOG_OUT" ] && grep -q "ext2: check" "$BOOTLOG_OUT"; then
        if grep -q "ext2: check: clean" "$BOOTLOG_OUT"; then
            note "the ext2 root was checked at this boot and found clean; it was not unmounted the time before"
        else
            bad "the ext2 root's check found a fault: $(grep 'ext2: check' "$BOOTLOG_OUT" | head -1)"
        fi
    fi
    # Read back through the same cache: that the bytes reached the disk is
    # what the procedure's reboot after a write shows, not this call.
    # The remote shell removes its file on its own exit, so a connection cut
    # after the mktemp leaves nothing behind either.
    # shellcheck disable=SC2016 # the machine expands these, not this script
    OUT="$(run 'f=$(mktemp /root/accept.XXXXXX) && trap '"'"'rm -f "$f"'"'"' EXIT && echo accept > "$f" && sync && cat "$f"')"
    if [ "$OUT" = "accept" ]; then
        note "the ext2 root takes a file and gives it back"
    else
        bad "the ext2 root did not take a write: $OUT"
    fi
else
    note "the root is not ext2 yet (rach2.rootfstype=ext2 absent from the command line)"
fi

# ---- the scratch partition, before milestone 43 makes the fourth partition
#      the ext2 root: mounted for writing, a file written and read back, the
#      mount taken away. Absent, or ext2 already, this is a note. ----
if run 'cat /proc/cmdline' | grep -q 'rach2.rootfstype=ext2'; then
    note "the fourth partition is the ext2 root now; no scratch partition"
elif run 'ls /dev/disk0p4 2>/dev/null' | grep -q disk0p4; then
    # shellcheck disable=SC2016 # the machine expands these, not this script
    OUT="$(run 'mkdir -p /mnt && mount -t vfat /dev/disk0p4 /mnt && f=$(mktemp /mnt/accept.XXXXXX) && echo accept > "$f" && cat "$f"; rm -f "$f"; umount /mnt')"
    if [ "$OUT" = "accept" ]; then
        note "the scratch partition mounts for writing, takes a file and gives it back"
    else
        bad "the scratch partition did not take a write: $OUT"
    fi
else
    note "no scratch partition yet (/dev/disk0p4 absent)"
fi

# ---- a file system in a file, which milestone 44 made possible: an image on
#      the root, attached to a loop device, mounted, written, read back,
#      unmounted, detached, and judged here by the host's own checker. The
#      image is built here and taken away again, so the machine is left as it
#      was found. ----
if ! run 'grep -q krffs /proc/filesystems && echo LISTED' | grep -q LISTED; then
    note "this kernel does not take the krffs type; no image row"
elif [ ! -x "$REPO/build/krffs/mkfs.krffs" ]; then
    note "no host KRFFS tools built; no image row"
else
    IMG="$REPO/build/images/metal-loop.krffs"
    TREE="$REPO/build/tests/system/loopimg"
    mkdir -p "$TREE"
    printf 'hello from inside an image' > "$TREE/hello.txt"
    dd if=/dev/zero of="$TREE/inner.bin" bs=512 count=8 2>/dev/null
    if ! sh "$REPO/krffs/mkimage.sh" "$REPO/krffs/manifests/loopimg-test.manifest" \
        "$IMG" 1 >/dev/null 2>&1; then
        note "the image could not be built here; no image row"
    elif ! MACHINE_TMP="$(run 'mktemp -d /tmp/accept.XXXXXX')" || [ -z "$MACHINE_TMP" ]; then
        note "the machine gave no temporary directory; no image row"
    elif ! put "$IMG" "$MACHINE_TMP/loop.img" ||
        [ "$(run "wc -c < $MACHINE_TMP/loop.img")" != "1048576" ]; then
        run "rm -rf $MACHINE_TMP"; MACHINE_TMP=""
        note "the root has no room for a 1 MiB image; no image row"
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
        get "$MACHINE_TMP/loop.img" "$IMG.after"
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
        run "rm -rf $MACHINE_TMP"; MACHINE_TMP=""
    fi
fi

# ---- a flash drive, when one is plugged in. Written in milestone 40 and run
#      in milestone 43 with the owner's stick, formatted with a GUID table and
#      then with an MBR: the kernel names it, reports it as a block device,
#      BusyBox finds its volume by label, and the volume takes a file and
#      gives it back after an unmount that flushed the stick's own cache. A
#      machine with nothing plugged in is a note, not a failure. The pull from
#      under a mount and the boot from a Rach2 stick need a person in the
#      room and are in dev-log/40-usbdisk/hardware.md.
#      The stick is whichever disk slot holds one: the kernel reclaims a slot
#      only when its next stick is found, so the machine's first stick may be
#      disk1 for a while. A stick whose table has a Rach2 recovery ISO in it,
#      entry type 17, is a recovery stick and is refused whatever else is on
#      it, since the acceptance must not write over the stick that boots the
#      recovery system. With a serial given, the stick is the one that
#      reports it and no other, and two that report it stop the acceptance.
#      The volume is the first FAT or exFAT partition blkid finds on the
#      chosen stick, mounted by its node and not by a label another stick
#      could carry. ----
USBLINES="$(run 'cat /proc/usb 2>/dev/null' | grep '^disk[0-9]* ' || true)"
STICK=""
USBLINE=""
if [ -z "$USBLINES" ]; then
    note "no flash drive is plugged in (/proc/usb names no usb disk)"
else
    echo "$USBLINES" | while read -r line; do note "stick: $line"; done
    PARTS="$(run 'cat /proc/disk 2>/dev/null' || true)"
    for cand in $(echo "$USBLINES" | awk '{print $1}'); do
        n="${cand#disk}"
        serial="$(echo "$PARTS" | grep "^identity $n " | head -1 | cut -d' ' -f6-)"
        if echo "$PARTS" | grep -q "^part $n [0-9]* [0-9]* [0-9]* mbr:17 "; then
            note "stick: /dev/$cand ($serial) carries a Rach2 recovery ISO in its table and is not written"
            continue
        fi
        if [ -n "$STICK_SERIAL" ] && [ "$serial" != "$STICK_SERIAL" ]; then
            note "stick: /dev/$cand ($serial) is not the stick named, $STICK_SERIAL, and is not written"
            continue
        fi
        if [ -n "$STICK" ] && [ -n "$STICK_SERIAL" ]; then
            bad "stick: /dev/$cand and /dev/$STICK both report the serial $STICK_SERIAL; neither is written"
        fi
        if [ -n "$STICK" ]; then
            note "stick: /dev/$cand ($serial) is a second stick that could be written; the first is"
            continue
        fi
        STICK="$cand"
        USBLINE="$(echo "$USBLINES" | grep "^$cand " | head -1)"
        note "stick: /dev/$cand ($serial) is the one written"
    done
fi
if [ -z "$STICK" ] && [ -n "$USBLINES" ]; then
    if [ -n "$STICK_SERIAL" ]; then
        bad "stick: no flash drive reports the serial $STICK_SERIAL that is not a recovery stick"
    else
        note "stick: every flash drive plugged in is a recovery stick; none is written"
    fi
fi
if [ -n "$STICK" ]; then
    case "$(run "ls -l /dev/$STICK 2>/dev/null")" in
        b*) note "stick: /dev/$STICK is a block device" ;;
        *)  bad "stick: /dev/$STICK is not a block device, so a label scan cannot find it" ;;
    esac
    BLKID="$(run 'blkid 2>/dev/null' | grep "^/dev/$STICK")"
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
        # The unmount runs whether or not the write and the read back did, so
        # a failure leaves neither a file nor a mount behind on the stick.
        CMD="mkdir -p /mnt && mount $VOLNODE /mnt && { f=\$(mktemp /mnt/accept.XXXXXX) && echo accept > \"\$f\" && cat \"\$f\"; rm -f \"\$f\"; umount /mnt; }"
        OUT="$(run "$CMD")"
        after="$(run 'cat /proc/usb 2>/dev/null' | grep "^$STICK " | sed -n 's/.*flushes \([0-9]*\).*/\1/p' || true)"
        if [ "$OUT" = "accept" ]; then
            note "stick: the volume $LABEL on $VOLNODE mounts, takes a file and gives it back"
        else
            bad "stick: the volume $LABEL did not take a write: $OUT"
        fi
        if [ -n "$before" ] && [ -n "$after" ] && [ "$after" -gt "$before" ]; then
            note "stick: the unmount flushed its cache ($before to $after)"
        else
            bad "stick: the unmount did not flush its cache (${before:-?} to ${after:-?})"
        fi
    fi
    # A KRFFS volume of the stick, which milestone 44 made mountable: a
    # partition blkid does not name is tried as one, and the first that mounts
    # is read, written and unmounted. A stick with no such volume says so.
    KVOL=""
    for part in 1 2 3 4; do
        node="/dev/${STICK}p${part}"
        if [ -n "$(run "ls $node 2>/dev/null")" ] &&
            ! echo "$BLKID" | grep -q "^$node:" &&
            [ "$(run "mkdir -p /mnt/k && mount -t krffs $node /mnt/k && echo MOUNTED")" \
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
        # Every field the format publishes is written after the bytes it
        # publishes have reached the medium, which on a stick is a flush and
        # not a write the device has merely taken.
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

# ---- a hub, when one is plugged in. Written in milestone 41 and run in
#      milestone 43 with the owner's hub: what the hub said it is, which
#      devices came up behind it and at what path, a low-speed device behind
#      a high-speed hub, the translator per port if the hub has them, and a
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

if [ "$fails" -gt 0 ]; then
    echo "metal-accept: FAIL ($fails)" >&2
    exit 1
fi
echo "metal-accept: PASS"
