#!/bin/sh
# bkl.sh OUT GAME...: each GAME once through lenovo.sh, with the GPU's clock
# pinned at 1000 MHz and debugfs's bkl/calls, bkl/profile and, on a kernel
# that has it, bkl/tick zeroed before the run and read after it, with
# /proc/stat around it, for 80-41's split of the kernel's time into work per
# call and waits on the big lock and 80-75's of the tick's time into its
# parts (dev-log/80-games/80-41.md, 80-75.md). Run as root on the trial
# root, beside lenovo.sh; OUT/GAME gets lenovo.sh's output and calls,
# profile, tick, stat-before and stat-after. lenovo.sh is on task/2664
# (80-7b) until that task merges: copy it beside this script with
# `git archive origin/task/2664 tests/system/games/lenovo.sh`. The clock's
# minimum goes back from an exit trap, an interrupted run's too.
set -u

[ $# -ge 2 ] || { echo "usage: bkl.sh OUT GAME..." >&2; exit 2; }
out=$1
shift
d=/sys/kernel/debug/bkl
gpu=/sys/class/drm/card0
here=$(dirname "$0")
[ -w "$d/calls" ] || { echo "bkl: no $d/calls" >&2; exit 1; }
[ -f "$here/lenovo.sh" ] || { echo "bkl: no $here/lenovo.sh; it is on task/2664" >&2; exit 1; }
min=$(cat "$gpu/gt_min_freq_mhz")
# shellcheck disable=SC2329 # the exit trap calls it
restore() {
    echo "$min" > "$gpu/gt_min_freq_mhz"
    echo "bkl: clock minimum back at $(cat "$gpu/gt_min_freq_mhz") MHz"
}
trap restore EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
echo 1000 > "$gpu/gt_min_freq_mhz"
status=0
for g; do
    o=$out/$g
    mkdir -p "$o"
    cat /proc/stat > "$o/stat-before"
    echo 0 > "$d/calls"
    echo 0 > "$d/profile"
    [ -w "$d/tick" ] && echo 0 > "$d/tick"
    sh "$here/lenovo.sh" -n 1 "$o" "$g" || status=1
    cat "$d/calls" > "$o/calls"
    cat "$d/profile" > "$o/profile"
    [ -r "$d/tick" ] && cat "$d/tick" > "$o/tick"
    cat /proc/stat > "$o/stat-after"
    echo "bkl: $g: $(grep -c . "$o/calls") lines of calls, $(head -n 1 "$o/profile")"
done
exit $status
