#!/bin/sh
# qemu.sh ROOT KERNEL OUT: the rehearsal in QEMU, on the testing host, of the
# trial boot of 5368's netguest rerun on the Lenovo. Lay ROOT, the trial
# root, on entry 6 of a new disk with the Lenovo's GUID table identity, as
# the data partition holds it, boot KERNEL under KVM from the root image as
# the step's trial line names it, with `rach2.ip=none` so that init brings
# eth0 up with `udhcpc -b` as on the Lenovo, run stopcheck.sh of this
# directory as root, and power off. OUT receives the serial log, the boot's
# state and stopcheck.sh's output; the last line is `qemu: result:
# stopcheck.sh status S`.
set -eu

[ $# -eq 3 ] || { echo 'usage: qemu.sh ROOT KERNEL OUT' >&2; exit 2; }
root=$1 kernel=$2 out=$3
here=$(cd "$(dirname "$0")" && pwd)
tree=$(cd "$here/../../.." && pwd)
key=$tree/krffs/rach2_demo_ed25519
guid=1acef00d-0f1e-4d2c-8b3a-c0ffee000001
port=${PORT:-25368}
work='' qemu=''
trap '[ -z "$qemu" ] || { kill "$qemu" 2> /dev/null || true; wait "$qemu" 2> /dev/null || true; }
      [ -z "$work" ] || rm -rf "$work"' EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM
mkdir -p "$out" "$tree/build/tmp"
work=$(mktemp -d "$tree/build/tmp/rach2-t5368.XXXXXX")

hash=$(sha256sum "$root" | cut -d' ' -f1)
image=root-$(echo "$hash" | cut -c1-16).img
echo "qemu: root $root sha256 $hash" | tee "$out/inputs.txt"
sha256sum "$kernel" | tee -a "$out/inputs.txt"
mkdir -p "$work/data/trial5368"
ln "$root" "$work/data/trial5368/$image" 2> /dev/null || cp --sparse=always "$root" "$work/data/trial5368/$image"

# Entry 6 from 1 MiB, 32 GiB, made with Rach2's mke2fs defaults as the
# Lenovo's was (dev-log/76-display/lenovo-storage.md). The table is written
# after the file system, since mke2fs wipes the signatures it finds.
truncate -s 33G "$work/disk.img"
MKE2FS_CONFIG=$tree/vendor/e2fsprogs/mke2fs.conf mke2fs -q -t ext2 -b 4096 -F -E offset=1048576 \
    -L Rach2_data -d "$work/data" "$work/disk.img" 8388608
sgdisk -o -U "$guid" -n 6:2048:+32G -t 6:8300 -c 6:'Rach2 data' "$work/disk.img" > /dev/null
rm -rf "$work/data"
echo "qemu: data partition made"

: > "$out/serial.log"
line="rach2.rootimage=GPTID=$guid,6:/trial5368/$image rach2.slot=trialroot rach2.ip=none"
qemu-system-x86_64 -accel kvm -cpu host,invtsc=on -smp 4 -m 4G -machine q35 \
    -kernel "$kernel" -append "$line" \
    -drive "file=$work/disk.img,format=raw,if=none,id=d0" -device ide-hd,drive=d0,bus=ide.0 \
    -nic "user,model=rtl8139,hostfwd=tcp:127.0.0.1:$port-:22" \
    -display none -vga none -serial "file:$out/serial.log" -monitor none &
qemu=$!
L() { ssh -i "$key" -p "$port" -o BatchMode=yes -o ConnectTimeout=5 -o LogLevel=ERROR \
          -o UserKnownHostsFile=/dev/null -o StrictHostKeyChecking=no root@127.0.0.1 "$@"; }
# Wait for SSH while the serial log grows; stop when it has not grown for 5 min.
size=0 still=0
until L true 2> /dev/null; do
    kill -0 "$qemu" 2> /dev/null || { echo 'qemu: ended before SSH answered' >&2; exit 1; }
    now=$(wc -c < "$out/serial.log")
    if [ "$now" -eq "$size" ]; then still=$((still + 5)); else still=0 size=$now; fi
    [ "$still" -lt 300 ] || { echo 'qemu: the boot stopped' >&2; tail -n 20 "$out/serial.log" >&2; exit 1; }
    sleep 5
done
L 'cat /proc/cmdline; uname -a; grep -e " / " -e " /data " /proc/mounts; ls -l /var/run/udhcpc.*.pid;
   ps -o pid,ppid,stat,args | grep -e udhcpc -e init | grep -v grep' 2>&1 | tee "$out/boot.txt"
status=0
L 'sh -s' < "$here/stopcheck.sh" > "$out/stopcheck.out" 2>&1 || status=$?
cat "$out/stopcheck.out"
L 'dmesg' > "$out/dmesg.txt" 2>&1 || true
L 'sync; poweroff' || true
left=120
while kill -0 "$qemu" 2> /dev/null && [ "$left" -gt 0 ]; do
    sleep 5
    left=$((left - 5))
done
[ "$left" -gt 0 ] || echo 'qemu: the guest did not power off; stopping it' >&2
echo "qemu: result: stopcheck.sh status $status"
exit "$status"
