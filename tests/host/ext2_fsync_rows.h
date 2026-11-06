/*
 * ext2_fsync_rows.h: rows of ext2_hosttest.c, which includes this file, for
 * the `fsync` and `fdatasync` of one file in rach2/sys/fs/ext2.c on a mount
 * that writes back: what an fsync beside a large dirty file writes, a stop
 * right after one, and beside a neighbour in its inode table block that
 * allocated, the whole device once a file's list of shared blocks overflows
 * or goes with its node, an fdatasync after a change of times alone, a
 * second call made while the first waits for the disk, a neighbour's batch
 * that fills during that wait, and one block that fails.
 */
#ifndef EXT2_FSYNC_ROWS_H_
#define EXT2_FSYNC_ROWS_H_

static int copy_file(const char *src, const char *dst);

/* The blocks of the rows' 16 MiB volumes, and for each, at the last
 * `fsync_note`, whether it was dirty in the cache and the inode it was
 * tagged for, 0 for a shared block. */
#define FSYNC_BLOCKS 4096
static unsigned char fsync_dirty[FSYNC_BLOCKS];
static uint64_t fsync_owner[FSYNC_BLOCKS];

/** Note which blocks of the device are dirty in the cache, and their tags. */
static void fsync_note(void)
{
    uint64_t blk;

    for (blk = 0; blk < FSYNC_BLOCKS; blk++) {
        struct bbuf *b = bcache_cached(&dev, blk) ? bcache_get(&dev, blk) : 0;

        fsync_dirty[blk] = b && b->dirty;
        fsync_owner[blk] = b && b->dirty ? b->owner : 0;
        if (b) {
            bcache_release(b);
        }
    }
}

/** How many blocks tagged as inode `ino`'s are dirty in the cache. */
static uint32_t fsync_owned_dirty(uint64_t ino)
{
    uint32_t n = 0;
    uint64_t blk;

    for (blk = 0; blk < FSYNC_BLOCKS; blk++) {
        struct bbuf *b = bcache_cached(&dev, blk) ? bcache_get(&dev, blk) : 0;

        n += b && b->dirty && b->owner == ino;
        if (b) {
            bcache_release(b);
        }
    }
    return n;
}

/** The inode number of the held node `node`, or 0. */
static uint64_t fsync_ino(void *node)
{
    struct vfs_stat st;

    return OPS->getattr(&mnt, node, &st) == 0 ? st.ino : 0;
}

/** Whether the write log covers block `blk`. */
static int fsync_written(uint64_t blk)
{
    return first_write_of(blk) >= 0;
}

/**
 * Whether every block the write log covers was dirty at the last
 * `fsync_note`, tagged for inode `ino` or one of the `nshared` blocks of
 * `shared`, and the log did not overflow. Prints the first other block.
 */
static int fsync_wrote_only(uint64_t ino, const uint64_t *shared, int nshared)
{
    int i, k;

    for (i = 0; i < wlog_n; i++) {
        uint64_t blk;

        for (blk = wlog_lba[i] / 8; blk <= (wlog_lba[i] + wlog_count[i] - 1) / 8; blk++) {
            int ok = blk < FSYNC_BLOCKS && fsync_dirty[blk] && fsync_owner[blk] == ino;

            for (k = 0; !ok && k < nshared; k++) {
                ok = blk == shared[k] && fsync_dirty[blk];
            }
            if (!ok) {
                printf("  the fsync wrote block %lu\n", (unsigned long)blk);
                return 0;
            }
        }
    }
    return !wlog_overflow;
}

/**
 * The block of inode `ino`'s slot on the write rows' volume, of 128-byte
 * inodes, from what debugfs says of its groups; 0 when it says nothing.
 */
static uint64_t fsync_table_block(uint64_t ino)
{
    uint64_t ipg[1], tables[64];
    int n = debugfs_numbers("stats", "Inodes per group:", ipg, 1);
    int groups = debugfs_numbers("stats", "inode table at ", tables, 64);
    uint64_t g;

    if (n != 1 || ipg[0] == 0) {
        return 0;
    }
    g = (ino - 1) / ipg[0];
    return (int)g < groups ? tables[g] + ((ino - 1) % ipg[0]) * 128 / 4096 : 0;
}

