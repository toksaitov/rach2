#!/bin/sh
# Compile mounted open cleanup, the KRFFS root's unlink and rename under the
# root's lock, and the calls by path that read or change a node under the lock
# their walk takes, before, during and after each fault.
set -eu

repo=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
cd "$repo"
mkdir -p build/tests
work=$(mktemp -d "$repo/build/tests/open-proof.XXXXXX")
child=
cp -R rach2/sys "$work/sys"
cp rach2/kernel.h "$work/kernel.h"
mkdir "$work/vendor"
cp rach2/vendor/multiboot.h "$work/vendor/multiboot.h"
cp "$work/sys/syscall_fs.c" "$work/open.saved"
cp "$work/sys/block/loop.c" "$work/loop.saved"

cleanup()
{
    if [ -n "$child" ]; then
        kill -TERM "$child" 2>/dev/null || :
        wait "$child" 2>/dev/null || :
        child=
    fi
    cp "$work/open.saved" "$work/sys/syscall_fs.c"
    cp "$work/loop.saved" "$work/sys/block/loop.c"
    touch "$work/sys/syscall_fs.c" "$work/sys/block/loop.c"
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
    if ! "${CC:-cc}" -std=c99 -DRACH2_HOSTTEST -g -O1 -ffreestanding \
        -Wall -Wextra -Werror -Wno-unused-function -Wno-pointer-to-int-cast -Wno-int-to-pointer-cast \
        -fsanitize=address,undefined -fno-sanitize-recover=all \
        -I "$work/sys" -I "$work/sys/lib" -I "$work/sys/proc" \
        tests/host/open_hosttest.c "$work/sys/fs/fd.c" \
        "$work/sys/lib/kstring.c" "$work/sys/lib/kctypes.c" -o "$binary" >"$binary.compile.log" 2>&1; then
        cat "$binary.compile.log" >&2
        exit 1
    fi
    cksum "$work/sys/syscall_fs.c" "$work/sys/block/loop.c" "$binary" >"$binary.cksum"
    perl -e 'alarm 10; exec @ARGV or die "open-mutation: exec: $!\n"' \
        "$binary" >"$binary.log" 2>&1 &
    child=$!
    status=0
    wait "$child" || status=$?
    child=
    cat "$binary.log"
    [ "$status" -eq "$expected" ] && grep -Fq "$message" "$binary.log" || exit 1
    echo "open-mutation: result: $phase status $status"
}

echo "open-mutation: logs: $work"
run_phase fixed 0 'open-hosttest: result: pass:'
for fault in node release truncate loop registration unlink-lock rename-lock rename-unlocked \
    rename-locked-walk rename-relocked chmod-walk-unlocked chown-walk-unlocked \
    stat64-walk-unlocked old-stat-walk-unlocked fstatat-walk-unlocked access-walk-unlocked; do
    source="$work/sys/syscall_fs.c"
    [ "$fault" != loop ] || source="$work/sys/block/loop.c"
    python3 - "$source" "$fault" <<'PY'
import pathlib
import sys

