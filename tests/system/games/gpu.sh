#!/bin/sh
# Run the Wine probes' gpu row on a machine's own GPU, as root, under a
# headless Weston that takes no screen: on Rach2's trial root with the
# root's rach2-game, or on Linux with this directory's copy and a Proton
# that -p names. Usage: gpu.sh [-u USER] [-r RENDERER] [-p PROTON] [-c LINUX]
# OUT. OUT receives the row's results, the machine's identity and the kernel's
# log lines of the run, and goes to USER; LINUX is the results.json of the
# same row under Linux on the same machine, against which the sums are
# judged. trip.py --out makes OUT by descriptor and refuses it unless every
# directory above it is root's and writable by root alone; root writes
# nothing in OUT: USER writes the identity and the log and makes and removes
# its home there.
set -eu

user='test'
renderer='HD Graphics 4400'
proton=
linux=
while getopts u:r:p:c: opt; do
    case $opt in
        u) user=$OPTARG ;;
        r) renderer=$OPTARG ;;
        p) proton=$OPTARG ;;
        c) linux=$OPTARG ;;
        *) echo 'usage: gpu.sh [-u USER] [-r RENDERER] [-p PROTON] [-c LINUX] OUT' >&2; exit 2 ;;
    esac
done
shift $((OPTIND - 1))
[ $# -eq 1 ] || { echo 'usage: gpu.sh [-u USER] [-r RENDERER] [-p PROTON] [-c LINUX] OUT' >&2; exit 2; }
[ "$(id -u)" -eq 0 ] || { echo 'gpu: run as root' >&2; exit 2; }
here=$(cd "$(dirname "$0")" && pwd)
out=$1
# The row runs in OUT, so LINUX and PROTON are made absolute from the
# caller's working directory.
case $linux in
    '' | /*) ;;
    *) linux=$(pwd)/$linux ;;
esac
case $proton in
    '' | /*) ;;
    *) proton=$(pwd)/$proton ;;
esac
made=
home=
# The EXIT trap's cleanup, each step of which runs whether or not the one
# before it failed.
# shellcheck disable=SC2329 # the exit trap calls it
cleanup() {
    [ -z "$made" ] || rm -f /opt/proton || true
    [ -z "$home" ] || su -s /bin/sh "$user" -c "rm -rf '$home'" || true
}
trap cleanup EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

out=$(PYTHONPATH="$here" python3 "$here/trip.py" --out "$user" "$out")
# HDA starts at 100 on Rach2; nothing here plays, but the level is kept low.
# Linux has no mixer(1), and a machine without a sound card no /dev/mixer.
if command -v mixer > /dev/null && [ -e /dev/mixer ]; then
    mixer vol 1
fi
launcher=rach2-game
if [ -n "$proton" ]; then
    # Linux: the launcher of this directory, with /opt/proton, where it
    # looks, naming the Proton given.
    if [ ! -e /opt/proton ]; then
        ln -s "$proton" /opt/proton
        made=1
    fi
    [ "$(readlink -f /opt/proton)" = "$(readlink -f "$proton")" ] ||
        { echo "gpu: /opt/proton is not $proton" >&2; exit 1; }
    launcher="sh $here/rach2-game.sh"
fi
{
    uname -a
    cat /proc/cmdline
    ls -l /dev/dri
    ls /usr/share/vulkan/icd.d
    readlink -f /opt/proton
    cat /opt/proton/version
} 2>&1 | su -s /bin/sh "$user" -c "cat > '$out/identity.txt'"
home=$out/home
su -s /bin/sh "$user" -c "mkdir -p '$home'"
status=0
su -s /bin/sh "$user" -c "cd '$out' && HOME='$out/home' TMPDIR='$out' GAMES_RENDERER='$renderer' \
    GAMES_COMPARE='$linux' GAMES_LAUNCHER='$launcher' python3 '$here/run.py' gpu '$out/gpu'" \
    || status=$?
su -s /bin/sh "$user" -c "rm -rf '$home'" || true
home=
# The kernel's log: the GPU's resets and faults, and on Rach2 with
# rach2.trace=enosys each call it lacks, once for each program that asked,
# with the number of lines that named it. Root reads it from dmesg, never
# from USER's copy.
log=$(dmesg)
printf '%s\n' "$log" | su -s /bin/sh "$user" -c "cat > '$out/dmesg.txt'"
printf '%s\n' "$log" | grep -e 'i915: reset' -e 'wedged' -e 'vtd: fault' || true
printf '%s\n' "$log" | sed -n 's/.*syscall: \(.*not implemented.*\)/\1/p' | sort | uniq -c
if command -v mixer > /dev/null && [ -e /dev/mixer ]; then
    mixer vol 1
fi
echo "gpu: result: status $status, results in $out/gpu/results.json"
[ "$status" -ne 0 ] || echo 'GPU OK'
exit "$status"
