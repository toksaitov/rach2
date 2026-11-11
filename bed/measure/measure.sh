#!/bin/sh
# measure.sh: one scene of the test bed's measurements, run on the machine
# whose rendering path is measured, with one line of numbers for the run
# (bed/measure/Readme.md).
#
#   measure.sh [-r RUNG] [-o OUT] [-t SECONDS] [-u USER] [-d] [-p] [-c]
#              [-L LATENCY] [-v RELAYLOG] [-R REPLAYS] glmark2|piglit|webgl|game
#
# The scenes, each the same work on every rung:
#
#   glmark2  glmark2-wayland's build, texture, shading (Phong) and bump
#            (normals) scenes on screen at 1280x800, or fullscreen with -d,
#            each for SECONDS (10)
#   piglit   the tests of piglit.txt, beside this script, one at a time on
#            the device EGL's surfaceless platform finds (piglit.sh -g),
#            once, or with -t again and again until SECONDS have passed
#   webgl    webgl.html in Chromium, fullscreen at 1280x800, for SECONDS
#            times 60 frames (600), its frames paced by neither the
#            display's refresh nor a frame rate limit
#   game     SuperTuxKart's replay of REPLAYS, as games.sh -r plays it, with
#            SECONDS of the race's own time when -t gives them
#
# glmark2 and webgl draw under a headless Weston of the script's own, with
# its GL renderer unless -p asks for Pixman, which webgl always has, or with
# -d under the caller's Wayland display; the game too, through games.sh. A
# scene's frame times come from Mesa's Gallium HUD for glmark2 and the game,
# the time between the page's frames for webgl, and each passing test's time
# for piglit, whose test counts as one frame. Every scene compiles its
# shaders into a new empty Mesa shader cache under OUT.
#
# Around the scene, the script reads the machine's counts and prints, as
# its last line,
#
#   measure: result: SCENE RUNG frames N frame_ms MEAN p50 P50 p99 P99
#       cpu_ms_per_frame C latency_ms L copied_bytes B exits E
#       round_trips T seconds S passed P
#
# on one line, `-` for a count this machine does not keep:
#
#   cpu_ms_per_frame  the CPU time of every processor of the machine,
#                     /proc/stat's busy ticks, over the frames, the guest's
#                     CPU among it where the guest is QEMU's process here;
#                     for the game, over every frame the HUD wrote, since
#                     the ticks span its loading screen too
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
#   passed            piglit's tests that passed, of the tests run, as
#                     P/T, the frames being the passing ones; `-` for the
#                     other scenes
#
# RELAYLOG is the standard error of `vtestrelay -v`, whose `result:` line
# for each client gives its commands and bytes. With -u, the script reads
# the counts as its caller, root where ivshmemd and debugfs need it, and
# runs the scene as USER, as Chromium needs, from a copy of the harness,
# its runners and REPLAYS under OUT, which USER may write through its
# group, since USER may not read the caller's directories. REPLAYS
# (/usr/share/games/rach2/replays) holds the games layer's replays,
# vendor/gnu/games/replays of the tree. With -c the script prints
# `measure: counts: before` and `measure: counts: after` as it reads the
# counts, for a caller that reads its own around the scene. OUT
# (./measure-SCENE) gets the record, `frametimes`, one frame's milliseconds
# a line, `counts`, the readings before and after, and `line`, the last
# line; a run first removes what an earlier run wrote there. The scene runs
# in a process group of its own, which the script ends, TERM and then KILL,
# on every exit and on SIGTERM, SIGINT or SIGHUP. The status is 0 when the
# scene ran to its end and gave frames, and 1 otherwise; the script sets no
# time limit of its own.
set -eu

here=$(cd "$(dirname "$0")" && pwd)
runners=${MEASURE_RUNNERS:-}
if [ -z "$runners" ]; then
    if [ -f "$here/common.sh" ]; then runners=$here; else runners=$here/../../tests/graphics/runners; fi
