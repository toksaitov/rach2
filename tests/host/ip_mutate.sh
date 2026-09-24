#!/bin/sh
# Compile the IP transmit rows of packet_hosttest.c against faults in a
# private copy of ip.c: a log line for every datagram the full loopback ring
# refuses, a refusal reported as sent, a refusal left uncounted, and a subnet
# broadcast sent by the first card whatever its subnet. Each fault must fail
# its row with exit status 1, and the restored source must pass again.
set -eu

repo=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
cd "$repo"
mkdir -p build/tests
work=$(mktemp -d "$repo/build/tests/ip-proof.XXXXXX")
child=
mkdir -p "$work/rach2" "$work/tests/host"
cp -R rach2/sys "$work/rach2/sys"
cp tests/host/packet_hosttest.c "$work/tests/host/"
ip="$work/rach2/sys/network/ip/ip.c"
cp "$ip" "$work/ip.saved"

cleanup()
{
    if [ -n "$child" ]; then
        kill -TERM "$child" 2>/dev/null || :
        wait "$child" 2>/dev/null || :
        child=
    fi
    cp "$work/ip.saved" "$ip"
    touch "$ip"
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
        -fsanitize=address,undefined -fno-sanitize-recover=all -g -O1 \
        -Wno-empty-body -ffreestanding \
        -I "$work/rach2/sys" -I "$work/rach2/sys/lib" -I "$work/rach2/sys/proc" \
        -I utils -I krffs -I tests/host/compat \
        "$work/tests/host/packet_hosttest.c" "$work/rach2/sys/lib/kstring.c" \
        "$work/rach2/sys/lib/kctypes.c" -o "$binary" >"$binary.compile.log" 2>&1; then
        cat "$binary.compile.log" >&2
        exit 1
    fi
    cksum "$ip" "$work/tests/host/packet_hosttest.c" "$binary" >"$binary.cksum"
    perl -e 'alarm 10; exec @ARGV or die "ip-mutation: exec: $!\n"' \
        "$binary" >"$binary.log" 2>&1 &
    child=$!
    status=0
    wait "$child" || status=$?
    child=
    cat "$binary.log"
    if [ "$status" -ne "$expected" ] || ! grep -Fq "$message" "$binary.log"; then
        echo "ip-mutation: unexpected result: $phase status $status" >&2
        exit 1
    fi
    echo "ip-mutation: result: $phase status $status"
}

echo "ip-mutation: logs: $work"
run_phase fixed 0 'packet_hosttest: PASS'
for fault in log-per-drop silent-drop uncounted first-card limited-scan; do
    python3 - "$ip" "$fault" <<'PY'
import pathlib
import sys

changes = {
    "log-per-drop": ("            if (!loop_drop_logged) {\n"
                     "                loop_drop_logged = 1;\n",
                     "            {\n"
                     "                loop_drop_logged = 1;\n"),
    "silent-drop": ("further drops are counted\");\n            }\n            return -ENOBUFS;",
                    "further drops are counted\");\n            }\n            return 0;"),
    "uncounted": ("            loop_dropped++;\n", ""),
    "first-card": ("out = ip_broadcast_iface(dst_ip);", "out = netdev_first();"),
    "limited-scan": ("    if (dst == 0xFFFFFFFFu) {\n"
                     "        return netdev_first(); /* a card masked /1 or /0 would claim it */\n"
                     "    }\n", ""),
}
old, new = changes[sys.argv[2]]
path = pathlib.Path(sys.argv[1])
source = path.read_text()
if source.count(old) != 1:
    raise SystemExit("ip-mutation: source pattern is not unique")
path.write_text(source.replace(old, new))
PY
    case $fault in
        log-per-drop) message='one line records the refusals, not one a datagram' ;;
        silent-drop) message='a datagram past a full loopback ring is refused with -ENOBUFS' ;;
        uncounted) message='each refused datagram is counted' ;;
        first-card) message="the second card's subnet broadcast leaves by that card" ;;
        limited-scan) message='the limited broadcast leaves by the first card beside a card masked /1' ;;
    esac
    run_phase "$fault-mutated" 1 "$message"
    cleanup
    run_phase "$fault-restored" 0 'packet_hosttest: PASS'
    cmp "$work/ip.saved" "$ip"
done
echo 'ip-mutation: result: pass: each fault failed its row and the restored source passed'
