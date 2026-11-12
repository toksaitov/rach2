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

/* The inode process B of `group_inode_row` made, or 0. */
static uint32_t group_b_ino;

/** Process B of `group_inode_row`: a file made in `group_b_dir`. */
static void group_second_create(void)
{
    void *n = 0;

    group_b_rc = OPS->create(&mnt, group_b_dir, "made", 0644, &n);
    if (n) {
        group_b_ino = ((struct ext2_node *)n)->ino;
        OPS->put(&mnt, n);
    }
}

/** At process A's write of a run that covers `group_pause_block`, give
 * process B the turn. */
static void group_write_turn(void)
{
    if (current_proc == &proc_a && group_pause_block >= request_block &&
        group_pause_block < request_block + request_blocks) {
        write_pause = 0;
        group_paused = 1;
        pass_turn();
    }
}

/** The group of inode `ino`. */
static uint32_t group_of_inode(uint32_t ino)
{
    return (ino - 1) / group_em()->ipg;
}

/**
 * Mount a new volume of sixteen groups of GROUP_BLOCKS blocks with
 * `dirsync`, so that a bit is written through under its group's lock.
 * Whether it mounted writable.
 */
static int group_strict_volume(void)
{
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
    rc = mount_rw();
    check(rc == 0 && (mnt.flags & VFS_MS_RDONLY) == 0 && ext2_em_strict(group_em()),
          "the inode row's volume mounts with dirsync");
    return rc == 0;
}

/**
 * An inode freed and an inode allocated in one group at once, the second
 * inside the first's read-modify-write of the group's inode bitmap. Group 0
 * is filled, so that a file made in a directory there takes its inode from
 * group 1, where process A removes a file. On a mount with `dirsync` A
 * clears the file's bit and writes the bitmap through under group 1's lock,
 * before it raises the descriptor's count; process B, run at that write,
 * makes a file in a directory of group 0, passes its fetches, which A holds
 * no buffer of, finds group 0 full and must sleep on group 1's lock. Without
 * the lock B goes on into the group, sleeps on the bitmap's buffer, which A
 * holds, and the row fails by that wait.
 */
static void group_inode_row(void)
{
    struct ext2_mount *em;
    void *ia = 0, *u = 0;
    uint32_t uino = 0, k;
    char name[16];
    int rc = 0;

    if (!group_strict_volume()) {
        return;
    }
    em = group_em();
    check(OPS->mkdir(&mnt, mnt.root, "ia", 0755) == 0 &&
              OPS->mkdir(&mnt, mnt.root, "ib", 0755) == 0,
          "two directories of one group");
    ia = walk("/ia", &rc);
    group_b_dir = walk("/ib", &rc);
    if (!ia || !group_b_dir) {
        group_finish(0, 0, "the inode row's volume");
        return;
    }
    check(group_of_inode(((struct ext2_node *)ia)->ino) == 0 &&
              group_of_inode(((struct ext2_node *)group_b_dir)->ino) == 0,
          "both in group 0");
    for (k = 0; rc == 0 && ext2_le16(em->gd + 14) != 0 && k < em->ipg; k++) {
        void *f;

        snprintf(name, sizeof name, "f%u", (unsigned)k);
        f = make_file(group_b_dir, name, &rc);
        if (f) {
            OPS->put(&mnt, f);
        }
    }
    check(rc == 0 && ext2_le16(em->gd + 14) == 0, "files fill group 0's inodes");
    u = make_file(ia, "u", &rc);
    if (u) {
        uino = ((struct ext2_node *)u)->ino;
        OPS->put(&mnt, u);
    }
    check(u && group_of_inode(uino) == 1, "a file of group 0's directory takes group 1's inode");
    check(OPS->sync(&mnt) == 0 && bcache_sync(&dev) == 0, "the cache is clean");
    if (!u) {
        OPS->put(&mnt, ia);
        OPS->put(&mnt, group_b_dir);
        group_finish(0, 0, "the inode row's volume");
        return;
    }
    group_pause_block =
        ((uint64_t)ext2_le32(em->gd + 32 + 4) * em->block_size) >> BCACHE_BLOCK_SHIFT;
    group_paused = 0;
    group_b_rc = -1;
    group_b_ino = 0;
    write_pause = group_write_turn;
    start_turns(group_second_create);
    rc = OPS->unlink(&mnt, ia, "u");
    finish_turns();
    write_pause = 0;
    check(rc == 0 && group_b_rc == 0 && group_paused,
          "a file is removed while another is made, inside the write of the freed bit");
    check(second_wait && streq(second_wait, "ext2 group"), "where it sleeps on the group's lock");
    check(group_of_inode(group_b_ino) == 1, "and takes an inode of group 1");
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
