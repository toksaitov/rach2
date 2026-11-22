#!/bin/sh
# Check BusyBox adaptive ping with replies delayed on a private Linux loopback.
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
EOF
