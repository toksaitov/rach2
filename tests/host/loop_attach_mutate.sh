#!/bin/sh
# Compile loop attachment with fixed, stale and restored resolution.
set -eu

repo=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
cd "$repo"
mkdir -p build/tests
work=$(mktemp -d "$repo/build/tests/loop-attach-proof.XXXXXX")
cp -R rach2/sys "$work/sys"
source="$work/sys/block/loop.c"
cp "$source" "$work/source.saved"
child=

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

run_phase()
{
    phase=$1
    expected=$2
    message=$3
    binary="$work/$phase"
    if ! "${CC:-cc}" -std=c99 -D_DEFAULT_SOURCE -D_FORTIFY_SOURCE=0 -DRACH2_HOSTTEST \
        -fsanitize=address,undefined -fno-sanitize-recover=all -g -O1 \
        -Wno-empty-body -Wno-pointer-to-int-cast -Wno-int-to-pointer-cast -ffreestanding \
        -I "$work/sys" -I "$work/sys/lib" -I "$work/sys/proc" \
        tests/host/loop_attach_hosttest.c "$work/sys/block/chain.c" \
        "$work/sys/lib/kstring.c" "$work/sys/lib/kctypes.c" \
        -o "$binary" >"$binary.compile.log" 2>&1; then
        cat "$binary.compile.log" >&2
        exit 1
    fi
    cksum "$source" "$binary" >"$binary.cksum"
    perl -e 'alarm 10; exec @ARGV or die "loop-attach-mutation: exec: $!\n"' \
        "$binary" >"$binary.log" 2>&1 &
    child=$!
    status=0
    wait "$child" || status=$?
    child=
    cat "$binary.log"
    [ "$status" -eq "$expected" ] && grep -Fq "$message" "$binary.log" || exit 1
    echo "loop-attach-mutation: result: $phase status $status"
}

echo "loop-attach-mutation: logs: $work"
run_phase fixed 0 'loop-attach-hosttest: result: pass:'
python3 - "$source" <<'PY'
from pathlib import Path
import sys

path = Path(sys.argv[1])
source = path.read_text()
start = source.index('static int loop_resolve(')
end = source.index('\n}', start) + 2
source = source[:start] + '''static int loop_resolve(const char *path, struct vfs_ref *ref)
{
    int rc = vfs_resolve(0, path, 0, ref, 0, 0);

    if (rc == 0) {
        vfs_ns_lock(ref->mnt);
    }
    return rc;
}''' + source[end:]
path.write_text(source)
PY
run_phase stale-resolution 1 'loop read avoids the reused old extent'
cleanup
run_phase restored 0 'loop-attach-hosttest: result: pass:'
cmp "$source" "$work/source.saved"
