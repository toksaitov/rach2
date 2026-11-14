/*
 * ext2_node_rows.h: rows of ext2_hosttest.c, which includes this file, for
 * the life of a node in rach2/sys/fs/ext2.c under its node locks: a second
 * caller on a loading node, a failed load with a sleeper, a node that dies
 * under a lookup, a last put under another lock, an inode number in the
 * hash, a remount that closes admission, a shutdown that closes it for good
 * with a bound on its wait, and an error flip. Two calls stand
 * for two processes and take turns, the second a parked call
 * (compat/park_host.c): a row hands over at a disk request, and a wait that
 * cannot end yet hands back. Files are empty, and a row's first process
 * holds a directory's lock around a change, as the VFS does; the rows of
 * the batch and of `ext2_punch` under their locks are in ext2_hosttest.c.
 */
#ifndef EXT2_NODE_ROWS_H_
#define EXT2_NODE_ROWS_H_

static void *row_nodes[2];
static int row_rc[2];
static int row_done;
static int row_skip, row_fail, row_seen;

/** Let `row_skip` reads go by, then give the turn to the other process once,
 * and fail the next `row_fail` reads, the one under way among them. */
static void turn_at_read(void)
{
    if (row_skip-- > 0) {
        return;
    }
    read_pause = 0;
    pass_turn();
    row_seen = row_done;
    read_fails = row_fail;
}

/** Give the turn to the other process at this write, once, and note whether
 * it finished meanwhile. */
static void turn_at_write(void)
{
    write_pause = 0;
    pass_turn();
    row_seen = row_done;
}

/** Give the turn to the other process at this flush, once. */
static void turn_at_flush(void)
{
    flush_pause = 0;
    pass_turn();
}

/** The free inodes of the mounted volume. */
static uint64_t row_free_inodes(void)
{
    struct vfs_statfs st;

    OPS->statfs(&mnt, &st);
    return st.ffree;
}

/**
 * Start a row: a new volume mounted writable with the empty files "one" and
 * "two", the cache emptied so that the first look at either reads the
 * disk. 1, or 0 for a volume that did not mount.
 */
static int node_row_begin(const char *what)
{
    int rc;
    void *f;

    wbytes = build_image(wimg, 4096, 128, PROFILE, 16, "tree");
    use_image(wimg, wbytes);
    check(mount_rw() == 0, what);
    f = make_file(mnt.root, "one", &rc);
    check(f != 0, "the row's first file is made");
    if (f) {
        OPS->put(&mnt, f);
    }
    f = make_file(mnt.root, "two", &rc);
    check(f != 0, "the row's second file is made");
    if (f) {
        OPS->put(&mnt, f);
    }
    check(OPS->sync(&mnt) == 0 && bcache_invalidate(&dev) == 0, "the row's cache is emptied");
    sleeps = 0;
    row_done = 0;
    row_seen = 0;
    row_skip = 0;
    row_fail = 0;
    return mnt.root != 0;
}

/** End a row: unmounted, and the volume clean when `clean` asks for that. */
static void node_row_end(int clean, const char *what)
{
    if (clean) {
        check(unmount_rw() == 0 && e2fsck_clean(wimg), what);
    } else {
        (void)unmount_rw();
    }
}

/** The second process's lookup of "one", into slot 1. */
static void lookup_second(void)
{
    row_rc[1] = OPS->lookup(&mnt, mnt.root, "one", &row_nodes[1]);
    row_done = 1;
}

/**
 * The first lookup of a name publishes its node before the inode's read,
 * and a second caller there sleeps on it and gets the same node, with the
 * inode read once.
 */
static void loading_node_row(void)
{
    if (!node_row_begin("the loading volume mounts")) {
        return;
    }
    row_nodes[0] = row_nodes[1] = 0;
    row_skip = 1; /* the directory's block, then the inode's */
    read_pause = turn_at_read;
    start_turns(lookup_second);
    row_rc[0] = OPS->lookup(&mnt, mnt.root, "one", &row_nodes[0]);
    finish_turns();
    check(row_rc[0] == 0 && row_rc[1] == 0 && row_nodes[0] && row_nodes[0] == row_nodes[1],
          "two lookups during a load get one node");
    check(sleeps > 0, "and the second slept on the load");
    if (row_nodes[0]) {
        OPS->put(&mnt, row_nodes[0]);
    }
    if (row_nodes[1]) {
        OPS->put(&mnt, row_nodes[1]);
    }
    node_row_end(1, "the loading volume unmounts clean");
}

