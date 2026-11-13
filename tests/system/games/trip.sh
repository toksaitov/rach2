#!/bin/sh
# Time WineD3D's GL round trip with the kernel's counters (trip.py), as root
# on the machine itself: on Rach2's trial root with the root's rach2-game, or
# on Linux with this directory's copy and a Proton that -p names, as gpu.sh
# runs the gpu row. Usage: trip.sh [-u USER] [-r RENDERER] [-p PROTON]
# [-n RUNS] [-t THREAD] OUT. OUT receives trip.py's results and the
# machine's identity, and goes to USER; its parent must be root's or the
# operator's, since root makes and gives away OUT by its path. Root writes
# nothing in OUT: USER writes the identity and makes and removes its home
# there. -t has Rach2's bkl/calls count the thread THREAD
# alone, by ID or name, as wined3d_cs, for the runs (debugfs(5)).
set -eu

user='test'
renderer='HD Graphics 4400'
proton=
runs=3
thread=
while getopts u:r:p:n:t: opt; do
    case $opt in
        u) user=$OPTARG ;;
        r) renderer=$OPTARG ;;
        p) proton=$OPTARG ;;
        n) runs=$OPTARG ;;
        t) thread=$OPTARG ;;
        *) echo 'usage: trip.sh [-u USER] [-r RENDERER] [-p PROTON] [-n RUNS] [-t THREAD] OUT' >&2; exit 2 ;;
    esac
done
shift $((OPTIND - 1))
[ $# -eq 1 ] || { echo 'usage: trip.sh [-u USER] [-r RENDERER] [-p PROTON] [-n RUNS] [-t THREAD] OUT' >&2; exit 2; }
[ "$(id -u)" -eq 0 ] || { echo 'trip: run as root' >&2; exit 2; }
here=$(cd "$(dirname "$0")" && pwd)
out=$1
# The link /opt/proton names the Proton, which a relative -p would make
# relative to /opt: make it the caller's working directory's.
case $proton in
    '' | /*) ;;
    *) proton=$(pwd)/$proton ;;
esac
made=
home=
reader=
bkl=
chosen=
# The EXIT trap's cleanup, each step of which runs whether or not the one
# before it failed: a reader already reaped makes kill fail.
# shellcheck disable=SC2329 # the exit trap calls it
cleanup() {
    [ -z "$made" ] || rm -f /opt/proton || true
    [ -z "$home" ] || su -s /bin/sh "$user" -c "rm -rf '$home'" || true
    [ -z "$reader" ] || { kill "$reader" 2> /dev/null || true; wait "$reader" 2> /dev/null || true; }
    [ -z "$bkl" ] || rm -rf "$bkl" || true
    [ -z "$chosen" ] || echo all > "$chosen" || true
}
trap cleanup EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

mkdir -p "$out"
out=$(cd "$out" && pwd -P)
chown -h "$user" "$out"

# trip.py, as USER, takes Rach2's bkl/calls, bkl/profile and bkl/tick at the
# loop's start and end, but debugfs is root's alone (0700) and stays so: a
# reader of root's (trip.py --reader) copies those three files, and nothing
# else, into BKL/snap, which USER may read but not write, for each request
# USER appends to BKL/ask, its one file there. trip.py --prepare makes BKL in
# /tmp, never in TMPDIR or OUT, with every directory root's and writable by
# root alone, so that USER can rename or replace nothing that root writes.
if [ -d /sys/kernel/debug/bkl ]; then
    bkl=$(PYTHONPATH="$here" python3 "$here/trip.py" --prepare "$(id -u "$user")")
    PYTHONPATH="$here" python3 "$here/trip.py" --reader "$bkl" &
    reader=$!
fi
if command -v mixer > /dev/null && [ -e /dev/mixer ]; then
    mixer vol 1
fi
if [ -n "$thread" ]; then
    calls=/sys/kernel/debug/bkl/calls
    [ -w "$calls" ] || { echo "trip: -t: no $calls to write" >&2; exit 2; }
    echo "thread $thread" > "$calls"
    chosen=$calls
fi
launcher=rach2-game
if [ -n "$proton" ]; then
    if [ ! -e /opt/proton ]; then
        ln -s "$proton" /opt/proton
        made=1
    fi
    [ "$(readlink -f /opt/proton)" = "$(readlink -f "$proton")" ] ||
        { echo "trip: /opt/proton is not $proton" >&2; exit 1; }
    launcher="sh $here/rach2-game.sh"
fi
{
    uname -a
    cat /proc/cmdline
    nproc
    readlink -f /opt/proton
    command -v strace perf || true
} 2>&1 | su -s /bin/sh "$user" -c "cat > '$out/identity.txt'"
home=$out/home
su -s /bin/sh "$user" -c "mkdir -p '$home'"
status=0
su -s /bin/sh "$user" -c "cd '$out' && HOME='$out/home' TMPDIR='$out' GAMES_RENDERER='$renderer' \
    GAMES_LAUNCHER='$launcher' TRIP_BKL='$bkl' TRIP_BKL_PID='$reader' PYTHONPATH='$here' \
    python3 '$here/trip.py' '$out/trip' '$runs'" || status=$?
su -s /bin/sh "$user" -c "rm -rf '$home'" || true
home=
if [ -n "$reader" ]; then
    kill "$reader" 2> /dev/null || true
    wait "$reader" 2> /dev/null || true
    reader=
fi
[ -z "$bkl" ] || rm -rf "$bkl"
bkl=
if command -v mixer > /dev/null && [ -e /dev/mixer ]; then
    mixer vol 1
fi
echo "trip: result: status $status, results in $out/trip"
exit "$status"
