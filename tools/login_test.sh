#!/usr/bin/env sh

#
# login_test.sh: prove the console cannot fall back to an unauthenticated shell.
#
# Boots two images that differ only in the init they declare, neither staging
# /bin/login, and then two boots of the real image: one that stops the login at
# its prompt, which only init can undo, and one that proves the way back in. The
# strict one (init-login) must report that it cannot exec /bin/login, must
# report it more than once, since once is a failure and not a retry, and must
# never print uid=0. The permissive one (init-shell) must print uid=0, and that
# half is what makes the strict half's silence mean something: a strict image
# that failed to boot at all for an unrelated reason would otherwise pass by
# printing nothing. `id` is a standalone-ash applet, so uid=0 at a console
# nobody authenticated at is an unauthenticated root shell and nothing else. A
# third boot proves the way back into a broken image still works: rach2.rescue
# must reach the kernel debug shell.
#
# The ENOENT case is the one that matters and the one an earlier draft never
# reached, having tested a chmod 0000 /bin/login instead, which yields EACCES.
# ENOENT is exactly the shape of an image built without the program, which is
# the image that must refuse hardest.
#
# Usage: login_test.sh <kernel> <strict.krffs> <shell.krffs> <os-real.krffs>
#            <user> <password>
#

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

# Type `id` at whatever the console offers. A shell answers it; a console that
# never came up does not, which is the whole distinction being drawn.
boot() {
    printf '\nid\n' | qemu-system-i386 -cpu max -display none -kernel "$KERNEL" \
        -initrd "$2" -append "$3" -serial stdio >"$1" 2>&1 &
    QPID=$!
}

# Wait until the serial log $1 contains the token $2, then return 0; give up
# after $3 seconds and return 1. Gating on a token rather than on a fixed sleep
# keeps a loaded host from killing QEMU before the console has had its say.
wait_serial() {
    i=0
    while [ "$i" -lt "$3" ]; do
        if grep -q "$2" "$1" 2>/dev/null; then
            return 0
        fi
        sleep 1
        i=$((i + 1))
    done
    return 1
}

stop() {
    qpid="$QPID"
    QPID=""
    if [ -n "$qpid" ]; then
        kill "$qpid" 2>/dev/null || true
        wait "$qpid" 2>/dev/null || true
    fi
}

# The machine goes down every way out of this script and not only the ways it
# writes for itself: a cancelled run that had only removed the logs would leave
# QEMU alive with nothing named to say so.
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
# the half that is supposed to succeed rather than as a false pass of the other.
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
# dead init rather than one that keeps the console closed.
boot "$STRICT.log" "$STRICT" ""
if ! wait_serial "$STRICT.log" "init: cannot exec /bin/login" 60; then
    stop
    echo "login-test: FAIL: the strict init did not report the missing program" >&2
    exit 1
fi
i=0
while [ "$i" -lt 60 ]; do
    said=$(grep -c "init: cannot exec /bin/login" "$STRICT.log" 2>/dev/null || echo 0)
    if [ "$said" -ge 2 ]; then
        break
    fi
    sleep 1
    i=$((i + 1))
done
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

# A console session stopped by an uncatchable SIGSTOP, which only init can undo:
# there is no shell above it to type `fg`, so an init that waited without
# WUNTRACED would go on waiting and the console would be gone until the next
# reboot. The signal is sent by the session itself, so what is proved is the
# recovery and not the delivery: `kill -STOP $$` cannot be caught, ignored or
# handled, and the token after it can only be printed by a session init brought
# back. The token is written split, because the console echoes what is typed:
# a command containing the whole token would put it in the log before it ran,
# and the wait would match the echo rather than the output.
# The feed is spaced, and that is not politeness: `login` flushes pending input
# when it puts the line back into canonical mode, so a script written all at
# once is eaten by the very repair that makes the console usable, and the test
# would pass having run nothing.
( sleep 12; printf '%s\n' "$USER_NAME"; sleep 2; printf '%s\n' "$PASSWORD"; sleep 3;
  printf 'kill -STOP $$; echo CONTINUED""-OK\n'; sleep 25 ) \
    | qemu-system-i386 -cpu max -display none \
    -kernel "$KERNEL" -initrd "$REAL" -serial stdio >"$REAL.stop.log" 2>&1 &
QPID=$!
if ! wait_serial "$REAL.stop.log" "CONTINUED-OK" 120; then
    stop
    echo "login-test: FAIL: a stopped console session never came back; init must" >&2
    echo "login-test: continue one, since nothing above it can" >&2
    exit 1
fi
stop
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
