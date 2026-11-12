#!/bin/sh
# display.sh: the Lenovo's display, touchpad and kept file, as the native
# cases display/* of gpu.cases check them on the release's trial boot: the
# rows of 76's checkpoint slice that need the machine.
#
#   display.sh drm
#   display.sh weston
#   display.sh touchpad
#   display.sh kept
#
# Run as root by etude-guest in the case's workspace, which holds the
# programs the case stages:
#
#   drm       drmprobe on the panel's card with the desktop's Weston ended
#             first, so that the probe takes the card's master: every mode
#             of the panel set, 1366x768 among them, a flip and the CRC rows,
#             `DRMPROBE OK`; the session is started again afterwards
#   weston    SIGKILL to the desktop's Weston: five seconds later no Weston
#             runs, the session's supervisor runs and has started none, and
#             the kernel's log has the console's return, `console: cardN:
#             taken back from a master`; then SIGHUP starts the session again
#             and its socket comes back
#   touchpad  evprobe touchpad: the Synaptics pad's event device and each
#             axis as Debian's kernel gives them, `EVPROBE OK`
#   kept      the data partition, /dev/disk0p6, mounted read-only in the
#             case's own mount namespace, and its `kept` file, written by
#             76-15c before an update of the root, read with its SHA-256
#
# The session is the desktop's (vendor/gnu/desktop): user `desktop`, socket
# /run/user/1001/wayland-0, its supervisor's pid in /run/rach2-desktop.pid.
# Prints `display: result: CHECK: pass` or `fail: why`, and exits 0 on a pass.
set -eu

socket=/run/user/1001/wayland-0
pidfile=/run/rach2-desktop.pid
data=/dev/disk0p6
kept_sha256=7bf019a047eae8e02b2cb3a2c6837433c8dc1ac9500b500c10e5c81061f92460
check=${1:-}
stopped=0 mounted=''

fail()
{
    echo "display: result: $check: fail: $*"
    exit 1
}

# shellcheck disable=SC2329 # the exit trap runs it
cleanup()
{
    [ -z "$mounted" ] || umount "$mounted" 2> /dev/null || :
    [ "$stopped" = 0 ] || session_start || :
}
trap cleanup EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

# wait_for TEST...: run TEST every 0.1 s until it succeeds, 600 tries at
# most; the status is TEST's last.
wait_for()
{
    tries=0
    until "$@"; do
        tries=$((tries + 1))
        [ "$tries" -lt 600 ] || return 1
        sleep 0.1
    done
}

no_weston() { ! pgrep -x weston > /dev/null; }
session_up() { [ -S "$socket" ] && pgrep -x weston > /dev/null; }

# session_stop: end the desktop's Weston so that a program may take the
# card's master, and wait until it has gone.
session_stop()
{
    pgrep -x weston > /dev/null || return 0
    [ -f "$pidfile" ] || fail "a Weston not the desktop's holds the display"
    stopped=1
    pkill -TERM -x weston || :
    wait_for no_weston || fail "weston did not end in 600 tries"
}

# session_start: have the supervisor start the session again and wait for
# its socket.
session_start()
{
    stopped=0
    session_up && return 0
    [ -f "$pidfile" ] || return 1
    kill -HUP "$(cat "$pidfile")"
    wait_for session_up
}

# new_log LAST: the kernel's lines after the line LAST, which the log, a
# ring, may already have dropped; then every line it has.
new_log()
{
    dmesg | awk -v last="$1" '$0 == last { n = 0; next } { line[++n] = $0 }
        END { for (i = 1; i <= n; i++) print line[i] }'
}

case $check in
    drm)
        [ -x ./drmprobe ] || fail "no drmprobe in the workspace"
        session_stop
        status=0
        ./drmprobe > drmprobe.out 2>&1 || status=$?
        cat drmprobe.out
        session_start || fail "the session did not start again"
        [ "$status" = 0 ] || fail "drmprobe exited $status: $(tail -n 1 drmprobe.out)"
        grep -qx 'DRMPROBE MODE 1366x768' drmprobe.out || fail "no mode 1366x768 was set"
        grep -q '^DRMPROBE FLIP ' drmprobe.out || fail "no flip"
        grep -qx 'DRMPROBE OK' drmprobe.out || fail "no DRMPROBE OK"
        ;;
    weston)
        [ -f "$pidfile" ] || fail "no desktop supervisor"
        sup=$(cat "$pidfile")
        westons=$(pgrep -x weston | tr '\n' ' ')
        [ -n "$westons" ] || fail "no weston runs"
        last=$(dmesg | tail -n 1)
        stopped=1
        # shellcheck disable=SC2086 # process ids
        kill -KILL $westons
        sleep 5
        new_log "$last" > kill.log
        cat kill.log
        left=$(pgrep -x weston | tr '\n' ' ')
        [ -z "$left" ] || fail "weston runs 5 s after the kill: $left"
        kill -0 "$sup" 2> /dev/null || fail "the supervisor $sup has gone"
        grep -q 'console: card[0-9]*: taken back from a master' kill.log ||
            fail "the kernel's log has no console return"
        session_start || fail "SIGHUP did not start the session again"
        ;;
    touchpad)
        [ -x ./evprobe ] || fail "no evprobe in the workspace"
        status=0
        ./evprobe touchpad > evprobe.out 2>&1 || status=$?
        cat evprobe.out
        [ "$status" = 0 ] || fail "evprobe exited $status"
        grep -qx 'EVPROBE OK' evprobe.out || fail "no EVPROBE OK"
        ;;
    kept)
        [ -b "$data" ] || fail "no data partition $data"
        mkdir -p data
        mount -t ext2 -o ro "$data" data || fail "$data would not mount read-only"
        mounted=$PWD/data
        [ -f data/kept ] || fail "no kept file on $data"
        got=$(sha256sum data/kept | cut -d ' ' -f 1)
        echo "display: kept: $(cat data/kept), sha256 $got"
        [ "$got" = "$kept_sha256" ] || fail "kept's SHA-256 is $got, not $kept_sha256"
        ;;
    *)
        echo "usage: display.sh drm|weston|touchpad|kept" >&2
        exit 2
        ;;
esac
echo "display: result: $check: pass"
