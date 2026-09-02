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
# Usage: metal_accept.sh <host> [port]
#

set -e

HOST="$1"
PORT="${2:-22}"

if [ -z "$HOST" ]; then
    echo "usage: metal_accept.sh <host> [port]" >&2
    exit 2
fi

REPO="$(cd "$(dirname "$0")/../../.." && pwd)"
KEY="$REPO/krffs/rach2_demo_ed25519"
KNOWN="$REPO/build/images/metal-accept.known"

# The MBR's SHA-256 as Windows measured it, from dev-log/24-metal/baseline.md.
# Kept here, not read from the document so this script is one file to
# copy to a development host, and stated in both places so a change to one is
# visible against the other.
BASELINE_MBR_SHA256="652ad40fe72061185cf3a69c1a9a45394f68374f409153e082b7c1fa04c107d3"
BASELINE_SECTORS="1953525168"
BASELINE_MODEL="Samsung SSD 870 EVO 1TB"

cleanup() {
    rm -f "$KNOWN"
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
        "root@$HOST" "$1" 2>/dev/null | tr -d '\r'
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
    [ "$policy" = "read-only" ] || bad "the running kernel is not the read-only build"

    # A machine with no disk reports that and stops there, which is a complete
    # answer and not a failure: this script is rehearsed against exactly such a
    # machine. What would be a failure is a disk that is present AND mounted.
    if [ "$state" != "present" ]; then
        note "disk: none answered, so there is nothing further to read"
    else
        note "disk: model [$model], $sectors sectors, mounted=$mounted"
        [ "$mounted" = "0" ] || bad "the disk is mounted, which this milestone forbids"
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
    usb_vector="$(echo "$INTR" | awk -F'\t' '$1 == "48:" { print $2 }')"
    audio_vector="$(echo "$INTR" | awk -F'\t' '$1 == "49:" { print $2 }')"
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

# ---- the scratch partition, once milestone 43 has made it: mounted for
#      writing, a file written and read back, the mount taken away. Before
#      that the node is absent and this is a note, not a failure. ----
if run 'ls /dev/disk0p4 2>/dev/null' | grep -q disk0p4; then
    OUT="$(run 'mkdir -p /mnt && mount -t vfat /dev/disk0p4 /mnt && echo accept > /mnt/accept.txt && cat /mnt/accept.txt && umount /mnt')"
    if [ "$OUT" = "accept" ]; then
        note "the scratch partition mounts for writing, takes a file and gives it back"
    else
        bad "the scratch partition did not take a write: $OUT"
    fi
else
    note "no scratch partition yet (/dev/disk0p4 absent)"
fi

# ---- a flash drive, when one is plugged in. Written in milestone 40 and run
#      in milestone 43 with the owner's stick, formatted with a GUID table and
#      then with an MBR: the kernel names it, reports it as a block device,
#      BusyBox finds its volume by label, and the volume takes a file and
#      gives it back after an unmount that flushed the stick's own cache. A
#      machine with nothing plugged in is a note, not a failure. The pull from
#      under a mount and the boot from a Rach2 stick need a person in the
#      room and are in dev-log/40-usbdisk/hardware.md. ----
USBLINE="$(run 'cat /proc/usb 2>/dev/null' | grep '^usb0 ' || true)"
if [ -z "$USBLINE" ]; then
    note "no flash drive is plugged in (/proc/usb names no usb0)"
else
    note "stick: $USBLINE"
    case "$(run 'ls -l /dev/usb0 2>/dev/null')" in
        b*) note "stick: /dev/usb0 is a block device" ;;
        *)  bad "stick: /dev/usb0 is not a block device, so a label scan cannot find it" ;;
    esac
    run 'blkid 2>/dev/null' | grep '^/dev/usb0' | while read -r line; do note "stick: $line"; done
    LABEL="$(run 'blkid 2>/dev/null' | grep '^/dev/usb0' | sed -n 's/.*LABEL="\([^"]*\)".*/\1/p' | head -1 || true)"
    # The label is the stick's own text and goes into a shell command on the
    # machine, so only a label of letters, digits, `_` and `-` is used.
    case "$LABEL" in
        *[!A-Za-z0-9_-]*) bad "stick: the label has a character this script will not put in a command: $LABEL"; LABEL="" ;;
    esac
    if [ -z "$LABEL" ]; then
        bad "stick: no volume with a plain label was found on it"
    else
        before="$(echo "$USBLINE" | sed -n 's/.*flushes \([0-9]*\).*/\1/p')"
        # The unmount runs whether or not the write and the read back did, so
        # a failure leaves neither a file nor a mount behind on the stick.
        OUT="$(run "mkdir -p /mnt && mount 'LABEL=$LABEL' /mnt && { echo accept > /mnt/accept.txt && cat /mnt/accept.txt; rm -f /mnt/accept.txt; umount /mnt; }")"
        after="$(run 'cat /proc/usb 2>/dev/null' | grep '^usb0 ' | sed -n 's/.*flushes \([0-9]*\).*/\1/p' || true)"
        if [ "$OUT" = "accept" ]; then
            note "stick: the volume $LABEL mounts by its label, takes a file and gives it back"
        else
            bad "stick: the volume $LABEL did not take a write: $OUT"
        fi
        if [ -n "$before" ] && [ -n "$after" ] && [ "$after" -gt "$before" ]; then
            note "stick: the unmount flushed its cache ($before to $after)"
        else
            bad "stick: the unmount did not flush its cache (${before:-?} to ${after:-?})"
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
USBLIST="$(run 'cat /proc/usb 2>/dev/null')"
if [ -z "$HUBLINES" ] && ! echo "$USBLIST" | grep -q 'class 9 '; then
    note "no hub is plugged in"
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
