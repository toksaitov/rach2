#!/bin/sh
# Prove each socket regression in a private compiled copy of the real handler.
set -eu

repo=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
cd "$repo"

# apply_fault ARGS...: put one fault into its source; with
# RACH2_FAULT_TEXTS set, only check its text and write nothing.
apply_fault()
{
    python3 - "$1" "$2" <<'PY'
import pathlib
import sys
import os
if os.environ.get('RACH2_FAULT_TEXTS'):
    pathlib.Path.write_text = lambda self, *args, **kwargs: 0

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
    "tcp-address": ("        local_ip = entry->sock->tcb->local_ip;\n",
                    "        local_ip = net_ip_addr;\n"),
    "accept": ("if (listen_sock->type != SOCK_STREAM || !listen_sock->listening) {",
               "if (0) {"),
    "socket-check": ("    if (flags & ~(uint32_t)(SOCK_CLOEXEC | SOCK_NONBLOCK)) {\n"
                     "        return -EINVAL;\n    }\n    fd = do_socket(",
                     "    fd = do_socket("),
    "socket-flags": ("        sock_fd_flags(&current_proc->fds->entries[fd], flags);\n", ""),
    "accept-check": ("    if (flags & ~(uint32_t)(SOCK_CLOEXEC | SOCK_NONBLOCK)) {\n"
                     "        return -EINVAL;\n    }\n    entry = fd_get(",
                     "    entry = fd_get("),
    "accept-flags": ("    sock_fd_flags(new_entry, flags);\n", ""),
    "accept-enomem": ("    if (new_fd < 0) {\n        return new_fd;",
                      "    if (new_fd < 0) {\n        return -EMFILE;"),
    "recvfrom-length": ("    want_addr = sa_ptr != 0;",
                        "    want_addr = sa_ptr != 0 && salen_ptr != 0;"),
    "recvfrom-sender": ("        if (!salen_ptr || !user_buffer_ok(salen_ptr, sizeof(cap), 1) ||",
                        "        if (salen_ptr || !user_buffer_ok(salen_ptr, sizeof(cap), 1) ||"),
    "accept-reserve": ("    conn_sock = listen_sock->accept_queue[listen_sock->accept_head];\n",
                       "    conn_sock = listen_sock->accept_queue[listen_sock->accept_head];\n"
                       "    current_proc->fds->entries[new_fd].type = FD_TYPE_SOCKET;\n"),
}
old, new = changes[sys.argv[2]]
source = path.read_text()
if source.count(old) != 1:
    raise SystemExit("netif-mutation: mutation target is not unique")
path.write_text(source.replace(old, new))
PY
}

faults='device-index device-name packet-index packet-mac tcp-address accept
    socket-check socket-flags accept-check accept-flags accept-enomem
    recvfrom-length recvfrom-sender accept-reserve'
if [ "${1:-}" = --texts ]; then
    for mutation in $faults; do
        RACH2_FAULT_TEXTS=1 apply_fault rach2/sys/syscall_net.c "$mutation" >/dev/null || exit 1
    done
    echo 'netif-mutation: texts: every fault is once in its source'
    exit 0
fi

. "$repo/tests/system/runlock.sh"
runlock_hold "$repo" netif-mutation "$repo/tests/host/netif_mutate.sh" "$@"

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
for mutation in $faults; do
    apply_fault "$work/syscall_net.c" "$mutation"
    touch "$work/syscall_net.c"
    case "$mutation" in
        device-index) detail='device index is two' ;;
        device-name) detail='device name is eth1' ;;
        packet-index) detail='packet interface index' ;;
        packet-mac) detail='packet interface mac' ;;
        tcp-address) detail='tcp local address' ;;
        accept) detail='invalid accept never waits' ;;
        socket-check) detail='a type bit other than SOCK_CLOEXEC and SOCK_NONBLOCK is refused' ;;
        socket-flags) detail='SOCK_CLOEXEC and SOCK_NONBLOCK reach the descriptor' ;;
        accept-check) detail='accept4 refuses a flag other than SOCK_CLOEXEC and SOCK_NONBLOCK' ;;
        accept-flags) detail='a connection accepted with both flags has both' ;;
        accept-enomem) detail='an accept the heap has no room for is ENOMEM' ;;
        recvfrom-length) detail='a recvfrom with an address and no length is EFAULT' ;;
        recvfrom-sender) detail='a recvfrom with a length pointer takes the datagram' ;;
        accept-reserve) detail="another thread's close of the number accept reserved finds nothing" ;;
    esac
    run_phase "$mutation" 1 "netif-test: fail: $detail"
    cleanup
    run_phase "$mutation-restored" 0 'netif-test: pass:'
done
cmp "$work/syscall_net.saved" "$work/syscall_net.c"
