#!/bin/sh
# lenovo.sh: the native games' fixed replays on the GNU root's own desktop
# session, on the panel, judged against Linux's frame times on the Lenovo.
#
#   sh lenovo.sh [-n RUNS] [-s WxH] [-t SECONDS] [-r RUNNERS] [-u USER]
#       OUT [GAME...]
#
# Run as root on a GNU root whose desktop session runs (vendor/gnu/desktop),
# by hand or as the native cases games/GAME (tests/native/gpu.cases):
# Weston's kiosk-shell on the DRM device as USER (desktop), with PulseAudio
# on /dev/dsp. Each GAME, by default supertuxkart, 0ad and supertuxkart-i386,
# plays its replay RUNS times (2), one run after another, through RUNNERS'
# games.sh -d (/tmp/graphics/runners) as USER, fullscreen at WxH (1366x768,
# the panel's mode) on the session's display, as Linux's runs on the Lenovo
# played them under cage (dev-log/80-games/80-2.md). -s and -t give games.sh
# its small form, for a machine that draws a frame in seconds; a run of the
# small form is not judged.
#
# Mesa's shader cache is OUT/cache, which every run shares, so that a
# game's first run starts with it empty and its second finds it warm, as
# Linux's runs found it.
#
# The volume is set to 1 before each game, and again 5 s and 30 s after its
# start, once the game has opened its sound; `mixer vol` is read every 10 s
# while it plays and logged, not changed. The GPU's clock,
# gt_act_freq_mhz, is read every second, as Linux's runs read theirs.
#
# OUT gets the record of the session, `session`, the volume's log, `volume`,
# and for each run a directory GAME-N with games.sh's output, its summary
# and frame times, `freq`, the clock's samples, `rps-boost-info`, the GPU's
# boost counters when i915 exposes them, and `dmesg`, the kernel's log
# at the run's end, which holds the lines of rach2.trace=enosys before the
# log's 16 KiB turn over. Each run prints its
# games.sh result line and then one line:
#
#   lenovo-games: result: GAME-N median A p90 B p99 C against Linux's
#       A' B' C': pass | fail | not judged
#
# where pass means each of the three percentiles is at most 10% over
# Linux's, the mean of its two Wayland runs. The i386 game, which Linux did
# not run, is set beside the 64-bit game's numbers and not judged, nor is
# the small form. The
# last line is `lenovo-games: result: pass` or `fail`, and the status 0 when
# every run reached its end and every judged run passed.
set -eu

runs=2 size=1366x768 seconds='' runners=/tmp/graphics/runners user=desktop
while getopts n:s:t:r:u: o; do
    case $o in
        n) runs=$OPTARG ;;
        s) size=$OPTARG ;;
        t) seconds=$OPTARG ;;
        r) runners=$OPTARG ;;
        u) user=$OPTARG ;;
        *) exit 2 ;;
    esac
done
shift $((OPTIND - 1))
[ $# -ge 1 ] || {
    echo "usage: lenovo.sh [-n RUNS] [-s WxH] [-t SECONDS] [-r RUNNERS] [-u USER] OUT [GAME...]" >&2
    exit 2
}
out=$1
shift
[ $# -ge 1 ] || set -- supertuxkart 0ad supertuxkart-i386
[ "$(id -u)" = 0 ] || { echo "lenovo-games: run as root" >&2; exit 2; }
[ -r "$runners/games.sh" ] || { echo "lenovo-games: no $runners/games.sh" >&2; exit 1; }
# The user's shell runs games.sh from the run's own directory, so both
# paths are made absolute, as a native case gives them relative to its
# workspace (tests/native/gpu.cases).
runners=$(cd "$runners" && pwd)
mkdir -p "$out"
out=$(cd "$out" && pwd)
uid=$(id -u "$user")
run=/run/user/$uid
[ -S "$run/wayland-0" ] || { echo "lenovo-games: no desktop session: no $run/wayland-0" >&2; exit 1; }
judged=1
[ "$size" = 1366x768 ] && [ -z "$seconds" ] || judged=0

# Linux's median, 90th and 99th percentiles in ms on the Lenovo, the mean of
# its two Wayland runs under cage with translation off (80-2.md).
linux()
{
    case $1 in
        supertuxkart | supertuxkart-i386) echo 14.72 16.96 19.14 ;;
        0ad) echo 17.06 19.40 36.135 ;;
    esac
}

