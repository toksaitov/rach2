#!/bin/sh
# Check BusyBox adaptive ping on a private Linux loopback: the send count and
# the retry against an echo responder that holds its replies, and the final
# wait and -w each alone against silence.
set -eu

if [ "$#" -ne 1 ] || [ ! -x "$1" ]; then
    echo 'usage: sh tests/system/fixtures/ping-delay.sh PATH-TO-BUSYBOX' >&2
    exit 2
fi

scratch=${TMPDIR:-build/tmp}
mkdir -p "$scratch"
# The inner shell is PID 1 of a PID namespace whose /proc is its own, in user,
# network and mount namespaces of its own, and runs every responder, tracer
# and ping. When it exits, the kernel kills what is left in the namespace;
# when unshare is killed, --kill-child kills the inner shell. unshare ignores
# INT and TERM, so a stalled row is cancelled with KILL, or with the
# terminal's INT, which reaches the inner shell's trap. The scratch is a
# private tmpfs mounted over the scratch root, which goes with the namespace
# however it ends. BusyBox is open on descriptor 3 before the mount, which
# may hide its path.
exec 3< "$1"
exec unshare --user --map-root-user --net --pid --fork --kill-child=KILL \
    --mount-proc sh -eu -s -- "$scratch" <<'EOF'
scratch=$1
work=
trap 'kill -9 -1 2>/dev/null || :; wait || :; [ -z "$work" ] || rm -rf "$work"' EXIT
trap 'exit 1' HUP INT TERM
mount -t tmpfs ping-delay "$scratch"
work=$(mktemp -d "$scratch/ping-delay.XXXXXX")
busybox=$work/busybox
cat <&3 > "$busybox"
exec 3<&-
chmod 755 "$busybox"

ip link set lo up
# The kernel answers no echo; the responder below does, or nobody does.
echo 1 > /proc/sys/net/ipv4/icmp_echo_ignore_all
echo 1 > /proc/sys/net/ipv6/icmp/echo_ignore_all
# Rows on the raw socket run as root; rows on a datagram socket drop
# CAP_NET_RAW, as an ordinary account has none, and ping then opens a
# datagram socket, which this range lets group 0 open.
echo '0 0' > /proc/sys/net/ipv4/ping_group_range

# An echo responder on a raw socket of ADDRESS's family. It prints "ready"
# once the socket is open, collects two echo requests with distinct
# identifiers or sequence numbers, and only then answers both, in order, and
# exits.
responder='
import socket, struct, sys

v6 = ":" in sys.argv[1]
if v6:
    s = socket.socket(socket.AF_INET6, socket.SOCK_RAW, socket.IPPROTO_ICMPV6)
    request, reply = 128, 129
else:
    s = socket.socket(socket.AF_INET, socket.SOCK_RAW, socket.IPPROTO_ICMP)
    request, reply = 8, 0
print("ready", flush=True)
sys.stdout.close()

