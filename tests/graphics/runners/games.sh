#!/bin/sh
# games.sh: a native game's fixed replay with its frame times, inside a
# graphics guest or on a machine's own display, as an ordinary user, the way
# Linux's reference runs on the Lenovo took them.
#
#   games.sh [-d] [-s WxH] [-o OUT] [-r REPLAYS] supertuxkart|0ad
#
# Plays the game's fixed replay of REPLAYS (/usr/share/games/rach2/replays,
# which the games layer installs) once, fullscreen at WxH (1280x800) under a
# headless Weston of its own, or with -d on the caller's own Wayland display:
#
#   supertuxkart  SuperTuxKart's profile mode, its benchmark, with the
#                 arguments of supertuxkart.args: AI karts drive one track for
#                 a fixed span of the race's own time, and the game ends
#                 itself with its profile summary.
#   0ad           0 A.D.'s visual replay of 0ad-mainland/commands.txt, a
#                 recorded match of two AI players, played for 150 s after
#                 its loading screen and then ended.
#
# These are the replays, arguments, settings and environment of
# tools/lenovo/debian-ref/games.sh, which took Linux's numbers, and the
# measured window and figures are those of its frames.py, so that the two
# compare. Each game starts with a new home, so that no setting, save or cache
# of an earlier run is reused; Mesa's shader cache is guest-env.sh's. Run it
# with guest-env.sh's performance class.
#
# The frame times come from Mesa's Gallium HUD, hidden, which writes, at each
# frame a GL context presents, the time since the last in milliseconds to a
# file of GALLIUM_HUD_DUMP_DIR: the same on llvmpipe and on Crocus. The
# window starts 5 s after the last frame of 1 s or more in the dump's first
# 120 s, the end of the loading screen, and lasts to the end of the dump for
# SuperTuxKart and 150 s for 0 A.D.; games.sh ends 0 A.D. once the dump holds
# that whole window.
#
# OUT (.) gets the record, the game's log, `hud`, every frame time the HUD
# wrote, one a line, `frametimes`, the window's, and `summary`, a line each:
# the window's first and last frame of the dump and its seconds, the frames,
# the mean frame rate, the median, 90th, 99th and 99.9th percentiles and the
# largest frame time, in milliseconds, and the frames over 16.7, 33.3, 50 and
# 100 ms. It prints one line:
#
#   games: result: GAME frames N seconds S fps F median A p90 B p99 C
#       p99.9 D max E over50 G end END
#
# END is `profile`, SuperTuxKart's summary printed, or `window`, 0 A.D. still
# playing once the window was whole. The status is 0 when the game reached
# that end and the window has frames, and 1 otherwise; games.sh sets no time
# limit of its own.
set -eu

RUNNER_DIR=$(cd "$(dirname "$0")" && pwd)
# shellcheck source=common.sh
. "$RUNNER_DIR/common.sh"

own=1 out=. replays=/usr/share/games/rach2/replays size=
while getopts ds:o:r: o; do
    case $o in
        d) own=0 ;;
        s) size=$OPTARG ;;
        o) out=$OPTARG ;;
        r) replays=$OPTARG ;;
        *) exit 2 ;;
    esac
done
shift $((OPTIND - 1))
[ $# -eq 1 ] || {
    echo "usage: games.sh [-d] [-s WxH] [-o OUT] [-r REPLAYS] supertuxkart|0ad" >&2
    exit 2
}
game=$1
PATH=$PATH:/usr/games
case $game in
    supertuxkart) program=supertuxkart span= ;;
    0ad) program=pyrogenesis span=150 ;;
    *) echo "games: no game $game; supertuxkart or 0ad" >&2; exit 2 ;;
esac
command -v "$program" > /dev/null || { echo "games: no $program" >&2; exit 1; }