/**
 * Mount the write rows' volume, made by mke2fs with `per_group` blocks in a
 * group or its own count for 0, without `dirsync`, so that a change stays
 * dirty in the cache until a sync. Whether it mounted writable.
 */
static int fsync_mount(unsigned long per_group)
{
    int rc;

    wbytes = (uint64_t)16 << 20;
    ext2_fixture_groups(per_group);
    rc = ext2_fixture_image(wimg, 4096, 128, PROFILE, 16, fix_path("tree"));
    ext2_fixture_groups(0);
    if (!rc) {
        printf("ext2_hosttest: mke2fs failed for %s\n", wimg);
        exit(1);
    }
    use_image(wimg, wbytes);
    rw_flags = 0;
    rc = mount_rw();
    rw_flags = VFS_MS_DIRSYNC;
    check(rc == 0 && (mnt.flags & VFS_MS_RDONLY) == 0, "the fsync volume mounts without dirsync");
    return rc == 0;
}

/**
 * Make 32 empty files and put them again, so that the next inode made is
 * in another table block than the last inode made before them.
 */
static void fsync_pad(void)
{
    int i, rc;

    for (i = 0; i < 32; i++) {
        char name[4];
        void *f;

        name[0] = 'p';
        name[1] = (char)('a' + i / 26);
        name[2] = (char)('a' + i % 26);
        name[3] = 0;
        f = make_file(mnt.root, name, &rc);
        check(f != 0, "an empty file between two table blocks is made");
        if (f) {
            OPS->put(&mnt, f);
        }
    }
}

/**
 * An fsync of a file that grew by four blocks, beside a neighbour of 150
 * dirty blocks and the indirect block that names most of them, in another
 * table block: it writes the file's blocks in one run, on a device that
 * takes 32 blocks at once, its bitmap and descriptor block and its inode's
 * table block, four requests, and leaves every block of the neighbour
 * dirty. Both files' writes are drained into the cache first by a close,
 * which writes nothing, so the row sees which blocks were dirty and for whom.
 */
static void fsync_neighbour_row(void)
{
    static unsigned char next[150 * 4096], own[4 * 4096];
    uint64_t shared[34], ino, other;
    uint32_t neighbour;
    void *a, *b, *h = 0, *drain = 0;
    int rc, nshared, before;

    if (!fsync_mount(0)) {
        return;
    }
    nshared = debugfs_numbers("stats", "block bitmap at ", shared, 32);
    a = make_file(mnt.root, "own", &rc);
    fsync_pad();
    b = make_file(mnt.root, "next", &rc);
    memset(next, 0x3C, sizeof(next));
    memset(own, 0xC3, sizeof(own));
    check(a && b && OPS->open(&mnt, a, 2, &h) == 0 && OPS->write(&mnt, h, own, 4096, 0) == 4096 &&
              OPS->sync(&mnt) == 0,
          "a file of one block is on the medium");
    if (!a || !b || !h) {
        return;
    }
    ino = fsync_ino(a);
    other = fsync_ino(b);
    shared[nshared++] = 1;
    shared[nshared++] = fsync_table_block(ino);
    before = writes_made;
    check(write_at(b, next, sizeof(next), 0) == 0 &&
              OPS->write(&mnt, h, own, sizeof(own), 4096) == (int32_t)sizeof(own),
          "its neighbour writes 150 blocks and the file four more");
    check(OPS->open(&mnt, b, 2, &drain) == 0, "the neighbour opens again");
    if (drain) {
        OPS->release(&mnt, drain);
    }
    check(writes_made == before, "and a close drains both into the cache with no write");
    fsync_note();
    neighbour = fsync_owned_dirty(other);
    check(neighbour == 151 && fsync_owned_dirty(ino) == 4,
          "the neighbour's 151 blocks and the file's four are dirty and tagged");
    wlog_reset();
    writes_made = 0;
    dev.max_sectors = 256;
    rc = OPS->fsync(&mnt, h);
    dev.max_sectors = 0;
    check(rc == 0 && writes_made == 4,
          "the fsync makes four writes: the bitmap, the descriptors, the data and the inode");
    check(fsync_wrote_only(ino, shared, nshared) && fsync_owned_dirty(ino) == 0 &&
              fsync_written(shared[nshared - 1]),
          "of the file's own blocks and the shared blocks, its table block among them");
    check(fsync_owned_dirty(other) == neighbour, "and every block of the neighbour is still dirty");
    OPS->release(&mnt, h);
    OPS->put(&mnt, a);
    OPS->put(&mnt, b);
    check(unmount_rw() == 0 && e2fsck_clean(wimg), "the neighbour volume unmounts clean");
}

