#!/bin/sh
# virgl-lost.sh: what OpenGL programs and Weston see when the guest's
# virtio-gpu is reset under them, inside a graphics guest, Rach2's or
# Debian's, as root.
#
#   virgl-lost.sh [-f FRAMES] [-o OUT]
#
# Ends any Weston that runs, such as Rach2's boot session when the runner is
# started by hand, and starts a Weston of its own on the DRM card with its
# GL renderer, virgl's, through libseat's noop backend, as vkcube.sh does.
# weston-simple-egl draws to it, so that Weston's renderer composites, and
# glreset.c, which the runner builds with the guest's GCC, draws with the
# most robust context the driver gives. Once the client has printed its
# first count of frames, after five seconds of them, so that it draws, on
# the card, with its buffers made, and glreset has drawn its frames right,
# the runner resets the device: on Rach2 by writing 1 to the driver's
# debugfs file, /sys/kernel/debug/dri/N/virtio_gpu_reset, and on Linux,
# which has no such file, by unbinding the card's virtio device from the
# virtio_gpu driver. Either way the driver unplugs its DRM device and resets
# the virtio device, and QEMU's virglrenderer frees every context and
# resource. glreset then draws FRAMES frames more (200) and makes a new
# context. The runner then starts weston-simple-shm, a client that needs no
# GPU, so that Weston has frames to repaint whatever became of the first
# client, and once Weston has logged ten lines reads what is left: whether
# Weston and its first client still run, what Weston's log says of its
# repaints, the DRM nodes, the renderer a new Weston gets on the headless
# backend with its GL renderer, and how a new Weston on the DRM backend
# ends.
#
# A client that starts as the device goes is another matter, which the
# runner keeps out of the result: one that has not opened the card draws in
# software, and one whose first buffer cannot be made dies in Mesa's Wayland
# platform (dev-log/77-vgpu/upstream.md).
#
# OUT (.) gets the record; `lost.txt`, the lines below, which the case
# compares with the Debian guest's; and the logs of glreset, the client and
# each Weston. It prints lost.txt:
#
#   glreset: context: KIND
#   glreset: before: pixels right|wrong, status STATUS, errors ERRORS
#   glreset: after: frames N, pixels right|wrong, status STATUS, errors ERRORS
#   glreset: new context: KIND on RENDERER | none (WHY)
#   glreset: exit STATUS
#   weston: running | exit STATUS
#   weston: log: LINE              Weston's first line since the reset
#   client: running | exit STATUS
#   nodes: none | NODE...
#   weston headless: GL renderer: RENDERER | exit STATUS
#   weston drm: running | exit STATUS, LAST LINE
#
# with llvmpipe named without its LLVM, and then `virgl-lost: result: reset
# by HOW`. The runner waits for each program's own line or end, and for
# Weston's ten lines 100 tries, ten seconds. The status is 0 when every step
# ran; what the programs saw is a result, which the case compares with the
# reference's.
set -eu

RUNNER_DIR=$(cd "$(dirname "$0")" && pwd)
# shellcheck source=common.sh
. "$RUNNER_DIR/common.sh"

frames=200 out=.
while getopts f:o: o; do
    case $o in
        f) frames=$OPTARG ;;
        o) out=$OPTARG ;;
        *) exit 2 ;;
    esac
