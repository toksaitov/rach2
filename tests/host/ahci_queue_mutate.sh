#!/bin/sh
# Prove independent AHCI completion with faults in a private driver copy.
# --texts checks the patterns without compiling; arguments select faults.
set -eu
repo=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
cd "$repo"

# apply_fault SOURCE FAULT: locate or replace one driver contract.
apply_fault()
{
    python3 - "$1" "$2" <<'PY'
from pathlib import Path
import sys

path = Path(sys.argv[1])
faults = {
    'serialized': ('        if (d->polling) {',
                   '        if (i == 0 && d->active && '
                   '(ahci_read32(ahci_port_off(d->port, AHCI_PxCI)) & 1u)) { return; }\n'
                   '        if (d->polling) {'),
    'flush-skipped': ('} else if (rc == 0 && r->write && !d->flushing) {',
                      '} else if (0 && rc == 0 && r->write && !d->flushing) {'),
}
text = path.read_text()
old, new = faults[sys.argv[2]]
if text.count(old) != 1:
    raise SystemExit('ahci-queue-mutation: source pattern is not unique: ' + sys.argv[2])
if path.parent.name != 'block':
    path.write_text(text.replace(old, new))
PY
}

faults='serialized flush-skipped'
if [ "${1:-}" = --texts ]; then
    for fault in $faults; do
        apply_fault rach2/sys/block/ahci.c "$fault"
    done
    echo 'ahci-queue-mutation: texts: every fault is once in its source'
    exit 0
fi
. "$repo/tests/system/runlock.sh"
runlock_hold "$repo" ahci-queue-mutation "$repo/tests/host/ahci_queue_mutate.sh" "$@"
work=
# cleanup: remove the private driver, executables and logs.
cleanup()
{
    [ -z "$work" ] || rm -rf "$work"
}
trap cleanup EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM
mkdir -p build/tests
work=$(mktemp -d "$repo/build/tests/ahci-queue-proof.XXXXXX")
cp rach2/sys/block/ahci.c "$work/ahci.c"
make build/tests/system/deadline

# run_phase NAME EXPECTED MESSAGE: compile and run the register model afresh.
run_phase()
{
    phase=$1
    expected=$2
    message=$3
    binary="$work/$phase"
    "${CC:-cc}" -std=c99 -D_DEFAULT_SOURCE -D_FORTIFY_SOURCE=0 -DRACH2_HOSTTEST \
        -g -O1 -ffreestanding -Wall -Wextra -Werror -Wno-type-limits \
        -fsanitize=address,undefined -fno-sanitize-recover=all \
        -I rach2/sys/block -I rach2/sys -I rach2/sys/arch/x86 \
        -I rach2/sys/arch/i686 -I rach2/sys/lib -I rach2/sys/proc \
        -I utils -I krffs -I tests/host/compat \
        "-DAHCI_SOURCE=\"$work/ahci.c\"" tests/host/ahci_hosttest.c \
        tests/host/compat/paging_host.c rach2/sys/lib/kstring.c rach2/sys/lib/kctypes.c \
        -o "$binary"
    status=0
    build/tests/system/deadline 30 "$binary" >"$binary.log" 2>&1 || status=$?
    cat "$binary.log"
    if [ "$expected" = pass ]; then
        [ "$status" -eq 0 ] || exit 1
    else
        [ "$status" -gt 0 ] && [ "$status" -lt 100 ] || exit 1
        grep -Fq "$message" "$binary.log" || exit 1
        if grep -Eq 'AddressSanitizer|runtime error:' "$binary.log"; then
            exit 1
        fi
    fi
    echo "ahci-queue-mutation: result: $phase status $status"
}

run_phase baseline pass ''
[ "$#" -eq 0 ] || faults="$*"
for fault in $faults; do
    apply_fault "$work/ahci.c" "$fault"
    case $fault in
        serialized) message='the independent port completes while port zero is held' ;;
        flush-skipped) message='a write waits for its held flush' ;;
    esac
    run_phase "$fault-faulty" fail "$message"
    cp rach2/sys/block/ahci.c "$work/ahci.c"
    touch "$work/ahci.c"
    run_phase "$fault-restored" pass ''
done
echo 'ahci-queue-mutation: result: pass: faults failed; restored tests passed'
