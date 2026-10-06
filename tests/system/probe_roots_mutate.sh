#!/bin/sh
# Prove the ext2 link, statfs and block-count branches with rebuilt kernel
# faults, on the kernel, test image and emulator of the architecture ARCH
# names.
set -eu

repo=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
cd "$repo"
RACH2_ARCH=${RACH2_ARCH:-${ARCH:-i686}}
REPO=$repo
. "$repo/mk/arch.sh"
RACH2_QEMU=$ARCH_QEMU
export RACH2_QEMU ARCH_BUILD
tree=build$ARCH_SUFFIX
. "$repo/tests/system/runlock.sh"
runlock_hold "$repo" probe-roots-mutation "$repo/tests/system/probe_roots_mutate.sh" "$@"
mkdir -p build
work=$(mktemp -d "$repo/build/probe-roots-proof.XXXXXX")
source=rach2/sys/fs/ext2.c
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
    rm -f "$tree"/rach2/sys/fs/ext2.o "$tree"/rach2/kernel
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

run_phase()
{
    phase=$1
    expected=$2
    rebuild >"$work/$phase.build.log" 2>&1
    cat "$work/$phase.build.log"
    cp "$tree"/rach2/kernel "$work/$phase.kernel"
    cksum "$source" "$work/$phase.kernel" >"$work/$phase.artifacts"
    for root in krffs ext2; do
        set --
        if [ "$expected" = fail ]; then
            [ "$root" = ext2 ] || continue
            set -- vfsprobe
        fi
        prefix="$work/$phase.$root"
        build/tests/system/runutil group sh tests/system/probe_roots.sh \
            "$work/$phase.kernel" "$prefix" "$root" "$@" \
            >"$prefix.log" 2>&1 &
        child=$!
        status=0
        wait "$child" || status=$?
        stop_child
        cat "$prefix.log"
        if [ "$expected" = pass ]; then
            [ "$status" -eq 0 ] || exit 1
        else
            [ "$status" -ne 0 ] || {
                echo "probe-roots-mutation: fault passed: $phase" >&2; exit 1;
            }
            grep -F "VFSPROBE FAIL: $detail" "$prefix.vfsprobe.serial.log" >/dev/null
            grep -Eq '(^|] )sys_exit: init process [0-9]+ exited with status 1, returning to kernel shell' \
                "$prefix.vfsprobe.serial.log" >/dev/null
        fi
        echo "probe-roots-mutation: result: $phase $root status $status"
    done
}

echo "probe-roots-mutation: logs: $work"
make -s test-image build/tests/system/harness build/tests/system/runutil \
    build/tools/image/mkmbr "ARCH=$RACH2_ARCH" >"$work/build.log" 2>&1
cat "$work/build.log"
run_phase fixed pass
for fault in link magic blocks; do
    changed=1
    python3 - "$source" "$fault" <<'PY'
from pathlib import Path
import sys

path = Path(sys.argv[1])
text = path.read_text()
if sys.argv[2] == 'link':
    start = text.index('static int ext2_link(')
    end = text.index('\n}', start)
    body = text[start:end]
    old = 'if ((n->mode & S_IFMT_) == S_IFDIR_) {'
    assert body.count(old) == 1
    text = text[:start] + body.replace(old, 'if (n != 0) {') + text[end:]
elif sys.argv[2] == 'magic':
    old = 'st->magic = EXT2_SUPER_MAGIC;'
    assert text.count(old) == 1
    text = text.replace(old, 'st->magic = 0x4B524646u;')
else:
    old = 'st->blocks_known = 1;'
    assert text.count(old) == 1
    text = text.replace(old, 'st->blocks_known = 0;')
path.write_text(text)
PY
    case "$fault" in
        link) detail='link on ext2 succeeds' ;;
        magic) detail='statfs64 of the root' ;;
        blocks) detail='st_blocks of a file that is one hole' ;;
    esac
    run_phase "$fault" fail
    if cmp -s "$work/fixed.kernel" "$work/$fault.kernel"; then
        echo 'probe-roots-mutation: kernel did not change' >&2; exit 1;
    fi
    restore
    run_phase "$fault-restored" pass
    cmp "$work/fixed.kernel" "$work/$fault-restored.kernel"
done
echo 'probe-roots-mutation: result: pass, compiled faults failed and restored kernels matched'
