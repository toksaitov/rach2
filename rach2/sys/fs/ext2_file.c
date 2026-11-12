/*
 * ext2_file.c: an ext2 file's data: reads, writes, the growth and truncation of
 * its block map, fallocate, fsync and cachestat.
 */

#include "ext2_internal.h"

/** The `open` of `vfs_ops`: a new handle holding the node once more, with
 * a directory cursor not yet validated; `flags` is not read. Returns
 * -ENOMEM when the handle cannot be allocated, or -EACCES for a device
 * node, since devices are opened in `/dev`. */
int ext2_open(struct vfs_mount *mnt, void *node, int flags, void **handle)
{
    struct ext2_node *n = node;
    struct ext2_handle *h;
    uint32_t type = n->mode & S_IFMT_;

    (void)mnt;
    (void)flags;
    if (type == S_IFCHR_ || type == S_IFBLK_) {
        return -EACCES;
    }
    h = kzalloc(sizeof(*h));
    if (!h) {
        return -ENOMEM;
    }
    h->node = n;
    h->cursor = ~0u;
    ext2_node_ref(n, 1);
    *handle = h;
    return 0;
}

/** The `release` of `vfs_ops`, at a description's last close: the batch
 * drained, a failure kept for the next `fsync`, and the hold `ext2_open`
 * took put. The drain, of a batch that is not empty or of a device that
 * went, which records the failure, runs under the batch and buffer locks;
 * a final put takes the big lock for the node table. */
void ext2_release(struct vfs_mount *mnt, void *handle)
{
    struct ext2_handle *h = handle;

    (void)ext2_writeback_drain(ext2_em_of(mnt));
    ext2_put(mnt, h->node);
    kfree(h);
}

/* The most times a read or a write brings the pages of a user buffer in
 * again with no byte moved between, before it answers -EFAULT, and the most
 * blocks a read fetches for one piece with the locks let go: the three
 * pointer levels and the data block. */
#define EXT2_REFAULTS 8
#define EXT2_FETCHES  4

/**
 * Bring in the pages of the user buffer [addr, addr + len) that a copy
 * under node `n`'s lock found absent (`vm_resident`), with `n`'s lock let
 * go around the faults, since a fault under a file lock could wait for
 * another file's; `write` is nonzero for a buffer the copy
 * fills. Returns 1 once the pages are in, after which `n` may have
 * changed; 0, with nothing let go, when the caller holds a file lock of
 * its own, `n`'s or another's, which this cannot give back, so that the
 * call ends short and its caller brings the pages in; or -EFAULT for
 * pages that cannot be had.
 */
static int user_refault(struct ext2_node *n, const void *addr, uint32_t len, int write)
{
    int mine = sleep_lock_held(&n->lock) ? 1 : 0;
    int rc;

    if (current_proc && (current_proc->file_locks != mine || n->lock.depth > 1)) {
        return 0;
    }
    ext2_node_unlock(n);
    rc = vm_prefault_range((uintptr_t)addr, len, write) ? 1 : -EFAULT;
    ext2_node_lock(n);
    return rc;
}

/**
 * The first block on the way to byte `at` of `n` that neither the cache
 * nor the batch has, a pointer block or the data block, into `*blk` as a
 * block of the cache: 1, or 0 when the way is had whole, ends in a hole, or
 * meets a fault, which the read under the lock then reports. Reads only
 * pointer blocks the cache has, so it waits on the disk only for one
 * evicted after the look.
 */
static int read_missing(struct ext2_mount *em, const struct ext2_node *n, uint64_t at,
                        uint64_t *blk)
{
    uint32_t per = em->block_size / 4;
    uint32_t index = (uint32_t)(at >> em->block_shift);
    uint32_t b, span = per, div = 1;
    int level, i;

    if (index < EXT2_NDIR) {
        b = n->block[index];
        level = 0;
    } else {
        index -= EXT2_NDIR;
        for (level = 1; level <= 3 && index >= span; level++) {
            index -= span;
            span *= per;
        }
        if (level > 3) {
            return 0;
        }
        b = n->block[EXT2_NDIR + level - 1];
        for (i = 1; i < level; i++) {
            div *= per;
        }
    }
    for (;;) {
        uint64_t off = (uint64_t)b * em->block_size;
        uint8_t ptr[4];

        if (b == 0 || !ext2_data_block_ok(em, b)) {
            return 0;
        }
        *blk = off >> BCACHE_BLOCK_SHIFT;
        if (!ext2_writeback_has(em, (uint32_t)*blk) && !bcache_cached(em->dev, *blk)) {
            return 1;
        }
        if (level == 0) {
            return 0;
        }
        if (ext2_read_bytes(em, off + (uint64_t)(index / div) * 4, ptr, 4) != 0) {
            return 0;
        }
        b = ext2_le32(ptr);
        index %= div;
        div /= per;
        level--;
    }
}

/**
 * Bring the first block on the way to byte `at` of `n` that the cache
 * lacks into it, with `n`'s lock let go, so that a disk that takes seconds
 * holds up no other call on the file. The block is only a hint here: the
 * caller, under the lock again, looks the file up anew, and a block freed
 * or evicted meanwhile costs it a read under it. Whether the lock was let
 * go, after which `n` may have changed; a caller that took it more than
 * once keeps it across the fetch.
 */
static int read_fetch(struct vfs_mount *mnt, struct ext2_node *n, uint64_t at)
{
    struct ext2_mount *em = ext2_em_of(mnt);
    uint64_t blk;
    struct bbuf *b;

    if (!read_missing(em, n, at, &blk)) {
        return 0;
    }
    ext2_node_unlock(n);
    b = bcache_get(em->dev, blk);
    if (b) {
        bcache_release(b);
    }
    ext2_node_lock(n);
    return 1;
}

/**
 * Copy `len` bytes of `n`'s data at `at`, inside one block, into `dst`, a
 * hole as zeros. The block, or the batch's image of it, is had first, and
 * the check that `dst` is present (`vm_resident`) and the copy follow it
 * with no sleep between, so that a page of a user buffer that went is found
 * before the copy and not by a fault under `n`'s lock. 0; -EFAULT, with
 * nothing copied, for a page of `dst` that is not present; or the errors of
 * `ext2_block_at`.
 */
static int read_piece(struct ext2_mount *em, const struct ext2_node *n, uint64_t at, uint8_t *dst,
                      uint32_t len)
{
    struct bbuf *buf;
    uint64_t off;
    uint32_t b, in;
    int rc = ext2_block_at(em, n, (uint32_t)(at >> em->block_shift), &b);

    if (rc != 0) {
        return rc;
    }
    if (b == 0) {
        if (!vm_resident((uintptr_t)dst, len, 1)) {
            return -EFAULT;
        }
        k_memset(dst, 0, len);
        return 0;
    }
    off = (uint64_t)b * em->block_size + ((uint32_t)at & (em->block_size - 1));
    in = (uint32_t)off & (BCACHE_BLOCK_BYTES - 1);
    if (ext2_writeback_has(em, (uint32_t)(off >> BCACHE_BLOCK_SHIFT))) {
        if (!vm_resident((uintptr_t)dst, len, 1)) {
            return -EFAULT;
        }
        if (ext2_writeback_copy(em, (uint32_t)(off >> BCACHE_BLOCK_SHIFT), in, dst, len, 0)) {
            return 0;
        }
    }
    buf = bcache_get(em->dev, off >> BCACHE_BLOCK_SHIFT);
    if (!buf) {
        return -EIO;
    }
    if (in + len > buf->bytes) {
        rc = -EIO;
    } else if (!vm_resident((uintptr_t)dst, len, 1)) {
        rc = -EFAULT;
    } else {
        k_memcpy(dst, buf->data + in, len);
    }
    bcache_release(buf);
    return rc;
}

