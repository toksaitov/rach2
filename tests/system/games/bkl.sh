#!/bin/sh
# bkl.sh [-k] OUT GAME...: each GAME once through lenovo.sh, with the GPU's
# clock pinned at 1000 MHz and debugfs's bkl/calls, bkl/profile, bkl/long and,
# on a kernel that has it, bkl/tick zeroed before the run and read after it,
# with /proc/stat around it, for 80-41's split of the kernel's time into work
# per call and waits on the big lock and 80-75's of the tick's time into its
# parts (dev-log/80-games/80-41.md, 80-75.md). Run as root on the trial
# root, beside lenovo.sh; OUT/GAME gets lenovo.sh's output and calls,
# profile, tick, stat-before and stat-after. audio-* snapshots separate the
# game from the report reads when a refill comes late; long-after-game and
# long-after-profile keep bkl/long's holds read after the game and after
# the profile's read. -k takes the profile in its whole-kernel mode, every
# tick in kernel mode with or without the lock, in place of the lock's holds
# (debugfs(5)). lenovo.sh is on task/2664
# (80-7b) until that task merges: copy it beside this script by running,
# in this script's directory:
#   git show origin/task/2664:tests/system/games/lenovo.sh > lenovo.sh
# The clock's minimum goes back from an exit trap, an
# interrupted run's too.
set -u

mode=0
if [ "${1-}" = -k ]; then
    mode=kernel
    shift
fi
[ $# -ge 2 ] || { echo "usage: bkl.sh [-k] OUT GAME..." >&2; exit 2; }
out=$1
shift
d=/sys/kernel/debug/bkl
gpu=/sys/class/drm/card0
here=$(dirname "$0")
[ -w "$d/calls" ] || { echo "bkl: no $d/calls" >&2; exit 1; }
[ -f "$here/lenovo.sh" ] || { echo "bkl: no $here/lenovo.sh; it is on task/2664" >&2; exit 1; }
min=$(cat "$gpu/gt_min_freq_mhz")
khda=''
for p in /proc/[0-9]*/comm; do
    if [ "$(cat "$p" 2>/dev/null)" = khda ]; then
        khda=${p%/comm}
        break
    fi
done
# Keep the audio counters on either side of each operation under study.
audio_snapshot() {
    [ -r /proc/audio ] || return 0
    {
        cat /proc/uptime /proc/audio
        if [ -n "$khda" ]; then cat "$khda/schedstat"; fi
        grep hda /proc/interrupts
    } > "$o/audio-$1"
}
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
    audio_snapshot before
    cat /proc/stat > "$o/stat-before"
    echo 0 > "$d/calls"
    echo "$mode" > "$d/profile"
    if [ -w "$d/long" ]; then echo 0 > "$d/long"; fi
    if [ -w "$d/tick" ]; then echo 0 > "$d/tick"; fi
    sh "$here/lenovo.sh" -n 1 "$o" "$g" || status=1
    audio_snapshot after-game
    if [ -r "$d/long" ]; then cat "$d/long" > "$o/long-after-game"; fi
    cat "$d/calls" > "$o/calls"
    audio_snapshot after-calls
    cat "$d/profile" > "$o/profile"
    if [ -r "$d/tick" ]; then cat "$d/tick" > "$o/tick"; fi
    audio_snapshot after-profile
    if [ -r "$d/long" ]; then cat "$d/long" > "$o/long-after-profile"; fi
    cat /proc/stat > "$o/stat-after"
    echo "bkl: $g: $(grep -c . "$o/calls") lines of calls, $(head -n 1 "$o/profile")"
done
exit $status
