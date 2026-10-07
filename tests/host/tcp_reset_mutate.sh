#!/bin/sh
# Compile tcp_reset_hosttest.c against faults in a private copy of tcp.c: a
# reset inside the window taken as if at the next sequence number, a reset
# outside the window answered, a reset taken while opening without the ACK
# flag, one taken while opening that acknowledges another number, a handshake
# completed after its listener closed left open, an initial sequence number
# without the keyed hash, and a key drawn at every open. Each fault must fail
# its row with exit status 1, and the restored source must pass again.
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

opening = ("return ((flags & TCP_ACK) && ack_num == tcb->snd_nxt) ? TCP_RST_ACCEPT "
           ": TCP_RST_IGNORE;")
changes = {
    "in-window": ("        return TCP_RST_CHALLENGE;\n", "        return TCP_RST_ACCEPT;\n"),
    "out-of-window": ("    return TCP_RST_IGNORE;\n}\n", "    return TCP_RST_CHALLENGE;\n}\n"),
    "opening-flag": (opening, "return ack_num == tcb->snd_nxt ? TCP_RST_ACCEPT : TCP_RST_IGNORE;"),
    "opening-number": (opening, "return (flags & TCP_ACK) ? TCP_RST_ACCEPT : TCP_RST_IGNORE;"),
    "listener-gone": ("            tcp_free_half_open(t);\n", "            (void)t;\n"),
    "unkeyed": ("return kernel_ticks * TCP_ISN_PER_TICK + tcp_isn_hash(tcb);",
                "return kernel_ticks * TCP_ISN_PER_TICK + tcb->local_port;"),
    "rekeyed": ("    isn_keyed = 1;\n", ""),
}
old, new = changes[sys.argv[2]]
path = pathlib.Path(sys.argv[1])
source = path.read_text()
if source.count(old) != 1:
    raise SystemExit("tcp-reset-mutation: source pattern is not unique")
path.write_text(source.replace(old, new))
PY
}

faults='in-window out-of-window opening-flag opening-number listener-gone
    unkeyed rekeyed'
if [ "${1:-}" = --texts ]; then
    for fault in $faults; do
        RACH2_FAULT_TEXTS=1 apply_fault rach2/sys/network/tcp/tcp.c "$fault" >/dev/null || exit 1
    done
    echo 'tcp-reset-mutation: texts: every fault is once in its source'
    exit 0
fi

mkdir -p build/tests
work=$(mktemp -d "$repo/build/tests/tcp-reset-proof.XXXXXX")
child=
mkdir -p "$work/rach2" "$work/tests/host/compat"
cp -R rach2/sys "$work/rach2/sys"
cp tests/host/tcp_reset_hosttest.c "$work/tests/host/"
cp tests/host/compat/kheap_host.c tests/host/compat/pressure_host.c "$work/tests/host/compat/"
source="$work/rach2/sys/network/tcp/tcp.c"
cp "$source" "$work/tcp.saved"

cleanup()
{
    if [ -n "$child" ]; then
        kill -TERM "$child" 2>/dev/null || :
        wait "$child" 2>/dev/null || :
        child=
    fi
    cp "$work/tcp.saved" "$source"
    touch "$source"
}
trap cleanup EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

run_phase()
{
    phase=$1
    expected=$2
    message=$3
    binary="$work/$phase"
    if ! "${CC:-cc}" -std=c99 -D_DEFAULT_SOURCE -D_FORTIFY_SOURCE=0 -DRACH2_HOSTTEST \
        -fsanitize=address,undefined -fno-sanitize-recover=all \
        -g -O1 -Wno-empty-body -ffreestanding \
        -I "$work/rach2/sys" -I "$work/rach2/sys/lib" -I "$work/rach2/sys/proc" \
        -I utils -I krffs -I "$work/tests/host/compat" \
        "$work/tests/host/tcp_reset_hosttest.c" "$work/rach2/sys/lib/kstring.c" \
        "$work/rach2/sys/lib/kctypes.c" "$work/rach2/sys/network/tcp/rcvwnd.c" \
        "$work/tests/host/compat/kheap_host.c" "$work/tests/host/compat/pressure_host.c" \
        -o "$binary" >"$binary.compile.log" 2>&1; then
        cat "$binary.compile.log" >&2
        exit 1
    fi
    cksum "$source" "$work/tests/host/tcp_reset_hosttest.c" "$binary" >"$binary.cksum"
    perl -e 'alarm 10; exec @ARGV or die "tcp-reset-mutation: exec: $!\n"' \
        "$binary" >"$binary.log" 2>&1 &
    child=$!
    status=0
    wait "$child" || status=$?
    child=
    cat "$binary.log"
    if [ "$status" -ne "$expected" ] || ! grep -Fq "$message" "$binary.log"; then
        echo "tcp-reset-mutation: unexpected result: $phase status $status" >&2
        exit 1
    fi
    echo "tcp-reset-mutation: result: $phase status $status"
}

echo "tcp-reset-mutation: logs: $work"
run_phase fixed 0 'tcp_reset_hosttest: PASS'
for fault in $faults; do
    apply_fault "$source" "$fault"
    case $fault in
        in-window)
            message='a reset inside the window but not at the next sequence number closes nothing' ;;
        out-of-window) message='a reset outside the window is dropped without an answer' ;;
        opening-flag) message='a reset with the acknowledgement of the SYN but no ACK flag leaves the open' ;;
        opening-number) message='a reset that acknowledges another number leaves the open' ;;
        listener-gone)
            message='closing the listener frees the handshake in progress and its socket' ;;
        unkeyed)
            message='an active open starts at the clock plus the keyed hash of its addresses and ports' ;;
        rekeyed) message='later opens and listeners draw no key' ;;
    esac
    run_phase "$fault-mutated" 1 "$message"
    cleanup
    run_phase "$fault-restored" 0 'tcp_reset_hosttest: PASS'
    cmp "$work/tcp.saved" "$source"
done
echo 'tcp-reset-mutation: result: pass: each fault failed its row and the restored source passed'
