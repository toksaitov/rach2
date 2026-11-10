#!/bin/sh
# qemu.sh ROOT BUNDLE KERNEL OUT: task 80-5's rehearsal in QEMU. Lay ROOT, the
# trial root, and BUNDLE, the Wine probes' payload, on entry 6 of a new disk
# with the Lenovo's GUID table identity, as its data partition holds them,
# boot KERNEL under KVM with the root image as the Lenovo's trial boot names
# it, check the boot, run gpu.sh on llvmpipe over SSH and power the guest off.
# OUT receives the serial log, the checks and gpu.sh's results.
set -eu

[ $# -eq 4 ] || { echo 'usage: qemu.sh ROOT BUNDLE KERNEL OUT' >&2; exit 2; }
root=$1 bundle=$2 kernel=$3 out=$4
here=$(cd "$(dirname "$0")" && pwd)
tree=$(cd "$here/../../.." && pwd)
key=$tree/krffs/rach2_demo_ed25519
guid=1acef00d-0f1e-4d2c-8b3a-c0ffee000001
port=${PORT:-26540}
work= qemu=
trap '[ -z "$qemu" ] || { kill "$qemu" 2> /dev/null || true; wait "$qemu" 2> /dev/null || true; }
      [ -z "$work" ] || rm -rf "$work"' EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM
mkdir -p "$out" "$tree/build/tmp"
work=$(mktemp -d "$tree/build/tmp/rach2-t2654.XXXXXX")

hash=$(sha256sum "$root" | cut -d' ' -f1)
image=root-$(echo "$hash" | cut -c1-16).img
echo "qemu: root $root sha256 $hash" | tee "$out/inputs.txt"
sha256sum "$bundle" "$kernel" | tee -a "$out/inputs.txt"
mkdir -p "$work/data/trial2654/probes"
ln "$root" "$work/data/trial2654/$image" 2> /dev/null || cp --sparse=always "$root" "$work/data/trial2654/$image"
tar -xf "$bundle" -C "$work/data/trial2654/probes"
# Entry 6 from 1 MiB, 32 GiB, made with Rach2's mke2fs defaults as the
# Lenovo's was (dev-log/76-display/lenovo-storage.md). The table is written
# after the file system, since mke2fs wipes the signatures it finds.
truncate -s 33G "$work/disk.img"
MKE2FS_CONFIG=$tree/vendor/e2fsprogs/mke2fs.conf mke2fs -q -t ext2 -b 4096 -F -E offset=1048576 \
    -L Rach2_data -d "$work/data" "$work/disk.img" 8388608
sgdisk -o -U "$guid" -n 6:2048:+32G -t 6:8300 -c 6:'Rach2 data' "$work/disk.img"
sgdisk -p "$work/disk.img" | tee -a "$out/inputs.txt"
rm -rf "$work/data"
echo "qemu: data partition made"

: > "$out/serial.log"
line="rach2.rootimage=GPTID=$guid,6:/trial2654/$image rach2.slot=trialroot rach2.trace=enosys"
qemu-system-x86_64 -accel kvm -cpu host,invtsc=on -smp 4 -m 8G -machine q35 \
    -kernel "$kernel" -append "$line" \
    -drive "file=$work/disk.img,format=raw,if=none,id=d0" -device ide-hd,drive=d0,bus=ide.0 \
    -nic "user,model=rtl8139,hostfwd=tcp:127.0.0.1:$port-:22" \
    -audiodev none,id=snd0 -device intel-hda -device hda-duplex,audiodev=snd0 \
    -display none -vga std -serial "file:$out/serial.log" -monitor none &
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
L "cat /proc/cmdline; grep -e ' / ' -e ' /data ' /proc/mounts; ls -l /data/trial2654" \
    | tee "$out/boot.txt"
status=0
L "sh /data/trial2654/probes/gpu.sh -r llvmpipe /data/trial2654/out" > "$out/gpu.out" 2>&1 || status=$?
cat "$out/gpu.out"
L 'tar -cf - -C /data/trial2654 out' | tar -xf - -C "$out" || true
L 'sync; poweroff' || true
left=120
while kill -0 "$qemu" 2> /dev/null && [ "$left" -gt 0 ]; do
    sleep 5
    left=$((left - 5))
done
[ "$left" -gt 0 ] || echo 'qemu: the guest did not power off; stopping it' >&2
echo "qemu: result: gpu.sh status $status"
exit "$status"