/**
 * The `read` of `vfs_ops`: up to `count` bytes at `offset`, ending at the
 * size, the batch's pending writes included and a hole read as zeros, under
 * the node's lock, which a block the cache lacks on the way to the data
 * lets go (`read_fetch`); the access time does not change. A page of the
 * buffer that is not present at the copy is brought in with the lock let
 * go (`user_refault`), or ends the read short when the caller holds a file
 * lock. A caller without the big lock passes a kernel buffer; the batch's
 * images it finds are copied under the batch's lists' lock in the step
 * that finds them (`ext2_writeback_copy`), and a drain puts an image in the
 * cache before it lets the image go. The bytes read, -EISDIR, -EFAULT, or
 * -EIO.
 */
int32_t ext2_read(struct vfs_mount *mnt, void *handle, void *buf, uint32_t count, uint64_t offset)
{
    struct ext2_mount *em = ext2_em_of(mnt);
    struct ext2_node *n = handle;
    uint8_t *out = buf;
    uint32_t done = 0;
    int rc = 0, fetches = 0, refaults = 0;

    if ((n->mode & S_IFMT_) == S_IFDIR_) {
        return -EISDIR;
    }
    ext2_node_lock(n);
    while (rc == 0 && done < count && offset + done < n->size) {
        uint64_t at = offset + done;
        uint32_t take = em->block_size - ((uint32_t)at & (em->block_size - 1));

        if (fetches < EXT2_FETCHES && read_fetch(mnt, n, at)) {
            fetches++;
            continue;
        }
        if (take > count - done) {
            take = count - done;
        }
        if (take > n->size - at) {
            take = (uint32_t)(n->size - at);
        }
        rc = read_piece(em, n, at, out + done, take);
        if (rc == -EFAULT) {
            rc = ++refaults <= EXT2_REFAULTS ? user_refault(n, out + done, take, 1) : -EFAULT;
            if (rc == 1) {
                rc = 0;
                fetches = 0;
                continue;
            }
            break;
        }
        if (rc == 0) {
            done += take;
            fetches = 0;
            refaults = 0;
        }
    }
    ext2_node_unlock(n);
    if (rc == -EFAULT && done > 0) {
        rc = 0;
    }
    return rc != 0 ? rc : (int32_t)done;
}

/* The pages one plan of `ext2_read_frames` covers on its stack, and with a
 * plan from the heap for a longer read the most it covers: what one request
 * to a device moves, so that a window read ahead goes to the disk in
 * requests as long as Linux's, whose bio for a window takes up to 256
 * pages (BIO_MAX_VECS). */
#define EXT2_FRAMES_PLAN 32u
#define EXT2_FRAMES_MOST (BLOCK_REQ_MAX_SECTORS / (PAGE_SIZE / BLOCK_SECTOR_SIZE))
/* What a plan found for a page: a hole, or a page another copy of which
 * may be newer than the disk's, which `ext2_read` reads; any other plan is
 * the page's first sector, which is past both, since block 0 holds no data
 * and a block of 1 KiB at 1 is the superblock. */
#define EXT2_PAGE_HOLE   ((uint64_t)0)
#define EXT2_PAGE_COPY   ((uint64_t)1)

/**
 * Find the blocks of page `pg` of `n`, for `frame_plan`: `*first` is the
 * first one's, 0 for a hole, and EXT2_PAGE_COPY when they are neither all
 * holes nor one run on the device, which only blocks smaller than a page
 * can be. 0, or the errors of `ext2_block_at`.
 */
static int frame_blocks(struct ext2_mount *em, const struct ext2_node *n, uint32_t pg,
                        uint64_t *first)
{
    uint32_t at = (uint32_t)(((uint64_t)pg * PAGE_SIZE) >> em->block_shift);
    uint32_t per = em->block_size < PAGE_SIZE ? (uint32_t)PAGE_SIZE >> em->block_shift : 1u;
    uint32_t b0 = 0, b, i;
    int rc;

    for (i = 0; i < per; i++) {
        rc = ext2_block_at(em, n, at + i, &b);
        if (rc != 0) {
            return rc;
        }
        if (i == 0) {
            b0 = b;
        } else if (b != (b0 == 0 ? 0 : b0 + i)) {
            *first = EXT2_PAGE_COPY;
            return 0;
        }
    }
    *first = b0;
    return 0;
}

/**
 * Find where page `pg` of `n` lies, for `ext2_read_frames`: the device
 * sector its bytes start at, when they are one run of sectors no other copy
 * of which may be newer, EXT2_PAGE_HOLE for a hole, or EXT2_PAGE_COPY for a
 * page `ext2_read` must read: a block the batch has an image of or the
 * cache holds, either of which may be newer than the device, or a page of
 * smaller blocks that are not one run, as Linux's `do_mpage_readpage` gives
 * such a page to `block_read_full_folio`. 0, or the errors of `ext2_block_at`.
 * The caller holds `n`'s lock, so that no write of the file changes a
 * block after the look; a drain of another file's write moves this file's
 * images into the cache before it lets them go, so a block is in one or the
 * other at every look.
 */
static int frame_plan(struct ext2_mount *em, const struct ext2_node *n, uint32_t pg, uint64_t *plan)
{
    uint64_t at = (uint64_t)pg * PAGE_SIZE, off, b, u;
    int rc;

    rc = frame_blocks(em, n, pg, &b);
    if (rc != 0 || b == EXT2_PAGE_HOLE || b == EXT2_PAGE_COPY) {
        *plan = b;
        return rc;
    }
    off = (b << em->block_shift) + ((uint32_t)at & (em->block_size - 1));
    for (u = off >> BCACHE_BLOCK_SHIFT; u <= (off + PAGE_SIZE - 1) >> BCACHE_BLOCK_SHIFT; u++) {
        if (ext2_writeback_has(em, (uint32_t)u) || bcache_cached(em->dev, u)) {
            *plan = EXT2_PAGE_COPY;
            return 0;
        }
    }
    *plan = off >> 9; /* sectors of 512 bytes */
    return 0;
}

/**
 * Read `left` pages from page `pg` of `n`, whose plans are `plan`, into
 * `frames`, under `n`'s lock: a run of pages whose sectors follow each
 * other in one transfer straight into the frames (`block_read_frames`), a
 * hole as zeros and a copied page through `ext2_read`; the bytes past the
 * file's size are cleared. The pages filled, from the first; or the first
 * page's error.
 */
static int32_t frame_fill(struct vfs_mount *mnt, struct ext2_node *n, uint32_t pg, uint32_t left,
                          const uint64_t *plan, const uintptr_t *frames)
{
    struct ext2_mount *em = ext2_em_of(mnt);
    uint64_t size = n->size;
    uint32_t i = 0, run;
    int32_t rc = 0;
    uint8_t *page;

    while (i < left) {
        uint64_t at = (uint64_t)(pg + i) * PAGE_SIZE;

        if (plan[i] == EXT2_PAGE_HOLE) {
            rc = paging_zero_frame(frames[i]) == 0 ? 0 : -EIO;
            run = 1;
        } else if (plan[i] == EXT2_PAGE_COPY) {
            page = kmap_temp(frames[i]);
            rc = page ? ext2_read(mnt, n, page, PAGE_SIZE, at) : -EIO;
            if (rc >= 0 && rc < (int32_t)PAGE_SIZE) {
                k_memset(page + rc, 0, PAGE_SIZE - (uint32_t)rc);
            }
            if (page) {
                kunmap_temp(page);
            }
            rc = rc < 0 ? rc : 0;
            run = 1;
        } else {
            for (run = 1; i + run < left && plan[i + run] > EXT2_PAGE_COPY &&
                          plan[i + run] == plan[i] + run * (PAGE_SIZE / BLOCK_SECTOR_SIZE);
                 run++) {
            }
            rc = block_read_frames(em->dev, plan[i], run * (PAGE_SIZE / BLOCK_SECTOR_SIZE),
                                   frames + i);
        }
        if (rc != 0) {
            break;
        }
        i += run;
        if (at + (uint64_t)run * PAGE_SIZE > size && plan[i - 1] > EXT2_PAGE_COPY) {
            page = kmap_temp(frames[i - 1]);
            if (!page) {
                rc = -EIO;
                i--;
                break;
            }
            k_memset(page + (uint32_t)(size & (PAGE_SIZE - 1)), 0,
                     PAGE_SIZE - (uint32_t)(size & (PAGE_SIZE - 1)));
            kunmap_temp(page);
        }
    }
    return i > 0 ? (int32_t)i : rc;
}

