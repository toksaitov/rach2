/*
 * ext2_alloc.c: ext2's allocators: the block and inode bitmaps, the group
 * descriptors and the free counts that change with them.
 */

#include "ext2_internal.h"

/** Write the primary descriptor of group `g` in the way `how` says; the
 * backup tables are not written. The caller holds the group's lock, so that
 * no other change of its 32 bytes comes between their copy and their
 * write. 0, or -errno. */
static int gd_write(struct ext2_mount *em, uint32_t g, int how)
{
    uint64_t at = (uint64_t)(em->first_data + 1) * em->block_size + (uint64_t)g * 32;

    ext2_assert_group_locked(em, g);
    return ext2_write_bytes(em, at, em->gd + g * 32, 32, how);
}

/** The block of the cache that holds group `g`'s descriptor. */
static uint32_t gd_cache_block(const struct ext2_mount *em, uint32_t g)
{
    return (uint32_t)(((uint64_t)(em->first_data + 1) * em->block_size + (uint64_t)g * 32) >>
                      BCACHE_BLOCK_SHIFT);
}

/**
 * `gd_write` with WRITE_THROUGH. While the batch holds blocks the descriptor
 * counts them taken and the cache's bitmap does not yet show them: a stop
 * then leaves a free count below the bitmap's, which the mount's check
 * recounts and e2fsck repairs.
 */
int ext2_gd_store(struct ext2_mount *em, uint32_t g)
{
    return gd_write(em, g, WRITE_THROUGH);
}

/**
 * The first clear bit at or after `from` and before `count` of the bitmap
 * in block `bitmap_block`, into `*out`, or `count` when every one is set.
 * The bitmap is read 64 bytes at a time and a byte with every bit set is
 * passed over whole, so an allocation in a group whose first part is full
 * does not read each of its bits through the cache alone. 0, or -EIO.
 */
int ext2_bit_first_clear(struct ext2_mount *em, uint32_t bitmap_block, uint32_t from,
                         uint32_t count, uint32_t *out)
{
    uint8_t chunk[64];
    uint32_t i = from;

    while (i < count) {
        uint32_t first = i / 8;
        uint32_t take = (count - 1) / 8 - first + 1;
        int rc;

        if (take > sizeof(chunk)) {
            take = sizeof(chunk);
        }
        rc = ext2_read_bytes(em, (uint64_t)bitmap_block * em->block_size + first, chunk, take);
        if (rc != 0) {
            return rc;
        }
        for (; i < count && i / 8 < first + take; i++) {
            uint8_t b = chunk[i / 8 - first];

            if (b == 0xFFu) {
                i |= 7u;
            } else if (!((b >> (i % 8)) & 1u)) {
                *out = i;
                return 0;
            }
        }
    }
    *out = count;
    return 0;
}

/** Set bit `index` of the bitmap in block `bitmap_block` to `value`, written
 * in the way `how` says; 0, or -errno. The caller holds the lock of the
 * bitmap's group, so that no other change of the byte comes between its
 * read and its write. */
static int bit_write(struct ext2_mount *em, uint32_t bitmap_block, uint32_t index, int value,
                     int how)
{
    uint64_t at = (uint64_t)bitmap_block * em->block_size + index / 8;
    uint8_t byte;
    int rc = ext2_read_bytes(em, at, &byte, 1);

    if (rc != 0) {
        return rc;
    }
    if (value) {
        byte = (uint8_t)(byte | (1u << (index % 8)));
    } else {
        byte = (uint8_t)(byte & ~(1u << (index % 8)));
    }
    return ext2_write_bytes(em, at, &byte, 1, how);
}

/** `bit_write` with WRITE_THROUGH. */
static int bit_put(struct ext2_mount *em, uint32_t bitmap_block, uint32_t index, int value)
{
    return bit_write(em, bitmap_block, index, value, WRITE_THROUGH);
}

/** Whether the caller may take the blocks the superblock reserves: a
 * privileged process (`proc_privileged`). */
int ext2_may_use_reserved(void)
{
    return current_proc && proc_privileged(current_proc);
}

/**
 * Whether a fault under errors=remount-ro has stopped the mount's
 * allocations (`em->stopped`): -EROFS then, 0 otherwise. An operation
 * already inside that needs a block or an inode gives up as at ENOSPC and
 * rolls back what it changed, which it may still write, so that the fault
 * leaves no half-made file. Read without a lock, as one word: an
 * allocation that races the fault ends as an operation under way does.
 */
