/*
 * ext2_group_rows.h: the rows of ext2's group locks, included by
 * ext2_hosttest.c: two processes allocate and free blocks and inodes in one
 * group of a volume of sixteen, the second run inside the first's disk
 * request under the group's lock, and the bitmaps, the descriptors and the
 * mount's free counts must agree after.
 *
 * Under the big lock two processes interleave only where one sleeps, and a
 * bitmap read through the cache is kept by its buffer, which a process holds
 * while it loads the block. A block bitmap the write batch has an image of
 * is read from the image instead, with no buffer, so a process that loads
 * the bitmap to write a bit under the group's lock leaves the image open to
 * another: the rows put a pending piece's image of the group's bitmap in
 * the batch, drop the bitmap from the cache and send the second process
 * into the group through a full one, whose search fetches nothing of it.
 * There it must sleep on the group's lock; without the lock it reads the
 * image, takes the bit the first is setting or rewrites the byte the first
 * is clearing, and a block is given twice or a free is lost.
 */

#ifndef EXT2_GROUP_ROWS_H_
#define EXT2_GROUP_ROWS_H_

#include "../../rach2/sys/fs/ext2_internal.h"

/* The blocks in each group of the rows' volume, 1 MiB at 4 KiB. */
#define GROUP_BLOCKS 256u

/* The full group the rows send both searches through, and the group after
 * it, where they meet. */
#define GROUP_FULL 2u
#define GROUP_MEET 3u

/* The cache block whose load by process A gives process B the turn, and
 * whether it did. */
static uint64_t group_pause_block;
static int group_paused;

/* Process B's directory, its operation's result, and the node it made. */
static void *group_b_dir;
static int group_b_rc;

/** The driver's record of the rows' mount. */
static struct ext2_mount *group_em(void)
{
    return mnt.priv;
}

/** At process A's load of `group_pause_block`, give process B the turn. */
static void group_turn(void)
{
    if (current_proc == &proc_a && request_block == group_pause_block) {
        read_pause = 0;
        group_paused = 1;
        pass_turn();
    }
}

/** Process B: a directory made in `group_b_dir`, whose block it allocates. */
static void group_second_mkdir(void)
{
    group_b_rc = OPS->mkdir(&mnt, group_b_dir, "sub", 0755);
}

/** The clear bits among the first `count` of `bits`. */
static uint32_t group_clear_bits(const uint8_t *bits, uint32_t count)
{
    uint32_t i, n = 0;

    for (i = 0; i < count; i++) {
        n += !((bits[i / 8] >> (i % 8)) & 1u);
    }
    return n;
}

/**
 * Whether every group's block and inode bitmaps have as many clear bits as
 * its descriptor counts free, and the descriptors' sums are the mount's free
 * counts. A group that differs is printed.
 */
static int group_totals_ok(void)
{
    static uint8_t bits[4096];
    struct ext2_mount *em = group_em();
    uint64_t blocks = 0, inodes = 0;
    uint32_t g;
    int ok = 1;

    for (g = 0; g < em->groups; g++) {
        const uint8_t *gd = em->gd + g * 32;
        uint32_t base = em->first_data + g * em->bpg;
        uint32_t count = em->blocks - base < em->bpg ? em->blocks - base : em->bpg;
        uint32_t fb, fi;

        if (ext2_read_bytes(em, (uint64_t)ext2_le32(gd) * em->block_size, bits, 4096) != 0) {
            return 0;
        }
        fb = group_clear_bits(bits, count);
        if (ext2_read_bytes(em, (uint64_t)ext2_le32(gd + 4) * em->block_size, bits, 4096) != 0) {
            return 0;
        }
        fi = group_clear_bits(bits, em->ipg);
        if (fb != ext2_le16(gd + 12) || fi != ext2_le16(gd + 14)) {
            printf("  group %u: %u and %u free in the bitmaps, %u and %u in the descriptor\n",
                   (unsigned)g, (unsigned)fb, (unsigned)fi, (unsigned)ext2_le16(gd + 12),
                   (unsigned)ext2_le16(gd + 14));
            ok = 0;
        }
        blocks += fb;
        inodes += fi;
    }
    if (blocks != em->free_blocks || inodes != em->free_inodes) {
        printf("  the bitmaps have %lu and %lu free, the mount counts %lu and %lu\n",
               (unsigned long)blocks, (unsigned long)inodes, (unsigned long)em->free_blocks,
               (unsigned long)em->free_inodes);
        ok = 0;
    }
    return ok;
}

/**
 * Mount a new volume of sixteen groups of GROUP_BLOCKS blocks without
 * `dirsync`, so that a write waits in the batch, and fill GROUP_FULL with a
 * file's blocks. Whether it mounted writable with the group full.
 */
