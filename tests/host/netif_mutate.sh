#!/bin/sh
# Prove each socket regression in a private compiled copy of the real handler.
set -eu

repo=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
cd "$repo"
mkdir -p build/tests/host
work=$(mktemp -d "$repo/build/tests/host/netif-mutation.XXXXXX")
child=
cp rach2/sys/syscall_net.c "$work/syscall_net.saved"
cp "$work/syscall_net.saved" "$work/syscall_net.c"

cleanup()
{
    if [ -n "$child" ]; then
        kill -TERM "$child" 2>/dev/null || :
        wait "$child" 2>/dev/null || :
    fi
    cp "$work/syscall_net.saved" "$work/syscall_net.c"
    touch "$work/syscall_net.c"
}

trap cleanup EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

run_phase()
{
    phase=$1
    expected=$2
    detail=$3
    binary="$work/$phase"
    if ! "${CC:-cc}" -std=c99 -g -O1 -ffreestanding -DRACH2_HOSTTEST \
        -fsanitize=address,undefined -fno-sanitize-recover=all \
        -Wno-int-to-pointer-cast \
        -I "$work" -I rach2/sys -I rach2/sys/lib -I rach2/sys/proc \
        tests/host/netif_hosttest.c rach2/sys/lib/kstring.c \
        rach2/sys/lib/kctypes.c tests/host/compat/kheap_host.c \
        -o "$binary" >"$binary.compile.log" 2>&1; then
        cat "$binary.compile.log" >&2
        exit 1
    fi
    perl -e 'alarm 10; exec @ARGV or die "netif-mutation: exec: $!\n"' \
        "$binary" >"$binary.log" 2>&1 &
    child=$!
    status=0
    wait "$child" || status=$?
    child=
    cat "$binary.log"
    [ "$status" -eq "$expected" ] || exit 1
    grep -q "$detail" "$binary.log" || exit 1
    echo "netif-mutation: result: $phase status $status"
}

echo "netif-mutation: logs: $work"
run_phase fixed 0 'netif-test: pass:'
for mutation in device-index device-name packet-index packet-mac tcp-address accept; do
    python3 - "$work/syscall_net.c" "$mutation" <<'PY'
import pathlib
import sys

path = pathlib.Path(sys.argv[1])
changes = {
    "device-index": ("entry->sock->bound_device = nd->index;",
                     "entry->sock->bound_device = 1;"),
    "device-name": ("k_strncpy(name, nd->name, sizeof(name) - 1);",
                    "k_strncpy(name, NETDEV_IFNAME, sizeof(name) - 1);"),
    "packet-index": ("int ifindex = packet_ifindex(entry->sock);",
                     "int ifindex = NETDEV_IFINDEX;"),
    "packet-mac": ("struct netdev *nd = netdev_at(ifindex - 1);",
                   "struct netdev *nd = netdev_first();"),
    "tcp-address": ("entry->sock->tcb ? entry->sock->tcb->local_ip : net_ip_addr",
                    "net_ip_addr"),
    "accept": ("if (listen_sock->type != SOCK_STREAM || !listen_sock->listening) {",
               "if (0) {"),
}
old, new = changes[sys.argv[2]]
source = path.read_text()
if source.count(old) != 1:
    raise SystemExit("netif-mutation: mutation target is not unique")
path.write_text(source.replace(old, new))
PY
    touch "$work/syscall_net.c"
    case "$mutation" in
        device-index) detail='device index is two' ;;
        device-name) detail='device name is eth1' ;;
        packet-index) detail='packet interface index' ;;
        packet-mac) detail='packet interface mac' ;;
        tcp-address) detail='tcp local address' ;;
        accept) detail='invalid accept never waits' ;;
    esac
    run_phase "$mutation" 1 "netif-test: fail: $detail"
    cleanup
    run_phase "$mutation-restored" 0 'netif-test: pass:'
done
cmp "$work/syscall_net.saved" "$work/syscall_net.c"