int ext2_alloc_stopped(struct ext2_mount *em)
{
    return em->stopped ? -EROFS : 0;
}

/**
 * Refuse block `b`, which bit `bit` of the bitmap in block `bitmap` calls
 * free, when it may not hold data (`ext2_data_block_ok`): a superblock, a
 * descriptor table, a bitmap or an inode table, as Linux's
 * `ext2_new_blocks` refuses a block in the system zone
 * (fs/ext2/balloc.c:1377-1392). The bit is set, so that no search offers
 * the block again, the counts are left as they are, and the fault is
 * logged and reported (`ext2_mark_error`). 1 when the block was refused, 0
 * when it may hold data, or the bit's -errno. The caller holds the lock of
 * the bitmap's group and no buffer.
 */
int ext2_block_refused(struct ext2_mount *em, uint32_t bitmap, uint32_t bit, uint32_t b)
{
    int rc;

    if (ext2_data_block_ok(em, b)) {
        return 0;
    }
    rc = bit_put(em, bitmap, bit, 1);
    if (rc != 0) {
        return rc;
    }
    klog(KLOG_WARN, "ext2", "block %d is free in its bitmap but holds the volume's tables", (int)b);
    ext2_mark_error(em);
    return 1;
}

/**
 * Take the first free block of group `g` at or after bit `from` and before
 * bit `count` of its bitmap in block `bitmap`, into `*bit`: the bit set, the
 * group's count lowered and written, and the free count lowered with the
 * hint after it. A clear bit on a block that may not hold data is refused
 * (`ext2_block_refused`) and the search goes on past it, unless the fault
 * stopped the mount's allocations (`ext2_alloc_stopped`). -ENOSPC when the
 * group has none, -EROFS, or -errno. The caller holds the group's lock.
 */
static int group_take_block(struct ext2_mount *em, uint32_t g, uint32_t bitmap, uint32_t from,
                            uint32_t count, uint32_t *bit)
{
    uint8_t *gd = em->gd + g * 32;
    uint32_t i;
    int rc;

    if (ext2_le16(gd + 12) == 0) {
        return -ENOSPC;
    }
    for (;;) {
        rc = ext2_bit_first_clear(em, bitmap, from, count, &i);
        if (rc != 0) {
            return rc;
        }
        if (i >= count) {
            return -ENOSPC;
        }
        rc = ext2_block_refused(em, bitmap, i, em->first_data + g * em->bpg + i);
        if (rc == 0) {
            break;
        }
        if (rc < 0) {
            return rc;
        }
        rc = ext2_alloc_stopped(em);
        if (rc != 0) {
            return rc;
        }
        from = i + 1;
    }
    rc = bit_put(em, bitmap, i, 1);
    if (rc != 0) {
        return rc;
    }
    LE16_PUT(gd + 12, ext2_le16(gd + 12) - 1);
    rc = ext2_gd_store(em, g);
    if (rc != 0) {
        return rc;
    }
    ext2_count_blocks(em, -1, em->first_data + g * em->bpg + i + 1);
    *bit = i;
    return 0;
}

/**
 * Allocate one block into `*out`, first-fit from `hint`: every group from
 * the hint's on, wrapping, and last the part of the hint's group before the
 * hint. The bit is set and the group's count lowered before the block is
 * returned, so that no pointer ever names a free block, and the block after
 * it becomes the mount's hint. 0, -ENOSPC when none is free or only the
 * reserved blocks are and the caller is not privileged, -EROFS once a fault
 * stopped the mount's allocations (`ext2_alloc_stopped`), or -errno. Each
 * group is searched and changed under its lock, one at a time, as Linux's
 * `ext2_new_blocks` sets a bit under `sb_bgl_lock`; the free count read
 * first is a hint, as Linux's counter is. Under a plan (`ext2_volume_get`) a
 * bitmap the cache lacks is -EAGAIN, met before the first change, since
 * every read of the search and of the chosen bit's byte comes before its
 * write. The caller holds no group's lock.
 */
