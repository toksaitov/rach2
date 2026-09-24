#!/bin/sh
# Prove the ext2 link and statfs branches with rebuilt kernel faults.
set -eu

repo=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
cd "$repo"
[ ! -d build/.run.lock ] || {
    echo 'probe-roots-mutation: another suite owns this tree' >&2; exit 1;
}
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
    rm -f build/rach2/sys/fs/ext2.o build/rach2/kernel
    make -s rach2
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
    cp build/rach2/kernel "$work/$phase.kernel"
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
    build/tools/image/mkmbr >"$work/build.log" 2>&1
cat "$work/build.log"
run_phase fixed pass
for fault in link magic; do
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
else:
    old = 'st->magic = EXT2_SUPER_MAGIC;'
    assert text.count(old) == 1
    text = text.replace(old, 'st->magic = 0x4B524646u;')
path.write_text(text)
PY
    case "$fault" in
        link) detail='link on ext2 succeeds' ;;
        magic) detail='statfs64 of the root' ;;
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
