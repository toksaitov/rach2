#!/bin/sh
# measure.sh: one scene of the test bed's measurements, run on the machine
# whose rendering path is measured, with one line of numbers for the run
# (bed/measure/Readme.md).
#
#   measure.sh [-r RUNG] [-o OUT] [-t SECONDS] [-u USER] [-d] [-p]
#              [-L LATENCY] [-v RELAYLOG] glmark2|piglit|webgl|game
#
# The scenes, each the same work on every rung:
#
#   glmark2  glmark2-wayland's build, texture, shading (Phong) and bump
#            (normals) scenes on screen at 1280x800, each for SECONDS (10)
#   piglit   the tests of piglit.txt, beside this script, one at a time on
#            the device EGL's surfaceless platform finds (piglit.sh -g)
#   webgl    webgl.html in Chromium, fullscreen at 1280x800, for SECONDS
#            times 60 frames (600)
#   game     SuperTuxKart's replay, as games.sh plays it, with SECONDS of the
#            race's own time when -t gives them
#
# glmark2 and webgl draw under a headless Weston of the script's own, with
# its GL renderer unless -p asks for Pixman, or with -d under the caller's
# Wayland display; the game too, through games.sh. A scene's frame times
# come from Mesa's Gallium HUD for glmark2 and the game, the time between
# the page's frames for webgl, and each test's time for piglit, whose test
# counts as one frame.
#
# Around the scene, the script reads the machine's counts and prints, as
# its last line,
#
#   measure: result: SCENE RUNG frames N frame_ms MEAN p50 P50 p99 P99
#       cpu_ms_per_frame C latency_ms L copied_bytes B exits E
#       round_trips T seconds S
#
# on one line, `-` for a count this machine does not keep:
#
#   cpu_ms_per_frame  the CPU time of every processor of the machine,
#                     /proc/stat's busy ticks, over the frames, the guest's
#                     CPU among it where the guest is QEMU's process here
#   latency_ms        latency's median round trip, a clear and one pixel
#                     read back, before the scene; LATENCY is the program,
#                     which latency.c, beside this script, is compiled to
#                     with cc when -L names none
#   copied_bytes      /proc/stat's virtio_gpu_copied_bytes on Rach2, or the
#                     vtest relay's bytes both ways when -v names its log
#   exits             /proc/stat's kvm_exits on Rach2, or KVM's debugfs
#                     `exits` on Linux, readable by root
#   round_trips       the doorbells Rach2's driver rang the VM on its queue
#                     vector, by ivshmemd -i, while a region is attached, or
#                     the commands the vtest relay carried with -v
#
# RELAYLOG is the standard error of `vtestrelay -v`, whose `result:` line
# for each client gives its commands and bytes. With -u, the script reads
# the counts as its caller, root where ivshmemd and debugfs need it, and
# runs the scene as USER, as Chromium needs. OUT (./measure-SCENE) gets the
# record, `frametimes`, one frame's milliseconds a line, `counts`, the
# readings before and after, and `line`, the last line. The status is 0 when
# the scene ran to its end and gave frames, and 1 otherwise; the script sets
# no time limit of its own.
set -eu

here=$(cd "$(dirname "$0")" && pwd)
runners=${MEASURE_RUNNERS:-}
if [ -z "$runners" ]; then
    if [ -f "$here/common.sh" ]; then runners=$here; else runners=$here/../../tests/graphics/runners; fi
fi
runners=$(cd "$runners" && pwd)

rung=unnamed out='' seconds='' user='' own=1 renderer=gl latency='' relay='' inner=0
usage()
{
    echo "usage: measure.sh [-r RUNG] [-o OUT] [-t SECONDS] [-u USER] [-d] [-p] [-L LATENCY] [-v RELAYLOG] glmark2|piglit|webgl|game" >&2
    exit 2
}
while getopts r:o:t:u:dpL:v:I o; do
    case $o in
        r) rung=$OPTARG ;;
        o) out=$OPTARG ;;
        t) seconds=$OPTARG ;;
        u) user=$OPTARG ;;
        d) own=0 ;;
        p) renderer=pixman ;;
        L) latency=$OPTARG ;;
        v) relay=$OPTARG ;;
        I) inner=1 ;;
        *) usage ;;
    esac
done
shift $((OPTIND - 1))
[ $# -eq 1 ] || usage
scene=$1
case $scene in glmark2 | piglit | webgl | game) ;; *) usage ;; esac
case $seconds in *[!0-9]* | 0*) usage ;; esac
[ -n "$out" ] || out=./measure-$scene
mkdir -p "$out"
out=$(cd "$out" && pwd)

