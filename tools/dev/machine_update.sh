#!/bin/sh
#
# machine_update.sh: install a kernel on the installed Lenovo over SSH, boot
# it once as a trial, and commit it. A phase's release installs the whole
# system with tools/flow/release.sh; this is for a kernel alone, by hand.
#
# Usage: machine_update.sh KERNEL [ADDRESS]
#
# KERNEL is the x86-64 kernel on this host, build/x86_64/rach2/kernel of a
# build from clean. ADDRESS is the machine's, 192.168.0.106 unless given;
# RACH2_MACHINE_MAC names the station address the machine must report,
# 28:d2:44:7e:e0:af by default, the Lenovo's RTL8168GU
# (dev-log/43-machine/hardware.md). Before anything is written the machine
# must report that address, a boot from its candidate or fallback slot and no
# trial armed. The kernel goes to /dev/shm, a tmpfs, and is compared there by
# SHA-256. `bootctl kernel` then sets the fallback as the saved default,
# writes the candidate slot and arms one trial of it, and the machine
# reboots. When SSH answers again, machine_identity.sh must find the
# candidate slot booted and holding KERNEL, and `bootctl commit` makes the
# candidate the saved default.
#
# A kernel that never answers leaves the machine in its trial boot, and a
# power cycle returns it to the fallback kernel; that needs a person at the
# machine, and the script says so. Exit 0 when the kernel runs and is
# committed, 1 when a step fails, 2 on a usage error.
set -u

[ "$#" -ge 1 ] && [ "$#" -le 2 ] || {
    echo 'usage: machine_update.sh KERNEL [ADDRESS]' >&2
    exit 2
}
kernel=$1
address=${2:-192.168.0.106}
mac=${RACH2_MACHINE_MAC:-28:d2:44:7e:e0:af}
root=$(cd "$(dirname "$0")/../.." && pwd)
key=$root/krffs/rach2_demo_ed25519
[ -s "$kernel" ] || { echo "machine-update: no kernel at $kernel" >&2; exit 2; }
chmod 600 "$key"

work=$(mktemp -d "${TMPDIR:-/tmp}/machine-update.XXXXXX") || exit 1
trap 'rm -rf "$work"' EXIT
set -- ssh -i "$key" -o BatchMode=yes -o ConnectTimeout=10 -o LogLevel=ERROR \
    -o UserKnownHostsFile="$work/known" -o StrictHostKeyChecking=accept-new \
    "root@$address"

fail()
{
    echo "machine-update: failed: $*" >&2
    exit 1
}

echo "machine-update: checking the machine at $address"
"$@" 'cat /sys/class/net/eth0/address; bootctl status' > "$work/before" 2>&1 ||
    fail "$(tail -n 1 "$work/before")"
[ "$(sed -n 1p "$work/before")" = "$mac" ] ||
    fail "the machine reports station address $(sed -n 1p "$work/before"), not $mac"
grep -Eq '^bootctl: booted: (candidate|fallback)$' "$work/before" ||
    fail "the machine did not boot from a slot: $(grep 'booted:' "$work/before")"
grep -Eq '^bootctl: next_entry: (empty|absent)' "$work/before" ||
    fail "a trial is armed: $(grep 'next_entry:' "$work/before")"

echo "machine-update: sending $kernel"
want=$(shasum -a 256 "$kernel" | cut -d ' ' -f 1)
"$@" 'cat > /dev/shm/kernel' < "$kernel" || fail "the copy to /dev/shm did not finish"
got=$("$@" 'sha256sum /dev/shm/kernel' | cut -d ' ' -f 1)
[ "$got" = "$want" ] || fail "the copy hashes to $got, not $want"

echo "machine-update: installing it in the candidate slot"
"$@" 'bootctl kernel /dev/shm/kernel' > "$work/install" 2>&1
grep -q '^bootctl: installed:' "$work/install" || fail "$(cat "$work/install")"
"$@" 'reboot' > /dev/null 2>&1

echo "machine-update: waiting for the trial boot"
sleep 15
i=0
until "$@" true > /dev/null 2>&1; do
    i=$((i + 1))
    [ "$i" -lt 60 ] ||
        fail "no answer 300 s after the reboot; the trial may have hung, and a power cycle returns the machine to the fallback kernel"
    sleep 5
done
RACH2_MEASURE_SLOT=candidate RACH2_MEASURE_KERNEL=$kernel \
    sh "$root/tools/dev/machine_identity.sh" "$work/identity" "$@" ||
    fail "the machine does not run the new kernel from the candidate slot: $(tail -n 3 "$work/identity")"

"$@" 'bootctl commit' > "$work/commit" 2>&1
grep -q '^bootctl: committed:' "$work/commit" || fail "$(cat "$work/commit")"
echo "machine-update: result: $address runs $want from the candidate slot, committed"
