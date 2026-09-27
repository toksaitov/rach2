#!/bin/sh
# Compile whole production functions with counted filesystem substitutes.
set -eu
repo=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
work=$(mktemp -d "${TMPDIR:-/tmp}/rach2-unmount.XXXXXX")
echo "vfs-unmount: logs: $work"
[ "$#" -eq 0 ] || [ "$*" = --mutate ] || exit 2
cp "$repo/rach2/sys/fs/vfs.c" "$work/saved.c"
cp "$work/saved.c" "$work/vfs.c"
trap 'cp "$work/saved.c" "$work/vfs.c"' EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

run_phase()
{
    awk '
        /^(static int streq|int vfs_mount_gone|static int mount_permanent|static void unmount_abandoned|static struct vfs_mount \*unmount_below_gone|int vfs_do_umount)\(/ {copy = 1}
        copy {print}
        copy && /^}/ {copy = 0}
    ' "$work/vfs.c" > "$work/unmount.inc"
    "${CC:-cc}" -std=c99 -O1 -g -Wall -Wextra -Werror -I "$work" \
        -fsanitize=address,undefined -fno-sanitize-recover=all \
        "$repo/tests/system/fixtures/vfs_unmount.c" -o "$work/$1"
    "$work/$1" > "$work/$1.log" 2>&1
}

run_phase fixed
cat "$work/fixed.log"
if [ "$#" -ne 0 ]; then
    sed 's/m = rc == -EIO ?/m = rc == -EIO \&\& 0 ?/' "$work/saved.c" > "$work/vfs.c"
    status=0
    run_phase mutated || status=$?
    [ "$status" -eq 1 ]
    grep -q 'failed: live child below removed parent unmounts' "$work/mutated.log"
    echo 'vfs-unmount: result: missing recovery lookup detected'
    cp "$work/saved.c" "$work/vfs.c"
    run_phase restored
    cat "$work/restored.log"
fi
