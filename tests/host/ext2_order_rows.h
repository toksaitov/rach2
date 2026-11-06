/*
 * ext2_order_rows.h: rows of ext2_hosttest.c, which includes this file after
 * ext2_fsync_rows.h, for the order in which a mount that writes back puts
 * blocks on the device: a reloaded inode's table block after its data, and
 * a hole's data before the pointer blocks above it, at each depth.
 */
#ifndef EXT2_ORDER_ROWS_H_
#define EXT2_ORDER_ROWS_H_

/** The 32-bit word at index `at` of block `blk` of the write rows' image,
 * or 0 when it cannot be read. */
static uint32_t order_word(uint64_t blk, uint32_t at)
{
    unsigned char w[4];

    return host_read(wimg, w, 4, (long)(blk * 4096 + at * 4)) == 4 ? batch_word(w) : 0;
}

/** Pointer `slot` of inode `ino` on the write rows' image, of 128-byte
 * inodes, or 0. */
static uint32_t order_inode_word(uint64_t ino, uint32_t slot)
{
    uint64_t ipg[1], table = fsync_table_block(ino);

    if (!table || debugfs_numbers("stats", "Inodes per group:", ipg, 1) != 1 || ipg[0] == 0) {
        return 0;
    }
    return order_word(table, (uint32_t)(((ino - 1) % ipg[0] * 128 % 4096 + 40) / 4 + slot));
}

/**
 * Store in `chain`, from the inode down, the pointer blocks on the image
 * that lead to logical block `index` of inode `ino`, below the single and
 * double indirection, and the data block last. Returns how many, or 0 when
 * a pointer on the way is 0.
 */
static int order_chain(uint64_t ino, uint32_t index, uint64_t *chain)
{
    int n;

    if (index < 1036) {
        chain[0] = order_inode_word(ino, 12);
        chain[1] = chain[0] ? order_word(chain[0], index - 12) : 0;
        n = 2;
    } else {
        chain[0] = order_inode_word(ino, 13);
        chain[1] = chain[0] ? order_word(chain[0], (index - 1036) / 1024) : 0;
        chain[2] = chain[1] ? order_word(chain[1], (index - 1036) % 1024) : 0;
        n = 3;
    }
    return chain[n - 1] ? n : 0;
}

/**
 * A file with logical block `first` on the device and a hole below it, at
 * `fill`, under a pointer block that is on the device too. Filling the hole
 * dirties a new data block, and new pointer blocks between, which lie above
 * the old pointer block, so that a sync in block order reaches the old one
 * first. Each block on the way must reach the device after the one it names:
 * the data, then each pointer block up to the one on the device.
 */
static void order_hole_row(uint32_t first, uint32_t fill)
{
    static unsigned char data[4096];
    uint64_t chain[3], ino = 0;
    int n, i, ok, rc, before;
    void *f;

    if (!fsync_mount(0)) {
        return;
    }
    memset(data, 0x68, sizeof(data));
    f = make_file(mnt.root, "hole", &rc);
    check(f && write_at(f, data, 4096, (uint64_t)first * 4096) == 0 && OPS->fsync(&mnt, 0) == 0,
          "a file with one block past a hole, on the device");
    if (!f) {
        return;
    }
    ino = fsync_ino(f);
    memset(data, 0x69, sizeof(data));
    wlog_reset();
    before = writes_made;
    check(write_at(f, data, 4096, (uint64_t)fill * 4096) == 0 && writes_made == before,
          "the hole filled, in the cache");
    check(OPS->fsync(&mnt, 0) == 0, "and written by a sync of the device");
    OPS->put(&mnt, f);
    check(unmount_rw() == 0, "the hole's volume unmounts");
    n = order_chain(ino, fill, chain);
    ok = n > 0 && !wlog_overflow;
    for (i = 0; ok && i + 1 < n; i++) {
        int parent = first_write_of(chain[i]), child = last_write_of(chain[i + 1]);

        if (child < 0 || parent <= child) {
            printf("  block %lu written at %d, before block %lu it names, at %d\n",
                   (unsigned long)chain[i], parent, (unsigned long)chain[i + 1], child);
            ok = 0;
        }
    }
    check(ok, fill < 1036 ? "the data of a hole reaches the device before the indirect block"
                          : "the data of a hole, then its new indirect block, reach the device "
                            "before the double indirect block");
    check(e2fsck_clean(wimg), "and the volume is clean");
}

/**
 * A file of one block, whose node is freed with its list of shared blocks
 * while its data is dirty and loaded again: its table block must still wait
 * for that data. The data is read again, so that it is the most recently
 * used buffer and a shrink, which writes the oldest first, reaches the
 * table block before it.
 */
static void order_lost_row(void)
{
    static unsigned char data[4096];
    uint64_t ino, table, blk, data_blk = 0;
    uint32_t written = 0;
    int rc, err, at_data, at_table;
    void *f;

    if (!fsync_mount(0)) {
        return;
    }
    memset(data, 0x6c, sizeof(data));
    f = make_file(mnt.root, "lost", &rc);
    check(f && write_at(f, data, 4096, 0) == 0, "a file of one block, in the cache");
    if (!f) {
        return;
    }
    ino = fsync_ino(f);
    fsync_note();
    for (blk = 0; blk < FSYNC_BLOCKS; blk++) {
        if (fsync_dirty[blk] && fsync_owner[blk] == ino) {
            data_blk = blk;
        }
    }
    OPS->put(&mnt, f);
    f = walk("/lost", &err);
    check(f && read_at(f, abuf, 4096, 0) == 4096, "its node freed with its list and loaded again");
    table = fsync_table_block(ino);
    wlog_reset();
    bcache_shrink(bcache_resident(), 1, &written);
    at_data = first_write_of(data_blk);
    at_table = first_write_of(table);
    if (!data_blk || at_data < 0 || at_table <= at_data) {
        printf("  data block %lu written at %d, table block %lu at %d\n", (unsigned long)data_blk,
               at_data, (unsigned long)table, at_table);
    }
    check(data_blk && at_data >= 0 && at_table > at_data,
          "a shrink writes a reloaded inode's data before its table block");
    if (f) {
        OPS->put(&mnt, f);
    }
    check(unmount_rw() == 0 && e2fsck_clean(wimg), "and the volume unmounts clean");
}

/** The rows of the order of writing back, under a cache of 8192 buffers, as
 * the fsync rows have. */
static void writeback_order_rows(void)
{
    bcache_set_limit(8192);
    order_lost_row();
    order_hole_row(13, 12);
    order_hole_row(2060, 1036);
    bcache_set_limit(0);
}

#endif