/**
 * Stop the device right after an fsync of open file `h` of node `a` beside
 * node `b`, so that nothing else reaches it, and judge the stop: e2fsck
 * finds the volume clean and the driver's check mounts a copy writable and
 * clean, which the caller reads and unmounts. Whether it mounted.
 */
static int fsync_stop_judge(void *h, void *a, void *b)
{
    int status;

    stop_after = writes_made;
    bcache_device_gone(&dev);
    OPS->release(&mnt, h);
    OPS->put(&mnt, a);
    OPS->put(&mnt, b);
    OPS->put(&mnt, mnt.root);
    OPS->unmount(&mnt);
    mnt.root = 0;
    stop_after = -1;
    dev.flags = BLOCK_FAULT_SAFE;
    use_image(wimg, wbytes);
    status = e2fsck_status(wimg);
    check(status == 0, "e2fsck -fn finds the stop clean");
    if (status != 0) {
        ext2_fixture_head(fix_path("fsck.out"), 20);
    }
    last_check[0] = 0;
    check(copy_file(wimg, fix_path("check.ext2")), "the stop is copied for the check");
    use_image(fix_path("check.ext2"), wbytes);
    status =
        mount_image() == 0 && (mnt.flags & VFS_MS_RDONLY) == 0 && streq(last_check, "check: clean");
    check(status, "the driver's check mounts the stop writable and clean");
    return status;
}

/**
 * A stop right after an fsync: a file of four blocks grows by twenty and its
 * neighbour's forty blocks are rewritten, then the file is synced and the
 * device goes, so that nothing else reaches it. e2fsck finds the volume
 * clean, the driver's check mounts it writable, and the file reads back
 * whole beside the neighbour's old bytes.
 */
static void fsync_stop_row(void)
{
    static unsigned char old[40 * 4096], newer[40 * 4096], own[24 * 4096];
    void *a, *b, *h = 0;
    int rc;

    if (!fsync_mount(0)) {
        return;
    }
    memset(old, 0x51, sizeof(old));
    memset(newer, 0x15, sizeof(newer));
    memset(own, 0x77, sizeof(own));
    b = make_file(mnt.root, "next", &rc);
    a = make_file(mnt.root, "own", &rc);
    check(a && b && write_at(b, old, sizeof(old), 0) == 0 && write_at(a, own, 4 * 4096, 0) == 0 &&
              OPS->sync(&mnt) == 0,
          "a file of four blocks and a neighbour of forty are on the medium");
    check(a && OPS->open(&mnt, a, 2, &h) == 0, "the file opens");
    if (!a || !b || !h) {
        return;
    }
    check(write_at(b, newer, sizeof(newer), 0) == 0 &&
              OPS->write(&mnt, h, own + 4 * 4096, 20 * 4096, 4 * 4096) == 20 * 4096,
          "the neighbour is rewritten and the file grows by twenty blocks");
    check(OPS->fsync(&mnt, h) == 0, "the file is synced");
    fsync_stop_judge(h, a, b);
    check(file_is("/own", own, sizeof(own)) && file_is("/next", old, sizeof(old)),
          "with the file whole and the neighbour's old bytes");
    unmount_image();
}