/**
 * The `read_frames` of `vfs_ops`: pages `first` to `first + count - 1` of a
 * regular file into `frames`, ending at the size, as Linux's
 * `mpage_readahead` reads a file's pages into the page cache's folios
 * (fs/mpage.c): a page whose blocks follow on the device goes into one
 * transfer with its neighbours, made on the frames themselves, so that no
 * buffer of the cache holds the bytes and no copy moves them; a hole is
 * zeroed; and a page the cache may hold newer bytes of, or whose smaller
 * blocks are not one run, is read as `ext2_read` reads it, as
 * `do_mpage_readpage` falls back to `block_read_full_folio` for a page with
 * buffers or with blocks out of line. Data written to a file goes through
 * the batch and the cache, so the device's bytes of a block that neither
 * holds are the file's.
 *
 * The node's lock is held across each plan and its transfers, so that no
 * write, truncation or free of the file changes a planned block before it
 * is read, as Linux's locked folio holds off a truncation of its page until
 * its read ends (`truncate_inode_pages_range`, mm/truncate.c). The pages
 * filled, 0 at or past the end, -EISDIR, or the first page's error.
 */
int32_t ext2_read_frames(struct vfs_mount *mnt, void *handle, uint32_t first, uint32_t count,
                         const uintptr_t *frames)
{
    struct ext2_mount *em = ext2_em_of(mnt);
    struct ext2_node *n = handle;
    uint64_t small[EXT2_FRAMES_PLAN], *plan = small, size;
    uint32_t done = 0, pages, left, i, room = EXT2_FRAMES_PLAN;
    int32_t got;
    int rc = 0;

    if ((n->mode & S_IFMT_) != S_IFREG_) {
        return (n->mode & S_IFMT_) == S_IFDIR_ ? -EISDIR : -EINVAL;
    }
    if (count > room) {
        room = count < EXT2_FRAMES_MOST ? count : EXT2_FRAMES_MOST;
        plan = kmalloc(room * sizeof(*plan));
        if (!plan) {
            plan = small;
            room = EXT2_FRAMES_PLAN;
        }
    }
    ext2_node_lock(n);
    size = n->size;
    pages = (uint32_t)((size + PAGE_SIZE - 1) >> 12);
    if (first >= pages) {
        ext2_node_unlock(n);
        if (plan != small) {
            kfree(plan);
        }
        return 0;
    }
    if (count > pages - first) {
        count = pages - first;
    }
    while (done < count) {
        left = count - done < room ? count - done : room;
        for (i = 0; i < left; i++) {
            rc = frame_plan(em, n, first + done + i, &plan[i]);
            if (rc != 0) {
                break;
            }
        }
        if (i == 0) {
            break;
        }
        got = frame_fill(mnt, n, first + done, i, plan, frames + done);
        if (got <= 0) {
            rc = got;
            break;
        }
        done += (uint32_t)got;
        if ((uint32_t)got < i || rc != 0) {
            break;
        }
    }
    ext2_node_unlock(n);
    if (plan != small) {
        kfree(plan);
    }
    return done > 0 ? (int32_t)done : rc;
}

/* ---- the block map, written ---- */

/* The blocks a free of a file collects before it frees them, and the
 * collection when the heap has no room for that many. */
#define PUNCH_BLOCKS 512
#define PUNCH_SMALL  16
/* The pointer slots one walk reads between service points: the list fills
 * by blocks freed, and a sparse tree reads many empty slots for each. */
#define PUNCH_VISITS 1024

/* `punch_level`'s answer when the list is full: the walk stops, the blocks
 * listed are freed, and it starts again from the end. */
#define PUNCH_FULL 1

/* One walk of `punch_level`: the file `n` of `em` cut from data index `keep`
 * on, its blocks going into `list`. */
struct punch_walk {
    struct ext2_mount *em;
    struct ext2_node *n;
    uint32_t keep;
    struct punch_list *list;
    uint32_t visits; /* slots read since the last service point */
};

/**
 * Clear the pointers of `n`'s blocks from data index `keep` on below the
 * pointer `*slot`, of `level` (0 for a data block) and covering the data
 * from index `base`, from the end, and list each block whose pointer is
 * cleared in `list`. A pointer is cleared at `slot_at` on the volume,
 * dirtied `first`, or for 0 in the inode in memory, which the caller stores;
 * a pointer block whose children are all gone goes the same way. No bit
 * changes here. `*emptied` says whether the pointer is now 0, and
 * `n->blocks` is kept current. A slot's read may sleep, so a service point
 * before it asks nothing more of the walk: every removal so far is whole,
 * and the listed blocks stay taken until the round frees them. 0, PUNCH_FULL
 * when the list has no room for the next block, or -errno, -EIO for a
 * pointer block out of range.
 */
static int punch_level(struct punch_walk *w, uint32_t *slot, uint64_t slot_at, int level,
                       uint32_t base, int *emptied)
{
    struct ext2_mount *em = w->em;
    struct ext2_node *n = w->n;
    struct punch_list *list = w->list;
    uint32_t keep = w->keep;
    uint32_t b = *slot;
    uint32_t per = em->block_size / 4;
    uint32_t span = 1;
    uint32_t sectors = em->block_size / 512;
    int i, rc;

    *emptied = 0;
    if (b == 0) {
        *emptied = 1;
        return 0;
    }
    for (i = 0; i < level; i++) {
        span *= per;
    }
    if (level > 0) {
        int all_gone = 1;
        uint32_t k;

        if (!ext2_data_block_ok(em, b)) {
            return -EIO;
        }
        for (k = per; k > 0; k--) {
            uint32_t child_base = base + (k - 1) * (span / per);
            uint8_t ptr[4];
            uint32_t child;
            uint64_t at = (uint64_t)b * em->block_size + (uint64_t)(k - 1) * 4;
            int gone;

            if (child_base + span / per <= keep) {
                all_gone = 0;
                break; /* everything below this slot stays */
            }
            if (++w->visits >= PUNCH_VISITS) {
                w->visits = 0;
                sched_service_point();
            }
            rc = ext2_read_bytes(em, at, ptr, 4);
            if (rc != 0) {
                return rc;
            }
            child = ext2_le32(ptr);
            if (child == 0) {
                continue;
            }
            rc = punch_level(w, &child, at, level - 1, child_base, &gone);
            if (rc != 0) {
                return rc;
            }
            if (!gone) {
                all_gone = 0;
                break;
            }
        }
        if (!all_gone) {
            return 0;
        }
    }
    if (list->count == list->cap) {
        return PUNCH_FULL;
    }
    if (slot_at != 0) {
        uint8_t zero[4] = {0, 0, 0, 0};

        rc = ext2_write_owned(em, n->ino, slot_at, zero, 4, WRITE_FIRST);
        if (rc != 0) {
            return rc;
        }
    }
    *slot = 0;
    list->blocks[list->count++] = b;
    if (n->blocks >= sectors) {
        n->blocks -= sectors;
    }
    *emptied = 1;
    return 0;
}

/** Clear `n`'s pointers from data index `keep` on by `punch_level`, listing
 * their blocks: the triple, double and single indirect trees first, then the
 * direct blocks from the last. 0, PUNCH_FULL, or -errno. */