static int block_alloc(struct ext2_mount *em, uint32_t hint, uint32_t *out)
{
    uint32_t g0, g, tried;
    uint32_t start_in;
    uint64_t free;

    if (ext2_alloc_stopped(em)) {
        return -EROFS;
    }
    free = ext2_free_blocks(em);
    if (free == 0 || (!ext2_may_use_reserved() && free <= em->r_blocks)) {
        return -ENOSPC;
    }
    if (hint < em->first_data || hint >= em->blocks) {
        hint = em->first_data;
    }
    g0 = (hint - em->first_data) / em->bpg;
    start_in = (hint - em->first_data) % em->bpg;
    for (tried = 0; tried <= em->groups; tried++) {
        uint32_t i, count, base, bitmap, from;
        uint8_t *gd;
        int rc;

        g = (g0 + tried) % em->groups;
        gd = em->gd + g * 32;
        base = em->first_data + g * em->bpg;
        count = em->blocks - base < em->bpg ? em->blocks - base : em->bpg;
        bitmap = ext2_le32(gd + 0);
        from = tried == 0 ? start_in : 0;
        if (tried == em->groups) {
            count = start_in < count ? start_in : count;
        }
        if (ext2_le16(gd + 12) == 0) {
            continue;
        }
        ext2_group_lock(em, g);
        rc = group_take_block(em, g, bitmap, from, count, &i);
        ext2_group_unlock(em, g);
        if (rc == 0) {
            *out = base + i;
            return 0;
        }
        if (rc != -ENOSPC) {
            return rc;
        }
    }
    return -ENOSPC;
}

/** Free block `b`: its bit cleared and its group's count raised, as
 * `bit_put` and `ext2_gd_store` write them. 0, -EIO with a record for a block
 * that may not hold data, or -errno, under the group's lock, which the
 * caller may hold already. */
static int block_free(struct ext2_mount *em, uint32_t b)
{
    uint32_t g, i;
    uint8_t *gd;
    int rc;

    if (!ext2_data_block_ok(em, b)) {
        klog(KLOG_WARN, "ext2", "free of block %d, which is out of range", (int)b);
        return -EIO;
    }
    g = (b - em->first_data) / em->bpg;
    i = (b - em->first_data) % em->bpg;
    gd = em->gd + g * 32;
    ext2_group_lock(em, g);
    rc = bit_put(em, ext2_le32(gd + 0), i, 0);
    if (rc == 0) {
        LE16_PUT(gd + 12, ext2_le16(gd + 12) + 1);
        rc = ext2_gd_store(em, g);
    }
    if (rc == 0) {
        ext2_count_blocks(em, 1, 0);
    }
    ext2_group_unlock(em, g);
    return rc;
}

/**
 * Take a free inode of group `g` for `n`, as `ext2_inode_alloc` says: the
 * first whose bit is clear at or after `s_first_ino` and that the hash does
 * not hold. -ENOSPC when the group has none, -EAGAIN under a plan, or
 * -errno. The caller holds the group's lock.
 */
static int group_take_inode(struct ext2_mount *em, uint32_t g, int is_dir, struct ext2_node *n)
{
    uint8_t *gd = em->gd + g * 32;
    uint32_t base = g * em->ipg, from = 0, limit = em->ipg, i;
    int rc;

    if (ext2_le16(gd + 14) == 0) {
        return -ENOSPC;
    }
    if (base + 1 < em->first_ino) {
        from = em->first_ino - 1 - base;
    }
    if (em->inodes - base < limit) {
        limit = em->inodes - base;
    }
    for (;;) {
        rc = ext2_bit_first_clear(em, ext2_le32(gd + 4), from, limit, &i);
        if (rc != 0) {
            return rc;
        }
        if (i >= limit || !ext2_node_find(em, base + i + 1)) {
            break;
        }
        from = i + 1;
    }
    if (i >= limit) {
        return -ENOSPC;
    }
    rc = ext2_plan_has(em, gd_cache_block(em, g));
    if (rc != 0) {
        return rc;
    }
    (void)ext2_plan_end(em);
    ext2_node_publish(em, n, base + i + 1);
    ext2_node_share_group(em, n, g, 4);
    rc = bit_write(em, ext2_le32(gd + 4), i, 1, WRITE_UNFLUSHED);
    if (rc != 0) {
        return rc;
    }
    LE16_PUT(gd + 14, ext2_le16(gd + 14) - 1);
    if (is_dir) {
        LE16_PUT(gd + 16, ext2_le16(gd + 16) + 1);
    }
    rc = ext2_gd_store(em, g);
    if (rc != 0) {
        return rc;
    }
    ext2_count_inodes(em, -1);
    return 0;
}

