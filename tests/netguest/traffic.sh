#!/bin/sh
# traffic.sh: what run.sh has netguest.sh run while the guest has the card,
# in the environment netguest.sh gives its command.
#
# Usage: traffic.sh transfers|deny|kill
#
# `transfers` has nettput, beside this script, send and receive 8388608
# bytes through the guest to NETGUEST_PEER's port NETGUEST_PORT and make
# 1000 exchanges and 200 connections, each of which must reach its count.
# `deny` does the same, then makes ten attempts at the peer's port 9, which
# the guest's filter drops, and waits for a report on the guest's console,
# NETGUEST_OUT/console, whose deny9 count is above 0. `kill` starts a send
# that does not end and returns once tap0 has sent a mebibyte more than it
# had, leaving the send running for netguest.sh to kill QEMU under it. A
# wait ends early, with status 1, when QEMU, NETGUEST_QEMU_PID, has ended.
# The status is 0 when every count was reached, 1 otherwise, 2 for a usage
# error.

here=$(cd "$(dirname "$0")" && pwd)
nettput=$here/nettput
peer=${NETGUEST_PEER:?} port=${NETGUEST_PORT:?}

# tap_sent: the bytes tap0 has sent, as /proc/net/dev counts them.
tap_sent()
{
    awk '$1 == "tap0:" { print $10 }' /proc/net/dev
}

transfers()
{
    "$nettput" -c "$peer" -p "$port" -n 8388608 send &&
        "$nettput" -c "$peer" -p "$port" -n 8388608 recv &&
        "$nettput" -c "$peer" -p "$port" -n 1000 rr &&
        "$nettput" -c "$peer" -p "$port" -n 200 crr
}

case ${1:-} in
    transfers)
        transfers || exit 1
        ;;
    deny)
        transfers || exit 1
        "$nettput" -c "$peer" -p 9 -n 10 wait
        until tr -d '\r' < "$NETGUEST_OUT/console" | grep -a 'NETGUEST STATS' | tail -n 1 |
            grep -q 'deny9=[1-9]'; do
            kill -0 "$NETGUEST_QEMU_PID" 2> /dev/null || exit 1
            sleep 1
        done
        ;;
    kill)
        start=$(tap_sent)
        "$nettput" -c "$peer" -p "$port" -n 4294967295 send > /dev/null 2>&1 &
        until [ $(($(tap_sent) - start)) -ge 1048576 ]; do
            kill -0 "$NETGUEST_QEMU_PID" 2> /dev/null || exit 1
            sleep 0.2
        done
        echo "traffic: tap0 sent $(($(tap_sent) - start)) bytes of the send"
        ;;
    *)
        echo "usage: traffic.sh transfers|deny|kill" >&2
        exit 2
        ;;
esac