static int punch_from(struct ext2_mount *em, struct ext2_node *n, uint32_t keep,
                      struct punch_list *list)
{
    uint32_t per = em->block_size / 4;
    struct punch_walk w;
    int level, i, rc, gone;

    w.em = em;
    w.n = n;
    w.keep = keep;
    w.list = list;
    w.visits = 0;
    for (level = 3; level >= 1; level--) {
        uint32_t lbase = EXT2_NDIR, lspan = per;
        int l;

        for (l = 1; l < level; l++) {
            lbase += lspan;
            lspan *= per;
        }
        if (lbase + lspan > keep && n->block[EXT2_NDIR + level - 1] != 0) {
            rc = punch_level(&w, &n->block[EXT2_NDIR + level - 1], 0, level, lbase, &gone);
            if (rc != 0) {
                return rc;
            }
        }
    }
    for (i = (int)EXT2_NDIR - 1; i >= (int)keep && i >= 0; i--) {
        if (n->block[i] != 0) {
            rc = punch_level(&w, &n->block[i], 0, 0, (uint32_t)i, &gone);
            if (rc != 0) {
                return rc;
            }
        }
    }
    return 0;
}

/**
 * Free the blocks of `n` from data index `keep` on, in two phases a round,
 * under `n`'s lock. First up to PUNCH_BLOCKS pointers are cleared from the
 * end and the inode stored, all dirtied `first`, their blocks collected on
 * the caller's stack; then those blocks are freed a group at a time with
 * their bitmaps and descriptors dirtied `after` (`ext2_punch_free`). The cache
 * writes every `first` block before any `after` one, whichever path writes
 * (bcache.h), so no pointer on the disk names a block whose bit is clear;
 * two punches of two files keep that by the same two classes and share
 * nothing else. A strict mount syncs between the phases and after the last.
 * Freed one by one and written through, a shrink would cost three requests
 * a block.
 *
 * A batch that holds `n` is drained first, so that no block is freed while a
 * pending piece names it. A failure still frees what its round listed, in
 * the same order, once the inode is stored and a strict mount's sync has
 * put the pointers on the medium; otherwise the blocks stay taken, lost
 * blocks for e2fsck, and the free counts stay exact. A device that failed a
 * write is -EIO and not written to again: the blocks go to the cache first
 * and would reach the device at the sync that ends the round, past the check
 * a write through makes. A round ends at a service point, since a file of
 * gigabytes in the cache frees with no sleep and what is ready on the
 * processor waits for it; a device that failed meanwhile stops the next
 * round. The caller holds `n`'s lock and not `em->batch`. 0, or -errno.
 */
int ext2_punch(struct ext2_mount *em, struct ext2_node *n, uint32_t keep)
{
    uint32_t small[PUNCH_SMALL];
    struct punch_list list;
    int rc, err;

    ext2_assert_node_locked(n);
    if (em->dev->flags & BLOCK_WRITE_FAILED) {
        return -EIO;
    }
    if (ext2_writeback_holds(em, n)) {
        rc = ext2_writeback_drain(em);
        if (rc != 0) {
            return rc;
        }
    }
    list.blocks = kmalloc(PUNCH_BLOCKS * sizeof(uint32_t));
    list.cap = list.blocks ? PUNCH_BLOCKS : PUNCH_SMALL;
    if (!list.blocks) {
        list.blocks = small;
    }
    do {
        list.count = 0;
        rc = punch_from(em, n, keep, &list);
        if (list.count == 0) {
            break;
        }
        err = em->dev->flags & BLOCK_WRITE_FAILED ? -EIO : ext2_inode_write(em, n, 0, WRITE_FIRST);
        if (err == 0) {
            err = ext2_em_sync(em);
        }
        if (err == 0) {
            err = ext2_punch_free(em, n, &list);
        }
        if (err == 0) {
            err = ext2_em_sync(em);
        }
        if (err != 0 && rc >= 0) {
            rc = err;
        }
        sched_service_point();
        if (rc == PUNCH_FULL && (em->dev->flags & BLOCK_WRITE_FAILED)) {
            rc = -EIO;
        }
    } while (rc == PUNCH_FULL);
    if (list.blocks != small) {
        kfree(list.blocks);
    }
    if (rc == 0) {
        ext2_em_balance(em);
    }
    return rc;
}

/** Point the slot at byte `slot_at` of a pointer block of `owner`'s tree,
 * at `level` there, to block `b`, as `ext2_map_alloc` does. 0, or -errno. */
static int map_point(struct ext2_mount *em, uint32_t owner, uint64_t slot_at, uint32_t b, int level)
{
    uint8_t raw[4];

    LE32_PUT(raw, b);
    return ext2_write_owned(em, owner, slot_at, raw, 4, WRITE_LEVEL(level) | WRITE_THROUGH);
}

/**
 * The block for data index `index` of `n`, allocated when missing, into
 * `*out`, with `*fresh` set for a new data block. A missing pointer block is
 * allocated and its zeros written before the pointer to it, so that the
 * pointer cannot reach the disk over what the block held before; an inode
 * that has that pointer is stored at once. A data block's pointer is
 * written after the block is in the bitmap, and a pointer block's after the
 * block's zeros. Each pointer block is marked at its level (`WRITE_LEVEL`),
 * so that a mount that writes back keeps that order too. A directory's
 * blocks are shared, as its entries are, and a regular file's are its own.
 * `n->blocks` and the inode's own pointers change in memory; the caller
 * stores the inode with them, and holds `n`'s lock and not `em->batch`. 0,
 * -EFBIG past the triple indirect's reach, -ENOSPC, or -errno.
 */
int ext2_map_alloc(struct ext2_mount *em, struct ext2_node *n, uint32_t index, uint32_t *out,
                   int *fresh)
{
    uint32_t per = em->block_size / 4;
    uint32_t sectors = em->block_size / 512;
    uint32_t *slot;
    uint32_t held;        /* a slot's value, read from its pointer block */
    uint64_t slot_at = 0; /* 0: the slot is in the inode */
    uint32_t hint = ext2_alloc_hint(em);
    uint32_t owner = (n->mode & S_IFMT_) == S_IFREG_ ? n->ino : 0;
    int level, rc;

    *fresh = 0;
    if (index < EXT2_NDIR) {
        slot = &n->block[index];
    } else {
        uint32_t span = per;
        uint32_t rest;

        index -= EXT2_NDIR;
        for (level = 1; level <= 3; level++) {
            if (index < span) {
                break;
            }
            index -= span;
            span *= per;
        }
        if (level > 3) {
            return -EFBIG;
        }
        slot = &n->block[EXT2_NDIR + level - 1];
        rest = index;
        while (level > 0) {
            uint32_t div = 1, b;
            uint8_t ptr[4];
            int i;

            for (i = 1; i < level; i++) {
                div *= per;
            }
            if (*slot == 0) {
                rc = ext2_alloc_block(em, hint, &b);
                if (rc != 0) {
                    return rc;
                }
                ext2_node_share_group(em, n, (b - em->first_data) / em->bpg, 0);
                rc = ext2_write_owned(em, owner, (uint64_t)b * em->block_size, 0, em->block_size,
                                      WRITE_LEVEL(level) | WRITE_THROUGH);
                if (rc != 0) {
                    return rc;
                }
                n->blocks += sectors;
                if (slot_at != 0) {
                    rc = map_point(em, owner, slot_at, b, level + 1);
                    if (rc != 0) {
                        return rc;
                    }
                } else {
                    *slot = b;
                    rc = ext2_inode_store(em, n);
                    if (rc != 0) {
                        return rc;
                    }
                }
                *slot = b;
                hint = b + 1;
            }
            b = *slot;
            if (!ext2_data_block_ok(em, b)) {
                return -EIO;
            }
            slot_at = (uint64_t)b * em->block_size + (uint64_t)(rest / div) * 4;
            rc = ext2_read_bytes(em, slot_at, ptr, 4);
            if (rc != 0) {
                return rc;
            }
            held = ext2_le32(ptr);
            slot = &held;
            rest %= div;
            level--;
        }
    }
    if (*slot == 0) {
        uint32_t b;

        rc = ext2_alloc_block(em, hint, &b);
        if (rc != 0) {
            return rc;
        }
        ext2_node_share_group(em, n, (b - em->first_data) / em->bpg, 0);
        n->blocks += sectors;
        if (slot_at != 0) {
            rc = ext2_write_owned(em, owner, (uint64_t)b * em->block_size, 0, em->block_size,
                                  WRITE_THROUGH);
            if (rc == 0) {
                rc = map_point(em, owner, slot_at, b, 1);
            }
            if (rc != 0) {
                return rc;
            }
        }
        *slot = b;
        *fresh = 1;
    } else if (!ext2_data_block_ok(em, *slot)) {
        return -EIO;
    }
    *out = *slot;
    return 0;
}

