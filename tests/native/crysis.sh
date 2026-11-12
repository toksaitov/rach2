#!/bin/sh
# crysis.sh: Crysis's acceptance on the Lenovo as the native cases games/crysis*
# of gpu.cases run it, through crysis.py (tests/system/games), on the game
# that 80-6 installed on the data partition.
#
#   crysis.sh flyby ROUTE
#   crysis.sh load SAVE
#
# Run as root by etude-guest in the case's workspace, which holds crysis.py
# and wm.py, in a mount namespace of the case's own, on the GNU root whose
# desktop session runs. The data partition, /dev/disk0p6, is mounted on /data
# in that namespace. The checks:
#
#   flyby  Linux's island flyby through ROUTE, umu or direct, four loops, each
#          judged by crysis.py against 24 frames a second beside Linux's
#   load   SAVE, a save that crysis.py's `save` recorded in an earlier boot
#          before the root was updated, such as default_accept_s2.CRYSISJMSF
#          of 80-9, loaded through the game's own prefix with umu and through
#          a new prefix of the case's own, directly: the load after a restart
#          and an update, through the existing prefix and a new one
#
# The game, its prefix and the player's saves are the owner's. Before the
# game starts the script keeps a copy of what crysis.py's `prepare` and the
# game write over: /data/crysis/saves, the profile, settings and saves, the
# game's system.cfg, and the session's weston.ini and rach2-session. On every
# exit it puts each file back as it was, gives the game back to its owner
# with `crysis.py restore`, starts the session again, removes the new prefix,
# and then requires every kept file, /data/kept among them, to read as it did
# before. A save the game or crysis.py adds under a name of its own, such as
# default_accept_*, stays. The game's own start checkpoint, which every start
# of the first level writes, is put back too.
#
# Prints crysis.py's lines and `crysis-case: result: pass` or `fail: why`, and
# exits 0 on a pass.
set -eu

here=$(pwd)
data=/dev/disk0p6
base=/data/crysis
fresh=/data/native/crysis-compat
ini=/etc/xdg/weston/weston.ini
session=/usr/libexec/rach2-session
pidfile=/run/rach2-desktop.pid
mounted=0 kept=0 failed=''

say() { echo "crysis-case: $*"; }

# digests: the SHA-256 of every file of the kept set, a line a file, which
# sha256sum -c reads again at the end; a file added meanwhile is not read.
digests()
{
    { find "$base/saves" -type f; echo "$base/game/system.cfg"; echo /data/kept; } |
        while read -r f; do
            [ -f "$f" ] && sha256sum "$f"
        done | sort -k 2
}

# shellcheck disable=SC2329 # the exit trap runs it
cleanup()
{
    status=$?
    trap - EXIT
    if [ "$kept" = 1 ]; then
        python3 "$here/crysis.py" restore || failed="$failed restore"
        tar -xpf "$here/kept.tar" -C / || failed="$failed put-back"
        cp -p "$here/weston.ini" "$ini" && cp -p "$here/rach2-session" "$session" ||
            failed="$failed session-files"
        [ ! -f "$pidfile" ] || kill -HUP "$(cat "$pidfile")" 2> /dev/null || :
        rm -rf "$fresh"
        rmdir "${fresh%/*}" 2> /dev/null || :
        if ! sha256sum -c "$here/before.sha256" > "$here/after.check" 2>&1; then
            grep -v ': OK$' "$here/after.check" || :
            failed="$failed originals-changed"
        fi
    fi
    # A process of the game's that has not yet ended may hold the partition;
    # the namespace's mount goes with the case's last process.
    [ "$mounted" = 0 ] || umount /data 2> /dev/null || umount -l /data || failed="$failed umount"
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
[ -f crysis.py ] && [ -f wm.py ] || fail "no crysis.py and wm.py in the workspace"
command -v crysis.sh > /dev/null || fail "no crysis.sh on this root"
[ -d /data ] && [ -b "$data" ] || fail "no /data or no $data"
mount -t ext2 "$data" /data || fail "$data would not mount on /data"
mounted=1
[ -f "$base/game/Bin64/Crysis.exe" ] || fail "no game in $base/game"

digests > before.sha256
[ -f "$base/game/system.cfg" ] && cfg=${base#/}/game/system.cfg || cfg=''
# shellcheck disable=SC2086 # empty or one path
tar -cpf kept.tar -C / "${base#/}/saves" $cfg
cp -p "$ini" weston.ini
cp -p "$session" rach2-session
kept=1
say "kept $(wc -l < before.sha256) files' digests"

python3 ./crysis.py prepare "$here/out" || fail prepare
case $1 in
    flyby)
        python3 ./crysis.py flyby "$2" "flyby-$2" 4 || fail "flyby-$2"
        ;;
    load)
        python3 ./crysis.py load load-update "$2" umu || fail load-update
        rm -rf "$fresh"
        mkdir -p "${fresh%/*}"
        install -d -o 1001 -g 1001 -m 0755 "$fresh"
        python3 ./crysis.py load load-new "$2" direct "$fresh" || fail load-new
        ;;
    *)
        fail "no check $1"
        ;;
esac
