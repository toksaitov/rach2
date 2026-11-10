#!/bin/sh
# games.sh: a native game's fixed replay with its frame times, inside a
# graphics guest, Rach2's or Debian's, or on a machine's own display, as an
# ordinary user.
#
#   games.sh [-d] [-o OUT] [-r REPLAYS] supertuxkart|0ad
#
# Plays the game's fixed replay of REPLAYS (/usr/share/games/rach2/replays,
# which the games layer installs) once, fullscreen at 1280x800 under a
# headless Weston of its own, or with -d on the caller's own Wayland display
# at its size:
#
#   supertuxkart  SuperTuxKart's profile mode, its benchmark, on the race
#                 that supertuxkart.args gives: AI karts drive a fixed number
#                 of laps of one track and the game ends itself, printing the
#                 frames it drew.
#   0ad           0 A.D.'s visual replay of 0ad/commands.txt, a recorded match
#                 of two AI players, with the mod 0ad/rach2-replay, which
#                 ends the game at the replay's last turn.
#
# Each game starts with a new configuration and a new home, so that no
# setting or cache of an earlier run is reused. The frame times come from
# Mesa's Gallium HUD, hidden, whose `frametime` graph writes the time from
# each presented frame to the next, in milliseconds, to a file of
# GALLIUM_HUD_DUMP_DIR: the same on llvmpipe and on Crocus, in QEMU and on
# Linux. The HUD times the loading screens too, so the replay's frames are
# the last ones, back to the game's own measure of the replay's wall time:
# SuperTuxKart's race time from its profile summary, and the time from the
# session's start to the last turn that the mod prints. Run it with
# guest-env.sh's performance class.
#
# OUT (.) gets the record, the game's log, `hud`, every frame time the HUD
# wrote, one a line in milliseconds, `frametimes`, the replay's frames of it,
# and `summary`, the count of those frames, their sum, their 50th, 90th and
# 99th percentiles and the largest, and the frames over 33.3 ms and 50 ms. It
# prints one line:
#
#   games: result: GAME frames N seconds S p50 A p90 B p99 C max D
#       over50 E end END
#
# END is the game's own word that it reached the replay's end: SuperTuxKart's
# race time, `race=SECONDS`, from its profile summary, and 0 A.D.'s last
# turn, `turn=T`, that the mod prints. The status is 0 when the game reached
# that end and wrote frame times, and 1 otherwise; the game runs to its own
# end and games.sh sets no time limit of its own.
set -eu

RUNNER_DIR=$(cd "$(dirname "$0")" && pwd)
# shellcheck source=common.sh
. "$RUNNER_DIR/common.sh"

own=1 out=. replays=/usr/share/games/rach2/replays
while getopts do:r: o; do
    case $o in
        d) own=0 ;;
        o) out=$OPTARG ;;
        r) replays=$OPTARG ;;
        *) exit 2 ;;
    esac
done
shift $((OPTIND - 1))
[ $# -eq 1 ] || { echo "usage: games.sh [-d] [-o OUT] [-r REPLAYS] supertuxkart|0ad" >&2; exit 2; }
game=$1
PATH=$PATH:/usr/games
case $game in
    supertuxkart) program=supertuxkart ;;
    0ad) program=pyrogenesis ;;
    *) echo "games: no game $game; supertuxkart or 0ad" >&2; exit 2 ;;
esac
command -v "$program" > /dev/null || { echo "games: no $program" >&2; exit 1; }

runner_init games "$out"
runner_start plain
if [ "$own" = 1 ]; then
    runner_weston
    size=${RUNNER_WIDTH}x$RUNNER_HEIGHT
else
    [ -n "${WAYLAND_DISPLAY:-}" ] || runner_die "-d needs WAYLAND_DISPLAY"
    runner_record "compositor the caller's, $WAYLAND_DISPLAY"
    size=display
fi
runner_record "size $size"

# A new home, configuration and data directory for the game, under the work
# directory, which the exit trap removes.
HOME=$RUNNER_WORK/home
XDG_CONFIG_HOME=$HOME/.config
XDG_DATA_HOME=$HOME/.local/share
XDG_CACHE_HOME=$HOME/.cache
export HOME XDG_CONFIG_HOME XDG_DATA_HOME XDG_CACHE_HOME
mkdir -p "$XDG_CONFIG_HOME" "$XDG_DATA_HOME" "$XDG_CACHE_HOME" "$RUNNER_WORK/hud"
runner_record "cache game a new home, configuration and data directory"

# SDL draws on Wayland; GL comes from the guest's Mesa through EGL.
SDL_VIDEODRIVER=wayland
GALLIUM_HUD=frametime
GALLIUM_HUD_VISIBLE=false
GALLIUM_HUD_DUMP_DIR=$RUNNER_WORK/hud
export SDL_VIDEODRIVER GALLIUM_HUD GALLIUM_HUD_VISIBLE GALLIUM_HUD_DUMP_DIR
runner_record "env SDL_VIDEODRIVER=$SDL_VIDEODRIVER"
runner_record "frametimes Mesa's Gallium HUD, GALLIUM_HUD=frametime GALLIUM_HUD_VISIBLE=false"

