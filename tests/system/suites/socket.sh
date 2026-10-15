#!/usr/bin/env sh
#
# socket.sh: the guest as a network client, against a real host peer.
#
# udptest and tcptest send a string to an echo server at the user network's
# gateway (10.0.2.2, which is the host's loopback) and require the same bytes
# back. The port is the fifth argument, 7777 when it is not given;
# tests/system/suites.mk moves it with the run's block of ports, and each
# probe is given it as its argument. The SSH suites have the guest listening;
# this suite checks the guest's client path, end to end through a peer that
# answers only frames that are right.
#
# The server is one `hostpeer echo`, started here and stopped in the trap,
# serving both boots.
#
# Usage: socket.sh <kernel> <image> <harness> <hostpeer> [port]
#
# The runner helper, whose `until` and `gone` are the suite's waits.
RUNUTIL="$(cd "$(dirname "$0")/../../.." && pwd)/build/tests/system/runutil"
set -u

KERNEL="$1"
IMAGE="$2"
# What this suite writes under the images directory starts with `socket-test`.
OUT="$(dirname "$IMAGE")/socket-test"
HARNESS="$3"
HOSTPEER="$4"
PORT="${5:-7777}"

if [ ! -x "$HARNESS" ] || [ ! -x "$HOSTPEER" ]; then
    echo "socket-test: the harness and hostpeer must be built first" >&2
    exit 2
fi

PEER=""
PEER_LOG="$OUT.peer.log"
# Stop the echo server, if it runs, and reap it.
stop() {
    [ -n "$PEER" ] && kill "$PEER" 2>/dev/null
    [ -n "$PEER" ] && wait "$PEER" 2>/dev/null
    PEER=""
}
trap 'stop' EXIT INT TERM HUP
rm -f "$PEER_LOG"

"$HOSTPEER" echo "$PORT" 200 >"$PEER_LOG" 2>&1 &
PEER=$!
# The server must be listening before the machine boots: a datagram that
# reaches a closed port is discarded, and the probe would spend its retries
# on nothing. The wait is for the server's line saying that it listens, since
# the shell has the child's pid before the child has bound anything. The
# script makes no connection of its own: the server records each peer that
# closes its end, and a connection from here would leave the same record as
# the machine's.
if ! "$RUNUTIL" until "$PEER_LOG" "hostpeer: echoing" 30; then
    echo "socket-test: FAIL: nothing is listening on port $PORT" >&2
    exit 1
fi

# run_probe PROBE TEXT: boot the image, run PROBE with the port, and require
# TEXT; return 1 when the boot fails.
run_probe() {
    if ! "$HARNESS" --kernel "$KERNEL" --initrd "$IMAGE" \
            --qemu-arg=-cpu --qemu-arg=max \
            --timeout 150 --ok "$2" --never "Some tests failed!" \
            "expect=Type 'help'" "send=exec /$1 $PORT" "expect=$2"; then
        echo "socket-test: FAIL: $1 did not report its echo" >&2
        return 1
    fi
    echo "socket-test: $1: OK"
}

run_probe udptest "All UDP tests passed!" || exit 1
run_probe tcptest "All TCP tests passed!" || exit 1

# The graceful close, which the probe cannot see from inside, since a `close`
# that returned zero without sending anything looks the same to it: the
# server's log must record the end of the stream.
stop
if ! grep -q "the peer closed its end" "$PEER_LOG"; then
    echo "socket-test: FAIL: the machine's close did not reach the server," >&2
    echo "socket-test: so the connection was dropped rather than closed" >&2
    exit 1
fi
echo "socket-test: tcptest closed its connection: OK"
echo "socket-test: PASS"
