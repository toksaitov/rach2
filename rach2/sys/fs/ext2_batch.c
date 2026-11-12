/*
 * ext2_batch.c: ext2's file-write batch: private images of the blocks writes
 * change, planned and committed without I/O and drained to the cache stage by
 * stage.
 */

#include "ext2_internal.h"

/** Give back a write batch and every buffer page it holds; NULL is nothing. */
void ext2_writeback_free(struct ext2_writeback *wb)
{
    int i;

    if (!wb) {
        return;
    }
    for (i = 0; i < EXT2_WB_DATA + EXT2_WB_META; i++) {
        if (wb->slot[i]) {
            kfree_pages(wb->slot[i], 1);
        }
    }
    kfree(wb);
}

/** The batch's image of block `block`, metadata or data, or NULL; the cache
 * is neither read nor changed. The caller holds `em->batch` or `wb->lock`,
 * which keep the lists. */
static struct ext2_image *writeback_find(struct ext2_writeback *wb, uint32_t block)
{
    int i;

    for (i = 0; i < wb->metas; i++) {
        if (wb->meta[i].block == block) {
            return &wb->meta[i];
        }
    }
    for (i = 0; i < wb->datas; i++) {
        if (wb->data[i].block == block) {
            return &wb->data[i];
        }
    }
    return 0;
}

/** Whether the batch has an image of block `block`: 0 for none, 1 for an
 * image the batch has not changed, 2 for one it has, looked up under
 * `wb->lock`. A caller without `em->batch` learns what was so at the
 * look. */
int ext2_writeback_has(struct ext2_mount *em, uint32_t block)
{
    struct ext2_writeback *wb = em->wb;
    struct ext2_image *im;
    uint32_t flags;
    int has = 0;

    if (!wb) {
        return 0;
    }
    flags = spin_lock_irqsave(&wb->lock);
    im = writeback_find(wb, block);
    if (im) {
        has = im->dirty ? 2 : 1;
    }
    spin_unlock_irqrestore(&wb->lock, flags);
    return has;
}

/**
 * Copy `len` bytes at byte `at` of the batch's image of block `block` into
 * `buf`, or from `buf` into the image when `into` is set, in one step under
 * `wb->lock`, so that a drain that empties the lists or a plan that gives
 * back its images does not reuse the page under the copy. Returns 1, or 0
 * when the batch has no image of the block. The bytes copied are kept by
 * the caller's own lock: a node's, a group's or `em->super`.
 */
int ext2_writeback_copy(struct ext2_mount *em, uint32_t block, uint32_t at, void *buf, uint32_t len,
                        int into)
{
    struct ext2_writeback *wb = em->wb;
    struct ext2_image *im;
    uint32_t flags;

    if (!wb) {
        return 0;
    }
    flags = spin_lock_irqsave(&wb->lock);
    im = writeback_find(wb, block);
    if (im && into) {
        k_memcpy(im->data + at, buf, len);
    } else if (im) {
        k_memcpy(buf, im->data + at, len);
    }
    spin_unlock_irqrestore(&wb->lock, flags);
    return im != 0;
}

/** Whether the batch holds inodes, which every commit adds and only a
 * drain takes out, read as one word: a caller without `em->batch` learns
 * what was so at the look. */
int ext2_batch_pending(const struct ext2_mount *em)
{
    const volatile struct ext2_writeback *wb = em->wb;

    return wb && wb->count != 0;
}

/** Whether the file-write batch holds node `n`, whose pending pieces may
 * name blocks a free of `n` would give up, read without the batch's locks:
 * the caller holds `n`'s lock, under which alone `n` joins the batch, so a
 * 1 holds until a drain, and a drain under way meanwhile makes a 1 only a
 * drain too many. */
int ext2_writeback_holds(const struct ext2_mount *em, const struct ext2_node *n)
{
    int i;

    for (i = 0; em->wb && i < em->wb->count; i++) {
        if (em->wb->nodes[i] == n) {
            return 1;
        }
    }
    return 0;
}

/**
 * Whether a drain still to come stores inode `n`, on a mount that writes
 * back. A drain under way has encoded its nodes and may be asleep over a
 * buffer, so it is waited for, under `em->batch`, and the answer holds until
 * the caller sleeps.
 */
int ext2_writeback_keeps(struct ext2_mount *em, const struct ext2_node *n)
{
    int kept;

    if (ext2_em_strict(em) || !ext2_writeback_holds(em, n)) {
        return 0;
    }
    ext2_batch_lock(em);
    kept = ext2_writeback_holds(em, n);
    ext2_batch_unlock(em);
    return kept;
}

/** List image `im`, whose page is filled, as block `block`'s at stage
 * `stage`, raising the count `*count` of its list, under `wb->lock`. The
 * caller holds `em->batch`. */
static void writeback_list(struct ext2_writeback *wb, struct ext2_image *im, uint32_t block,
                           int stage, int *count)
{
    uint32_t flags = spin_lock_irqsave(&wb->lock);

    im->block = block;
    im->stage = stage;
    (*count)++;
    spin_unlock_irqrestore(&wb->lock, flags);
}

