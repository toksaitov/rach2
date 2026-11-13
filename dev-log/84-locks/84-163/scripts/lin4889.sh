#!/bin/sh
# 84-163's loops on the Lenovo's Debian, as root. MODE plain runs two plain
# loops, whose thread lines carry schedstat's run and ready-wait times; freq
# one with sample.py beside it; trace one with tracefs recording the request,
# fence, interrupt and scheduler events on the monotonic clock, kept in
# DIR/trace.txt. Usage: lin4889.sh MODE DIR PROTON
set -u
mode=$1
dir=$2
proton=$3
here=$(cd "$(dirname "$0")" && pwd)
stop=$dir/stop-$mode
tr=/sys/kernel/tracing
rm -f "$stop"
pid=
n=1
traced=
trap '[ -z "$traced" ] || { echo 0 > $tr/tracing_on; for e in $events $calls; do echo 0 > $tr/events/$e/enable; done; for e in $calls; do echo 0 > $tr/events/$e/filter; done; }' EXIT
# wined3d_cs's ioctls, whose entry and exit give each SYNCOBJ_WAIT's time.
calls='syscalls/sys_enter_ioctl syscalls/sys_exit_ioctl'
events='i915/i915_request_add dma_fence/dma_fence_signaled irq/irq_handler_entry sched/sched_waking sched/sched_wakeup sched/sched_switch'
case $mode in
    plain) n=2 ;;
    freq)
        python3 "$here/sample.py" "$dir/sample-$mode.txt" "$stop" > /dev/null 2>&1 < /dev/null &
        pid=$!
        ;;
    trace)
        echo 0 > $tr/tracing_on
        echo mono > $tr/trace_clock
        echo 65536 > $tr/buffer_size_kb
        echo 0 > $tr/options/overwrite
        echo > $tr/trace
        traced=1
        for e in $events; do
            echo 1 > $tr/events/$e/enable || exit 1
        done
        for e in $calls; do
            { echo 'comm == "wined3d_cs"' > $tr/events/$e/filter && echo 1 > $tr/events/$e/enable; } ||
                echo "lin4889: no $e"
        done
        echo 1 > $tr/tracing_on
        ;;
    *) echo 'lin4889: MODE is plain, freq or trace' >&2; exit 2 ;;
esac
# The trace stops once the plain loop has ended, before trip.py's strace runs.
rm -f "$dir/status-$mode"
{ s=0; sh "$here/trip.sh" -u gfx -p "$proton" -n "$n" "$dir/linux-$mode" || s=$?; echo $s > "$dir/status-$mode"; } 2>&1 |
    while IFS= read -r line; do
        printf '%s\n' "$line"
        case $line in
            *'trip-gl-0: exit'*) [ -z "$traced" ] || echo 0 > $tr/tracing_on ;;
        esac
    done
s=$(cat "$dir/status-$mode")
echo "TRIP_STATUS=$s"
if [ -n "$traced" ]; then
    echo 0 > $tr/tracing_on
    cat $tr/trace > "$dir/trace.txt"
    grep -H -e overrun -e 'dropped' $tr/per_cpu/cpu*/stats
    echo "TRACE_LINES=$(wc -l < "$dir/trace.txt")"
fi
if [ -n "$pid" ]; then
    touch "$stop"
    w=0
    wait "$pid" || w=$?
    echo "SAMPLER_STATUS=$w samples $(grep -c '^== ' "$dir/sample-$mode.txt")"
    grep '^freq ' "$dir/sample-$mode.txt"
fi
rm -f "$stop"
sync
exit "$s"
