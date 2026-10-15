#!/bin/sh
# Prove arp_hosttest.c and packet_hosttest.c with private mutations of
# rach2/sys/network/arp/arp.c and ip/ip.c.
set -eu

repo=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
cd "$repo"

# apply_fault ARP IP FAULT: put FAULT into ARP or IP, copies of arp.c and
# ip.c; with RACH2_FAULT_TEXTS set, only check its text and write nothing.
apply_fault()
{
    python3 - "$1" "$2" "$3" <<'PY'
import pathlib
import sys
import os
if os.environ.get('RACH2_FAULT_TEXTS'):
    pathlib.Path.write_text = lambda self, *args, **kwargs: 0

changes = {
    "hardware-length": (sys.argv[1], " || data[4] != 6", ""),
    "protocol-length": (sys.argv[1], " || data[5] != 4", ""),
    "network-interface": (sys.argv[1], "ip == (nd->ip & nd->mask)",
                          "ip == (net_ip_addr & net_ip_mask)"),
    "broadcast-interface": (sys.argv[1], "ip == (nd->ip | ~nd->mask)",
                            "netconf_is_broadcast(ip)"),
    "ip-mtu": (sys.argv[2], "if (plen > 1500 - IP_HDR_SIZE)", "if (plen > 1500)"),
}
name, old, new = changes[sys.argv[3]]
path = pathlib.Path(name)
source = path.read_text()
if source.count(old) != 1:
    raise SystemExit("network-mutation: source pattern is not unique")
path.write_text(source.replace(old, new))
PY
}

faults='hardware-length protocol-length network-interface broadcast-interface
    ip-mtu'
if [ "${1:-}" = --texts ]; then
    for fault in $faults; do
        RACH2_FAULT_TEXTS=1 apply_fault rach2/sys/network/arp/arp.c rach2/sys/network/ip/ip.c "$fault" >/dev/null || exit 1
    done
    echo 'network-mutation: texts: every fault is once in its source'
    exit 0
fi

. "$repo/tests/system/runlock.sh"
runlock_hold "$repo" network-mutation "$repo/tests/host/network_mutate.sh" "$@"

mkdir -p build/tests
work=$(mktemp -d "$repo/build/tests/network-proof.XXXXXX")
child=
mkdir -p "$work/rach2" "$work/tests/host"
cp -R rach2/sys "$work/rach2/sys"
cp tests/host/arp_hosttest.c tests/host/packet_hosttest.c "$work/tests/host/"
arp="$work/rach2/sys/network/arp/arp.c"
ip="$work/rach2/sys/network/ip/ip.c"
cp "$arp" "$work/arp.saved"
cp "$ip" "$work/ip.saved"

# cleanup: stop the running test and put the saved arp.c and ip.c back; the
# loop calls it after each fault, and it is the exit trap.
cleanup()
{
    if [ -n "$child" ]; then
        kill -TERM "$child" 2>/dev/null || :
        wait "$child" 2>/dev/null || :
        child=
    fi
    cp "$work/arp.saved" "$arp"
    cp "$work/ip.saved" "$ip"
    touch "$arp" "$ip"
}
trap cleanup EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

# run_phase PHASE TEST STATUS MESSAGE: compile TEST_hosttest.c over the copies
# as $work/PHASE, run it for at most 10 seconds, and exit unless it ends with
# STATUS and its log contains MESSAGE.
run_phase()
{
    phase=$1
    test=$2
    expected=$3
    message=$4
    binary="$work/$phase"
    if ! "${CC:-cc}" -std=c99 -D_DEFAULT_SOURCE -D_FORTIFY_SOURCE=0 -DRACH2_HOSTTEST \
        -fsanitize=address,undefined -fno-sanitize-recover=all -g -O1 \
        -Wno-empty-body -ffreestanding \
        -I "$work/rach2/sys" -I "$work/rach2/sys/lib" -I "$work/rach2/sys/proc" \
        -I utils -I krffs -I tests/host/compat \
        "$work/tests/host/${test}_hosttest.c" "$work/rach2/sys/lib/kstring.c" \
        "$work/rach2/sys/lib/kctypes.c" -o "$binary" >"$binary.compile.log" 2>&1; then
        cat "$binary.compile.log" >&2
        exit 1
    fi
    cksum "$arp" "$ip" "$work/tests/host/${test}_hosttest.c" "$binary" >"$binary.cksum"
    # A sanitizer's report ends the test with status 1; macOS would abort it.
    ASAN_OPTIONS=abort_on_error=0 UBSAN_OPTIONS=abort_on_error=0 \
        perl -e 'alarm 10; exec @ARGV or die "network-mutation: exec: $!\n"' \
        "$binary" >"$binary.log" 2>&1 &
    child=$!
    status=0
    wait "$child" || status=$?
    child=
    cat "$binary.log"
    [ "$status" -eq "$expected" ] && grep -Fq "$message" "$binary.log" || exit 1
    echo "network-mutation: result: $phase status $status"
}

echo "network-mutation: logs: $work"
run_phase arp-fixed arp 0 'arp_hosttest: OK'
run_phase packet-fixed packet 0 'packet_hosttest: PASS'
for fault in $faults; do
    apply_fault "$arp" "$ip" "$fault"
    test=arp
    pass='arp_hosttest: OK'
    case $fault in
        hardware-length|protocol-length) message='invalid ARP layout changed a mapping' ;;
        network-interface) message='second interface network address was cached' ;;
        broadcast-interface) message='another interface broadcast address was rejected' ;;
        ip-mtu)
            test=packet
            pass='packet_hosttest: PASS'
            message='oversize IP payload returns -EMSGSIZE on every path'
            ;;
    esac
    run_phase "$fault-mutated" "$test" 1 "$message"
    cleanup
    run_phase "$fault-restored" "$test" 0 "$pass"
    cmp "$work/arp.saved" "$arp"
    cmp "$work/ip.saved" "$ip"
done
