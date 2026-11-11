#!/bin/sh
# qemu.sh ROOT KERNEL VMDISK OUT: 83-5b's rehearsal in QEMU. Lay ROOT, the
# trial root, VMDISK, the test bed's VM, as qcow2, and the run's files on
# entry 6 of a new disk with the Lenovo's GUID table identity, as the
# Lenovo's data partition will hold them; boot KERNEL under KVM with the
# root image as the Lenovo's trial boot names it, an emulated VT-d, and
# QEMU's edu device as the Intel driver's fake chip in the GPU's place; and
# run the Lenovo step's commands over SSH: the VM's disk made raw, then
# bed-shm.sh with -k and -g, which gives the fake chip's function to the VM
# through gpu.sh. OUT receives the serial log, the boot's record, the run's
# lines and its directory. The caller reads boot.txt and run.txt.
set -eu

[ $# -eq 4 ] || { echo 'usage: qemu.sh ROOT KERNEL VMDISK OUT' >&2; exit 2; }
abs() { (cd "$(dirname "$1")" && echo "$PWD/$(basename "$1")"); }
root=$(abs "$1") kernel=$(abs "$2") vmdisk=$(abs "$3")
mkdir -p "$4"
out=$(abs "$4")
here=$(cd "$(dirname "$0")" && pwd)
tree=$(cd "$here/../../.." && pwd)
key=$tree/krffs/rach2_demo_ed25519
guid=1acef00d-0f1e-4d2c-8b3a-c0ffee000001
port=${PORT:-26568}
work= qemu=
trap '[ -z "$qemu" ] || { kill "$qemu" 2> /dev/null || true; wait "$qemu" 2> /dev/null || true; }
      [ -z "$work" ] || rm -rf "$work"' EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM
mkdir -p "$out" "$tree/build/tmp"
work=$(mktemp -d "$tree/build/tmp/rach2-t2668.XXXXXX")

hash=$(sha256sum "$root" | cut -d' ' -f1)
image=root-$(echo "$hash" | cut -c1-16).img
echo "qemu: root $root sha256 $hash" | tee "$out/inputs.txt"
mkdir -p "$work/data/t2668/run"
ln "$root" "$work/data/t2668/$image" 2> /dev/null ||
    cp --sparse=always "$root" "$work/data/t2668/$image"
qemu-img convert -O qcow2 "$vmdisk" "$work/data/t2668/vm.qcow2"
cp "$tree/tests/kvm/bed-shm.sh" "$tree/tests/assign/gpu.sh" "$tree/tests/kvm/nocopy.c" \
    "$tree/build/x86_64/tests/vgshmprobe" "$work/data/t2668/run/"
(cd "$work/data/t2668" && sha256sum "$kernel" vm.qcow2 run/*) | tee -a "$out/inputs.txt"
# Entry 6 from 1 MiB, 48 GiB, made with Rach2's mke2fs defaults as the
# Lenovo's was (dev-log/76-display/lenovo-storage.md). The table is written
# after the file system, since mke2fs wipes the signatures it finds.
truncate -s 49G "$work/disk.img"
MKE2FS_CONFIG=$tree/vendor/e2fsprogs/mke2fs.conf mke2fs -q -t ext2 -b 4096 -F -E offset=1048576 \
    -L Rach2_data -d "$work/data" "$work/disk.img" 12582912
sgdisk -o -U "$guid" -n 6:2048:+48G -t 6:8300 -c 6:'Rach2 data' "$work/disk.img" > /dev/null
rm -rf "$work/data"
echo "qemu: data partition made"

: > "$out/serial.log"
line="rach2.rootimage=GPTID=$guid,6:/t2668/$image rach2.slot=trialroot rach2.i915fake"
qemu-system-x86_64 -accel kvm -cpu host -smp 4 -m 8G -machine q35,kernel-irqchip=split \
    -device intel-iommu,intremap=on,caching-mode=on \
    -kernel "$kernel" -append "$line" \
    -drive "file=$work/disk.img,format=raw,if=none,id=d0" -device ide-hd,drive=d0,bus=ide.0 \
    -nic "user,model=e1000,hostfwd=tcp:127.0.0.1:$port-:22" \
    -device edu,addr=07.0,dma_mask=0xffffffffffffffff \
    -display none -vga std -serial "file:$out/serial.log" -monitor none &
qemu=$!
L() { ssh -i "$key" -p "$port" -o BatchMode=yes -o ConnectTimeout=5 -o LogLevel=ERROR \
          -o ServerAliveInterval=15 -o ServerAliveCountMax=8 \
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
L 'cat /proc/cmdline; grep -e " / " -e " /data " /proc/mounts; qemu-system-x86_64 --version | head -n 1;
   readlink /sys/bus/pci/devices/0000:00:07.0/driver; ls /sys/bus/pci/devices/0000:00:07.0/drm;
   grep -E "^(iommu_|vfio_|msi_vectors_free|kvm_)" /proc/stat' | tee "$out/boot.txt"
L 'cd /data/t2668 && qemu-img convert -O raw vm.qcow2 vm.disk && ls -ls vm.disk && df -k /data' |
    tee -a "$out/boot.txt"
status=0
L 'cd /data/t2668/run && sh bed-shm.sh -d /data/t2668/vm.disk -p ./vgshmprobe -k ./nocopy.c \
       -g 0000:00:07.0 -o /data/t2668/out; echo "exit $?"' > "$out/run.txt" 2>&1 || status=$?
cat "$out/run.txt"
L 'dmesg' > "$out/dmesg.txt" || true
L 'grep -E "^(iommu_|vfio_|msi_vectors_free|kvm_)" /proc/stat' > "$out/after.txt" || true
L 'tar -cf - -C /data/t2668 out' | tar -xf - -C "$out" || true
L 'sync; poweroff' || true
left=120
while kill -0 "$qemu" 2> /dev/null && [ "$left" -gt 0 ]; do
    sleep 5
    left=$((left - 5))
done
[ "$left" -gt 0 ] || echo 'qemu: the guest did not power off; stopping it' >&2
echo "qemu: result: ssh status $status"
exit "$status"