static int group_volume(void)
{
    static uint8_t block[4096];
    struct ext2_mount *em;
    void *f;
    uint64_t off;
    int rc;

    wbytes = (uint64_t)16 << 20;
    ext2_fixture_groups(GROUP_BLOCKS);
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
    check(rc == 0 && (mnt.flags & VFS_MS_RDONLY) == 0, "the group volume mounts without dirsync");
    if (rc != 0) {
        return 0;
    }
    em = group_em();
    check(em->groups == 16 && em->bpg == GROUP_BLOCKS, "with sixteen groups of 1 MiB");
    em->alloc_hint = em->first_data + GROUP_FULL * em->bpg;
    f = make_file(mnt.root, "fill", &rc);
    for (off = 0; f && rc == 0 && ext2_le16(em->gd + GROUP_FULL * 32 + 12) != 0 &&
                  off < (uint64_t)GROUP_BLOCKS * 4096;
         off += 4096) {
        rc = write_at(f, block, sizeof(block), off);
    }
    if (f) {
        OPS->put(&mnt, f);
    }
    rc = rc == 0 && ext2_le16(em->gd + GROUP_FULL * 32 + 12) == 0;
    check(rc, "a file fills one group");
    return rc;
}

/**
 * Leave a pending piece of a new file `name` in the batch, whose block comes
 * from GROUP_MEET past the full group, so that the batch holds an image of
 * that group's bitmap; then point the hint at the full group again and drop
 * every block from the cache. The file's description, open, in `*handle`,
 * whose release drains the batch; NULL when a step failed.
 */
static void *group_pending(const char *name, void **handle)
{
    static uint8_t block[4096];
    struct ext2_mount *em = group_em();
    uint32_t bitmap = ext2_le32(em->gd + GROUP_MEET * 32);
    void *p;
    int32_t got = -1;
    int rc;

    *handle = 0;
    p = make_file(mnt.root, name, &rc);
    if (!p || OPS->sync(&mnt) != 0 || OPS->open(&mnt, p, 2, handle) != 0) {
        return p;
    }
    em->alloc_hint = em->first_data + GROUP_FULL * em->bpg;
    OPS->lock_node(&mnt, p);
    got = OPS->write(&mnt, *handle, block, sizeof(block), 0);
    OPS->unlock_node(&mnt, p);
    em->alloc_hint = em->first_data + GROUP_FULL * em->bpg;
    check(got == (int32_t)sizeof(block) && ext2_writeback_has(em, bitmap) == 2,
          "a pending piece holds the image of the next group's bitmap");
    check(bcache_sync(&dev) == 0 && bcache_invalidate(&dev) == 0 && !bcache_cached(&dev, bitmap),
          "and the bitmap leaves the cache");
    return p;
}

/** Drain the pending piece by releasing `handle`, put `p`, sync, and check
 * the totals, the volume after an unmount, and `e2fsck -fn`'s verdict. */
static void group_finish(void *p, void *handle, const char *what)
{
    if (handle) {
        OPS->release(&mnt, handle);
    }
    if (p) {
        OPS->put(&mnt, p);
    }
    check(OPS->sync(&mnt) == 0 && group_totals_ok(), what);
    check(unmount_rw() == 0 && e2fsck_clean(wimg), "and e2fsck -fn finds the volume clean");
}

/** The first block of the directory at `path`, or 0. */
static uint32_t group_dir_block(const char *path)
{
    int err;
    struct ext2_node *n = walk(path, &err);
    uint32_t b;

    if (!n) {
        return 0;
    }
    b = n->block[0];
    OPS->put(&mnt, n);
    return b;
}

/**
 * Two directories made at once, each allocating its block past the full
 * group: process A loads the next group's bitmap to set its bit under the
 * group's lock, and process B, run there, must sleep on that lock. Without
 * it B reads the batch's image, finds the bit A is setting still clear and
 * takes the same block, which two directories then name.
 */
static void group_alloc_row(void)
{
    struct ext2_mount *em;
    void *a = 0, *p = 0, *handle = 0;
    uint32_t ab, bb;
    int rc;

    if (!group_volume()) {
        return;
    }
    em = group_em();
    check(OPS->mkdir(&mnt, mnt.root, "ga", 0755) == 0 &&
              OPS->mkdir(&mnt, mnt.root, "gb", 0755) == 0,
          "two directories for the two processes");
    a = walk("/ga", &rc);
    group_b_dir = walk("/gb", &rc);
    p = group_pending("gp", &handle);
    if (!a || !group_b_dir || !handle) {
        group_finish(p, handle, "the allocation row's volume");
        return;
    }
    group_pause_block = ext2_le32(em->gd + GROUP_MEET * 32);
    group_paused = 0;
    group_b_rc = -1;
    read_pause = group_turn;
    start_turns(group_second_mkdir);
    rc = OPS->mkdir(&mnt, a, "sub", 0755);
    finish_turns();
    read_pause = 0;
    check(rc == 0 && group_b_rc == 0 && group_paused,
          "two directories are made, the second inside the first's bitmap load");
    check(second_wait && streq(second_wait, "ext2 group"), "where it sleeps on the group's lock");
    ab = group_dir_block("/ga/sub");
    bb = group_dir_block("/gb/sub");
    check(ab != 0 && bb != 0 && ab != bb, "and the two take two blocks");
    OPS->put(&mnt, a);
    OPS->put(&mnt, group_b_dir);
    group_finish(p, handle, "and each group's bitmaps and the free counts agree");
}

