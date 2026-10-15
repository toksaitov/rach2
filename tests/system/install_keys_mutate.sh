#!/bin/sh
# install_keys_mutate.sh [--texts]: show that the installer suite catches
# three faults in utils/rach2-install, built in a private copy of the tree,
# on the kernel, images and emulator of the architecture ARCH names.
#
# `keys` takes the archiving of the machine's Dropbear keys out of
# write_archive, which row 17 must catch. `archive` makes the update rebuild
# the archive slot it was chosen from, which leaves no complete copy when the
# update stops, and row archive-stop must catch it. `case` takes out the
# refusal of two key names that differ only in case, which row archive-keys
# must catch. The installer suite must pass on the installer as it is and
# after each restoration. --texts checks that each fault is in the installer.
set -eu

repo=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
cd "$repo"

# apply_keys SOURCE: put the `keys` fault into SOURCE; with
# RACH2_FAULT_TEXTS set, only check its text and write nothing.
apply_keys()
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
start = source.index('    if [ -d "$1/etc/dropbear" ]; then', source.index('write_archive()'))
end = source.index('    if [ -n "${2:-}" ]; then', start)
assert 'could not be archived' in source[start:end]
path.write_text(source[:start] + source[end:])
PY
}


# apply_archive SOURCE: put the `archive` fault into SOURCE; with
# RACH2_FAULT_TEXTS set, only check its text and write nothing.
apply_archive()
{
    python3 - "$1" <<'PYOLD'
from pathlib import Path
import sys
import os
import pathlib
if os.environ.get('RACH2_FAULT_TEXTS'):
    pathlib.Path.write_text = lambda self, *args, **kwargs: 0

path = Path(sys.argv[1])
source = path.read_text()
needle = '    rm -f "$next_archive/generation"'
assert source.count(needle) == 1
source = source.replace(needle, '    [ -z "$archive" ] || next_archive="$archive"\n' + needle)
path.write_text(source)
PYOLD
}


# apply_case SOURCE: put the `case` fault into SOURCE; with
# RACH2_FAULT_TEXTS set, only check its text and write nothing.
apply_case()
{
    python3 - "$1" <<'PYCASE'
from pathlib import Path
import sys
import os
import pathlib
if os.environ.get('RACH2_FAULT_TEXTS'):
    pathlib.Path.write_text = lambda self, *args, **kwargs: 0

path = Path(sys.argv[1])
source = path.read_text()
needle = '                die "ssh key names collide on FAT32: $entry and $other"'
assert source.count(needle) == 1
path.write_text(source.replace(needle, '                :'))
PYCASE
}

if [ "${1:-}" = --texts ]; then
    RACH2_FAULT_TEXTS=1
    export RACH2_FAULT_TEXTS
    apply_keys utils/rach2-install >/dev/null
    apply_archive utils/rach2-install >/dev/null
    apply_case utils/rach2-install >/dev/null
    echo 'install-keys-mutation: texts: every fault is once in its source'
    exit 0
fi


RACH2_ARCH=${RACH2_ARCH:-${ARCH:-i686}}
REPO=$repo
. "$repo/mk/arch.sh"
RACH2_QEMU=$ARCH_QEMU
export RACH2_QEMU ARCH_BUILD
tree=build$ARCH_SUFFIX
. "$repo/tests/system/runlock.sh"
runlock_hold "$repo" install-keys-mutation "$repo/tests/system/install_keys_mutate.sh" "$@"
mkdir -p build
work=$(mktemp -d "$repo/build/install-keys-proof.XXXXXX")
echo "install-keys-mutation: logs: $work"
# The tree with its built programs, so nothing is built again, and without
# the images, which the rows make, the GNU layers, the failed runs' logs and
# the other proofs' work, a hundred gigabytes on a host that has built the
# GNU system.
rsync -a --exclude=/.git --exclude=/build/.run.lock --exclude=/build/images \
    --exclude='/build/*/images' --exclude=/build/vendor/gnu --exclude='/build/*/vendor/gnu' \
    --exclude=/build/tests/failed --exclude='/build/machine-*' --exclude='/build/*-proof.*' \
    "$repo/" "$work/tree/"
cd "$work/tree"
ARCH_BUILD=$work/tree/build$ARCH_SUFFIX
source="$work/tree/utils/rach2-install"
cp "$source" "$work/installer.saved"
child=