fi
runners=$(cd "$runners" && pwd)

rung=unnamed out='' seconds='' user='' own=1 renderer=gl latency='' relay='' inner=0 marks=0
replays=/usr/share/games/rach2/replays
usage()
{
    echo "usage: measure.sh [-r RUNG] [-o OUT] [-t SECONDS] [-u USER] [-d] [-p] [-c] [-L LATENCY] [-v RELAYLOG] [-R REPLAYS] glmark2|piglit|webgl|game" >&2
    exit 2
}
while getopts r:o:t:u:dpcL:v:R:I o; do
    case $o in
        r) rung=$OPTARG ;;
        o) out=$OPTARG ;;
        t) seconds=$OPTARG ;;
        u) user=$OPTARG ;;
        d) own=0 ;;
        p) renderer=pixman ;;
        c) marks=1 ;;
        R) replays=$OPTARG ;;
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

# With -u as root, OUT stays root's, and USER, who runs the scene's half,
# writes there only through its group once the scene is about to start. The
# sticky bit keeps USER from removing or renaming root's own files, so that
# root never writes, runs, reads or signals through a name USER chose: it
# reads the scene's files from copies USER makes into `taken`, and changes
# the owner only of `harness`, which it made. Until then USER may not write
# OUT, and the names an earlier run wrote, root's and USER's, are removed;
# rm -r unlinks a link USER left and never follows it. What else USER made
# in OUT stays, untouched.
written='counts line scene.log latency latency.build latency.out taken harness
    frametimes cpuframes passes tmp shader-cache scene glmark2.log piglit
    browser.log game game.log'
if [ "$inner" = 0 ]; then
    if [ -n "$user" ] && [ "$(id -u)" = 0 ]; then
        chown 0:0 "$out"
        chmod 700 "$out"
    fi
    for f in $written "$out"/piglit-*.log; do rm -rf "${out:?}/${f##*/}"; done
fi

# scene_glmark2: glmark2's scenes on screen, frame times from the HUD.
scene_glmark2()
{
    d=${seconds:-10}
    mkdir -p "$RUNNER_WORK/hud"
    # On the caller's display, the whole output, as Linux's references ran.
    if [ "$own" = 1 ]; then size="-s ${RUNNER_WIDTH}x$RUNNER_HEIGHT"; else size=--fullscreen; fi
    # shellcheck disable=SC2086 # one or two words
    GALLIUM_HUD=frametime GALLIUM_HUD_VISIBLE=false GALLIUM_HUD_PERIOD=0 \
        GALLIUM_HUD_DUMP_DIR=$RUNNER_WORK/hud \
        glmark2-wayland $size --results fps:cpu \
        -b "build:duration=$d" -b "texture:duration=$d" \
        -b "shading:shading=phong:duration=$d" -b "bump:bump-render=normals:duration=$d" \
        > "$out/glmark2.log" 2>&1 ||
        runner_die "glmark2-wayland failed: $(tail -n 3 "$out/glmark2.log")"
    runner_record "renderer $(sed -n 's/^ *GL_RENDERER: *//p' "$out/glmark2.log" | head -n 1)"
    grep 'FPS:' "$out/glmark2.log" | sed 's/^ */glmark2 /' >> "$RUNNER_OUT/record"
    cat "$RUNNER_WORK/hud/"* 2>/dev/null > "$out/frametimes" || true
}