done
shift $((OPTIND - 1))
[ $# -eq 0 ] || { echo "usage: virgl-lost.sh [-f FRAMES] [-o OUT]" >&2; exit 2; }
for program in weston weston-simple-egl weston-simple-shm stdbuf gcc; do
    command -v "$program" > /dev/null || { echo "virgl-lost: no $program" >&2; exit 1; }
done

# westons: the process ids of the running compositors.
westons()
{
    for comm in /proc/[0-9]*/comm; do
        [ "$(cat "$comm" 2>/dev/null)" = weston ] || continue
        pid=${comm#/proc/}
        echo "${pid%/comm}"
    done
}

# state NAME PID: `NAME: running`, or `NAME: exit STATUS` for a process of
# this shell that has ended.
state()
{
    if kill -0 "$2" 2>/dev/null; then
        echo "$1: running"
    else
        st=0
        wait "$2" || st=$?
        echo "$1: exit $st"
    fi
}

# await FILE PATTERN PID WHAT: wait until FILE has a line that matches
# PATTERN, and fail when the process PID ends first.
await()
{
    until grep -q "$2" "$1" 2>/dev/null; do
        kill -0 "$3" 2>/dev/null || runner_die "$4 ended early: $(tail -n 3 "$1" 2>/dev/null)"
        sleep 0.1
    done
}

# cleanup: end the programs, which may run on after the loss, then what
# common.sh's cleanup ends; installed as the exit trap.
cleanup()
{
    for pid in $gl $client $shm $again; do
        kill -KILL "$pid" 2>/dev/null || true
        wait "$pid" 2>/dev/null || true
    done
    if [ -n "$RUNNER_WESTON" ]; then
        kill -KILL "$RUNNER_WESTON" 2>/dev/null || true
    fi
    runner_cleanup
}

gl='' client='' shm='' again=''
runner_init virgl-lost "$out"
trap cleanup EXIT
runner_start plain
runner_record "case virgl-lost $frames frames after the reset"

gcc -std=c99 -O2 -o "$RUNNER_WORK/glreset" "$RUNNER_DIR/glreset.c" \
    /usr/lib/x86_64-linux-gnu/libEGL.so.1 /usr/lib/x86_64-linux-gnu/libGLESv2.so.2 ||
    runner_die "glreset did not build"

pids=$(westons)
if [ -n "$pids" ]; then
    # shellcheck disable=SC2086 # the ids are words
    kill -TERM $pids
    for pid in $pids; do
        while [ -d "/proc/$pid" ]; do sleep 0.1; done
    done
    runner_record "before ended the running Weston, $pids"
fi

XDG_RUNTIME_DIR=$RUNNER_WORK/run
WAYLAND_DISPLAY=wayland-lost
export XDG_RUNTIME_DIR WAYLAND_DISPLAY
mkdir -m 700 "$XDG_RUNTIME_DIR"
LIBSEAT_BACKEND=noop setsid weston --backend=drm --renderer=gl --shell=kiosk --idle-time=0 \
    --continue-without-input --no-config --socket="$WAYLAND_DISPLAY" --log="$out/weston.log" \
    < /dev/null > /dev/null 2>&1 &
RUNNER_WESTON=$!
until [ -S "$XDG_RUNTIME_DIR/$WAYLAND_DISPLAY" ]; do
    kill -0 "$RUNNER_WESTON" 2>/dev/null || runner_die "weston ended: $(tail -n 3 "$out/weston.log")"
    sleep 0.1
done
runner_record "compositor $(weston --version 2>/dev/null) drm gl kiosk"
runner_record "renderer $(sed -n 's/.*GL renderer: *//p' "$out/weston.log" | head -n 1)"

stdbuf -oL weston-simple-egl > "$out/client.log" 2>&1 < /dev/null &
client=$!
"$RUNNER_WORK/glreset" "$RUNNER_WORK/before" "$RUNNER_WORK/after" "$frames" \
    > "$out/glreset.log" 2>&1 < /dev/null &
gl=$!
await "$out/glreset.log" '^glreset: drawing$' "$gl" glreset
await "$out/client.log" ' frames in ' "$client" "the client"
# A client that could not open the card draws in software, which no reset
# touches.
! grep -q 'could not open' "$out/client.log" || runner_die "the client does not draw on the card"
runner_record "client $(grep ' frames in ' "$out/client.log" | head -n 1)"
: > "$RUNNER_WORK/before"
await "$out/glreset.log" '^glreset: before: ' "$gl" glreset
logged=$(wc -l < "$out/weston.log")

# The reset: Rach2's debugfs file, or on Linux the driver's unbind.
how=
for f in /sys/kernel/debug/dri/*/virtio_gpu_reset; do
    [ -e "$f" ] || continue
    echo 1 > "$f" || runner_die "the reset through $f failed"
    how="$f"
    break
done
if [ -z "$how" ]; then
    dev=
    for f in /sys/bus/virtio/drivers/virtio_gpu/virtio[0-9]*; do
        [ -e "$f" ] || continue
        dev=${f##*/}
        break
    done
    [ -n "$dev" ] || runner_die "no virtio-gpu device to reset"
    echo "$dev" > /sys/bus/virtio/drivers/virtio_gpu/unbind || runner_die "the unbind of $dev failed"
    how="the unbind of $dev from virtio_gpu"
fi
runner_record "reset $how at $(date +%H:%M:%S.%N)"
: > "$RUNNER_WORK/after"
st=0
wait "$gl" || st=$?
gl=''
weston-simple-shm > "$out/shm.log" 2>&1 < /dev/null &
shm=$!
# Weston's first line since the reset, without its time: wait for ten lines
# while Weston runs, by when the first client has had its frame callbacks,
# 100 tries at most, which a case's bound on silence allows.
tries=0
while [ "$(wc -l < "$out/weston.log")" -lt "$((logged + 10))" ] &&
    kill -0 "$RUNNER_WESTON" 2>/dev/null && [ "$tries" -lt 100 ]; do
    tries=$((tries + 1))
    sleep 0.1
done
first=$(tail -n "+$((logged + 1))" "$out/weston.log" | sed -n '1s/^\[[0-9:.]*\] //p')

{
    grep '^glreset: ' "$out/glreset.log" | grep -v '^glreset: drawing$'
    echo "glreset: exit $st"
    state weston "$RUNNER_WESTON"
    echo "weston: log: ${first:-nothing in 100 tries}"
    state client "$client"
    nodes=
    for f in /dev/dri/*; do
        [ ! -e "$f" ] || nodes="$nodes${nodes:+ }${f##*/}"
    done
    echo "nodes: ${nodes:-none}"
} > "$RUNNER_WORK/lost"
kill -KILL "$RUNNER_WESTON" "$client" "$shm" 2>/dev/null || true
wait "$client" 2>/dev/null || true
wait "$shm" 2>/dev/null || true
client='' shm=''
wait "$RUNNER_WESTON" 2>/dev/null || true
RUNNER_WESTON=''

