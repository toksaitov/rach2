/*
 * ext2_fence_rows.h: rows of ext2_hosttest.c, which includes this file, for a
 * fenced node, which a store found changed without its mark and which is then
 * Linux's bad inode, and for a sync that meets a store of a node under way.
 * The unmarked change is made on the node's field directly, as a missed mark
 * would leave it, and stored through `ext2_inode_store`, which fences it.
 */
#ifndef EXT2_FENCE_ROWS_H_
#define EXT2_FENCE_ROWS_H_

/** The free blocks statfs gives, or ~0 when it fails. */
static uint64_t fence_bfree(void)
{
    struct vfs_statfs st;

    return OPS->statfs(&mnt, &st) == 0 ? st.bfree : ~(uint64_t)0;
}

/**
 * Start a fence row: a new volume mounted writable under `errors=continue`,
 * so that the fault the fence reports leaves the mount writing, with the
 * empty files "one" and "two" and the directory "fenced". 1, or 0 for a volume
 * that did not mount.
 */
static int fence_begin(const char *what)
{
    int rc;
    void *f;

    wbytes = build_image(wimg, 4096, 128, PROFILE, 16, "tree");
    use_image(wimg, wbytes);
    check(mount_options(rw_flags, "errors=continue") == 0, what);
    if (!mnt.root) {
        return 0;
    }
    OPS->lock_node(&mnt, mnt.root);
    f = make_file(mnt.root, "one", &rc);
    if (f) {
        OPS->put(&mnt, f);
    }
    f = make_file(mnt.root, "two", &rc);
    if (f) {
        OPS->put(&mnt, f);
    }
    rc = OPS->mkdir(&mnt, mnt.root, "fenced", 0755);
    OPS->unlock_node(&mnt, mnt.root);
    check(rc == 0 && OPS->sync(&mnt) == 0, "the fence row's files are made");
    sleeps = 0;
    row_done = 0;
    row_seen = 0;
    row_skip = 0;
    row_fail = 0;
    return 1;
}

/** Look up `name` in the root; the node, held, or NULL. */
static void *fence_lookup(const char *name)
{
    void *n = 0;

    return OPS->lookup(&mnt, mnt.root, name, &n) == 0 ? n : 0;
}

/** Store node `n` under its lock, as a sync would; its result. */
static int fence_store(void *n)
{
    int rc;

    OPS->lock_node(&mnt, n);
    rc = ext2_inode_store(ext2_em_of(&mnt), n);
    OPS->unlock_node(&mnt, n);
    return rc;
}

/** Give node `n` an owner without its mark and store it, which fences it;
 * whether the store refused and the node is fenced. */
static int fence_uid(void *n)
{
    ((struct ext2_node *)n)->uid = 4321;
    return fence_store(n) == -EIO && ((struct ext2_node *)n)->fenced;
}

/**
 * An empty file whose attribute block was set, without the mark, to the one
 * another file holds, which a store fences, frees nothing of that block: with
 * `held` 0 its unlink is refused before the entry goes, and with `held` 1,
 * removed while open before the fence, its last close drops it unfreed. Both
 * free the other file's block without the fence (j157's finding 1), which
 * leaves that file pointing at a free block.
 */
static void fence_release_row(int held)
{
    void *one, *two, *h = 0;
    char value[8], set[] = "val";
    uint64_t bfree, ffree;
    int rc = -1;

    if (!fence_begin("the fence release volume mounts")) {
        return;
    }
    two = fence_lookup("two");
    one = fence_lookup("one");
    check(one && two && xa_call(two, VFS_XATTR_SET, "user.k", set, 3, 0) == 0 &&
              ((struct ext2_node *)two)->file_acl != 0,
          "the second file takes an attribute block");
    if (!one || !two) {
        node_row_end(0, 0);
        return;
    }
    if (held) {
        check(OPS->open(&mnt, one, 0, &h) == 0, "the first file is opened");
        OPS->lock_node(&mnt, mnt.root);
        rc = OPS->unlink(&mnt, mnt.root, "one");
        OPS->unlock_node(&mnt, mnt.root);
        check(rc == 0, "and removed while open");
    }
    check(OPS->sync(&mnt) == 0, "the volume is synced before the fence");
    bfree = fence_bfree();
    ffree = row_free_inodes();
    ((struct ext2_node *)one)->file_acl = ((struct ext2_node *)two)->file_acl;
    check(fence_store(one) == -EIO && ((struct ext2_node *)one)->fenced,
          "a store of the first file with the second's attribute block unmarked fences it");
    if (held) {
        OPS->put(&mnt, one);
        if (h) {
            OPS->release(&mnt, h);
        }
        check(fence_bfree() == bfree && row_free_inodes() == ffree,
              "the last close of the fenced removed file frees neither its inode nor the "
              "attribute block it names");
    } else {
        OPS->lock_node(&mnt, mnt.root);
        rc = OPS->unlink(&mnt, mnt.root, "one");
        OPS->unlock_node(&mnt, mnt.root);
        OPS->put(&mnt, one);
        check(rc == -EIO && fence_bfree() == bfree && row_free_inodes() == ffree,
              "the unlink of the fenced file is EIO and frees nothing");
    }
    check(xa_call(two, VFS_XATTR_GET, "user.k", value, sizeof value, 0) == 3 &&
              OPS->sync(&mnt) == 0 && fence_bfree() == bfree,
          "and the second file's attribute stays, its block still taken");
    OPS->put(&mnt, two);
    node_row_end(0, 0);
}

