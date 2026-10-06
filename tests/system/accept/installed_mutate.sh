#!/bin/sh
# Prove the installed acceptance rows against staged and compiled faults, for
# the architecture ARCH names: its kernel, its bootctl and its emulator.
set -eu
repo=$(CDPATH='' cd -- "$(dirname -- "$0")/../../.." && pwd)
cd "$repo"
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
accept=tests/system/accept/metal_accept.sh
bootctl=utils/bootctl.c
cp "$accept" "$work/accept.saved"
cp "$bootctl" "$work/bootctl.saved"
child=
changed=

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

rebuild()
{
    rm -f "$tree/utils/sbin/bootctl"
    make -s -C utils "../$tree/utils/sbin/bootctl" "ARCH=$RACH2_ARCH"
}

restore()
{
    cp "$work/accept.saved" "$accept"
    cp "$work/bootctl.saved" "$bootctl"
    touch "$accept" "$bootctl"
    rebuild > "$work/restore.build.log" 2>&1
    changed=
}

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
for fault in partition readonly window slot; do
    changed=1
    python3 - "$fault" "$accept" "$bootctl" <<'PY'
from pathlib import Path
import sys

fault = sys.argv[1]
path = Path(sys.argv[3] if fault == 'slot' else sys.argv[2])
text = path.read_text()
changes = {
    'partition': ('*"/dev/disk0p3 / ext2 rw,relatime"*)', '*"/dev/disk0p"[34]" / ext2 rw,relatime"*)'),
    'readonly': ('*"/dev/disk0p3 / ext2 rw,relatime"*)', '*"/dev/disk0p3 / ext2 r"[ow]",relatime"*)'),
    'window': ('note "disk: the MBR hashes to $mbr"', 'bad "disk: the MBR hashes to $mbr"'),
    'slot': ('printf("bootctl: booted: fallback\\n");', 'printf("bootctl: booted: candidate\\n");'),
}
old, new = changes[fault]
assert text.count(old) == 1
path.write_text(text.replace(old, new))
PY
    case "$fault" in
        partition) row=wrong-partition; expected='wrong-partition: wrong acceptance status' ;;
        readonly) row='readonly'; expected='readonly: missing root rejection' ;;
        window) row=candidate; expected='candidate: wrong acceptance status' ;;
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
echo 'metal-installed-mutation: result: all four faults detected and restorations passed'
