/*
 * ext2_lock_rows.h: rows of ext2_hosttest.c, which includes this file after
 * ext2_batch_lifecycle.h, for what rach2/sys/fs/ext2.c does under its node
 * locks while the mount's lock still covers it: a user page gone at the
 * copy of a read or a write, a directory's removal against an entry made
 * in it, and the checks a rename makes before it locks a node.
 */
#ifndef EXT2_LOCK_ROWS_H_
#define EXT2_LOCK_ROWS_H_

/** The free inodes the mount counts, or 0 when statfs fails. */
static uint64_t lock_free_inodes(void)
{
    struct vfs_statfs st;

    return OPS->statfs(&mnt, &st) == 0 ? st.ffree : 0;
}

/** The inode number of `node`, or 0. */
static uint64_t lock_ino(void *node)
{
    struct vfs_stat st;

    return node && OPS->getattr(&mnt, node, &st) == 0 ? st.ino : 0;
}

/** The link count of `node`, or -1. */
static long lock_nlink(void *node)
{
    struct vfs_stat st;

    return node && OPS->getattr(&mnt, node, &st) == 0 ? (long)st.nlink : -1;
}

/** Make the bytes `at` to `at + len` of a user buffer absent, as a page
 * reclaimed before the copy is. */
static void lock_gone(const void *at, uint32_t len)
{
    gone_from = (uintptr_t)at;
    gone_to = gone_from + len;
    prefaults = 0;
}

/**
 * A page of the buffer that is absent at the copy is brought in and the
 * call goes on when the caller holds no file lock, and ends the call short
 * at that piece when it holds one, which it would have to give back for the
 * fault. A file lock is played by `file_locks`, since the mount's lock
 * still covers the driver here.
 */
static void user_copy_row(void)
{
    static unsigned char back[12000];
    void *node, *handle = 0;
    int rc;

    fill_pattern();
    wbytes = build_image(wimg, 4096, 128, PROFILE, 16, "tree");
    use_image(wimg, wbytes);
    check(mount_rw() == 0, "the user-copy volume mounts writable");
    node = make_file(mnt.root, "copied", &rc);
    check(node && OPS->open(&mnt, node, 2, &handle) == 0, "the user-copy file opens");
    if (!node || !handle) {
        return;
    }
    lock_gone(pattern + 5000, 1);
    check(OPS->write(&mnt, handle, pattern, 12000, 0) == 12000 && prefaults == 1,
          "a write brings in a page gone before its copy and writes every byte");
    lock_gone(back + 6000, 1);
    check(OPS->read(&mnt, handle, back, 12000, 0) == 12000 && prefaults == 1 &&
              memcmp(back, pattern, 12000) == 0,
          "a read brings in a page gone before its copy and reads every byte");
    proc_a.file_locks = 1;
    lock_gone(pattern + 100 + 5000, 1);
    check(OPS->write(&mnt, handle, pattern + 100, 12000, 0) == 4096 && prefaults == 0,
          "a write under a file lock ends short at the piece whose page went");
    check(OPS->write(&mnt, handle, pattern + 100 + 4096, 4000, 4096) == 0 && prefaults == 0,
          "and writes nothing when the first piece's page went");
    gone_from = gone_to = 0;
    check(OPS->write(&mnt, handle, pattern + 100 + 4096, 7904, 4096) == 7904,
          "and the rest goes once the page is back");
    lock_gone(back + 9000, 1);
    check(OPS->read(&mnt, handle, back, 12000, 0) == 8192 && prefaults == 0,
          "a read under a file lock ends short at the piece whose page went");
    proc_a.file_locks = 0;
    gone_from = gone_to = 0;
    check(OPS->read(&mnt, handle, back, 12000, 0) == 12000 &&
              memcmp(back, pattern + 100, 12000) == 0,
          "and the file holds each byte once, where it was written");
    OPS->release(&mnt, handle);
    OPS->put(&mnt, node);
    check(unmount_rw() == 0 && e2fsck_clean(wimg), "the user-copy volume is clean");
}