/**
 * A stop right after an fsync of an unchanged file, beside a file of its
 * inode table block that grew by twenty blocks, one of them a pointer block:
 * the table block puts the neighbour's pointers on the device, so its
 * bitmap, descriptor and blocks go first, and the stop reads back whole.
 * The same beside a new directory, whose entries carry no tag, which writes
 * the whole device.
 */
static void fsync_table_row(int directory)
{
    static unsigned char own[4096], next[20 * 4096];
    void *a, *b, *h = 0;
    int rc;

    if (!fsync_mount(0)) {
        return;
    }
    memset(own, 0x3A, sizeof(own));
    memset(next, 0xA3, sizeof(next));
    a = make_file(mnt.root, "own", &rc);
    b = make_file(mnt.root, "next", &rc);
    check(a && b && write_at(a, own, sizeof(own), 0) == 0 && OPS->sync(&mnt) == 0 &&
              OPS->open(&mnt, a, 2, &h) == 0,
          "a file of one block and an empty neighbour are on the medium");
    if (!a || !b || !h) {
        return;
    }
    if (directory) {
        OPS->lock_node(&mnt, mnt.root);
        check(OPS->mkdir(&mnt, mnt.root, "made", 0755) == 0, "a new directory is beside the file");
        OPS->unlock_node(&mnt, mnt.root);
    } else {
        check(write_at(b, next, sizeof(next), 0) == 0, "the neighbour grows by twenty blocks");
    }
    check(OPS->fsync(&mnt, h) == 0, "the unchanged file is synced");
    if (!fsync_stop_judge(h, a, b)) {
        unmount_image();
        return;
    }
    check(file_is("/own", own, sizeof(own)), "with the file whole");
    check(directory || file_is("/next", next, sizeof(next)), "and the neighbour whole");
    unmount_image();
}

/**
 * The whole device instead of one file's blocks: on a volume of 1 MiB
 * groups, an fsync of a file of eight MiB, whose bitmaps outnumber its list
 * of shared blocks, writes a dirty neighbour too; so does the next fsync of
 * any file after a node with a list was freed; and the fsync after that
 * writes the file alone again.
 */
static void fsync_whole_row(void)
{
    static unsigned char chunk[64 * 1024], next[8 * 4096];
    void *a, *b, *c, *h = 0;
    uint64_t other;
    int rc, i, wrote = 1;

    if (!fsync_mount(256)) {
        return;
    }
    memset(chunk, 0x2E, sizeof(chunk));
    memset(next, 0x6B, sizeof(next));
    a = make_file(mnt.root, "own", &rc);
    b = make_file(mnt.root, "next", &rc);
    c = make_file(mnt.root, "gone", &rc);
    check(a && b && c && write_at(b, next, sizeof(next), 0) == 0 && OPS->sync(&mnt) == 0 &&
              OPS->open(&mnt, a, 2, &h) == 0,
          "a neighbour of eight blocks is on the medium");
    if (!a || !b || !c || !h) {
        return;
    }
    other = fsync_ino(b);
    for (i = 0; i < 8 * 16; i++) {
        wrote &= OPS->write(&mnt, h, chunk, sizeof(chunk), (uint64_t)i * sizeof(chunk)) ==
                 (int32_t)sizeof(chunk);
    }
    check(wrote && write_at(b, next, sizeof(next), 0) == 0 && fsync_owned_dirty(other) == 8,
          "a file of eight MiB spans more than eight groups, and the neighbour is rewritten");
    check(OPS->fsync(&mnt, h) == 0 && fsync_owned_dirty(other) == 0,
          "an fsync of the file with its list overflowed writes the neighbour too");
    check(write_at(c, next, sizeof(next), 0) == 0, "another file takes eight blocks");
    OPS->put(&mnt, c);
    check(write_at(b, next, sizeof(next), 0) == 0 && fsync_owned_dirty(other) == 8,
          "its node goes with its list, and the neighbour is rewritten");
    check(OPS->fsync(&mnt, h) == 0 && fsync_owned_dirty(other) == 0,
          "the next fsync of the file writes the neighbour too");
    check(write_at(b, next, sizeof(next), 0) == 0 && OPS->fsync(&mnt, h) == 0 &&
              fsync_owned_dirty(other) == 8,
          "and the one after leaves the neighbour dirty");
    OPS->release(&mnt, h);
    OPS->put(&mnt, a);
    OPS->put(&mnt, b);
    check(unmount_rw() == 0 && e2fsck_clean(wimg), "the overflow volume unmounts clean");
}

