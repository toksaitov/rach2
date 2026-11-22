#!/bin/sh
# crysis.sh: Crysis's acceptance on the Lenovo as the native cases games/crysis*
# of gpu.cases run it, through crysis.py (tests/system/games), on a private
# copy of the game that 80-6 installed on the data partition.
#
#   crysis.sh flyby ROUTE
#   crysis.sh load SAVE
#   crysis.sh shm
#   crysis.sh session ran|none
#
# Run as root by etude-guest in the case's workspace, which holds crysis.py,
# wm.py and the empty directory `private` of the private fixture, in a mount
# namespace of the case's own, on the GNU root whose desktop supervisor runs.
# The checks:
#
#   flyby  Linux's island flyby through ROUTE, umu or direct, its first two
#          loops, each judged by crysis.py against 24 frames a second beside
#          Linux's
#   load   SAVE, a save that crysis.py's `save` recorded in an earlier boot
#          before the root was updated, such as default_accept_s2.CRYSISJMSF
#          of 80-9, loaded through a copy of the game's own prefix with umu
#          and through a new prefix of the case's own, directly: the load
#          after a restart and an update, through the existing prefix and a
#          new one
#   shm    the umu flyby on the bed's shared-memory card. The launcher
#          supplies a private mount namespace and an already mounted /data.
#   session
#          no check: crysis-shm.sh's end, which starts the session again on
#          the files put back when it ran before the launcher, and otherwise
#          ends it, and waits as the case's own end does
#
# The data partition, /dev/disk0p6, and all it holds are the owner's; the
# case never writes it. It mounts the partition read-only on /data/owner
# within a /data of its own, the directory `private` of the workspace bound
# over /data in the namespace, and builds /data/crysis there for the game's
# own paths, which crysis.sh of the root fixes under /data/crysis:
#
#   game     a tree of the case's directories whose files are symbolic links
#            to the owner's, read-only, which the game reads through; a file
#            the game or crysis.py writes, such as Game.log, system.cfg or
#            the benchmark's log, replaces its link with a file of the case
#   compat, saves, steam, cache, accept.json
#            copies owned by the desktop account, which runs the game: its
#            prefix, its profile and saves, Proton's Steam client directory,
#            Mesa's shader cache and the saves' record
#   logs     a new directory for Proton's logs
#
# The case fails before the game starts if a link in the tree resolves
# outside the private /data and Proton's read-only /opt/proton, Wine's drive
# links aside, since the game would write through it.
#
# The load's new prefix is /data/native/crysis-compat, in the same /data. On
# every exit the script ends the game's mounts, puts back the session's
# weston.ini and rach2-session, which crysis.py's `prepare` writes, and leaves
# the session as it found it: started again by SIGHUP, with the files put back,
# when it ran, and otherwise ended by SIGUSR2, and waits for the new Weston or
# for none. `private`, all the case makes on the root besides `out`, is the
# fixture's: etude-guest removes it once every process of the case has ended,
# after a pass, a failure or a kill that skips this trap alike.
#
# Prints crysis.py's lines and `crysis-case: result: pass` or `fail: why`, and
# exits 0 on a pass.
set -eu

here=$(pwd)
data=/dev/disk0p6
private=$here/private
owner=/data/owner
base=/data/crysis
fresh=/data/native/crysis-compat
ini=/etc/xdg/weston/weston.ini
session=/usr/libexec/rach2-session
pidfile=/run/rach2-desktop.pid
uid=1001
bound=0 mounted=0 kept=0 failed='' found=0
source_bound=0

say() { echo "crysis-case: $*"; }

# mounted_at DIR MODE: whether /proc/mounts has a mount on DIR whose options
# begin with MODE, rw or ro, and, for rw, whose type is ext2, as crysis.sh
# of the root requires of /data.
mounted_at()
{
    awk -v dir="$1" -v mode="$2" '$2 == dir && $4 ~ "^" mode "(,|$)" &&
        (mode == "ro" || $3 == "ext2") { ok = 1 } END { exit !ok }' /proc/mounts
}