/**
 * An rmdir that comes first leaves a directory, still held, that takes no
 * entry of any kind and allocates no inode for one; a create that comes
 * first leaves the rmdir ENOTEMPTY.
 */
static void removal_race_row(void)
{
    uint64_t before;
    void *d, *made = 0, *file;
    int err, rc;

    wbytes = build_image(wimg, 4096, 128, PROFILE, 16, "tree");
    use_image(wimg, wbytes);
    check(mount_rw() == 0, "the removal volume mounts writable");
    file = make_file(mnt.root, "linked", &rc);
    d = OPS->mkdir(&mnt, mnt.root, "doomed", 0755) == 0 ? walk("/doomed", &err) : 0;
    check(d && file, "a directory, made and held, and a file");
    if (!d || !file) {
        return;
    }
    check(OPS->rmdir(&mnt, mnt.root, "doomed") == 0, "the rmdir comes first");
    before = lock_free_inodes();
    check(OPS->create(&mnt, d, "f", 0644, &made) == -ENOENT &&
              OPS->mknod(&mnt, d, "p", 0010644, 0, &made) == -ENOENT &&
              OPS->mkdir(&mnt, d, "s", 0755) == -ENOENT &&
              OPS->symlink(&mnt, d, "l", "target") == -ENOENT &&
              OPS->link(&mnt, d, "h", file) == -ENOENT,
          "and the removed directory refuses a file, a node, a directory and both links");
    check(lock_free_inodes() == before && before != 0, "and no inode is taken for them");
    OPS->put(&mnt, d);
    d = OPS->mkdir(&mnt, mnt.root, "kept", 0755) == 0 ? walk("/kept", &err) : 0;
    made = d ? make_file(d, "first", &rc) : 0;
    check(made && OPS->rmdir(&mnt, mnt.root, "kept") == -ENOTEMPTY,
          "a create that comes first leaves the rmdir ENOTEMPTY");
    if (made) {
        OPS->put(&mnt, made);
    }
    if (d) {
        OPS->put(&mnt, d);
    }
    OPS->put(&mnt, file);
    check(unmount_rw() == 0 && e2fsck_clean(wimg), "the removal volume is clean");
}

/** Make the directories of `paths`, in order under the root; 0 or -errno. */
static int lock_mkdirs(const char *const *paths, int count)
{
    int i, err;

    for (i = 0; i < count; i++) {
        const char *slash = paths[i];
        const char *name = paths[i];
        char parent[64];
        void *dir;
        int rc;

        while (*slash) {
            if (*slash == '/') {
                name = slash + 1;
            }
            slash++;
        }
        __builtin_memcpy(parent, paths[i], (unsigned long)(name - paths[i]));
        parent[name - paths[i]] = '\0';
        dir = walk(parent, &err);
        if (!dir) {
            return err;
        }
        rc = OPS->mkdir(&mnt, dir, name, 0755);
        OPS->put(&mnt, dir);
        if (rc != 0) {
            return rc;
        }
    }
    return 0;
}

/** Rename `oldname` in the directory at `from` to `newname` in the one at
 * `to`; the rename's answer, or the walk's error. */
static int lock_rename(const char *from, const char *oldname, const char *to, const char *newname)
{
    void *od, *nd;
    int err, rc;

    od = walk(from, &err);
    if (!od) {
        return err;
    }
    nd = walk(to, &err);
    if (!nd) {
        OPS->put(&mnt, od);
        return err;
    }
    rc = OPS->rename(&mnt, od, oldname, nd, newname);
    OPS->put(&mnt, nd);
    OPS->put(&mnt, od);
    return rc;
}