/**
 * The batch's image of metadata block `block` at stage `stage`: the one it
 * has, or a new one in the next free page, zeroed with `blank` and a copy of
 * the cache's block otherwise, not yet marked changed, listed before the
 * cache's buffer goes, so that a write through the cache to the block from
 * then on finds the image (`ext2_write_owned`). NULL with `*rc` set:
 * -EIO for an image the batch has at another stage or a failed read,
 * -ENOMEM for a full table, and -EAGAIN for a block the cache lacks under a
 * plan (`ext2_volume_get`). A piece that fails gives back the new images it
 * took (`writeback_try`); an image the batch had is not changed here.
 */
static struct ext2_image *writeback_meta(struct ext2_mount *em, uint32_t block, int stage,
                                         int blank, int *rc)
{
    struct ext2_writeback *wb = em->wb;
    struct ext2_image *im;
    struct bbuf *b;
    int i;

    for (i = 0; i < wb->metas; i++) {
        im = &wb->meta[i];
        if (im->block == block) {
            if (im->stage != stage) {
                *rc = -EIO;
                return 0;
            }
            return im;
        }
    }
    if (wb->metas == EXT2_WB_META) {
        *rc = -ENOMEM;
        return 0;
    }
    im = &wb->meta[wb->metas];
    im->data = wb->slot[EXT2_WB_DATA + wb->metas];
    im->dirty = 0;
    im->owner = 0;
    if (blank) {
        k_memset(im->data, 0, BCACHE_BLOCK_BYTES);
        writeback_list(wb, im, block, stage, &wb->metas);
        return im;
    }
    b = ext2_volume_get(em, block, rc);
    if (!b) {
        return 0;
    }
    k_memcpy(im->data, b->data, BCACHE_BLOCK_BYTES);
    writeback_list(wb, im, block, stage, &wb->metas);
    bcache_release(b);
    return im;
}

/* A block chosen for a piece and not yet taken: its number, group and bit,
 * and the batch's images of its bitmap and descriptor blocks. */
struct ext2_reservation {
    uint32_t block, group, bit;
    struct ext2_image *bitmap, *descriptor;
};

/*
 * What a piece of a write needs before it is committed: the blocks it
 * reserves, `allocs` of them; the images of the `paths` pointer blocks on its
 * path, with the byte offset of the slot in each and the block each slot is
 * to name; the images of its inode's table block and, when the file reaches
 * 2 GiB, the superblock; the inode's pointer at the top of the path, its
 * index and value; the data block, `fresh` when new, and its image; and the
 * inode's place in the batch's table, `count` for a new one; and the
 * `nlocked` groups whose locks it took for its reservations.
 */
struct ext2_write_plan {
    struct ext2_reservation reservations[4];
    struct ext2_image *parents[3], *table, *feature, *data;
    uint32_t slots[3], children[3];
    uint32_t root, root_slot, block;
    uint32_t locked[4];
    int paths, fresh, allocs, node_index, nlocked;
};

/**
 * Take group `g`'s lock for the plan unless the plan holds it: 1 when it
 * took it, as the last of `plan->locked`, and 0 when the plan held it. The
 * second and later are told to the checker at subclasses 1 to 3: the plan
 * holds `em->batch`, so no other holds several groups' locks to wait on.
 */
static int plan_group_lock(struct ext2_mount *em, struct ext2_write_plan *plan, uint32_t g)
{
    if (ext2_group_held(em, g)) {
        return 0;
    }
    if (plan->nlocked != 0) {
        sleep_lock_subclass(plan->nlocked);
    }
    ext2_group_lock(em, g);
    plan->locked[plan->nlocked++] = g;
    return 1;
}

/** Give back every group's lock the plan took, the last first. */
static void plan_groups_unlock(struct ext2_mount *em, struct ext2_write_plan *plan)
{
    while (plan->nlocked > 0) {
        ext2_group_unlock(em, plan->locked[--plan->nlocked]);
    }
}

/**
 * Reserve for the plan the first free block of group `g` from bit `from`
 * to before bit `end` that the plan has not chosen, as `writeback_reserve`
 * says. 0, -ENOSPC when the group has none, or -errno. The caller holds the
 * group's lock.
 */
static int reserve_in(struct ext2_mount *em, struct ext2_write_plan *plan, uint32_t g,
                      uint32_t from, uint32_t end)
{
    struct ext2_reservation *list = plan->reservations, *r = &list[plan->allocs];
    uint8_t *gd = em->gd + g * 32;
    uint32_t bit, block;
    int i, rc;

    if (ext2_le16(gd + 12) == 0) {
        return -ENOSPC;
    }
    for (bit = from; bit < end; bit++) {
        rc = ext2_bit_first_clear(em, ext2_le32(gd), bit, end, &bit);
        if (rc != 0) {
            return rc;
        }
        if (bit >= end) {
            break;
        }
        block = em->first_data + g * em->bpg + bit;
        for (i = 0; i < plan->allocs && list[i].block != block; i++) {
        }
        if (i != plan->allocs) {
            continue;
        }
        if (!ext2_data_block_ok(em, block)) {
            return -EIO;
        }
        r->bitmap = writeback_meta(em, ext2_le32(gd), WB_ALLOC, 0, &rc);
        if (!r->bitmap) {
            return rc;
        }
        r->descriptor = writeback_meta(em, em->first_data + 1 + (g * 32) / 4096, WB_ALLOC, 0, &rc);
        if (!r->descriptor) {
            return rc;
        }
        r->block = block;
        r->group = g;
        r->bit = bit;
        return 0;
    }
    return -ENOSPC;
}

