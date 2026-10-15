#!/usr/bin/env sh

#
# login.sh: the console cannot fall back to an unauthenticated shell.
#
# Two images that differ only in the init they declare boot first, neither
# staging /bin/login. The strict one (init-login) must report that it cannot
# exec /bin/login, must report it more than once, since once is a failure and
# not a retry, and must never print uid=0. The permissive one (init-shell)
# must print uid=0, which gives the strict half's silence its meaning: a
# strict image that failed to boot for an unrelated reason would otherwise
# pass by printing nothing. `id` is a standalone-ash applet, so uid=0 at a
# console nobody authenticated at is an unauthenticated root shell. The
# missing program gives ENOENT, the error of an image built without it,
# which is the image that must refuse; a /bin/login without permission would
# give EACCES instead.
#
# Two boots of the real image follow: one stops the console session, which
# only init can continue, and one requires rach2.rescue to reach the kernel
# debug shell, the way back into a broken image.
#
# Usage: login.sh <kernel> <strict.krffs> <shell.krffs> <os-real.krffs>
#            <user> <password>
#

# The emulator of the architecture tests/system/suites.mk names.
QEMU=${RACH2_QEMU:-qemu-system-i386}

# shellcheck source=tests/system/deadline.sh
# The runner helper, whose `until` and `gone` are the suite's waits.
RUNUTIL="$(cd "$(dirname "$0")/../../.." && pwd)/build/tests/system/runutil"
. "$(cd "$(dirname "$0")/../../.." && pwd)/tests/system/deadline.sh"
set -e

KERNEL="$1"
STRICT="$2"
SHELL_IMAGE="$3"
REAL="$4"
USER_NAME="$5"
PASSWORD="$6"

if [ -z "$KERNEL" ] || [ -z "$STRICT" ] || [ -z "$SHELL_IMAGE" ] || [ -z "$REAL" ] ||
        [ -z "$USER_NAME" ] || [ -z "$PASSWORD" ]; then
    echo "usage: login_test.sh <kernel> <strict.krffs> <shell.krffs> <os-real.krffs>" >&2
    echo "                     <user> <password>" >&2
    exit 2
fi

LOGS="$STRICT.log $SHELL_IMAGE.log $REAL.stop.log $REAL.rescue.log"
# Removed here and not only in the trap. An uncatchable kill leaves a previous
# run's logs, and each has the token its phase waits for; the redirect that
# would truncate one is performed by the backgrounded child, which the parent's
# grep is not ordered after, so a wait could end before its boot began.
# shellcheck disable=SC2086
rm -f $LOGS

QPID=""

# boot LOG IMAGE LINE: boot IMAGE with the boot line LINE in the background,
# its console in LOG, and type `id` at whatever the console offers: a shell
# answers it, and a console that never came up does not.
boot() {
    printf '\nid\n' | "$QEMU" -cpu max -display none -kernel "$KERNEL" \
        -initrd "$2" -append "$3" -serial stdio >"$1" 2>&1 &
    QPID=$!
}

# Wait until the serial log $1 contains the token $2, then return 0; give up
# after $3 seconds and return 1. A wait for a token, and not for a fixed
# time, keeps a loaded host from stopping QEMU before the console printed it.
wait_serial() {
    "$RUNUTIL" until "$1" "$2" "$3"
}

# Stop the machine, if it runs.
stop() {
    qpid="$QPID"
    QPID=""
    if [ -n "$qpid" ]; then
        kill "$qpid" 2>/dev/null || true
        wait "$qpid" 2>/dev/null || true
    fi
}

# Stop the machine and remove the logs, on every way out of the script: a
# cancelled run that only removed the logs would leave QEMU running.
# shellcheck disable=SC2086
cleanup() {
    stop
    rm -f $LOGS
}

trap 'cleanup' EXIT
trap 'cleanup; exit 130' INT
trap 'cleanup; exit 143' TERM
trap 'cleanup; exit 129' HUP