/**
 * fdatasync and the inode's table block: after a rewrite inside the size
 * and after a change of the times alone it writes no table block, after a
 * write that grows the file it does, and an fsync after a rewrite does.
 */
static void fsync_data_row(void)
{
    unsigned char block[4096];
    struct vfs_stat st;
    uint64_t table;
    void *a, *h = 0;
    int rc;

    if (!fsync_mount(0)) {
        return;
    }
    memset(block, 0x4D, sizeof(block));
    a = make_file(mnt.root, "own", &rc);
    check(a && OPS->open(&mnt, a, 2, &h) == 0 && OPS->write(&mnt, h, block, 4096, 0) == 4096 &&
              OPS->write(&mnt, h, block, 4096, 4096) == 4096 && OPS->fsync(&mnt, h) == 0,
          "a file of two blocks is synced");
    if (!a || !h) {
        return;
    }
    table = fsync_table_block(fsync_ino(a));
    clock_seconds += 10;
    wlog_reset();
    writes_made = 0;
    check(OPS->write(&mnt, h, block, 4096, 4096) == 4096 && OPS->fdatasync(&mnt, h) == 0 &&
              writes_made == 1 && !fsync_written(table),
          "fdatasync after a rewrite writes the data block and no table block");
    clock_seconds += 10;
    memset(&st, 0, sizeof(st));
    st.mtime_sec = clock_seconds;
    wlog_reset();
    writes_made = 0;
    check(OPS->setattr(&mnt, a, VFS_SET_MTIME, &st) == 0 && OPS->fdatasync(&mnt, h) == 0 &&
              writes_made == 0,
          "and after a change of the times alone writes nothing");
    wlog_reset();
    check(OPS->write(&mnt, h, block, 4096, 8192) == 4096 && OPS->fdatasync(&mnt, h) == 0 &&
              fsync_written(table),
          "fdatasync after a write that grows the file writes the table block");
    clock_seconds += 10;
    wlog_reset();
    check(OPS->write(&mnt, h, block, 4096, 0) == 4096 && OPS->fsync(&mnt, h) == 0 &&
              fsync_written(table),
          "and fsync after a rewrite writes it for the times");
    OPS->release(&mnt, h);
    OPS->put(&mnt, a);
    check(unmount_rw() == 0 && e2fsck_clean(wimg), "the fdatasync volume unmounts clean");
}

/* A row of two calls. Process A makes `sync` or an fsync of the file that
   grew, process B an fsync of its neighbour in the table block or an
   fdatasync of that file; `what` names it and `want` the wait B must be in
   at A's request. */
struct beside_row {
    const char *what;
    const char *want;
    int device;   /* process A syncs the device, not the file */
    int same;     /* process B's call is an fdatasync of the grown file */
    int at_table; /* B runs at the write of the table block, not the first */
    int fail;     /* A's first write fails, once B has run */
};

/* The run under way: the row, B's handle and result, the table block, and
   what B did at A's request: whether it ran, had ended, the kind of its
   wait, whether the table block was then on the medium, and, when it had
   ended, whether e2fsck found the medium as it stood then clean. */
static const struct beside_row *beside;
static void *beside_handle;
static uint64_t beside_table;
static int beside_rc, beside_fired, beside_ended, beside_table_written, beside_fsck;
static const char *beside_slept;

/** Process B: its call. */
static void beside_second(void)
{
    beside_rc =
        beside->same ? OPS->fdatasync(&mnt, beside_handle) : OPS->fsync(&mnt, beside_handle);
}

/** Whether e2fsck finds a copy of the medium, as it stands, clean. */
static int beside_medium_clean(void)
{
    int status;

    fflush(image);
    check(copy_file(wimg, fix_path("beside.ext2")), "the medium is copied for e2fsck");
    status = e2fsck_status(fix_path("beside.ext2"));
    if (status != 0) {
        ext2_fixture_head(fix_path("fsck.out"), 20);
    }
    return status == 0;
}