/**
 * Allocate an inode for the new node `n` of `ext2_node_make`, in the group of
 * inode `parent_ino` when it has one free, else in the next group that has,
 * wrapping; an inode below `s_first_ino` is never given, and neither is one
 * in the hash, whose bit may be clear while it dies, so that nothing here
 * waits for a node. `n` is published under the number in one step with the
 * search, before the bit is set and the counts lowered, the group's
 * directory count raised for `is_dir`, and the caller writes the inode
 * initialised and names it, in that order. The bit and the counts have no
 * order between them, so the bitmap's write leaves the disk's cache to the
 * flush of the descriptor's, which puts both on the medium before the inode
 * is written. Under a plan (`ext2_volume_get`) a bitmap or descriptor block the
 * cache lacks is -EAGAIN, met before `n` is published and anything changes;
 * from there the plan is over, so a block evicted meanwhile is read under
 * the locks. Each group is searched and changed under its lock, one at a
 * time, as Linux's `ext2_new_inode` sets the bit under `sb_bgl_lock`. 0,
 * -ENOSPC, -EROFS once a fault stopped the mount's allocations
 * (`ext2_alloc_stopped`), or -errno, with `n->ino` nonzero when `n` was
 * published. The caller holds no group's lock.
 */
int ext2_inode_alloc(struct ext2_mount *em, uint32_t parent_ino, int is_dir, struct ext2_node *n)
{
    uint32_t g0 = (parent_ino - 1) / em->ipg;
    uint32_t tried;

    if (ext2_alloc_stopped(em)) {
        return -EROFS;
    }
    if (ext2_free_inodes(em) == 0) {
        return -ENOSPC;
    }
    for (tried = 0; tried < em->groups; tried++) {
        uint32_t g = (g0 + tried) % em->groups;
        uint8_t *gd = em->gd + g * 32;
        int rc;

        if (ext2_le16(gd + 14) == 0 || g * em->ipg >= em->inodes) {
            continue;
        }
        ext2_group_lock(em, g);
        rc = group_take_inode(em, g, is_dir, n);
        ext2_group_unlock(em, g);
        if (rc != -ENOSPC) {
            return rc;
        }
    }
    return -ENOSPC;
}
/** Clear the bit of inode `ino` and raise its group's free count, lowering
 * the directory count for `was_dir`, once the inode itself was written
 * freed, under the group's lock; 0, or -errno. */
int ext2_inode_bit_free(struct ext2_mount *em, uint32_t ino, int was_dir)
{
    uint32_t g = (ino - 1) / em->ipg;
    uint32_t i = (ino - 1) % em->ipg;
    uint8_t *gd = em->gd + g * 32;
    int rc;

    ext2_group_lock(em, g);
    rc = bit_put(em, ext2_le32(gd + 4), i, 0);
    if (rc == 0) {
        LE16_PUT(gd + 14, ext2_le16(gd + 14) + 1);
        if (was_dir && ext2_le16(gd + 16) > 0) {
            LE16_PUT(gd + 16, ext2_le16(gd + 16) - 1);
        }
        rc = ext2_gd_store(em, g);
    }
    if (rc == 0) {
        ext2_count_inodes(em, 1);
    }
    ext2_group_unlock(em, g);
    return rc;
}

/**
 * Drop inode `ino`'s reference to its extended-attribute block `b`, once the
 * inode was written freed: the block is freed when the reference count in
 * its header is 1, and the count lowered otherwise, as Linux's
 * `ext2_xattr_delete_inode` does, so that a block several inodes share
 * stays until the last of them goes. A block out of range, or whose header
 * lacks the magic number or a length of one block, is left as it is with
 * the error bit set, for e2fsck. The count is read and written under the
 * lock of the block's group, which the free takes again, so that two
 * inodes that share the block and go at once each count once, as Linux
 * holds the block's buffer lock for it. 0, or -errno.
 */
int ext2_attr_release(struct ext2_mount *em, uint32_t ino, uint32_t b)
{
    uint64_t at = (uint64_t)b * em->block_size;
    uint8_t raw[12];
    uint32_t refs, g;
    int rc;

    if (!ext2_data_block_ok(em, b)) {
        klog(KLOG_WARN, "ext2", "inode %d: attribute block %d is out of range", (int)ino, (int)b);
        ext2_mark_error(em);
        return 0;
    }
    g = (b - em->first_data) / em->bpg;
    ext2_group_lock(em, g);
    rc = ext2_read_bytes(em, at, raw, sizeof(raw));
    if (rc == 0 && (ext2_le32(raw) != EXT2_XATTR_MAGIC || ext2_le32(raw + 8) != 1)) {
        klog(KLOG_WARN, "ext2", "inode %d: attribute block %d has no valid header", (int)ino,
             (int)b);
        ext2_mark_error(em);
    } else if (rc == 0 && ext2_le32(raw + 4) > 1) {
        refs = ext2_le32(raw + 4);
        LE32_PUT(raw + 4, refs - 1);
        rc = ext2_write_bytes(em, at + 4, raw + 4, 4, WRITE_THROUGH);
    } else if (rc == 0) {
        rc = block_free(em, b);
    }
    ext2_group_unlock(em, g);
    return rc;
}

