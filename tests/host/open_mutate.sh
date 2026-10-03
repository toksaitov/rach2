#!/bin/sh
# Compile mounted open cleanup, the errors of an open, a dup and a dup2 that
# get no descriptor, the KRFFS root's unlink and rename under the root's lock
# and of a program's file, the calls by path that read or change a node under
# the lock their walk takes, the key of a file's locks, a mkdir's lookup under
# the lock of its parent, an orphan's locks dropped before it is freed, an
# rmdir on the KRFFS root of a directory a description or a working directory
# has, a chdir's pin of a directory on a type that keeps a node only while it
# is open, a read and a write of a /proc file, and a read of /dev/bootlog,
# that hold the description while a sharer's seek waits, the log's seeks
# checked past the offsets they can return, a sum past the largest signed
# one among them, and the release of the last
# holder of a nameless file
# of the KRFFS root under the root's lock, by a descriptor or a file mapping,
# before, during and after each fault.
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
cp "$work/sys/mm/filemap.c" "$work/filemap.saved"

cleanup()
{
    if [ -n "$child" ]; then
        kill -TERM "$child" 2>/dev/null || :
        wait "$child" 2>/dev/null || :
        child=
    fi
    cp "$work/open.saved" "$work/sys/syscall_fs.c"
    cp "$work/loop.saved" "$work/sys/block/loop.c"
    cp "$work/filemap.saved" "$work/sys/mm/filemap.c"
    touch "$work/sys/syscall_fs.c" "$work/sys/block/loop.c" "$work/sys/mm/filemap.c"
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
        tests/host/open_hosttest.c "$work/sys/fs/fd.c" "$work/sys/lib/kstring.c" \
        "$work/sys/lib/kctypes.c" tests/host/compat/park_host.c \
        -o "$binary" >"$binary.compile.log" 2>&1; then
        cat "$binary.compile.log" >&2
        exit 1
    fi
    cksum "$work/sys/syscall_fs.c" "$work/sys/block/loop.c" "$work/sys/mm/filemap.c" "$binary" \
        >"$binary.cksum"
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
    open-emfile dup-enomem dup2-enomem program-holder program-reclaim-unlocked root-lock-key \
    volume-lock-key fat-lock-key mkdir-walk-unlocked orphan-locks-after rmdir-checked-unlocked \
    rmdir-descriptions-ignored rmdir-tables-only rmdir-cwd-ignored rmdir-cwd-caller-only \
    chdir-unpinned chdir-walk-unlocked chdir-unpinned-before-record proc-read-unheld \
    proc-write-unheld orphan-release-unlocked mapping-left-unlocked log-read-unheld \
    log-seek-unlocked log-seek-wraps log-seek-past-signed; do
    source="$work/sys/syscall_fs.c"
    [ "$fault" != loop ] || source="$work/sys/block/loop.c"
    [ "$fault" != mapping-left-unlocked ] || source="$work/sys/mm/filemap.c"
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
         "    /* rename is within one mount;",
         "    if (rc != 0) {\n        vfs_walk_unlock(opar.mnt);\n        vfs_ref_release(&opar);\n"
         "        return rc;\n    }\n    /* rename is within one mount;"),
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
    "program-reclaim-unlocked": ("        fd_release_objects(&e);\n        vfs_ns_unlock(locked);\n",
                                 "        vfs_ns_unlock(locked);\n        fd_release_objects(&e);\n"),
    "rename-relocked": ("    if (npar->mnt->type->ops->lookup(npar->mnt, npar->node, nleaf, &found) == 0) {",
                        "    vfs_ns_unlock(opar->mnt);\n    vfs_ns_lock(opar->mnt);\n"
                        "    if (npar->mnt->type->ops->lookup(npar->mnt, npar->node, nleaf, &found) == 0) {"),
    "root-lock-key": ("    return mnt->type == &vfs_krffs_type || mnt->type == &vfs_krffs_volume_type ||",
                      "    return mnt->type == &vfs_krffs_volume_type ||"),
    "volume-lock-key": ("    return mnt->type == &vfs_krffs_type || mnt->type == &vfs_krffs_volume_type ||",
                        "    return mnt->type == &vfs_krffs_type ||"),
    "fat-lock-key": ("    return mnt->type == &vfs_krffs_type || mnt->type == &vfs_krffs_volume_type ||\n"
                     "           mnt->type == &vfs_vfat_type || mnt->type == &vfs_exfat_type;",
                     "    return mnt->type == &vfs_krffs_type || mnt->type == &vfs_krffs_volume_type;"),
    "orphan-locks-after": ("    if (e->ofd && e->ofd->refcount == 1 && lock_any()) {\n"
                           "        lock_ofd_gone(e->ofd);\n    }\n"
                           "    krffs_remove_reserved_node(krffs_fs, node);\n",
                           "    (void)e;\n    krffs_remove_reserved_node(krffs_fs, node);\n"),
    "mkdir-walk-unlocked": [
        ("rc = vfs_resolve(0, pathbuf, VFS_WALK_PARENT | VFS_WALK_LOCK, &parent, leaf, sizeof(leaf));\n"
         "    if (rc != 0) {\n        return rc; /* a missing parent, or a denied ancestor */",
         "rc = vfs_resolve(0, pathbuf, VFS_WALK_PARENT, &parent, leaf, sizeof(leaf));\n"
         "    if (rc != 0) {\n        return rc; /* a missing parent, or a denied ancestor */"),
        ("    rc = leaf_is_dot(leaf) ? -EEXIST : mkdir_in(&parent, leaf, mode, pathbuf);\n"
         "    vfs_walk_unlock(parent.mnt);\n",
         "    rc = leaf_is_dot(leaf) ? -EEXIST : mkdir_in(&parent, leaf, mode, pathbuf);\n"),
    ],
    "rmdir-checked-unlocked": [
        ("    rc = vfs_resolve(0, pathbuf, VFS_WALK_PARENT | VFS_WALK_LOCK, &parent, leaf, sizeof(leaf));\n"
         "    if (rc != 0) {\n        return rc;\n    }\n    if (leaf_is_dot(leaf)) {\n"
         "        rc = leaf[1] == '\\0' ? -EINVAL : -ENOTEMPTY;",
         "    rc = vfs_resolve(0, pathbuf, VFS_WALK_PARENT, &parent, leaf, sizeof(leaf));\n"
         "    if (rc != 0) {\n        return rc;\n    }\n    if (leaf_is_dot(leaf)) {\n"
         "        rc = leaf[1] == '\\0' ? -EINVAL : -ENOTEMPTY;"),
        ("        vfs_ns_unlock(parent.mnt);\n    }\n    vfs_walk_unlock(parent.mnt);\n"
         "    vfs_ref_release(&parent);\n    return rc;\n}\n\n/* rmdir(2) takes path. */",
         "        vfs_ns_unlock(parent.mnt);\n    }\n"
         "    vfs_ref_release(&parent);\n    return rc;\n}\n\n/* rmdir(2) takes path. */"),
        ("    return rc != 0 ? rc : vfs_rmdir(parent->mnt, parent->node, leaf);",
         "    if (rc == 0) {\n        vfs_ns_lock(parent->mnt);\n"
         "        rc = vfs_rmdir(parent->mnt, parent->node, leaf);\n"
         "        vfs_ns_unlock(parent->mnt);\n    }\n    return rc;"),
    ],
    "rmdir-descriptions-ignored": ("    if (fd_ofd_holders(mnt, dir, NULL) > 0) {\n        return 1;\n    }\n", ""),
    "rmdir-tables-only": ("    if (fd_ofd_holders(mnt, dir, NULL) > 0) {\n        return 1;\n    }\n",
                          "    for (pr = proc_list; pr; pr = pr->next) {\n        int i;\n\n"
                          "        for (i = 0; i < fd_table_cap(pr->fds); i++) {\n"
                          "            if (pr->fds->entries[i].ofd && pr->fds->entries[i].ofd->vmnt == mnt &&\n"
                          "                pr->fds->entries[i].ofd->vnode == dir) {\n"
                          "                return 1;\n            }\n        }\n    }\n"),
    "rmdir-cwd-ignored": ("    for (pr = proc_list; pr; pr = pr->next) {\n"
                          "        if (pr->fs && pr->fs->cwd_mnt == mnt && pr->fs->cwd_node == dir) {\n"
                          "            return 1;\n        }\n    }\n",
                          "    (void)pr;\n"),
    "rmdir-cwd-caller-only": ("    for (pr = proc_list; pr; pr = pr->next) {\n"
                              "        if (pr->fs && pr->fs->cwd_mnt == mnt && pr->fs->cwd_node == dir) {",
                              "    for (pr = current_proc; pr; pr = NULL) {\n"
                              "        if (pr->fs && pr->fs->cwd_mnt == mnt && pr->fs->cwd_node == dir) {"),
    "chdir-unpinned": ("    if (!mnt_is_root(ref.mnt) && ref.mnt->type->ops->pin) {\n"
                       "        rc = ref.mnt->type->ops->pin(ref.mnt, ref.node, &pin);\n"
                       "    }\n    vfs_walk_unlock(ref.mnt);\n",
                       "    vfs_walk_unlock(ref.mnt);\n"),
    "chdir-walk-unlocked": [
        ("    rc = vfs_resolve(0, path, VFS_WALK_LOCK, &ref, 0, 0);\n    if (rc != 0) {\n"
         "        return rc;\n    }\n    if (!mnt_is_root(ref.mnt) && ref.mnt->type->ops->pin) {",
         "    rc = vfs_resolve(0, path, 0, &ref, 0, 0);\n    if (rc != 0) {\n"
         "        return rc;\n    }\n    if (!mnt_is_root(ref.mnt) && ref.mnt->type->ops->pin) {"),
        ("        rc = ref.mnt->type->ops->pin(ref.mnt, ref.node, &pin);\n"
         "    }\n    vfs_walk_unlock(ref.mnt);\n",
         "        rc = ref.mnt->type->ops->pin(ref.mnt, ref.node, &pin);\n    }\n"),
    ],
    "chdir-unpinned-before-record": ("        rc = chdir_mounted(&ref, path);\n        if (pin) {\n"
                                     "            ref.mnt->type->ops->unpin(ref.mnt, pin);\n        }\n",
                                     "        if (pin) {\n"
                                     "            ref.mnt->type->ops->unpin(ref.mnt, pin);\n        }\n"
                                     "        rc = chdir_mounted(&ref, path);\n"),
    "proc-read-unheld": ("static int32_t read_ops(fd_entry_t *entry, char __user *buf, uint32_t count)\n{\n"
                         "    struct open_file *held = ops_offset_held(entry);",
                         "static int32_t read_ops(fd_entry_t *entry, char __user *buf, uint32_t count)\n{\n"
                         "    struct open_file *held = devfs_fd_is_raw(entry) ? entry->ofd : NULL;"),
    "proc-write-unheld": ("static int32_t write_ops(fd_entry_t *entry, const char __user *buf, uint32_t count)\n{\n"
                          "    struct open_file *held = ops_offset_held(entry);",
                          "static int32_t write_ops(fd_entry_t *entry, const char __user *buf, uint32_t count)\n{\n"
                          "    struct open_file *held = devfs_fd_is_raw(entry) ? entry->ofd : NULL;"),
    "orphan-release-unlocked": [
        ("    if (!locked) {\n        return;\n    }\n    if (e->type == FD_TYPE_FILE) {\n",
         "    if (!locked) {\n        return;\n    }\n    vfs_ns_unlock(locked);\n"
         "    if (e->type == FD_TYPE_FILE) {\n"),
        ("        fd_fifo_orphan(e);\n    }\n    vfs_ns_unlock(locked);\n}\n",
         "        fd_fifo_orphan(e);\n    }\n}\n"),
    ],
    "log-read-unheld": ("    return devfs_fd_moves_offset(entry) || entry->type == FD_TYPE_PROC ? entry->ofd : NULL;\n",
                        "    return devfs_fd_is_raw(entry) || entry->type == FD_TYPE_PROC ? entry->ofd : NULL;\n"),
    "log-seek-unlocked": [
        ("    fd_ofd_lock(entry->ofd);\n    (void)devfs_fd_log_size(entry, &size);\n",
         "    (void)devfs_fd_log_size(entry, &size);\n"),
        ("    fd_ofd_unlock(entry->ofd);\n    *out = at;\n", "    *out = at;\n"),
    ],
    "log-seek-wraps": ("        rc = seek_add(entry->ofd->offset, off, &at);\n    } else if (whence == 2) {\n",
                       "        at = (int32_t)(uint32_t)(entry->ofd->offset + (uint64_t)off);\n"
                       "    } else if (whence == 2) {\n"),
    "log-seek-past-signed": ("    if (rc == -EOVERFLOW || (rc == 0 && (at < 0 || at > (long long)0xFFFFFFFFu))) {\n",
                             "    if (rc == 0 && (at < 0 || at > (long long)0xFFFFFFFFu)) {\n"),
    "mapping-left-unlocked": [
        ("    locked = m->entry.ofd ? fd_release_lock(&m->entry) : NULL;\n"
         "    while (*link && *link != m) {\n",
         "    while (*link && *link != m) {\n"),
        ("    if (*link) {\n        *link = m->next;\n    }\n    if (m->sh) {\n",
         "    if (*link) {\n        *link = m->next;\n    }\n"
         "    locked = m->entry.ofd ? fd_release_lock(&m->entry) : NULL;\n    if (m->sh) {\n"),
    ],
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
        program-reclaim-unlocked) message="a program's release counts the file's descriptor in the hold it took" ;;
        root-lock-key) message='a file of the KRFFS root is keyed by its node' ;;
        volume-lock-key) message='a file of a KRFFS volume is keyed by its node' ;;
        fat-lock-key) message='a file of a FAT32 volume is keyed by its node, which a rename keeps' ;;
        mkdir-walk-unlocked) message='a mkdir of a name another process made while it waited is EEXIST' ;;
        orphan-locks-after) message="a description's locks go before the orphan they are on is freed" ;;
        rmdir-checked-unlocked) message='an rmdir of a directory an open publishes a descriptor on while it waits is EBUSY' ;;
        rmdir-descriptions-ignored) message='an rmdir of a directory a descriptor has open is EBUSY' ;;
        rmdir-tables-only) message='an rmdir of a directory a description in flight has is EBUSY' ;;
        rmdir-cwd-ignored) message="an rmdir of the caller's working directory is EBUSY" ;;
        rmdir-cwd-caller-only) message="an rmdir of another process's working directory is EBUSY" ;;
        chdir-unpinned) message='the directory is pinned while the chdir sleeps before its record' ;;
        chdir-walk-unlocked) message='a chdir pins the node under the lock of the walk that found it' ;;
        chdir-unpinned-before-record) message='the directory is pinned when the working directory records it' ;;
        proc-read-unheld) message='a seek waits for a /proc read that sleeps in a page fetch' ;;
        proc-write-unheld) message='a seek waits for a /proc write that sleeps in a page fetch' ;;
        orphan-release-unlocked) message='an allocation on the root waits for the reclaim of a nameless file to end' ;;
        mapping-left-unlocked) message="a mapping counts among a file's holders until its release has the root's lock" ;;
        log-read-unheld) message='a seek waits for a /dev/bootlog read that sleeps in its copy' ;;
        log-seek-unlocked) message='an _llseek waits for a /dev/bootlog read that sleeps in its copy' ;;
        log-seek-wraps) message='an lseek of /dev/bootlog past 4294967295 is EINVAL and leaves the offset' ;;
        log-seek-past-signed) message='an _llseek of /dev/bootlog past the largest signed offset is EINVAL' ;;
    esac
    run_phase "$fault-mutated" 1 "$message"
    cleanup
    run_phase "$fault-restored" 0 'open-hosttest: result: pass:'
    cmp "$work/open.saved" "$work/sys/syscall_fs.c"
    cmp "$work/loop.saved" "$work/sys/block/loop.c"
    cmp "$work/filemap.saved" "$work/sys/mm/filemap.c"
done