/** At the request the row names, give process B the turn and note what it
 * did. */
static void beside_pause(void)
{
    if (current_proc != &proc_a || second_inside || beside_fired ||
        (beside->at_table && request_block != beside_table)) {
        return;
    }
    beside_fired = 1;
    pass_turn();
    beside_ended = !turns_active();
    beside_slept = second_wait;
    beside_table_written = fsync_written(beside_table) && !beside->at_table;
    beside_fsck = !beside_ended || beside_medium_clean();
    if (beside->fail) {
        stop_after = writes_made;
    }
}

/**
 * Two calls at once: process A's call, stopped at a disk wait with the file
 * that grew unwritten, must not let process B's call return, or write the
 * table block that points at that file's blocks, before those blocks are
 * written. Then both end, with the first failure when the row has one, and a
 * stop leaves a volume e2fsck and the driver's check find clean.
 */
static void fsync_beside_row(const struct beside_row *row)
{
    static unsigned char own[4096], next[20 * 4096];
    char what[160];
    void *a, *b, *ha = 0, *hb = 0;
    int rc, a_rc;

    if (!fsync_mount(0)) {
        return;
    }
    memset(own, 0x3A, sizeof(own));
    memset(next, 0xA3, sizeof(next));
    a = make_file(mnt.root, "own", &rc);
    b = make_file(mnt.root, "next", &rc);
    check(a && b && write_at(a, own, sizeof(own), 0) == 0 && OPS->sync(&mnt) == 0 &&
              OPS->open(&mnt, a, 2, &ha) == 0 && OPS->open(&mnt, b, 2, &hb) == 0,
          "a file of one block and an empty neighbour are on the medium");
    if (!a || !b || !ha || !hb) {
        return;
    }
    check(OPS->write(&mnt, hb, next, sizeof(next), 0) == (int32_t)sizeof(next),
          "the neighbour grows by twenty blocks");
    wlog_reset();
    beside = row;
    beside_handle = row->same ? hb : ha;
    beside_table = fsync_table_block(fsync_ino(b));
    beside_rc = beside_fired = beside_ended = beside_table_written = 0;
    beside_slept = 0;
    beside_fsck = 1;
    write_pause = beside_pause;
    start_turns(beside_second);
    a_rc = row->device ? OPS->sync(&mnt) : OPS->fsync(&mnt, hb);
    write_pause = 0;
    finish_turns();
    snprintf(what, sizeof(what), "%s: waits for %s, not %s", row->what, row->want,
             beside_ended ? "nothing" : (beside_slept ? beside_slept : "anything"));
    check(beside_fired && !beside_ended && beside_slept && streq(beside_slept, row->want), what);
    check(!beside_table_written && beside_fsck, "and the medium points at no unwritten block");
    check(row->fail ? a_rc == -EIO && beside_rc == -EIO && !fsync_written(beside_table)
                    : a_rc == 0 && beside_rc == 0,
          "and both calls end with the right result");
    OPS->release(&mnt, hb);
    stop_after = -1;
    if (fsync_stop_judge(ha, a, b)) {
        check(row->fail || file_is("/next", next, sizeof(next)), "with the neighbour whole");
        unmount_image();
    }
}

/* The neighbour's handle and node, and its bytes, for process B of a row of
   `fsync_pending_row`, the file's table block, and what B did. */
static void *pending_handle, *pending_node;
static unsigned char pending_bytes[20 * 4096];
static uint64_t pending_table;
static int pending_fired, pending_ended;

/** The neighbour's bytes once its growth is on the medium. */
static const unsigned char *pending_whole(void)
{
    static unsigned char whole[21 * 4096];

    memset(whole, 0x3A, 4096);
    __builtin_memcpy(whole + 4096, pending_bytes, sizeof(pending_bytes));
    return whole;
}

/** Process B: the neighbour grows by twenty blocks, which stay in the batch's
 * images, and a change of its mode stores its inode. */
