#!/bin/sh
# sample.sh: on the trial root, as root, read what a frame stall would show,
# four times a second, so that a stall in the frame times can be set beside
# the big lock, the sound and the shader cache at the same moment: the
# processors' times, the big lock's waits, longest wait and longest hold,
# its sixteen longest-holding entries, HDA's queue line from /proc/audio,
# the frames Gallium's HUD has dumped so far, the bytes of Mesa's shader
# cache and the sectors read from the data partition, where the game is.
#
# Usage: sample.sh DIR
#
# It runs until killed and appends to DIR/sample.txt one block a reading,
# each opening with `t` and the time in milliseconds.
set -eu
dir=$1
hud=/tmp/80-12/hud/frametime_\(ms\)
cache=/home/test/.cache/mesa_shader_cache_db
mkdir -p "$dir"
while :; do
    {
        echo "t $(date +%s%3N)"
        grep -E '^cpu |^bkl_wait_us|^bkl_wait_max_us|^bkl_hold_max_us|^bkl_hold_max_entry|^bkl_hold_entry' /proc/stat
        grep -E '^queue' /proc/audio
        echo "frames $(wc -l < "$hud" 2> /dev/null || echo 0)"
        echo "cache $(du -sk "$cache" 2> /dev/null | cut -f1)"
        awk '$3 == "disk0p6" { print "read", $6 }' /proc/diskstats
    } >> "$dir/sample.txt"
    sleep 0.25
done