/**
 * Choose a free block for the piece being planned, the next of the
 * `plan->allocs` reservations, first-fit from the hint as `block_alloc`
 * searches, passing over the blocks the piece has chosen already, and take
 * the images of its bitmap and descriptor blocks. Each group is searched
 * under its lock, which the plan keeps for a group it reserves in until its
 * commit sets the bit (`writeback_try`), and gives back for one it does not.
 * The free count already excludes the blocks of committed pieces but not
 * this piece's reservations, so those are subtracted before another block
 * is admitted, against the reserved blocks for a caller that is not
 * privileged. 0, -ENOSPC, -EIO for a clear bit on a structural block, or
 * the errors of `writeback_meta`.
 */
static int writeback_reserve(struct ext2_mount *em, struct ext2_write_plan *plan)
{
    uint32_t start = ext2_alloc_hint(em);
    uint64_t free = ext2_free_blocks(em);
    uint32_t g0, tried, g;
    int count = plan->allocs, rc;

    if (free <= (uint32_t)count ||
        (!ext2_may_use_reserved() && free - (uint32_t)count <= em->r_blocks)) {
        return -ENOSPC;
    }
    if (start >= em->blocks) {
        start = em->first_data;
    }
    g0 = (start - em->first_data) / em->bpg;
    for (tried = 0; tried <= em->groups; tried++) {
        uint32_t from, end;
        int took;

        g = (g0 + tried) % em->groups;
        if (ext2_le16(em->gd + g * 32 + 12) == 0) {
            continue;
        }
        from = tried == 0 ? (start - em->first_data) % em->bpg : 0;
        end = em->blocks - (em->first_data + g * em->bpg);
        if (end > em->bpg) {
            end = em->bpg;
        }
        if (tried == em->groups && end > (start - em->first_data) % em->bpg) {
            end = (start - em->first_data) % em->bpg;
        }
        took = plan_group_lock(em, plan, g);
        rc = reserve_in(em, plan, g, from, end);
        if (rc != -ENOSPC) {
            return rc;
        }
        if (took) {
            ext2_group_unlock(em, g);
            plan->nlocked--;
        }
    }
    return -ENOSPC;
}

/**
 * Plan the pointer path and the data block of data index `index` of `n`
 * into `plan`: the image of each pointer block on the path, a missing one
 * reserved and blank, and a missing data block reserved, nothing yet taken.
 * The arithmetic is for 4 KiB blocks of 1024 pointers, which a writable
 * mount has. 0, -EIO for a block out of range, or the errors of
 * `writeback_reserve` and `writeback_meta`.
 */
static int writeback_plan_map(struct ext2_mount *em, struct ext2_node *n, uint32_t index,
                              struct ext2_write_plan *plan)
{
    uint32_t rest = index, div, block;
    int depth = (int)ext2_pointer_levels(em, index), level, rc = 0;

    plan->root_slot = depth ? EXT2_NDIR + depth - 1 : index;
    plan->root = n->block[plan->root_slot];
    if (depth) {
        rest -= EXT2_NDIR;
        if (depth >= 2) {
            rest -= 1024;
        }
        if (depth == 3) {
            rest -= 1024u * 1024u;
        }
    }
    block = plan->root;
    for (level = depth; level > 0; level--) {
        struct ext2_image *parent;
        int blank = block == 0;

        if (blank) {
            rc = writeback_reserve(em, plan);
            if (rc != 0) {
                return rc;
            }
            block = plan->reservations[plan->allocs++].block;
            if (plan->paths == 0) {
                plan->root = block;
            } else {
                plan->children[plan->paths - 1] = block;
            }
        } else if (!ext2_data_block_ok(em, block)) {
            return -EIO;
        }
        parent = writeback_meta(em, block, WB_LEAF + level - 1, blank, &rc);
        if (!parent) {
            return rc;
        }
        parent->owner = n->ino;
        div = level == 3 ? 1024u * 1024u : (level == 2 ? 1024u : 1u);
        plan->slots[plan->paths] = (rest / div) * 4;
        plan->parents[plan->paths] = parent;
        block = ext2_le32(parent->data + plan->slots[plan->paths]);
        plan->children[plan->paths++] = block;
        rest %= div;
    }
    if (block == 0) {
        rc = writeback_reserve(em, plan);
        if (rc != 0) {
            return rc;
        }
        block = plan->reservations[plan->allocs++].block;
        plan->fresh = 1;
        if (plan->paths) {
            plan->children[plan->paths - 1] = block;
        } else {
            plan->root = block;
        }
    } else if (!ext2_data_block_ok(em, block)) {
        return -EIO;
    }
    plan->block = block;
    return 0;
}

/**
 * Plan a piece of `len` bytes at `offset` of `n`: its place in the batch's
 * inode table, -ENOMEM when that is full; the image of its inode's table
 * block; the superblock's image when the file reaches 2 GiB without
 * `large_file`; and the block mapping, by `writeback_plan_map`.
 */
static int writeback_plan(struct ext2_mount *em, struct ext2_node *n, uint64_t offset, uint32_t len,
                          struct ext2_write_plan *plan)
{
    struct ext2_writeback *wb = em->wb;
    int rc = 0;

