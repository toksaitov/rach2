#!/bin/sh
# 5426's loops on the Lenovo's installed release, as root: three loops of
# WineD3D's GL round trip in one trip.sh as RPS sets the GPU's clock, then
# two with the clock held at HOLD MHz by gt_min_freq_mhz and gt_max_freq_mhz,
# with sample4904.py beside them all and i915's frequency, RPS, engine and
# runtime PM files read before, between and after. The limits go back to
# what they were on exit. Usage: run5426.sh DIR HOLD.
set -u
dir=$1
hold=$2
here=$(cd "$(dirname "$0")" && pwd)
stop=$dir/stop-5426
dri=$(ls -d /sys/kernel/debug/dri/* 2> /dev/null | head -n 1)
card=$(ls -d /sys/class/drm/card*/gt_min_freq_mhz 2> /dev/null | head -n 1)
card=${card%/gt_min_freq_mhz}
power=/sys/bus/pci/devices/0000:00:02.0/power
mkdir -p "$dir" && rm -f "$stop" || exit 1
min0=
max0=
pid=
profile=/sys/kernel/debug/bkl/profile
profile_set=
# shellcheck disable=SC2329 # the exit trap calls it
finish() {
    status=$?
    if [ -n "$max0" ]; then
        if echo "$max0" > "$card/gt_max_freq_mhz" &&
            echo "$min0" > "$card/gt_min_freq_mhz" &&
            [ "$(cat "$card/gt_min_freq_mhz")" = "$min0" ] &&
            [ "$(cat "$card/gt_max_freq_mhz")" = "$max0" ]; then
            echo "run5426: limits back at $min0 to $max0 MHz"
        else
            echo 'run5426: could not restore the frequency limits' >&2
            status=1
        fi
    fi
    [ -z "$profile_set" ] || echo bkl > "$profile" || status=1
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
        for f in i915_frequency_info i915_rps_boost_info i915_engine_info; do
            echo "### $f"
            cat "$dri/$f" 2> /dev/null || echo "run5426: no $f"
        done
        for f in gt_min_freq_mhz gt_max_freq_mhz gt_cur_freq_mhz gt_act_freq_mhz; do
            echo "$f $(cat "$card/$f" 2> /dev/null)"
        done
        for f in control runtime_status runtime_active_time runtime_suspended_time; do
            echo "power/$f $(cat "$power/$f" 2> /dev/null)"
        done
    } > "$dir/$1.txt"
}
[ -n "$card" ] || { echo 'run5426: no i915 frequency files' >&2; exit 2; }
min=$(cat "$card/gt_min_freq_mhz") && max=$(cat "$card/gt_max_freq_mhz") &&
    [ "$hold" -ge "$min" ] && [ "$hold" -le "$max" ] ||
    { echo 'run5426: held clock outside the original limits' >&2; exit 2; }
dump before
python3 "$here/sample4904.py" "$dir/sample.txt" "$stop" < /dev/null &
pid=$!
echo kernel > "$profile" || exit 1
profile_set=yes
s=0
sh "$here/trip.sh" -n 3 -t wined3d_cs "$dir/free" || s=$?
echo "TRIP_FREE_STATUS=$s"
[ "$s" -eq 0 ] || exit "$s"
dump between
min0=$min
max0=$max
echo "$hold" > "$card/gt_min_freq_mhz" && echo "$hold" > "$card/gt_max_freq_mhz" &&
    [ "$(cat "$card/gt_min_freq_mhz")" = "$hold" ] &&
    [ "$(cat "$card/gt_max_freq_mhz")" = "$hold" ] ||
    { echo "run5426: the clock not held at $hold MHz" >&2; exit 2; }
echo "run5426: held at $hold to $hold MHz"
h=0
sh "$here/trip.sh" -n 2 -t wined3d_cs "$dir/held" || h=$?
echo "TRIP_HELD_STATUS=$h"
dump after
grep -h -e '^Work runs' -e '^Parks' -e '^RPNSWREQ' -e '^GT park' -e '^GT idle' -e '^power/' \
    "$dir/before.txt" "$dir/between.txt" "$dir/after.txt"
exit "$h"
