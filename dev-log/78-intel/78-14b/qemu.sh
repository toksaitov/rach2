#!/bin/sh
# qemu.sh ROOT KERNEL CLIPS OUT: 78-14b's rehearsal in QEMU of the Lenovo
# step's trial boot. Lay ROOT, the trial root, and the browsers' six clips of
# CLIPS on entry 6 of a new disk with the Lenovo's GUID table identity, as its
# data partition holds them, boot KERNEL under KVM from the root image as the
# step's trial line names it, with the fake HD 4400 on QEMU's edu device in
# place of the chip, stage the step's kit in /tmp/t2661 as the step does, run
# vainfo as desktop and `gpuvideo.sh browsers` as root, and power off. The
# line has the Lenovo step's lockup words but softlockup_panic=0, below.
#
# The decode and hang checks need vahash and the clips Debian made, which only
# the Lenovo's data partition holds, and the fake chip reads no batch, so they
# do not run here. The sums the browsers check reads are data.sha256's lines
# of the six clips it plays and the digest of a stub vahash. OUT receives the
# serial log, the boot's state, vainfo's output and gpuvideo.sh's; the judging
# against Linux's plays is printed but not the rehearsal's verdict, since the
# fake chip draws nothing. The last lines are `qemu: soft lockups: N` and
# `qemu: result: gpuvideo.sh status S`.
set -eu

[ $# -eq 4 ] || { echo 'usage: qemu.sh ROOT KERNEL CLIPS OUT' >&2; exit 2; }
root=$1 kernel=$2 clips=$3 out=$4
here=$(cd "$(dirname "$0")" && pwd)
tree=$(cd "$here/../../.." && pwd)
key=$tree/krffs/rach2_demo_ed25519
guid=1acef00d-0f1e-4d2c-8b3a-c0ffee000001
port=${PORT:-26561}
PLAY="h264-720p.mp4 h264-1080p.mp4 vp9-720p.webm vp9-1080p.webm av1-720p.webm av1-1080p.webm"
work='' qemu=''
trap '[ -z "$qemu" ] || { kill "$qemu" 2> /dev/null || true; wait "$qemu" 2> /dev/null || true; }
      [ -z "$work" ] || rm -rf "$work"' EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM
mkdir -p "$out" "$tree/build/tmp"
work=$(mktemp -d "$tree/build/tmp/rach2-t2661.XXXXXX")

hash=$(sha256sum "$root" | cut -d' ' -f1)
image=root-$(echo "$hash" | cut -c1-16).img
echo "qemu: root $root sha256 $hash" | tee "$out/inputs.txt"
sha256sum "$kernel" | tee -a "$out/inputs.txt"
mkdir -p "$work/data/trial2661" "$work/data/native/video/clips" "$work/kit"
ln "$root" "$work/data/trial2661/$image" 2> /dev/null || cp --sparse=always "$root" "$work/data/trial2661/$image"
for f in $PLAY; do
    cp "$clips/$f" "$work/data/native/video/clips/"
done
# gpuvideo.sh makes vahash executable whatever the check; the browsers check
# never runs it, so a stub stands in for Debian's build.
printf '#!/bin/sh\necho "vahash: the rehearsal has no vahash" >&2\nexit 1\n' > "$work/data/native/video/clips/vahash"

# The kit, as gpu.cases gives video/browsers its workspace.
for f in tests/native/gpuvideo.sh tests/graphics/lenovo/video-gpu.sh tests/graphics/guest-env.sh \
    tests/graphics/runners/video.sh tests/graphics/runners/common.sh tests/graphics/runners/server.py \
    tests/graphics/runners/cdp.py tests/graphics/runners/events.py tests/graphics/runners/video.html \
    tests/graphics/runners/clips.sha256 tests/graphics/runners/judge.py tests/graphics/runners/video.limits \
    tests/graphics/ref/lenovo-video/linux.tar.gz tests/graphics/ref/lenovo-video/spread; do
    cp "$tree/$f" "$work/kit/"
done
for f in $PLAY; do
    grep "  $f\$" "$tree/tests/graphics/ref/lenovo-video/data.sha256"
done > "$work/kit/data.sha256"
(cd "$work/data/native/video/clips" && sha256sum vahash) >> "$work/kit/data.sha256"
(cd "$work/data/native/video/clips" && sha256sum -c "$work/kit/data.sha256") | tee -a "$out/inputs.txt"

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
line="rach2.rootimage=GPTID=$guid,6:/trial2661/$image rach2.slot=trialroot rach2.i915fake"
# softlockup_panic=0, where the Lenovo's line has 1: on the fake chip the
# runtime suspend's sideband waits never end, and the guest stalls for seconds
# while they run (78-14b.md, Preparation), which a panic would turn into a
# reset that loses the check.
line="$line nmi_watchdog=panic softlockup_panic=0 rach2.trace=enosys"
qemu-system-x86_64 -accel kvm -cpu host,invtsc=on -smp 4 -m 8G -machine q35 \
    -kernel "$kernel" -append "$line" \
    -drive "file=$work/disk.img,format=raw,if=none,id=d0" -device ide-hd,drive=d0,bus=ide.0 \
    -nic "user,model=rtl8139,hostfwd=tcp:127.0.0.1:$port-:22" -device edu \
    -audiodev none,id=snd0 -device intel-hda -device hda-duplex,audiodev=snd0 \
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
# shellcheck disable=SC2016 # the guest's shell expands them
L 'cat /proc/cmdline; uname -a; grep -e " / " -e " /data " /proc/mounts; ls -ln /dev/dri; id desktop;
   ls /usr/lib/x86_64-linux-gnu/dri/i965_drv_video.so /usr/lib/x86_64-linux-gnu/piglit/bin/read-front;
   echo 1 > /proc/sys/kernel/hung_task_panic;
   for f in nmi_watchdog softlockup_panic hardlockup_panic hung_task_panic panic; do
       echo "$f $(cat /proc/sys/kernel/$f)"; done' 2>&1 | tee "$out/boot.txt"
L "su -s /bin/sh desktop -c 'vainfo --display drm --device /dev/dri/renderD128' 2>&1; echo \"exit \$?\"" \
    > "$out/vainfo.txt" 2>&1 || true
grep -E 'Driver version|VAEntrypointVLD|^exit' "$out/vainfo.txt" || true
tar -cf - -C "$work/kit" . | L 'rm -rf /tmp/t2661 && mkdir -p /tmp/t2661 && tar -xf - -C /tmp/t2661'
status=0
L 'cd /tmp/t2661 && sh gpuvideo.sh browsers data.sha256 linux.tar.gz spread' > "$out/browsers.out" 2>&1 ||
    status=$?
cat "$out/browsers.out"
L 'tar -cf - -C /tmp/t2661 rach2' | tar -xf - -C "$out" || true
L 'dmesg' > "$out/dmesg.txt" 2>&1 || true
L 'sync; poweroff' || true
left=120
while kill -0 "$qemu" 2> /dev/null && [ "$left" -gt 0 ]; do
    sleep 5
    left=$((left - 5))
done
[ "$left" -gt 0 ] || echo 'qemu: the guest did not power off; stopping it' >&2
echo "qemu: soft lockups: $(grep -c 'watchdog: BUG: soft lockup' "$out/serial.log" || true)"
echo "qemu: result: gpuvideo.sh status $status"
exit "$status"
