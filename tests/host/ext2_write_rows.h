/*
 * ext2_write_rows.h: the rows of ext2's file writes without the big lock,
 * included by ext2_hosttest.c: two processes write, append, truncate,
 * fallocate and sync one file, the second run inside the first's disk
 * request, with the big lock reported free (compat/bkl_held.c), as the file
 * calls run on a type that sets `writes_unlocked`.
 *
 * The calls take the node's lock as the VFS does: an O_APPEND write reads
 * the size and writes there under it (`file_write`, syscall_fs.c), as
 * Linux's generic_file_write_iter does under the inode's lock
 * (mm/filemap.c:4191-4206 in 6.12); a truncation runs under it, as
 * do_truncate takes the inode's lock (fs/open.c:63-65); and the driver's
 * write and fallocate take it themselves. An fsync takes no lock of the
 * node's, as Linux's ext2_fsync takes none (fs/ext2/file.c:153-163). With
 * the node's lock made a no-op each of the first three rows fails: the
 * second process sleeps instead on the buffer the first is loading, which
 * orders the two only while the first holds it, and the second O_APPEND
 * record lands on the first's bytes.
 */

#ifndef EXT2_WRITE_ROWS_H_
#define EXT2_WRITE_ROWS_H_

/* The row's file, process B's description of it and its call's result, and
   what B did at A's request: whether it ran, whether it had ended, and the
   kind of the first wait it slept in. */
static void *wr_node, *wr_b_handle;
static int wr_b_rc, wr_fired, wr_ended;
static const char *wr_slept;
static unsigned char wr_b_bytes[4096 + 100];

/** At process A's first request, give process B the turn and note what it
 * did. */
static void wr_pause(void)
{
    if (current_proc != &proc_a || second_inside || wr_fired) {
        return;
    }
    wr_fired = 1;
    pass_turn();
    wr_ended = !turns_active();
    wr_slept = second_wait;
}

/** Ready a row's two processes: nothing done yet, A paused at its first read
 * of the disk, or its first write when `at_write`, and the big lock reported
 * free. */
static void wr_arm(void (*body)(void), int at_write)
{
    wr_b_rc = -1;
    wr_fired = wr_ended = 0;
    wr_slept = 0;
    if (at_write) {
        write_pause = wr_pause;
    } else {
        read_pause = wr_pause;
    }
    bkl_host_free = 1;
    bkl_host_takes = 0;
    start_turns(body);
}

/** End a row's two processes, and check that A ran B at its request and that
 * neither kept a take of the big lock. */
static void wr_end(const char *what)
{
    finish_turns();
    read_pause = 0;
    write_pause = 0;
    check(wr_fired && !wr_ended, what);
    check(!bkl_held(), "and every take of the big lock is given back");
    bkl_host_free = 0;
}

/** Drop every block from the cache, so that the next call reads from the
 * disk. Whether the cache emptied. */
static int wr_cold(void)
{
    return OPS->sync(&mnt) == 0 && bcache_sync(&dev) == 0 && bcache_invalidate(&dev) == 0;
}

/**
 * Append `len` bytes of `buf` to the file of `node` through its description
 * `h`, as an O_APPEND write does: under the node's lock, read the size and
 * write there. The bytes written, or a negative errno.
 */
static int32_t wr_append(void *node, void *h, const void *buf, uint32_t len)
{
    struct vfs_stat st;
    int32_t n;

    OPS->lock_node(&mnt, node);
    n = OPS->getattr(&mnt, node, &st);
    if (n == 0) {
        n = OPS->write(&mnt, h, buf, len, st.size);
    }
    OPS->unlock_node(&mnt, node);
    return n;
}

/** Process B of `wr_append_row`: its record appended. */
static void wr_second_append(void)
{
    wr_b_rc = wr_append(wr_node, wr_b_handle, wr_b_bytes, sizeof(wr_b_bytes));
}

/** Process B of `wr_truncate_row` and `wr_fallocate_row`: a block written at
 * `wr_b_at` under the node's lock, as the VFS writes. */
static uint64_t wr_b_at;
static void wr_second_write(void)
{
    OPS->lock_node(&mnt, wr_node);
    wr_b_rc = OPS->write(&mnt, wr_b_handle, wr_b_bytes, 4096, wr_b_at);
    OPS->unlock_node(&mnt, wr_node);
}

/** Open the row's file twice for writing, `*ha` for process A and
 * `wr_b_handle` for B. Whether both opened. */
static int wr_open(void *node, void **ha)
{
    *ha = 0;
    wr_b_handle = 0;
    wr_node = node;
    return node && OPS->open(&mnt, node, 2, ha) == 0 && OPS->open(&mnt, node, 2, &wr_b_handle) == 0;
}