/* ---- the operations ---- */

/**
 * The `setattr` of `vfs_ops`: the permission bits, the owner, the group, the
 * access and modification times and the flags chattr may change, as `what`
 * selects, with the change time set to the present and the inode stored.
 * A strict mount drains the batch first (`ext2_assert_writable`). Takes the
 * node's lock, which the caller may hold already. 0, -EROFS, or -errno.
 */
int ext2_setattr(struct vfs_mount *mnt, void *node, int what, const struct vfs_stat *st)
{
    struct ext2_mount *em = ext2_em_of(mnt);
    struct ext2_node *n = node;
    int rc;

    ext2_node_lock(n);
    rc = ext2_assert_writable(mnt, em);
    if (rc != 0) {
        ext2_node_unlock(n);
        return rc;
    }
    if (what & VFS_SET_MODE) {
        n->mode = (n->mode & S_IFMT_) | (st->mode & 07777u);
    }
    if (what & VFS_SET_UID) {
        n->uid = st->uid;
    }
    if (what & VFS_SET_GID) {
        n->gid = st->gid;
    }
    if (what & VFS_SET_ATIME) {
        n->atime = ext2_time_fit(n, EXT2_ATIME_EXTRA + 4, st->atime_sec, st->atime_nsec);
    }
    if (what & VFS_SET_MTIME) {
        n->mtime = ext2_time_fit(n, EXT2_MTIME_EXTRA + 4, st->mtime_sec, st->mtime_nsec);
    }
    if (what & VFS_SET_FLAGS) {
        n->flags = (n->flags & ~EXT2_FL_USER_MODIFIABLE) | (st->flags & EXT2_FL_USER_MODIFIABLE);
    }
    ext2_stamp(n, T_CTIME);
    rc = ext2_inode_store(em, n);
    ext2_node_unlock(n);
    return rc;
}

/**
 * Refuse `offset` for the operation `op`, past the largest file this volume
 * can address, with a record of the offset and the limit: the limit is the
 * block pointers' reach or the 32-bit `i_blocks`, whichever is smaller. The
 * seek that put the descriptor there was legal; the write or truncate is
 * not. Returns -EFBIG.
 */
static int past_limit(const struct ext2_mount *em, const char *op, uint64_t offset)
{
    klog(KLOG_WARN, "ext2",
         "%s: offset %d MiB is past the file limit of %d MiB "
         "for %d-byte blocks",
         op, (int)(offset >> 20), (int)(em->file_limit >> 20), (int)em->block_size);
    return -EFBIG;
}

/**
 * The `truncate` of `vfs_ops`: the size of regular file `handle` made
 * `size`, under the node's lock, which the caller may hold already; both
 * directions set the modification and change times.
 *
 * A shrink writes the smaller size first, then zeroes the tail of the last
 * block kept, so that a later growth over it reads zeros and not what was
 * there, then frees the blocks from the end (`ext2_punch`) and stores the inode
 * again; a stop between leaves blocks past the end. A growth leaves the gap
 * a hole, and puts the zeroed tail a shrink may have left unsynced on the
 * disk before the size covers it. A growth past the volume's file limit is
 * -EFBIG; a shrink is not checked against it, so a file larger than the
 * limit, which a volume may hold from another system, can still be cut, as
 * Linux's `inode_newsize_ok` checks `s_maxbytes` only for growth. 0, -EINVAL
 * for a file that is not regular, -EFBIG, -EROFS, or -errno.
 */
int ext2_truncate(struct vfs_mount *mnt, void *handle, uint64_t size)
{
    struct ext2_mount *em = ext2_em_of(mnt);
    struct ext2_node *n = handle;
    int rc;

    if ((n->mode & S_IFMT_) != S_IFREG_) {
        return -EINVAL;
    }
    ext2_node_lock(n);
    rc = ext2_assert_writable(mnt, em);
    if (rc == 0 && size > n->size && size > em->file_limit) {
        rc = past_limit(em, "truncate", size);
    }
    if (rc != 0) {
        ext2_node_unlock(n);
        return rc;
    }
    ext2_assert_node_locked(n);
    if (size < n->size) {
        uint32_t keep = (uint32_t)((size + em->block_size - 1) >> em->block_shift);

        ext2_node_size_set(n, size);
        ext2_stamp(n, T_MTIME | T_CTIME);
        rc = ext2_inode_store(em, n);
        if (rc == 0 && ((uint32_t)size & (em->block_size - 1)) != 0) {
            uint32_t b;

            rc = ext2_block_at(em, n, (uint32_t)(size >> em->block_shift), &b);
            if (rc == 0 && b != 0) {
                uint32_t in = (uint32_t)size & (em->block_size - 1);

                rc = ext2_write_owned(em, n->ino, (uint64_t)b * em->block_size + in, 0,
                                      em->block_size - in, 0);
            }
        }
        if (rc == 0) {
            rc = ext2_punch(em, n, keep);
        }
        if (rc == 0) {
            rc = ext2_inode_store(em, n);
        }
    } else if (size > n->size) {
        rc = ext2_em_sync(em);
        if (rc == 0) {
            ext2_node_size_set(n, size);
            ext2_stamp(n, T_MTIME | T_CTIME);
            rc = ext2_inode_store(em, n);
        }
    }
    ext2_node_unlock(n);
    return rc;
}

/* The rounds an `fsync` makes before it writes the whole device instead,
 * each over a node of its table block, the batch or a wait for the cache:
 * twice the most a block holds, since another process may list blocks for a
 * node already written. */
#define EXT2_TABLE_TRIES 64

/**
 * Write the shared blocks listed for `n`, then its own data and pointer
 * blocks (`bcache_sync_owned`), with no lock held. The list is taken and
 * emptied in one step, so a block dirtied for `n` during the writes is the
 * next call's, and a failure leaves `n`'s next `fsync` the whole device.
 * `n->writing` shows the blocks to other callers until they are written:
 * they wait for it (`node_writes_wait`) before they count on them, and find
 * a failure recorded when they wake. The list and the count change under
 * `em->share_lock`. The caller holds the big lock and has no batch
 * pending, since what the list names may be in its images. 0, or -errno.
 */
static int node_blocks_write(struct ext2_mount *em, struct ext2_node *n)
{
    uint32_t shared[EXT2_NODE_SHARED];
    uint32_t flags = spin_lock_irqsave(&em->share_lock);
    int count = n->nshared;
    int i, rc = 0, last;

    k_memcpy(shared, n->shared, sizeof(shared));
    n->nshared = 0;
    n->writing++;
    spin_unlock_irqrestore(&em->share_lock, flags);
    for (i = 0; rc == 0 && i < count; i++) {
        rc = bcache_sync_block(em->dev, shared[i]);
    }
    if (rc == 0) {
        rc = bcache_sync_owned(em->dev, n->ino);
    }
    flags = spin_lock_irqsave(&em->share_lock);
    if (rc != 0) {
        n->shared_over = 1;
    }
    last = --n->writing == 0;
    spin_unlock_irqrestore(&em->share_lock, flags);
    if (rc != 0) {
        rc = ext2_writeback_error(em, rc);
    }
    if (last) {
        wq_wake_all(&em->node_wq);
    }
    return rc;
}

