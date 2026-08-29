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
set -u

KERNEL="$1"
IMAGE="$2"
HARNESS="$3"
HOSTPEER="$4"
PORT=7777

if [ ! -x "$HARNESS" ] || [ ! -x "$HOSTPEER" ]; then
    echo "socket-test: the harness and hostpeer must be built first" >&2
    exit 2
fi

PEER=""
stop() {
    [ -n "$PEER" ] && kill "$PEER" 2>/dev/null
    PEER=""
}
trap 'stop' EXIT INT TERM HUP

"$HOSTPEER" echo "$PORT" 200 &
PEER=$!
# The server must be listening before the machine boots: a datagram that
# arrives at a closed port is discarded, and the probe would spend its
# retries on an empty room.
i=0
while [ "$i" -lt 50 ]; do
    if kill -0 "$PEER" 2>/dev/null; then
        break
    fi
    i=$((i + 1))
    sleep 0.1
done
if ! kill -0 "$PEER" 2>/dev/null; then
    echo "socket-test: FAIL: no echo server on port $PORT" >&2
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
echo "socket-test: PASS"
