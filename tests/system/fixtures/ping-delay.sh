#!/bin/sh
# Check BusyBox adaptive ping with replies delayed on a private Linux loopback:
# the send count, the retries and the deadline.
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

# A retry interval shorter than one pass of the loop, with replies queued:
# the replies are read between retries, so some are counted. The short delay
# and the long queue keep netem from dropping the flood's requests, which a
# raw IPv6 socket reports as ENOBUFS.
tc qdisc change dev lo root netem delay 10ms limit 100000
for address in 127.0.0.1 ::1; do
    status=0
    "$busybox" ping -q -A -i 0.000002 -w 1 "$address" > "$work/output" ||
        status=$?
    if [ "$status" -ne 0 ] ||
        ! grep -E ' packets transmitted, [1-9][0-9]* packets received' "$work/output"; then
        echo "ping-delay: $address retry: expected replies read between retries, status $status" >&2
        cat "$work/output" >&2
        exit 1
    fi
    echo "ping-delay: $address retry: passed"
done

# A reply readable only after -w: ping is stopped in its wait across the
# deadline and continued once the reply, 1.5 s out, has arrived. It must not
# count the reply. Without CAP_NET_RAW it opens a datagram socket, as an
# ordinary account does, which holds only the reply, where a raw IPv4 socket
# would first read the ping's own request and check -w again before the reply.
tc qdisc change dev lo root netem delay 750ms
echo '0 0' > /proc/sys/net/ipv4/ping_group_range
for address in 127.0.0.1 ::1; do
    status=0
    setpriv --bounding-set=-net_raw \
        "$busybox" ping -q -A -c 1 -w 1 "$address" > "$work/output" &
    pid=$!
    sleep 0.3
    kill -STOP "$pid"
    sleep 2.2
    kill -CONT "$pid"
    wait "$pid" || status=$?
    if [ "$status" -ne 1 ] ||
        ! grep -Fx '1 packets transmitted, 0 packets received, 100% packet loss' "$work/output"; then
        echo "ping-delay: $address late: expected the reply after -w not counted, status $status" >&2
        cat "$work/output" >&2
        exit 1
    fi
    echo "ping-delay: $address late: passed"
done
EOF