    for (plan->node_index = 0; plan->node_index < wb->count; plan->node_index++) {
        if (wb->nodes[plan->node_index] == n) {
            break;
        }
    }
    if (plan->node_index == wb->count && wb->count == EXT2_WB_NODES) {
        return -ENOMEM;
    }
    plan->table =
        writeback_meta(em, (uint32_t)(ext2_inode_offset(em, n->ino) >> 12), WB_INODE, 0, &rc);
    if (!plan->table) {
        return rc;
    }
    if (offset + len >= 0x80000000u && !(em->ro_compat & EXT2_RO_LARGE_FILE)) {
        plan->feature = writeback_meta(em, 0, WB_FEATURE, 0, &rc);
        if (!plan->feature) {
            return rc;
        }
    }
    return writeback_plan_map(em, n, (uint32_t)(offset >> 12), plan);
}

/** The batch's image of the plan's data block, a block of inode `owner`:
 * the one it has, or a new one, zeroed for a new block or one the piece of
 * `len` bytes writes whole, and a copy of the cache's block otherwise. 0,
 * -ENOMEM when the table is full, -EIO, or -EAGAIN under a plan
 * (`ext2_volume_get`). */
static int writeback_prepare_data(struct ext2_mount *em, uint32_t owner, uint32_t len,
                                  struct ext2_write_plan *plan)
{
    struct ext2_writeback *wb = em->wb;
    struct ext2_image *data;
    int i, rc;

    for (i = 0; i < wb->datas && wb->data[i].block != plan->block; i++) {
    }
    if (i == wb->datas) {
        struct bbuf *b;

        if (i == EXT2_WB_DATA) {
            return -ENOMEM;
        }
        data = &wb->data[i];
        data->data = wb->slot[i];
        data->dirty = 0;
        if (!plan->fresh && len != BCACHE_BLOCK_BYTES) {
            b = ext2_volume_get(em, plan->block, &rc);
            if (!b) {
                return rc;
            }
            k_memcpy(wb->slot[i], b->data, BCACHE_BLOCK_BYTES);
            writeback_list(wb, data, plan->block, WB_DATA, &wb->datas);
            bcache_release(b);
        } else {
            k_memset(wb->slot[i], 0, BCACHE_BLOCK_BYTES);
            writeback_list(wb, data, plan->block, WB_DATA, &wb->datas);
        }
    }
    plan->data = &wb->data[i];
    plan->data->owner = owner;
    return 0;
}

/**
 * Commit a planned piece, with no I/O and nothing that can fail: the
 * reserved blocks taken in their bitmap and descriptor images and in the
 * free count, the pointer path and the inode's pointers and `i_blocks` set,
 * the superblock's `large_file` flag set, the inode held by the batch, the
 * bytes copied into the data image, and the size and times raised. Reads
 * see the piece at once, since `ext2_read_bytes` looks in the images first. The
 * bitmaps, descriptors and superblock it changes are listed among `n`'s
 * shared blocks (`ext2_node_share`). The caller holds `em->batch` and the
 * locks of the reservations' groups.
 */
static void writeback_commit(struct ext2_mount *em, struct ext2_node *n, uint64_t offset,
                             const uint8_t *src, uint32_t len, struct ext2_write_plan *plan)
{
    struct ext2_writeback *wb = em->wb;
    int i;

    for (i = 0; i < plan->allocs; i++) {
        struct ext2_reservation *r = &plan->reservations[i];
        uint8_t *gd = em->gd + r->group * 32;

        r->bitmap->data[r->bit / 8] |= (uint8_t)(1u << (r->bit % 8));
        r->bitmap->dirty = 1;
        LE16_PUT(gd + 12, ext2_le16(gd + 12) - 1);
        k_memcpy(r->descriptor->data + (r->group * 32) % 4096, gd, 32);
        r->descriptor->dirty = 1;
        ext2_node_share_group(em, n, r->group, 0);
        ext2_count_blocks(em, -1, r->block + 1);
    }
    for (i = 0; i < plan->paths; i++) {
        if (ext2_le32(plan->parents[i]->data + plan->slots[i]) != plan->children[i]) {
            LE32_PUT(plan->parents[i]->data + plan->slots[i], plan->children[i]);
            plan->parents[i]->dirty = 1;
        }
    }
    if (plan->feature) {
        ext2_node_share(em, n, 0);
        LE32_PUT(plan->feature->data + 1024 + 100,
                 ext2_feature_add(em, &em->ro_compat, EXT2_RO_LARGE_FILE));
        plan->feature->dirty = 1;
    }
    if (plan->node_index == wb->count) {
        uint32_t flags = spin_lock_irqsave(&wb->lock);

        wb->nodes[plan->node_index] = n;
        wb->count++;
        spin_unlock_irqrestore(&wb->lock, flags);
        ext2_node_ref(n, 1);
    }
    n->block[plan->root_slot] = plan->root;
    n->blocks += (uint32_t)plan->allocs * 8;
    k_memcpy(plan->data->data + ((uint32_t)offset & 4095u), src, len);
    plan->data->dirty = 1;
    if (offset + len > n->size) {
        ext2_node_size_set(n, offset + len);
    }
    ext2_stamp(n, T_MTIME | T_CTIME);
    plan->table->dirty = 1;
    wb->bytes += len;
    if (current_proc) {
        current_proc->fs_written += len;
    }
}