/**
 * Bring into the cache, with no group's lock held, the bitmap block and the
 * descriptor block of the group that holds block `hint`, which an
 * allocation from that hint reads first.
 */
void ext2_group_fetch(struct ext2_mount *em, uint32_t hint)
{
    uint32_t g;

    if (hint < em->first_data || hint >= em->blocks) {
        hint = em->first_data;
    }
    g = (hint - em->first_data) / em->bpg;
    ext2_volume_fetch(em, ext2_le32(em->gd + g * 32));
    ext2_volume_fetch(em, em->first_data + 1 + (g * 32) / BCACHE_BLOCK_BYTES);
}

/**
 * Allocate one block from `hint` into `*out` by `block_alloc`, whose groups'
 * locks wait on the disk only for a block evicted again since its fetch.
 * The hint's group is fetched first with no lock held, and a search that
 * meets another bitmap the cache lacks lets its lock go, fetches it and
 * starts again, PLAN_TRIES times before it reads under the lock. The caller
 * holds the lock of the node it allocates for and no group's lock. 0, or
 * the errors of `block_alloc`.
 */
int ext2_alloc_block(struct ext2_mount *em, uint32_t hint, uint32_t *out)
{
    uint32_t missed;
    int tries, rc;

    ext2_group_fetch(em, hint);
    for (tries = 0;; tries++) {
        ext2_plan_begin(em, tries);
        rc = block_alloc(em, hint, out);
        missed = ext2_plan_end(em);
        if (rc != -EAGAIN) {
            return rc;
        }
        ext2_volume_fetch(em, missed);
    }
}

/**
 * Free the `list->count` blocks of `list`, whose pointers in `n` are
 * cleared, a group at a time: the group's bitmap and descriptor block
 * fetched with no lock held, then under the group's lock each bit cleared and
 * the counts raised, the bitmap and descriptor dirtied `after`, so that they
 * reach the device after every pointer and inode dirtied `first`, and
 * listed among `n`'s shared blocks. The list is emptied. A
 * block that may not hold data is skipped with a record and makes the
 * answer -EIO. 0, or -errno.
 */
int ext2_punch_free(struct ext2_mount *em, struct ext2_node *n, struct punch_list *list)
{
    int rc = 0, i;

    while (list->count > 0) {
        uint32_t g, first = list->blocks[0];
        uint8_t *gd;
        int freed = 0, left = 0;

        if (!ext2_data_block_ok(em, first)) {
            klog(KLOG_WARN, "ext2", "free of block %d, which is out of range", (int)first);
            list->blocks[0] = list->blocks[--list->count];
            rc = -EIO;
            continue;
        }
        g = (first - em->first_data) / em->bpg;
        gd = em->gd + g * 32;
        ext2_group_fetch(em, first);
        ext2_group_lock(em, g);
        for (i = 0; i < list->count; i++) {
            uint32_t b = list->blocks[i];
            int err;

            if (!ext2_data_block_ok(em, b) || (b - em->first_data) / em->bpg != g) {
                list->blocks[left++] = b;
                continue;
            }
            err = bit_write(em, ext2_le32(gd + 0), (b - em->first_data) % em->bpg, 0, WRITE_AFTER);
            if (err != 0) {
                rc = rc != 0 ? rc : err;
                continue;
            }
            LE16_PUT(gd + 12, ext2_le16(gd + 12) + 1);
            freed++;
        }
        list->count = left;
        if (freed) {
            int err = gd_write(em, g, WRITE_AFTER);

            rc = rc != 0 ? rc : err;
            ext2_count_blocks(em, freed, 0);
            ext2_node_share_group(em, n, g, 0);
        }
        ext2_group_unlock(em, g);
    }
    return rc;
}

/** Fetch the block of the volume that holds group `g`'s descriptor, which
 * `ext2_gd_store` writes in part and so reads first. */
void ext2_gd_fetch(struct ext2_mount *em, uint32_t g)
{
    ext2_block_fetch(em, (uint64_t)(em->first_data + 1) * em->block_size + (uint64_t)g * 32);
}