/**
 * The cases the VFS leaves a node unlocked in are refused before a node is
 * locked: a directory moved below itself is EINVAL, and one moved over a
 * directory above its own parent ENOTEMPTY. A victim or a source that
 * something holds is the node the holder sees: a file renamed over keeps
 * its bytes for its holder with no link left, a directory renamed over
 * lists as ENOENT, and a directory moved has its new parent at `..`.
 */
static void rename_order_row(void)
{
    static const char *const dirs[] = {"/a", "/a/b", "/a/b/c", "/m", "/e", "/p", "/q", "/p/s"};
    unsigned char text[4];
    void *victim, *empty, *moved, *up, *q, *n;
    int err, rc;
    char path[64];

    wbytes = build_image(wimg, 4096, 128, PROFILE, 16, "tree");
    use_image(wimg, wbytes);
    check(mount_rw() == 0, "the rename volume mounts writable");
    check(lock_mkdirs(dirs, (int)(sizeof(dirs) / sizeof(dirs[0]))) == 0,
          "the rename rows' directories are made");
    check(lock_rename("/", "a", "/a/b/c", "x") == -EINVAL,
          "a directory moved below itself is EINVAL");
    check(lock_rename("/a/b", "c", "/", "a") == -ENOTEMPTY,
          "a directory moved over a directory above its parent is ENOTEMPTY");
    n = walk("/a/b/c", &err);
    check(n != 0, "and both leave the tree as it was");
    if (n) {
        OPS->put(&mnt, n);
    }
    n = make_file(mnt.root, "f1", &rc);
    if (n) {
        write_at(n, "one", 3, 0);
        OPS->put(&mnt, n);
    }
    n = make_file(mnt.root, "f2", &rc);
    if (n) {
        write_at(n, "two", 3, 0);
        OPS->put(&mnt, n);
    }
    victim = walk("/f2", &err);
    check(victim && lock_rename("/", "f1", "/", "f2") == 0 && lock_nlink(victim) == 0 &&
              read_at(victim, text, 3, 0) == 3 && memcmp(text, "two", 3) == 0,
          "a file renamed over keeps its bytes for its holder with no link left");
    n = walk("/f2", &err);
    check(n && n != victim && read_at(n, text, 3, 0) == 3 && memcmp(text, "one", 3) == 0,
          "and the name has the source's bytes");
    if (n) {
        OPS->put(&mnt, n);
    }
    check(lock_rename("/", "f2", "/", "e") == -EISDIR, "a file over a directory is EISDIR");
    if (victim) {
        OPS->put(&mnt, victim);
    }
    empty = walk("/e", &err);
    check(empty && lock_rename("/", "m", "/", "e") == 0 && lock_nlink(empty) == 0,
          "a directory renamed over an empty held one leaves it with no link");
    if (empty) {
        struct vfs_dirent ent;
        uint32_t cursor = 0;

        check(OPS->readdir(&mnt, empty, &cursor, &ent) == -ENOENT,
              "and the held directory lists as ENOENT");
        OPS->put(&mnt, empty);
    }
    moved = walk("/p/s", &err);
    q = walk("/q", &err);
    check(moved && q && lock_rename("/p", "s", "/q", "s") == 0, "a held directory moves");
    up = 0;
    check(moved && q && OPS->lookup(&mnt, moved, "..", &up) == 0 && lock_ino(up) == lock_ino(q),
          "and its node finds its new parent at ..");
    check(moved && OPS->path(&mnt, moved, path, sizeof(path)) == 0 && streq(path, "/q/s"),
          "and its path is the new one");
    if (up) {
        OPS->put(&mnt, up);
    }
    if (q) {
        OPS->put(&mnt, q);
    }
    if (moved) {
        OPS->put(&mnt, moved);
    }
    check(unmount_rw() == 0 && e2fsck_clean(wimg), "the rename volume is clean");
}

/** The rows of this file. */
static void lock_rows(void)
{
    user_copy_row();
    removal_race_row();
    rename_order_row();
}

#endif
