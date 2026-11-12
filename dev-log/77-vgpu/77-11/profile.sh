#!/bin/sh
# profile.sh: where a Venus guest's processor time goes during vkcube, on the
# testing PC, under KVM with Venus over lavapipe and QEMU's egl-headless
# display: 77-11's measurement, the machine of 77-10's wsl-cube.sh.
#
#   profile.sh rach2 KERNEL DISK OUT
#   profile.sh debian QCOW2 KEY OUT
#
# Rach2 boots KERNEL directly with DISK, a raw gnu-test.disk, as its root
# in a snapshot; Debian boots an overlay of QCOW2 and logs in as gfx with
# KEY. The guest runs one round of vkcube.sh to warm its caches, then one of
# FRAMES presents (3346). Around that round it keeps /proc/stat, /proc/vmstat
# where there is one, and each thread's stat line, /proc/stat and the threads'
# lines also 8 and 18 seconds into it, while vkcube runs; on Rach2, debugfs's
# bkl/calls, bkl/tick and bkl/profile in its whole-kernel mode, zeroed before
# the round, or at the second of those times when TAIL is 1; on Debian,
# `perf record -a -g` at 1000 Hz, with Debian's linux-perf installed first
# when it lacks perf, unless PERF is 0. OUT gets them in measured/, with the
# serial and QEMU logs and the guest's output. CPU (host) and SMP (4) are
# QEMU's processor model and count. TRACE, a file, gets QEMU's trace of each
# processor's exits and entries and of the virtio GPU's notifies, commands
# and fences, with their times, or of the events EVENTS names. STATS, a
# script such as qmpstats.py, gets QEMU's QMP socket, OUT's kvm-stats and
# QEMU's PID, and runs beside the guest. PHASES, a count, runs phases.py
# with it in place of the rounds, which times each phase of a round's start
# alone. The guest powers off on every exit path.
set -eu
system=$1 image=$2 extra=$3 out=$4
frames=${FRAMES:-3346}
tree=$(cd "$(dirname "$0")/../../.." && pwd)
mkdir -p "$out" "$tree/build/tmp"
TMPDIR=$tree/build/tmp
export TMPDIR
work='' qemu=''
trap 'if [ -n "$qemu" ]; then kill "$qemu" 2>/dev/null || true; wait "$qemu" 2>/dev/null || true; fi
      [ -z "$work" ] || rm -rf "$work"' EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM
work=$(mktemp -d "$TMPDIR/rach2-profile.XXXXXX")
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
    *) echo "usage: profile.sh rach2|debian ..." >&2; exit 2 ;;
esac
tracing=''
if [ -n "${TRACE:-}" ]; then
    events=${EVENTS:-'kvm_run_exit kvm_vcpu_ioctl virtio_queue_notify virtio_notify virtio_gpu_*'}
    set -f
    # shellcheck disable=SC2086 # a list of event patterns, not file names
    printf '%s\n' $events > "$work/events"
    set +f
    tracing="-msg timestamp=on -trace events=$work/events,file=$TRACE"
fi
GALLIUM_DRIVER=llvmpipe
VK_DRIVER_FILES=/usr/share/vulkan/icd.d/lvp_icd.json
LD_PRELOAD=/usr/lib/x86_64-linux-gnu/libm.so.6
export GALLIUM_DRIVER VK_DRIVER_FILES LD_PRELOAD
# shellcheck disable=SC2086 # boot and tracing are words
qemu-system-x86_64 -name "profile-$system" -accel kvm -cpu "${CPU:-host}" -smp "${SMP:-4}" -m 6G \
    -object memory-backend-memfd,id=mem,size=6G,share=on \
    -machine q35,memory-backend=mem -no-reboot \
    -vga none -device virtio-vga-gl,edid=on,xres=1280,yres=800,venus=on,blob=on,hostmem=4G \
    -display egl-headless,rendernode=/dev/dri/renderD128 -audiodev none,id=noaudio \
    -drive "$drive" -device ide-hd,drive=root,bus=ide.0 \
    -netdev "user,id=n0,hostfwd=tcp:127.0.0.1:$port-:22" -device e1000,netdev=n0 \
    -device virtio-rng-pci -device virtio-keyboard-pci -device virtio-tablet-pci \
    -serial "file:$out/serial.log" -monitor none -qmp "unix:$work/qmp,server=on,wait=off" \
    $boot $tracing > "$out/qemu.log" 2>&1 &
qemu=$!
if [ -n "${STATS:-}" ]; then
    python3 -I "$STATS" "$work/qmp" "$out/kvm-stats" "$qemu" &
fi
ssh="ssh -i $key -p $port -o BatchMode=yes -o ConnectTimeout=5 -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR $user@127.0.0.1"
tries=0
until $ssh true 2>/dev/null </dev/null; do
    kill -0 "$qemu" 2>/dev/null || { echo "profile: QEMU ended; read $out/qemu.log" >&2; exit 1; }
    tries=$((tries + 1))
    [ "$tries" -lt 120 ] || { echo "profile: SSH did not answer" >&2; exit 1; }
    sleep 2
