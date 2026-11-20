#!/bin/sh
# debian-qemu.sh OUT: boot the Debian reference guest's prepared image
# (tests/graphics/debian-ref) in the machine qemu.sh gives Rach2, the
# i3-4030U's shape under KVM with Bochs VGA, on a throwaway overlay, with SSH
# on 127.0.0.1:PORT (26528) for the user gfx with the cache's key, and
# QEMU_EXTRA added to QEMU's words as qemu.sh adds it. OUT
# receives the serial log. The script waits for QEMU, so run it in the
# background and end the guest with `sudo poweroff` over SSH.
set -eu

[ $# -eq 1 ] || { echo 'usage: debian-qemu.sh OUT' >&2; exit 2; }
out=$1
here=$(cd "$(dirname "$0")" && pwd)
tree=$(cd "$here/../../.." && pwd)
common=$(git -C "$tree" rev-parse --path-format=absolute --git-common-dir)
cache=${DEBIAN_REF_CACHE:-$(dirname "$common")/build/cache/debian-ref}
port=${PORT:-26528}
work='' qemu=''
trap '[ -z "$qemu" ] || { kill "$qemu" 2> /dev/null || true; wait "$qemu" 2> /dev/null || true; }
      [ -z "$work" ] || rm -rf "$work"' EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM
mkdir -p "$out" "$tree/build/tmp"
work=$(mktemp -d "$tree/build/tmp/debian-t5527.XXXXXX")
qemu-img create -q -f qcow2 -F qcow2 -b "$cache/prepared.qcow2" "$work/overlay.qcow2"
: > "$out/serial.log"
qemu-system-x86_64 -accel kvm -cpu host,invtsc=on,topoext=on -smp 4,sockets=1,cores=2,threads=2 -m 8G \
    -machine q35 -nodefaults \
    -drive "file=$work/overlay.qcow2,format=qcow2,if=virtio,cache=unsafe" \
    -nic "user,model=virtio-net-pci,hostfwd=tcp:127.0.0.1:$port-:22" \
    -device virtio-rng-pci -vga "${VGA:-std}" ${QEMU_EXTRA:-} -display none -serial "file:$out/serial.log" -monitor none &
qemu=$!
wait "$qemu"
qemu=''