# scene_piglit: piglit.txt's tests, once or for SECONDS, each passing
# test's time a frame; `passes` gets a line for each test run, 1 when it
# passed.
scene_piglit()
{
    round=0 began=$(date +%s)
    : > "$out/frametimes"
    : > "$out/passes"
    while :; do
        round=$((round + 1))
        sh "$runners/piglit.sh" -g -o "$out/piglit/$round" -l "$here/piglit.txt" \
            > "$out/piglit-$round.log" 2>&1 ||
            runner_die "piglit.sh failed: $(tail -n 3 "$out/piglit-$round.log")"
        cat "$out/piglit-$round.log" >> "$RUNNER_OUT/record"
        python3 -I - "$out/frametimes" "$out/passes" "$out/piglit/$round"/piglit*.json << 'EOF'
import json, sys
times, passes = open(sys.argv[1], "a"), open(sys.argv[2], "a")
for path in sys.argv[3:]:
    tests = json.load(open(path, encoding="utf-8"))["tests"]
    for name in sorted(tests):
        t = tests[name].get("time", {})
        ok = tests[name].get("result") == "pass"
        passes.write("%d\n" % ok)
        if ok:
            times.write("%.3f\n" % ((t.get("end", 0) - t.get("start", 0)) * 1000))
EOF
        [ -n "$seconds" ] && [ $(($(date +%s) - began)) -lt "$seconds" ] || break
    done
    runner_record "piglit rounds $round passed $(grep -c 1 "$out/passes") of $(grep -c . "$out/passes")"
}

