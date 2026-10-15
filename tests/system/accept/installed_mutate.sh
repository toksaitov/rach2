#!/bin/sh
# installed_mutate.sh [--texts]: show that installed_test.sh catches five
# faults, for the architecture ARCH names: its kernel, its bootctl and its
# emulator.
#
# Four faults are put into metal_accept.sh and one is compiled into bootctl.
# Each must fail its row with the expected message, and the row must pass
# again once the source is restored and bootctl rebuilt to the same bytes.
# --texts checks that the text each fault replaces is once in its source.
set -eu
repo=$(CDPATH='' cd -- "$(dirname -- "$0")/../../.." && pwd)
cd "$repo"

# apply_fault ARGS...: put one fault into its source; with
# RACH2_FAULT_TEXTS set, only check its text and write nothing.
apply_fault()
{
    python3 - "$1" "$2" "$3" <<'PY'
from pathlib import Path
import sys
import os
import pathlib
if os.environ.get('RACH2_FAULT_TEXTS'):
    pathlib.Path.write_text = lambda self, *args, **kwargs: 0

fault = sys.argv[1]
path = Path(sys.argv[3] if fault == 'slot' else sys.argv[2])
text = path.read_text()
changes = {
    'partition': ('*"/dev/disk0p3 / ext2 rw,relatime"*)', '*"/dev/disk0p"[34]" / ext2 rw,relatime"*)'),
    'readonly': ('*"/dev/disk0p3 / ext2 rw,relatime"*)', '*"/dev/disk0p3 / ext2 r"[ow]",relatime"*)'),
    'window': ('note "disk: the MBR hashes to $mbr"', 'bad "disk: the MBR hashes to $mbr"'),
    'gptid': ("*' rach2.root=GPTID='*|", ''),
    'slot': ('printf("bootctl: booted: fallback\\n");', 'printf("bootctl: booted: candidate\\n");'),
}
old, new = changes[fault]
assert text.count(old) == 1
path.write_text(text.replace(old, new))
PY
}
accept=tests/system/accept/metal_accept.sh
bootctl=utils/bootctl.c
faults='partition readonly window gptid slot'
if [ "${1:-}" = --texts ]; then
    for fault in $faults; do
        RACH2_FAULT_TEXTS=1 apply_fault "$fault" "$accept" "$bootctl" >/dev/null || exit 1
    done
    echo 'metal-installed-mutation: texts: every fault is once in its source'
    exit 0
fi

RACH2_ARCH=${RACH2_ARCH:-${ARCH:-i686}}
REPO=$repo
. "$repo/mk/arch.sh"
RACH2_QEMU=$ARCH_QEMU
export ARCH_BUILD RACH2_QEMU
tree=build$ARCH_SUFFIX
. "$repo/tests/system/runlock.sh"
runlock_hold "$repo" metal-installed-mutation "$repo/tests/system/accept/installed_mutate.sh" "$@"
mkdir -p build
work=$(mktemp -d "$repo/build/metal-installed-proof.XXXXXX")
cp "$accept" "$work/accept.saved"
cp "$bootctl" "$work/bootctl.saved"
child=
changed=

# Stop the process group of the running row, and wait until it is gone.
stop_child()
{
    if [ -n "$child" ]; then
        kill -TERM "-$child" 2>/dev/null || :
        wait "$child" 2>/dev/null || :
        while kill -0 "-$child" 2>/dev/null; do
            sleep 1
        done
        child=
    fi
}

# Build bootctl again from its source.
rebuild()
{
    rm -f "$tree/utils/sbin/bootctl"
    make -s -C utils "../$tree/utils/sbin/bootctl" "ARCH=$RACH2_ARCH"
}

# Put back the saved sources and rebuild bootctl from them.
restore()
{
    cp "$work/accept.saved" "$accept"
    cp "$work/bootctl.saved" "$bootctl"
    touch "$accept" "$bootctl"
    rebuild > "$work/restore.build.log" 2>&1
    changed=
}

# On exit: stop the row and restore any source a fault changed.
cleanup()
{
    stop_child
    if [ -n "$changed" ]; then
        restore
    fi
}
trap cleanup EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

# run_phase PHASE ROW EXPECTED: run installed_test.sh's ROW; EXPECTED is
# `pass`, or the text the failing row must print.
run_phase()
{
    phase=$1
    row=$2
    expected=$3
    build/tests/system/runutil group sh tests/system/accept/installed_test.sh \
        "$tree/rach2/kernel" build/tests/system/harness "$work/$phase" "$row" \
        > "$work/$phase.log" 2>&1 &
    child=$!
    status=0
    wait "$child" || status=$?
    stop_child
    cat "$work/$phase.log"
    if [ "$expected" = pass ]; then
        [ "$status" -eq 0 ] || exit 1
    else
        [ "$status" -ne 0 ] || {
            echo "metal-installed-mutation: $phase was not detected" >&2
            exit 1
        }
        grep -F "$expected" "$work/$phase.log" >/dev/null
    fi
    echo "metal-installed-mutation: result: $phase status $status"
}

echo "metal-installed-mutation: logs: $work"
make -s -j4 metal-installed-tools "ARCH=$RACH2_ARCH" > "$work/build.log" 2>&1
cat "$work/build.log"
run_phase fixed all pass
cp "$tree/utils/sbin/bootctl" "$work/fixed.bootctl"
for fault in $faults; do
    changed=1
    apply_fault "$fault" "$accept" "$bootctl"
    case "$fault" in
        partition) row=wrong-partition; expected='wrong-partition: wrong acceptance status' ;;
        readonly) row='readonly'; expected='readonly: missing root rejection' ;;
        window) row=candidate; expected='candidate: wrong acceptance status' ;;
        gptid) row=gpt; expected='gpt: wrong acceptance status' ;;
        slot)
            row=fallback; expected='fallback: wrong acceptance status'
            rebuild > "$work/slot.build.log" 2>&1
            cat "$work/slot.build.log"
            cmp -s "$work/fixed.bootctl" "$tree/utils/sbin/bootctl" && exit 1
            ;;
    esac
    run_phase "$fault" "$row" "$expected"
    restore
    cmp "$work/fixed.bootctl" "$tree/utils/sbin/bootctl"
    run_phase "$fault-restored" "$row" pass
done
echo 'metal-installed-mutation: result: all five faults detected and restorations passed'
