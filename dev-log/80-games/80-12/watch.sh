#!/bin/sh
# watch.sh: on the trial root, as root, keep what a run would otherwise
# lose: a copy of the kernel's log each time it changes, since the trace of
# missing calls turns its 16 KiB over, and every ten seconds the processor
# and big-lock lines of /proc/stat.
#
# Usage: watch.sh DIR
#
# It runs until killed. DIR gets dmesg.N and stat.txt.
set -eu
dir=$1
mkdir -p "$dir"
n=0
tick=0
: > "$dir/dmesg.last"
while :; do
    dmesg > "$dir/dmesg.new"
    if ! cmp -s "$dir/dmesg.new" "$dir/dmesg.last"; then
        n=$((n + 1))
        cp "$dir/dmesg.new" "$dir/dmesg.$n"
        mv "$dir/dmesg.new" "$dir/dmesg.last"
    fi
    if [ $((tick % 5)) -eq 0 ]; then
        {
            echo "time $(date +%s)"
            grep -E '^cpu |^bkl_wait|^bkl_hold_max_us|^bkl_hold_entry' /proc/stat
        } >> "$dir/stat.txt"
    fi
    tick=$((tick + 1))
    sleep 2
done
