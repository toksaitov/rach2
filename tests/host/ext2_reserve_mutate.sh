#!/bin/sh
# Prove ext2_reserve_hosttest.c, the ext2 driver's checks of free and reserved
# blocks before a write, with private mutations of rach2/sys/fs/ext2.c: the
# `estimate` fault puts a walk of the data blocks with an estimate of the
# pointer blocks in place of writeback_space; `pending` makes the check in
# writeback_reserve leave out the blocks the batch has reserved; `original`
# is the estimate with no check in writeback_reserve at all.
set -eu

repo=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
cd "$repo"

# apply_fault FILE FAULT: put FAULT into FILE, a copy of ext2.c; with
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
source = path.read_text()
if sys.argv[2] in ('estimate', 'original'):
    start = source.index('static int writeback_space(')
    end = source.index('\n}', start) + 2
    source = source[:start] + '''static int writeback_space(struct ext2_mount *em, struct ext2_node *n,
                           uint64_t offset, uint32_t count)
{
    uint32_t first = (uint32_t)(offset >> em->block_shift);
    uint32_t last = (uint32_t)((offset + count - 1) >> em->block_shift);
    uint64_t need = 0;
    uint32_t i;
    int rc;

    for (i = first; i <= last && need <= em->free_blocks + 3; i++) {
        uint32_t b;

        rc = block_at(em, n, i, &b);
        if (rc != 0) {
            return rc;
        }
        if (b == 0) {
            need++;
        }
    }
    if (need > 0) {
        need += pointer_levels(em, last) + (last - first) / (em->block_size / 4);
    }
    if (need > em->free_blocks ||
        (!may_use_reserved() && need > 0 && em->free_blocks - need < em->r_blocks)) {
        return -ENOSPC;
    }
    return 0;
}''' + source[end:]
else:
    old = ('em->free_blocks <= (uint32_t)count ||\n'
           '        (!may_use_reserved() && em->free_blocks - (uint32_t)count <= em->r_blocks)')
    new = ('em->free_blocks == 0 ||\n'
           '        (!may_use_reserved() && em->free_blocks <= em->r_blocks)')
    if source.count(old) != 1:
        raise SystemExit('ext2-reserve-mutation: source pattern is not unique')
    source = source.replace(old, new)
if sys.argv[2] == 'original':
    old = ('    if (em->free_blocks <= (uint32_t)count ||\n'
           '        (!may_use_reserved() && em->free_blocks - (uint32_t)count <= em->r_blocks)) {\n'
           '        return -ENOSPC;\n'
           '    }\n')
    if source.count(old) != 1:
        raise SystemExit('ext2-reserve-mutation: quota pattern is not unique')
    source = source.replace(old, '')
path.write_text(source)
PY
}

faults='estimate pending original'
if [ "${1:-}" = --texts ]; then
    for fault in $faults; do
        RACH2_FAULT_TEXTS=1 apply_fault rach2/sys/fs/ext2.c "$fault" >/dev/null || exit 1
    done
    echo 'ext2-reserve-mutation: texts: every fault is once in its source'
    exit 0
fi

. "$repo/tests/system/runlock.sh"
runlock_hold "$repo" ext2-reserve-mutation "$repo/tests/host/ext2_reserve_mutate.sh" "$@"

mkdir -p build/tests
work=$(mktemp -d "$repo/build/tests/ext2-reserve-proof.XXXXXX")
cp -R rach2/sys "$work/sys"
source="$work/sys/fs/ext2.c"
cp "$source" "$work/source.saved"
child=

# cleanup: stop the running test and put the saved ext2.c back; the loop calls
# it after each fault, and it is the exit trap.
cleanup()
{
    if [ -n "$child" ]; then
        kill -TERM "$child" 2>/dev/null || :
        wait "$child" 2>/dev/null || :
        child=
    fi
    cp "$work/source.saved" "$source"
    touch "$source"
}
trap cleanup EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

# run_phase PHASE STATUS MESSAGE: compile ext2_reserve_hosttest.c over the copy
# as $work/PHASE, run it for at most 10 seconds, and exit unless it ends with
# STATUS and its log contains MESSAGE.
run_phase()
{
    phase=$1
    expected=$2
    message=$3
    binary="$work/$phase"
    if ! "${CC:-cc}" -std=c99 -D_DEFAULT_SOURCE -D_FORTIFY_SOURCE=0 -DRACH2_HOSTTEST \
        -fsanitize=address,undefined -fno-sanitize-recover=all -g -O1 \
        -Wno-empty-body -ffreestanding \
        -I "$work/sys" -I "$work/sys/lib" -I "$work/sys/proc" \
        tests/host/ext2_reserve_hosttest.c "$work/sys/lib/kstring.c" \
        "$work/sys/lib/kctypes.c" -o "$binary" >"$binary.compile.log" 2>&1; then
        cat "$binary.compile.log" >&2
        exit 1
    fi
    cksum "$source" "$binary" >"$binary.cksum"
    perl -e 'alarm 10; exec @ARGV or die "ext2-reserve-mutation: exec: $!\n"' \
        "$binary" >"$binary.log" 2>&1 &
    child=$!
    status=0
    wait "$child" || status=$?
    child=
    cat "$binary.log"
    [ "$status" -eq "$expected" ] && grep -Fq "$message" "$binary.log" || exit 1
    echo "ext2-reserve-mutation: result: $phase status $status"
}

echo "ext2-reserve-mutation: logs: $work"
run_phase fixed 0 'ext2-reserve-hosttest: result: pass:'
for fault in $faults; do
    apply_fault "$source" "$fault"
    case $fault in
        estimate) message='boundary shortage is refused before accepting bytes' ;;
        pending) message='piece quota includes uncommitted reservations' ;;
        original) message='unprivileged boundary write preserves reserved blocks' ;;
    esac
    run_phase "$fault-mutated" 1 "$message"
    cleanup
    run_phase "$fault-restored" 0 'ext2-reserve-hosttest: result: pass:'
    cmp "$source" "$work/source.saved"
done