# scene_glmark2: glmark2's scenes on screen, frame times from the HUD.
scene_glmark2()
{
    d=${seconds:-10}
    mkdir -p "$RUNNER_WORK/hud"
    GALLIUM_HUD=frametime GALLIUM_HUD_VISIBLE=false GALLIUM_HUD_PERIOD=0 \
        GALLIUM_HUD_DUMP_DIR=$RUNNER_WORK/hud \
        glmark2-wayland -s "${RUNNER_WIDTH}x$RUNNER_HEIGHT" --results fps:cpu \
        -b "build:duration=$d" -b "texture:duration=$d" \
        -b "shading:shading=phong:duration=$d" -b "bump:bump-render=normals:duration=$d" \
        > "$out/glmark2.log" 2>&1 ||
        runner_die "glmark2-wayland failed: $(tail -n 3 "$out/glmark2.log")"
    runner_record "renderer $(sed -n 's/^ *GL_RENDERER: *//p' "$out/glmark2.log" | head -n 1)"
    grep 'FPS:' "$out/glmark2.log" | sed 's/^ */glmark2 /' >> "$RUNNER_OUT/record"
    cat "$RUNNER_WORK/hud/"* 2>/dev/null > "$out/frametimes" || true
}

# scene_piglit: piglit.txt's tests, each test's time a frame.
scene_piglit()
{
    sh "$runners/piglit.sh" -g -o "$out/piglit" -l "$here/piglit.txt" > "$out/piglit.log" 2>&1 ||
        runner_die "piglit.sh failed: $(tail -n 3 "$out/piglit.log")"
    cat "$out/piglit.log" >> "$RUNNER_OUT/record"
    python3 -I - "$out/piglit/piglit.json" > "$out/frametimes" << 'EOF'
import json, sys
tests = json.load(open(sys.argv[1], encoding="utf-8"))["tests"]
for name in sorted(tests):
    t = tests[name].get("time", {})
    print("%.3f" % ((t.get("end", 0) - t.get("start", 0)) * 1000))
EOF
}

# scene_webgl: webgl.html in Chromium, the time between its frames.
scene_webgl()
{
    frames=$((${seconds:-10} * 60))
    runner_server "$here"
    runner_browser_record chromium
    runner_browser chromium "http://127.0.0.1:$RUNNER_PORT/webgl.html?frames=$frames" \
        "$out/browser.log"
    # shellcheck disable=SC2046 # two words
    set -- $(python3 -I "$runners/events.py" wait "$RUNNER_WORK/events" 0 "$RUNNER_BROWSER" 120)
    runner_stop
    [ "$1" = "done" ] || runner_die "the page ended $1: $(tail -n 3 "$out/browser.log")"
    python3 -I - "$RUNNER_WORK/events" "$out/frametimes" "$RUNNER_OUT/record" << 'EOF'
import json, sys
done = [e for e in map(json.loads, open(sys.argv[1], encoding="utf-8")) if e.get("kind") == "done"]
page = done[-1]
if not page.get("ok"):
    sys.exit("measure: webgl: the page failed: %s" % page.get("error"))
times = page["times"]
with open(sys.argv[2], "w") as f:
    for a, b in zip(times, times[1:]):
        f.write("%.3f\n" % (b - a))
with open(sys.argv[3], "a") as f:
    f.write("renderer %s\nwebgl size %s pixel %s\n" % (page.get("renderer"), page.get("size"),
                                                       page.get("pixel")))
EOF
}

# scene_game: SuperTuxKart's replay through games.sh.
scene_game()
{
    sh "$runners/games.sh" ${seconds:+-t "$seconds"} -d -o "$out/game" supertuxkart \
        > "$out/game.log" 2>&1 || runner_die "games.sh failed: $(tail -n 3 "$out/game.log")"
    cat "$out/game.log" >> "$RUNNER_OUT/record"
    cp "$out/game/frametimes" "$out/frametimes"
}

# The scene's half, -I, as the user who runs it: Weston, the scene and its
# frames, with its scratch under OUT, which the caller gave the user.
if [ "$inner" = 1 ]; then
    TMPDIR=$out/tmp
    export TMPDIR
    mkdir -p "$TMPDIR"
    # shellcheck source=../../tests/graphics/runners/common.sh
    . "$runners/common.sh"
    # shellcheck disable=SC2034 # common.sh reads it
    RUNNER_DIR=$runners
    runner_init measure "$out/scene"
    runner_start plain
    runner_record "scene $scene rung $rung"
    if [ "$own" = 1 ]; then
        # shellcheck disable=SC2034 # runner_weston reads it
        RUNNER_RENDERER=$renderer
        runner_weston
    else
        [ -n "${WAYLAND_DISPLAY:-}" ] || runner_die "-d needs WAYLAND_DISPLAY"
        runner_record "compositor the caller's, $WAYLAND_DISPLAY"
    fi
    "scene_$scene"
    runner_finish
    [ -s "$out/frametimes" ] || runner_die "the scene gave no frame times"
    exit 0