# session_restore: have the supervisor start the session again, on the files
# put back, when it ran, and otherwise end it, and wait, 600 tries of 0.1 s
# at most, for a Weston other than the one that ran, or for none, so that
# the next case does not draw in a session about to end. The end is sent
# even with no Weston, since a start the supervisor has taken may not have
# reached Weston yet; it is done once neither Weston nor the session's
# script has run for 15 tries, longer than the supervisor's turn of a second
# in which it may still begin a start.
session_restore()
{
    old=$(pgrep -x weston | tr '\n' ' ')
    [ -f "$pidfile" ] || return 1
    if [ "$found" = 1 ]; then
        kill -HUP "$(cat "$pidfile")" 2> /dev/null || return 1
    else
        kill -USR2 "$(cat "$pidfile")" 2> /dev/null || return 1
    fi
    tries=0 clear=0
    while :; do
        now=$(pgrep -x weston | tr '\n' ' ')
        if [ "$found" = 1 ]; then
            [ -z "$now" ] || [ "$now" = "$old" ] || [ ! -S "/run/user/$uid/wayland-0" ] || return 0
        elif [ -z "$now" ] && ! pgrep -f "$session" > /dev/null; then
            clear=$((clear + 1))
            [ "$clear" -lt 15 ] || return 0
        else
            clear=0
        fi
        tries=$((tries + 1))
        [ "$tries" -lt 600 ] || return 1
        sleep 0.1
    done
}

# shellcheck disable=SC2329 # the exit trap runs it
cleanup()
{
    status=$?
    trap - EXIT
    if [ "$kept" = 1 ]; then
        cp -p "$here/weston.ini" "$ini" && cp -p "$here/rach2-session" "$session" ||
            failed="$failed session-files"
        session_restore || say "session: not left as it was found" >&2
    fi
    # A process of the game's that has not yet ended may hold either mount;
    # the namespace's mounts go with the case's last process.
    [ "$mounted" = 0 ] || umount "$owner" 2> /dev/null || umount -l "$owner" ||
        failed="$failed umount-owner"
    [ "$bound" = 0 ] || umount /data 2> /dev/null || umount -l /data || failed="$failed umount"
    [ "$source_bound" = 0 ] || umount "$private/owner" 2> /dev/null ||
        umount -l "$private/owner" || failed="$failed umount-source"
    if [ -n "$failed" ]; then
        say "result: fail:$failed"
        exit 1
    fi
    if [ "$status" != 0 ]; then
        say "result: fail: status $status"
        exit "$status"
    fi
    say "result: pass"
}
# `crysis.sh session ran|none`, for crysis-shm.sh: leave the session
# running on the files put back, or ended, and wait for it.
if [ "${1:-}:$#" = session:2 ]; then
    case $2 in
        ran) found=1 ;;
        none) found=0 ;;
        *) echo "usage: crysis.sh session ran|none" >&2; exit 2 ;;
    esac
    session_restore || { say "session: not left as it was found" >&2; exit 1; }
    exit 0
fi

trap cleanup EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

fail()
{
    failed="$failed $*"
    exit 1
}

case ${1:-}:$# in
    flyby:2 | load:2 | shm:1) ;;
    *)
        echo "usage: crysis.sh flyby umu|direct | crysis.sh load SAVE | crysis.sh shm |" \
            "crysis.sh session ran|none" >&2
        exit 2
        ;;
esac
[ -f crysis.py ] && [ -f wm.py ] || fail "no crysis.py and wm.py in the workspace"
command -v crysis.sh > /dev/null || fail "no crysis.sh on this root"
[ -d /data ] && [ -b "$data" ] || fail "no /data or no $data"
[ -d "$private" ] && [ -z "$(ls -A "$private")" ] ||
    fail "no empty $private; the case runs with the private fixture"

if [ "$1" = shm ]; then
    current_mountns=$(readlink /proc/self/ns/mnt) || fail "cannot read the mount namespace"
    [ -n "${CRYSIS_PARENT_MNTNS:-}" ] &&
        [ "$current_mountns" != "$CRYSIS_PARENT_MNTNS" ] ||
        fail "no private mount namespace"
    awk -v dev="$data" '$1 == dev && $2 == "/data" && $3 == "ext2" && $4 ~ /(^|,)rw(,|$)/ {
        ok = 1
    } END { exit !ok }' /proc/mounts || fail "$data is not mounted on /data"
    [ "$(stat -c %d "$private")" != "$(stat -c %d /data)" ] ||
        fail "the workspace is on the owner's partition"
else
    awk '$2 == "/data" { found = 1 } END { exit found }' /proc/mounts ||
        fail "/data is mounted already"
    ! pgrep -x weston > /dev/null || found=1
fi

mkdir "$private/owner"
if [ "$1" = shm ]; then
    # The mounted device already has a claim. Make a read-only view of it.
    mount --bind /data "$private/owner" || fail "$data would not bind"
    source_bound=1
    mount -o remount,bind,ro "$private/owner" || fail "$data bind would not turn read-only"
    mount --rbind "$private" /data || fail "the workspace would not bind on /data"
    mounted=1
