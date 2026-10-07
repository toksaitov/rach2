#!/bin/sh
# Compile the USB contract tests with fixed, faulty and restored sources.
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

root = pathlib.Path(sys.argv[1])
fault = sys.argv[2]
changes = {
    "doorbells": ("xhci.c", [
        ("hc->caps.dboff > XHCI_MAP_SIZE || ", ""),
        ("doorbells_bytes > XHCI_MAP_SIZE - hc->caps.dboff",
         "hc->caps.dboff + doorbells_bytes > XHCI_MAP_SIZE"),
    ]),
    "runtime": ("xhci.c", [
        ("hc->caps.rtsoff > XHCI_MAP_SIZE ||", ""),
        ("runtime_bytes > XHCI_MAP_SIZE - hc->caps.rtsoff",
         "hc->caps.rtsoff + runtime_bytes > XHCI_MAP_SIZE"),
    ]),
    "interrupters": ("xhci.c", [("hc->caps.max_intrs == 0", "0")]),
    "timeout": ("core.c", [
        ("d->abandoned = !xhci_command_free(d->hc);", "d->abandoned = 0;"),
    ]),
    "interfaces": ("core.c", [
        ("            k_memset(out, 0, sizeof(*out));\n", ""),
    ]),
    "context": ("xhci.c", [
        ("slot_words(s, slot, last_ctx);", "slot_words(s, slot, dci);"),
    ]),
    "shutdown": ("xhci.c", [
        ('klog(KLOG_WARN, "usb", "stop: the controller would not halt");\n        return;',
         'klog(KLOG_WARN, "usb", "stop: the controller would not halt");'),
    ]),
    "abort": ("xhci.c", [
        ("    hc->command_stop_seen = 0;\n    hc->command_aborting = 1;",
         "    hc->command_running = 0;\n    hc->command_stop_seen = 0;\n    hc->command_aborting = 1;"),
    ]),
    "stop": ("xhci.c", [
        ("    hc->command_stop_seen = 0;\n    hc->command_aborting = 1;",
         "    hc->command_stop_seen = 1;\n    hc->command_aborting = 1;"),
    ]),
}
name, pairs = changes[fault]
path = root / name
source = path.read_text()
for old, new in pairs:
    if source.count(old) != 1:
        raise SystemExit("usb-mutation: source pattern is not unique: " + fault)
    source = source.replace(old, new)
path.write_text(source)
PY
}

faults='doorbells runtime interrupters timeout interfaces context shutdown
    abort stop'
if [ "${1:-}" = --texts ]; then
    for fault in $faults; do
        RACH2_FAULT_TEXTS=1 apply_fault rach2/sys/usb "$fault" >/dev/null || exit 1
    done
    echo 'usb-mutation: texts: every fault is once in its source'
    exit 0
fi

mkdir -p build/tests
work=$(mktemp -d "$repo/build/tests/usb-contract-proof.XXXXXX")
child=
mkdir -p "$work/rach2" "$work/tests/host"
cp -R rach2/sys "$work/rach2/sys"
cp tests/host/usb_hosttest.c tests/host/xhci_hosttest.c "$work/tests/host/"
cp "$work/rach2/sys/usb/core.c" "$work/core.saved"
cp "$work/rach2/sys/usb/xhci.c" "$work/xhci.saved"

cleanup()
{
    if [ -n "$child" ]; then
        kill -TERM "$child" 2>/dev/null || :
        wait "$child" 2>/dev/null || :
        child=
    fi
    cp "$work/core.saved" "$work/rach2/sys/usb/core.c"
    cp "$work/xhci.saved" "$work/rach2/sys/usb/xhci.c"
    touch "$work/rach2/sys/usb/core.c" "$work/rach2/sys/usb/xhci.c"
}
trap cleanup EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

run_phase()
{
    phase=$1
    test_name=$2
    expected=$3
    message=$4
    binary="$work/$phase-$test_name"
    if ! "${CC:-cc}" -std=c99 -D_DEFAULT_SOURCE -D_FORTIFY_SOURCE=0 \
        -DRACH2_HOSTTEST -g -O1 -Wno-empty-body -ffreestanding \
        -fsanitize=address,undefined -fno-sanitize-recover=all \
        -I "$work/rach2/sys" -I "$work/rach2/sys/lib" -I "$work/rach2/sys/proc" \
        -I utils -I krffs -I tests/host/compat \
        "$work/tests/host/${test_name}_hosttest.c" -o "$binary" \
        >"$binary.compile.log" 2>&1; then
        cat "$binary.compile.log" >&2
        exit 1
    fi
    cksum "$work/rach2/sys/usb/core.c" "$work/rach2/sys/usb/xhci.c" \
        "$work/tests/host/${test_name}_hosttest.c" "$binary" >"$binary.cksum"
    perl -e 'alarm 30; exec @ARGV or die "usb-mutation: exec: $!\n"' \
        "$binary" >"$binary.log" 2>&1 &
    child=$!
    status=0
    wait "$child" || status=$?
    child=
    cat "$binary.log"
    [ "$status" -eq "$expected" ] && grep -Fq "$message" "$binary.log" || exit 1
    echo "usb-mutation: result: $phase $test_name status $status"
}

echo "usb-mutation: logs: $work"
run_phase fixed usb 0 'usb_hosttest: OK'
run_phase fixed xhci 0 'xhci_hosttest: OK'
for fault in $faults; do
    apply_fault "$work/rach2/sys/usb" "$fault"
    test_name=xhci
    case $fault in
        doorbells) message='wrapping doorbell range is refused' ;;
        runtime) message='wrapping runtime range is refused' ;;
        interrupters) message='controller without an interrupter is refused' ;;
        timeout) test_name=usb; message='slot timeout retains the device and command turn' ;;
        interfaces) test_name=usb; message='split storage interfaces are incomplete' ;;
        context) message='context entries retains highest endpoint index' ;;
        shutdown) message='halt timeout does not report success' ;;
        abort) message='late enable slot completion survives abort' ;;
        stop) message='cleared crr still waits for stop event' ;;
    esac
    run_phase "$fault-mutated" "$test_name" 1 "$message"
    cleanup
    run_phase "$fault-restored" "$test_name" 0 "${test_name}_hosttest: OK"
    cmp "$work/core.saved" "$work/rach2/sys/usb/core.c"
    cmp "$work/xhci.saved" "$work/rach2/sys/usb/xhci.c"
done
