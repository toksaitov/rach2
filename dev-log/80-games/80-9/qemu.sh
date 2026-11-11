#!/bin/sh
# qemu.sh ROOT BUNDLE KERNEL GAME OUT: task 80-9's rehearsal in QEMU. Lay
# ROOT, the trial root, and BUNDLE, the Wine probes' payload with crysis.py,
# on entry 6 of a new disk with the Lenovo's GUID table identity, boot KERNEL
# under KVM with the root image as the Lenovo's trial boot names it, on virgl
# with HDA, mount GAME, crysis-game.ext2, at /data/crysis/game through a
# throwaway overlay, and run the Lenovo step's commands of crysis.py over
# SSH, then power the guest off; STEPS, steps separated by `|`, runs those
# instead of the whole procedure. OUT receives the serial log, the boot's
# mounts and each command's output and files. The rates are not judged
# (CRYSIS_GATE=0): virgl on the testing PC is not the Lenovo's GPU.
set -eu

[ $# -eq 5 ] || { echo 'usage: qemu.sh ROOT BUNDLE KERNEL GAME OUT' >&2; exit 2; }
root=$1 bundle=$2 kernel=$3 game=$4 out=$5
here=$(cd "$(dirname "$0")" && pwd)
tree=$(cd "$here/../../.." && pwd)
key=$tree/krffs/rach2_demo_ed25519
guid=1acef00d-0f1e-4d2c-8b3a-c0ffee000001
port=${PORT:-26541}
work='' qemu=''
trap '[ -z "$qemu" ] || { kill "$qemu" 2> /dev/null || true; wait "$qemu" 2> /dev/null || true; }
      [ -z "$work" ] || rm -rf "$work"' EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM
mkdir -p "$out" "$tree/build/tmp"
work=$(mktemp -d "$tree/build/tmp/rach2-t2665.XXXXXX")

hash=$(sha256sum "$root" | cut -d' ' -f1)
image=root-$(echo "$hash" | cut -c1-16).img
echo "qemu: root $root sha256 $hash" | tee "$out/inputs.txt"
sha256sum "$bundle" "$kernel" | tee -a "$out/inputs.txt"
mkdir -p "$work/data/trial2665/probes" "$work/data/crysis/game"
ln "$root" "$work/data/trial2665/$image" 2> /dev/null ||
    cp --sparse=always "$root" "$work/data/trial2665/$image"
tar -xf "$bundle" -C "$work/data/trial2665/probes"
# Entry 6 from 1 MiB, made with Rach2's mke2fs defaults as the Lenovo's was;
# the table is written after the file system, which mke2fs would wipe.
truncate -s 41G "$work/disk.img"
MKE2FS_CONFIG=$tree/vendor/e2fsprogs/mke2fs.conf mke2fs -q -t ext2 -b 4096 -F -E offset=1048576 \
    -L Rach2_data -d "$work/data" "$work/disk.img" 10485760
sgdisk -o -U "$guid" -n 6:2048:+40G -t 6:8300 -c 6:'Rach2 data' "$work/disk.img"
sgdisk -p "$work/disk.img" | tee -a "$out/inputs.txt"
rm -rf "$work/data"
echo "qemu: data partition made"

: > "$out/serial.log"
line="rach2.rootimage=GPTID=$guid,6:/trial2665/$image rach2.slot=trialroot rach2.trace=enosys"
# QEMU's GL on the testing PC's GPU, as etude's virgl profile puts it.
GALLIUM_DRIVER=d3d12 MESA_D3D12_DEFAULT_ADAPTER_NAME=NVIDIA \
qemu-system-x86_64 -accel kvm -cpu host,invtsc=on -smp 4 -m 8G -machine q35 \
    -kernel "$kernel" -append "$line" \
    -drive "file=$work/disk.img,format=raw,if=none,id=d0" -device ide-hd,drive=d0,bus=ide.0 \
    -drive "file=$game,format=raw,if=none,id=d1,snapshot=on" -device ide-hd,drive=d1,bus=ide.1 \
    -nic "user,model=rtl8139,hostfwd=tcp:127.0.0.1:$port-:22" \
    -audiodev none,id=snd0 -device intel-hda -device hda-duplex,audiodev=snd0 \
    -vga none -device virtio-vga-gl,edid=on,xres=1366,yres=768 \
    -display egl-headless,rendernode=/dev/dri/renderD128 \
    -serial "file:$out/serial.log" -monitor none &
qemu=$!
L() { ssh -i "$key" -p "$port" -o BatchMode=yes -o ConnectTimeout=5 -o LogLevel=ERROR \
          -o ServerAliveInterval=15 -o UserKnownHostsFile=/dev/null \
          -o StrictHostKeyChecking=no root@127.0.0.1 "$@"; }
# Wait for SSH while the serial log grows; stop when it has not grown for 5 min.
size=0 still=0
until L true 2> /dev/null; do
    kill -0 "$qemu" 2> /dev/null || { echo 'qemu: ended before SSH answered' >&2; exit 1; }
    now=$(wc -c < "$out/serial.log")
    if [ "$now" -eq "$size" ]; then still=$((still + 5)); else still=0 size=$now; fi
    [ "$still" -lt 300 ] || { echo 'qemu: the boot stopped' >&2; tail -n 20 "$out/serial.log" >&2; exit 1; }
    sleep 5
done
L "cat /proc/cmdline; grep -e ' / ' -e ' /data ' /proc/mounts; ls -l /data/trial2665" \
    | tee "$out/boot.txt"
L 'mount -t ext2 /dev/disk1 /data/crysis/game && mkdir -p /data/crysis/saves && df -k /data /data/crysis/game' \
    | tee -a "$out/boot.txt"
P='CRYSIS_GATE=0 python3 /data/trial2665/probes/crysis.py'
status=0
# Each step as the Lenovo's procedure runs it; a failed step does not stop
# the rest, and its status is kept.
all='prepare /data/trial2665/out|flyby direct direct|flyby umu umu 1|flyby 32 bits32 1|save s1'
all="$all|load l-old default_accept_s1.CRYSISJMSF umu"
all="$all|load l-new default_accept_s1.CRYSISJMSF direct /data/crysis/compat-new"
all="$all|hud h1 default_accept_s1.CRYSISJMSF umu|restore"
IFS='|'
set -f
# shellcheck disable=SC2086 # the steps are split at `|` alone
set -- ${STEPS:-$all}
set +f
unset IFS
for step in "$@"; do
    name=$(echo "$step" | tr ' /' '__')
    echo "qemu: step: $step at $(date +%T)"
    s=0
    L "$P $step" > "$out/$name.out" 2>&1 || s=$?
    cat "$out/$name.out"
    echo "qemu: step: $step: status $s at $(date +%T)" | tee -a "$out/steps.txt"
    [ "$s" -eq 0 ] || status=1
done
L 'umount /data/crysis/game; tar -cf - -C /data/trial2665 out' | tar -xf - -C "$out" || true
L 'sync; poweroff' || true
left=120
while kill -0 "$qemu" 2> /dev/null && [ "$left" -gt 0 ]; do
    sleep 5
    left=$((left - 5))
done
[ "$left" -gt 0 ] || echo 'qemu: the guest did not power off; stopping it' >&2
echo "qemu: result: status $status"
exit "$status"
