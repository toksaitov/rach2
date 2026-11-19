#!/bin/sh
# boot.sh DISK KERNEL APPEND OUT CMD: boot KERNEL with the boot line
# `rach2.root=ext2 APPEND` under KVM on an overlay of DISK, the GNU test
# disk, with virtio-vga's 2D display as qemu.sh has it, copy the files of
# OUT/in to /tmp/p in the guest, run CMD there as root over SSH and power the
# guest off. OUT receives the serial log. The probes beside this script are
# built on the host with `gcc -O2 -static -pthread` into OUT/in.
set -eu

[ $# -eq 5 ] || { echo 'usage: boot.sh DISK KERNEL APPEND OUT CMD' >&2; exit 2; }
here=$(cd "$(dirname "$0")" && pwd)
tree=$(cd "$here/../../../.." && pwd)
disk=$1 kernel=$2 append=$3 out=$4 cmd=$5
key=$tree/krffs/rach2_demo_ed25519
port=${PORT:-26692}
work='' qemu=''
trap '[ -z "$qemu" ] || { kill "$qemu" 2> /dev/null || true; wait "$qemu" 2> /dev/null || true; }
      [ -z "$work" ] || rm -rf "$work"' EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM
mkdir -p "$tree/build/tmp"
work=$(mktemp -d "$tree/build/tmp/rach2-t3292.XXXXXX")
qemu-img create -q -f qcow2 -F raw -b "$disk" "$work/disk.qcow2"
: > "$out/serial.log"
qemu-system-x86_64 -accel kvm -cpu host -smp 4 -m 8G -machine q35 \
    -kernel "$kernel" -append "rach2.root=ext2 $append" \
    -drive "file=$work/disk.qcow2,format=qcow2,if=none,id=d0" -device ahci,id=ahci \
    -device ide-hd,drive=d0,bus=ahci.0 \
    -nic "user,model=e1000,hostfwd=tcp:127.0.0.1:$port-:22" \
    -display none -vga none -device virtio-vga -serial "file:$out/serial.log" -monitor none &
qemu=$!
L() { ssh -i "$key" -p "$port" -o BatchMode=yes -o ConnectTimeout=5 -o LogLevel=ERROR \
          -o UserKnownHostsFile=/dev/null -o StrictHostKeyChecking=no root@127.0.0.1 "$@"; }
n=0
until L true 2> /dev/null; do
    kill -0 "$qemu" 2> /dev/null || { echo 'boot: qemu ended before SSH answered' >&2; exit 1; }
    n=$((n + 1))
    [ "$n" -lt 120 ] || { echo 'boot: no SSH after 600 s' >&2; exit 1; }
    sleep 5
done
tar -cf - -C "$out/in" . | L 'rm -rf /tmp/p && mkdir -p /tmp/p && tar -xf - -C /tmp/p'
status=0
L "cd /tmp/p && $cmd" || status=$?
L poweroff || true
left=60
while kill -0 "$qemu" 2> /dev/null && [ "$left" -gt 0 ]; do
    sleep 2
    left=$((left - 2))
done
exit "$status"