/**
 * Bring into the cache, with no lock but `n`'s held, what the plan of the
 * piece of `len` bytes at `offset` of `n` reads: the inode's table block, the
 * pointer path, the data block of a partial write, the hint's group's bitmap
 * and descriptor block, and the superblock when the file reaches 2 GiB
 * without `large_file`. A hint only: what is evicted again before the plan
 * costs it a restart.
 */
static void writeback_fetch(struct ext2_mount *em, struct ext2_node *n, uint64_t offset,
                            uint32_t len)
{
    uint32_t block = 0;

    ext2_volume_fetch(em, ext2_inode_offset(em, n->ino) >> BCACHE_BLOCK_SHIFT);
    if (ext2_block_at(em, n, (uint32_t)(offset >> 12), &block) == 0 && block != 0 &&
        len != BCACHE_BLOCK_BYTES) {
        ext2_volume_fetch(em, block);
    }
    ext2_group_fetch(em, ext2_alloc_hint(em));
    if (offset + len >= 0x80000000u && !(em->ro_compat & EXT2_RO_LARGE_FILE)) {
        ext2_volume_fetch(em, 0);
    }
}

/**
 * Plan, prepare and commit one piece, as `writeback_piece` says, under
 * `em->batch`, the plan taking the locks of the groups it reserves in and
 * giving them back after the commit or the failure. A failure gives back the
 * images the piece took before it returns, in the same step as the plan:
 * once the lock goes, other writers change the tables and the counts. A
 * piece whose bytes the batch has no room left for is -ENOMEM, looked at
 * under the lock, since another writer may have filled it since the
 * caller's look (`ext2_writeback_fit`). The
 * check that the user's pages are present and the commit that copies them
 * run under the big lock, taken for them when the caller lacks it, since
 * every change that takes a page away holds it (`vm_resident`). 0, or
 * -errno, -EAGAIN for a block the cache lacks under a plan.
 */
static int writeback_try(struct ext2_mount *em, struct ext2_node *n, uint64_t offset,
                         const uint8_t *src, uint32_t len)
{
    struct ext2_writeback *wb = em->wb;
    struct ext2_write_plan plan;
    int old_metas = wb->metas, old_datas = wb->datas;
    uint32_t flags;
    int rc, big;

    ext2_assert_batch_locked(em);
    k_memset(&plan, 0, sizeof(plan));
    if (len > EXT2_WB_BYTES - wb->bytes) {
        return -ENOMEM;
    }
    rc = writeback_plan(em, n, offset, len, &plan);
    if (rc != 0) {
        goto fail;
    }
    rc = writeback_prepare_data(em, n->ino, len, &plan);
    if (rc != 0) {
        goto fail;
    }
    if (em->dev->flags & (BLOCK_GONE | BLOCK_WRITE_FAILED) || em->mnt->flags & VFS_MS_RDONLY) {
        rc = -EIO;
        goto fail;
    }
    big = ext2_big_enter();
    if (!vm_resident((uintptr_t)src, len, 0)) {
        ext2_big_exit(big);
        rc = -EFAULT;
        goto fail;
    }
    writeback_commit(em, n, offset, src, len, &plan);
    ext2_big_exit(big);
    plan_groups_unlock(em, &plan);
    return 0;
fail:
    flags = spin_lock_irqsave(&wb->lock);
    wb->metas = old_metas;
    wb->datas = old_datas;
    spin_unlock_irqrestore(&wb->lock, flags);
    plan_groups_unlock(em, &plan);
    return rc;
}

/**
 * Accept one piece of a write, `len` bytes from `src` at `offset` of `n`
 * within one block: planned, its data image prepared, and committed. No
 * image readers see changes until every read and reservation has
 * succeeded, and a piece that fails gives back the images it took. A
 * removed device, one that failed a write, or a mount made read-only is
 * -EIO. `src` may be a user buffer: the check that its pages are present
 * runs in the step of the commit, which copies it, and a page that is not
 * is -EFAULT with nothing committed.
 *
 * What the plan reads is fetched first with `em->batch` let go
 * (`writeback_fetch`). Under its locks the plan reads only what the cache
 * has and nobody holds; a block it lacks or another holds gives the piece
 * back, lets the locks go, fetches the block or waits for it and starts
 * again, and after PLAN_TRIES such starts the plan reads under the locks,
 * one request. The caller holds `n`'s lock and not `em->batch`. 0, or
 * -errno.
 */
static int writeback_piece(struct ext2_mount *em, struct ext2_node *n, uint64_t offset,
                           const uint8_t *src, uint32_t len)
{
    uint32_t missed;
    int tries, rc;

    writeback_fetch(em, n, offset, len);
    for (tries = 0;; tries++) {
        ext2_batch_lock(em);
        ext2_plan_begin(em, tries);
        rc = writeback_try(em, n, offset, src, len);
        missed = ext2_plan_end(em);
        ext2_batch_unlock(em);
        if (rc != -EAGAIN) {
            return rc;
        }
        ext2_volume_fetch(em, missed);
    }
}

