#!/bin/sh
# Check BusyBox adaptive ping on a private Linux loopback, with replies delayed
# and reads held by strace: the send count, the retries and the ends.
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

# The inner shell is the first process of a PID namespace of its own, so the
# kernel kills every process it started when it exits by any path, a child
# whose PID it had not yet recorded among them.
unshare --user --map-root-user --net --pid --fork sh -eu -s -- "$1" "$work" <<'EOF'
busybox=$1
work=$2
flood=
trap '[ -z "$flood" ] || { kill "$flood" 2>/dev/null; wait "$flood" 2>/dev/null; } || :' EXIT
trap 'exit 1' HUP INT TERM

# Print the return value of the system call on strace's line LINE, -1 for an
# error.
result='function result(line) {
    if (!match(line, / = -?[0-9]+( [A-Z]+)?( \(.*\))?$/))
        return "?"
    return substr(line, RSTART + 3) + 0
}'

ip link set lo up
tc qdisc add dev lo root netem delay 100ms
# Rows that set no capability bound ping on root's raw socket; the others drop
# CAP_NET_RAW and ping on a datagram socket, as an ordinary account does,
# which holds only the ping's own replies, where a raw socket would also hold
# its requests.
echo '0 0' > /proc/sys/net/ipv4/ping_group_range

for address in 127.0.0.1 ::1; do
    for mode in zero adaptive; do
        case $mode in
            zero) set -- -i 0 ;;
            adaptive) set -- -A -i 0.01 ;;
        esac
        if ! strace -qq -s 256 -o "$work/trace" \
            -e trace=sendto,write,writev,setitimer,alarm \
            "$busybox" ping "$@" -c 2 -w 5 "$address" > "$work/output" ||
            ! grep -qFx '2 packets transmitted, 2 packets received, 0% packet loss' "$work/output" ||
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

