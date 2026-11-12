#!/bin/sh
# crysis.sh: Crysis's acceptance on the Lenovo as the native cases games/crysis*
# of gpu.cases run it, through crysis.py (tests/system/games), on a private
# copy of the game that 80-6 installed on the data partition.
#
#   crysis.sh flyby ROUTE
#   crysis.sh load SAVE
#
# Run as root by etude-guest in the case's workspace, which holds crysis.py,
# wm.py and the empty directory `private` of the private fixture, in a mount
# namespace of the case's own, on the GNU root whose desktop session runs.
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
# The load's new prefix is /data/native/crysis-compat, in the same /data. On
# every exit the script ends the game's mounts, puts back the session's
# weston.ini and rach2-session, which crysis.py's `prepare` writes, and
# starts the session again. `private`, all the case makes on the root
# besides `out`, is the fixture's: etude-guest removes it once every process
# of the case has ended, after a pass, a failure or a kill that skips this
# trap alike.
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
bound=0 mounted=0 kept=0 failed=''

say() { echo "crysis-case: $*"; }

# mounted_at DIR MODE: whether /proc/mounts has a mount on DIR whose options
# begin with MODE, rw or ro, and, for rw, whose type is ext2, as crysis.sh
# of the root requires of /data.
mounted_at()
{
    awk -v dir="$1" -v mode="$2" '$2 == dir && $4 ~ "^" mode "(,|$)" &&
        (mode == "ro" || $3 == "ext2") { ok = 1 } END { exit !ok }' /proc/mounts
}

# shellcheck disable=SC2329 # the exit trap runs it
cleanup()
{
    status=$?
    trap - EXIT
    if [ "$kept" = 1 ]; then
        cp -p "$here/weston.ini" "$ini" && cp -p "$here/rach2-session" "$session" ||
            failed="$failed session-files"
        [ ! -f "$pidfile" ] || kill -HUP "$(cat "$pidfile")" 2> /dev/null || :
    fi
    # A process of the game's that has not yet ended may hold either mount;
    # the namespace's mounts go with the case's last process.
    [ "$mounted" = 0 ] || umount "$owner" 2> /dev/null || umount -l "$owner" ||
        failed="$failed umount-owner"
    [ "$bound" = 0 ] || umount /data 2> /dev/null || umount -l /data || failed="$failed umount"
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
trap cleanup EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

fail()
{
    failed="$failed $*"
    exit 1
}

[ $# -eq 2 ] || {
    echo "usage: crysis.sh flyby umu|direct | crysis.sh load SAVE" >&2
    exit 2
}
case $1 in flyby | load) ;; *) fail "no check $1" ;; esac
[ -f crysis.py ] && [ -f wm.py ] || fail "no crysis.py and wm.py in the workspace"
command -v crysis.sh > /dev/null || fail "no crysis.sh on this root"
[ -d /data ] && [ -b "$data" ] || fail "no /data or no $data"
awk '$2 == "/data" { found = 1 } END { exit found }' /proc/mounts || fail "/data is mounted already"
[ -d "$private" ] && [ -z "$(ls -A "$private")" ] ||
    fail "no empty $private; the case runs with the private fixture"

mkdir "$private/owner"
mount --bind "$private" /data || fail "the workspace would not bind on /data"
bound=1
mounted_at /data rw || fail "the workspace is not a writable ext2 file system"
mount -t ext2 -o ro "$data" "$owner" || fail "$data would not mount read-only on $owner"
mounted=1
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
chown -R -h "$uid:$uid" "$base"
say "private game: $(find "$base/game" -type l | wc -l) links, copies of$(
    echo "$copies" | sed "s|$owner/crysis/||g"), $need KiB reserved"

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
