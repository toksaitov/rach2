#!/bin/sh
# qemu.sh DISK KERNEL OUT [SECONDS]: 83-7.1's check of the measurement
# harness in QEMU. Boot KERNEL under KVM on an overlay of DISK, the GNU test
# disk, with virtio-vga's 2D display, as the graphics suites' virtio-vga
# profile has, so that every scene draws on llvmpipe, the rung
# native-rach2; copy bed/measure and the graphics runners in over SSH, run
# each of the harness's four scenes, or those SCENES names, once as the
# desktop user, each SECONDS long (5), and power the guest off. APPEND
# adds words to the boot line, as `rach2.notsc` makes every clock call read
# the HPET. OUT receives the serial log, each scene's directory and run.txt,
# the harness's lines.
set -eu

[ $# -ge 3 ] || { echo 'usage: qemu.sh DISK KERNEL OUT [SECONDS]' >&2; exit 2; }
abs() { (cd "$(dirname "$1")" && echo "$PWD/$(basename "$1")"); }
disk=$(abs "$1") kernel=$(abs "$2") seconds=${4:-5}
mkdir -p "$3"
out=$(abs "$3")
here=$(cd "$(dirname "$0")" && pwd)
tree=$(cd "$here/../../.." && pwd)
key=$tree/krffs/rach2_demo_ed25519
port=${PORT:-26670}
work='' qemu=''
trap '[ -z "$qemu" ] || { kill "$qemu" 2> /dev/null || true; wait "$qemu" 2> /dev/null || true; }
      [ -z "$work" ] || rm -rf "$work"' EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM
mkdir -p "$tree/build/tmp"
work=$(mktemp -d "$tree/build/tmp/rach2-t2670.XXXXXX")
qemu-img create -q -f qcow2 -F raw -b "$disk" "$work/disk.qcow2"
sha256sum "$kernel" | tee "$out/inputs.txt"
: > "$out/serial.log"
qemu-system-x86_64 -accel kvm -cpu host -smp 4 -m 8G -machine q35 \
    -kernel "$kernel" -append "rach2.root=ext2${APPEND:+ $APPEND}" \
    -drive "file=$work/disk.qcow2,format=qcow2,if=none,id=d0" -device ahci,id=ahci \
    -device ide-hd,drive=d0,bus=ahci.0 \
    -nic "user,model=e1000,hostfwd=tcp:127.0.0.1:$port-:22" \
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
tar -cf - -C "$tree/bed" measure -C "$tree/tests/graphics" runners |
    L 'rm -rf /tmp/t2670 && mkdir -p /tmp/t2670 && tar -xf - -C /tmp/t2670'
status=0
for scene in ${SCENES:-glmark2 piglit webgl game}; do
    L "cd /tmp/t2670 && MEASURE_RUNNERS=/tmp/t2670/runners sh measure/measure.sh -r native-rach2 \
           -u desktop -t $seconds -o /tmp/t2670/out/$scene $scene; echo \"measure: $scene: exit \$?\"" \
        >> "$out/run.txt" 2>&1 || status=$?
done
cat "$out/run.txt"
L 'cat /proc/cmdline; uname -a; grep -E "^(kvm_exits|virtio_gpu_copied_bytes)" /proc/stat' > "$out/after.txt" || true
L 'tar -cf - -C /tmp/t2670 out' | tar -xf - -C "$out" || true
L 'sync; poweroff' || true
left=120
while kill -0 "$qemu" 2> /dev/null && [ "$left" -gt 0 ]; do
    sleep 5
    left=$((left - 5))
done
[ "$left" -gt 0 ] || echo 'qemu: the guest did not power off; stopping it' >&2
echo "qemu: result: ssh status $status"
exit "$status"