# Retries with replies arriving between them. Each reply comes 200 ms after
# its request, so the first polls time out and the 10 ms retries fall due,
# and the replies then come while retries go on, until 30 requests are out.
# The row passes on the order of the sends, polls and reads that strace
# records: a send after a poll that timed out, which is a due retry; between
# two polls no more sends than one more than the replies read, since a pass
# sends at most one retry and a reply at most one request; a read after each
# poll that found the socket readable; and no read after one that did not. A
# loop that sent retries without polling between them starved reception
# whenever its pass outlasted the interval; strace cannot lengthen a pass
# that makes no system call, so the row checks the order that rules it out.
for address in 127.0.0.1 ::1; do
    status=0
    setpriv --bounding-set=-net_raw strace -qq -o "$work/trace" \
        -e trace=sendto,recvmsg,/^ppoll \
        "$busybox" ping -q -A -i 0.01 -c 30 -w 10 "$address" > "$work/output" ||
        status=$?
    if [ "$status" -ne 0 ] ||
        ! grep -qFx '30 packets transmitted, 30 packets received, 0% packet loss' "$work/output" ||
        ! awk "$result"'
            function segment() {
                if (sent > 1 + read)
                    bad = bad " " sent " sends after one poll and " read " reads;"
                if (polled && ready > 0 && !read)
                    bad = bad " no read after a readable poll;"
            }
            /^sendto\(/ {
                sends++
                sent++
                if (polled && ready == 0)
                    retries++
            }
            /^recvmsg\(/ {
                if (!polled || ready <= 0)
                    bad = bad " a read with no readable poll;"
                if (result($0) > 0)
                    read++
            }
            /^ppoll/ {
                segment()
                polled = 1
                ready = result($0)
                sent = read = 0
            }
            END {
                segment()
                if (bad != "" || !retries || sends != 30) {
                    print "ping-delay:" bad " " retries " retries, " sends " sends" > "/dev/stderr"
                    exit 1
                }
            }
        ' "$work/trace"; then
        echo "ping-delay: $address retry: expected due retries with a poll between any two and a read after each readable poll, status $status" >&2
        cat "$work/output" >&2
        exit 1
    fi
    echo "ping-delay: $address retry: passed"
done
tc qdisc del dev lo root

# A send that takes longer than the final wait: strace holds the send 1.5 s at
# its entry, and the reply, which loopback returns at once, must still be
# counted, since the 1 s wait begins when the send returns. The trace must
# show the held send.
for address in 127.0.0.1 ::1; do
    status=0
    setpriv --bounding-set=-net_raw strace -qq -o "$work/trace" \
        -e trace=sendto -e inject=sendto:delay_enter=1500000 \
        "$busybox" ping -q -A -c 1 -W 1 "$address" > "$work/output" ||
        status=$?
    if [ "$status" -ne 0 ] ||
        ! grep -qFx '1 packets transmitted, 1 packets received, 0% packet loss' "$work/output" ||
        ! awk "$result"'
            /^sendto\(/ && result($0) > 0 { sends++ }
            END { exit sends != 1 }
        ' "$work/trace"; then
        echo "ping-delay: $address slow send: expected the reply to the held send counted, status $status" >&2
        cat "$work/output" "$work/trace" >&2
        exit 1
    fi
    echo "ping-delay: $address slow send: passed"
done

# A reply read after -w, and one read after the final wait: strace holds the
# read 1.5 s at its entry, after the poll has found the reply queued, the
# final wait began at the end of the send and -w at the start, so the read
# returns past the 1 s of either. The trace must show the read that returned
# the reply, and the statistics must not count it.
for address in 127.0.0.1 ::1; do
    for end in -w -W; do
        status=0
        setpriv --bounding-set=-net_raw strace -qq -o "$work/trace" \
            -e trace=recvmsg -e inject=recvmsg:delay_enter=1500000 \
            "$busybox" ping -q -A -c 1 "$end" 1 "$address" > "$work/output" ||
            status=$?
        if [ "$status" -ne 1 ] ||
            ! grep -qFx '1 packets transmitted, 0 packets received, 100% packet loss' "$work/output" ||
            ! awk "$result"'
                /^recvmsg\(/ && result($0) > 0 { reads++ }
                END { exit reads != 1 }
            ' "$work/trace"; then
            echo "ping-delay: $address late $end: expected one held read of the reply, not counted, status $status" >&2
            cat "$work/output" "$work/trace" >&2
            exit 1
        fi
        echo "ping-delay: $address late $end: passed"
    done
done

# A socket that does not empty. On a raw socket, which root's ping opens and
# which receives every ICMP message of the network namespace, a flood of
# echoes to the loopback address arrives far faster than ping reads it while
# strace holds each read 5 ms. Ping sends to an address on a dummy link, which
# never answers, so it must send two retries 50 ms apart and end at its 1 s
# final wait. The row passes on the order strace records: each retry follows
# a poll that found the socket readable and the read after it, with no poll
# that timed out between, and the last poll before ping exits found the
# socket readable and was followed by its read. A loop that returns to the
# read whenever the socket is readable sends no retry and does not end; the
# -w of 30 s ends such a ping and fails the row.

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
    "$busybox" ping -q -i 0 "$loopback" > /dev/null &
    flood=$!
    # Start the tested ping once the flood runs; the verdict is the trace's.
    while [ "$(replies "$loopback")" -lt $((base + 10000)) ]; do
        if ! kill -0 "$flood" 2>/dev/null; then
            echo "ping-delay: $silent busy: the flood to $loopback ended before it began" >&2
            exit 1
        fi
        sleep 0.01
    done
    status=0
    strace -qq -o "$work/trace" -e trace=sendto,recvmsg,/^ppoll \
        -e inject=recvmsg:delay_exit=5000 \
        "$busybox" ping -q -A -c 3 -i 0.05 -W 1 -w 30 "$silent" > "$work/output" ||
        status=$?
    kill "$flood" 2>/dev/null || :
    wait "$flood" 2>/dev/null || :
    flood=
    if [ "$status" -ne 1 ] ||
        ! grep -qFx '3 packets transmitted, 0 packets received, 100% packet loss' "$work/output" ||
        ! awk "$result"'
            /^sendto\(/ {
                sends++
                if (sends > 1 && !(ready > 0 && read))
                    held = 1
            }
            /^recvmsg\(/ && result($0) > 0 { read++ }
            /^ppoll/ {
                ready = result($0)
                read = 0
            }
            END { exit !(sends == 3 && !held && ready > 0 && read) }
        ' "$work/trace"; then
        echo "ping-delay: $silent busy: expected each retry and the end after a readable poll and its read, status $status" >&2
        cat "$work/output" >&2
        exit 1
    fi
    echo "ping-delay: $silent busy: passed"
done
EOF
