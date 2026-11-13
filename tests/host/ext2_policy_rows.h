/*
 * ext2_policy_rows.h: the rows of ext2's error policy by its two matrices,
 * included by ext2_hosttest.c: the mount's mode and its transitions, and a
 * fault of each kind of block the read primitive reads, each met by the
 * operation that reads it, the superblock's by the report's own write too,
 * on mounts under `errors=remount-ro`.
 */

#ifndef EXT2_POLICY_ROWS_H_
#define EXT2_POLICY_ROWS_H_

/** The free inodes statfs gives, or ~0 when it fails. */
static uint64_t policy_ffree(void)
{
    struct vfs_statfs st;

    return OPS->statfs(&mnt, &st) == 0 ? st.ffree : ~(uint64_t)0;
}

/** Make `name` in the root and open it for writing into `*h`; the node,
 * held, or NULL. */
static void *policy_open(const char *name, void **h)
{
    int rc = 0;
    void *n = make_file(mnt.root, name, &rc);

    *h = 0;
    if (n && OPS->open(&mnt, n, 1, h) != 0) {
        *h = 0;
    }
    return n;
}

/** Fail the first read that covers `fail_read_block`, and no later one. */
static void policy_fail_once(void)
{
    if (fail_read_block >= 0 && (uint64_t)fail_read_block >= request_block &&
        (uint64_t)fail_read_block < request_block + request_blocks) {
        read_pause = 0;
        fail_read_hits++;
        read_fails = 1;
    }
}

/**
 * The transitions of the mode, on a volume that stays whole, so that a
 * remount's check passes it: two files are made, held open and removed, the
 * cache is emptied, and the lookup of /dir/sub/deep.txt fails at the one
 * read of its inode table's block that fails, which the read primitive
 * reports as Linux's `ext2_get_inode` reports a failed `sb_bread`. The fault
 * makes the mount STOPPED, which must:
 *
 * - admit no create, EROFS;
 * - free the first file at its last put, as Linux's `ext2_evict_inode`
 *   frees under SB_RDONLY (j129's finding 2);
 * - take a remount read-only with `errors=` as a no-op that succeeds with
 *   the second file still open, as Linux's `ext2_remount` returns early on a
 *   read-only file system, and stay STOPPED, so that the second file is
 *   still freed at its last put (items 2 and 4);
 * - write nothing at an abandoned unmount (item 5);
 * - leave the stop only through a remount writable that checks the volume,
 *   after which a create allocates again (j129's finding 1).
 */
static void policy_mode_rows(void)
{
    const char *img = fix_path("mode.ext2");
    void *a, *b, *ha = 0, *hb = 0, *n;
    uint64_t ffree;
    int64_t table;
    int rc = 0, writes;

    if (!damage_mount(img, 0, 0, "the mode rows' volume mounts writable")) {
        return;
    }
    table = inode_block_of(img, "/dir/sub/deep.txt");
    ffree = policy_ffree();
    a = policy_open("held", &ha);
    b = policy_open("kept", &hb);
    check(a && b && ha && hb && OPS->unlink(&mnt, mnt.root, "held") == 0 &&
              OPS->unlink(&mnt, mnt.root, "kept") == 0 && policy_ffree() == ffree - 2,
          "two files are made, held open and removed");
    if (a) {
        OPS->put(&mnt, a);
    }
    if (b) {
        OPS->put(&mnt, b);
    }
    check(table > 0 && OPS->sync(&mnt) == 0 && bcache_invalidate(&dev) == 0,
          "the mode rows' cache is emptied");
    fail_read_block = table;
    fail_read_hits = 0;
    read_pause = policy_fail_once;
    rc = damage_lookup("/dir/sub/deep.txt");
    read_pause = 0;
    read_fails = 0;
    fail_read_block = -1;
    check(rc == -EIO && fail_read_hits == 1 && (mnt.flags & VFS_MS_RDONLY) != 0 &&
              streq(last_log, "error: remounting read-only"),
          "a fault under errors=remount-ro stops the mount by name");
    check(make_file(mnt.root, "new", &rc) == 0 && rc == -EROFS, "and a create is EROFS");
    if (ha) {
        OPS->release(&mnt, ha);
    }
    check(policy_ffree() == ffree - 1,
          "and the stopped mount frees a removed file at its last put");
    check(OPS->remount(&mnt, rw_flags | VFS_MS_RDONLY, "errors=remount-ro") == 0,
          "a remount read-only of the stopped mount, a removed file open, takes its options");
    mnt.flags = rw_flags | VFS_MS_RDONLY;
    if (hb) {
        OPS->release(&mnt, hb);
    }
    check(policy_ffree() == ffree, "and leaves it stopped: the file is freed at its last put");
    rc = OPS->prepare_unmount(&mnt);
    writes = writes_made;
    if (rc == 0) {
        OPS->abort_unmount(&mnt);
    }
    check(rc == 0 && writes_made == writes && disk_state(img) == 2,
          "an abandoned unmount of the stopped mount writes nothing");
    last_check[0] = 0;
    rc = OPS->remount(&mnt, rw_flags, NULL);
    mnt.flags = rw_flags;
    check(rc == 0 && last_check[0] != 0, "a remount writable ends the stop through the check");
    n = make_file(mnt.root, "after", &rc);
    check(n != 0 && rc == 0 && policy_ffree() == ffree - 1,
          "and the mount admits and allocates again");
    if (n) {
        OPS->put(&mnt, n);
    }
    check(unmount_rw() == 0 && disk_state(img) == 2, "and the unmount keeps the error bit");
}