runner_init games "$out"
RUNNER_GAME=''
if [ -n "$size" ]; then
    RUNNER_WIDTH=${size%x*}
    RUNNER_HEIGHT=${size#*x}
fi
size=${RUNNER_WIDTH}x$RUNNER_HEIGHT
runner_start plain
if [ "$own" = 1 ]; then
    runner_weston
else
    [ -n "${WAYLAND_DISPLAY:-}" ] || runner_die "-d needs WAYLAND_DISPLAY"
    runner_record "compositor the caller's, $WAYLAND_DISPLAY"
fi
runner_record "size $size"

# game_stop: end the game's session, if it runs, and wait for it.
game_stop()
{
    [ -n "$RUNNER_GAME" ] || return 0
    kill -TERM "-$RUNNER_GAME" 2>/dev/null || true
    tries=0
    while kill -0 "$RUNNER_GAME" 2>/dev/null && [ "$tries" -lt 100 ]; do
        sleep 0.1
        tries=$((tries + 1))
    done
    kill -KILL "-$RUNNER_GAME" 2>/dev/null || true
    wait "$RUNNER_GAME" 2>/dev/null || true
    RUNNER_GAME=
}
trap 'game_stop; runner_cleanup' EXIT

# A new home for the game, under the work directory, which the exit trap
# removes.
HOME=$RUNNER_WORK/home
mkdir -p "$HOME" "$RUNNER_WORK/hud"
export HOME
runner_record "cache game a new home"

# SDL on Wayland without libdecor, whose path draws nothing under cage, as
# the Linux runs had it; GL from the guest's Mesa through EGL. The HUD draws
# nothing and writes every frame.
SDL_VIDEODRIVER=wayland
SDL_VIDEO_WAYLAND_ALLOW_LIBDECOR=0
GALLIUM_HUD=frametime
GALLIUM_HUD_VISIBLE=false
GALLIUM_HUD_PERIOD=0
GALLIUM_HUD_DUMP_DIR=$RUNNER_WORK/hud
LANG=C.UTF-8
export SDL_VIDEODRIVER SDL_VIDEO_WAYLAND_ALLOW_LIBDECOR GALLIUM_HUD GALLIUM_HUD_VISIBLE \
    GALLIUM_HUD_PERIOD GALLIUM_HUD_DUMP_DIR LANG
for v in SDL_VIDEODRIVER SDL_VIDEO_WAYLAND_ALLOW_LIBDECOR GALLIUM_HUD GALLIUM_HUD_VISIBLE \
    GALLIUM_HUD_PERIOD LANG; do
    runner_record "env $v=$(printenv "$v")"
done

# frames [SPAN]: the window's frame times, one a line, after a line `# FIRST
# LAST` that gives its bounds in the dump, as frames.py chooses them; nothing
# while the dump does not yet reach the window's end, when SPAN is given.
frames()
{
    cat "$RUNNER_WORK/hud/"* 2>/dev/null | awk -v span="${1:-}" '
        NF { v[++n] = $NF }
        END {
            t = 0; last = 0
            for (i = 1; i <= n && t <= 120000; i++) {
                if (v[i] >= 1000) last = i
                t += v[i]
            }
            t = 0
            for (i = last + 1; i <= n && t < 5000; i++) t += v[i]
            start = i; t = 0
            for (; i <= n && (span == "" || t < span * 1000); i++) t += v[i]
            if (span != "" && t < span * 1000) exit
            printf "# %d %d\n", start - 1, i - 1
            for (j = start; j < i; j++) print v[j]
        }'
}

log=$out/$game.log
case $game in
    supertuxkart)
        args=$replays/supertuxkart.args
        [ -r "$args" ] || runner_die "no $args"
        words="$(grep -v '^#' "$args" | tr '\n' ' ')--fullscreen --screensize=$size"
        runner_record "case supertuxkart the profile race of $args"
        runner_record "game $(supertuxkart --version 2>&1 |
            sed -n 's/.*\(SuperTuxKart, [0-9][^ ]*\)\.$/\1/p' | head -n 1)"
        runner_record "arguments $words"
        # shellcheck disable=SC2086 # the arguments are words
        supertuxkart $words > "$log" 2>&1 < /dev/null ||
            runner_die "supertuxkart failed: $(tail -n 3 "$log")"
        grep -q 'Number of frames: ' "$log" ||
            runner_die "supertuxkart printed no profile summary: $(tail -n 3 "$log")"
        runner_record "profile $(sed -n 's/.*\(Number of frames: .*\)/\1/p' "$log" | tail -n 1)"
        end=profile
        ;;
    0ad)
        replay=$replays/0ad-mainland/commands.txt
        [ -r "$replay" ] || runner_die "no $replay"
        runner_record "case 0ad the visual replay of $replay for $span s after its loading screen"
        runner_record "replay $(grep -c '^turn ' "$replay") turns, sha256 $(sha256sum "$replay" | cut -d' ' -f1)"
        runner_record "game $(pyrogenesis -version 2>/dev/null | head -n 1)"
        words="-replay-visual=$replay -conf=windowed:false -conf=xres:$RUNNER_WIDTH"
        words="$words -conf=yres:$RUNNER_HEIGHT -conf=sound.mastergain:0"
        runner_record "arguments $words"
        # shellcheck disable=SC2086 # the arguments are words
        setsid 0ad $words > "$log" 2>&1 < /dev/null &
        RUNNER_GAME=$!
        until [ -n "$(frames "$span" | head -n 1)" ]; do
            kill -0 "$RUNNER_GAME" 2>/dev/null ||
                runner_die "0 A.D. ended before its window was whole: $(tail -n 3 "$log")"
            sleep 1
        done
        game_stop
        end=window
        ;;