done
(cd "$tree" && tar -czf - tests/graphics/runners tests/graphics/guest-env.sh dev-log/77-vgpu/77-11/phases.py) |
    $ssh "${privilege}mkdir -p /tmp/77-11 && ${privilege}tar -xzf - -C /tmp/77-11"
icd=/usr/share/vulkan/icd.d/venus_icd.x86_64.json
if [ -n "${PHASES:-}" ]; then
    # shellcheck disable=SC2016 # expanded in the guest
    $ssh "${privilege}sh -c 'cd /tmp/77-11 && mkdir -p build/tmp results/measured && TMPDIR=/tmp/77-11/build/tmp && export TMPDIR &&
        ( GRAPHICS_CLASS=performance GRAPHICS_DONE=1 MESA_SHADER_CACHE_DIR=\$TMPDIR/cache
          . ./tests/graphics/guest-env.sh; graphics_env > /tmp/graphics.env
          VK_DRIVER_FILES=$icd; export VK_DRIVER_FILES
          python3 -I dev-log/77-vgpu/77-11/phases.py tests/graphics/runners results/measured $PHASES )'" \
        > "$out/guest.log" 2>&1 || echo "profile: guest status $?" >> "$out/guest.log"
else
    # shellcheck disable=SC2016 # expanded in the guest
    $ssh "${privilege}sh -c 'cd /tmp/77-11 && mkdir -p build/tmp results/measured && TMPDIR=/tmp/77-11/build/tmp && export TMPDIR &&
        m=results/measured
        threads() { for f in /proc/[0-9]*/task/[0-9]*/stat; do cat \$f 2>/dev/null; done; }
        if [ ${PERF:-1} = 1 ] && [ ! -e /sys/kernel/debug/bkl ] && ! command -v perf > /dev/null; then
            DEBIAN_FRONTEND=noninteractive apt-get install -y -q linux-perf > results/measured/apt.log 2>&1 || true
        fi
        uname -a && for round in 1 2; do
            rm -f /tmp/graphics.env
            ( GRAPHICS_CLASS=performance GRAPHICS_DONE=1 MESA_SHADER_CACHE_DIR=\$TMPDIR/cache
              . ./tests/graphics/guest-env.sh; graphics_env > /tmp/graphics.env
              cube=tests/graphics/runners/vkcube.sh
              if [ \$round = 2 ]; then
                  sed s/^FRAMES=346/FRAMES=$frames/ \$cube > tests/graphics/runners/vkcube-long.sh
                  cube=tests/graphics/runners/vkcube-long.sh
                  if [ -e /sys/kernel/debug/bkl/calls ]; then
                      echo 0 > /sys/kernel/debug/bkl/calls; echo 0 > /sys/kernel/debug/bkl/tick
                      echo kernel > /sys/kernel/debug/bkl/profile
                  fi
                  threads > \$m/threads-before; cat /proc/stat > \$m/stat-before; cat /proc/vmstat > \$m/vmstat-before 2>/dev/null || true
                  ( sleep 8; threads > \$m/threads-mid1; cat /proc/stat > \$m/stat-mid1
                    sleep 10; threads > \$m/threads-mid2; cat /proc/stat > \$m/stat-mid2
                    if [ ${TAIL:-0} = 1 ] && [ -e /sys/kernel/debug/bkl/calls ]; then
                        echo 0 > /sys/kernel/debug/bkl/calls; echo 0 > /sys/kernel/debug/bkl/tick
                        echo kernel > /sys/kernel/debug/bkl/profile
                    fi ) &
                  if [ ${PERF:-1} = 1 ] && command -v perf > /dev/null; then
                      perf record -a -g -F 1000 -o \$m/perf.data > \$m/perf.log 2>&1 & perf=\$!
                  fi
              fi
              sh \$cube -i $icd -o results/cube-\$round
              if [ \$round = 2 ]; then
                  cat /proc/stat > \$m/stat-after; cat /proc/vmstat > \$m/vmstat-after 2>/dev/null || true; threads > \$m/threads-after
                  if [ -n \"\${perf:-}\" ]; then
                      kill -INT \$perf; wait \$perf || true
                      perf report -i \$m/perf.data --no-children --sort sym --stdio > \$m/perf-sym.txt 2>&1 || true
                      perf report -i \$m/perf.data --no-children --sort comm,sym --stdio -g none > \$m/perf-comm.txt 2>&1 || true
                      rm -f \$m/perf.data
                  fi
                  if [ -e /sys/kernel/debug/bkl/calls ]; then
                      cat /sys/kernel/debug/bkl/calls > \$m/calls
                      cat /sys/kernel/debug/bkl/tick > \$m/tick
                      cat /sys/kernel/debug/bkl/profile > \$m/profile
                  fi
              fi )
        done'" > "$out/guest.log" 2>&1 || echo "profile: guest status $?" >> "$out/guest.log"
fi
$ssh "${privilege}tar -czf - -C /tmp/77-11/results ." | tar -xzf - -C "$out"
$ssh "${privilege}poweroff -f" >/dev/null 2>&1 || true
wait "$qemu" 2>/dev/null || true
qemu=''
grep -h "vkcube: result" "$out/guest.log" || true
