#!/bin/sh
# crysis-bkl.sh CRYSIS OUT [SECONDS]: 84-7's reading of Crysis in play. Start
# the 32-bit flyby once through crysis.py (CRYSIS, task/2665's harness, after
# its `prepare`), and once its level has loaded zero debugfs's bkl/calls and
# bkl/profile, the profile in its whole-kernel mode, read /proc/stat, wait
# SECONDS (150) of play, read them again and end the game through the
# harness's own cleanup. OUT gets calls, profile, stat-before, stat-after,
# flyby.out and the times. Run as root on the trial root, in the boot
# session that crysis.py's `prepare` set up. An exit trap ends the game, an
# interrupted run's too.
set -u

[ $# -ge 2 ] && [ $# -le 3 ] || { echo "usage: crysis-bkl.sh CRYSIS OUT [SECONDS]" >&2; exit 2; }
crysis=$1
out=$2
secs=${3:-150}
d=/sys/kernel/debug/bkl
[ -w "$d/calls" ] || { echo "crysis-bkl: no $d/calls" >&2; exit 1; }
[ -r "$crysis" ] || { echo "crysis-bkl: no $crysis" >&2; exit 1; }
mkdir -p "$out" || exit 1
pid=''
# shellcheck disable=SC2329 # the exit trap calls it
finish() {
    if [ -n "$pid" ] && kill -0 "$pid" 2> /dev/null; then
        # crysis.py's flyby ends the game, its Wine server and its logs in a
        # finally block, which an interrupt reaches.
        kill -INT "$pid" 2> /dev/null
        wait "$pid"
    fi
    echo "crysis-bkl: harness ended at $(cut -d' ' -f1 /proc/uptime) s"
}
trap finish EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
# A shell without job control starts a background command with SIGINT
# ignored, and Python then installs no handler of its own; put it back.
python3 -c '
import os, runpy, signal, sys
signal.signal(signal.SIGINT, signal.default_int_handler)
sys.argv = sys.argv[1:]
sys.path.insert(0, os.path.dirname(os.path.abspath(sys.argv[0])))
runpy.run_path(sys.argv[0], run_name="__main__")
' "$crysis" flyby 32 crysis-bkl 1 > "$out/flyby.out" 2>&1 &
pid=$!
echo "crysis-bkl: started at $(cut -d' ' -f1 /proc/uptime) s"
# The harness prints `crysis: loaded in S s` once Game.log has the level's
# load time; it gives up by itself when no log grows for its silence.
until grep -q '^crysis: loaded in' "$out/flyby.out"; do
    kill -0 "$pid" 2> /dev/null || { echo "crysis-bkl: harness ended before the load" >&2; exit 1; }
    sleep 1
done
echo kernel > "$d/profile"
echo 0 > "$d/calls"
cat /proc/stat > "$out/stat-before"
echo "crysis-bkl: loaded at $(cut -d' ' -f1 /proc/uptime) s: $(grep '^crysis: loaded in' "$out/flyby.out")"
sleep "$secs"
kill -0 "$pid" 2> /dev/null || echo "crysis-bkl: the game ended within $secs s" >&2
cat "$d/calls" > "$out/calls"
cat "$d/profile" > "$out/profile"
cat /proc/stat > "$out/stat-after"
echo "crysis-bkl: read at $(cut -d' ' -f1 /proc/uptime) s: $(head -n 1 "$out/profile")"
