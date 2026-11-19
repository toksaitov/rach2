#!/bin/sh
# free.sh [-s WxH] [-t SECONDS] OUT CACHE [GAME]: one replay of GAME
# (supertuxkart) on the desktop session with the GPU's clock left to RPS,
# for 80-7b's step on the Lenovo, where lenovo.sh pins the 64-bit games'
# minimum at 1000 MHz. Run as root after lenovo.sh, with the runners in
# /tmp/graphics/runners and CACHE, lenovo.sh's Mesa shader cache, so that
# the run starts warm as lenovo.sh's second runs do.
#
# It starts the desktop's session by SIGHUP when none runs and ends the one
# it started by SIGUSR2, as lenovo.sh does; holds the volume at 1; samples
# gt_act_freq_mhz every second into OUT/GAME-free/freq; reads
# i915_rps_boost_info before and after; and prints
#
#   free: result: GAME median A p90 B p99 C, min M MHz, at 1000 MHz K of N
#       samples, mean F MHz
#
# with the status of games.sh. -s and -t give games.sh its small form.
set -eu

size=1366x768 seconds=''
while getopts s:t: o; do
    case $o in
        s) size=$OPTARG ;;
        t) seconds=$OPTARG ;;
        *) exit 2 ;;
    esac
done
shift $((OPTIND - 1))
[ $# -ge 2 ] || { echo 'usage: free.sh [-s WxH] [-t SECONDS] OUT CACHE [GAME]' >&2; exit 2; }
out=$1 cache=$2 game=${3:-supertuxkart}
runners=/tmp/graphics/runners user=desktop
uid=$(id -u "$user")
run=/run/user/$uid
pidfile=/run/rach2-desktop.pid
started=0 sample='' volumes=''
freq=''
for f in /sys/class/drm/card*/gt_act_freq_mhz; do
    [ -r "$f" ] && freq=$f && break
done
[ -n "$freq" ] || { echo 'free: no GPU clock' >&2; exit 1; }
boost=/sys/kernel/debug/dri/0/i915_rps_boost_info

# cleanup: end the samplers, and the session if this script started it.
cleanup()
{
    for p in $sample $volumes; do
        kill "$p" 2> /dev/null || :
        wait "$p" 2> /dev/null || :
    done
    if [ "$started" = 1 ] && [ -f "$pidfile" ]; then
        kill -USR2 "$(cat "$pidfile")" 2> /dev/null || :
        tries=0
        while pgrep -x weston > /dev/null && [ "$tries" -lt 600 ]; do
            tries=$((tries + 1))
            sleep 0.1
        done
    fi
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

# wait_for TEST...: run TEST every 0.1 s until it passes, 600 tries at most.
wait_for()
{
    tries=0
    until "$@"; do
        tries=$((tries + 1))
        [ "$tries" -lt 600 ] || return 1
        sleep 0.1
    done
}
session_up() { [ -S "$run/wayland-0" ] && pgrep -x weston > /dev/null; }
if ! session_up; then
    wait_for test -s "$pidfile" || { echo 'free: no supervisor' >&2; exit 1; }
    started=1
    kill -HUP "$(cat "$pidfile")"
    wait_for session_up || { echo "free: no desktop session: no $run/wayland-0" >&2; exit 1; }
fi
wait_for test -S "$run/pulse/native" || { echo "free: no sound server: no $run/pulse/native" >&2; exit 1; }

dir=$out/$game-free
rm -rf "$dir"
mkdir -p "$out"
install -d -o "$user" -m 0755 "$dir"
min=$(cat "${freq%gt_act_freq_mhz}gt_min_freq_mhz")
[ ! -r "$boost" ] || cat "$boost" > "$dir/rps-boost-before"
mixer vol 1 > /dev/null
(while :; do echo "$(cut -d' ' -f1 /proc/uptime) $(cat "$freq")"; sleep 1; done) > "$dir/freq" &
sample=$!
# shellcheck disable=SC2016 # the user's shell expands the words
su -s /bin/sh "$user" -c 'cd "$1" && XDG_RUNTIME_DIR=$2 WAYLAND_DISPLAY=wayland-0 \
    PULSE_SERVER=unix:$2/pulse/native MESA_SHADER_CACHE_DIR=$7 \
    exec sh "$3/games.sh" -d -s "$4" ${5:+-t "$5"} -o "$1" "$6"' \
    free "$dir" "$run" "$runners" "$size" "$seconds" "$game" "$cache" \
    > "$dir/free.out" 2>&1 < /dev/null &
game_pid=$!
(
    t=0
    while kill -0 "$game_pid" 2> /dev/null; do
        sleep 5
        t=$((t + 5))
        case $t in
            5 | 30) mixer vol 1 > /dev/null ;;
        esac
        [ $((t % 10)) -ne 0 ] || echo "$(date +%T) $game-free ${t}s volume $(mixer vol)"
    done
) > "$dir/volume" &
volumes=$!
status=0
wait "$game_pid" || status=$?
for p in $sample $volumes; do
    kill "$p" 2> /dev/null || :
    wait "$p" 2> /dev/null || :
done
sample='' volumes=''
[ ! -r "$boost" ] || cat "$boost" > "$dir/rps-boost-after"
dmesg > "$dir/dmesg" 2>&1 || :
grep 'games: result: ' "$dir/free.out" | tail -n 1 || :
awk -v game="$game" -v min="$min" '
    FILENAME == ARGV[1] { if ($1 == "median" || $1 == "p90" || $1 == "p99") v[$1] = $2; next }
    { n++; sum += $2; if ($2 >= 1000) k++ }
    END {
        printf "free: result: %s median %s p90 %s p99 %s, min %s MHz, at 1000 MHz %d of %d samples, mean %.0f MHz\n",
            game, v["median"], v["p90"], v["p99"], min, k, n, n ? sum / n : 0
    }' "$dir/summary" "$dir/freq" 2> /dev/null || echo "free: result: $game no summary"
echo "free: games.sh status $status"
exit "$status"