/** Release the row's descriptions and the file, unmount, and check that
 * `e2fsck -fn` finds the volume clean. */
static void wr_close(void *ha, const char *what)
{
    if (wr_b_handle) {
        OPS->release(&mnt, wr_b_handle);
    }
    if (ha) {
        OPS->release(&mnt, ha);
    }
    if (wr_node) {
        OPS->put(&mnt, wr_node);
    }
    wr_node = wr_b_handle = 0;
    check(unmount_rw() == 0 && e2fsck_clean(wimg), what);
}

/**
 * Two O_APPEND writers of one file through two descriptions: process A's
 * record, which takes a new block, reads the block bitmap from the disk
 * under the node's lock, and process B, run there, must sleep on that lock
 * and append after A's record. Without it B reads the old size and the two
 * records land on the same bytes.
 */
static void wr_append_row(void)
{
    static unsigned char first[4096 + 50], a_rec[4096 + 70],
        want[sizeof(first) + sizeof(a_rec) + sizeof(wr_b_bytes)];
    void *f, *ha = 0;
    int32_t a_rc;
    int rc;

    if (!fsync_mount(0)) {
        return;
    }
    memset(first, 'f', sizeof(first));
    memset(a_rec, 'a', sizeof(a_rec));
    memset(wr_b_bytes, 'b', sizeof(wr_b_bytes));
    f = make_file(mnt.root, "log", &rc);
    check(f && write_at(f, first, sizeof(first), 0) == 0 && wr_open(f, &ha) && wr_cold(),
          "an append row's file of a block and a part is on the medium");
    if (!ha || !wr_b_handle) {
        wr_close(ha, "the append row's volume is clean");
        return;
    }
    wr_arm(wr_second_append, 0);
    a_rc = wr_append(f, ha, a_rec, sizeof(a_rec));
    wr_end("two O_APPEND writes, the second inside the first's disk read");
    check(wr_slept && streq(wr_slept, "ext2 node"), "where it sleeps on the node's lock");
    memcpy(want, first, sizeof(first));
    memcpy(want + sizeof(first), a_rec, sizeof(a_rec));
    memcpy(want + sizeof(first) + sizeof(a_rec), wr_b_bytes, sizeof(wr_b_bytes));
    check(a_rc == (int32_t)sizeof(a_rec) && wr_b_rc == (int32_t)sizeof(wr_b_bytes) &&
              file_is("/log", want, sizeof(want)),
          "and both records follow the file's bytes whole, in the order of the lock");
    wr_close(ha, "and e2fsck -fn finds the append row's volume clean");
}

/**
 * A write racing a truncation: process A cuts a file of forty blocks to two
 * under the node's lock, reading its indirect block from the disk to free
 * what it names, and process B, run there, must sleep on that lock before
 * it writes block thirty. Then the file is the two blocks, a hole and B's
 * block, and no block is lost or named twice.
 */
static void wr_truncate_row(void)
{
    static unsigned char data[40 * 4096], want[31 * 4096];
    void *f, *ha = 0;
    int a_rc, rc;

    if (!fsync_mount(0)) {
        return;
    }
    memset(data, 0x5C, sizeof(data));
    memset(wr_b_bytes, 0xB7, sizeof(wr_b_bytes));
    f = make_file(mnt.root, "cut", &rc);
    check(f && write_at(f, data, sizeof(data), 0) == 0 && wr_open(f, &ha) && wr_cold(),
          "a truncate row's file of forty blocks is on the medium");
    if (!ha || !wr_b_handle) {
        wr_close(ha, "the truncate row's volume is clean");
        return;
    }
    wr_b_at = 30 * 4096;
    wr_arm(wr_second_write, 0);
    OPS->lock_node(&mnt, f);
    a_rc = OPS->truncate(&mnt, ha, 2 * 4096);
    OPS->unlock_node(&mnt, f);
    wr_end("a write of block thirty inside a truncation's disk read");
    check(wr_slept && streq(wr_slept, "ext2 node"), "where it sleeps on the node's lock");
    memset(want, 0, sizeof(want));
    memcpy(want, data, 2 * 4096);
    memcpy(want + 30 * 4096, wr_b_bytes, 4096);
    check(a_rc == 0 && wr_b_rc == 4096 && file_is("/cut", want, sizeof(want)),
          "and the file is its first two blocks, a hole and the block written after");
    wr_close(ha, "and e2fsck -fn finds no block lost or named twice");
}

/**
 * fallocate racing a write: process A fills a new file's first twenty
 * blocks with zeros, reading the block bitmap from the disk under the
 * node's lock for the first, and process B, run there, must sleep on that
 * lock before it writes the first block. Then the block holds B's bytes and
 * the rest zeros.
 */