# scene_webgl: webgl.html in Chromium, the time between its frames.
scene_webgl()
{
    frames=$((${seconds:-10} * 60))
    # Frames as fast as the path draws them, as glmark2's swap interval 0.
    # shellcheck disable=SC2034 # runner_switches reads it
    RUNNER_CHROMIUM_EXTRA='--disable-gpu-vsync --disable-frame-rate-limit'
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

# scene_game: SuperTuxKart's replay through games.sh; `cpuframes` gets the
# count of every frame the HUD wrote, which the CPU's ticks span.
scene_game()
{
    sh "$runners/games.sh" ${seconds:+-t "$seconds"} -d -r "$replays" -o "$out/game" supertuxkart \
        > "$out/game.log" 2>&1 || runner_die "games.sh failed: $(tail -n 3 "$out/game.log")"
    cat "$out/game.log" >> "$RUNNER_OUT/record"
    cp "$out/game/frametimes" "$out/frametimes"
    grep -c . "$out/game/hud" > "$out/cpuframes"
}

# The scene's half, -I, as the user who runs it: Weston, the scene and its
# frames, with its scratch under OUT, which the caller gave the user.
if [ "$inner" = 1 ]; then
    TMPDIR=$out/tmp
    # A new empty shader cache, so that no rung starts warm from an earlier
    # run: guest-env.sh's GRAPHICS_DONE, which the record names.
    MESA_SHADER_CACHE_DIR=$out/shader-cache GRAPHICS_DONE=1
    export TMPDIR MESA_SHADER_CACHE_DIR GRAPHICS_DONE
    rm -rf "$MESA_SHADER_CACHE_DIR"
    mkdir -p "$TMPDIR" "$MESA_SHADER_CACHE_DIR"
    # shellcheck source=../../tests/graphics/runners/common.sh
    . "$runners/common.sh"
    # shellcheck disable=SC2034 # common.sh reads it
    RUNNER_DIR=$runners
    runner_init measure "$out/scene"
    runner_start plain
    runner_record "scene $scene rung $rung"
    if [ "$own" = 1 ]; then
        # Chromium's GPU process, not the DRM master, loses every context
        # under Weston's GL (tests/graphics/Readme.md, Browser runners).
        [ "$scene" != webgl ] || renderer=pixman
        # shellcheck disable=SC2034 # runner_weston reads it
        RUNNER_RENDERER=$renderer
        runner_weston
    else
        [ -n "${WAYLAND_DISPLAY:-}" ] || runner_die "-d needs WAYLAND_DISPLAY"
        runner_record "compositor the caller's, $WAYLAND_DISPLAY"
    fi
    # The latency's round trips are over: what draws from here is the scene.
    echo "measure: scene: $scene: started"
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

# The scene's half runs in a process group of its own, which a stop of this
# half ends whole: its Weston, its scene and their children. util-linux's su
# -c, Debian's, starts USER's shell in a session of its own, setsid() in the
# child it forks (login-utils/su-common.c:583, :1241-1244 of 2.42.4), and on
# SIGTERM sends that child TERM and, 2 seconds later, KILL (:634-647), which
# would cut the half's cleanup short. So when su's child leads a group of
# its own, the stop sends TERM to that group alone and leaves su waiting for
# its child; su's group gets only the final KILL. The group is read from
# /proc, never from OUT, which USER writes. The stop waits 15 seconds before
# its KILL, longer than the scene's half takes to end its browser, 5
# seconds, before it sends its server and Weston, which lead groups of their
# own, TERM, so that the half's own cleanup runs.
scene_pid='' copy_pid='' under_su=0

# inner_group PID: print the group of su's child when PID is still su and
# the child leads that group, or nothing. BusyBox's su, Rach2's, execs
# USER's shell in its own process, whose children, Weston among them, lead
# groups of their own. comm, the second field of /proc/PID/stat, may hold
# spaces, parentheses and newlines, so it ends at the last parenthesis.
inner_group()
{
    p=$1
    st=$(tr '\n' ' ' 2>/dev/null < "/proc/$p/stat") || return 0
    st=${st#*\(}
    [ "${st%) *}" = su ] || return 0
    for d in /proc/[0-9]*; do
        st=$(tr '\n' ' ' 2>/dev/null < "$d/stat") || continue
        # set -- replaces $1, so the PID is compared from p.
        # shellcheck disable=SC2086 # state, ppid, pgrp and the rest are words
        set -- ${st##*) }
        if [ "${2:-}" = "$p" ] && [ "${3:-}" = "${d#/proc/}" ]; then
            echo "$3"
            return 0
        fi
    done
}

# live PID: succeed while PID has not exited. kill -0 succeeds on a zombie
# too, and the shell's own child stays one until its wait reaps it.
live()
{
    st=$(tr '\n' ' ' 2>/dev/null < "/proc/$1/stat") || return 1
    st=${st##*) }
    [ "${st%% *}" != Z ]
}

# stop_copy: end the current reader without waiting for it. util-linux's su
# forwards TERM to its child and KILL 2 seconds later; BusyBox's su execs the
# reader. The reader is USER's, which USER may stop with SIGSTOP or ptrace,
# and util-linux's su stops itself when its child stops; a stopped task
# takes no TERM until it runs again (kernel/signal.c:988-989 of 6.12.115).
# So CONT follows TERM and wakes it (:923-937), and when the reader has gone
# or 5 seconds have passed, KILL goes to it and to su's child's group, which
# no stop or tracer can hold off (:985-986, :1037-1064). The KILL is sent
# even when /proc shows Z, since a leader whose other threads live shows Z.
# The reader is not reaped here: a traced zombie is reported only to its
# tracer and a leader with live threads not at all (kernel/exit.c:1476-1484),
# so wait could block. Once this shell exits, its reaper takes it
# (:698-722).
# shellcheck disable=SC2317 # the EXIT trap calls it
stop_copy()
{
    [ -n "$copy_pid" ] || return 0
    kill -TERM "$copy_pid" 2>/dev/null || true
    kill -CONT "$copy_pid" 2>/dev/null || true
    n=50
    while [ "$n" -gt 0 ] && live "$copy_pid"; do
        sleep 0.1
        n=$((n - 1))
    done
    group=$(inner_group "$copy_pid")
    [ -z "$group" ] || kill -KILL "-$group" 2>/dev/null || true
    kill -KILL "$copy_pid" 2>/dev/null || true
    copy_pid=''
}

# stop_scene: end the scene's half, TERM and after 15 seconds KILL, without
# waiting for it. Under -u the half is USER's own shell, after the execs of
# setsid and BusyBox's su, which USER may trace; a traced child's end goes to
# its tracer, and the parent's wait passes over it until the tracer collects
# it (kernel/exit.c:1476-1484 of 6.12.115), so a wait could block. As with
# stop_copy, this shell's reaper takes the half once the shell exits.
# shellcheck disable=SC2317 # the traps call it
stop_scene()
{
    [ -n "$scene_pid" ] || return 0
    group=''
    [ "$under_su" = 0 ] || group=$(inner_group "$scene_pid")
    if [ -n "$group" ]; then term=-$group; else term=-$scene_pid; fi
    kill -TERM "$term" 2>/dev/null || true
    n=150
    while [ "$n" -gt 0 ] && kill -0 "$term" 2>/dev/null; do
        sleep 0.1
        n=$((n - 1))
    done
    if kill -0 "$term" 2>/dev/null; then kill -KILL "$term" 2>/dev/null || true; fi
    kill -KILL "-$scene_pid" 2>/dev/null || true
    scene_pid=''
}
trap 'stop_copy; stop_scene' EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

# OUT/taken gets root's copies of the scene's files, below.
mkdir -m 700 "$out/taken"

flags=''
[ "$own" = 1 ] || flags=-d
[ "$renderer" = gl ] || flags="$flags -p"
harness=$here inner_runners=$runners inner_replays=$replays
if [ -n "$user" ] && [ "$(id -u)" = 0 ]; then
    # USER may not read the caller's directories, as gfx cannot read
    # root's home on Debian: the scene's half runs from a copy under OUT.
    rm -rf "$out/harness"
    mkdir -p "$out/harness/runners" "$out/harness/replays"
    cp "$here/measure.sh" "$here/piglit.txt" "$here/webgl.html" "$out/harness/"
    cp -R "$runners/." "$out/harness/runners/"
    [ ! -d "$replays" ] || cp -R "$replays/." "$out/harness/replays/"
    # su runs `start`; measure\.sh in a stop pattern must have its dot
    # escaped, since measure/shm in OUT would also match an unescaped dot.
    harness=$out/harness inner_runners=$out/harness/runners inner_replays=$out/harness/replays
    cat > "$harness/start" << 'EOF'
MEASURE_RUNNERS=$1
export MEASURE_RUNNERS
shift
exec sh "${MEASURE_RUNNERS%/runners}/measure.sh" "$@"
EOF
    # The files root writes from here on exist before USER may write OUT.
    for f in counts line scene.log; do : > "$out/$f"; done
    chown -hR "$user" "$out/harness"
    chgrp "$(id -g "$user")" "$out"
    chmod 1770 "$out"
    under_su=1
    # shellcheck disable=SC2016 # USER's shell expands its own argument
    su -s /bin/sh -c 'test -r "$1"' -- "$user" sh "$harness/start" 2>/dev/null || {
        echo "measure: $scene: failed: $user cannot read $harness; give OUT a path $user can reach" >&2
        exit 1
    }
fi

[ "$marks" = 0 ] || echo "measure: counts: before"
{ echo before; counts; } > "$out/counts"
start=$(date +%s.%N)
status=0
if [ "$harness" != "$here" ]; then
    # su parses options among its arguments too; -- ends its options.
    # shellcheck disable=SC2086 # the flags are words
    setsid su -s /bin/sh -c 'exec sh "$@"' -- "$user" sh "$harness/start" "$inner_runners" \
        -I -r "$rung" -o "$out" -R "$inner_replays" ${seconds:+-t "$seconds"} $flags "$scene" \
        > "$out/scene.log" 2>&1 &
else
    # shellcheck disable=SC2086 # the flags are words
    MEASURE_RUNNERS=$inner_runners setsid sh "$here/measure.sh" -I -r "$rung" -o "$out" -R "$inner_replays" \
        ${seconds:+-t "$seconds"} $flags "$scene" > "$out/scene.log" 2>&1 &
fi
scene_pid=$!
wait "$scene_pid" || status=$?
scene_pid=''
end=$(date +%s.%N)
{ echo after; counts; } >> "$out/counts"
[ "$marks" = 0 ] || echo "measure: counts: after"
# The scene's files, USER's under -u, are read once, by USER, and only as
# plain files, into OUT/taken, which only root may enter: root opening a
# name USER may swap for a link, such as one to /dev/watchdog, would act
# with root's rights on what USER chose. Linux's protected_symlinks guards
# only a sticky directory that others may write (fs/namei.c:1127 of
# 6.12.115), and OUT is not one. A file the scene did not write is left out
# of OUT/taken. A swapped-in FIFO can block the reader; wait, unlike a
# foreground command, lets a trapped signal interrupt it (POSIX XCU 2.11).
# The EXIT trap ends that reader, TERM and CONT and after 5 seconds KILL,
# and does not wait for it, so a reader USER stopped or traces cannot hold
# the stop either.
for f in frametimes cpuframes passes; do
    g="$out/$f" rc=0
    # shellcheck disable=SC2016 # the reader's shell expands its argument
    take='if [ -h "$1" ] || { [ -e "$1" ] && [ ! -f "$1" ]; }; then exit 3; fi; [ -e "$1" ] || exit 4; exec cat "$1"'
    if [ "$under_su" = 1 ]; then
        su -s /bin/sh -c "$take" -- "$user" sh "$g" > "$out/taken/$f" 2>/dev/null &
    else
        sh -c "$take" sh "$g" > "$out/taken/$f" 2>/dev/null &
    fi
    copy_pid=$!
    wait "$copy_pid" || rc=$?
    copy_pid=''
    case $rc in
        0) ;;
        4) rm -f "$out/taken/$f" ;;
        3)
            echo "measure: $scene: failed: OUT/$f is not a plain file" >&2
            exit 1
            ;;
        *)
            echo "measure: $scene: failed: OUT/$f could not be read" >&2
            exit 1
            ;;
    esac
