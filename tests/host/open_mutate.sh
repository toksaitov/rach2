#!/bin/sh
# Compile mounted open cleanup, the errors of an open, a dup and a dup2 that
# get no descriptor, the KRFFS root's unlink and rename under the root's lock
# and of a program's file, and the calls by path that read or change a node
# under the lock their walk takes, before, during and after each fault.
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
    stat64-walk-unlocked old-stat-walk-unlocked fstatat-walk-unlocked access-walk-unlocked \
    utimensat-walk-unlocked utimensat-fd-node-unlocked open-reserves-after-walk open-enomem \
    open-emfile dup-enomem dup2-enomem program-holder; do
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
    "rename-lock": ("    vfs_ns_lock(opar.mnt);\n    rc = rename_root_names(&opar, oleaf, &npar, nleaf, flags, slash);\n"
                    "    vfs_ns_unlock(opar.mnt);\n",
                    "    rc = rename_root_names(&opar, oleaf, &npar, nleaf, flags, slash);\n"),
    "rename-unlocked": ("    vfs_ns_lock(opar.mnt);\n    rc = rename_root_names(&opar, oleaf, &npar, nleaf, flags, slash);\n"
                        "    vfs_ns_unlock(opar.mnt);\n",
                        "    vfs_ns_lock(opar.mnt);\n    vfs_ns_unlock(opar.mnt);\n"
                        "    rc = rename_root_names(&opar, oleaf, &npar, nleaf, flags, slash);\n"),
    "rename-locked-walk": [
        ("vfs_resolve(0, old_buf, VFS_WALK_PARENT, &opar, oleaf,",
         "vfs_resolve(0, old_buf, VFS_WALK_PARENT | VFS_WALK_LOCK, &opar, oleaf,"),
        ("    if (rc != 0) {\n        vfs_ref_release(&opar);\n        return rc;\n    }\n"
         "    rc = mnt_is_root(npar.mnt)",
         "    if (rc != 0) {\n        vfs_walk_unlock(opar.mnt);\n        vfs_ref_release(&opar);\n"
         "        return rc;\n    }\n    rc = mnt_is_root(npar.mnt)"),
        ("out:\n    vfs_ref_release(&npar);", "out:\n    vfs_walk_unlock(opar.mnt);\n    vfs_ref_release(&npar);"),
    ],
    "chmod-walk-unlocked": ("    rc = resolve_named(name, VFS_WALK_LOCK, dir, &ref);", "    rc = resolve_named(name, 0, dir, &ref);"),
    "chown-walk-unlocked": ("(follow ? 0 : VFS_WALK_NOFOLLOW) | VFS_WALK_LOCK, dir, &ref);",
                            "(follow ? 0 : VFS_WALK_NOFOLLOW), dir, &ref);"),
    "stat64-walk-unlocked": ("        int rc =\n            vfs_resolve(0, pathbuf, VFS_WALK_LOCK | (follow ? 0 : VFS_WALK_NOFOLLOW), &ref, 0, 0);",
                             "        int rc = vfs_resolve(0, pathbuf, follow ? 0 : VFS_WALK_NOFOLLOW, &ref, 0, 0);"),
    "old-stat-walk-unlocked": ("        int32_t rc =\n            vfs_resolve(0, pathbuf, VFS_WALK_LOCK | (follow ? 0 : VFS_WALK_NOFOLLOW), &ref, 0, 0);",
                               "        int32_t rc = vfs_resolve(0, pathbuf, follow ? 0 : VFS_WALK_NOFOLLOW, &ref, 0, 0);"),
    "fstatat-walk-unlocked": ("    rc = vfs_resolve(0, path, VFS_WALK_LOCK | (follow ? 0 : VFS_WALK_NOFOLLOW), &ref, 0, 0);",
                              "    rc = vfs_resolve(0, path, follow ? 0 : VFS_WALK_NOFOLLOW, &ref, 0, 0);"),
    "access-walk-unlocked": ("resolve_named(name, VFS_WALK_REAL_IDS | VFS_WALK_LOCK, dir, &ref);",
                             "resolve_named(name, VFS_WALK_REAL_IDS, dir, &ref);"),
    "utimensat-walk-unlocked": ("((flags & AT_SYMLINK_NOFOLLOW) ? VFS_WALK_NOFOLLOW : 0) | VFS_WALK_LOCK, dir, &ref);",
                                "((flags & AT_SYMLINK_NOFOLLOW) ? VFS_WALK_NOFOLLOW : 0), dir, &ref);"),
    "utimensat-fd-node-unlocked": ("    mnt = entry->ofd->vmnt;\n    vfs_ns_lock(mnt);\n"
                                   "    if (entry->node && fd_node_check(entry) != 0) {\n"
                                   "        rc = -EBADF;\n    } else {\n"
                                   "        rc = utimes_node(mnt, entry->ofd->vnode, times);\n    }\n"
                                   "    vfs_ns_unlock(mnt);\n    return rc;",
                                   "    (void)mnt;\n    (void)rc;\n"
                                   "    if (entry->node && fd_node_check(entry) != 0) {\n"
                                   "        return -EBADF;\n    }\n"
                                   "    return utimes_node(entry->ofd->vmnt, entry->ofd->vnode, times);"),
    "open-reserves-after-walk": [
        ("    fd = fd_alloc(current_proc->fds);\n    if (fd < 0) {\n        return fd;\n    }\nagain:\n",
         "    if (!fd_can_alloc(current_proc->fds)) {\n        return -EMFILE;\n    }\n    fd = -1;\nagain:\n"),
        ("                     &ref, 0, 0);\n    if (rc == -ENOENT && (flags & O_CREAT)) {",
         "                     &ref, 0, 0);\n    if (rc == 0 && fd < 0) {\n"
         "        fd = fd_alloc(current_proc->fds);\n        if (fd < 0) {\n"
         "            vfs_walk_unlock(ref.mnt);\n            vfs_ref_release(&ref);\n"
         "            return -EMFILE;\n        }\n    }\n"
         "    if (rc == -ENOENT && (flags & O_CREAT)) {"),
    ],
    "open-enomem": ("    if (fd < 0) {\n        return fd;\n    }\nagain:",
                    "    if (fd < 0) {\n        return -EMFILE;\n    }\nagain:"),
    "open-emfile": ("    if (fd < 0) {\n        return fd;\n    }\nagain:",
                    "    if (fd < 0) {\n        return -ENOMEM;\n    }\nagain:"),
    "dup-enomem": ("    newfd = fd_alloc_from(current_proc->fds, 0);\n    if (newfd < 0) {\n        return newfd;",
                   "    newfd = fd_alloc_from(current_proc->fds, 0);\n    if (newfd < 0) {\n        return -EMFILE;"),
    "dup2-enomem": ("    rc = fd_table_reach(current_proc->fds, newfd);\n    if (rc != 0) {\n        return rc;",
                    "    rc = fd_table_reach(current_proc->fds, newfd);\n    if (rc != 0) {\n        return -EMFILE;"),
    "program-holder": ("    if (unix_inflight_holds(node) || filemap_holds(node) || "
                       "mm_exe_holds(vfs_root_mount(), node)) {",
                       "    if (unix_inflight_holds(node) || filemap_holds(node)) {"),
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
            fstatat-walk-unlocked | access-walk-unlocked | utimensat-walk-unlocked | \
            utimensat-fd-node-unlocked) message='attributes use node identity' ;;
        open-reserves-after-walk) message='an open that truncates keeps the slot it reserved before its walk' ;;
        open-enomem) message='an open the heap has no room for is ENOMEM' ;;
        open-emfile) message='an open at the descriptor limit is EMFILE' ;;
        dup-enomem) message='a dup the heap has no room for is ENOMEM' ;;
        dup2-enomem) message='a dup2 past the end of a table the heap cannot grow is ENOMEM' ;;
        program-holder) message='an unlink keeps the file a program runs from' ;;
    esac
    run_phase "$fault-mutated" 1 "$message"
    cleanup
    run_phase "$fault-restored" 0 'open-hosttest: result: pass:'
    cmp "$work/open.saved" "$work/sys/syscall_fs.c"
    cmp "$work/loop.saved" "$work/sys/block/loop.c"
done