/** Process A of `group_free_row`: its file truncated to nothing. */
static int group_truncate(void *t)
{
    void *h = 0;
    int rc = OPS->open(&mnt, t, 2, &h);

    if (rc != 0) {
        return rc;
    }
    OPS->lock_node(&mnt, t);
    rc = OPS->truncate(&mnt, h, 0);
    OPS->unlock_node(&mnt, t);
    OPS->release(&mnt, h);
    return rc;
}

/**
 * A free and an allocation in one group at once: process A truncates a
 * file whose last block lies in one bitmap byte with the block a search
 * gives next, and loads the bitmap under the group's lock to clear its
 * bits; process B, run there, makes a directory, whose block it allocates
 * past the full group, and must sleep on the lock. Without it B reads the
 * byte from the batch's image before A clears it and writes it back after,
 * setting A's freed bits again for blocks no file names.
 */
static void group_free_row(void)
{
    static uint8_t data[3 * 4096];
    struct ext2_mount *em;
    void *t = 0, *p = 0, *handle = 0;
    uint32_t bitmap, base, first = 0, k = 1;
    int rc;

    if (!group_volume()) {
        return;
    }
    em = group_em();
    bitmap = ext2_le32(em->gd + GROUP_MEET * 32);
    base = em->first_data + GROUP_MEET * em->bpg;
    check(OPS->mkdir(&mnt, mnt.root, "gb", 0755) == 0 &&
              ext2_bit_first_clear(em, bitmap, 0, em->bpg, &first) == 0 && first < em->bpg,
          "a directory for process B, and the next group's first free block");
    /* The file's last block is bit first + k - 1, the pending piece's
     * first + k and B's first + k + 1: the first and the last in one byte. */
    while ((first + k) % 8 == 0 || (first + k) % 8 == 7) {
        k++;
    }
    em->alloc_hint = base + first;
    t = make_file(mnt.root, "gt", &rc);
    check(t && write_at(t, data, k * 4096, 0) == 0 &&
              ((struct ext2_node *)t)->block[k - 1] == base + first + k - 1,
          "a file of blocks before the next group's free run");
    group_b_dir = walk("/gb", &rc);
    p = group_pending("gp", &handle);
    if (!t || !group_b_dir || !handle) {
        group_finish(p, handle, "the free row's volume");
        return;
    }
    group_pause_block = bitmap;
    group_paused = 0;
    group_b_rc = -1;
    read_pause = group_turn;
    start_turns(group_second_mkdir);
    rc = group_truncate(t);
    finish_turns();
    read_pause = 0;
    check(rc == 0 && group_b_rc == 0 && group_paused,
          "a truncate and a new directory, the second inside the first's bitmap load");
    check(second_wait && streq(second_wait, "ext2 group"), "where it sleeps on the group's lock");
    OPS->put(&mnt, t);
    OPS->put(&mnt, group_b_dir);
    group_finish(p, handle, "and each group's bitmaps and the free counts agree");
}

/** Process B of `group_inode_row`: a file made in `group_b_dir`. */
static void group_second_create(void)
{
    void *n = 0;

    group_b_rc = OPS->create(&mnt, group_b_dir, "made", 0644, &n);
    if (n) {
        OPS->put(&mnt, n);
    }
}

/**
 * An inode freed and an inode allocated in one group at once: process A
 * removes a file of blocks, and process B, run at A's first disk request
 * inside the removal, makes a file in another directory of the same group.
 * An inode bitmap has no image in the batch, so under the big lock its
 * buffer orders the two whatever the group's lock does; the row checks the
 * inode and block totals the two leave.
 */
static void group_inode_row(void)
{
    static uint8_t data[8 * 4096];
    void *ia = 0, *u = 0;
    int rc;

    if (!group_volume()) {
        return;
    }
    check(OPS->mkdir(&mnt, mnt.root, "ia", 0755) == 0 &&
              OPS->mkdir(&mnt, mnt.root, "ib", 0755) == 0,
          "two directories of one group");
    ia = walk("/ia", &rc);
    group_b_dir = walk("/ib", &rc);
    u = ia ? make_file(ia, "u", &rc) : 0;
    check(u && write_at(u, data, sizeof(data), 0) == 0, "a file of eight blocks");
    if (u) {
        OPS->put(&mnt, u);
    }
    if (!ia || !group_b_dir || !u) {
        group_finish(0, 0, "the inode row's volume");
        return;
    }
    check(OPS->sync(&mnt) == 0 && bcache_sync(&dev) == 0 && bcache_invalidate(&dev) == 0,
          "the cache empties");
    group_b_rc = -1;
    read_pause = overlap_turn;
    start_turns(group_second_create);
    rc = OPS->unlink(&mnt, ia, "u");
    finish_turns();
    read_pause = 0;
    check(rc == 0 && group_b_rc == 0, "a file is removed while another is made");
    OPS->put(&mnt, ia);
    OPS->put(&mnt, group_b_dir);
    group_finish(0, 0, "and each group's bitmaps and the free counts agree");
}

/** The group rows. */
static void group_rows(void)
{
    group_alloc_row();
    group_free_row();
    group_inode_row();
}

#endif