mkdir -p "$out"
chmod 0755 "$out"
rm -rf "$out/cache"
install -d -o "$user" -m 0755 "$out/cache"
freq=''
for f in /sys/class/drm/card*/gt_act_freq_mhz; do
    [ -r "$f" ] && freq=$f && break
done
{
    echo "cmdline $(cat /proc/cmdline)"
    echo "kernel $(uname -sr)"
    echo "user $user uid $uid"
    ls -l "$run/wayland-0" "$run/pulse/native" 2>&1
    echo "freq ${freq:-none}"
    grep -E 'Loading module|DRM backend|GL renderer|Output .* mode|kiosk' \
        /var/log/services 2>/dev/null | tail -n 20
    echo "volume $(mixer vol)"
} > "$out/session"

sample=''
volumes=''
# stop: end the samplers.
stop()
{
    for p in $sample $volumes; do
        kill "$p" 2>/dev/null || :
        wait "$p" 2>/dev/null || :
    done
    sample='' volumes=''
}
trap stop EXIT

failed=0
for game in "$@"; do
    n=1
    while [ "$n" -le "$runs" ]; do
        dir=$out/$game-$n
        rm -rf "$dir"
        install -d -o "$user" -m 0755 "$dir"
        mixer vol 1 > /dev/null
        echo "$(date +%T) $game-$n start, volume $(mixer vol)" >> "$out/volume"
        if [ -n "$freq" ]; then
            (while :; do echo "$(cat /proc/uptime | cut -d' ' -f1) $(cat "$freq")"; sleep 1; done) \
                > "$dir/freq" &
            sample=$!
        fi
        # shellcheck disable=SC2016 # the user's shell expands the words
        su -s /bin/sh "$user" -c 'cd "$1" && XDG_RUNTIME_DIR=$2 WAYLAND_DISPLAY=wayland-0 \
            PULSE_SERVER=unix:$2/pulse/native MESA_SHADER_CACHE_DIR=$7 \
            exec sh "$3/games.sh" -d -s "$4" ${5:+-t "$5"} -o "$1" "$6"' \
            lenovo "$dir" "$run" "$runners" "$size" "$seconds" "$game" "$out/cache" \
            > "$dir/lenovo.out" 2>&1 < /dev/null &
        game_pid=$!
        (
            t=0
            while kill -0 "$game_pid" 2>/dev/null; do
                sleep 5
                t=$((t + 5))
                case $t in
                    5 | 30) mixer vol 1 > /dev/null ;;
                esac
                [ $((t % 10)) -ne 0 ] || echo "$(date +%T) $game-$n ${t}s volume $(mixer vol)"
            done
        ) >> "$out/volume" &
        volumes=$!
        status=0
        wait "$game_pid" || status=$?
        stop
        echo "$(date +%T) $game-$n end, status $status, volume $(mixer vol)" >> "$out/volume"
        dmesg > "$dir/dmesg" 2>&1 || :
        if [ -r /sys/kernel/debug/dri/0/i915_rps_boost_info ]; then
            cat /sys/kernel/debug/dri/0/i915_rps_boost_info > "$dir/rps-boost-info"
        fi
        line=$(grep 'games: result: ' "$dir/lenovo.out" | tail -n 1) || line=''
        if [ "$status" -ne 0 ] || [ -z "$line" ]; then
            echo "lenovo-games: result: $game-$n fail: games.sh status $status: $(tail -n 3 "$dir/lenovo.out")"
            failed=1
            n=$((n + 1))
            continue
        fi
        echo "$line"
        gated=$judged
        [ "$game" != supertuxkart-i386 ] || gated=0
        verdict=$(awk -v judged="$gated" -v ref="$(linux "$game")" '
            $1 == "median" || $1 == "p90" || $1 == "p99" { v[$1] = $2 }
            END {
                split(ref, l, " ")
                ok = v["median"] <= 1.1 * l[1] && v["p90"] <= 1.1 * l[2] && v["p99"] <= 1.1 * l[3]
                printf "median %s p90 %s p99 %s against Linux'"'"'s %s %s %s: %s\n",
                    v["median"], v["p90"], v["p99"], l[1], l[2], l[3],
                    judged ? (ok ? "pass" : "fail") : "not judged"
            }' "$dir/summary")
        echo "lenovo-games: result: $game-$n $verdict"
        case $verdict in *': fail') failed=1 ;; esac
        n=$((n + 1))
    done
done
if [ "$failed" = 0 ]; then
    echo "lenovo-games: result: pass"
else
    echo "lenovo-games: result: fail"
    exit 1
fi
