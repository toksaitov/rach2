#!/bin/sh
# Prove netdev_hosttest.c and netconf_hosttest.c, the network configuration,
# with private mutations of rach2/sys/network/driver/netdev.c and netconf.c.
set -eu

repo=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
cd "$repo"

# apply_fault NETDEV NETCONF FAULT: put FAULT into NETDEV or NETCONF, copies
# of netdev.c and netconf.c; with RACH2_FAULT_TEXTS set, only check its text
# and write nothing.
apply_fault()
{
    python3 - "$1" "$2" "$3" <<'PY'
import pathlib
import sys
import os
if os.environ.get('RACH2_FAULT_TEXTS'):
    pathlib.Path.write_text = lambda self, *args, **kwargs: 0

fault = sys.argv[3]
changes = {
    "refusal": ("if (!refused && drivers[i]->error)", "if (drivers[i]->error)"),
    "tcp": ("        tcp_local_addr_changed();",
            "        if (is_machine(nd)) {\n"
            "            tcp_local_addr_changed();\n        }"),
    "primary": ("""        if (nd == netdev_first()) {
            net_ip_addr = ip;
            net_ip_mask = mask;
            net_ip_gateway = gw;
        }
""", ""),
    "oversized": ("""            klog(KLOG_WARN, "net", "ifaddr: value too long");
            continue;""", "            break;"),
}
path = pathlib.Path(sys.argv[1] if fault == "refusal" else sys.argv[2])
source = path.read_text()
old, new = changes[fault]
if source.count(old) != 1:
    raise SystemExit("netconfig-mutation: source pattern is not unique")
path.write_text(source.replace(old, new))
PY
}

faults='refusal tcp primary oversized'
if [ "${1:-}" = --texts ]; then
    for fault in $faults; do
        RACH2_FAULT_TEXTS=1 apply_fault rach2/sys/network/driver/netdev.c rach2/sys/network/netconf.c "$fault" >/dev/null || exit 1
    done
    echo 'netconfig-mutation: texts: every fault is once in its source'
    exit 0
fi

. "$repo/tests/system/runlock.sh"
runlock_hold "$repo" netconfig-mutation "$repo/tests/host/netconfig_mutate.sh" "$@"

mkdir -p build/tests
work=$(mktemp -d "$repo/build/tests/network-proof.XXXXXX")
child=
mkdir -p "$work/rach2" "$work/tests/host"
cp -R rach2/sys "$work/rach2/sys"
cp tests/host/netdev_hosttest.c tests/host/netconf_hosttest.c "$work/tests/host/"
netdev="$work/rach2/sys/network/driver/netdev.c"
netconf="$work/rach2/sys/network/netconf.c"
cp "$netdev" "$work/netdev.saved"
cp "$netconf" "$work/netconf.saved"

# cleanup: stop the running test and put the saved netdev.c and netconf.c back;
# the loop calls it after each fault, and it is the exit trap.
cleanup()
{
    if [ -n "$child" ]; then
        kill -TERM "$child" 2>/dev/null || :
        wait "$child" 2>/dev/null || :
        child=
    fi
    cp "$work/netdev.saved" "$netdev"
    cp "$work/netconf.saved" "$netconf"
    touch "$netdev" "$netconf"
}
trap cleanup EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

# run_phase TEST PHASE pass|fail MESSAGE: compile TEST_hosttest.c over the
# copies as $work/TEST-PHASE and run it for at most 10 seconds. A pass must
# exit 0, a fail with a status from 1 to 63; either must print MESSAGE. Exit
# if not.
run_phase()
{
    test=$1
    phase=$2
    expected=$3
    message=$4
    binary="$work/$test-$phase"
    set --
    if [ "$test" = netconf ]; then
        set -- "$netconf" "$work/rach2/sys/cmdline.c" \
            "$work/rach2/sys/lib/kstring.c" "$work/rach2/sys/lib/kctypes.c"
    fi
    if ! "${CC:-cc}" -std=c99 -D_DEFAULT_SOURCE -D_FORTIFY_SOURCE=0 \
        -DRACH2_HOSTTEST -g -O1 -Wno-empty-body -ffreestanding \
        -fsanitize=address,undefined -fno-sanitize-recover=all \
        -I "$work/rach2/sys" -I "$work/rach2/sys/lib" -I "$work/rach2/sys/proc" \
        "$work/tests/host/${test}_hosttest.c" "$@" \
        -o "$binary" >"$binary.compile.log" 2>&1; then
        cat "$binary.compile.log" >&2
        exit 1
    fi
    cksum "$netdev" "$netconf" "$binary" >"$binary.cksum"
    perl -e 'alarm 10; exec @ARGV or die "netconfig-mutation: exec: $!\n"' \
        "$binary" >"$binary.log" 2>&1 &
    child=$!
    status=0
    wait "$child" || status=$?
    child=
    cat "$binary.log"
    if [ "$expected" = pass ]; then
        [ "$status" -eq 0 ] || exit 1
    else
        [ "$status" -gt 0 ] && [ "$status" -lt 64 ] || exit 1
    fi
    grep -Fq "$message" "$binary.log" || exit 1
    echo "netconfig-mutation: result: $test-$phase status $status"
}

echo "netconfig-mutation: logs: $work"
run_phase netdev fixed pass 'netdev-hosttest: result: pass'
run_phase netconf fixed pass 'netconf-hosttest: result: pass'
for fault in $faults; do
    apply_fault "$netdev" "$netconf" "$fault"
    test=netconf
    case $fault in
        refusal) test=netdev; message='injected refusal reason is retained' ;;
        tcp) message='secondary address change notifies TCP under the network lock' ;;
        primary) message='MAC configuration updates all primary globals' ;;
        oversized) message='valid MAC option after invalid value is applied' ;;
    esac
    run_phase "$test" "$fault-mutated" fail "$message"
    cleanup
    run_phase "$test" "$fault-restored" pass "$test-hosttest: result: pass"
    cmp "$work/netdev.saved" "$netdev"
    cmp "$work/netconf.saved" "$netconf"
done