def checksum(data):
    if len(data) % 2:
        data += b"\0"
    total = sum(struct.unpack("!%dH" % (len(data) // 2), data))
    while total >> 16:
        total = (total & 0xffff) + (total >> 16)
    return ~total & 0xffff

held = []
while len(held) < 2:
    packet, peer = s.recvfrom(65536)
    if not v6:
        packet = packet[(packet[0] & 15) * 4:]
    if len(packet) < 8 or packet[0] != request:
        continue
    if any(packet[4:8] == old[4:8] for old, _ in held):
        continue
    held.append((packet, peer))
for packet, peer in held:
    echo = bytes([reply, 0, 0, 0]) + packet[4:]
    # The kernel fills in the checksum of a raw ICMPv6 socket.
    if not v6:
        echo = echo[:2] + struct.pack("!H", checksum(echo)) + echo[4:]
    s.sendto(echo, peer)
'

# Print the return value of the system call on strace's line LINE, -1 for an
# error.
result='function result(line) {
    if (!match(line, / = -?[0-9]+( [A-Z]+)?( \(.*\))?$/))
        return "?"
    return substr(line, RSTART + 3) + 0
}'

failed=0
sends=0
timers=0
status=0

# Run ping on SOCKET, raw or dgram, with ARGS under strace, with at most MAX
# successful sends, and set sends, timers (alarm and setitimer calls) and
# status. The trace is read as strace writes it, and a send past MAX kills
# ping at once and sets status to 125. INJECT, when set, is strace's inject
# option. Ping's output is in $work/output.
trace_ping() {
    max=$1
    socket=$2
    shift 2
    prefix=
    [ "$socket" = raw ] || prefix='setpriv --bounding-set=-net_raw'
    rm -f "$work/trace"
    mkfifo "$work/trace"
    $prefix strace -qq --kill-on-exit -o "$work/trace" \
        -e trace=sendto,alarm,setitimer ${inject:+-e "$inject"} \
        "$busybox" ping "$@" > "$work/output" &
    tracer=$!
    status=0
    counts=$(awk -v max="$max" "$result"'
        # END still runs after this exit and prints the counts.
        /^sendto\(/ && result($0) > 0 && ++sends > max { exit 1 }
        /^(alarm|setitimer)\(/ { timers++ }
        END { print sends + 0, timers + 0 }
    ' < "$work/trace") || status=125
    if [ "$status" -eq 125 ]; then
        kill -s KILL "$tracer" 2>/dev/null || :
        wait "$tracer" 2>/dev/null || :
    else
        wait "$tracer" || status=$?
    fi
    set -- $counts
    sends=$1
    timers=$2
}

# Report the row NAME as passed when ping exited with STATUS after SENDS
# successful sends, armed no timer and printed the statistics STATS, and as
# failed otherwise, with what trace_ping read either way.
verdict() {
    stats=$(grep -E 'packets transmitted' "$work/output" || :)
    if [ "$status" -eq "$2" ] && [ "$sends" -eq "$3" ] && [ "$timers" -eq 0 ] &&
        [ "$stats" = "$4" ]; then
        echo "ping-delay: $1: passed: status $status, $sends sends, $timers timer calls, $stats"
    else
        echo "ping-delay: $1: failed: status $status, $sends sends, $timers timer calls, ${stats:-no statistics}" >&2
        failed=1
    fi
}

# Retries, reception and the send count. The responder answers nothing until
# both requests are out, so the second is a retry that falls due with no
# reply, and both replies then arrive with no request left to send. -W 0
# takes away the final wait and no -w is given, so nothing ends the row but
# the second reply: a correct ping finishes however long it is held. The old
# count fault sends a third request on the first reply; a zero interval that
# stops the timer never sends the second; adaptive sends driven by SIGALRM, as
# before, arm a timer, which this row counts. It does not show that every way
# the receive loop could starve is gone.
for address in 127.0.0.1 ::1; do
    for socket in raw dgram; do
        for mode in zero adaptive; do
            case $mode in
                zero) set -- -i 0 ;;
                adaptive) set -- -A -i 0.000002 ;;
            esac
            rm -f "$work/ready"
            mkfifo "$work/ready"
            python3 -I -c "$responder" "$address" > "$work/ready" &
            answer=$!
            read line < "$work/ready" || {
                echo "ping-delay: the responder did not start" >&2
                exit 1
            }
            inject=
            trace_ping 2 "$socket" -q "$@" -c 2 -W 0 "$address"
            if [ "$status" -eq 125 ]; then
                kill -s KILL "$answer" 2>/dev/null || :
            fi
            wait "$answer" 2>/dev/null || :
            verdict "$address $socket $mode" 0 2 \
                '2 packets transmitted, 2 packets received, 0% packet loss'
        done
    done
done

# The final wait ends a silent exchange: with no reply and no -w, only the
# 1 s final wait after the one request ends ping.
for address in 127.0.0.1 ::1; do
    for socket in raw dgram; do
        inject=
        trace_ping 1 "$socket" -q -A -c 1 -W 1 "$address"
        verdict "$address $socket final wait" 1 1 \
            '1 packets transmitted, 0 packets received, 100% packet loss'
    done
done

# -w ends a silent exchange alone: -W 0 takes away the final wait, and strace
# holds the first send 1.5 s at its exit, so -w's 1 s has passed when ping
# first looks.
for address in 127.0.0.1 ::1; do
    for socket in raw dgram; do
        inject=inject=sendto:delay_exit=1500000:when=1
        trace_ping 1 "$socket" -q -i 0 -c 1 -W 0 -w 1 "$address"
        verdict "$address $socket deadline" 1 1 \
            '1 packets transmitted, 0 packets received, 100% packet loss'
    done
done
exit "$failed"
EOF
