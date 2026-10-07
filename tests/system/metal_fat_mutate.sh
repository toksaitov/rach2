#!/bin/sh
# Prove the FAT acceptance row with corrupt disk bytes and a stale cache, on the
# kernels and suite of the architecture ARCH names.
set -eu

repo=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
cd "$repo"

# apply_fault ARGS...: put one fault into its source; with
# RACH2_FAULT_TEXTS set, only check its text and write nothing.
apply_fault()
{
    python3 - "$1" "$2" <<'PY'
from pathlib import Path
import sys
import os
import pathlib
if os.environ.get('RACH2_FAULT_TEXTS'):
    pathlib.Path.write_text = lambda self, *args, **kwargs: 0

path = Path(sys.argv[1])
text = path.read_text()
start = text.index('int bcache_invalidate(block_device_t *dev)')
old = '''        if (b->dev == dev) {
            make_empty(b);
        }'''
new = '''        if (b->dev == dev) {
            uint32_t at, match;
            int witness = 0;

            for (at = 0; at + 15 <= b->bytes; at += BLOCK_SECTOR_SIZE) {
                for (match = 0; match < 15 &&
                     b->data[at + match] == (uint8_t)"fat acceptance "[match]; match++) {
                }
                if (match == 15) {
                    b->data[at] = 'x';
                    if (block_request(dev, 1, b->blk * BCACHE_BLOCK_SECTORS,
                                      b->bytes / BLOCK_SECTOR_SIZE, b->data) != 0) {
                        return -EIO;
                    }
                    b->data[at] = 'f';
                    witness = 1;
                    klog(KLOG_WARN, "bcache", "mutation: FAT witness changed");
                }
            }
            if (!witness || DROP_WITNESS) {
                make_empty(b);
            }
        }'''
new = new.replace('DROP_WITNESS', '1' if sys.argv[2] == 'disk' else '0')
assert text[start:].count(old) == 1
path.write_text(text[:start] + text[start:].replace(old, new))
PY
}

faults='disk cached'
if [ "${1:-}" = --texts ]; then
    for fault in $faults; do
        RACH2_FAULT_TEXTS=1 apply_fault rach2/sys/fs/bcache.c "$fault" >/dev/null || exit 1
    done
    echo 'metal-fat-mutation: texts: every fault is once in its source'
    exit 0
fi

RACH2_ARCH=${RACH2_ARCH:-${ARCH:-i686}}
REPO=$repo
. "$repo/mk/arch.sh"
RACH2_QEMU=$ARCH_QEMU
export RACH2_QEMU ARCH_BUILD
tree=build$ARCH_SUFFIX
. "$repo/tests/system/runlock.sh"
runlock_hold "$repo" metal-fat-mutation "$repo/tests/system/metal_fat_mutate.sh" "$@"
mkdir -p build
work=$(mktemp -d "$repo/build/metal-fat-proof.XXXXXX")
source=rach2/sys/fs/bcache.c
cp "$source" "$work/source.saved"
child=
recorder=
changed=

stop_child()
{
    if [ -n "$child" ]; then
        kill -TERM "-$child" 2>/dev/null || :
        kill -TERM "$child" 2>/dev/null || :
        wait "$child" 2>/dev/null || :
        while kill -0 "-$child" 2>/dev/null; do
            sleep 1
        done
        child=
    fi
    if [ -n "$recorder" ]; then
        wait "$recorder" 2>/dev/null || :
        recorder=
    fi
    rm -f "$work/output"
}

rebuild()
{
    rm -f "$tree"/rach2/sys/fs/bcache.o "$tree"/rach2/kernel \
        "$tree"/rach2-metal/sys/fs/bcache.o "$tree"/rach2-metal/kernel
    make -s rach2 rach2-metal "ARCH=$RACH2_ARCH"
}

restore()
{
    cp "$work/source.saved" "$source"
    touch "$source"
    rebuild > "$work/restore.build.log" 2>&1
    cat "$work/restore.build.log"
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
    expected=$2
    mkfifo "$work/output"
    tee "$work/$phase.log" < "$work/output" &
    recorder=$!
    ARCH=$RACH2_ARCH "$work/runutil" group tests/run metal > "$work/output" 2>&1 &
    child=$!
    status=0
    wait "$child" || status=$?
    stop_child
    if [ "$expected" = pass ]; then
        [ "$status" -eq 0 ] || exit 1
        grep -q 'FAT acceptance rejects a busy unmount despite flushes' "$work/$phase.log"
        grep -q 'FAT remount and independent host bytes match: OK' "$work/$phase.log"
    else
        [ "$status" -ne 0 ] || {
            echo "metal-fat-mutation: $phase was not detected" >&2
            exit 1
        }
        grep -Fq "$expected" "$work/$phase.log"
        grep -q 'stick: cached bytes match' "$work/$phase.log"
        grep -q 'mutation: FAT witness changed' "$tree"/images/metal-test.metal.log
    fi
    cp "$tree"/rach2-metal/kernel "$work/$phase.kernel"
    cp "$tree"/rach2/kernel "$work/$phase.normal-kernel"
    cksum "$source" "$tree"/rach2-metal/kernel "$tree"/rach2/kernel \
        "$tree"/images/rach2-metal.krffs > "$work/$phase.artifacts"
    mkdir "$work/$phase.logs"
    for log in "$tree"/images/metal-test*.log; do
        [ ! -f "$log" ] || cp "$log" "$work/$phase.logs/"
    done
    echo "metal-fat-mutation: result: $phase status $status"
}

echo "metal-fat-mutation: logs: $work"
"${CC:-cc}" -O2 -Wall -Wextra -Werror -o "$work/runutil" \
    tests/system/runner/runutil.c > "$work/helper.build.log" 2>&1
cat "$work/helper.build.log"
run_phase fixed pass
for fault in $faults; do
    changed=1
    apply_fault "$source" "$fault"
    rebuild > "$work/$fault.build.log" 2>&1
    cat "$work/$fault.build.log"
    if cmp -s "$work/fixed.kernel" "$tree"/rach2-metal/kernel; then
        echo 'metal-fat-mutation: the compiled kernel did not change' >&2
        exit 1
    fi
    case "$fault" in
        disk) expected='FAIL: stick: remount bytes differ' ;;
        cached) expected='FAIL: stick: host FAT bytes differ' ;;
    esac
    run_phase "$fault" "$expected"
    if [ "$fault" = cached ]; then
        grep -q 'stick: remount bytes match after successful unmounts' "$work/$fault.log"
    fi
    restore
    run_phase "$fault-restored" pass
    cmp "$work/fixed.kernel" "$work/$fault-restored.kernel"
    cmp "$work/fixed.normal-kernel" "$work/$fault-restored.normal-kernel"
done
echo 'metal-fat-mutation: result: pass, both compiled faults failed and restored kernels matched'
