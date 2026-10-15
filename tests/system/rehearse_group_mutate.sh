#!/bin/sh
# rehearse_group_mutate.sh [ROOT DISK] | --texts: show that the installer
# rehearsal requires its root's first ext2 group to be whole.
#
# The installer root the rehearsal saves from is built with 16384-block
# groups, so its 64 MiB are one whole group, which growset requires. With the
# recipe's group size set to mke2fs's default of 32768 blocks, the first group
# is partial, and the rehearsal's setup must fail on growset before it writes
# the base disk, the overlay or a stick.
#
# The root is built three times from the arguments of the recipe in
# tests/system/suites.mk: as written, where it must equal the artifact the
# rehearsal ran and growset must take it; with the default group size, where
# tests/system/suites/installer.sh must stop at its setup; and restored,
# which must equal the first byte for byte. Nothing under build/ is written:
# the mutated root goes into a private copy of the installer disk.
#
# Usage: sh tests/system/rehearse_group_mutate.sh [ROOT DISK] | --texts
# ROOT and DISK name the installer root and disk the rehearsal ran, by
# default installer-root.ext2 and installer-test.disk under the images of
# the architecture ARCH names, build/images or build/x86_64/images. --texts
# checks that the fault is once in suites.mk.
set -eu
repo=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
cd "$repo"

# apply_fault SOURCE: give the recipe in SOURCE mke2fs's default group size;
# with RACH2_FAULT_TEXTS set, only check its text and write nothing.
apply_fault()
{
    python3 - "$1" <<'PY'
import pathlib
import sys
import os
if os.environ.get('RACH2_FAULT_TEXTS'):
    pathlib.Path.write_text = lambda self, *args, **kwargs: 0

path = pathlib.Path(sys.argv[1])
text = path.read_text()
old = 'mkext2.sh krffs/manifests/rach2-metal-disk.manifest $@ 64 16384 32768\n'
if text.count(old) != 1:
    raise SystemExit('rehearse-group-mutation: the recipe pattern is not unique')
path.write_text(text.replace(old, 'mkext2.sh krffs/manifests/rach2-metal-disk.manifest $@ 64 32768 32768\n'))
PY
}

if [ "${1:-}" = --texts ]; then
    RACH2_FAULT_TEXTS=1
    export RACH2_FAULT_TEXTS
    apply_fault tests/system/suites.mk >/dev/null
    echo 'rehearse-group-mutation: texts: every fault is once in its source'
    exit 0
fi


. "$repo/tests/system/runlock.sh"
runlock_hold "$repo" rehearse-group-mutation "$repo/tests/system/rehearse_group_mutate.sh" "$@"
RACH2_ARCH=${RACH2_ARCH:-${ARCH:-i686}}
REPO=$repo
. "$repo/mk/arch.sh"
RACH2_QEMU=$ARCH_QEMU
export RACH2_QEMU ARCH_BUILD
case $# in
    0) root=$ARCH_BUILD/images/installer-root.ext2 disk=$ARCH_BUILD/images/installer-test.disk ;;
    2) root=$1 disk=$2 ;;
    *) echo 'rehearse-group-mutation: usage: rehearse_group_mutate.sh [ROOT DISK]' >&2; exit 2 ;;
esac
growset=$repo/build/tools/inspect/growset
for artifact in "$growset" "$root" "$disk" build/krffs/stageimage.krffs; do
    if [ ! -f "$artifact" ]; then
        echo "rehearse-group-mutation: missing $artifact; run tests/run installer-rehearse first" >&2
        exit 1
    fi
done
work=$(mktemp -d "${TMPDIR:-/tmp}/rach2-rehearse-group.XXXXXX")
cp tests/system/suites.mk "$work/suites.mk.saved"
cp "$work/suites.mk.saved" "$work/suites.mk"
child=

# Stop the running suite and wait for it.
stop_child()
{
    if [ -n "$child" ]; then
        kill -TERM "$child" 2>/dev/null || :
        wait "$child" 2>/dev/null || :
        child=
    fi
}

# Stop the suite and put back the private suites.mk.
cleanup()
{
    stop_child
    cp "$work/suites.mk.saved" "$work/suites.mk"
    touch "$work/suites.mk"
}
trap cleanup EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

# The arguments after `$@` in the installer root's recipe.
recipe_args()
{
    python3 - "$work/suites.mk" <<'PY'
import pathlib
import sys

lines = pathlib.Path(sys.argv[1]).read_text().splitlines()
start = next(i for i, line in enumerate(lines) if line.startswith('$(INSTALLER_EXT2):'))
recipe = next(line for line in lines[start:] if line.startswith('\tsh tools/image/mkext2.sh '))
words = recipe.split()
print(' '.join(words[words.index('$@') + 1:]))
PY
}