static void wr_fallocate_row(void)
{
    static unsigned char want[20 * 4096];
    void *f, *ha = 0;
    int a_rc, rc;

    if (!fsync_mount(0)) {
        return;
    }
    memset(wr_b_bytes, 0x6D, sizeof(wr_b_bytes));
    f = make_file(mnt.root, "fill", &rc);
    check(f && wr_open(f, &ha) && wr_cold(), "a fallocate row's empty file is on the medium");
    if (!ha || !wr_b_handle) {
        wr_close(ha, "the fallocate row's volume is clean");
        return;
    }
    wr_b_at = 0;
    wr_arm(wr_second_write, 0);
    a_rc = OPS->fallocate(&mnt, f, 0, 0, sizeof(want));
    wr_end("a write of the first block inside fallocate's disk read");
    check(wr_slept && streq(wr_slept, "ext2 node"), "where it sleeps on the node's lock");
    memset(want, 0, sizeof(want));
    memcpy(want, wr_b_bytes, 4096);
    check(a_rc == 0 && wr_b_rc == 4096 && file_is("/fill", want, sizeof(want)),
          "and the file is the block written and nineteen of zeros");
    wr_close(ha, "and e2fsck -fn finds the fallocate row's volume clean");
}

/**
 * Whether the file at `path` is the `len` bytes of `want`, or those and the
 * `more` bytes of `extra` after them.
 */
static int wr_file_is_either(const char *path, const unsigned char *want, uint32_t len,
                             const unsigned char *extra, uint32_t more)
{
    int err, got;
    void *n = walk(path, &err);

    if (!n) {
        return 0;
    }
    got = read_at(n, abuf, sizeof(abuf), 0);
    OPS->put(&mnt, n);
    if (got != (int)len && got != (int)(len + more)) {
        printf("  %s: %d bytes, %u or %u wanted\n", path, got, (unsigned)len,
               (unsigned)(len + more));
        return 0;
    }
    return memcmp(abuf, want, len) == 0 &&
           (got == (int)len || memcmp(abuf + len, extra, more) == 0);
}

/**
 * fsync racing a write: process A syncs a file that grew by twenty blocks,
 * and process B, run at A's first write to the disk, appends a block to the
 * same file. The fsync holds no lock of the node's, so B must not sleep on
 * it, and both end. A stop of the device right after the fsync leaves a
 * volume e2fsck and the driver's check find clean, with the file's synced
 * blocks whole and B's block after them or not at all.
 */
static void wr_fsync_row(void)
{
    static unsigned char own[24 * 4096];
    void *f, *g, *ha = 0;
    int a_rc, rc;

    if (!fsync_mount(0)) {
        return;
    }
    memset(own, 0x2B, sizeof(own));
    memset(wr_b_bytes, 0xD4, sizeof(wr_b_bytes));
    g = make_file(mnt.root, "next", &rc);
    f = make_file(mnt.root, "own", &rc);
    check(f && g && write_at(f, own, 4 * 4096, 0) == 0 && OPS->sync(&mnt) == 0 && wr_open(f, &ha) &&
              OPS->write(&mnt, ha, own + 4 * 4096, 20 * 4096, 4 * 4096) == 20 * 4096,
          "an fsync row's file of four blocks on the medium grows by twenty");
    if (!f || !g || !ha || !wr_b_handle) {
        return;
    }
    wr_arm(wr_second_append, 1);
    a_rc = OPS->fsync(&mnt, ha);
    wr_end("an append of the file inside its fsync's disk write");
    check(!wr_slept || !streq(wr_slept, "ext2 node"), "which sleeps on no lock of the node's");
    check(a_rc == 0 && wr_b_rc == (int32_t)sizeof(wr_b_bytes), "and both calls succeed");
    OPS->release(&mnt, wr_b_handle);
    wr_b_handle = 0;
    wr_node = 0;
    if (fsync_stop_judge(ha, f, g)) {
        check(wr_file_is_either("/own", own, sizeof(own), wr_b_bytes, sizeof(wr_b_bytes)),
              "with the synced blocks whole, and the appended block after them or not at all");
        unmount_image();
    }
}

/** The rows of one file's writes, truncation, fallocate and fsync by two
 * processes without the big lock, under a cache of 8192 buffers, as the
 * fsync rows run, so that no writer's drain writes the device's blocks. */
static void write_lock_rows(void)
{
    bcache_set_limit(8192);
    wr_append_row();
    wr_truncate_row();
    wr_fallocate_row();
    wr_fsync_row();
    bcache_set_limit(0);
}

#endif