static void pending_second(void)
{
    struct vfs_stat st;

    memset(&st, 0, sizeof(st));
    st.mode = 0640;
    OPS->write(&mnt, pending_handle, pending_bytes, sizeof(pending_bytes), 4096);
    OPS->setattr(&mnt, pending_node, VFS_SET_MODE, &st);
}

/** At process A's first request past the table block, of the file's data,
 * whose buffer B has no use for, give B the turn. */
static void pending_pause(void)
{
    if (current_proc != &proc_a || second_inside || pending_fired ||
        request_block <= pending_table) {
        return;
    }
    pending_fired = 1;
    pass_turn();
    pending_ended = !turns_active();
}

/**
 * A neighbour's batch that fills while a sync sleeps: process A's fsync of
 * the file, or sync of the device, stops at the file's data, process B
 * appends to a neighbour in the table block and chmods it, and A goes on. A
 * stop after A's call finds no pointer to a block whose data and bit are in
 * the batch's images: `device` leaves them there, and an fsync drains them
 * and writes them first.
 */
static void fsync_pending_row(int device)
{
    static unsigned char own[2 * 4096];
    void *a, *b, *ha = 0, *hb = 0;
    int rc, a_rc;

    if (!fsync_mount(0)) {
        return;
    }
    memset(own, 0x3A, sizeof(own));
    memset(pending_bytes, 0xA3, sizeof(pending_bytes));
    a = make_file(mnt.root, "own", &rc);
    b = make_file(mnt.root, "next", &rc);
    check(a && b && write_at(a, own, 4096, 0) == 0 && write_at(b, own, 4096, 0) == 0 &&
              OPS->sync(&mnt) == 0 && OPS->open(&mnt, a, 2, &ha) == 0 &&
              OPS->open(&mnt, b, 2, &hb) == 0,
          "two files of one block are on the medium");
    if (!a || !b || !ha || !hb) {
        return;
    }
    check(OPS->write(&mnt, ha, own, 4096, 4096) == 4096, "the file grows by a block");
    pending_handle = hb;
    pending_node = b;
    pending_table = fsync_table_block(fsync_ino(b));
    pending_fired = pending_ended = 0;
    write_pause = pending_pause;
    start_turns(pending_second);
    a_rc = device ? OPS->sync(&mnt) : OPS->fsync(&mnt, ha);
    write_pause = 0;
    finish_turns();
    check(pending_fired && pending_ended && a_rc == 0,
          device ? "a neighbour's batch fills during a sync, which succeeds"
                 : "a neighbour's batch fills during an fsync, which succeeds");
    OPS->release(&mnt, hb);
    if (fsync_stop_judge(ha, a, b)) {
        check(file_is("/own", own, sizeof(own)), "and the synced file reads back whole");
        check(device ? file_is("/next", own, 4096) : file_is("/next", pending_whole(), 21 * 4096),
              device ? "and the neighbour as it was on the medium" : "and the neighbour whole");
        unmount_image();
    }
}

/* The file process B syncs and what it did, for `fsync_failed_row`. */
static void *failed_handle;
static int failed_rc, failed_fired, failed_ended;
static const char *failed_slept;

/** Process B: an fsync of the neighbour. */
static void failed_second(void)
{
    failed_rc = OPS->fsync(&mnt, failed_handle);
}

/** At process A's first request, give B the turn and note its wait. */
static void failed_pause(void)
{
    if (current_proc != &proc_a || second_inside || failed_fired) {
        return;
    }
    failed_fired = 1;
    pass_turn();
    failed_ended = !turns_active();
    failed_slept = second_wait;
}

/**
 * One block that fails, with every other write going through. A file grew by
 * twenty blocks in a table block it shares with a neighbour, and the highest
 * of them fails. Either the file's fsync fails with the neighbour's fsync
 * waiting on its list, or a sync of the device fails with nobody waiting.
 * The calls end in -EIO, a retry of the device while the block still fails
 * too, and no write has reached the table block. Once the block heals, a
 * sync writes the table block after it and the stop reads back whole.
 */
