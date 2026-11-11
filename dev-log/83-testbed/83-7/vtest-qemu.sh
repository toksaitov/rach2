#!/bin/sh
# vtest-qemu.sh DISK KERNEL VTEST OUT: 83-7.3's rehearsal of the vtest rung
# in QEMU. Boot KERNEL under KVM on an overlay of DISK, the GNU test disk,
# with VTEST, the bed-vtest suite's data disk, as a snapshot, a scratch disk
# that holds the run's files, an emulated VT-d and QEMU's edu device as the
# Intel driver's fake chip, as the Lenovo's trial has the GPU; then over SSH
# run vtest-rach2.sh twice, the guest under TCG, since Debian's cloud image
# does not finish its boot at the testing PC's third level of KVM
# (83-2.md): with -x, the harness's
# glmark2 and piglit through the relay on the guest's llvmpipe, and with
# -g, the fake chip's function given to the guest through gpu.sh and given
# back. OUT receives the serial log, run.txt, the runs' lines, and out/,
# the measurements.
set -eu

[ $# -eq 4 ] || { echo 'usage: vtest-qemu.sh DISK KERNEL VTEST OUT' >&2; exit 2; }
abs() { (cd "$(dirname "$1")" && echo "$PWD/$(basename "$1")"); }
disk=$(abs "$1") kernel=$(abs "$2") vtest=$(abs "$3")
mkdir -p "$4"
out=$(abs "$4")
here=$(cd "$(dirname "$0")" && pwd)
tree=$(cd "$here/../../.." && pwd)
key=$tree/krffs/rach2_demo_ed25519
port=${PORT:-26672}
work='' qemu=''
trap '[ -z "$qemu" ] || { kill "$qemu" 2> /dev/null || true; wait "$qemu" 2> /dev/null || true; }
      [ -z "$work" ] || rm -rf "$work"' EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM
mkdir -p "$tree/build/tmp"
work=$(mktemp -d "$tree/build/tmp/rach2-t2670v.XXXXXX")
qemu-img create -q -f qcow2 -F raw -b "$disk" "$work/disk.qcow2"
mkdir -p "$work/run"
cp "$tree/bed/fixture/vtest-rach2.sh" "$tree/tests/assign/gpu.sh" "$work/run/"
cp -r "$tree/bed/measure" "$work/run/measure"
cp -r "$tree/tests/graphics/runners" "$work/run/runners"
(cd "$work/run" && sha256sum "$kernel" "$vtest" vtest-rach2.sh gpu.sh measure/*) | tee "$out/inputs.txt"
truncate -s 24G "$work/scratch.img"
MKE2FS_CONFIG=$tree/vendor/e2fsprogs/mke2fs.conf mke2fs -q -t ext2 -b 4096 -F -d "$work/run" \
    "$work/scratch.img"
rm -rf "$work/run"

: > "$out/serial.log"
qemu-system-x86_64 -accel kvm -cpu host -smp 4 -m 8G -machine q35,kernel-irqchip=split \
    -device intel-iommu,intremap=on,caching-mode=on \
    -kernel "$kernel" -append 'rach2.root=ext2 rach2.i915fake' \
    -drive "file=$work/disk.qcow2,format=qcow2,if=none,id=d0" -device ahci,id=ahci \
    -device ide-hd,drive=d0,bus=ahci.0 \
    -drive "file=$vtest,format=raw,if=none,id=d1,snapshot=on" -device ide-hd,drive=d1,bus=ahci.1 \
    -drive "file=$work/scratch.img,format=raw,if=none,id=d2" -device ide-hd,drive=d2,bus=ahci.2 \
    -nic "user,model=e1000,hostfwd=tcp:127.0.0.1:$port-:22" \
    -device edu,addr=07.0,dma_mask=0xffffffffffffffff \
    -display none -vga none -device virtio-vga -serial "file:$out/serial.log" -monitor none &
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
L 'mkdir -p /tmp/vtest /tmp/run && mount -t ext2 -o ro /dev/disk1 /tmp/vtest && mount -t ext2 /dev/disk2 /tmp/run &&
   cat /proc/cmdline; readlink /sys/bus/pci/devices/0000:00:07.0/driver; ls /sys/bus/pci/devices/0000:00:07.0/drm;
   grep -E "^(iommu_|vfio_|msi_vectors_free|kvm_)" /proc/stat; df -k /tmp/run' | tee "$out/boot.txt"
status=0
L 'cd /tmp/run && TMPDIR=/tmp/run/tmp && mkdir -p $TMPDIR && export TMPDIR MEASURE_RUNNERS=/tmp/run/runners &&
   sh vtest-rach2.sh -a tcg -x /tmp/run/measure/measure.sh -o /tmp/run/out/x -t 5 /tmp/vtest; echo "vtest-qemu: -x: exit $?";
   sh vtest-rach2.sh -a tcg -g 0000:00:07.0 /tmp/vtest; echo "vtest-qemu: -g: exit $?"' \
    > "$out/run.txt" 2>&1 || status=$?
grep -E '^vtest-(rach2: (result|measure|handoff|record)|qemu)' "$out/run.txt" || true
L 'dmesg' > "$out/dmesg.txt" || true
L 'grep -E "^(iommu_|vfio_|msi_vectors_free|kvm_)" /proc/stat' > "$out/after.txt" || true
L 'tar -cf - -C /tmp/run out' | tar -xf - -C "$out" || true
L 'sync; umount /tmp/run; poweroff' || true
left=120
while kill -0 "$qemu" 2> /dev/null && [ "$left" -gt 0 ]; do
    sleep 5
    left=$((left - 5))
done
[ "$left" -gt 0 ] || echo 'qemu: the guest did not power off; stopping it' >&2
echo "qemu: result: ssh status $status"
exit "$status"
