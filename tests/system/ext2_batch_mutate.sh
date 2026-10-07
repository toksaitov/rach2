#!/bin/sh
# Prove cross-call counts; an optional revision also measures the starting code.
set -eu

repo=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
cd "$repo"

# apply_fault ARGS...: put one fault into its source; with
# RACH2_FAULT_TEXTS set, only check its text and write nothing.
apply_fault()
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

if [ "${1:-}" = --texts ]; then
    RACH2_FAULT_TEXTS=1
    export RACH2_FAULT_TEXTS
    apply_fault rach2/sys/fs/ext2.c >/dev/null
    echo 'ext2-batch-mutation: texts: every fault is once in its source'
    exit 0
fi


# The architecture, from ARCH as mk/arch.sh reads it: its build tree and its
# emulator.
RACH2_ARCH=${RACH2_ARCH:-${ARCH:-i686}}
REPO=$repo
. "$repo/mk/arch.sh"
RACH2_QEMU=$ARCH_QEMU
export RACH2_QEMU
kernel=build$ARCH_SUFFIX/rach2/kernel
probe=build$ARCH_SUFFIX/tests/ext2probe
. "$repo/tests/system/runlock.sh"
runlock_hold "$repo" ext2-batch-mutation "$repo/tests/system/ext2_batch_mutate.sh" "$@"
mkdir -p build
work=$(mktemp -d "$repo/build/ext2-batch-proof.XXXXXX")
source=rach2/sys/fs/ext2.c
baseline=${1:-}
cp "$source" "$work/source.saved"
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
    rm -f "build$ARCH_SUFFIX/rach2/sys/fs/ext2.o" "$kernel"
    make -s rach2 "ARCH=$RACH2_ARCH"
}

restore()
{
    cp "$work/source.saved" "$source"
    touch "$source"
    changed=
}

cleanup()
{
    stop_child
    if [ -n "$changed" ]; then
        restore
        rebuild >"$work/cleanup.build.log" 2>&1
    fi
}
trap cleanup EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

check_disk()
{
    phase=$1
    dd if="$work/$phase.disk" of="$work/$phase.ext2" bs=1048576 skip=1 count=16 \
        2>"$work/$phase.extract.log"
    e2fsck -fn "$work/$phase.ext2" >"$work/$phase.fsck.log" 2>&1 || {
        cat "$work/$phase.fsck.log" >&2; exit 1;
    }
    debugfs -R "dump /cost-rewrite $work/$phase.rewrite" "$work/$phase.ext2" \
        >"$work/$phase.debugfs.log" 2>&1
    debugfs -R "dump /cost-append $work/$phase.append" "$work/$phase.ext2" \
        >>"$work/$phase.debugfs.log" 2>&1
    python3 - "$work/$phase" <<'PY'
from pathlib import Path
import sys

prefix = sys.argv[1]
if Path(prefix + '.rewrite').read_bytes() != bytes([16]) * 4096:
    raise SystemExit('ext2-batch-mutation: wrong rewrite bytes on disk')
if Path(prefix + '.append').read_bytes() != b''.join(bytes([n]) * 4096 for n in (64, 65, 66)):
    raise SystemExit('ext2-batch-mutation: wrong append bytes on disk')
print('ext2-batch-mutation: result: disk bytes and file lengths match')
PY
}

run_phase()
{
    phase=$1
    expected=$2
    mode=${3:-cost}
    failure=${4:-rewrite}
    rebuild >"$work/$phase.build.log" 2>&1
    cat "$work/$phase.build.log"
    cp "$kernel" "$work/$phase.kernel"
    cp "$work/base.disk" "$work/$phase.disk"
    cksum "$source" "$work/$phase.kernel" "$work/probe.krffs" "$probe" \
        >"$work/$phase.artifacts"
    build/tests/system/runutil group build/tests/system/harness \
        --kernel "$work/$phase.kernel" --initrd "$work/probe.krffs" \
        --net hubport,id=net0,hubid=0 --qemu-arg=-cpu --qemu-arg=max \
        --drive "file=$work/$phase.disk,format=raw,if=ide,index=0,media=disk" \
        --append rach2.rescue --timeout 30 --steptmo 10 \
        --ok "EXT2PROBE OK ($mode)" --never 'EXT2PROBE FAIL' \
        --log "$work/$phase.serial.log" \
        "expect=Type 'help'" "send=exec /ext2probe $mode" \
        'expect=EXT2PROBE' 'expect=returning to kernel shell' \
        >"$work/$phase.log" 2>&1 &
    child=$!
    status=0
    wait "$child" || status=$?
    stop_child
    cat "$work/$phase.log" "$work/$phase.serial.log"
    if [ "$expected" = pass ]; then
        [ "$status" -eq 0 ] || exit 1
        check_disk "$phase"
    else
        [ "$status" -ne 0 ] || {
            echo "ext2-batch-mutation: fault passed" >&2; exit 1;
        }
        grep -F "EXT2PROBE FAIL: $failure command count" "$work/$phase.serial.log" \
            >/dev/null || {
            echo "ext2-batch-mutation: wrong failure" >&2; exit 1;
        }
    fi
    echo "ext2-batch-mutation: result: $phase status $status"
}

echo "ext2-batch-mutation: logs: $work"
make -s krffs build/tests/system/harness build/tests/system/runutil \
    build/tools/image/mkmbr "ARCH=$RACH2_ARCH" >"$work/build.log" 2>&1
make -s -C tests "../$probe" "ARCH=$RACH2_ARCH" >>"$work/build.log" 2>&1
cat "$work/build.log"
cat >"$work/probe.manifest" <<EOF
file 0755 /ext2probe $probe
EOF
dd if=/dev/zero of="$work/probe.krffs" bs=1048576 count=2 2>"$work/image.log"
build/krffs/mkfs.krffs "$work/probe.krffs" >>"$work/image.log" 2>&1
build/krffs/stageimage.krffs "$work/probe.manifest" "$work/probe.krffs" \
    >>"$work/image.log" 2>&1
cat "$work/image.log"
build/krffs/readfile.krffs "$work/probe.krffs" /ext2probe "$work/probe.staged"
cmp "$probe" "$work/probe.staged"
. tools/setup/e2fsprogs_path.sh
sh tests/system/mkext2fixture.sh "$work/base.disk" build/tools/image/mkmbr \
    >"$work/fixture.log" 2>&1
cat "$work/fixture.log"
run_phase fixed pass
if [ -n "$baseline" ]; then
    git show "$baseline:rach2/sys/fs/ext2.c" >"$work/baseline.source"
    changed=1
    cp "$work/baseline.source" "$source"
    touch "$source"
    run_phase starting fail
    run_phase starting-append fail cost-append append
    restore
fi
changed=1
apply_fault "$source"
run_phase per-call fail
run_phase per-call-append fail cost-append append
if cmp -s "$work/fixed.kernel" "$work/per-call.kernel"; then
    echo "ext2-batch-mutation: kernel did not change" >&2; exit 1
fi
restore
run_phase restored pass
cmp "$work/fixed.kernel" "$work/restored.kernel"
echo "ext2-batch-mutation: result: pass, compiled fault failed and restored kernel matched"