/** The wait condition of `node_writes_wait`, read as one word. */
static int node_not_writing(void *arg)
{
    return ((const volatile struct ext2_node *)arg)->writing == 0;
}

/** Wait for the `node_blocks_write` under way of `n`, which the caller
 * holds. No lock is held: such a write waits only for the disk. */
static void node_writes_wait(struct ext2_mount *em, struct ext2_node *n)
{
    while (!node_not_writing(n)) {
        wq_wait_event_nointr(&em->node_wq, node_not_writing, n);
    }
}

/**
 * A node with an inode in the same block of the cache as inode `ino`'s, or
 * of `ino` itself, whose shared blocks are listed or not yet written, or
 * NULL. A write of that block puts its inode on the device, pointers and
 * all, so what the inode depends on must go first. A dying node is left
 * out: its inode is going, and a failed one has no inode. The caller holds
 * the big lock, which keeps the node table, and the lists are read under
 * `em->share_lock`.
 */
static struct ext2_node *table_unsynced(struct ext2_mount *em, uint32_t ino)
{
    uint64_t table = ext2_inode_offset(em, ino) >> BCACHE_BLOCK_SHIFT;
    uint32_t reach = BCACHE_BLOCK_BYTES / em->inode_size - 1;
    uint32_t first = (ino - 1) / em->ipg * em->ipg + 1;
    uint32_t last = first + em->ipg - 1;
    uint32_t i;

    first = ino - first > reach ? ino - reach : first;
    last = last > ino + reach ? ino + reach : last;
    last = last > em->inodes ? em->inodes : last;
    for (i = first; i <= last; i++) {
        struct ext2_node *m = ext2_node_find(em, i);
        uint32_t flags;
        int listed;

        if (!m || (m->state != N_LIVE && m->state != N_LOADING) ||
            ext2_inode_offset(em, i) >> BCACHE_BLOCK_SHIFT != table) {
            continue;
        }
        flags = spin_lock_irqsave(&em->share_lock);
        listed = m->nshared || m->shared_over || m->writing || m->unlisted;
        spin_unlock_irqrestore(&em->share_lock, flags);
        if (listed) {
            return m;
        }
    }
    return 0;
}

/**
 * Empty the batch and wait out another call's write of `n`'s list, until
 * neither slept. A later writer can fill the batch again; the table
 * buffer's dependency check orders any pointers it publishes. A mount's
 * failure stops the call. A list that keeps
 * being written, or a batch that keeps being filled, past EXT2_TABLE_TRIES
 * sets `*whole`. 0, or -errno.
 */
static int node_write_ready(struct ext2_mount *em, struct ext2_node *n, int *whole)
{
    int tries, rc;

    for (tries = 0; tries < EXT2_TABLE_TRIES; tries++) {
        if (em->mnt->write_error) {
            return em->mnt->write_error;
        }
        if (ext2_batch_pending(em)) {
            rc = ext2_writeback_drain(em);
            if (rc != 0) {
                return rc;
            }
        } else if (!node_not_writing(n)) {
            node_writes_wait(em, n);
        } else {
            return 0;
        }
    }
    *whole = 1;
    return 0;
}

/**
 * Put on the device what the inodes in `n`'s table block depend on, before
 * that block is written, with a hold on each node: its shared and own
 * blocks are written here, or, while another call writes them, waited for.
 *
 * Every round that sleeps starts over until the batch is empty, no
 * neighbour has a list and the cache finds no dependency or wait for
 * `table` (`bcache_sync_block_ready`). Writers can still change it before
 * `bcache_sync_block`, which checks dependencies again with the buffer
 * held until its write ends. A
 * failure on the mount, here or in a call this one waited for, is returned
 * first: the block would publish a pointer to what failed.
 *
 * A neighbour's block is tagged only when it is a regular file's, so
 * another kind of file sets `*whole`. So do a list that overflowed, a list
 * lost with its node or still going to a write of the whole device, whose
 * blocks only such a write of our own finds and waits for (bcache.h), and a
 * table block that keeps being dirtied past EXT2_TABLE_TRIES. Such a write
 * puts the table blocks last and takes no pointer from the batch's images
 * (`ext2_inode_write`), so it needs no round of its own. 0, or -errno.
 */
static int table_write_ready(struct ext2_mount *em, struct ext2_node *n, uint64_t table, int *whole)
{
    int tries, rc;

    for (tries = 0; tries < EXT2_TABLE_TRIES; tries++) {
        struct ext2_node *m;
        uint32_t flags;
        int lost, writing;

        if (em->mnt->write_error) {
            return em->mnt->write_error;
        }
        if (ext2_batch_pending(em)) {
            rc = ext2_writeback_drain(em);
            if (rc != 0) {
                return rc;
            }
            continue;
        }
        flags = spin_lock_irqsave(&em->share_lock);
        lost = em->shared_lost || em->device_lost;
        spin_unlock_irqrestore(&em->share_lock, flags);
        if (lost) {
            break;
        }
        m = table_unsynced(em, n->ino);
        if (!m) {
            rc = bcache_sync_block_ready(em->dev, table);
            if (rc <= 0) {
                return rc;
            }
            continue;
        }
        flags = spin_lock_irqsave(&em->share_lock);
        lost = m->shared_over || m->unlisted;
        writing = m->writing;
        spin_unlock_irqrestore(&em->share_lock, flags);
        if (lost || (m->mode & S_IFMT_) != S_IFREG_) {
            break;
        }
        ext2_node_ref(m, 1);
        rc = 0;
        if (writing) {
            node_writes_wait(em, m);
        } else {
            rc = node_blocks_write(em, m);
        }
        ext2_put(em->mnt, m);
        if (rc != 0) {
            return rc;
        }
    }
    *whole = 1;
    return 0;
}

/**
 * Write what regular file `n` left dirty, with no lock held, and flush the
 * disk's cache: its shared blocks and its own (`node_blocks_write`), then
 * its inode's table block, which `data_only` leaves out unless a field but
 * the times changed (`n->dirty_meta`) or another call's write of that block
 * is under way. A bitmap's or descriptor's bit is so on the device before a
 * pointer to its block, as a strict mount orders them; a bitmap a free
 * dirtied `after` waits for the `first` pointers and inodes marked before
 * it, and a table block for the blocks it names (`bcache_sync_block`).
 *
 * Another call's write of `n`'s list is waited for first, so that this one
 * returns with those blocks on the device (`node_write_ready`). The table
 * block holds other inodes as they are in memory, so `table_write_ready`
 * goes before it: a stop after the call finds no neighbour's pointer to a
 * block that is free or unwritten, and after a failure no table block goes
 * out.
 *
 * A list that overflowed, or went with its node or to a write of the whole
 * device under way, makes it the whole device's `bcache_sync`, as a table
 * block's neighbours may. A failure is recorded as the batch's error and
 * leaves the next call the whole device. The caller holds the big lock
 * for the node table, but writers need not hold it. The lists and dirty
 * marks are taken under `em->share_lock`; a completed inode store sets
 * its mark after dirtying the table buffer. The cache checks dependencies
 * under its write lock and holds each buffer across that check and its
 * write. A writer dirties and lists a block before storing a pointer to
 * it, so a concurrent store either precedes the table write and has its
 * dependencies checked, or waits for the held buffer. 0, or -errno.
 */