/**
 * A fenced node answers every operation but getattr with EIO, as Linux's bad
 * inode does: a file's open and a read through a description opened before
 * the fence, and a directory's lookup, readdir through a description opened
 * before, and create. Without the fence's checks each reads through pointers
 * that cannot be trusted.
 */
static void fence_read_row(void)
{
    void *one, *dir, *hf = 0, *hd = 0, *h = 0, *n = 0;
    struct vfs_dirent ent;
    uint32_t cursor = 0;
    char buf[8];
    int rc;

    if (!fence_begin("the fence read volume mounts")) {
        return;
    }
    one = fence_lookup("one");
    dir = fence_lookup("fenced");
    check(one && dir && write_at(one, "hello", 5, 0) == 0 && OPS->sync(&mnt) == 0 &&
              OPS->open(&mnt, one, 0, &hf) == 0 && OPS->open(&mnt, dir, 0, &hd) == 0,
          "the fence read row's file is written and both are opened");
    if (!one || !dir || !hf || !hd) {
        node_row_end(0, 0);
        return;
    }
    check(fence_uid(one) && fence_uid(dir), "unmarked owners fence the file and the directory");
    check(OPS->open(&mnt, one, 0, &h) == -EIO, "the fenced file's open is EIO");
    check(OPS->read(&mnt, hf, buf, 5, 0) == -EIO, "and a read through an earlier open is EIO");
    check(OPS->lookup(&mnt, dir, "none", &n) == -EIO, "the fenced directory's lookup is EIO");
    check(OPS->readdir(&mnt, hd, &cursor, &ent) == -EIO, "and its readdir is EIO");
    OPS->lock_node(&mnt, dir);
    rc = OPS->create(&mnt, dir, "new", 0644, &n);
    OPS->unlock_node(&mnt, dir);
    check(rc == -EIO, "and a create in it is EIO");
    OPS->release(&mnt, hf);
    OPS->release(&mnt, hd);
    OPS->put(&mnt, one);
    OPS->put(&mnt, dir);
    node_row_end(0, 0);
}

static void *storing_handle;

/** The second process's fsync of the file the first is storing. */
static void storing_fsync_second(void)
{
    row_rc[1] = OPS->fsync(&mnt, storing_handle);
    row_done = 1;
}

/** The second process's sync of the volume while the first is storing. */
static void storing_sync_second(void)
{
    row_rc[1] = OPS->sync(&mnt);
    row_done = 1;
}

/**
 * A sync, of the file with `volume` 0 and of the volume with 1, that comes
 * while a first fsync's store of the file sleeps on the read of its cold
 * table block waits for that store on the node's lock, and so ends after it.
 * Without the storing bit the mark is clear by then and the sync passes the
 * node over and returns while the store is still under way (j157's finding
 * 2).
 */
static void storing_sync_row(int volume)
{
    void *one;
    int rc;

    if (!fence_begin("the storing sync volume mounts")) {
        return;
    }
    one = fence_lookup("one");
    storing_handle = 0;
    check(one && OPS->open(&mnt, one, 1, &storing_handle) == 0, "the file is opened");
    if (!one || !storing_handle) {
        node_row_end(0, 0);
        return;
    }
    OPS->lock_node(&mnt, one);
    EXT2_NODE_SET((struct ext2_node *)one, uid, 4321);
    OPS->unlock_node(&mnt, one);
    check(bcache_invalidate(&dev) == 0, "the marked file's table block leaves the cache");
    read_pause = turn_at_read;
    start_turns(volume ? storing_sync_second : storing_fsync_second);
    rc = OPS->fsync(&mnt, storing_handle);
    finish_turns();
    read_pause = 0;
    check(rc == 0 && row_done && row_rc[1] == 0, "both syncs succeed");
    check(!row_seen && sleeps > 0, volume ? "and the volume's sync ended only after the store"
                                          : "and the second fsync ended only after the store");
    check(((struct ext2_node *)one)->unstored == 0, "and the file is stored");
    OPS->release(&mnt, storing_handle);
    OPS->put(&mnt, one);
    node_row_end(1, "the storing sync volume unmounts clean");
}

/** Every row of a fenced node and of a store under way. */
static void fence_rows(void)
{
    uint32_t saved = rw_flags;

    rw_flags = VFS_MS_DIRSYNC;
    fence_release_row(0);
    fence_release_row(1);
    fence_read_row();
    storing_sync_row(0);
    storing_sync_row(1);
    rw_flags = saved;
}

#endif
