#!/bin/sh
# Measure one inbound transfer after the receiver has announced its port.
set -eu
[ "$#" -eq 8 ] || {
    echo 'usage: machine_net_in.sh KEY ADDRESS REMOTE-PROGRAM SENDER MIB WRITE-BYTES TIMEOUT DIR' >&2
    exit 2
}
key=$1
address=$2
program=$3
sender=$4
mib=$5
chunk=$6
timeout=$7
directory=$8
account=${RACH2_NET_USER:-root}
port=27158

case "$account" in
    ''|-*|*[!A-Za-z0-9_.-]*) echo 'machine-net-in: invalid account' >&2; exit 2 ;;
esac
case "$address" in
    ''|*[!0-9.]*) echo 'machine-net-in: expected an IPv4 address' >&2; exit 2 ;;
esac
case "$program" in
    /*) ;;
    *) echo 'machine-net-in: the remote program needs an absolute path' >&2; exit 2 ;;
esac
case "$program" in
    *[!A-Za-z0-9_./-]*) echo 'machine-net-in: invalid remote path' >&2; exit 2 ;;
esac
for value in "$mib" "$chunk" "$timeout"; do
    case "$value" in
        ''|*[!0-9]*) echo 'machine-net-in: expected decimal arguments' >&2; exit 2 ;;
    esac
done
if ! { [ "$mib" -ge 1 ] && [ "$mib" -le 4096 ] &&
       [ "$chunk" -ge 1 ] && [ "$chunk" -le 65536 ] &&
       [ "$timeout" -ge 0 ] && [ "$timeout" -le 60 ]; }; then
    echo 'machine-net-in: transfer arguments out of range' >&2
    exit 2
fi
[ -r "$key" ] && [ -x "$sender" ] && [ -d "$directory" ] || {
    echo 'machine-net-in: need a readable key, executable sender and log directory' >&2
    exit 2
}
work=$(mktemp -d "$directory/net-in.XXXXXX")
known=${RACH2_NET_KNOWN:-$work/known_hosts}
receiver=

# The background PID must identify SSH, not a shell waiting for SSH.
connect()
{
    exec ssh -i "$key" -o BatchMode=yes -o ConnectTimeout=10 \
        -o UserKnownHostsFile="$known" -o StrictHostKeyChecking=accept-new \
        -o ServerAliveInterval=10 -o ServerAliveCountMax=2 \
        "$account@$address" "$1"
}

# The exit trap stops only the receiver started by this invocation.
# shellcheck disable=SC2329
cleanup()
{
    if [ -n "$receiver" ]; then
        remote_pid=$(sed -n 's/^machine-net: receiver-pid: //p' "$work/receive.log")
        case "$remote_pid" in
            ''|*[!0-9]*) ;;
            *)
                if [ "$remote_pid" -gt 1 ]; then
                    (connect "kill -TERM $remote_pid") > /dev/null 2>&1 || :
                fi
                ;;
        esac
        kill -TERM "$receiver" 2>/dev/null || :
        wait "$receiver" 2>/dev/null || :
    fi
}
trap cleanup EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

echo "machine-net-in: logs: $work"
printf 'machine-net-in: transfer: %s MiB, write %s, select-timeout %s\n' \
    "$mib" "$chunk" "$timeout"
shasum -a 256 "$sender" > "$work/sender.sha256"
cat "$work/sender.sha256"
connect "sha256sum $program && cat /proc/interrupts || exit 1
    $program receive $port $mib $timeout &
    child=\$!
    printf 'machine-net: receiver-pid: %s\n' \"\$child\"
    rc=0
    wait \"\$child\" || rc=\$?
    cat /proc/interrupts || exit 1
    exit \"\$rc\"" > "$work/receive.log" 2>&1 &
receiver=$!
ready=0
attempt=0
while [ "$attempt" -lt 15 ]; do
    if grep -q "^machine-net: listening: port $port$" "$work/receive.log"; then
        ready=1
        break
    fi
    kill -0 "$receiver" 2>/dev/null || break
    attempt=$((attempt + 1))
    sleep 1
done
if [ "$ready" -ne 1 ]; then
    cat "$work/receive.log" >&2
    echo 'machine-net-in: the receiver did not announce its port' >&2
    exit 1
fi
echo "machine-net-in: sending to port $port"
if ! "$sender" send "$address" "$port" "$mib" "$chunk" > "$work/send.log" 2>&1; then
    cat "$work/send.log" >&2
    echo 'machine-net-in: the sender failed' >&2
    exit 1
fi
rc=0
wait "$receiver" || rc=$?
receiver=
cat "$work/receive.log" "$work/send.log"
[ "$rc" -eq 0 ] || exit "$rc"
grep -q '^machine-net: result:' "$work/receive.log" || {
    echo 'machine-net-in: the receiver returned no measurement' >&2
    exit 1
}
echo 'machine-net-in: result: both ends completed'
