#!/bin/sh
# traffic.sh: what run.sh has netguest.sh run while the guest has the card,
# in the environment netguest.sh gives its command.
#
# Usage: traffic.sh transfers|deny|kill
#
# `transfers` has nettput, beside this script, send and receive 8388608
# bytes through the guest to NETGUEST_PEER's port NETGUEST_PORT and make
# 1000 exchanges and 200 connections, each of which must reach its count,
# then waits for the guest's next report on its console,
# NETGUEST_OUT/console, and prints it as `traffic: report: LINE`, the
# report the case is judged by. `deny` does the same, then makes ten
# attempts at once at the peer's port NETGUEST_DENY, where the peer serves
# too, each of which must go unanswered and unrefused until Rach2's TCP
# gives its handshake up, so that the guest let no SYN of any attempt
# through; it waits for the guest's next report, prints it the same way,
# and requires the guest's deny count to have grown by at least one SYN of
# each attempt, so that its rule, not another, dropped them. `kill`
# starts a send that
# does not end, writes its process ID to NETGUEST_OUT/sender, and returns
# once tap0 has sent a mebibyte more than it had, leaving the send running
# for netguest.sh to kill QEMU under it. A wait ends early, with status 1,
# when QEMU, NETGUEST_QEMU_PID, or the send has ended. The status is 0 when
# every count was reached, 1 otherwise, 2 for a usage error.

here=$(cd "$(dirname "$0")" && pwd)
nettput=$here/nettput
peer=${NETGUEST_PEER:?} port=${NETGUEST_PORT:?} deny=${NETGUEST_DENY:?}

# tap_sent: the bytes tap0 has sent, as /proc/net/dev counts them.
tap_sent()
{
    awk '$1 == "tap0:" { print $10 }' /proc/net/dev
}

# alive PID: succeed while PID runs, a zombie counting as ended.
alive()
{
    s=$(sed -n 's/^.*) \([A-Z]\).*/\1/p' "/proc/$1/stat" 2> /dev/null)
    case $s in '' | Z | X) return 1 ;; esac
}

# reports: the guest's reports on its console so far.
reports()
{
    grep -a -c 'NETGUEST STATS' "$NETGUEST_OUT/console"
}

# report_after N: wait until the console holds more than N reports, and
# print the first of them as `traffic: report: LINE`; `judged` is its
# number.
report_after()
{
    judged=$(($1 + 1))
    # A report is whole once its last field, states, has begun.
    until tr -d '\r' < "$NETGUEST_OUT/console" | grep -a 'NETGUEST STATS' | sed -n "${judged}p" |
        grep -q 'states='; do
        kill -0 "$NETGUEST_QEMU_PID" 2> /dev/null || exit 1
        sleep 1
    done
    tr -d '\r' < "$NETGUEST_OUT/console" | grep -a 'NETGUEST STATS' | sed -n "${judged}p" |
        sed 's/^.*NETGUEST STATS/traffic: report:/'
}

# report_deny N: the deny count of the guest's report N, or nothing.
report_deny()
{
    tr -d '\r' < "$NETGUEST_OUT/console" | grep -a 'NETGUEST STATS' | sed -n "${1}p" |
        sed -n 's/.* deny=\([0-9][0-9]*\).*/\1/p'
}

transfers()
{
    "$nettput" -c "$peer" -p "$port" -n 8388608 send &&
        "$nettput" -c "$peer" -p "$port" -n 8388608 recv &&
        "$nettput" -c "$peer" -p "$port" -n 1000 rr &&
        "$nettput" -c "$peer" -p "$port" -n 200 crr || exit 1
    report_after "$(reports)"
}

case ${1:-} in
    transfers)
        transfers
        ;;
    deny)
        transfers
        before=$(report_deny "$judged")
        [ -n "$before" ] || {
            echo "traffic: the guest's report after the transfers has no deny count" >&2
            exit 1
        }
        # The peer answers on this port too, so that a SYN the guest let
        # through would be answered and fail drop.
        "$nettput" -c "$peer" -p "$deny" -n 10 drop || exit 1
        report_after "$(reports)"
        after=$(report_deny "$judged")
        [ -n "$after" ] || {
            echo "traffic: the guest's report after the attempts has no deny count" >&2
            exit 1
        }
        echo "traffic: port $deny: 10 attempts unanswered, $((after - before)) SYNs counted by the guest"
        [ $((after - before)) -ge 10 ] || {
            echo "traffic: the guest counted $((after - before)) SYNs to port $deny, fewer than the 10 attempts" >&2
            exit 1
        }
        ;;
    kill)
        start=$(tap_sent)
        "$nettput" -c "$peer" -p "$port" -n 4294967295 send > /dev/null 2>&1 &
        sender=$!
        echo "$sender" > "$NETGUEST_OUT/sender" || {
            kill "$sender"
            echo "traffic: the send's process ID could not be kept" >&2
            exit 1
        }
        until [ $(($(tap_sent) - start)) -ge 1048576 ]; do
            kill -0 "$NETGUEST_QEMU_PID" 2> /dev/null || exit 1
            alive "$sender" || {
                echo "traffic: the send ended before tap0 carried a mebibyte of it" >&2
                exit 1
            }
            sleep 0.2
        done
        echo "traffic: tap0 sent $(($(tap_sent) - start)) bytes of the send"
        ;;
    *)
        echo "usage: traffic.sh transfers|deny|kill" >&2
        exit 2
        ;;
esac
