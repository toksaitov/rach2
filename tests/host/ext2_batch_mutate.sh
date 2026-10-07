#!/bin/sh
# Compile and run the ext2 batch rows with fixed, faulty and restored sources.
set -eu

repo=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
cd "$repo"

# apply_per_call ARGS...: put one fault into its source; with
# RACH2_FAULT_TEXTS set, only check its text and write nothing.
apply_per_call()
{
    python3 - "$1" <<'PY'
from pathlib import Path
import sys
import os
import pathlib
if os.environ.get('RACH2_FAULT_TEXTS'):
    pathlib.Path.write_text = lambda self, *args, **kwargs: 0

path = Path(sys.argv[1])
source = path.read_text()
old = 'if (synchronous || wb->bytes == EXT2_WB_BYTES)'
if source.count(old) != 1:
    raise SystemExit('ext2-batch-mutation: source pattern is not unique')
path.write_text(source.replace(old, 'if (synchronous || 1)'))
PY
}


# apply_drain ARGS...: put one fault into its source; with
# RACH2_FAULT_TEXTS set, only check its text and write nothing.
apply_drain()
{
    python3 - "$1" <<'PY'
from pathlib import Path
import sys
import os
import pathlib
if os.environ.get('RACH2_FAULT_TEXTS'):
    pathlib.Path.write_text = lambda self, *args, **kwargs: 0

path = Path(sys.argv[1])
source = path.read_text()
start = source.index('static int writeback_drain(struct ext2_mount *em)\n{')
end = source.index('\n}', start) + 2
body = source[start:end]
old = 'rc = block_flush(em->dev);'
if body.count(old) != 1:
    raise SystemExit('ext2-batch-mutation: drain flush pattern is not unique')
path.write_text(source[:start] + body.replace(old, 'rc = 0;') + source[end:])
PY
}


# apply_fault ARGS...: put one fault into its source; with
# RACH2_FAULT_TEXTS set, only check its text and write nothing.
apply_fault()
{
    python3 - "$1" "$2" <<'PYFAULT'
from pathlib import Path
import sys
import os
import pathlib
if os.environ.get('RACH2_FAULT_TEXTS'):
    pathlib.Path.write_text = lambda self, *args, **kwargs: 0

path = Path(sys.argv[1])
source = path.read_text()
fault = sys.argv[2]
function = {
    'release': 'static void ext2_release(',
    'sticky': 'static int ext2_fsync(',
    'handoff': 'static int ext2_setattr(',
    'abort': 'static void ext2_abort_unmount(',
    'zero': 'static int writeback_prepare_data(',
    'stage-lock': 'static int ext2_fsync(',
    'reserve-run': 'static int writeback_init(struct ext2_mount *em)\n{',
}.get(fault, 'static int32_t ext2_write(')
start = source.index(function)
end = source.index('\n}', start) + 2
body = source[start:end]
changes = {
    'bytes': ('synchronous || wb->bytes == EXT2_WB_BYTES', 'synchronous'),
    'data': ('wb->datas == EXT2_WB_DATA', '0'),
    'metadata': ('wb->metas == EXT2_WB_META', '0'),
    'inodes': ('wb->count == EXT2_WB_NODES', '0'),
    'release': ('(void)writeback_drain(em_of(mnt));', '(void)mnt;'),
    'sticky': ('if (mnt->write_error)', 'if (0)'),
    'abort': ('em->state &= ~EXT2_VALID_FS;', 'em->state |= EXT2_VALID_FS;'),
    'zero': ('k_memset(data->data, 0, BCACHE_BLOCK_BYTES);',
             'k_memset(data->data, 0xAB, BCACHE_BLOCK_BYTES);'),
    'reserve-run': ('kalloc_pages(1, 0)', 'kalloc_pages(EXT2_WB_DATA + EXT2_WB_META + 1, 0)'),
}
def first(body, old, new):
    if old not in body:
        raise SystemExit('ext2-batch-mutation: fault pattern is not in its function: ' + fault)
    return body.replace(old, new, 1)


if fault == 'stage-lock':
    body = first(body, '    vfs_ns_lock(mnt);\n', '')
    body = first(body, '    vfs_ns_unlock(mnt);\n', '')
elif fault == 'handoff':
    body = first(body, '    vfs_ns_lock(mnt);\n', '')
    body = first(body, '    if (what & VFS_SET_MODE)', '    vfs_ns_lock(mnt);\n    if (what & VFS_SET_MODE)')
else:
    old, new = changes[fault]
    if body.count(old) != 1:
        raise SystemExit('ext2-batch-mutation: fault pattern is not unique: ' + fault)
    body = body.replace(old, new)
path.write_text(source[:start] + body + source[end:])
PYFAULT
}

faults='bytes data metadata inodes release sticky handoff abort zero
    stage-lock reserve-run'
if [ "${1:-}" = --texts ]; then
    RACH2_FAULT_TEXTS=1 apply_per_call rach2/sys/fs/ext2.c >/dev/null || exit 1
    RACH2_FAULT_TEXTS=1 apply_drain rach2/sys/fs/ext2.c >/dev/null || exit 1
    for fault in $faults; do
        RACH2_FAULT_TEXTS=1 apply_fault rach2/sys/fs/ext2.c "$fault" >/dev/null || exit 1
    done
    echo 'ext2-batch-mutation: texts: every fault is once in its source'
    exit 0
fi

. "$repo/tests/system/runlock.sh"
runlock_hold "$repo" ext2-batch-mutation "$repo/tests/host/ext2_batch_mutate.sh" "$@"

