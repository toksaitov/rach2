#!/bin/sh
# vfs_unmount_test.sh [--mutate | --texts]: compile the unmount functions of
# rach2/sys/fs/vfs.c, copied whole, with the counted file system substitutes
# of fixtures/vfs_unmount.c, and run its cases under the address and
# undefined-behavior sanitizers.
#
# --mutate also runs four faults in a copy of vfs.c, each of which must fail
# its case: the lookup below a removed parent taken out of the unmount; a
# target compared as the caller wrote it rather than as named from the
# caller's root; a directory target not checked in the walk; and a mount
# point's attributes read for a directory target. The restored copy must
# pass. --texts checks that each fault is once in vfs.c.
set -eu
repo=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
if [ "${1:-}" != --texts ]; then
    . "$repo/tests/system/runlock.sh"
    runlock_hold "$repo" vfs-unmount "$repo/tests/system/vfs_unmount_test.sh" "$@"
fi
[ "$#" -eq 0 ] || [ "$*" = --mutate ] || [ "$*" = --texts ] || exit 2
if [ "$*" = --texts ]; then
    for line in 'm = rc == -EIO ?' 'streq(m->covered_path, want)' \
        'if (rc == 0 && !VFS_IS_DIR(st.mode)) {' 'if (dir && tref->node != tref->mnt->root) {'; do
        if [ "$(grep -Fc "$line" "$repo/rach2/sys/fs/vfs.c" 2>/dev/null)" != 1 ]; then
            echo "vfs-unmount: not once in vfs.c: $line" >&2
            exit 1
        fi
    done
    echo 'vfs-unmount: texts: every fault is once in its source'
    exit 0
fi
work=$(mktemp -d "${TMPDIR:-/tmp}/rach2-unmount.XXXXXX")
echo "vfs-unmount: logs: $work"
cp "$repo/rach2/sys/fs/vfs.c" "$work/saved.c"
cp "$work/saved.c" "$work/vfs.c"
trap 'cp "$work/saved.c" "$work/vfs.c"' EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

# run_phase NAME: copy the unmount functions out of the copy of vfs.c, build
# the fixture with them as NAME, and run it with its output in NAME.log.
run_phase()
{
    awk '
        /^(static int streq|int vfs_mount_gone|static struct vfs_mount \*mount_base|static int mount_permanent|static void unmount_abandoned|static int unmount_target|static struct vfs_mount \*unmount_below_gone|static int unmount_found|int vfs_do_umount|static int unmount_bind)\(/ {copy = 1}
        /^static int unmount_mount\(.*\)$/ {copy = 1}
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
    [ "$(grep -c 'streq(m->covered_path, want)' "$work/saved.c")" -eq 1 ]
    sed 's/streq(m->covered_path, want)/streq(m->covered_path, target)/' "$work/saved.c" \
        > "$work/vfs.c"
    status=0
    run_phase unrooted || status=$?
    [ "$status" -eq 1 ]
    grep -q "failed: a target named from the caller's root is found below it" "$work/unrooted.log"
    echo 'vfs-unmount: result: a target compared without the caller root detected'
    [ "$(grep -c 'if (rc == 0 && !VFS_IS_DIR(st.mode)) {' "$work/saved.c")" -eq 1 ]
    sed 's/if (rc == 0 \&\& !VFS_IS_DIR(st.mode)) {/if (rc == 0 \&\& 0) {/' \
        "$work/saved.c" > "$work/vfs.c"
    status=0
    run_phase unchecked || status=$?
    [ "$status" -eq 1 ]
    grep -q 'failed: a target named as a directory that reaches a file is ENOTDIR from the one walk' \
        "$work/unchecked.log"
    echo 'vfs-unmount: result: a directory target not checked in the walk detected'
    [ "$(grep -c 'if (dir && tref->node != tref->mnt->root) {' "$work/saved.c")" -eq 1 ]
    sed 's/if (dir \&\& tref->node != tref->mnt->root) {/if (dir) {/' "$work/saved.c" \
        > "$work/vfs.c"
    status=0
    run_phase rootread || status=$?
    [ "$status" -eq 1 ]
    grep -q "failed: a target named as a directory that reaches a removed device's mount point unmounts it" \
        "$work/rootread.log"
    echo "vfs-unmount: result: a mount point's attributes read for a directory target detected"
    cp "$work/saved.c" "$work/vfs.c"
    run_phase restored
    cat "$work/restored.log"
fi