log=$out/$game.log
case $game in
    supertuxkart)
        args=$replays/supertuxkart.args
        [ -r "$args" ] || runner_die "no $args"
        words=$(grep -v '^#' "$args" | tr '\n' ' ')
        if [ "$size" = display ]; then
            words="$words --fullscreen"
        else
            words="$words --fullscreen --screensize=$size"
        fi
        runner_record "case supertuxkart the profile race of $args"
        runner_record "game $(supertuxkart --version 2>/dev/null | grep -i 'supertuxkart' | head -n 1)"
        runner_record "arguments $words"
        # shellcheck disable=SC2086 # the arguments are words
        supertuxkart $words > "$log" 2>&1 ||
            runner_die "supertuxkart failed: $(tail -n 3 "$log")"
        race=$(sed -n 's/.*Number of frames: [0-9]* time \([0-9.]*\),.*/\1/p' "$log" | tail -n 1)
        [ -n "$race" ] || runner_die "supertuxkart printed no profile summary: $(tail -n 3 "$log")"
        end="race=$race"
        window=$race
        renderer=$(sed -n 's/.*OpenGL renderer: *//p; s/.*GL_RENDERER: *//p' "$log" | head -n 1)
        ;;
    0ad)
        replay=$replays/0ad/commands.txt
        [ -r "$replay" ] || runner_die "no $replay"
        mods=$XDG_DATA_HOME/0ad/mods
        mkdir -p "$mods"
        cp -R "$replays/0ad/rach2-replay" "$mods/rach2-replay"
        last=$(grep -c '^turn ' "$replay")
        runner_record "case 0ad the visual replay of $replay, $last turns"
        runner_record "game $(pyrogenesis -version 2>/dev/null | head -n 1)"
        words="-mod=public -mod=rach2-replay -replay-visual=$replay"
        if [ "$size" != display ]; then
            words="$words -xres=$RUNNER_WIDTH -yres=$RUNNER_HEIGHT"
        fi
        runner_record "arguments $words"
        # shellcheck disable=SC2086 # the arguments are words
        pyrogenesis $words > "$log" 2>&1 ||
            runner_die "pyrogenesis failed: $(tail -n 3 "$log")"
        turn=$(sed -n 's/.*rach2-replay: end: turn \([0-9]*\) .*/\1/p' "$log" | tail -n 1)
        us=$(sed -n 's/.*rach2-replay: end: .* us \([0-9]*\).*/\1/p' "$log" | tail -n 1)
        [ -n "$turn" ] && [ -n "$us" ] ||
            runner_die "0 A.D. did not reach the replay's end: $(tail -n 3 "$log")"
        [ "$turn" = "$last" ] || runner_die "0 A.D. ended at turn $turn of $last"
        end="turn=$turn"
        window=$(awk -v us="$us" 'BEGIN { printf "%.6f", us / 1e6 }')
        renderer=$(sed -n 's/.*GL_RENDERER *[:=] *//p' "$log" | head -n 1)
        ;;
esac
runner_record "renderer ${renderer:-unknown}"
runner_finish

[ -s "$RUNNER_WORK/hud/frametime" ] || runner_die "the HUD wrote no frame times"
cp "$RUNNER_WORK/hud/frametime" "$out/hud"
# The replay's frames: the last of the HUD's whose times add up to the
# window, in the order they were drawn.
awk -v ms="$window" '{ t[NR] = $1 }
    END {
        ms *= 1000
        for (i = NR; i > 0 && sum < ms; i--) sum += t[i]
        for (j = i + 1; j <= NR; j++) print t[j]
    }' "$out/hud" > "$out/frametimes"
[ -s "$out/frametimes" ] || runner_die "no frame of the replay's window"
runner_record "window $window seconds, $(wc -l < "$out/frametimes") of the HUD's $(wc -l < "$out/hud") frames"
sort -n "$out/frametimes" | awk -v end="$end" '
    { t[NR] = $1; sum += $1; if ($1 > 33.3) o33++; if ($1 > 50) o50++ }
    function p(q,   i) { i = int(q * NR + 0.999999); if (i < 1) i = 1; return t[i] }
    END {
        printf "frames %d\nseconds %.3f\np50 %.3f\np90 %.3f\np99 %.3f\nmax %.3f\nover33 %d\nover50 %d\nend %s\n",
            NR, sum / 1000, p(0.5), p(0.9), p(0.99), t[NR], o33, o50, end
    }' > "$out/summary"
v()
{
    awk -v k="$1" '$1 == k { print $2 }' "$out/summary"
}
echo "games: result: $game frames $(v frames) seconds $(v seconds) p50 $(v p50) p90 $(v p90)" \
    "p99 $(v p99) max $(v max) over50 $(v over50) end $end" | tee -a "$out/results"