path = pathlib.Path(sys.argv[1])
source = path.read_text()
changes = {
    "registration": ("    entry->ofd->vnode = node;", "    entry->ofd->vnode = NULL;"),
    "node": ("vfs_truncate(ref->mnt, entry->ofd->fhandle, 0)",
             "vfs_truncate(ref->mnt, node, 0)"),
    "release": ("            fd_free(current_proc->fds, fd);\n            return rc;",
                "            return rc;"),
    "truncate": ("if (!mnt_is_root(ref->mnt) && (flags & O_TRUNC)) {",
                 "if (0) {"),
    "loop": ("mnt->type == &vfs_krffs_type && loops[i].handle == from",
             "loops[i].handle == from"),
    "unlink-lock": ("vfs_resolve(0, path, VFS_WALK_PARENT | VFS_WALK_LOCK, &parent, leaf,",
                    "vfs_resolve(0, path, VFS_WALK_PARENT, &parent, leaf,"),
    "rename-lock": ("    vfs_ns_lock(opar.mnt);\n    rc = rename_root_names(&opar, oleaf, &npar, nleaf, flags);\n"
                    "    vfs_ns_unlock(opar.mnt);\n",
                    "    rc = rename_root_names(&opar, oleaf, &npar, nleaf, flags);\n"),
    "rename-unlocked": ("    vfs_ns_lock(opar.mnt);\n    rc = rename_root_names(&opar, oleaf, &npar, nleaf, flags);\n"
                        "    vfs_ns_unlock(opar.mnt);\n",
                        "    vfs_ns_lock(opar.mnt);\n    vfs_ns_unlock(opar.mnt);\n"
                        "    rc = rename_root_names(&opar, oleaf, &npar, nleaf, flags);\n"),
    "rename-locked-walk": [
        ("vfs_resolve(0, old_buf, VFS_WALK_PARENT, &opar, oleaf,",
         "vfs_resolve(0, old_buf, VFS_WALK_PARENT | VFS_WALK_LOCK, &opar, oleaf,"),
        ("    if (rc != 0) {\n        vfs_ref_release(&opar);\n        return rc;\n    }\n"
         "    rc = mnt_is_root(npar.mnt)",
         "    if (rc != 0) {\n        vfs_walk_unlock(opar.mnt);\n        vfs_ref_release(&opar);\n"
         "        return rc;\n    }\n    rc = mnt_is_root(npar.mnt)"),
        ("out:\n    vfs_ref_release(&npar);", "out:\n    vfs_walk_unlock(opar.mnt);\n    vfs_ref_release(&npar);"),
    ],
    "chmod-walk-unlocked": ("    rc = vfs_resolve(0, name, VFS_WALK_LOCK, &ref, 0, 0);\n    if (rc != 0) {\n        return rc;\n    }\n    svp = vfs_synth_vpath(ref.mnt, ref.node);\n    if (svp) {\n        vp = *svp;\n        vfs_walk_unlock(ref.mnt);\n        vfs_ref_release(&ref);\n        if (vp.kind == VNODE_PTS) {",
                            "    rc = vfs_resolve(0, name, 0, &ref, 0, 0);\n    if (rc != 0) {\n        return rc;\n    }\n    svp = vfs_synth_vpath(ref.mnt, ref.node);\n    if (svp) {\n        vp = *svp;\n        vfs_walk_unlock(ref.mnt);\n        vfs_ref_release(&ref);\n        if (vp.kind == VNODE_PTS) {"),
    "chown-walk-unlocked": ("(follow ? 0 : VFS_WALK_NOFOLLOW) | VFS_WALK_LOCK, &ref, 0, 0);",
                            "(follow ? 0 : VFS_WALK_NOFOLLOW), &ref, 0, 0);"),
    "stat64-walk-unlocked": ("        int rc =\n            vfs_resolve(0, pathbuf, VFS_WALK_LOCK | (follow ? 0 : VFS_WALK_NOFOLLOW), &ref, 0, 0);",
                             "        int rc = vfs_resolve(0, pathbuf, follow ? 0 : VFS_WALK_NOFOLLOW, &ref, 0, 0);"),
    "old-stat-walk-unlocked": ("        int32_t rc =\n            vfs_resolve(0, pathbuf, VFS_WALK_LOCK | (follow ? 0 : VFS_WALK_NOFOLLOW), &ref, 0, 0);",
                               "        int32_t rc = vfs_resolve(0, pathbuf, follow ? 0 : VFS_WALK_NOFOLLOW, &ref, 0, 0);"),
    "fstatat-walk-unlocked": ("    rc = vfs_resolve(0, path, VFS_WALK_LOCK | (follow ? 0 : VFS_WALK_NOFOLLOW), &ref, 0, 0);",
                              "    rc = vfs_resolve(0, path, follow ? 0 : VFS_WALK_NOFOLLOW, &ref, 0, 0);"),
    "access-walk-unlocked": ("vfs_resolve(0, name, VFS_WALK_REAL_IDS | VFS_WALK_LOCK, &ref, 0, 0);",
                             "vfs_resolve(0, name, VFS_WALK_REAL_IDS, &ref, 0, 0);"),
    "rename-relocked": ("    if (npar->mnt->type->ops->lookup(npar->mnt, npar->node, nleaf, &found) == 0) {",
                        "    vfs_ns_unlock(opar->mnt);\n    vfs_ns_lock(opar->mnt);\n"
                        "    if (npar->mnt->type->ops->lookup(npar->mnt, npar->node, nleaf, &found) == 0) {"),
}
edits = changes[sys.argv[2]]
for old, new in edits if isinstance(edits, list) else [edits]:
    if source.count(old) != 1:
        raise SystemExit("open-mutation: source pattern is not unique")
    source = source.replace(old, new)
path.write_text(source)
PY
    case $fault in
        node) message='mounted truncation uses a live open handle' ;;
        release) message='every open handle released' ;;
        truncate) message='only O_TRUNC truncates and clears set-id bits' ;;
        loop) message='mounted loop handle stays stable when node moves' ;;
        registration) message='close retains current node identity' ;;
        unlink-lock) message="the unlink keeps the file for the open's descriptor" ;;
        rename-lock) message="the rename keeps the file it replaces for the open's descriptor" ;;
        rename-unlocked) message="a lookup in the root runs under the root's lock" ;;
        rename-locked-walk) message='a walk starts with no namespace lock held' ;;
        rename-relocked) message='the lookups, the count and the change run under one hold of the root' ;;
        chmod-walk-unlocked | chown-walk-unlocked | stat64-walk-unlocked | old-stat-walk-unlocked | \
            fstatat-walk-unlocked | access-walk-unlocked) message='attributes use node identity' ;;
    esac
    run_phase "$fault-mutated" 1 "$message"
    cleanup
    run_phase "$fault-restored" 0 'open-hosttest: result: pass:'
    cmp "$work/open.saved" "$work/sys/syscall_fs.c"
    cmp "$work/loop.saved" "$work/sys/block/loop.c"
done