# The permissive image first, so a failure to boot at all is reported against
# the half that is supposed to succeed, not as a false pass of the other.
boot "$SHELL_IMAGE.log" "$SHELL_IMAGE" ""
if ! wait_serial "$SHELL_IMAGE.log" "uid=0" 60; then
    stop
    echo "login-test: FAIL: the permissive init never reached a root shell, so" >&2
    echo "login-test: the strict init's refusal would prove nothing" >&2
    exit 1
fi
stop
echo "login-test: the permissive init reaches a root shell: OK"

# And the strict one, which must refuse, and must go on refusing: the message
# has to appear at least twice, since a single failure followed by silence is a
# dead init, not one that keeps the console closed.
boot "$STRICT.log" "$STRICT" ""
if ! wait_serial "$STRICT.log" "init: cannot exec /bin/login" 60; then
    stop
    echo "login-test: FAIL: the strict init did not report the missing program" >&2
    exit 1
fi
"$RUNUTIL" until "$STRICT.log" "init: cannot exec /bin/login" 60 2 || true
said=$(grep -c "init: cannot exec /bin/login" "$STRICT.log" 2>/dev/null || echo 0)
stop
if [ "$said" -lt 2 ]; then
    echo "login-test: FAIL: the strict init said it once and stopped; init must" >&2
    echo "login-test: keep respawning, since an init that exits is a panic" >&2
    exit 1
fi
if grep -q "uid=0" "$STRICT.log"; then
    echo "login-test: FAIL: the strict init reached a root shell at the console" >&2
    exit 1
fi
if ! grep -q "init: boot rach2.rescue" "$STRICT.log"; then
    echo "login-test: FAIL: the strict init did not say how to get back in" >&2
    exit 1
fi
echo "login-test: the strict init refuses and keeps refusing: OK"

# A console session stopped by SIGSTOP, which only init can undo: there is no
# shell above it to type `fg`, so an init that waited without WUNTRACED would
# go on waiting and the console would be gone until the next reboot. The
# session sends the signal to itself, so what is checked is the recovery and
# not the delivery: `kill -STOP $$` cannot be caught, ignored or handled, and
# only a session that init continued can print the token after it. The token
# is typed split, since the console echoes what is typed and the wait would
# otherwise match the echo.
#
# Each line is typed after the prompt it answers: `login` flushes pending
# input when it puts the line back into canonical mode, so a script typed all
# at once would be lost to that flush.
FEED="$REAL.stop.feed"
rm -f "$FEED"
mkfifo "$FEED"
"$QEMU" -cpu max -display none \
    -kernel "$KERNEL" -initrd "$REAL" -serial stdio < "$FEED" >"$REAL.stop.log" 2>&1 &
QPID=$!
# tests/system/drive.sh types the lines; its feeder holds the fifo open while
# the machine runs.
REPO=$(cd "$(dirname "$0")/../../.." && pwd)
# shellcheck source=tests/system/drive.sh
. "$REPO/tests/system/drive.sh"
DRIVE_LOG="$REAL.stop.log"
DRIVE_FIFO=$FEED
drive_feed "$QPID"
if ! drive_login "$USER_NAME" "$PASSWORD" "$(scaled 90)"; then
    drive_stop
    stop
    echo "login-test: FAIL: os-real did not log its user in" >&2
    exit 1
fi
drive_type 'kill -STOP $$; echo CONTINUED""-OK'
if ! wait_serial "$REAL.stop.log" "CONTINUED-OK" 120; then
    drive_stop
    stop
    echo "login-test: FAIL: a stopped console session never came back; init must" >&2
    echo "login-test: continue one, since nothing above it can" >&2
    exit 1
fi
drive_stop
stop
rm -f "$FEED"
echo "login-test: a stopped console session is continued by init: OK"

# The way back in, against the real image: the kernel never consults the image's
# init in rescue mode, so a broken os-real is repairable in place.
boot "$REAL.rescue.log" "$REAL" "rach2.rescue"
if ! wait_serial "$REAL.rescue.log" "rescue mode" 60; then
    stop
    echo "login-test: FAIL: rach2.rescue did not reach the kernel debug shell" >&2
    exit 1
fi
stop
echo "login-test: rach2.rescue reaches the debug shell: OK"
echo "login-test: PASS"