/**
 * Record a failure `rc` of the batch: the first one's errno kept for the
 * mount's life, the error bit set in memory, the mount made read-only, and
 * the mount's `write_error` set when it has none, which every later
 * `fsync` reports. The mount's flags and `write_error` are the VFS's,
 * which the big lock keeps, so it is taken for them. Returns `rc`.
 */
int ext2_writeback_error(struct ext2_mount *em, int rc)
{
    struct ext2_writeback *wb = em->wb;
    int big;

    if (wb) {
        uint32_t flags = spin_lock_irqsave(&wb->lock);

        if (!wb->error) {
            wb->error = rc;
        }
        spin_unlock_irqrestore(&wb->lock, flags);
    }
    (void)ext2_state_change(em, EXT2_ERROR_FS, 0);
    big = ext2_big_enter();
    em->mnt->flags |= VFS_MS_RDONLY;
    if (!em->mnt->write_error) {
        em->mnt->write_error = rc;
    }
    ext2_big_exit(big);
    return rc;
}

/** The batch's changed images at or past the stage a drain is at: the
 * buffers a drain takes from the cache. */
static uint32_t writeback_dirty(const struct ext2_writeback *wb)
{
    uint32_t n = 0;
    int i;

    for (i = 0; i < wb->datas; i++) {
        n += wb->data[i].dirty && wb->data[i].stage >= wb->stage;
    }
    for (i = 0; i < wb->metas; i++) {
        n += wb->meta[i].dirty && wb->meta[i].stage >= wb->stage;
    }
    return n;
}

/** Mark dirty the held buffer `b` that a drain copied image `im` into: a
 * pointer block at the level its stage is for, a block of its file, or a
 * shared block, which clears a tag a freed block kept. */
static void drain_mark(struct bbuf *b, const struct ext2_image *im)
{
    if (im->stage >= WB_LEAF && im->stage < WB_FEATURE) {
        bcache_dirty_pointer(b, im->owner, im->stage - WB_LEAF + 1);
    } else if (im->owner || b->owner) {
        bcache_dirty_owned(b, im->owner);
    } else {
        bcache_dirty(b);
    }
}

/** Whether another process holds in the cache a block the drain will copy
 * into, a changed image at or past the drain's stage, as a sync or an
 * fsync does while it writes the block; the first such block goes in
 * `*blk`. The caller holds `em->batch`. */
static int drain_busy(struct ext2_mount *em, uint32_t *blk)
{
    struct ext2_writeback *wb = em->wb;
    const struct ext2_image *im;
    int i;

    for (i = 0; i < wb->datas + wb->metas; i++) {
        im = i < wb->datas ? &wb->data[i] : &wb->meta[i - wb->datas];
        if (im->dirty && im->stage >= wb->stage && bcache_busy(em->dev, im->block)) {
            *blk = im->block;
            return 1;
        }
    }
    return 0;
}

/**
 * Take `em->batch` for a drain once no other process holds a block it will
 * copy into, waiting for each such block with the lock let go, PLAN_TRIES
 * times before the drain waits under it; a strict mount takes it at once.
 * Returns 1 with the lock held, or 0 without it when the batch emptied
 * meanwhile.
 */
static int drain_lock(struct ext2_mount *em)
{
    uint32_t busy;
    int tries;

    for (tries = 0;; tries++) {
        ext2_batch_lock(em);
        if (!em->wb->count) {
            ext2_batch_unlock(em);
            return 0;
        }
        if (ext2_em_strict(em) || tries >= PLAN_TRIES || !drain_busy(em, &busy)) {
            return 1;
        }
        ext2_batch_unlock(em);
        bcache_wait_unheld(em->dev, busy);
    }
}

/**
 * Write the batch out, stage by stage, under `em->batch`: the inodes encoded
 * into their table images, then each stage's changed images copied into the
 * cache, from the allocation bitmaps and descriptors through the data, the
 * pointer blocks and the superblock to the inode tables, a file's data and
 * pointer blocks tagged as its own and each pointer block marked at its
 * level after the blocks below it, so that the cache writes it after them
 * (`bcache_dirty_pointer`). No buffer is held across a sync, which may
 * reach a backing file.
 *
 * On a mount that writes back the copies are all the work: the cache first
 * writes dirty victims with no lock taken until it can give a buffer for
 * each copy without a write (`bcache_reserve`), and a block another process
 * holds, which a sync or an fsync holds for the length of its write, is
 * waited for with the lock let go, PLAN_TRIES times before the drain waits
 * under it; so the lock waits on the disk only for what others took
 * meanwhile. Every EXT2_SERVICE_BLOCKS copies the drain offers the big lock,
 * with no buffer held: the point is the one where the next `bcache_get_blank`
 * may sleep, and `em->batch` keeps the batch while others run. A strict mount
 * holds the
 * lock across its ordered writes, as its option asks: each stage is synced
 * and flushed before the next enters the cache, so that a sync or an
 * eviction can write only a stage whose dependencies are on the medium.
 *
 * At the end the batch is emptied, and with the lock let go its inodes'
 * holds are put and the cache balanced (`ext2_em_balance`). A failure keeps the
 * images and the stage, so that the next drain resumes there, and is
 * recorded by `ext2_writeback_error`; a removed device fails at once. The
 * reserve and the copies into the cache run under the big lock, which the
 * cache's write side keeps and the drain takes when its caller lacks it
 * (`ext2_big_enter`), let go and taken back at each sleep. The caller holds
 * no `em->batch`. 0, or -errno.
 */
