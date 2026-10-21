#!/bin/sh
# rehearse_group_test.sh ROOT REFERENCE DISK: check that an installer root
# built by the recipe in tests/system/suites.mk is one the installer
# rehearsal can save from, and that the rehearsal stops at its setup on one
# that is not.
#
# ROOT must equal REFERENCE, the installer root the rehearsal runs, have
# 16384 blocks to an ext2 group, so that its 64 MiB are one whole group, and
# give growset a save set for the small form's disk of 524288 sectors with
# the root at DISK's third entry. A root growset refuses is written into a
# copy of DISK, and the rehearsal's fixture, tests/cases/machine/
# rehearse_fixture.sh, made from that copy must fail before it writes the
# save disk or the base disk. The status is 0 when every check passes, 1 when
# one fails, 2 for a usage error and 3 when the fixture goes past the save
# set on a root growset refused. The scratch is under build/tests and is
# removed on every exit.
set -eu
[ $# -eq 3 ] || { echo 'rehearse-group: usage: rehearse_group_test.sh ROOT REFERENCE DISK' >&2; exit 2; }
root=$1
reference=$2
disk=$3
repo=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
cd "$repo"
growset=build/tools/inspect/growset
work=
trap '[ -z "$work" ] || rm -rf "$work"' EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM
mkdir -p build/tests
work=$(mktemp -d "$repo/build/tests/rehearse-group.XXXXXX")
failed=0

# Report $1 as a failed check.
fail()
{
    echo "rehearse-group: FAIL: $1" >&2
    failed=1
}

# The root's first sector, the disk's third entry, read as the installer
# suite reads it: a GUID table by mkgpt, which the protective MBR's type 0xEE
# says the disk has, and an MBR by mkmbr; both list the extent in bytes.
if [ "$(od -An -tx1 -j 450 -N 1 "$disk" | tr -d ' ')" = ee ]; then
    build/tools/image/mkgpt --list "$disk"
else
    build/tools/image/mkmbr --list "$disk"
fi > "$work/entries" 2>&1
root_start=$(awk '$1 == 3 { printf "%d\n", $3 / 512; exit }' "$work/entries")
case "$root_start" in
    '' | 0 | *[!0-9]*) echo "rehearse-group: $disk has no third entry" >&2; exit 2 ;;
esac

cmp -s "$root" "$reference" || fail "$root differs from $reference"
# The superblock's blocks per group, at byte 32 of the superblock.
groups=$(od -A n -t u4 -j 1056 -N 4 "$root" | tr -d ' ')
[ "$groups" = 16384 ] || fail "$root has $groups blocks per group, not 16384"
if "$growset" --lba "$root_start" "$root" $((524288 - 33 - root_start)) >"$work/save.set" 2>&1; then
    echo 'rehearse-group: growset computed the save set'
    exit "$failed"
fi
cat "$work/save.set"
fail 'growset did not compute the save set'

cp "$disk" "$work/root.disk"
dd if="$root" of="$work/root.disk" bs=512 seek="$root_start" conv=notrunc 2>/dev/null
status=0
sh tests/cases/machine/rehearse_fixture.sh "$work/root.disk" 524288 "$growset" \
    build/tests/host/mkexfatfixture build/tools/image/mkmbr build/tools/image/mkgpt \
    "$work/fixture" > "$work/fixture.log" 2>&1 || status=$?
if [ "$status" -eq 0 ]; then
    echo "rehearse-group: the rehearsal's fixture was made on a root growset refused" >&2
    exit 3
fi
for written in ranges save.img base.raw; do
    if [ -e "$work/fixture/$written" ]; then
        echo "rehearse-group: the rehearsal's fixture wrote $written past the refused save set" >&2
        exit 3
    fi
done
echo "rehearse-group: the rehearsal's fixture stopped at the save set with status $status"
exit 1