static int node_write_out(struct vfs_mount *mnt, struct ext2_node *n, int data_only)
{
    struct ext2_mount *em = ext2_em_of(mnt);
    uint64_t table = ext2_inode_offset(em, n->ino) >> BCACHE_BLOCK_SHIFT;
    int whole = 0, with_table, device, rc;
    uint32_t flags;

    rc = node_write_ready(em, n, &whole);
    flags = spin_lock_irqsave(&em->share_lock);
    whole |= n->shared_over || n->unlisted || em->shared_lost || em->device_lost;
    with_table = !data_only || n->dirty_meta || n->table_writing;
    if (with_table) {
        n->dirty_meta = 0;
        n->table_writing++;
    }
    spin_unlock_irqrestore(&em->share_lock, flags);
    if (rc == 0 && !whole) {
        rc = node_blocks_write(em, n);
    }
    if (rc == 0 && !whole && with_table) {
        rc = table_write_ready(em, n, table, &whole);
    }
    device = rc == 0 && whole;
    if (device) {
        ext2_device_write_begin(em);
        rc = bcache_sync(em->dev);
    } else if (rc == 0 && with_table) {
        rc = bcache_sync_block(em->dev, table);
    }
    if (rc == 0) {
        rc = block_flush(em->dev);
    }
    if (device) {
        ext2_device_write_end(em);
    }
    flags = spin_lock_irqsave(&em->share_lock);
    if (with_table) {
        n->table_writing--;
    }
    if (rc != 0) {
        n->shared_over = 1;
        n->dirty_meta |= with_table;
    }
    spin_unlock_irqrestore(&em->share_lock, flags);
    if (rc != 0) {
        rc = ext2_writeback_error(em, rc);
    }
    return rc;
}

/**
 * The body of `fsync` and `fdatasync`: the batch drained under `em->batch`,
 * then a regular file's blocks written as `node_write_out` says,
 * `data_only` for `fdatasync`. Any other file, a mount that is not
 * writable, and the sync of a whole mount (a NULL `handle`) write the
 * device's dirty buffers instead (`ext2_em_write_out`): a directory's entries
 * are not tagged as its own. A failure is recorded as the batch's error,
 * and the mount's first failure, of this call or any before, is what it
 * returns; 0 when there has been none. The call takes no node lock, as
 * Linux's takes none (`ext2_fsync` and `generic_buffers_fsync`,
 * fs/ext2/file.c:153-163 and fs/buffer.c:595-622 in 6.12.115): a write
 * that races it lands on either side, since a writer dirties a block before
 * it lists it, as Linux's `mark_buffer_dirty_inode` does. A caller without
 * the big lock has it taken here (`ext2_big_enter`) for the node table,
 * which the look at the neighbours in the node's table block walks
 * (`table_unsynced`), and for the mount's error; the waits let it go.
 */
static int file_sync(struct vfs_mount *mnt, void *handle, int data_only)
{
    struct ext2_mount *em = ext2_em_of(mnt);
    struct ext2_node *n = handle;
    int big = ext2_big_enter();
    int rc;

    rc = ext2_writeback_drain(em);
    if (rc != 0) {
        rc = ext2_writeback_error(em, rc);
    } else if (!n || (n->mode & S_IFMT_) != S_IFREG_ || !em->writable ||
               (mnt->flags & VFS_MS_RDONLY)) {
        rc = ext2_em_write_out(mnt);
    } else {
        rc = node_write_out(mnt, n, data_only);
    }
    if (mnt->write_error) {
        rc = mnt->write_error;
    }
    ext2_big_exit(big);
    return rc;
}

/** The `fsync` of `vfs_ops`: `file_sync` of the file and its inode. */
int ext2_fsync(struct vfs_mount *mnt, void *handle)
{
    return file_sync(mnt, handle, 0);
}

/** The `fdatasync` of `vfs_ops`: `file_sync` that leaves out an inode whose
 * times alone changed. */
int ext2_fdatasync(struct vfs_mount *mnt, void *handle)
{
    return file_sync(mnt, handle, 1);
}

/**
 * The `write` of `vfs_ops`: `count` bytes from `buf` at `offset`, through
 * the file-write batch, under the node's lock, which the caller may hold
 * already.
 *
 * ENOSPC is decided first, for the whole write (`ext2_writeback_space`). An
 * overwrite inside the file's blocks or a contiguous append joins the batch
 * a block at a time; any other write drains the batch before it and after
 * each block. The batch drains too when its bytes, images or inodes run out,
 * and a piece that finds its tables full drains it and is tried once more.
 * Every EXT2_SERVICE_BLOCKS pieces the write offers the big lock: the node's
 * lock stays held, `em->batch` is not, no plan is held, and the next piece
 * looks at the user's page again, as it does after a drain that slept. A
 * write that fails after some of its bytes were accepted returns their
 * count; a failure of the batch's I/O stays recorded for `fsync`, after a
 * close and a retry too.
 *
 * A page of `buf` not present at a piece's commit is brought in with the
 * locks let go, and the rest of the write is decided again from there; when
 * the caller holds the file's lock, or another, which this cannot give
 * back, the write ends short at that piece, 0 bytes for the first, and the
 * caller brings the page in (`user_refault`). A write that starts below the
 * volume's file limit and crosses it is clipped at the limit, and one that
 * starts at or past it is -EFBIG, as Linux's `generic_write_check_limits`
 * does with `s_maxbytes`. Returns the bytes accepted, -EISDIR, -EFBIG,
 * -EROFS, -ENOSPC, -EFAULT, or -errno. The caller holds the node's lock.
 */
int32_t ext2_file_write(struct vfs_mount *mnt, void *handle, const void *buf, uint32_t count,
                        uint64_t offset)
{
    struct ext2_mount *em = ext2_em_of(mnt);
    struct ext2_node *n = handle;
    const uint8_t *in = buf;
    uint32_t done = 0, pieces = 0;
    int rc = 0, synchronous, refaults = 0;

    if ((n->mode & S_IFMT_) == S_IFDIR_) {
        return -EISDIR;
    }
    if (count == 0) {
        return 0;
    }
    if (offset >= em->file_limit) {
        return past_limit(em, "write", offset);
    }
    if (count > em->file_limit - offset) {
        count = (uint32_t)(em->file_limit - offset);
    }
    ext2_assert_node_locked(n);
again:
    if (!em->writable || (mnt->flags & VFS_MS_RDONLY)) {
        rc = -EROFS;
        goto out;
    }
    rc = ext2_writeback_space(em, n, offset + done, count - done);
    if (rc != 0) {
        goto out;
    }
    rc = ext2_writeback_contiguous(em, n, offset + done, count - done);
    if (rc < 0) {
        goto out;
    }
    synchronous = !rc;
    if (synchronous) {
        rc = ext2_writeback_drain(em);
        if (rc != 0) {
            goto out;
        }
    }
    while (done < count) {
        uint64_t at = offset + done;
        uint32_t take = 4096u - ((uint32_t)at & 4095u);

        if (take > count - done) {
            take = count - done;
        }
        rc = ext2_writeback_fit(em, n, at, in + done, take);
        if (rc == -EFAULT) {
            rc = ++refaults <= EXT2_REFAULTS ? user_refault(n, in + done, take, 0) : -EFAULT;
            if (rc == 1) {
                goto again;
            }
            goto out;
        }
        if (rc != 0) {
            goto out;
        }
        done += take;
        refaults = 0;
        rc = ext2_writeback_room(em, synchronous);
        if (rc != 0) {
            goto out;
        }
        if (++pieces % EXT2_SERVICE_BLOCKS == 0 && done < count) {
            sched_service_point();
        }
    }
    rc = 0;
out:
    if (rc == -EIO && em->wb && em->wb->count) {
        (void)ext2_writeback_error(em, rc);
    }
    return done ? (int32_t)done : rc;
}

/** `ext2_file_write` under the lock of node `handle`, which the caller may hold
 * already. */
static int32_t ext2_write(struct vfs_mount *mnt, void *handle, const void *buf, uint32_t count,
                          uint64_t offset)
{
    int32_t n;

    ext2_node_lock(handle);
    n = ext2_file_write(mnt, handle, buf, count, offset);
    ext2_node_unlock(handle);
    return n;
}

