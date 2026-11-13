#!/bin/sh
# 84-163's loops on Rach2's trial root, as root. MODE cs runs two plain loops
# counting wined3d_cs; freq one such loop with sample.py beside it; pin one
# with sample.py binding tripprobe's threads to processors 1 to 3. ARGS go to
# trip.sh, as -r llvmpipe in QEMU. Usage: run4889.sh MODE DIR [ARGS]
set -u
mode=$1
dir=$2
shift 2
here=$(cd "$(dirname "$0")" && pwd)
stop=$dir/stop-$mode
rm -f "$stop"
pid=
case $mode in
    cs) n=2 ;;
    freq)
        n=1
        python3 "$here/sample.py" "$dir/sample-$mode.txt" "$stop" > /dev/null 2>&1 < /dev/null &
        pid=$!
        ;;
    pin)
        n=1
        python3 "$here/sample.py" "$dir/sample-$mode.txt" "$stop" --pin 0xe > /dev/null 2>&1 < /dev/null &
        pid=$!
        ;;
    *) echo 'run4889: MODE is cs, freq or pin' >&2; exit 2 ;;
esac
echo kernel > /sys/kernel/debug/bkl/profile
s=0
sh "$here/trip.sh" -n "$n" -t wined3d_cs "$@" "$dir/out-$mode" || s=$?
echo "TRIP_STATUS=$s"
if [ -n "$pid" ]; then
    touch "$stop"
    w=0
    wait "$pid" || w=$?
    echo "SAMPLER_STATUS=$w samples $(grep -c '^== ' "$dir/sample-$mode.txt")"
    grep -e '^pin ' -e '^freq ' "$dir/sample-$mode.txt"
fi
rm -f "$stop"
sync
exit "$s"
