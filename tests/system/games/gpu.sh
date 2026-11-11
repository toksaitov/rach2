#!/bin/sh
# Run the Wine probes' gpu row on a machine's own GPU, as root, under a
# headless Weston that takes no screen: on Rach2's trial root with the
# root's rach2-game, or on Linux with this directory's copy and a Proton
# that -p names. Usage: gpu.sh [-u USER] [-r RENDERER] [-p PROTON] [-c LINUX]
# OUT. OUT receives the row's results, the machine's identity and the kernel's
# log lines of the run; LINUX is the results.json of the same row under Linux
# on the same machine, against which the sums are judged.
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
made=
home=
trap '[ -z "$made" ] || rm -f /opt/proton
      [ -z "$home" ] || rm -rf "$home"' EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

mkdir -p "$out"
out=$(cd "$out" && pwd)
chown "$user" "$out"
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
} > "$out/identity.txt" 2>&1
home=$out/home
mkdir -p "$home"
chown "$user" "$home"
status=0
su -s /bin/sh "$user" -c "cd '$out' && HOME='$out/home' TMPDIR='$out' GAMES_RENDERER='$renderer' \
    GAMES_COMPARE='$linux' GAMES_LAUNCHER='$launcher' python3 '$here/run.py' gpu '$out/gpu'" \
    || status=$?
# The kernel's log: the GPU's resets and faults, and on Rach2 with
# rach2.trace=enosys each call it lacks, once for each program that asked,
# with the number of lines that named it.
dmesg > "$out/dmesg.txt"
grep -e 'i915: reset' -e 'wedged' -e 'vtd: fault' "$out/dmesg.txt" || true
sed -n 's/.*syscall: \(.*not implemented.*\)/\1/p' "$out/dmesg.txt" | sort | uniq -c
if command -v mixer > /dev/null && [ -e /dev/mixer ]; then
    mixer vol 1
fi
echo "gpu: result: status $status, results in $out/gpu/results.json"
[ "$status" -ne 0 ] || echo 'GPU OK'
exit "$status"
