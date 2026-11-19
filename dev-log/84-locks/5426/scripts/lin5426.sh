#!/bin/sh
# 5426's loops on the Lenovo's Debian, as root, as run5426.sh runs them on
# Rach2: three loops of WineD3D's GL round trip in one trip.sh as RPS sets
# the GPU's clock, then two with the clock held at HOLD MHz, with
# sample4904.py beside them and i915's frequency and RPS files read before,
# between and after. The limits go back to what they were on every exit.
# Usage: lin5426.sh DIR HOLD PROTON
set -u
dir=$1
hold=$2
proton=$3
here=$(cd "$(dirname "$0")" && pwd)
stop=$dir/stop-5426
dri=/sys/kernel/debug/dri/0
card=/sys/class/drm/card0
[ -e "$card/gt_min_freq_mhz" ] || card=/sys/class/drm/card1
mkdir -p "$dir" && rm -f "$stop" || exit 1
min0=
max0=
pid=
# shellcheck disable=SC2329 # the exit trap calls it
finish() {
    status=$?
    if [ -n "$max0" ]; then
        if echo "$max0" > "$card/gt_max_freq_mhz" &&
            echo "$min0" > "$card/gt_min_freq_mhz" &&
            [ "$(cat "$card/gt_min_freq_mhz")" = "$min0" ] &&
            [ "$(cat "$card/gt_max_freq_mhz")" = "$max0" ]; then
            echo "lin5426: limits back at $min0 to $max0 MHz"
        else
            echo 'lin5426: could not restore the frequency limits' >&2
            status=1
        fi
    fi
    if [ -n "$pid" ]; then
        touch "$stop" || kill "$pid" 2> /dev/null || :
        sampler=0
        wait "$pid" || sampler=$?
        echo "SAMPLER_STATUS=$sampler"
        [ "$sampler" -eq 0 ] || status=1
    fi
    rm -f "$stop"
    sync
    exit "$status"
}
trap finish EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM
dump() {
    {
        for f in i915_frequency_info i915_rps_boost_info; do
            echo "### $f"
            cat "$dri/$f" 2> /dev/null || echo "lin5426: no $f"
        done
        for f in gt_min_freq_mhz gt_max_freq_mhz gt_cur_freq_mhz gt_act_freq_mhz; do
            echo "$f $(cat "$card/$f" 2> /dev/null)"
        done
    } > "$dir/$1.txt"
}
min=$(cat "$card/gt_min_freq_mhz") && max=$(cat "$card/gt_max_freq_mhz") &&
    [ "$hold" -ge "$min" ] && [ "$hold" -le "$max" ] ||
    { echo 'lin5426: held clock outside the original limits' >&2; exit 2; }
dump before
python3 "$here/sample4904.py" "$dir/sample.txt" "$stop" < /dev/null &
pid=$!
s=0
sh "$here/trip.sh" -u gfx -p "$proton" -n 3 "$dir/free" || s=$?
echo "TRIP_FREE_STATUS=$s"
[ "$s" -eq 0 ] || exit "$s"
dump between
min0=$min
max0=$max
echo "$hold" > "$card/gt_min_freq_mhz" && echo "$hold" > "$card/gt_max_freq_mhz" &&
    [ "$(cat "$card/gt_min_freq_mhz")" = "$hold" ] &&
    [ "$(cat "$card/gt_max_freq_mhz")" = "$hold" ] ||
    { echo "lin5426: the clock not held at $hold MHz" >&2; exit 2; }
echo "lin5426: held at $hold to $hold MHz"
h=0
sh "$here/trip.sh" -u gfx -p "$proton" -n 2 "$dir/held" || h=$?
echo "TRIP_HELD_STATUS=$h"
dump after
exit "$h"
