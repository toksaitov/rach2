#!/bin/sh
# 84-176's loops on Rach2's trial root, as root: i915's frequency, RPS and
# engine files before, three loops of WineD3D's GL round trip in one
# trip.sh with sample4904.py beside them, and the three files after. ARGS go
# to trip.sh, as -r llvmpipe in QEMU. Usage: run4904.sh DIR [ARGS]
set -u
dir=$1
shift
here=$(cd "$(dirname "$0")" && pwd)
stop=$dir/stop-4904
dri=$(ls -d /sys/kernel/debug/dri/* 2> /dev/null | head -n 1)
rm -f "$stop"
dump() {
    for f in i915_frequency_info i915_rps_boost_info i915_engine_info; do
        echo "### $f"
        cat "$dri/$f" 2> /dev/null || echo "run4904: no $f"
    done > "$dir/$1.txt"
}
dump before
python3 "$here/sample4904.py" "$dir/sample.txt" "$stop" > /dev/null 2>&1 < /dev/null &
pid=$!
s=0
sh "$here/trip.sh" -n 3 -t wined3d_cs "$@" "$dir/out" || s=$?
echo "TRIP_STATUS=$s"
touch "$stop"
w=0
wait "$pid" || w=$?
echo "SAMPLER_STATUS=$w samples $(grep -c '^[0-9]' "$dir/sample.txt") boost $(grep -c '^== ' "$dir/sample.txt")"
dump after
grep -h -e '^Work runs' -e '^Parks' -e '^RPNSWREQ' -e '^GT park' -e '^GT idle' "$dir/before.txt" "$dir/after.txt"
rm -f "$stop"
sync
exit "$s"
