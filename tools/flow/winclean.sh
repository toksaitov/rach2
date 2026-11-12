#!/bin/sh
# shellcheck disable=SC2086 # the paths of dirs are a word each
# winclean.sh: on the testing host's Windows, stop what is left of a Windows
# release run and remove the scratch it made outside its own files.
#
# Usage: winclean.sh TREE OUT ID...
#
# release.sh and stop.sh pipe it to the host's MSYS2 bash with `bash -l -s
# TREE OUT ID...`, TREE being Windows' copy and OUT its output directory, as
# the machines file names them. For each run ID it stops each process of the
# run's etude, OUT/etude-ID.exe, with its tree, which a stopped ssh leaves
# running, and removes the run's TMPDIR, OUT/ID.tmp, and the collection
# workspaces under TREE/build/tests and the tooling job directories under
# TREE/build/tooling that are newer than the run's start marker, OUT/ID.start.
# etude removes those itself when it ends or a signal stops it, but for
# evidence its output names; a process stopped by force, as taskkill /F and
# Stop-Process -Force stop one, leaves them. Only one run uses Windows' copy at
# a time, so what is newer than the marker is the run's. The run's own files,
# OUT/ID and OUT/ID.*, stay for the caller to keep or remove.

[ "$#" -ge 3 ] || { echo 'usage: winclean.sh TREE OUT ID...' >&2; exit 2; }
tree=$1 out=$2
shift 2
cd "$out" || exit 0
for id; do
    MSYS_NO_PATHCONV=1 taskkill /F /T /IM "etude-$id.exe" > /dev/null 2>&1
    dirs=
    [ ! -e "$id.start" ] ||
        dirs=$(find "$tree/build/tests" "$tree/build/tooling" -mindepth 1 -maxdepth 1 -type d \
            \( -name 'rach2-collection.*' -o -name 'job-*' \) -newer "$id.start" 2> /dev/null)
    # A QEMU that taskkill ended may hold its files for a few seconds more.
    i=0
    until rm -rf "$id.tmp" $dirs 2> /dev/null || [ "$i" -ge 6 ]; do
        i=$((i + 1))
        sleep 5
    done
    [ "$i" -lt 6 ] || echo "winclean: cannot remove:" "$id.tmp" $dirs >&2
done