/**
 * A load that fails ends with the failure for the caller who read and for
 * the one who slept on it, and leaves nothing in the hash: the next lookup
 * reads the inode again and finds it.
 */
static void failed_load_row(void)
{
    void *again = 0;

    if (!node_row_begin("the failed-load volume mounts")) {
        return;
    }
    row_nodes[0] = row_nodes[1] = 0;
    row_skip = 1;
    row_fail = 1;
    read_pause = turn_at_read;
    start_turns(lookup_second);
    row_rc[0] = OPS->lookup(&mnt, mnt.root, "one", &row_nodes[0]);
    finish_turns();
    check(sleeps > 0, "the second caller slept on the load that failed");
    check(row_rc[0] == -EIO && row_rc[1] == -EIO, "a failed load fails both callers");
    check(OPS->lookup(&mnt, mnt.root, "one", &again) == 0 && again != 0,
          "and the next lookup loads the inode");
    if (again) {
        OPS->put(&mnt, again);
    }
    node_row_end(1, "the failed-load volume unmounts clean");
}

/**
 * A lookup that finds a dying inode waits for it to go and looks again. Only
 * a damaged volume names one: here "one" and "alias" share a link count of
 * 1, so the driver frees the inode at the last put of "alias", and the
 * lookup of "one" then reads a freed inode, which it reports and answers
 * EIO, as Linux's `ext2_iget` answers a deleted inode a name references.
 */
static void dying_node_row(void)
{
    void *n = 0, *f;
    int rc;

    wbytes = build_image(wimg, 4096, 128, PROFILE, 16, "tree");
    use_image(wimg, wbytes);
    check(mount_rw() == 0, "the dying volume mounts");
    f = make_file(mnt.root, "one", &rc);
    check(f != 0, "the file is made");
    if (f) {
        OPS->put(&mnt, f);
    }
    (void)unmount_rw();
    check(ext2_fixture_debugfs(wimg, "ln one alias", 1, 0), "a second name is made");
    use_image(wimg, wbytes);
    check(mount_rw() == 0, "the dying volume mounts again");
    sleeps = 0;
    row_done = 0;
    row_seen = 0;
    OPS->lock_node(&mnt, mnt.root);
    rc = OPS->lookup(&mnt, mnt.root, "alias", &n);
    check(rc == 0 && OPS->unlink(&mnt, mnt.root, "alias") == 0, "the alias is removed");
    OPS->unlock_node(&mnt, mnt.root);
    row_nodes[1] = 0;
    write_pause = turn_at_write;
    start_turns(lookup_second);
    if (n) {
        OPS->put(&mnt, n);
    }
    finish_turns();
    check(sleeps > 0, "a lookup of the dying inode slept");
    check(row_rc[1] == -EIO && row_nodes[1] == 0 &&
              strstr(last_warn, "a name references it, freed") != 0,
          "and, once the inode had gone, found it freed and answered EIO");
    if (row_rc[1] != -EIO) {
        printf("  rc %d, last warning '%s'\n", row_rc[1], last_warn);
    }
    if (row_nodes[1]) {
        OPS->put(&mnt, row_nodes[1]);
    }
    node_row_end(0, 0);
}

/**
 * The last put of an unlinked inode under another node's lock leaves the
 * inode for the end of the operation, which frees it at the last unlock.
 */
static void reap_at_unlock_row(void)
{
    void *n = 0;
    uint64_t before;

    if (!node_row_begin("the reap volume mounts")) {
        return;
    }
    check(OPS->lookup(&mnt, mnt.root, "one", &n) == 0 && n != 0, "the file is found");
    if (!n) {
        return;
    }
    before = row_free_inodes();
    OPS->lock_node(&mnt, mnt.root);
    check(OPS->unlink(&mnt, mnt.root, "one") == 0,
          "the file is removed under its directory's lock");
    OPS->put(&mnt, n);
    check(row_free_inodes() == before, "a last put under a lock frees nothing");
    OPS->unlock_node(&mnt, mnt.root);
    check(row_free_inodes() == before + 1, "and the last unlock frees the inode");
    node_row_end(1, "the reap volume unmounts clean");
}

