#!/usr/bin/env sh

#
# exitline_test.sh: the plain process-exit line is printed under the debug
# shell and nowhere else.
#
# `sys_exit` used to print "Process exited with status N" on every process exit.
# On an image running /sbin/init and a login shell that is one line of noise per
# command, a forked child's included, and ash already answers the question
# through `$?`. It is now gated on `debug_shell_is_home`, which is exactly where
# something still reads it.
#
# Four boots, because the two ways to get this wrong are opposite. Silencing the
# line everywhere breaks the harness, which decodes a probe's status from
# it in its no-token mode; silencing nothing leaves the noise. And a probe that
# stopped being SCORED would look like a passing run, so the two dedicated
# markers `tests/system/runner/runtests.c` reads are asserted here too.
#
#   1. os-real, logged in, running a program that forks, execs and exits: the
#      program's own output must appear and the plain line must not. Asserting
#      the output first is what makes the silence mean something, since a boot
#      that never reached a shell would otherwise pass by printing nothing.
#   2. The test image, whose debug shell IS home, running a probe that forks:
#      the plain line must appear, and more than once, since the child and the
#      probe both exit.
#   3. The same image running a probe that faults on purpose: the dedicated
#      signal marker must appear, which is what scores a killed probe.
#   4. `the harness` with no token, which has nothing to read but the plain
#      line: it must still decode a status and exit 0.
#
# Usage: exitline_test.sh <kernel> <test.krffs> <os-real.krffs> <user>
#            <password> <harness-kernel>
#

set -e

KERNEL="$1"
TEST_IMAGE="$2"
REAL="$3"
USER_NAME="$4"
PASSWORD="$5"
# The harness build reports each exit status to QEMU's isa-debug-exit device, so
# `the harness` can stop the machine at all; the plain serial line is what it
# then reads the untruncated status from. Both halves have to hold, which is why
# the fourth boot runs against this kernel and not the ordinary one.
HARNESS_KERNEL="$6"

if [ -z "$KERNEL" ] || [ -z "$TEST_IMAGE" ] || [ -z "$REAL" ] ||
        [ -z "$USER_NAME" ] || [ -z "$PASSWORD" ] || [ -z "$HARNESS_KERNEL" ]; then
    echo "usage: exitline_test.sh <kernel> <test.krffs> <os-real.krffs>" >&2
    echo "                        <user> <password> <harness-kernel>" >&2
    exit 2
fi

PLAIN="process: exited with status"
LOGS="$REAL.exitline.log $TEST_IMAGE.fork.log $TEST_IMAGE.signal.log"
# Removed here and not only in the trap: an uncatchable kill leaves a previous
# run's logs, and each holds the token its phase waits for.
# shellcheck disable=SC2086
rm -f $LOGS

QPID=""
FEED="$TEST_IMAGE.exitline.feed"

# The console is driven through tests/system/drive.sh: a line is typed when
# the prompt it answers has arrived, under a bound.
REPO=$(cd "$(dirname "$0")/../../.." && pwd)
# shellcheck source=tests/system/drive.sh
. "$REPO/tests/system/drive.sh"

stop() {
    qpid="$QPID"
    QPID=""
    drive_stop
    if [ -n "$qpid" ]; then
        kill "$qpid" 2>/dev/null || true
        wait "$qpid" 2>/dev/null || true
    fi
    rm -f "$FEED"
}

# A machine on the fifo and the log `$1`, and the feeder that types at it.
boot_driven() {
    DRIVE_LOG=$1
    shift
    rm -f "$FEED"
    mkfifo "$FEED"
    qemu-system-i386 -cpu max -display none "$@" -serial stdio < "$FEED" > "$DRIVE_LOG" 2>&1 &
    QPID=$!
    DRIVE_FIFO=$FEED
    drive_feed "$QPID"
}

# The debug shell's command, typed once its prompt has been printed.
shell_command() {
    if ! drive_expect "Type 'help'" 60; then
        stop
        echo "exitline-test: FAIL: no debug shell on $DRIVE_LOG" >&2
        exit 1
    fi
    drive_type "$1"
}

# shellcheck disable=SC2086
cleanup() {
    stop
    rm -f $LOGS
}

trap 'cleanup' EXIT
trap 'cleanup; exit 130' INT
trap 'cleanup; exit 143' TERM
trap 'cleanup; exit 129' HUP

# Wait until serial log $1 holds token $2, giving up after $3 seconds. Gating on
# a token rather than a fixed sleep keeps a loaded host from killing QEMU before
# the console has had its say.
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

