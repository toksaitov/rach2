#!/bin/sh
# Measure the Crysis flyby on the Lenovo. With native, use i915 instead.
# Run as root from a new workspace on the installed system's ext2 root,
# with crysis.sh (the native case), crysis.py, wm.py, bed-shm.sh, bedkeep.c
# and gpu.sh.
# The workspace keeps out/ and the manifests data.before and data.after.
set -eu

child='' before=0 weston=0
games='Crysis.exe|wineserver|pressure-vessel|pv-adverb|srt-bwrap'

# manifest FILE: record the real Crysis tree's names, metadata and links
# without reading file contents or changing access times of regular files.
manifest()
{
    find /data/crysis -printf '%P\t%y\t%i\t%m\t%U\t%G\t%s\t%T@\t%C@\t%l\0' > "$1.unsorted" &&
        LC_ALL=C sort -z "$1.unsorted" > "$1" &&
        rm "$1.unsorted"
}

# cleanup: finish the child and the game, leave the session as it was
# found, compare the real data and clear the game fixture. Leave only the
# run's evidence in the workspace.
# shellcheck disable=SC2329 # the exit trap runs it
cleanup()
{
    status=$?
    trap - EXIT
    trap '' HUP INT TERM
    if [ -n "$child" ]; then
        kill -TERM "$child" 2>/dev/null || :
        wait "$child" 2>/dev/null || :
    fi
    if [ "$before" = 1 ]; then
        # No game ran when the launcher began, so any left is the run's,
        # and may hold the private tree that the end removes.
        tries=0
        while pkill -9 -u desktop -f "$games"; do
            tries=$((tries + 1))
            if [ "$tries" = 100 ]; then
                echo 'crysis-shm: the game would not end' >&2
                status=1
                break
            fi
            sleep 0.1
        done
        # bed-shm.sh put weston.ini back, but a session may have started
        # on the changed file, as when QEMU ended early and the GPU came
        # back before that; start it again, or end it if none ran before.
        if [ "$weston" = 1 ]; then
            sh ./crysis.sh session ran || status=1
        else
            sh ./crysis.sh session none || status=1
        fi
        if manifest data.after && cmp -s data.before data.after; then
            echo "crysis-shm: data: unchanged: $(tr -cd '\000' < data.after | wc -c) entries"
            sha256sum data.before data.after
        else
            echo 'crysis-shm: data: failed: manifests differ or the final read failed' >&2
            status=1
        fi
        echo 1 > /sys/class/backlight/intel_backlight/brightness || status=1
        rm -rf private || status=1
    fi
    exit "$status"
}
trap cleanup EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

case ${1:-shm}:$# in
    shm:0) route=shm ;;
    native:1) route=native ;;
    *) echo 'usage: crysis-shm.sh [native]' >&2; exit 2 ;;
esac
[ "$(id -u)" = 0 ] && [ -d /data/crysis ] &&
    [ "$(stat -c %d .)" != "$(stat -c %d /data)" ] ||
    { echo 'crysis-shm: use a workspace on the root as root' >&2; exit 2; }
[ ! -e data.before ] && [ ! -e private ] ||
    { echo 'crysis-shm: use a new workspace for each run' >&2; exit 2; }
if pgrep -u desktop -f "$games" >/dev/null; then
    echo 'crysis-shm: the desktop account is already running a game or container' >&2
    exit 1
fi
! pgrep -x weston > /dev/null || weston=1
manifest data.before
before=1
mkdir -m 0755 private
mixer vol 1
CRYSIS_PARENT_MNTNS=$(readlink /proc/self/ns/mnt)
export CRYSIS_PARENT_MNTNS
unshare --mount --propagation private sh ./crysis.sh "$route" &
child=$!
status=0
wait "$child" || status=$?
child=''
exit "$status"