/** Fail every read of block `blk` while `step` runs on a cache emptied
 * first; the reads failed so, the cache's sync failing counted as none. */
static int policy_unread(int64_t blk, int (*step)(void), int *rc)
{
    if (OPS->sync(&mnt) != 0 || bcache_invalidate(&dev) != 0) {
        *rc = -1;
        return 0;
    }
    fail_read_block = blk;
    fail_read_hits = 0;
    read_pause = fail_block_reads;
    *rc = step();
    read_pause = 0;
    read_fails = 0;
    fail_read_block = -1;
    return fail_read_hits;
}

/* The node and description a step of `policy_kind_rows` works on. */
static void *policy_node, *policy_handle;

/** Truncate the policy node to nothing. */
static int policy_truncate(void)
{
    return OPS->truncate(&mnt, policy_handle, 0);
}

/** Remove /many/file2.txt, whose directory is the policy node. */
static int policy_unlink(void)
{
    return OPS->unlink(&mnt, policy_node, "file2.txt");
}

/** Write three bytes at the start of the policy node. */
static int policy_write(void)
{
    return (int)OPS->write(&mnt, policy_handle, "HEL", 3, 0);
}

/** Take `path` as the policy node, opened for writing when `open` is set;
 * whether it was found. */
static int policy_take(const char *path, int open)
{
    int err = 0;

    policy_handle = 0;
    policy_node = walk(path, &err);
    if (policy_node && open && OPS->open(&mnt, policy_node, 1, &policy_handle) != 0) {
        policy_handle = 0;
    }
    return policy_node && (!open || policy_handle);
}

/** Give back the policy node and its description. */
static void policy_drop(void)
{
    if (policy_handle) {
        OPS->release(&mnt, policy_handle);
    }
    if (policy_node) {
        OPS->put(&mnt, policy_node);
    }
    policy_node = policy_handle = 0;
}

/**
 * A fault of each kind of block the read primitive reads, met by the
 * operation that reads it, each logged by its record, the error bit on the
 * disk and the mount stopped (`damage_reported`):
 *
 * - SLOT: an entry that names inode 7, reserved, answers EIO, as Linux's
 *   `ext2_get_inode` refuses a number below `s_first_ino` (j129's
 *   finding 4);
 * - BITMAP: a block bitmap whose bit of the group's inode table is clear is
 *   reported at its first read, a free's, which goes on, as Linux's
 *   `read_block_bitmap` checks it (j129's finding 6);
 * - RECLAIM: a truncate whose single indirect block cannot be read succeeds,
 *   the block abandoned, as Linux's `ext2_free_branches` goes on (j129's
 *   finding 5);
 * - BITMAP at a free: an unlink whose group's block bitmap cannot be read
 *   still frees the inode, the blocks left taken (item 1);
 * - SLOT in the batch: a write whose table block cannot be read is EIO and
 *   reported, as a load's and a store's are (j129's finding 3).
 */