/** The second process prepares the unmount, which reaps what a holder of a
 * lock left. */
static void prepare_second(void)
{
    row_rc[1] = OPS->prepare_unmount(&mnt);
    row_done = 1;
}

/**
 * A final put left on the list by a process that still holds its lock is done
 * by an unmount's preparation, so nothing outlives the mount.
 */
static void reap_at_unmount_row(void)
{
    void *n = 0;
    uint64_t before;

    if (!node_row_begin("the unmount reap volume mounts")) {
        return;
    }
    check(OPS->lookup(&mnt, mnt.root, "one", &n) == 0 && n != 0, "the file is found");
    if (!n) {
        return;
    }
    before = row_free_inodes();
    OPS->lock_node(&mnt, mnt.root);
    check(OPS->unlink(&mnt, mnt.root, "one") == 0, "the file is removed");
    OPS->put(&mnt, n);
    start_turns(prepare_second);
    pass_turn();
    check(row_done && row_rc[1] == 0 && row_free_inodes() == before + 1,
          "an unmount's preparation frees the inode a holder of a lock left");
    OPS->unlock_node(&mnt, mnt.root);
    finish_turns();
    node_row_end(1, "the unmount reap volume unmounts clean");
}

/**
 * An inode in the hash is passed over although its bit is clear: a held file
 * whose bit debugfs cleared keeps its number, which the next allocation
 * takes once the file has gone.
 */
static void inode_skip_row(void)
{
    struct vfs_stat st;
    void *held = 0, *fresh, *later;
    uint32_t ino = 0;
    int rc;

    wbytes = build_image(wimg, 4096, 128, PROFILE, 16, "tree");
    check(ext2_fixture_debugfs(wimg, "freei hello.txt", 1, 0), "the file's bit is cleared");
    use_image(wimg, wbytes);
    check(mount_rw() == 0, "the skip volume mounts");
    check(OPS->lookup(&mnt, mnt.root, "hello.txt", &held) == 0 && held != 0, "the file is held");
    if (held) {
        OPS->getattr(&mnt, held, &st);
        ino = st.ino;
    }
    fresh = make_file(mnt.root, "fresh", &rc);
    check(fresh != 0, "a file is made beside it");
    if (fresh) {
        OPS->getattr(&mnt, fresh, &st);
        check((uint32_t)st.ino != ino, "inode_alloc passes over an inode in the hash");
        OPS->put(&mnt, fresh);
    }
    if (held) {
        OPS->put(&mnt, held);
    }
    later = make_file(mnt.root, "later", &rc);
    check(later != 0, "a file is made once the other has gone");
    if (later) {
        OPS->getattr(&mnt, later, &st);
        check((uint32_t)st.ino == ino, "and takes the number the hash kept");
        OPS->put(&mnt, later);
    }
    (void)unmount_rw();
}

/** The second process remounts read-only, into slot 1. */
static void remount_second(void)
{
    row_rc[1] = OPS->remount(&mnt, rw_flags | VFS_MS_RDONLY, NULL);
    if (row_rc[1] == 0) {
        mnt.flags = rw_flags | VFS_MS_RDONLY;
    }
    row_done = 1;
}

/**
 * A remount that finds a writer inside waits for it before it scans: the
 * writer's unlink, begun first and stopped at the disk, leaves a removed
 * file that is still open, which only a scan after the wait can see.
 */
static void remount_waits_row(void)
{
    void *n = 0;
    uint64_t before;

    if (!node_row_begin("the remount volume mounts")) {
        return;
    }
    check(OPS->lookup(&mnt, mnt.root, "one", &n) == 0 && n != 0, "the file is open");
    if (!n) {
        return;
    }
    before = row_free_inodes();
    write_pause = turn_at_write;
    start_turns(remount_second);
    OPS->lock_node(&mnt, mnt.root);
    check(OPS->unlink(&mnt, mnt.root, "one") == 0, "the writer's unlink ends");
    check(!row_seen, "the remount did not end while the writer was inside");
    OPS->unlock_node(&mnt, mnt.root);
    finish_turns();
    check(sleeps > 0, "the remount slept for the writer");
    check(row_rc[1] == -EBUSY, "and its scan saw what the writer unlinked");
    OPS->put(&mnt, n);
    check(row_free_inodes() == before + 1, "and admission opened: the last put frees the inode");
    node_row_end(1, "the remount volume unmounts clean");
}