/**
 * The `fallocate` of `vfs_ops`: ext2 keeps no block reserved ahead of its
 * data, so each hole in the range is given a block by a write of zeros, a
 * block the file has is written only past its size, and without
 * FALLOC_FL_KEEP_SIZE the size grows to the range's end; with it the range
 * stops at the size. A punched hole and any other mode are -EOPNOTSUPP, as
 * Linux's ext2 returns for every mode. The node's lock is held throughout,
 * so that the size and the map do not change between the look at a block
 * and its write. 0, -ENODEV for a file that is not regular, -EFBIG,
 * -ENOMEM, or the errors of `ext2_write`.
 */
int ext2_fallocate(struct vfs_mount *mnt, void *handle, uint32_t mode, uint64_t offset,
                   uint64_t len)
{
    struct ext2_mount *em = ext2_em_of(mnt);
    struct ext2_node *n = handle;
    uint64_t at = offset, end = offset + len;
    uint8_t *zero;
    int rc = 0;

    if (mode & ~0x01u) {
        return -EOPNOTSUPP;
    }
    if ((n->mode & S_IFMT_) != S_IFREG_) {
        return -ENODEV;
    }
    if (end > em->file_limit) {
        return past_limit(em, "fallocate", offset);
    }
    zero = kzalloc(em->block_size);
    if (!zero) {
        return -ENOMEM;
    }
    ext2_node_lock(n);
    if ((mode & 0x01u) && end > n->size) {
        end = n->size;
    }
    while (at < end && rc == 0) {
        uint32_t take = em->block_size - ((uint32_t)at & (em->block_size - 1));
        uint64_t from = at;
        uint32_t b = 0;

        if (take > end - at) {
            take = (uint32_t)(end - at);
        }
        rc = ext2_block_at(em, n, (uint32_t)(at >> em->block_shift), &b);
        if (b != 0 && from < n->size) {
            from = n->size;
        }
        if (rc == 0 && from < at + take) {
            int32_t w = ext2_write(mnt, handle, zero, (uint32_t)(at + take - from), from);

            rc = w < 0 ? (int)w : 0;
        }
        at += take;
    }
    ext2_node_unlock(n);
    kfree(zero);
    return rc;
}

/* The cached-only map walk, in logical block order. A page counts once,
 * after its last allocated block; holes alone count no page. */
struct ext2_cache_walk {
    struct ext2_mount *em;
    struct vfs_cachestat *cs;
    uint32_t first, last; /* inclusive logical block indexes */
    uint32_t perpage;
    uint32_t page;
    uint32_t visits; /* steps since the last scheduling point */
    int any, missing, dirty;
};

/** Account for the page just visited, then clear its block state. */
static void cache_page_end(struct ext2_cache_walk *walk)
{
    if (walk->any && !walk->missing) {
        walk->cs->cache++;
        walk->cs->dirty += walk->dirty != 0;
    }
    walk->any = walk->missing = walk->dirty = 0;
}

/** Count one allocated data block at logical `index`, present in the
 * pending write batch or the buffer cache. No lock is taken and no read
 * or wait is performed: the file's node lock keeps its map stable. */
static void cache_data(struct ext2_cache_walk *walk, uint32_t block, uint32_t index)
{
    struct ext2_mount *em = walk->em;
    uint64_t key = ((uint64_t)block << em->block_shift) >> BCACHE_BLOCK_SHIFT;
    int pending = ext2_writeback_has(em, (uint32_t)key);
    uint32_t page = index / walk->perpage;
    int state = pending ? pending : bcache_state(em->dev, key);

    if (page != walk->page) {
        cache_page_end(walk);
        walk->page = page;
    }
    walk->any = 1;
    walk->missing |= state == 0;
    walk->dirty |= state == 2;
}

/** Read entry `index` of a pointer `block` from the batch or a resident,
 * idle buffer. Return 1 with the pointer in `out`, or 0 for an uncached
 * map block. Never fetch metadata merely to count cached data pages. */
static int cache_pointer(struct ext2_mount *em, uint32_t block, uint32_t index, uint32_t *out)
{
    uint64_t at = ((uint64_t)block << em->block_shift) + index * 4u;
    uint32_t offset = (uint32_t)at & (BCACHE_BLOCK_BYTES - 1u);
    uint8_t raw[4];

    if (!ext2_writeback_copy(em, (uint32_t)(at >> BCACHE_BLOCK_SHIFT), offset, raw, sizeof(raw),
                             0) &&
        !bcache_copy_cached(em->dev, at >> BCACHE_BLOCK_SHIFT, offset, raw, sizeof(raw))) {
        return 0;
    }
    *out = ext2_le32(raw);
    return 1;
}

/** Visit the allocated, resident part of a subtree rooted at `block`.
 * Its logical indexes are [base, base + span), and `level` is 0 for data,
 * up to 3 for a pointer block. The node lock remains held, but no buffer
 * pointer survives the scheduling point every EXT2_SERVICE_BLOCKS steps. */
static void cache_branch(struct ext2_cache_walk *walk, uint32_t block, uint32_t base, uint32_t span,
                         int level)
{
    uint32_t first, last, sub, i;

    if (++walk->visits == EXT2_SERVICE_BLOCKS) {
        sched_service_point();
        walk->visits = 0;
    }
    if (!block || !ext2_data_block_ok(walk->em, block) || base > walk->last ||
        base + span - 1u < walk->first) {
        return;
    }
    if (level == 0) {
        cache_data(walk, block, base);
        return;
    }
    sub = span / (walk->em->block_size / 4u);
    first = walk->first > base ? (walk->first - base) / sub : 0;
    last = walk->last < base + span - 1u ? (walk->last - base) / sub : span / sub - 1u;
    for (i = first; i <= last; i++) {
        uint32_t child;

        if (!cache_pointer(walk->em, block, i, &child)) {
            /* The rest of the page under way, if any, is under this entry. */
            if ((base + i * sub) / walk->perpage == walk->page) {
                walk->missing = 1;
            }
            return;
        }
        cache_branch(walk, child, base + i * sub, sub, level - 1);
    }
}

/** Count file pages whose allocated blocks are in the batch or cache.
 * Walk only allocated branches with resident metadata, under the node's
 * lock. A sparse hole costs one step per absent branch, and the call
 * performs no disk I/O; long walks let other kernel work run. */
void ext2_cachestat(struct vfs_mount *mnt, void *node, uint64_t first, uint64_t last,
                    struct vfs_cachestat *cs)
{
    struct ext2_node *n = node;
    struct ext2_cache_walk walk;
    uint32_t base, span, i;

    ext2_node_lock(n);
    if (n->size == 0 || first > (n->size - 1u) >> 12) {
        ext2_node_unlock(n);
        return;
    }
    if (last > (n->size - 1u) >> 12) {
        last = (n->size - 1u) >> 12;
    }
    k_memset(&walk, 0, sizeof(walk));
    walk.em = ext2_em_of(mnt);
    walk.cs = cs;
    walk.perpage = 4096u / walk.em->block_size;
    walk.first = (uint32_t)(first * walk.perpage);
    walk.last = (uint32_t)((last + 1u) * walk.perpage - 1u);
    if (walk.last > (n->size - 1u) >> walk.em->block_shift) {
        walk.last = (uint32_t)((n->size - 1u) >> walk.em->block_shift);
    }
    for (i = 0; i < EXT2_NDIR; i++) {
        cache_branch(&walk, n->block[i], i, 1, 0);
    }
    base = EXT2_NDIR;
    span = 1;
    for (i = 1; i <= 3; i++) {
        span *= walk.em->block_size / 4u;
        cache_branch(&walk, n->block[EXT2_NDIR + i - 1u], base, span, (int)i);
        base += span;
    }
    cache_page_end(&walk);
    ext2_node_unlock(n);
}