# The ext2 superblock's blocks per group, at byte 32 of the superblock.
blocks_per_group()
{
    od -A n -t u4 -j 1056 -N 4 "$1" | tr -d ' '
}

# The rehearsal's own save-set computation on a root, with the small form's
# disk of 524288 sectors, whose last 33 hold the GUID table's backup, and the
# installer disk's root offset.
save_set()
{
    "$growset" --lba "$root_start" "$1" $((524288 - 33 - root_start)) >"$2" 2>&1
}

# build_root PHASE: build the root from the private recipe as PHASE.ext2.
build_root()
{
    phase=$1
    # shellcheck disable=SC2046
    sh tools/image/mkext2.sh krffs/manifests/rach2-metal-disk.manifest "$work/$phase.ext2" \
        $(recipe_args) >"$work/$phase.build.log" 2>&1 || {
        cat "$work/$phase.build.log" >&2
        exit 1
    }
    cksum "$work/suites.mk" "$work/$phase.ext2" >"$work/$phase.cksum"
    echo "rehearse-group-mutation: $phase: recipe arguments $(recipe_args)," \
        "blocks per group $(blocks_per_group "$work/$phase.ext2")"
}

# The root's first sector, the disk's third entry, read as the installer
# suite reads it: a GUID table by mkgpt, which the protective MBR's type 0xEE
# says the disk has, and an MBR by mkmbr; both list the extent in bytes.
if [ "$(od -An -tx1 -j 450 -N 1 "$disk" | tr -d ' ')" = ee ]; then
    "$repo/build/tools/image/mkgpt" --list "$disk"
else
    "$repo/build/tools/image/mkmbr" --list "$disk"
fi > "$work/entries" 2>&1
root_start=$(awk '$1 == 3 { printf "%d\n", $3 / 512; exit }' "$work/entries")
case "$root_start" in
    '' | 0 | *[!0-9]*) echo 'rehearse-group-mutation: the installer disk has no third entry' >&2; exit 1 ;;
esac
echo "rehearse-group-mutation: logs: $work"

build_root fixed
if ! cmp -s "$work/fixed.ext2" "$root"; then
    echo "rehearse-group-mutation: the recipe no longer builds $root;" \
        "run tests/run installer-rehearse to rebuild it" >&2
    exit 1
fi
[ "$(blocks_per_group "$work/fixed.ext2")" = 16384 ]
save_set "$work/fixed.ext2" "$work/fixed.set"
echo 'rehearse-group-mutation: result: fixed: growset computed the save set'

apply_fault "$work/suites.mk"
build_root mutated
[ "$(blocks_per_group "$work/mutated.ext2")" = 32768 ]
if save_set "$work/mutated.ext2" "$work/mutated.set"; then
    echo 'rehearse-group-mutation: growset took a root whose first group is partial' >&2
    exit 1
fi
cp "$disk" "$work/mutated.disk"
dd if="$work/mutated.ext2" of="$work/mutated.disk" bs=512 seek="$root_start" conv=notrunc \
    2>/dev/null
status=0
sh tests/system/suites/installer.sh "$work/unused-stick.img" "$work/unused-sealed.disk" \
    "$work/unused-harness" "$work/installer" unused "$work/unused-test-stick.img" \
    "$work/mutated.disk" 0 "$work/unused-ptydrive" "$work/unused-key" rehearse \
    >"$work/mutated.log" 2>&1 &
child=$!
wait "$child" || status=$?
child=
cat "$work/mutated.log"
if [ "$status" -eq 0 ] ||
    ! grep -Fq 'installer-test: FAIL: growset did not compute the save set' "$work/mutated.log"; then
    echo "rehearse-group-mutation: the rehearsal did not stop at its setup (status $status)" >&2
    exit 1
fi
for written in base.raw disk.qcow2 save-stick.img product-stick.img; do
    if [ -e "$work/installer/rehearse/$written" ]; then
        echo "rehearse-group-mutation: the rehearsal wrote $written before its setup failed" >&2
        exit 1
    fi
done
echo "rehearse-group-mutation: result: mutated: setup failed with status $status before any write"

cleanup
build_root restored
cmp "$work/fixed.ext2" "$work/restored.ext2"
save_set "$work/restored.ext2" "$work/restored.set"
echo 'rehearse-group-mutation: result: pass: the old group size failed the setup; the restored recipe rebuilt the first root'