/**
 * New writers wait while a remount is under way, here stopped at its flush,
 * and when let in see that the mount is read-only.
 */
static void remount_admission_row(void)
{
    void *n = 0;

    if (!node_row_begin("the admission volume mounts")) {
        return;
    }
    flush_pause = turn_at_flush;
    start_turns(remount_second);
    pass_turn();
    OPS->lock_node(&mnt, mnt.root);
    row_rc[0] = OPS->create(&mnt, mnt.root, "late", 0644, &n);
    OPS->unlock_node(&mnt, mnt.root);
    check(row_done && row_rc[1] == 0, "the remount ended before the new writer went in");
    check(sleeps > 0, "the new writer slept at the gate");
    check(row_rc[0] == -EROFS, "and then found the mount read-only");
    finish_turns();
    check(OPS->remount(&mnt, rw_flags, NULL) == 0, "a second remount makes it writable");
    mnt.flags = rw_flags;
    OPS->lock_node(&mnt, mnt.root);
    check(OPS->create(&mnt, mnt.root, "late", 0644, &n) == 0, "and a writer goes in again");
    OPS->unlock_node(&mnt, mnt.root);
    if (n) {
        OPS->put(&mnt, n);
    }
    node_row_end(1, "the admission volume unmounts clean");
}

/* A shutdown row's watch of the superblock: whether a write set its valid
   bit, and whether one did so while the row's writer was inside. While
   `row_stuck` is set, the writer's disk does not answer. */
static int valid_written, valid_inside, row_stuck;

/** Note a write that carries the superblock's state with the valid bit set,
 * and whether the second process had ended its call by then. */
static void watch_valid(uint64_t lba, uint32_t count, const void *buf)
{
    if (lba <= 2 && lba + count > 2 && (((const unsigned char *)buf)[(2 - lba) * 512 + 58] & 1)) {
        valid_written = 1;
        if (!row_done) {
            valid_inside = 1;
        }
    }
}

/** Give the turn away at this read until the row lets the disk answer. */
static void stuck_at_read(void)
{
    read_pause = 0;
    while (row_stuck) {
        pass_turn();
    }
}

/** The second process writes 3000 bytes to the file in slot 0 under its
 * lock, as the VFS does, into slot 1. */
static void write_second(void)
{
    void *h = 0;

    row_rc[1] = OPS->open(&mnt, row_nodes[0], 2, &h);
    if (row_rc[1] == 0) {
        OPS->lock_node(&mnt, row_nodes[0]);
        row_rc[1] = OPS->write(&mnt, h, pattern, 3000, 0);
        OPS->unlock_node(&mnt, row_nodes[0]);
        OPS->release(&mnt, h);
    }
    row_done = 1;
}

/** Whether the image's superblock has its valid bit set. */
static int image_valid(void)
{
    unsigned char sb[2];

    return host_read(wimg, sb, 2, 1024 + 58) == 2 && (sb[0] & 1) != 0;
}

/**
 * A shutdown that finds a writer inside a disk wait closes admission and
 * waits for it before it writes the valid bit, so the volume it leaves is
 * clean and holds the write; the write is what a shutdown that only
 * prepared the unmount would let change the volume after the bit.
 */
static void shutdown_waits_row(void)
{
    void *n = 0;
    int rc;

    if (!node_row_begin("the shutdown volume mounts")) {
        return;
    }
    check(OPS->lookup(&mnt, mnt.root, "one", &n) == 0 && n != 0, "the file is open");
    if (!n) {
        return;
    }
    row_nodes[0] = n;
    read_pause = turn_at_read;
    start_turns(write_second);
    pass_turn();
    check(!row_done && read_pause == 0, "the writer waits on the disk");
    valid_written = valid_inside = 0;
    write_watch = watch_valid;
    rc = OPS->shutdown(&mnt, tick_count() + 1000);
    write_watch = 0;
    check(rc == 0 && row_done && row_rc[1] == 3000,
          "the shutdown waited for the writer and marked the volume clean");
    check(valid_written && !valid_inside, "the valid bit was written once no writer was left");
    check(image_valid() && e2fsck_clean(wimg), "and the volume on the disk is clean as it stops");
    finish_turns();
    OPS->put(&mnt, n);
    node_row_end(1, "the shutdown volume unmounts clean");
}