done
if [ "$status" -ne 0 ] || [ ! -s "$out/taken/frametimes" ]; then
    echo "measure: $scene: failed: $(tail -n 3 "$out/scene.log" | tr '\n' ' ')" >&2
    exit 1
fi

copied=$(delta virtio_gpu_copied_bytes) exits=$(delta kvm_exits) trips=$(delta rung)
passed=- cpuframes=$(grep -c . "$out/taken/frametimes")
[ ! -s "$out/taken/cpuframes" ] || cpuframes=$(cat "$out/taken/cpuframes")
[ ! -f "$out/taken/passes" ] || passed=$(grep -c 1 "$out/taken/passes")/$(grep -c . "$out/taken/passes")
[ "$exits" != - ] || exits=$(delta linux_exits)
if [ -n "$relay" ]; then
    copied=$(delta relay_bytes) trips=$(delta relay_commands)
fi
tick=$(getconf CLK_TCK 2>/dev/null || echo 100)
sort -g "$out/taken/frametimes" | awk -v busy="$(delta busy)" -v tick="$tick" -v scene="$scene" -v rung="$rung" \
    -v lat="$lat" -v copied="$copied" -v exits="$exits" -v trips="$trips" -v s="$start" -v e="$end" \
    -v cpuframes="$cpuframes" -v passed="$passed" '
    { t[NR] = $1; sum += $1 }
    END {
        p50 = t[int((NR - 1) * 0.50) + 1]; p99 = t[int((NR - 1) * 0.99) + 1]
        printf "measure: result: %s %s frames %d frame_ms %.3f p50 %.3f p99 %.3f cpu_ms_per_frame %.3f latency_ms %s copied_bytes %s exits %s round_trips %s seconds %.1f passed %s\n",
            scene, rung, NR, sum / NR, p50, p99, busy * 1000 / tick / cpuframes, lat, copied, exits, trips, e - s, passed
    }' | tee "$out/line"
