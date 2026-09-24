#!/bin/sh
set -eu

repo=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
cd "$repo"
[ ! -d build/.run.lock ] || {
    echo 'install-keys-mutation: another suite owns this tree' >&2; exit 1;
}
mkdir -p build
work=$(mktemp -d "$repo/build/install-keys-proof.XXXXXX")
echo "install-keys-mutation: logs: $work"
rsync -a --exclude=/.git --exclude=/build/.run.lock --exclude=/build/images \
    --exclude='/build/install-keys-proof.*' "$repo/" "$work/tree/"
cd "$work/tree"
source="$work/tree/utils/rach2-install"
cp "$source" "$work/installer.saved"
child=

stop_child()
{
    if [ -n "$child" ]; then
        kill -TERM -- "-$child" 2>/dev/null || :
        wait "$child" 2>/dev/null || :
        while kill -0 -- "-$child" 2>/dev/null; do sleep 1; done
        child=
    fi
}

restore()
{
    cp "$work/installer.saved" "$source"
    touch "$source"
}

cleanup()
{
    stop_child
    restore
}
trap cleanup EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

# Each phase rebuilds the images which stage the installer. The source tree
# and build outputs are private so an interrupted proof cannot affect a suite.
run_phase()
{
    phase=$1
    expected=$2
    rm -f build/images/rach2-recovery*.krffs build/images/rach2-recovery*.iso \
        build/images/installer-root.ext2 build/images/installer-test.disk \
        build/images/install-test-stick.img
    make -s -j8 build/images/install-test-stick.img \
        build/tests/system/harness build/tests/system/runutil >"$work/$phase.build.log" 2>&1
    cat "$work/$phase.build.log"
    shasum -a 256 "$source" build/images/install-test-stick.img \
        build/images/installer-root.ext2 >"$work/$phase.artifacts"
    cp "$source" "$work/$phase.installer"
    build/tests/system/runutil group sh tests/system/suites/installer.sh \
        build/images/install-test-stick.img build/images/installer-test.disk \
        build/tests/system/harness "build/images/keys-$phase" \
        "$(cat krffs/rach2_demo_password)" build/images/install-test-stick.img \
        build/images/installer-test.disk "${RACH2_INSTALL_KEYS_PORT:-2327}" \
        unused krffs/rach2_demo_ed25519 resume build/images/installer-root.ext2 \
        >"$work/$phase.log" 2>&1 &
    child=$!
    status=0
    wait "$child" || status=$?
    stop_child
    cat "$work/$phase.log"
    if [ "$status" -ne "$expected" ]; then
        echo "install-keys-mutation: wrong result: $phase status $status" >&2; exit 1
    fi
    grep -Fq 'retried from the marked archive: OK' "$work/$phase.log"
    if [ "$expected" -eq 0 ]; then
        grep -Fq 'row 17: host, root and test keys match the installed machine: OK' "$work/$phase.log"
        grep -Fq 'row 17: root logs in with its restored key: OK' "$work/$phase.log"
    else
        grep -Fq 'FAIL: row 17:' "$work/$phase.log"
    fi
    echo "install-keys-mutation: result: $phase status $status"
}

make -s -j8 rach2 utils krffs >"$work/components.build.log" 2>&1
cat "$work/components.build.log"
run_phase fixed 0
python3 - "$source" <<'PY'
from pathlib import Path
import sys

path = Path(sys.argv[1])
source = path.read_text()
start = source.index('    for keys in ', source.index('write_archive()'))
end = source.index('    if [ -n "${2:-}" ]; then', start)
assert 'could not be archived' in source[start:end]
path.write_text(source[:start] + source[end:])
PY
run_phase mutated 1
restore
run_phase restored 0
cmp "$work/fixed.installer" "$work/restored.installer"
echo 'install-keys-mutation: result: pass: row 17 rejected missing keys and passed after restoration'