int ext2_writeback_drain(struct ext2_mount *em)
{
    struct ext2_writeback *wb = em->wb;
    struct ext2_node *held[EXT2_WB_NODES];
    uint32_t copied = 0, flags;
    int i, count, rc, big;

    if (em->dev->flags & BLOCK_GONE) {
        return ext2_writeback_error(em, -EIO);
    }
    if (!ext2_batch_pending(em)) {
        return 0;
    }
    big = ext2_big_enter();
    if (!ext2_em_strict(em)) {
        (void)bcache_reserve(em->dev, writeback_dirty(wb));
    }
    if (!drain_lock(em)) {
        ext2_big_exit(big);
        return 0;
    }
    for (i = 0; i < wb->count; i++) {
        struct ext2_node *n = wb->nodes[i];
        uint64_t at = ext2_inode_offset(em, n->ino);
        struct ext2_image *im = writeback_find(wb, (uint32_t)(at >> 12));

        ext2_inode_encode_noting(em, n, im->data + ((uint32_t)at & 4095u),
                                 em->inode_size > EXT2_GOOD_OLD ? EXT2_LARGE : EXT2_GOOD_OLD);
    }
    for (; wb->stage <= WB_INODE; wb->stage++) {
        struct ext2_image *images = wb->stage == WB_DATA ? wb->data : wb->meta;
        int wrote = 0;

        count = wb->stage == WB_DATA ? wb->datas : wb->metas;
        rc = ext2_em_sync(em);
        if (rc != 0) {
            goto fail;
        }
        for (i = 0; i < count; i++) {
            struct ext2_image *im = &images[i];
            struct bbuf *b;

            if (!im->dirty || im->stage != wb->stage) {
                continue;
            }
            b = bcache_get_blank(em->dev, im->block);
            if (!b) {
                rc = -EIO;
                goto fail;
            }
            k_memcpy(b->data, im->data, BCACHE_BLOCK_BYTES);
            drain_mark(b, im);
            bcache_release(b);
            wrote = 1;
            if (++copied % EXT2_SERVICE_BLOCKS == 0) {
                sched_service_point();
            }
        }
        if (wrote) {
            rc = ext2_em_sync(em);
            if (rc == 0 && ext2_em_strict(em)) {
                rc = block_flush(em->dev);
            }
            if (rc != 0) {
                goto fail;
            }
        }
    }
    count = wb->count;
    for (i = 0; i < count; i++) {
        held[i] = wb->nodes[i];
    }
    flags = spin_lock_irqsave(&wb->lock);
    wb->count = wb->datas = wb->metas = 0;
    spin_unlock_irqrestore(&wb->lock, flags);
    wb->bytes = 0;
    wb->stage = 0;
    ext2_batch_unlock(em);
    ext2_big_exit(big);
    for (i = 0; i < count; i++) {
        ext2_put(em->mnt, held[i]);
    }
    ext2_em_balance(em);
    return 0;
fail:
    rc = ext2_writeback_error(em, rc);
    ext2_batch_unlock(em);
    ext2_big_exit(big);
    return rc;
}

/**
 * Whether the write of `count` bytes at `offset` of `n` may join the batch:
 * 1 for an overwrite of blocks the file has inside its size, or an append
 * that starts at the size on a block boundary or in a last block the file
 * has; 0 for any other write, which drains the batch first and after each
 * block; or the errors of `ext2_block_at`.
 */
int ext2_writeback_contiguous(struct ext2_mount *em, struct ext2_node *n, uint64_t offset,
                              uint32_t count)
{
    uint32_t first, last, i, block;
    int rc;

    if (offset > n->size || (offset < n->size && count > n->size - offset)) {
        return 0;
    }
    if (offset == n->size && !(offset & 4095u)) {
        return 1;
    }
    first = (uint32_t)(offset >> 12);
    last = offset == n->size ? first : (uint32_t)((offset + count - 1) >> 12);
    for (i = first; i <= last; i++) {
        rc = ext2_block_at(em, n, i, &block);
        if (rc != 0) {
            return rc;
        }
        if (!block) {
            return 0;
        }
    }
    return 1;
}

/* One step of the walk below: a block pointer, its level (0 for a data
 * block), and the first and last data indexes, both included and counted
 * from the start of its subtree, that the step covers. */
struct writeback_step {
    uint32_t block;
    int level;
    uint32_t first;
    uint32_t last;
};

/**
 * Add to `*need` the blocks `n` lacks under `step`. An absent subtree needs
 * its data blocks and every pointer block covering the range, each counted
 * once by dividing both ends of the range at each level, also when the
 * range crosses a leaf or a higher boundary. A present pointer block is read,
 * the batch's accepted allocations included, and its children counted. 0,
 * or -EIO, with the error bit set and a record, for a block out of range.
 */
