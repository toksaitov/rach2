#!/bin/sh
# vkcube.sh: vkcube presenting through a Vulkan driver to Weston on the
# display, inside a graphics guest, Rach2's or Debian's, as root, with the
# frames it shows read back inside the guest and checksummed.
#
#   vkcube.sh [-d] [-i ICD] [-o OUT]
#
# -d uses the caller's DRM Weston and its screenshot protocol, without
# stopping or starting a compositor. It may run as that display's user.
#
# Ends any Weston that runs, such as Rach2's boot session, whose supervisor
# then waits, and starts a Weston of its own on the DRM card with its GL
# renderer, kiosk-shell, --debug, which offers the screenshot protocol, and
# --no-config, so that no guest's weston.ini adds to it, through libseat's
# noop backend, which opens the card as root. vkcube then presents 346 frames
# to it in FIFO, through the Vulkan ICD file ICD alone when -i names one,
# with the layer of vkhold.c, which the runner builds with the guest's GCC,
# since Rach2's cc is TinyCC, which makes no shared object. The layer times
# the first 300 presents and holds vkcube after the 300th and the 345th.
# During each hold, Weston shows that frame and nothing changes it:
# weston-screenshooter reads it back from Weston's renderer, and frame.py
# checksums its pixels, until two shots in a row give the same sum. vkcube's
# picture depends on the number of frames drawn alone, never on the clock,
# so a frame's sum is the same in every guest whose driver draws it the
# same; the reference is the Debian guest's, whose Venus draws on the same
# host as Rach2's. Run it with guest-env.sh's performance class,
# LP_NUM_THREADS at the guest's CPU count.
#
# OUT (.) gets the record, `frames`, one line a held frame, `frame N WxH
# crc32 0xCRC`, `timing`, the layer's line for the 300 presents, the GPU line
# vkcube printed, and the logs of Weston, vkcube and the layer. It prints
#
#   vkcube: result: frames 300 0xCRC 345 0xCRC; 300 presents in S s, R a second
#
# The status is 0 when vkcube ended by itself with status 0 after every
# frame held was read; judge.py compares the frames with the reference's,
# and the rate is recorded alone, against the Debian guest's under the same
# QEMU.
set -eu

RUNNER_DIR=$(cd "$(dirname "$0")" && pwd)
# shellcheck source=common.sh
. "$RUNNER_DIR/common.sh"

FRAMES=346
HOLDS="300 345"
TIMED=300

icd='' out=. display=0
while getopts di:o: o; do
    case $o in
        d) display=1 ;;
        i) icd=$OPTARG ;;
        o) out=$OPTARG ;;
        *) exit 2 ;;
    esac