esac
runner_record "renderer $(sed -n 's/.*\(GL_RENDERER\|OpenGL renderer\)[^:=]*[:=] *//p' "$log" | head -n 1)"
runner_finish

[ -n "$(ls "$RUNNER_WORK/hud")" ] || runner_die "the HUD wrote no frame times"
cat "$RUNNER_WORK/hud/"* > "$out/hud"
frames "$span" > "$RUNNER_WORK/window"
sed 1d "$RUNNER_WORK/window" > "$out/frametimes"
[ -s "$out/frametimes" ] || runner_die "no frame in the window"
# frames.py's figures: of the window's sorted times, the one at the index
# that the quantile times the count gives, counted from 0, never past the
# largest.
sort -g "$out/frametimes" | awk -v bounds="$(sed -n '1s/^# //p' "$RUNNER_WORK/window")" '
    BEGIN { lim[1] = 16.7; lim[2] = 33.3; lim[3] = 50; lim[4] = 100 }
    { s[NR] = $1; t += $1; for (k = 1; k <= 4; k++) if ($1 > lim[k]) over[k]++ }
    function p(q,   i) { i = int(q * NR) + 1; return s[i > NR ? NR : i] }
    END {
        printf "window %s\nseconds %.3f\nframes %d\nfps %.2f\n", bounds, t / 1000, NR, NR * 1000 / t
        printf "median %.2f\np90 %.2f\np99 %.2f\np99.9 %.2f\nmax %.2f\n",
            p(0.5), p(0.9), p(0.99), p(0.999), s[NR]
        printf "over16.7 %d\nover33.3 %d\nover50 %d\nover100 %d\n", over[1] + 0, over[2] + 0,
            over[3] + 0, over[4] + 0
    }' > "$out/summary"
echo "end $end" >> "$out/summary"
v()
{
    awk -v k="$1" '$1 == k { print $2 }' "$out/summary"
}
echo "games: result: $game frames $(v frames) seconds $(v seconds) fps $(v fps) median $(v median)" \
    "p90 $(v p90) p99 $(v p99) p99.9 $(v p99.9) max $(v max) over50 $(v over50) end $end" |
    tee -a "$out/results"