# 1. The quiet boot. `echo` is an ash builtin and would not fork at all, so the
# command runs busybox by path: that forks, execs and exits, which is precisely
# the exit that used to print. What is waited for is the SECOND token, printed
# by a builtin, because the first is written by the child before it exits and
# stopping there could kill the machine in the window where a wrongly enabled
# line had not been printed yet. A builtin runs only once the child is reaped,
# so its token puts the exit in the past. Both tokens are written split: the
# console echoes what is typed, and a command holding a whole token would put
# it in the log before it ran.
boot_driven "$REAL.exitline.log" -kernel "$KERNEL" -initrd "$REAL"
if ! drive_login "$USER_NAME" "$PASSWORD" 90; then
    stop
    echo "exitline-test: FAIL: os-real did not log its user in" >&2
    exit 1
fi
drive_type '/bin/busybox echo FORKED""-AND-EXITED; echo REAPED""-OK'
if ! wait_serial "$REAL.exitline.log" "REAPED-OK" 90; then
    stop
    echo "exitline-test: FAIL: os-real never ran the forked program, so its" >&2
    echo "exitline-test: silence about process exits would prove nothing" >&2
    exit 1
fi
stop
if grep -q "$PLAIN" "$REAL.exitline.log"; then
    echo "exitline-test: FAIL: an image running init still prints the plain" >&2
    echo "exitline-test: process-exit line" >&2
    exit 1
fi
echo "exitline-test: an image running init prints no plain exit line: OK"

# 2. The debug shell, where the line is what a person reads. forktest's child
# exits with a known status and the probe then exits with it, so a working
# print appears for each. The two are named by their statuses rather than
# counted, so that two copies of the probe's own exit from a kernel that had
# stopped printing its children's could not pass for the pair.
boot_driven "$TEST_IMAGE.fork.log" -kernel "$KERNEL" -initrd "$TEST_IMAGE"
shell_command 'exec /forktest'
if ! wait_serial "$TEST_IMAGE.fork.log" "$PLAIN 42" 90; then
    stop
    echo "exitline-test: FAIL: the debug shell did not print the forked" >&2
    echo "exitline-test: child's exit, which the harness reads a status from" >&2
    exit 1
fi
if ! wait_serial "$TEST_IMAGE.fork.log" "$PLAIN 0" 30; then
    stop
    echo "exitline-test: FAIL: the debug shell printed the child's exit but" >&2
    echo "exitline-test: not the probe's own" >&2
    exit 1
fi
stop
if ! grep -qE "^<[0-9]>\[[0-9]+\.[0-9][0-9]\] sys_exit: init process [0-9]+ exited with status 0, returning to kernel shell" \
        "$TEST_IMAGE.fork.log"; then
    echo "exitline-test: FAIL: the line that hands the shell back is not the" >&2
    echo "exitline-test: one the probe runner scores a probe from" >&2
    exit 1
fi
echo "exitline-test: the debug shell prints it for a child and a probe: OK"

# 3. The two markers the probe runner scores from, neither of which is gated and
# neither of which may become so. vmfault writes through a wild pointer on
# purpose. Each is matched whole, prefix and tail included: a substring would
# accept a line renamed at either end, and the runner matches the whole thing.
boot_driven "$TEST_IMAGE.signal.log" -kernel "$KERNEL" -initrd "$TEST_IMAGE"
shell_command 'exec /vmfault'
if ! wait_serial "$TEST_IMAGE.signal.log" "killed by signal" 90; then
    stop
    echo "exitline-test: FAIL: a probe killed by a signal printed no marker," >&2
    echo "exitline-test: so the probe runner would score it as never having run" >&2
    exit 1
fi
stop
if ! grep -qE "^<[0-9]>\[[0-9]+\.[0-9][0-9]\] terminate: init process [0-9]+ killed by signal 11, returning to kernel shell" \
        "$TEST_IMAGE.signal.log"; then
    echo "exitline-test: FAIL: the signal marker is not the whole line" >&2
    echo "exitline-test: the probe runner matches, so a probe killed on purpose" >&2
    echo "exitline-test: would stop being scored" >&2
    exit 1
fi
echo "exitline-test: a killed probe still prints its dedicated marker: OK"

# 4. the harness with no --ok, which reads the plain line and nothing else: it
# stops the machine through the debug-exit port and then takes the real,
# untruncated status off the serial line. A gate that silenced that line
# everywhere would fail here rather than in a later milestone.
if ! build/tests/system/harness --kernel "$HARNESS_KERNEL" --initrd "$TEST_IMAGE" \
        --timeout 90 "execq=/hello" >/dev/null 2>&1; then
    echo "exitline-test: FAIL: the harness could not decode a status in its" >&2
    echo "exitline-test: no-token mode" >&2
    exit 1
fi
echo "exitline-test: the harness still decodes a status with no token: OK"
echo "exitline-test: PASS"
