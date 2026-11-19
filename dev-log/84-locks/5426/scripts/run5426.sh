#!/bin/sh
# 5426's loops on the Lenovo's installed release, as root: three loops of
# WineD3D's GL round trip in one trip.sh as RPS sets the GPU's clock, then
# two with the clock held at HOLD MHz by gt_min_freq_mhz and gt_max_freq_mhz,
# with sample4904.py beside them all and i915's frequency, RPS, engine and
# runtime PM files read before, between and after. The limits go back to
# what they were on every exit. ARGS go to trip.sh, as -r llvmpipe in QEMU.
# Under QEMU, with no i915, the second run's clock is not held.
# Usage: run5426.sh DIR HOLD [ARGS]
set -u
dir=$1
hold=$2
shift 2
here=$(cd "$(dirname "$0")" && pwd)
stop=$dir/stop-5426
dri=$(ls -d /sys/kernel/debug/dri/* 2> /dev/null | head -n 1)
card=$(ls -d /sys/class/drm/card*/gt_min_freq_mhz 2> /dev/null | head -n 1)
card=${card%/gt_min_freq_mhz}
power=/sys/bus/pci/devices/0000:00:02.0/power
mkdir -p "$dir"
rm -f "$stop"
min0=
max0=
pid=
# shellcheck disable=SC2329 # the exit trap calls it
finish() {
    if [ -n "$max0" ]; then
        echo "$max0" > "$card/gt_max_freq_mhz"
        echo "$min0" > "$card/gt_min_freq_mhz"
        echo "run5426: limits back at $(cat "$card/gt_min_freq_mhz") to $(cat "$card/gt_max_freq_mhz") MHz"
    fi
    if [ -n "$pid" ]; then
        touch "$stop"
        wait "$pid"
        echo "SAMPLER_STATUS=$? samples $(grep -c '^[0-9]' "$dir/sample.txt") boost $(grep -c '^== ' "$dir/sample.txt")"
    fi
    rm -f "$stop"
    sync
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
dump before
python3 "$here/sample4904.py" "$dir/sample.txt" "$stop" > /dev/null 2>&1 < /dev/null &
pid=$!
echo kernel > /sys/kernel/debug/bkl/profile
s=0
sh "$here/trip.sh" -n 3 -t wined3d_cs "$@" "$dir/free" || s=$?
echo "TRIP_FREE_STATUS=$s"
dump between
if [ -n "$card" ]; then
    min0=$(cat "$card/gt_min_freq_mhz")
    max0=$(cat "$card/gt_max_freq_mhz")
    echo "$hold" > "$card/gt_min_freq_mhz" && echo "$hold" > "$card/gt_max_freq_mhz" ||
        { echo "run5426: the clock not held at $hold MHz"; exit 2; }
    echo "run5426: held at $(cat "$card/gt_min_freq_mhz") to $(cat "$card/gt_max_freq_mhz") MHz, cur $(cat "$card/gt_cur_freq_mhz")"
else
    echo "run5426: no i915 frequency files, so the second run's clock is not held"
fi
h=0
sh "$here/trip.sh" -n 2 -t wined3d_cs "$@" "$dir/held" || h=$?
echo "TRIP_HELD_STATUS=$h"
dump after
grep -h -e '^Work runs' -e '^Parks' -e '^RPNSWREQ' -e '^GT park' -e '^GT idle' -e '^power/' \
    "$dir/before.txt" "$dir/between.txt" "$dir/after.txt"
[ "$s" -eq 0 ] && [ "$h" -eq 0 ]
