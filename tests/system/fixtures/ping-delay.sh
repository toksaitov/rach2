#!/bin/sh
# Check BusyBox adaptive ping on a private Linux loopback, with replies delayed
# and reads and sends held by strace: the send count, the retries and the ends.
set -eu

if [ "$#" -ne 1 ] || [ ! -x "$1" ]; then
    echo 'usage: sh tests/system/fixtures/ping-delay.sh PATH-TO-BUSYBOX' >&2
    exit 2
fi

work=
trap '[ -z "$work" ] || rm -rf "$work"' EXIT
trap 'exit 1' HUP INT TERM
scratch=${TMPDIR:-build/tmp}
mkdir -p "$scratch"
work=$(mktemp -d "$scratch/ping-delay.XXXXXX")

unshare --user --map-root-user --net sh -eu -s -- "$1" "$work" <<'EOF'
busybox=$1
work=$2
flood=
trap '[ -z "$flood" ] || { kill "$flood" 2>/dev/null; wait "$flood" 2>/dev/null; } || :' EXIT
trap 'exit 1' HUP INT TERM
ip link set lo up
tc qdisc add dev lo root netem delay 100ms

for address in 127.0.0.1 ::1; do
    for mode in zero adaptive; do
        case $mode in
            zero) set -- -i 0 ;;
            adaptive) set -- -A -i 0.01 ;;
        esac
        if ! strace -qq -s 256 -o "$work/trace" \
            -e trace=sendto,write,writev,setitimer,alarm \
            "$busybox" ping "$@" -c 2 -w 5 "$address" > "$work/output" ||
            ! grep -Fx '2 packets transmitted, 2 packets received, 0% packet loss' "$work/output" ||
            ! awk '
                /^sendto\(/ { sends++ }
                /^(write|writev)\(1,.*seq=[0-9]+ / && !seen_reply {
                    sends_before_reply = sends
                    seen_reply = 1
                }
                /^(alarm|setitimer)\(/ { timer = 1 }
                END { exit !(sends == 2 && sends_before_reply == 2 && !timer) }
            ' "$work/trace"; then
            echo "ping-delay: $address $mode: expected two sends before the first reply and no signal timer" >&2
            cat "$work/output" "$work/trace" >&2
            exit 1
        fi
        echo "ping-delay: $address $mode: passed"
    done
done

# The rows below ping on a datagram socket, as an ordinary account does,
# unless they say otherwise: it holds only the ping's own replies, where a raw
# socket would also hold its requests.
tc qdisc del dev lo root
echo '0 0' > /proc/sys/net/ipv4/ping_group_range

# A retry due at every pass, with replies queued: strace holds each send
# 20 ms at its exit, so the 1 ms interval, which runs from the start of the
# send, has passed when the loop next looks, and the reply to the send is
# queued by then. The loop must read replies between retries, about one for
# every two sends, since each reply sends a request and each pass a retry; a
# loop that sends without polling whenever a retry is due reads none.
for address in 127.0.0.1 ::1; do
    status=0
    setpriv --bounding-set=-net_raw strace -qq -o /dev/null \
        -e trace=sendto -e inject=sendto:delay_exit=20000 \
        "$busybox" ping -q -A -i 0.001 -w 1 "$address" > "$work/output" ||
        status=$?
    if [ "$status" -ne 0 ] || ! awk '
            / packets transmitted, / { sent = $1; got = $4 }
            END { exit !(got > 0 && 3 * got >= sent) }
        ' "$work/output"; then
        echo "ping-delay: $address retry: expected a reply read for every three sends, status $status" >&2
        cat "$work/output" >&2
        exit 1
    fi
    echo "ping-delay: $address retry: passed"
done

# A reply read after -w, and one read after the final wait: strace holds the
# read 1.5 s at its entry, after the poll has found the reply queued and the
# loop has last read the clock, which takes it past the 1 s deadline or wait.
# Neither reply may be counted.
for address in 127.0.0.1 ::1; do
    for end in -w -W; do
        status=0
        setpriv --bounding-set=-net_raw strace -qq -o /dev/null \
            -e trace=recvmsg -e inject=recvmsg:delay_enter=1500000 \
            "$busybox" ping -q -A -c 1 "$end" 1 "$address" > "$work/output" ||
            status=$?
        if [ "$status" -ne 1 ] ||
            ! grep -Fx '1 packets transmitted, 0 packets received, 100% packet loss' "$work/output"; then
            echo "ping-delay: $address late $end: expected the reply read after the end not counted, status $status" >&2
            cat "$work/output" >&2
            exit 1
        fi
        echo "ping-delay: $address late $end: passed"
    done
done

# A socket that never empties. On a raw socket, which root's ping opens and
# which receives every echo of the network namespace, a flood of echoes to the
# loopback address arrives far faster than ping reads it while strace holds
# each read 5 ms. Ping sends to an address on a dummy link, which never
# answers, and must send its two retries 50 ms apart, reading flood packets
# before each, and end its 1 s final wait while the flood, bounded at 10 s by
# its -w, still runs. A loop that returns to the read whenever the socket is
# readable sends no retry and ends no wait until the flood stops.

# Print the network namespace's count of echo replies received for ADDRESS's
# family.
replies() {
    case $1 in
        *:*) awk '$1 == "Icmp6InEchoReplies" { print $2 }' /proc/net/snmp6 ;;
        *) awk '$1 == "Icmp:" {
                   if ($2 ~ /^[0-9]+$/) print $column
                   else for (i = 2; i <= NF; i++) if ($i == "InEchoReps") column = i
               }' /proc/net/snmp ;;
    esac
}

ip link add d0 type dummy
ip link set d0 up
ip addr add 10.9.0.1/24 dev d0
ip addr add fd00:9::1/64 dev d0 nodad
for pair in 127.0.0.1,10.9.0.2 ::1,fd00:9::2; do
    loopback=${pair%,*}
    silent=${pair#*,}
    base=$(replies "$loopback")
    "$busybox" ping -q -i 0 -w 10 "$loopback" > /dev/null &
    flood=$!
    while [ "$(replies "$loopback")" -lt $((base + 10000)) ]; do
        if ! kill -0 "$flood" 2>/dev/null; then
            echo "ping-delay: $silent busy: the flood to $loopback ended before it began" >&2
            exit 1
        fi
        sleep 0.01
    done
    status=0
    strace -qq -o "$work/trace" -e trace=sendto,recvmsg \
        -e inject=recvmsg:delay_exit=5000 \
        "$busybox" ping -q -A -c 3 -i 0.05 -W 1 "$silent" > "$work/output" ||
        status=$?
    flooding=0
    ! kill -0 "$flood" 2>/dev/null || flooding=1
    kill "$flood" 2>/dev/null || :
    wait "$flood" 2>/dev/null || :
    flood=
    if [ "$status" -ne 1 ] || [ "$flooding" -ne 1 ] ||
        ! grep -Fx '3 packets transmitted, 0 packets received, 100% packet loss' "$work/output" ||
        ! awk '
            /^sendto\(/ { sends++; next }
            /^recvmsg\(/ { reads[sends]++ }
            END { exit !(sends == 3 && reads[1] && reads[2] && reads[3]) }
        ' "$work/trace"; then
        echo "ping-delay: $silent busy: expected three sends and the end while the flood ran, status $status, flood running $flooding" >&2
        cat "$work/output" >&2
        exit 1
    fi
    echo "ping-delay: $silent busy: passed"
done
EOF