static void policy_kind_rows(void)
{
    const char *img = fix_path("kind.ext2");
    char script[64];
    int64_t blk;
    uint64_t ffree;
    int rc = 0, hits;

    if (damage_mount(img, "ln <7> /res\n", 0, "the reserved-inode volume mounts")) {
        damage_reported(img, damage_lookup("/res"), -EIO, "bad inode number",
                        "a lookup of a reserved inode an entry names answers EIO");
    }
    build_image(img, 4096, 128, PROFILE, 16, "tree");
    blk = inode_block_of(img, "<2>");
    snprintf(script, sizeof(script), "freeb %ld\n", (long)blk);
    if (blk > 0 && damage_mount(img, script, 0, "the bitmap damage mounts")) {
        rc = policy_take("/many", 0) ? policy_unlink() : -1;
        policy_drop();
        damage_reported(img, rc, 0, "does not mark the group's tables used",
                        "a block bitmap without its inode table's bit is reported at a free");
    }
    blk = debugfs_number(img, "stat /dir/big.bin", "(IND):");
    if (blk > 0 && damage_mount(img, 0, 0, "the unread pointer block's volume mounts")) {
        hits = policy_take("/dir/big.bin", 1) ? policy_unread(blk, policy_truncate, &rc) : 0;
        policy_drop();
        check(hits > 0, "a truncate meets its single indirect block unread");
        damage_reported(img, rc, 0, "pointer block",
                        "and succeeds, the block abandoned and reported");
    }
    blk = debugfs_number(img, "stats", "block bitmap at ");
    if (blk > 0 && damage_mount(img, 0, 0, "the unread bitmap's volume mounts")) {
        ffree = policy_ffree();
        hits = policy_take("/many", 0) ? policy_unread(blk, policy_unlink, &rc) : 0;
        policy_drop();
        check(hits > 0 && rc == 0 && policy_ffree() == ffree + 1,
              "an unlink whose block bitmap cannot be read frees the inode");
        damage_reported(img, rc, 0, "bitmap block", "and reports the bitmap");
    }
    blk = inode_block_of(img, "/hello.txt");
    if (blk > 0 && damage_mount(img, 0, 0, "the unread table block's volume mounts")) {
        hits = policy_take("/hello.txt", 1) ? policy_unread(blk, policy_write, &rc) : 0;
        policy_drop();
        check(hits > 0, "a write meets its inode's table block unread");
        damage_reported(img, rc, -EIO, "table block", "and is EIO, reported as a load's is");
    }
}

/** Sync the mount, the step of `policy_super_row`. */
static int policy_sync(void)
{
    return OPS->sync(&mnt);
}

/**
 * A sync whose write of the superblock's counts fails at the read of its
 * block reports the fault once: the report's own write of the error bit
 * meets the same failed read, which it logs and does not report again, as
 * Linux's `ext2_error` writes the superblock it keeps and reads nothing.
 * The mount under `errors=remount-ro` ends read-only, stopped by one record,
 * with the bit in memory alone and the volume left not valid on the disk,
 * for the check of its next mount. Before the error mark wrote quietly, each
 * report's write read the block again and reported its failure, without end.
 */
static void policy_super_row(void)
{
    const char *img = fix_path("super.ext2");
    int rc = 0, hits;

    if (!damage_mount(img, 0, 0, "the unread superblock's volume mounts")) {
        return;
    }
    stop_records = super_records = 0;
    hits = policy_unread(0, policy_sync, &rc);
    check(hits > 0 && rc == -EIO, "a sync meets the superblock's block unread and fails with EIO");
    check(stop_records == 1 && super_records == 2 &&
              streq(last_warn, "superblock cannot be stored: the error stays in memory") &&
              (mnt.flags & VFS_MS_RDONLY) != 0,
          "and reports the fault once, the mount read-only, the error bit not stored");
    if (stop_records != 1 || super_records != 2) {
        printf("  stops %d, superblock records %d, last warning '%s'\n", stop_records,
               super_records, last_warn);
    }
    unmount_image();
    check(disk_state(img) >= 0 && (disk_state(img) & 1) == 0,
          "and the volume stays not valid on the disk");
}

#endif
