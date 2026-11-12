#!/bin/sh
# wsl-cube.sh: run vkcube's three rounds of 77-10 in one guest on the testing
# PC, under KVM with Venus over lavapipe and QEMU's egl-headless display.
#
#   wsl-cube.sh rach2 KERNEL DISK OUT [TRACE]
#   wsl-cube.sh debian QCOW2 KEY OUT [TRACE]
#
# The machine is session.py's, but for the display, egl-headless on the
# render node, and the host's environment of the venus profile
# (tests/graphics/Readme.md, Venus on the testing PC). Rach2 boots KERNEL
# directly with DISK, a raw gnu-test.disk, as its root in a snapshot; Debian
# boots an overlay of QCOW2 and logs in as gfx with KEY. TRACE, a file,
# gets QEMU's virtio_gpu fence and flush trace events with their times. OUT
# gets cube-1 to cube-3, the serial and QEMU logs and the guest's output.
# CPU (host) is QEMU's processor model, SMP (4) its processors. With
# PROFILE=1, /proc/stat is kept before and after rounds 2 and 3 in OUT/bkl-2
# and OUT/bkl-3, with Rach2's debugfs counts of the big lock, bkl/calls and
# bkl/profile, zeroed before each and kept after it, and round 3 presents
# 3346 frames, not 346, so that the difference of the two is 3000 frames'
# worth. The guest powers off at the end, on every exit path.
set -eu
system=$1 image=$2 extra=$3 out=$4 trace=${5:-}
tree=$(cd "$(dirname "$0")/../../.." && pwd)
mkdir -p "$out" "$tree/build/tmp"
TMPDIR=$tree/build/tmp
export TMPDIR
work= qemu=
trap 'if [ -n "$qemu" ]; then kill "$qemu" 2>/dev/null || true; wait "$qemu" 2>/dev/null || true; fi
      [ -z "$work" ] || rm -rf "$work"' EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM
work=$(mktemp -d "$TMPDIR/rach2-wsl-cube.XXXXXX")
port=$(python3 -I -c 'import socket; s=socket.socket(); s.bind(("127.0.0.1",0)); print(s.getsockname()[1])')
case $system in
    rach2)
        key=$tree/krffs/rach2_demo_ed25519 user=root privilege=
        drive="file=$extra,if=none,id=root,format=raw,snapshot=on"
        boot="-kernel $image -append rach2.root=ext2"
        ;;
    debian)
        key=$extra user=gfx privilege="sudo "
        qemu-img create -q -f qcow2 -F qcow2 -b "$image" "$work/guest.qcow2"
        drive="file=$work/guest.qcow2,if=none,id=root,format=qcow2"
        boot=
        ;;
    *) echo "usage: wsl-cube.sh rach2|debian ..." >&2; exit 2 ;;
esac
tracing=
if [ -n "$trace" ]; then
    printf '%s\n' virtio_gpu_fence_ctrl virtio_gpu_fence_resp virtio_gpu_cmd_res_flush \
        virtio_gpu_cmd_ctx_submit virtio_gpu_cmd_set_scanout virtio_gpu_cmd_set_scanout_blob \
        virtio_queue_notify virtio_notify \
        > "$work/events"
    tracing="-msg timestamp=on -trace events=$work/events,file=$trace"
fi
GALLIUM_DRIVER=llvmpipe
VK_DRIVER_FILES=/usr/share/vulkan/icd.d/lvp_icd.json
LD_PRELOAD=/usr/lib/x86_64-linux-gnu/libm.so.6
export GALLIUM_DRIVER VK_DRIVER_FILES LD_PRELOAD
# shellcheck disable=SC2086 # boot and tracing are words
qemu-system-x86_64 -name "wsl-cube-$system" -accel kvm -cpu "${CPU:-host}" -smp "${SMP:-4}" -m 6G \
    -object memory-backend-memfd,id=mem,size=6G,share=on \
    -machine q35,memory-backend=mem -no-reboot \
    -vga none -device virtio-vga-gl,edid=on,xres=1280,yres=800,venus=on,blob=on,hostmem=4G \
    -display egl-headless,rendernode=/dev/dri/renderD128 -audiodev none,id=noaudio \
    -drive "$drive" -device ide-hd,drive=root,bus=ide.0 \
    -netdev "user,id=n0,hostfwd=tcp:127.0.0.1:$port-:22" -device e1000,netdev=n0 \
    -device virtio-rng-pci -device virtio-keyboard-pci -device virtio-tablet-pci \
    -serial "file:$out/serial.log" -monitor none $boot $tracing > "$out/qemu.log" 2>&1 &
qemu=$!
ssh="ssh -i $key -p $port -o BatchMode=yes -o ConnectTimeout=5 -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR $user@127.0.0.1"
tries=0
until $ssh true 2>/dev/null </dev/null; do
    kill -0 "$qemu" 2>/dev/null || { echo "wsl-cube: QEMU ended; read $out/qemu.log" >&2; exit 1; }
    tries=$((tries + 1))
    [ "$tries" -lt 120 ] || { echo "wsl-cube: SSH did not answer" >&2; exit 1; }
    sleep 2
done
(cd "$tree" && tar -czf - tests/graphics/runners tests/graphics/guest-env.sh) |
    $ssh "${privilege}mkdir -p /tmp/77-10 && ${privilege}tar -xzf - -C /tmp/77-10"
icd=/usr/share/vulkan/icd.d/venus_icd.x86_64.json
# shellcheck disable=SC2016 # expanded in the guest
$ssh "${privilege}sh -c 'cd /tmp/77-10 && mkdir -p build/tmp results && TMPDIR=/tmp/77-10/build/tmp && export TMPDIR &&
    uname -a && for round in 1 2 3; do
        rm -f /tmp/graphics.env
        ( GRAPHICS_CLASS=performance GRAPHICS_DONE=1 MESA_SHADER_CACHE_DIR=\$TMPDIR/cache-\$round
          . ./tests/graphics/guest-env.sh; graphics_env > /tmp/graphics.env
          cube=tests/graphics/runners/vkcube.sh
          if [ \$round != 1 ] && [ ${PROFILE:-0} = 1 ]; then
              if [ \$round = 3 ]; then
                  sed s/^FRAMES=346/FRAMES=3346/ \$cube > tests/graphics/runners/vkcube-long.sh
                  cube=tests/graphics/runners/vkcube-long.sh
              fi
              if [ -e /sys/kernel/debug/bkl/calls ]; then
                  echo 0 > /sys/kernel/debug/bkl/calls; echo 0 > /sys/kernel/debug/bkl/profile
              fi
              mkdir -p results/bkl-\$round; cat /proc/stat > results/bkl-\$round/stat-before
          fi
          sh \$cube -i $icd -o results/cube-\$round
          if [ \$round != 1 ] && [ ${PROFILE:-0} = 1 ]; then
              cat /proc/stat > results/bkl-\$round/stat-after
              if [ -e /sys/kernel/debug/bkl/calls ]; then
                  cat /sys/kernel/debug/bkl/calls > results/bkl-\$round/calls
                  cat /sys/kernel/debug/bkl/profile > results/bkl-\$round/profile
              fi
          fi )
    done'" > "$out/guest.log" 2>&1 || echo "wsl-cube: guest status $?" >> "$out/guest.log"
$ssh "${privilege}tar -czf - -C /tmp/77-10/results ." | tar -xzf - -C "$out"
$ssh "${privilege}poweroff -f" >/dev/null 2>&1 || true
wait "$qemu" 2>/dev/null || true
qemu=
grep -h "vkcube: result" "$out/guest.log" || true
