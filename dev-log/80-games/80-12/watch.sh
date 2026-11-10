#!/bin/sh
# watch.sh: on the trial root, as root, keep what a run would otherwise
# lose: a copy of the kernel's log each time it changes, since the trace of
# missing calls turns its 16 KiB over, and every ten seconds the processor
# and big-lock lines of /proc/stat. It also holds the volume at 1: the
# session's PulseAudio drives the kernel's mixer through its OSS sink, so a
# volume set on that sink reaches the speakers, and each time the volume
# reads otherwise it is logged and set back.
#
# Usage: watch.sh DIR
#
# It runs until killed. DIR gets dmesg.N, stat.txt and vol.txt.
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
    vol=$(mixer vol)
    if [ "$vol" != 'vol 1:1' ]; then
        echo "$(date +%T) $vol" >> "$dir/vol.txt"
        mixer vol 1 > /dev/null
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