/**
 * A writer whose disk never answers holds a shutdown only to its deadline,
 * after which the volume is left as it is, its valid bit clear, so that the
 * next mount checks it.
 */
static void shutdown_bounded_row(void)
{
    void *n = 0;
    int rc;

    if (!node_row_begin("the bounded shutdown volume mounts")) {
        return;
    }
    check(OPS->lookup(&mnt, mnt.root, "one", &n) == 0 && n != 0, "the file is open");
    if (!n) {
        return;
    }
    row_nodes[0] = n;
    row_stuck = 1;
    read_pause = stuck_at_read;
    start_turns(write_second);
    pass_turn();
    check(!row_done && read_pause == 0, "the writer waits on a disk that does not answer");
    valid_written = valid_inside = 0;
    write_watch = watch_valid;
    rc = OPS->shutdown(&mnt, tick_count() + 50);
    write_watch = 0;
    check(rc == -EBUSY && !row_done, "the shutdown ended at its deadline with the writer inside");
    check(sleeps > 0 && !valid_written && !image_valid(),
          "and left the valid bit clear for the next mount's check");
    row_stuck = 0;
    finish_turns();
    check(row_rc[1] == 3000, "the writer ends once its disk answers");
    OPS->put(&mnt, n);
    node_row_end(1, "the bounded shutdown volume unmounts clean");
}

/**
 * A removed file still open at a shutdown is a change still owed, its
 * inode's free at the last put: the volume is synced, its valid bit left
 * clear.
 */
static void shutdown_unlinked_row(void)
{
    void *n = 0;

    if (!node_row_begin("the unlinked shutdown volume mounts")) {
        return;
    }
    check(OPS->lookup(&mnt, mnt.root, "one", &n) == 0 && n != 0, "the file is open");
    if (!n) {
        return;
    }
    OPS->lock_node(&mnt, mnt.root);
    check(OPS->unlink(&mnt, mnt.root, "one") == 0, "and removed");
    OPS->unlock_node(&mnt, mnt.root);
    check(OPS->shutdown(&mnt, tick_count() + 50) == -EBUSY && !image_valid(),
          "a shutdown with a removed file open leaves the valid bit clear");
    OPS->put(&mnt, n);
    node_row_end(1, "the unlinked shutdown volume unmounts clean");
}

/** The second process syncs into a flush that fails, which flips the mount. */
static void failed_sync_second(void)
{
    flush_fail_at = flush_calls + 1;
    row_rc[1] = OPS->sync(&mnt);
    flush_fail_at = -1;
    row_done = 1;
}

/**
 * A write that fails makes the mount read-only at once and waits for nobody:
 * the failed sync ends while a writer is inside, and the writer ends too.
 */
static void error_flip_row(void)
{
    void *n = 0;

    if (!node_row_begin("the error flip volume mounts")) {
        return;
    }
    read_pause = turn_at_read; /* a clean block, which a sync does not wait for */
    start_turns(failed_sync_second);
    OPS->lock_node(&mnt, mnt.root);
    row_rc[0] = OPS->create(&mnt, mnt.root, "inside", 0644, &n);
    OPS->unlock_node(&mnt, mnt.root);
    finish_turns();
    check(row_seen, "an error flip ends with a writer inside");
    check(row_rc[1] == -EIO && (mnt.flags & VFS_MS_RDONLY), "and the mount is read-only");
    if (row_rc[0] == 0 && n) {
        OPS->put(&mnt, n);
    }
    node_row_end(0, 0);
    mnt.write_error = 0;
}

/** Every row of a node's life. */
static void node_rows(void)
{
    uint32_t saved = rw_flags;

    rw_flags = VFS_MS_DIRSYNC;
    loading_node_row();
    failed_load_row();
    dying_node_row();
    reap_at_unlock_row();
    reap_at_unmount_row();
    inode_skip_row();
    remount_waits_row();
    remount_admission_row();
    shutdown_waits_row();
    shutdown_bounded_row();
    shutdown_unlinked_row();
    error_flip_row();
    rw_flags = saved;
}

#endif
