#!/bin/sh
# watch.sh: on the trial root, as root, keep what a run would otherwise
# lose: a copy of the kernel's log each time it changes, since the trace of
# missing calls turns its 16 KiB over, and every ten seconds the processor
# and big-lock lines of /proc/stat. It logs each change of the volume with
# its time and does not set it back, since the owner may change it while
# playing; the step itself runs `mixer vol 1` after each boot and each
# start of a program that may set it.
#
# Usage: watch.sh DIR
#
# It runs until killed. DIR gets dmesg.N, stat.txt and vol.txt.
set -eu
dir=$1
mkdir -p "$dir"
n=0
tick=0
last=
: > "$dir/dmesg.last"
while :; do
    dmesg > "$dir/dmesg.new"
    if ! cmp -s "$dir/dmesg.new" "$dir/dmesg.last"; then
        n=$((n + 1))
        cp "$dir/dmesg.new" "$dir/dmesg.$n"
        mv "$dir/dmesg.new" "$dir/dmesg.last"
    fi
    vol=$(mixer vol)
    if [ "$vol" != "$last" ]; then
        echo "$(date +%T) $vol" >> "$dir/vol.txt"
        last=$vol
    fi
    if [ $((tick % 10)) -eq 0 ]; then
        {
            echo "time $(date +%s)"
            grep -E '^cpu |^bkl_wait|^bkl_hold_max_us|^bkl_hold_entry' /proc/stat
        } >> "$dir/stat.txt"
    fi
    tick=$((tick + 1))
    sleep 1
done
