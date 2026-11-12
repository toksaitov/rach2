#!/bin/sh
# Time WineD3D's GL round trip with the kernel's counters (trip.py), as root
# on the machine itself: on Rach2's trial root with the root's rach2-game, or
# on Linux with this directory's copy and a Proton that -p names, as gpu.sh
# runs the gpu row. Usage: trip.sh [-u USER] [-r RENDERER] [-p PROTON]
# [-n RUNS] OUT. OUT receives trip.py's results and the machine's identity.
set -eu

user='test'
renderer='HD Graphics 4400'
proton=
runs=3
while getopts u:r:p:n: opt; do
    case $opt in
        u) user=$OPTARG ;;
        r) renderer=$OPTARG ;;
        p) proton=$OPTARG ;;
        n) runs=$OPTARG ;;
        *) echo 'usage: trip.sh [-u USER] [-r RENDERER] [-p PROTON] [-n RUNS] OUT' >&2; exit 2 ;;
    esac
done
shift $((OPTIND - 1))
[ $# -eq 1 ] || { echo 'usage: trip.sh [-u USER] [-r RENDERER] [-p PROTON] [-n RUNS] OUT' >&2; exit 2; }
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
debugmode=
trap '[ -z "$made" ] || rm -f /opt/proton
      [ -z "$home" ] || rm -rf "$home"
      [ -z "$debugmode" ] || debugfs_mode "$debugmode"' EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

mkdir -p "$out"
out=$(cd "$out" && pwd)
chown "$user" "$out"

# Set the mode of debugfs's root, which every mount of it shares, through the
# mode= option of a mount of its own that is unmounted at once, since Rach2's
# debugfs has no chmod.
debugfs_mode() {
    mkdir "$out/debugfs" &&
        mount -t debugfs -o "mode=$1" debugfs "$out/debugfs" &&
        umount "$out/debugfs"
    rmdir "$out/debugfs"
}

# trip.py reads Rach2's bkl/calls as USER for the holds by entry and the takes
# after a switch by the call they slept in, but debugfs's root is root's alone
# (0700) by default: open it for the run and put its mode back on exit.
calls=/sys/kernel/debug/bkl/calls
if [ -e "$calls" ] && ! su -s /bin/sh "$user" -c "test -r $calls"; then
    was=$(stat -c %a /sys/kernel/debug)
    if debugfs_mode 755 && su -s /bin/sh "$user" -c "test -r $calls"; then
        debugmode=$was
    else
        debugfs_mode "$was" || true
        echo "trip: $calls: not readable by $user; holds by entry from /proc/stat alone" >&2
    fi
fi
if command -v mixer > /dev/null && [ -e /dev/mixer ]; then
    mixer vol 1
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
} > "$out/identity.txt" 2>&1
home=$out/home
mkdir -p "$home"
chown "$user" "$home"
status=0
su -s /bin/sh "$user" -c "cd '$out' && HOME='$out/home' TMPDIR='$out' GAMES_RENDERER='$renderer' \
    GAMES_LAUNCHER='$launcher' PYTHONPATH='$here' python3 '$here/trip.py' '$out/trip' '$runs'" \
    || status=$?
rm -rf "$home"
home=
if command -v mixer > /dev/null && [ -e /dev/mixer ]; then
    mixer vol 1
fi
echo "trip: result: status $status, results in $out/trip"
exit "$status"