mode=${1:---batch-only}
case "$mode" in
    --batch-only|--all) ;;
    *) echo "ext2-batch-mutation: usage: $0 [--batch-only|--all]" >&2; exit 2 ;;
esac
mkdir -p build/tests
work=$(mktemp -d "$repo/build/tests/ext2-batch-proof.XXXXXX")
mkdir -p "$work/rach2" "$work/tests/host"
cp -R rach2/sys "$work/rach2/sys"
cp -R tests/host/compat "$work/tests/host/compat"
cp tests/host/ext2_hosttest.c tests/host/ext2_batch_cases.h \
    tests/host/ext2_batch_lifecycle.h "$work/tests/host/"
source="$work/rach2/sys/fs/ext2.c"
cp "$source" "$work/source.saved"
child=
# Each phase's fixtures, in a directory of the phase's own that cleanup
# removes: a test given none makes its own and keeps it when it fails, as
# each fault makes it do.
fixtures=

cleanup()
{
    if [ -n "$child" ]; then
        kill -TERM "-$child" 2>/dev/null || :
        wait "$child" 2>/dev/null || :
        while kill -0 "-$child" 2>/dev/null; do
            sleep 1
        done
        child=
    fi
    if [ -n "$fixtures" ]; then
        rm -rf "$fixtures"
        fixtures=
    fi
    cp "$work/source.saved" "$source"
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
    message=${3:-}
    want_status=${4:-1}
    binary="$work/$phase"
    fixtures=$(mktemp -d /tmp/rach2-fix.XXXXXX)
    if ! "${CC:-cc}" -std=c99 -D_DEFAULT_SOURCE -D_FORTIFY_SOURCE=0 -DRACH2_HOSTTEST \
        -DFIXDIR="\"$fixtures\"" \
        -fsanitize=address,undefined -fno-sanitize-recover=all \
        -g -O1 -Wno-empty-body -ffreestanding \
        -I "$work/rach2/sys" -I "$work/rach2/sys/lib" -I "$work/rach2/sys/proc" \
        -I utils -I krffs -I "$work/tests/host/compat" \
        -o "$binary" "$work/tests/host/ext2_hosttest.c" \
        "$source" "$work/rach2/sys/fs/bcache.c" "$work/rach2/sys/fs/fatcommon.c" \
        "$work/rach2/sys/fs/fd.c" "$work/rach2/sys/fs/lock.c" "$work/rach2/sys/block/chain.c" \
        "$work/rach2/sys/lib/kstring.c" \
        "$work/rach2/sys/lib/kctypes.c" "$work/tests/host/compat/kheap_host.c" \
        "$work/tests/host/compat/pressure_host.c" "$work/tests/host/compat/fixdir_host.c" \
        >"$binary.compile.log" 2>&1; then
        cat "$binary.compile.log" >&2
        exit 1
    fi
    cksum "$source" "$binary" >"$binary.cksum"
    if [ "$mode" = --all ]; then
        set --
    else
        set -- --batch-only
    fi
    build/tests/system/runutil group \
        perl -e 'alarm 180; exec @ARGV or die "ext2-batch-mutation: exec: $!\n"' \
        "$binary" "$@" >"$binary.log" 2>&1 &
    child=$!
    status=0
    wait "$child" || status=$?
    cleanup
    cat "$binary.log"
    if [ "$expected" = pass ]; then
        [ "$status" -eq 0 ] && grep -Fq 'ext2_hosttest: PASS' "$binary.log" || exit 1
    else
        message=${message:-'FAIL: cross-call rewrite collection writes no blocks'}
        if [ "$expected" = media ]; then
            message='FAIL: volatile media preserves pointer and size dependencies'
        fi
        [ "$status" -eq "$want_status" ] && \
            grep -Fq "$message" "$binary.log" \
            || exit 1
    fi
    echo "ext2-batch-mutation: result: $phase status $status"
}

echo "ext2-batch-mutation: logs: $work"
make -s build/tests/system/runutil \
    >"$work/tools.build.log" 2>&1
cat "$work/tools.build.log"
run_phase fixed pass
apply_per_call "$source"
run_phase per-call fail
cmp "$source" "$work/source.saved"
run_phase restored pass
cmp "$source" "$work/source.saved"
apply_drain "$source"
run_phase no-stage-flush media
cmp "$source" "$work/source.saved"
run_phase flush-restored pass
cmp "$source" "$work/source.saved"
for fault in $faults; do
    apply_fault "$source" "$fault"
    status=1
    case $fault in
        bytes) message='FAIL: the byte limit drains on its last block' ;;
        data) message='FAIL: 256 distinct blocks drain without reaching the byte limit' ;;
        metadata) message='FAIL: 32 metadata images drain before another admission' ;;
        inodes) message='FAIL: the sixteenth inode drains the batch' ;;
        release) message='FAIL: the last shared close drains with an independent handle open' ;;
        sticky) message='FAIL: a successful retry preserves the earlier error' ;;
        handoff) message='ext2_hosttest: panic: ext2: inode store before writeback drain'; status=3 ;;
        abort) message='FAIL: an abandoned unmount clears the valid bit on disk' ;;
        zero) message='FAIL: partial bytes and zeroing precede durable size or hole publication' ;;
        stage-lock) message='FAIL: a draining generation retains mount ownership across I/O' ;;
        reserve-run) message='FAIL: a read-write mount needs no run longer than a reserve run' ;;
    esac
    run_phase "$fault-mutated" fault "$message" "$status"
    cmp "$source" "$work/source.saved"
    run_phase "$fault-restored" pass
    cmp "$source" "$work/source.saved"
done
echo "ext2-batch-mutation: result: pass, compiled fault failed and restored source passed"