else
    mount --bind "$private" /data || fail "the workspace would not bind on /data"
fi
bound=1
[ "$private" -ef /data ] || fail "/data does not name the workspace's bind"
# The workspace is root's and closed; the desktop account, which runs the
# game, must pass through /data to reach /data/crysis.
chmod 0755 /data || fail "/data would not open to the desktop account"
mounted_at /data rw || fail "the workspace is not a writable ext2 file system"
if [ "$1" != shm ]; then
    mount -t ext2 -o ro "$data" "$owner" || fail "$data would not mount read-only on $owner"
    mounted=1
fi
mounted_at "$owner" ro || fail "$owner is not read-only"
[ -f "$owner/crysis/game/Bin64/Crysis.exe" ] || fail "no game in $owner/crysis/game"
say "$data read-only on $owner"

# The desktop account reads the game through the links.
closed=$( {
    find "$owner" "$owner/crysis" -maxdepth 0 ! -perm -0001
    find "$owner/crysis/game" \( -type f ! -perm -0004 \) -o \( -type d ! -perm -0005 \)
} | head -n 3)
[ -z "$closed" ] || fail "the desktop account cannot read $(echo "$closed" | tr '\n' ' ')"

copies=''
for name in compat saves steam cache accept.json; do
    [ ! -e "$owner/crysis/$name" ] || copies="$copies $owner/crysis/$name"
done
# The copies, a new prefix as large as the game's own for `load`, and a GiB
# for the logs, the game's files and the links.
# shellcheck disable=SC2086 # paths without spaces
need=$( { [ -z "$copies" ] || du -sk $copies; } | awk '{ n += $1 } END { print n + 0 }')
[ "$1" != load ] || [ ! -d "$owner/crysis/compat" ] ||
    need=$((need + $(du -sk "$owner/crysis/compat" | cut -f 1)))
need=$((need + 1048576))
free=$(df -Pk "$private" | awk 'NR == 2 { print $4 }')
[ "$free" -ge "$need" ] || fail "the copies need $need KiB, the workspace has $free"

mkdir "$base" "$base/logs"
cp -rs "$owner/crysis/game" "$base/game" || fail "game-links"
# shellcheck disable=SC2086 # paths without spaces
[ -z "$copies" ] || cp -a $copies "$base/" || fail copies
# Every link in the private tree must resolve inside the private /data,
# whose only other mount is the owner's read-only one, or in Proton's
# read-only /opt/proton: through a link to anywhere else, such as a saves
# directory linked to the owner's home, the game or crysis.py would write
# the owner's files. Wine's drive links, such as z: -> /, name the running
# system and stay as they are.
escapes=$(find "$base" -type l ! -path '*/dosdevices/[a-z]:' ! -path '*/dosdevices/[a-z]::' \
    -exec realpath -m -- {} + | awk '!/^\/data(\/|$)/ && !/^\/opt\/proton(\/|$)/' | head -n 3)
[ -z "$escapes" ] ||
    fail "links in the private game resolve outside it, to $(echo "$escapes" | tr '\n' ' ')"
chown -R -h "$uid:$uid" "$base"
say "private game: $(find "$base/game" -type l | wc -l) links, copies of$(
    echo "$copies" | sed "s|$owner/crysis/||g"), $need KiB reserved"

# The umu container can write to the private /data. The test's output
# directory on the host root is outside the container.
CRYSIS_HUD_PARENT=$base/logs/hud
export CRYSIS_HUD_PARENT

if [ "$1" = shm ]; then
    # Let the VM runner own the handoff and its signal traps. The mounts
    # go away when the last process of this namespace ends.
    exec sh ./bed-shm.sh -d "$owner/native/bed/vm.disk" -w -l crysis \
        -g 0000:00:02.0 -m 8192 -o "$here/out"
fi

cp -p "$ini" weston.ini
cp -p "$session" rach2-session
kept=1

python3 ./crysis.py prepare "$here/out" || fail prepare
case $1 in
    flyby)
        python3 ./crysis.py flyby "$2" "flyby-$2" 2 || fail "flyby-$2"
        ;;
    load)
        python3 ./crysis.py load load-update "$2" umu || fail load-update
        mkdir -p "${fresh%/*}"
        install -d -o "$uid" -g "$uid" -m 0755 "$fresh"
        python3 ./crysis.py load load-new "$2" direct "$fresh" || fail load-new
        ;;
esac