done
shift $((OPTIND - 1))
[ $# -eq 0 ] || { echo "usage: vkcube.sh [-d] [-i ICD] [-o OUT]" >&2; exit 2; }
for program in vkcube weston weston-screenshooter gcc python3; do
    command -v "$program" > /dev/null || { echo "vkcube: no $program" >&2; exit 1; }
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

# vkcube_version: the version of Debian's vulkan-tools, from dpkg in a
# Debian guest and from the package's changelog on Rach2's root, which has no
# dpkg database.
vkcube_version()
{
    dpkg-query -W -f '${Version}' vulkan-tools 2> /dev/null ||
        gzip -dc /usr/share/doc/vulkan-tools/changelog.Debian.gz 2> /dev/null |
        sed -n '1s/^[^(]*(\([^)]*\)).*/\1/p'
}

# shot N: read the screen back until two shots in a row give one sum, and
# append the frame's line to the frames file.
shot()
{
    last='' tries=0
    while :; do
        tries=$((tries + 1))
        [ "$tries" -le 20 ] || runner_die "frame $1: no two shots in a row alike in 20"
        rm -rf "$RUNNER_WORK/shot"
        mkdir "$RUNNER_WORK/shot"
        (cd "$RUNNER_WORK/shot" && weston-screenshooter) >> "$out/screenshooter.log" 2>&1 ||
            runner_die "weston-screenshooter failed: $(tail -n 3 "$out/screenshooter.log")"
        png=$(find "$RUNNER_WORK/shot" -name '*.png' | head -n 1)
        [ -n "$png" ] || runner_die "weston-screenshooter wrote no picture"
        sum=$(python3 -I "$RUNNER_DIR/frame.py" "$png")
        if [ "$sum" = "$last" ]; then
            cp "$png" "$out/frame-$1.png"
            echo "frame $1 $sum" >> "$out/frames"
            return
        fi
        last=$sum
    done
}

# cleanup: end vkcube, which may wait in a hold, then what common.sh's
# cleanup ends; installed as the exit trap.
cleanup()
{
    if [ -n "$cube" ]; then
        kill -KILL "$cube" 2>/dev/null || true
        wait "$cube" 2>/dev/null || true
    fi
    runner_cleanup
}

cube=''
runner_init vkcube "$out"
trap cleanup EXIT
runner_start plain
runner_record "case vkcube $FRAMES frames in FIFO, held after $HOLDS, the first $TIMED timed"
runner_record "icd ${icd:-none named, every driver the loader finds}"
runner_record "vkcube $(vkcube_version)"

gcc -std=c99 -O2 -shared -fPIC -o "$RUNNER_WORK/libvkhold.so" "$RUNNER_DIR/vkhold.c" ||
    runner_die "the layer did not build"
mkdir "$RUNNER_WORK/layers"
cat > "$RUNNER_WORK/layers/vkhold.json" << EOF
{
    "file_format_version": "1.1.0",
    "layer": {
        "name": "VK_LAYER_RACH2_vkhold",
        "type": "GLOBAL",
        "library_path": "$RUNNER_WORK/libvkhold.so",
        "api_version": "1.0.0",
        "implementation_version": "1",
        "description": "holds a program after chosen presents",
        "functions": {
            "vkGetInstanceProcAddr": "vkhold_GetInstanceProcAddr",
            "vkGetDeviceProcAddr": "vkhold_GetDeviceProcAddr"
        }
    }
}
EOF

if [ "$display" = 0 ]; then
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
    WAYLAND_DISPLAY=wayland-vkcube
    export XDG_RUNTIME_DIR WAYLAND_DISPLAY
    mkdir -m 700 "$XDG_RUNTIME_DIR"
    LIBSEAT_BACKEND=noop setsid weston --backend=drm --renderer=gl --shell=kiosk --idle-time=0 \
        --no-config --debug --socket="$WAYLAND_DISPLAY" --log="$out/weston.log" < /dev/null > /dev/null 2>&1 &
    RUNNER_WESTON=$!
    tries=0
    until [ -S "$XDG_RUNTIME_DIR/$WAYLAND_DISPLAY" ]; do
        kill -0 "$RUNNER_WESTON" 2>/dev/null || runner_die "weston ended: $(tail -n 3 "$out/weston.log")"
        tries=$((tries + 1))
        [ "$tries" -lt 600 ] || runner_die "weston made no socket in 600 tries"
        sleep 0.1
    done
    runner_record "compositor $(weston --version 2>/dev/null) drm gl kiosk"
    runner_record "renderer $(sed -n 's/.*GL renderer: *//p' "$out/weston.log" | head -n 1)"
else
    [ -S "${XDG_RUNTIME_DIR:?}/${WAYLAND_DISPLAY:?}" ] || runner_die "no caller's Wayland display"
    runner_record "compositor caller's DRM Weston, see the caller's log"
fi

mkfifo "$RUNNER_WORK/fifo"
: > "$out/frames"
: > "$out/vkhold.log"
(
    [ -z "$icd" ] || { VK_DRIVER_FILES=$icd; export VK_DRIVER_FILES; }
    VK_INSTANCE_LAYERS=VK_LAYER_RACH2_vkhold VK_ADD_LAYER_PATH=$RUNNER_WORK/layers
    VKHOLD_FRAMES=$HOLDS VKHOLD_TIMED=$TIMED VKHOLD_LOG=$out/vkhold.log VKHOLD_FIFO=$RUNNER_WORK/fifo
    export VK_INSTANCE_LAYERS VK_ADD_LAYER_PATH VKHOLD_FRAMES VKHOLD_TIMED VKHOLD_LOG VKHOLD_FIFO
    exec vkcube --wsi wayland --c "$FRAMES"
) > "$out/vkcube.log" 2>&1 &
cube=$!
for frame in $HOLDS; do
    until grep -q "^vkhold: held: frame $frame\$" "$out/vkhold.log"; do
        kill -0 "$cube" 2>/dev/null || runner_die "vkcube ended before frame $frame: $(tail -n 3 "$out/vkcube.log")"
        sleep 0.1
    done
    shot "$frame"
    echo go > "$RUNNER_WORK/fifo"
done
status=0
wait "$cube" || status=$?
cube=''
runner_finish
[ "$status" -eq 0 ] || runner_die "vkcube exited $status: $(tail -n 3 "$out/vkcube.log")"
grep '^Selected GPU' "$out/vkcube.log" > "$out/gpu" || runner_die "vkcube named no GPU"
runner_record "gpu $(cat "$out/gpu")"
grep '^vkhold: timed: ' "$out/vkhold.log" > "$out/timing" || runner_die "the layer timed nothing"
runner_record "timing $(cat "$out/timing")"
rate=$(awk '{ printf "%.2f", $3 / $6 }' "$out/timing")
echo "rate $rate" >> "$out/timing"
echo "vkcube: result: frames $(awk '{ printf "%s%s %s", (NR > 1 ? " " : ""), $2, $5 }' "$out/frames");" \
    "$(awk '{ print $3 }' "$out/timing" | head -n 1) presents in" \
    "$(awk '{ print $6 }' "$out/timing" | head -n 1) s, $rate a second" | tee -a "$out/results"
