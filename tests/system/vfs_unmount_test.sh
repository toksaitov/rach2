#!/bin/sh
# vfs_unmount_test.sh [--build VFS OUT]: compile the unmount functions of
# rach2/sys/fs/vfs.c, copied whole, with the counted file system substitutes
# of fixtures/vfs_unmount.c, and run its cases under the address and
# undefined-behavior sanitizers.
#
# `--build VFS OUT` compiles the functions of VFS, a copy of vfs.c, into the
# program OUT, with their copy beside it as unmount.inc, and runs nothing;
# tests/proofs/vfs-unmount.json builds so from a private copy with each of
# its faults in it, and runs OUT as its test.
set -eu
repo=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)

# build VFS OUT: copy the unmount functions out of VFS into unmount.inc
# beside OUT, and build the fixture with them as OUT.
build()
{
    dir=$(dirname -- "$2")
    awk '
        /^(static int streq|int vfs_mount_gone|static struct vfs_mount \*mount_base|static int mount_permanent|static void unmount_abandoned|static int unmount_target|static struct vfs_mount \*unmount_below_gone|static int unmount_found|int vfs_do_umount|static int unmount_bind)\(/ {copy = 1}
        /^static int unmount_mount\(.*\)$/ {copy = 1}
        copy {print}
        copy && /^}/ {copy = 0}
    ' "$1" > "$dir/unmount.inc"
    "${CC:-cc}" -std=c99 -O1 -g -Wall -Wextra -Werror -I "$dir" \
        -fsanitize=address,undefined -fno-sanitize-recover=all \
        "$repo/tests/system/fixtures/vfs_unmount.c" -o "$2"
}

case $# in
    0) ;;
    3) [ "$1" = --build ] || exit 2; build "$2" "$3"; exit 0 ;;
    *) echo 'usage: vfs_unmount_test.sh [--build VFS OUT]' >&2; exit 2 ;;
esac
. "$repo/tests/system/runlock.sh"
runlock_hold "$repo" vfs-unmount "$repo/tests/system/vfs_unmount_test.sh" "$@"
work=
trap '[ -z "$work" ] || rm -rf "$work"' EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM
work=$(mktemp -d "${TMPDIR:-/tmp}/rach2-unmount.XXXXXX")
build "$repo/rach2/sys/fs/vfs.c" "$work/vfs_unmount"
"$work/vfs_unmount"