static void fsync_failed_row(int device)
{
    static unsigned char own[4096], next[20 * 4096];
    void *a, *b, *ha = 0, *hb = 0;
    uint64_t table, blk;
    int rc, a_rc;

    if (!fsync_mount(0)) {
        return;
    }
    memset(own, 0x3A, sizeof(own));
    memset(next, 0xA3, sizeof(next));
    a = make_file(mnt.root, "own", &rc);
    b = make_file(mnt.root, "next", &rc);
    check(a && b && write_at(a, own, sizeof(own), 0) == 0 && OPS->sync(&mnt) == 0 &&
              OPS->open(&mnt, a, 2, &ha) == 0 && OPS->open(&mnt, b, 2, &hb) == 0,
          "a file of one block and an empty neighbour are on the medium");
    if (!a || !b || !ha || !hb) {
        return;
    }
    check(write_at(b, next, sizeof(next), 0) == 0, "the neighbour grows by twenty blocks");
    table = fsync_table_block(fsync_ino(b));
    fsync_note();
    for (blk = 0; blk < FSYNC_BLOCKS; blk++) {
        if (fsync_owner[blk] == fsync_ino(b)) {
            fail_block = (int64_t)blk;
        }
    }
    check(fail_block > (int64_t)table, "its blocks lie past the table block");
    wlog_reset();
    failed_handle = ha;
    failed_rc = failed_fired = failed_ended = 0;
    failed_slept = 0;
    if (device) {
        a_rc = OPS->sync(&mnt);
    } else {
        write_pause = failed_pause;
        start_turns(failed_second);
        a_rc = OPS->fsync(&mnt, hb);
        write_pause = 0;
        finish_turns();
        check(failed_fired && !failed_ended && failed_slept &&
                  streq(failed_slept, "ext2 node state"),
              "the neighbour's fsync waits for the file's list");
        check(failed_rc == -EIO, "and fails with it");
    }
    check(a_rc == -EIO, "the call fails");
    check(!fsync_written(table), "and the table block stays off the medium");
    check(OPS->sync(&mnt) == -EIO && OPS->fsync(&mnt, ha) == -EIO && !fsync_written(table),
          "and a retry of the device, with the block still failing, leaves it off too");
    fail_block = -1;
    wlog_reset();
    check(OPS->sync(&mnt) == 0, "a sync after the block heals succeeds");
    check(fsync_written(table) && first_write_of(table) == wlog_n - 1,
          "and writes the table block last");
    OPS->release(&mnt, hb);
    if (fsync_stop_judge(ha, a, b)) {
        check(file_is("/next", next, sizeof(next)), "with the file whole");
        unmount_image();
    }
}

/** The rows of one file's fsync and fdatasync, under a cache of 8192
 * buffers, so that no row's dirty blocks reach the quarter of the limit at
 * which a drain writes the device's (`em_balance`). */
static void fsync_rows(void)
{
    bcache_set_limit(8192);
    printf("ext2-fsync: cache of %u buffers\n", (unsigned)bcache_capacity());
    fsync_neighbour_row();
    fsync_table_row(0);
    fsync_table_row(1);
    fsync_stop_row();
    fsync_whole_row();
    fsync_data_row();
    {
        static const struct beside_row rows[] = {
            {"an fsync beside a list being written", "ext2 node state", 0, 0, 0, 0},
            {"a failing fsync beside a list being written", "ext2 node state", 0, 0, 0, 1},
            {"an fdatasync of a list being written", "ext2 node state", 0, 1, 0, 0},
            {"an fdatasync of a table block being written", "buffer", 0, 1, 1, 0},
            {"an fsync beside a sync of the device", "buffer", 1, 0, 0, 0},
        };
        int i;

        for (i = 0; i < (int)(sizeof(rows) / sizeof(rows[0])); i++) {
            fsync_beside_row(&rows[i]);
        }
    }
    fsync_pending_row(0);
    fsync_pending_row(1);
    fsync_failed_row(0);
    fsync_failed_row(1);
    bcache_set_limit(0);
}

#endif