static int writeback_missing(struct ext2_mount *em, const struct ext2_node *n,
                             const struct writeback_step *step, uint64_t *need)
{
    uint32_t per = em->block_size / 4;
    uint32_t block = step->block;
    uint32_t first = step->first;
    uint32_t last = step->last;
    int level = step->level;
    uint32_t span = 1, i;
    int depth;

    if (block == 0) {
        *need += last - first + 1;
        while (level-- > 0) {
            first /= per;
            last /= per;
            *need += last - first + 1;
        }
        return 0;
    }
    if (!ext2_data_block_ok(em, block)) {
        klog(KLOG_WARN, "ext2", "inode %d: block %d is out of range", (int)n->ino, (int)block);
        ext2_mark_error(em);
        return -EIO;
    }
    if (level == 0) {
        return 0;
    }
    for (depth = 1; depth < level; depth++) {
        span *= per;
    }
    for (i = first / span; i <= last / span; i++) {
        uint8_t raw[4];
        uint32_t begin = i == first / span ? first % span : 0;
        uint32_t end = i == last / span ? last % span : span - 1;
        int rc;

        rc = ext2_read_bytes(em, (uint64_t)block * em->block_size + (uint64_t)i * 4, raw, 4);
        if (rc == 0) {
            struct writeback_step under;

            under.block = ext2_le32(raw);
            under.level = level - 1;
            under.first = begin;
            under.last = end;
            rc = writeback_missing(em, n, &under, need);
        }
        if (rc != 0) {
            return rc;
        }
    }
    return 0;
}

/** Whether the blocks the write of `count` bytes at `offset` of `n` lacks,
 * data and pointer blocks, fit the free count, less the reserved blocks for
 * a caller that is not privileged: 0, -ENOSPC, or the errors of
 * `writeback_missing`. */
int ext2_writeback_space(struct ext2_mount *em, struct ext2_node *n, uint64_t offset,
                         uint32_t count)
{
    uint32_t first = (uint32_t)(offset >> em->block_shift);
    uint32_t last = (uint32_t)((offset + count - 1) >> em->block_shift);
    uint32_t base = 0, span = 1, slot;
    uint64_t need = 0, free;
    int level = 0;

    for (slot = 0; slot < EXT2_NDIR + 3 && base <= last; slot++) {
        if (slot >= EXT2_NDIR) {
            span *= em->block_size / 4;
            level++;
        }
        if (first < base + span) {
            uint32_t begin = first > base ? first - base : 0;
            uint32_t end = last < base + span ? last - base : span - 1;
            struct writeback_step step;
            int rc;

            step.block = n->block[slot];
            step.level = level;
            step.first = begin;
            step.last = end;
            rc = writeback_missing(em, n, &step, &need);

            if (rc != 0) {
                return rc;
            }
            free = ext2_free_blocks(em);
            if (need > free ||
                (!ext2_may_use_reserved() && need > 0 && free - need < em->r_blocks)) {
                return -ENOSPC;
            }
        }
        base += span;
    }
    return 0;
}

/** Put one piece of a write into the batch, after a drain when its bytes
 * would overflow the batch's room, and a drain and a second try when the
 * batch's tables are full: `writeback_piece`'s answer or the drain's error. */
int ext2_writeback_fit(struct ext2_mount *em, struct ext2_node *n, uint64_t at, const uint8_t *in,
                       uint32_t take)
{
    struct ext2_writeback *wb = em->wb;
    int rc;

    if (take > EXT2_WB_BYTES - wb->bytes) {
        rc = ext2_writeback_drain(em);
        if (rc != 0) {
            return rc;
        }
    }
    rc = writeback_piece(em, n, at, in, take);
    if (rc == -ENOMEM && wb->count) {
        rc = ext2_writeback_drain(em);
        if (rc == 0) {
            rc = writeback_piece(em, n, at, in, take);
        }
    }
    return rc;
}

/** Drain the batch after a piece when the write is `synchronous` or the
 * batch's bytes, images or inodes have run out. 0 or the drain's error. */
int ext2_writeback_room(struct ext2_mount *em, int synchronous)
{
    const struct ext2_writeback *wb = em->wb;

    if (synchronous || wb->bytes >= EXT2_WB_BYTES || wb->datas == EXT2_WB_DATA ||
        wb->metas == EXT2_WB_META || wb->count == EXT2_WB_NODES) {
        return ext2_writeback_drain(em);
    }
    return 0;
}

/**
 * Set up the file-write batch and clear the superblock's valid bit on the
 * medium, before the mount is published or made writable.
 *
 * Each buffer is one page of the heap, which is one cache block, so no
 * buffer needs pages next to another's. A mount that cannot have them all
 * fails with ENOMEM, gives back what it took and leaves `em->wb` NULL, so
 * that a later remount sets the batch up again. 0, -ENOMEM, or the error of
 * `ext2_sb_store`, after which the batch stays.
 */
int ext2_writeback_init(struct ext2_mount *em)
{
    struct ext2_writeback *wb = kzalloc(sizeof(*wb));
    int i;

    if (!wb) {
        return -ENOMEM;
    }
    spin_lock_init(&wb->lock, "ext2 batch lists");
    for (i = 0; i < EXT2_WB_DATA + EXT2_WB_META; i++) {
        wb->slot[i] = kalloc_pages(1, 0);
        if (!wb->slot[i]) {
            ext2_writeback_free(wb);
            return -ENOMEM;
        }
    }
    em->wb = wb;
    (void)ext2_state_change(em, 0, EXT2_VALID_FS);
    return ext2_sb_store(em);
}
