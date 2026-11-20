#!/bin/sh
# qemu.sh KERNEL DISK OUT: boot the GNU root of 5527's reproduction under KVM
# in the Lenovo i3-4030U's shape, two cores of two threads and 8 GiB, on a
# throwaway overlay of DISK, with the firmware-style display that VGA names
# (std: Bochs VGA, as fbdrm or bochs drive it) and SSH on 127.0.0.1:PORT
# (26527) for root with krffs/rach2_demo_ed25519. EXTRA is appended to the
# kernel's line. With GRUB naming a disk of tools/image/mkdisk_grub.sh whose
# menu names the root by MBRID=00000000,1, the machine boots that disk by
# BIOS instead, KERNEL unused, so that GRUB sets a VBE mode and fbdrm drives
# it under `nomodeset`, as on the Lenovo. QEMU_EXTRA is added to QEMU's
# words, such as `-global VGA.refresh_rate=60000`. OUT receives the serial
# log. The script waits for QEMU, so run it in the background and end the
# guest with `poweroff -f` over SSH.
set -eu

[ $# -eq 3 ] || { echo 'usage: qemu.sh KERNEL DISK OUT' >&2; exit 2; }
kernel=$1 disk=$2 out=$3
here=$(cd "$(dirname "$0")" && pwd)
tree=$(cd "$here/../../.." && pwd)
port=${PORT:-26527}
work='' qemu=''
trap '[ -z "$qemu" ] || { kill "$qemu" 2> /dev/null || true; wait "$qemu" 2> /dev/null || true; }
      [ -z "$work" ] || rm -rf "$work"' EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM
mkdir -p "$out" "$tree/build/tmp"
work=$(mktemp -d "$tree/build/tmp/rach2-t5527.XXXXXX")
qemu-img create -q -f qcow2 -b "$(cd "$(dirname "$disk")" && pwd)/${disk##*/}" -F raw "$work/overlay.qcow2"
: > "$out/serial.log"
if [ -n "${GRUB:-}" ]; then
    set -- -drive "file=$GRUB,format=raw,if=none,id=boot" -device virtio-blk-pci,drive=boot,bootindex=0
else
    set -- -kernel "$kernel" -append "rach2.root=ext2 ${EXTRA:-}"
fi
qemu-system-x86_64 -accel kvm -cpu host,invtsc=on,topoext=on -smp 4,sockets=1,cores=2,threads=2 -m 8G \
    -machine q35 "$@" \
    -drive "file=$work/overlay.qcow2,format=qcow2,if=virtio,cache=unsafe" \
    -nic "user,model=virtio-net-pci,hostfwd=tcp:127.0.0.1:$port-:22" \
    -vga "${VGA:-std}" ${QEMU_EXTRA:-} -display none -serial "file:$out/serial.log" -monitor none &
qemu=$!
wait "$qemu"
qemu=''
