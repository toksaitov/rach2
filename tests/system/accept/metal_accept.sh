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
# being debugged on the machine it is meant to measure. Everything it does is a
# read: it mounts nothing, writes nothing, and issues no command the read-only
# build's allowlist would refuse.
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
    if grep -q 'usb: device: port' "$BOOTLOG_OUT"; then
        note "usb: a device was enumerated on this machine"
    elif grep -q 'usb: ready:' "$BOOTLOG_OUT"; then
        note "usb: the controller came up and nothing was enumerated on it"
    else
        note "usb: no controller came up; the lines above say why"
    fi
else
    bad "the boot log has no usb inventory; this kernel predates it"
fi

# ---- and that nothing on this machine has been refused a command ----
note "(the saved boot log above is the whole of what this machine printed)"

if [ "$fails" -gt 0 ]; then
    echo "metal-accept: FAIL ($fails)" >&2
    exit 1
fi
echo "metal-accept: PASS"
