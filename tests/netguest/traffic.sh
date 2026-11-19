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
# attempts at once at the peer's port 9, each of which must go unanswered
# and unrefused until Rach2's TCP gives its handshake up, waits for the
# guest's next report and prints it the same way; the guest's deny9 count
# must have grown by at least the SYNs Rach2 sent meanwhile, the
# connections it opened and the segments it sent again as /proc/net/snmp
# counts them, so that the guest dropped every SYN of every attempt. `kill`
# starts a send that
# does not end, writes its process ID to NETGUEST_OUT/sender, and returns
# once tap0 has sent a mebibyte more than it had, leaving the send running
# for netguest.sh to kill QEMU under it. A wait ends early, with status 1,
# when QEMU, NETGUEST_QEMU_PID, or the send has ended. The status is 0 when
# every count was reached, 1 otherwise, 2 for a usage error.

here=$(cd "$(dirname "$0")" && pwd)
nettput=$here/nettput
peer=${NETGUEST_PEER:?} port=${NETGUEST_PORT:?}

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

# report_deny N: the deny9 count of the guest's report N, or nothing.
report_deny()
{
    tr -d '\r' < "$NETGUEST_OUT/console" | grep -a 'NETGUEST STATS' | sed -n "${1}p" |
        sed -n 's/.*deny9=\([0-9][0-9]*\).*/\1/p'
}

# syns_sent: the connections Rach2's TCP has opened and the segments it has
# sent again, summed, as /proc/net/snmp counts them.
syns_sent()
{
    awk '$1 == "Tcp:" { if (!h) { for (i = 2; i <= NF; i++) c[$i] = i; h = 1; next }
        print $(c["ActiveOpens"]) + $(c["RetransSegs"]); exit }' /proc/net/snmp
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
            echo "traffic: the guest's report after the transfers has no deny9 count" >&2
            exit 1
        }
        sent=$(syns_sent)
        "$nettput" -c "$peer" -p 9 -n 10 drop || exit 1
        sent=$(($(syns_sent) - sent))
        report_after "$(reports)"
        after=$(report_deny "$judged")
        [ -n "$after" ] || {
            echo "traffic: the guest's report after the attempts has no deny9 count" >&2
            exit 1
        }
        echo "traffic: port 9: $sent SYNs sent, $((after - before)) counted by the guest"
        [ $((after - before)) -ge "$sent" ] || {
            echo "traffic: the guest counted $((after - before)) of the $sent SYNs sent to port 9" >&2
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
