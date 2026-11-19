#!/bin/sh
# qemu.sh ROOT KERNEL PAYLOAD OUT: 80-7b's rehearsal in QEMU of the Lenovo
# step. Lay ROOT, the trial root, on entry 6 of a new disk with the Lenovo's
# GUID table identity, boot KERNEL under KVM with the root image on a loop
# device as the Lenovo's trial boot names it, read the boot, run both eglinfo,
# and play the step's scripts from PAYLOAD, lenovo.sh and then free.sh, in
# their small form on the root's own desktop session; then power the guest
# off.
#
# The session draws on virtio-vga-gl with virgl, on the host's GPU through
# egl-headless. QEMU's edu device with rach2.i915fake adds the fake HD 4400,
# which has no connected connector, so the session keeps virtio's card while
# lenovo.sh finds the fake chip's gt_act_freq_mhz and pins and restores its
# minimum around the 64-bit games, as it does the Lenovo's. Frame times here
# say only that each game reached its end. OUT receives the serial log, the
# boot's readings, eglinfo's output, lenovo.sh's output and its directory,
# and the kernel's log. The guest is stopped when neither the serial log nor
# lenovo.sh's output has grown for 10 minutes.
set -eu

[ $# -eq 4 ] || { echo 'usage: qemu.sh ROOT KERNEL PAYLOAD OUT' >&2; exit 2; }
root=$1 kernel=$2 payload=$3 out=$4
here=$(cd "$(dirname "$0")" && pwd)
tree=$(cd "$here/../../.." && pwd)
key=$tree/krffs/rach2_demo_ed25519
guid=1acef00d-0f1e-4d2c-8b3a-c0ffee000001
port=${PORT:-26664}
work='' qemu='' play=''
trap '[ -z "$play" ] || { kill "$play" 2> /dev/null || true; wait "$play" 2> /dev/null || true; }
      [ -z "$qemu" ] || { kill "$qemu" 2> /dev/null || true; wait "$qemu" 2> /dev/null || true; }
      [ -z "$work" ] || rm -rf "$work"' EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM
mkdir -p "$out" "$tree/build/tmp"
work=$(mktemp -d "$tree/build/tmp/rach2-t2664.XXXXXX")

hash=$(sha256sum "$root" | cut -d' ' -f1)
image=root-$(echo "$hash" | cut -c1-16).img
echo "qemu: root $root sha256 $hash" | tee "$out/inputs.txt"
sha256sum "$kernel" "$payload" | tee -a "$out/inputs.txt"
mkdir -p "$work/data/trial"
ln "$root" "$work/data/trial/$image" 2> /dev/null || cp --sparse=always "$root" "$work/data/trial/$image"
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
line="rach2.rootimage=GPTID=$guid,6:/trial/$image rach2.slot=trialroot rach2.i915fake rach2.trace=enosys"
GALLIUM_DRIVER=d3d12 MESA_D3D12_DEFAULT_ADAPTER_NAME=NVIDIA \
qemu-system-x86_64 -accel kvm -cpu host,invtsc=on -smp 4 -m 8G -machine q35 \
    -kernel "$kernel" -append "$line" \
    -drive "file=$work/disk.img,format=raw,if=none,id=d0" -device ide-hd,drive=d0,bus=ide.0 \
    -nic "user,model=rtl8139,hostfwd=tcp:127.0.0.1:$port-:22" \
    -audiodev none,id=snd0 -device intel-hda -device hda-duplex,audiodev=snd0 \
    -vga none -device virtio-vga-gl,edid=on,xres=1366,yres=768 \
    -display egl-headless,rendernode=/dev/dri/renderD128 -device edu \
    -serial "file:$out/serial.log" -monitor none &
qemu=$!
L() { ssh -i "$key" -p "$port" -o BatchMode=yes -o ConnectTimeout=5 -o LogLevel=ERROR \
          -o UserKnownHostsFile=/dev/null -o StrictHostKeyChecking=no root@127.0.0.1 "$@"; }
# grown FILE...: whether any FILE grew since the last call; `still` counts
# the seconds since one did.
sizes=''
grown()
{
    now=$(cat "$@" 2> /dev/null | wc -c)
    if [ "$now" = "$sizes" ]; then still=$((still + 5)); else still=0 sizes=$now; fi
}
still=0
until L true 2> /dev/null; do
    kill -0 "$qemu" 2> /dev/null || { echo 'qemu: ended before SSH answered' >&2; exit 1; }
    grown "$out/serial.log"
    [ "$still" -lt 600 ] || { echo 'qemu: the boot stopped' >&2; tail -n 20 "$out/serial.log" >&2; exit 1; }
    sleep 5
done
L 'cat /proc/cmdline; uname -a; grep -e " / " -e " /data " /proc/mounts; ls -ln /dev/dri; ls /sys/class/drm; id desktop
   for p in supertuxkart supertuxkart.i386-linux-gnu pyrogenesis 0ad; do command -v $p || ls /usr/games/$p; done
   for f in /sys/class/drm/card*/gt_act_freq_mhz /sys/class/drm/card*/gt_min_freq_mhz; do echo "$f $(cat $f)"; done
   head -n 3 /sys/kernel/debug/bkl/calls; mixer vol' > "$out/boot.txt" 2>&1 || true
cat "$out/boot.txt"
L 'for p in eglinfo.i386-linux-gnu eglinfo; do su -s /bin/sh desktop -c "EGL_LOG_LEVEL=debug $p -B -p surfaceless" 2>&1; echo "exit $?"; done' \
    > "$out/eglinfo.txt" 2>&1 || true
grep -E 'pci id|OpenGL renderer|^exit' "$out/eglinfo.txt" || true
L 'rm -rf /tmp/graphics /tmp/t2664-out && mkdir -p /tmp/graphics && tar -xf - -C /tmp/graphics && chmod -R a+rX /tmp/graphics' < "$payload"
status=0
L 'sh /tmp/graphics/lenovo.sh -n 1 -s 640x400 -t 10 /tmp/t2664-out; s=$?
   sh /tmp/graphics/free.sh -s 640x400 -t 10 /tmp/t2664-out /tmp/t2664-out/cache || s=1; exit $s' \
    > "$out/lenovo.out" 2>&1 &
play=$!
still=0
while kill -0 "$play" 2> /dev/null; do
    grown "$out/serial.log" "$out/lenovo.out"
    if [ "$still" -ge 600 ]; then
        echo 'qemu: lenovo.sh made no progress for 10 minutes; stopping it' >&2
        kill "$play" 2> /dev/null || true
        break
    fi
    sleep 5
done
wait "$play" || status=$?
play=''
cat "$out/lenovo.out"
L 'for f in /sys/class/drm/card*/gt_min_freq_mhz; do echo "after $f $(cat $f)"; done; mixer vol; pgrep -x weston || echo "no weston"' \
    > "$out/after.txt" 2>&1 || true
cat "$out/after.txt"
L dmesg > "$out/dmesg.txt" 2>&1 || true
L 'tar -cf - --exclude=t2664-out/cache -C /tmp t2664-out' | tar -xf - -C "$out" || true
L 'sync; poweroff' || true
left=120
while kill -0 "$qemu" 2> /dev/null && [ "$left" -gt 0 ]; do
    sleep 5
    left=$((left - 5))
done
[ "$left" -gt 0 ] || echo 'qemu: the guest did not power off; stopping it' >&2
echo "qemu: result: lenovo.sh and free.sh status $status"
exit "$status"
