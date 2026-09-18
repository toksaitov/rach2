#!/usr/bin/env sh
#
# socket.sh: the guest as a network client, against a real host peer.
#
# udptest and tcptest send a string to an echo server at the user network's
# gateway (10.0.2.2:7777, which is the host's loopback) and require the same
# bytes back. Nothing else in the suite makes the guest open a connection of
# its own: the SSH suites have the guest listening, and the capture suites
# read what it sent without anyone answering. What is proved here is the
# other half, the guest's client path, end to end through a peer that would
# not answer if the frames were wrong.
#
# The server is one `hostpeer echo`, started here and stopped in the trap,
# serving both boots.
#
# Usage: socket.sh <kernel> <image> <harness> <hostpeer>
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
PORT=7777

if [ ! -x "$HARNESS" ] || [ ! -x "$HOSTPEER" ]; then
    echo "socket-test: the harness and hostpeer must be built first" >&2
    exit 2
fi

PEER=""
PEER_LOG="$OUT.peer.log"
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
# arrives at a closed port is discarded, and the probe would spend its retries
# on nothing. What is waited for is the server's own word once it has bound
# and listens, not a process that exists: the shell has a pid for the child
# before the child has run `hostpeer`, let alone bound anything. No
# connection is made from here: the server's record of a peer closing its
# end is the machine's close, and a connection of this script's would leave
# the same record.
if ! "$RUNUTIL" until "$PEER_LOG" "hostpeer: echoing" 30; then
    echo "socket-test: FAIL: nothing is listening on port $PORT" >&2
    exit 1
fi

run_probe() {
    if ! "$HARNESS" --kernel "$KERNEL" --initrd "$IMAGE" \
            --qemu-arg=-cpu --qemu-arg=max \
            --timeout 150 --ok "$2" --never "Some tests failed!" \
            "expect=Type 'help'" "send=exec /$1" "expect=$2"; then
        echo "socket-test: FAIL: $1 did not report its echo" >&2
        return 1
    fi
    echo "socket-test: $1: OK"
}

run_probe udptest "All UDP tests passed!" || exit 1
run_probe tcptest "All TCP tests passed!" || exit 1

# The graceful close, which the probe cannot prove from inside: a `close` that
# returned zero without sending anything looks the same to it. The server saw
# the end of the stream or it did not.
stop
if ! grep -q "the peer closed its end" "$PEER_LOG"; then
    echo "socket-test: FAIL: the machine's close did not reach the server," >&2
    echo "socket-test: so the connection was dropped rather than closed" >&2
    exit 1
fi
echo "socket-test: tcptest closed its connection: OK"
echo "socket-test: PASS"