# Stop the running suite's process group and wait until it is gone.
stop_child()
{
    if [ -n "$child" ]; then
        kill -TERM "-$child" 2>/dev/null || :
        wait "$child" 2>/dev/null || :
        while kill -0 "-$child" 2>/dev/null; do sleep 1; done
        child=
    fi
}

# Put back the private tree's saved installer and touch it.
restore()
{
    cp "$work/installer.saved" "$source"
    touch "$source"
}

# Stop the suite and restore the installer.
cleanup()
{
    stop_child
    restore
}
trap cleanup EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

# run_phase PHASE STATUS [FAILURE]: rebuild the images that stage the
# installer and run the installer suite on them, which must exit with STATUS:
# 0 with the archive and key rows passed, or 1 with `FAIL: FAILURE`. The
# tree and its build outputs are private, so a run that is stopped leaves
# the main tree's images as they were.
run_phase()
{
    phase=$1
    expected=$2
    failure=${3:-}
    rm -f "$tree"/images/rach2-recovery*.krffs "$tree"/images/rach2-recovery*.iso \
        "$tree"/images/installer-root.ext2 "$tree"/images/installer-test.disk \
        "$tree"/images/install-test-stick.img
    make -s -j8 "$tree"/images/install-test-stick.img build/tests/host/mkexfatfixture \
        build/tests/system/harness build/tests/system/runutil "ARCH=$RACH2_ARCH" \
        >"$work/$phase.build.log" 2>&1
    cat "$work/$phase.build.log"
    shasum -a 256 "$source" "$tree"/images/install-test-stick.img \
        "$tree"/images/installer-root.ext2 >"$work/$phase.artifacts"
    cp "$source" "$work/$phase.installer"
    build/tests/system/runutil group sh tests/system/suites/installer.sh \
        "$tree"/images/install-test-stick.img "$tree"/images/installer-test.disk \
        build/tests/system/harness "$tree/images/keys-$phase" \
        "$(cat krffs/rach2_demo_password)" "$tree"/images/install-test-stick.img \
        "$tree"/images/installer-test.disk "${RACH2_INSTALL_KEYS_PORT:-2327}" \
        unused krffs/rach2_demo_ed25519 resume "$tree"/images/installer-root.ext2 \
        >"$work/$phase.log" 2>&1 &
    child=$!
    status=0
    wait "$child" || status=$?
    stop_child
    cat "$work/$phase.log"
    if [ "$status" -ne "$expected" ]; then
        echo "install-keys-mutation: wrong result: $phase status $status" >&2; exit 1
    fi
    if [ "$expected" -eq 0 ]; then
        grep -Fq 'row archive-stop: the complete legacy archive supplies the machine configuration: OK' "$work/$phase.log"
        grep -Fq 'retried from the marked archive: OK' "$work/$phase.log"
        grep -Fq 'row 17: host and account keys match the installed machine: OK' "$work/$phase.log"
        grep -Fq 'row 17: root logs in with its restored key: OK' "$work/$phase.log"
    else
        grep -Fq "FAIL: $failure" "$work/$phase.log"
        case $failure in
            'row 17:')
                grep -Fq 'retried from the marked archive: OK' "$work/$phase.log" ;;
            'row archive-stop:'*)
                grep -Fq 'STOP_STATUS=99' "$tree/images/keys-$phase/archive-stop.log" ;;
        esac
    fi
    echo "install-keys-mutation: result: $phase status $status"
}

make -s -j8 rach2 utils krffs "ARCH=$RACH2_ARCH" >"$work/components.build.log" 2>&1
cat "$work/components.build.log"
run_phase fixed 0
apply_keys "$source"
run_phase mutated 1 "row 17:"
restore
run_phase restored 0
cmp "$work/fixed.installer" "$work/restored.installer"
# Rebuilding the chosen slot destroys the only complete copy at the stop.
apply_archive "$source"
run_phase archive-mutated 1 "row archive-stop: the retry did not recover"
restore
run_phase archive-restored 0
cmp "$work/fixed.installer" "$work/archive-restored.installer"
# Without the check, two key names that differ only in case reach FAT32,
# which folds them into one file, and the update writes the root.
apply_case "$source"
run_phase case-mutated 1 "row archive-keys: two key names that differ only in case were not refused"
restore
run_phase case-restored 0
cmp "$work/fixed.installer" "$work/case-restored.installer"
echo 'install-keys-mutation: result: pass: missing keys, rebuilding the chosen slot and names that differ in case failed their rows; each restoration passed'