# A new Weston: headless with its GL renderer, then on the DRM backend.
WAYLAND_DISPLAY=wayland-again
weston --backend=headless --renderer=gl --no-config --socket="$WAYLAND_DISPLAY" \
    --log="$out/weston-headless.log" < /dev/null > /dev/null 2>&1 &
again=$!
until [ -S "$XDG_RUNTIME_DIR/$WAYLAND_DISPLAY" ] || ! kill -0 "$again" 2>/dev/null; do
    sleep 0.1
done
if kill -0 "$again" 2>/dev/null; then
    echo "weston headless: $(sed -n 's/^\[[0-9:.]*\] \(GL renderer: .*\)/\1/p' "$out/weston-headless.log" | head -n 1)" >> "$RUNNER_WORK/lost"
    kill -TERM "$again"
    wait "$again" 2>/dev/null || true
else
    state "weston headless" "$again" >> "$RUNNER_WORK/lost"
fi
again=''
WAYLAND_DISPLAY=wayland-drm
LIBSEAT_BACKEND=noop weston --backend=drm --renderer=gl --continue-without-input --no-config \
    --socket="$WAYLAND_DISPLAY" --log="$out/weston-drm.log" < /dev/null > /dev/null 2>&1 &
again=$!
until [ -S "$XDG_RUNTIME_DIR/$WAYLAND_DISPLAY" ] || ! kill -0 "$again" 2>/dev/null; do
    sleep 0.1
done
state "weston drm" "$again" > "$RUNNER_WORK/drm"
echo "$(cat "$RUNNER_WORK/drm"), $(tail -n 1 "$out/weston-drm.log" | sed 's/^\[[0-9:.]*\] //')" >> "$RUNNER_WORK/lost"
kill -TERM "$again" 2>/dev/null || true
wait "$again" 2>/dev/null || true
again=''
runner_finish
# llvmpipe names its LLVM and the vector width, which differ between guests.
sed 's/llvmpipe (LLVM [^)]*)/llvmpipe/' "$RUNNER_WORK/lost" > "$out/lost.txt"
cat "$out/lost.txt"
echo "virgl-lost: result: reset by $how" | tee -a "$out/results"