fi

# counts: the machine's counts now, one `NAME VALUE` a line, a count it
# does not keep left out.
counts()
{
    awk '$1 == "cpu" { t = 0; for (i = 2; i <= NF; i++) t += $i; print "busy", t - $5 - $6; print "total", t }
         $1 == "kvm_exits" || $1 == "virtio_gpu_copied_bytes" { print $1, $2 }' /proc/stat
    [ ! -r /sys/kernel/debug/kvm/exits ] || echo "linux_exits $(cat /sys/kernel/debug/kvm/exits)"
    info=$(ivshmemd -i 2>/dev/null) || info=''
    case $info in
        *'attached 1'*) echo "$info" | tr ',' '\n' | sed -n 's/^.* rung \([0-9]*\)\/.*$/rung \1/p' | head -n 1 ;;
    esac
    if [ -n "$relay" ] && [ -r "$relay" ]; then
        sed -n 's/^vtestrelay: result: \([0-9]*\) commands.*; \([0-9]*\) bytes to the server, \([0-9]*\) to the client$/\1 \2 \3/p' \
            "$relay" | awk '{ c += $1; b += $2 + $3 } END { print "relay_commands", c + 0; print "relay_bytes", b + 0 }'
    fi
}

# delta NAME: the growth of count NAME over the scene, or `-`.
delta()
{
    awk -v n="$1" '$1 == n { if (seen) { print $2 - v; done = 1 } else { v = $2; seen = 1 } }
                   END { if (!done) print "-" }' "$out/counts"
}

# The latency's round trips, before the scene so that they do not load it.
if [ -z "$latency" ] && command -v cc > /dev/null &&
    cc -std=c99 -O2 -o "$out/latency" "$here/latency.c" -ldl > "$out/latency.build" 2>&1; then
    latency=$out/latency
fi
lat=-
if [ -n "$latency" ] && "$latency" 200 > "$out/latency.out" 2>&1; then
    lat=$(sed -n 's/^latency: .* median \([0-9.]*\) ms.*/\1/p' "$out/latency.out")
fi

{ echo before; counts; } > "$out/counts"
start=$(date +%s.%N)
status=0
flags=''
[ "$own" = 1 ] || flags=-d
[ "$renderer" = gl ] || flags="$flags -p"
if [ -n "$user" ] && [ "$(id -u)" = 0 ]; then
    chown -R "$user" "$out"
    su -s /bin/sh "$user" -c "sh '$here/measure.sh' -I -r '$rung' -o '$out' ${seconds:+-t $seconds} $flags $scene" \
        > "$out/scene.log" 2>&1 || status=$?
else
    # shellcheck disable=SC2086 # the flags are words
    sh "$here/measure.sh" -I -r "$rung" -o "$out" ${seconds:+-t "$seconds"} $flags "$scene" \
        > "$out/scene.log" 2>&1 || status=$?
fi
end=$(date +%s.%N)
{ echo after; counts; } >> "$out/counts"
if [ "$status" -ne 0 ] || [ ! -s "$out/frametimes" ]; then
    echo "measure: $scene: failed: $(tail -n 3 "$out/scene.log" | tr '\n' ' ')" >&2
    exit 1
fi

copied=$(delta virtio_gpu_copied_bytes) exits=$(delta kvm_exits) trips=$(delta rung)
[ "$exits" != - ] || exits=$(delta linux_exits)
if [ -n "$relay" ]; then
    copied=$(delta relay_bytes) trips=$(delta relay_commands)
fi
tick=$(getconf CLK_TCK 2>/dev/null || echo 100)
sort -g "$out/frametimes" | awk -v busy="$(delta busy)" -v tick="$tick" -v scene="$scene" -v rung="$rung" \
    -v lat="$lat" -v copied="$copied" -v exits="$exits" -v trips="$trips" -v s="$start" -v e="$end" '
    { t[NR] = $1; sum += $1 }
    END {
        p50 = t[int((NR - 1) * 0.50) + 1]; p99 = t[int((NR - 1) * 0.99) + 1]
        printf "measure: result: %s %s frames %d frame_ms %.3f p50 %.3f p99 %.3f cpu_ms_per_frame %.3f latency_ms %s copied_bytes %s exits %s round_trips %s seconds %.1f\n",
            scene, rung, NR, sum / NR, p50, p99, busy * 1000 / tick / NR, lat, copied, exits, trips, e - s
    }' | tee "$out/line"
