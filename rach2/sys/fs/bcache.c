/*
 * bcache.c: the buffer cache. The contract is in bcache.h; what is here is
 * the bookkeeping: a list of buffers from the heap keyed by device and
 * block, made as they are needed up to the cache's limit and given back
 * under memory pressure, least-recently-released replacement, and the two
 * hold rules checked on every acquisition.
 */

#include "bcache.h"
#include "../lib/kstring.h"

#include "vfs.h"

#include "../klog.h"
#include "../lib/kerrno.h"
#include "../mm/kheap.h"
#include "../mm/paging.h"
#include "../mm/pressure.h"
#include "../panic.h"
#include "../proc/process.h"

/* Dirty buffers linked both ways, the latest to go dirty at the head. */
struct dirty_list {
    struct bbuf *head;
    struct bbuf *tail;
};

static struct bbuf *bufs;     /* every buffer, newest first, linked both ways */
static uint32_t nbufs;        /* how many */
static struct bbuf *lru_head; /* the keyed buffers, least recently released first */
static struct bbuf *lru_tail;
static struct bbuf *empties;      /* the buffers keyed to nothing */
static struct dirty_list dirties; /* the dirty buffers but the firsts */
static struct dirty_list firsts;  /* the dirty buffers of the first class */
static uint32_t ndirty;           /* how many */
static uint64_t marks;            /* counts the dirty marks, for `dirtied` and `touched` */
static struct bbuf *kernel_held;  /* held with no process current */
static block_device_t *ahead_dev; /* the device and block a sequential reader */
static uint64_t ahead_next;       /* reaches next; a miss there fills a run */
static uint32_t write_gen;        /* counts the writes that reached a device */
static uint32_t limit_set;        /* a limit given by `bcache_set_limit`, or 0 */
static uint32_t clock_stamp;
static uint64_t stat_hits, stat_fills, stat_waits;
/* Every buffer a walk of the cache's lists reads: the whole list, the release
 * order, the empties, a bucket of the index and a process's holds; and each
 * step a sync takes to put a block in its batch. It is what a get, a victim's
 * choice and a sync cost in bookkeeping, and a host test holds it under a
 * bound. */
static uint64_t stat_visits;

/** The most buffers the cache makes: half of what the caches together may
 * hold under pressure, and never fewer than the floor, since a file system
 * holds up to four at once and a write of one may take one more. Each
 * buffer is a page of the kernel heap, whose region is an eighth of memory
 * and never less than 256 MiB (kheap.h), so the cache also stops at half
 * the region's pages. The memory share, an eighth of memory too, would take
 * the whole region, and on i686 with more than 2 GiB more than it, leaving
 * fork with no page for a stack. */
static uint32_t bcache_limit(void)
{
    size_t share = limit_set ? limit_set : mem_cache_target() / 2u;
    uint32_t most = kheap_region_pages() / 2u;
    uint32_t n = share > most ? most : (uint32_t)share;

    return n < BCACHE_MIN_BUFFERS ? BCACHE_MIN_BUFFERS : n;
}

void bcache_set_limit(uint32_t n)
{
    limit_set = n;
}

uint32_t bcache_resident(void)
{
    return nbufs;
}

uint32_t bcache_capacity(void)
{
    return bcache_limit();
}

int bcache_dirty_heavy(void)
{
    return ndirty >= bcache_limit() / 4u;
}

uint64_t bcache_stat_hits(void)
{
    return stat_hits;
}
uint64_t bcache_stat_fills(void)
{
    return stat_fills;
}
uint64_t bcache_stat_waits(void)
{
    return stat_waits;
}
uint64_t bcache_stat_visits(void)
{
    return stat_visits;
}

static void index_fit(void);

/** A buffer from the heap, empty and unheld, on the list; NULL when the
 * heap has no room for it. */
static struct bbuf *buffer_new(void)
{
    struct bbuf *b = kzalloc(sizeof(*b));

    if (!b) {
        return 0;
    }
    b->data = kalloc_cache_page();
    if (!b->data) {
        kfree(b);
        return 0;
    }
    b->holder = -1;
    wait_queue_init(&b->wq);
    b->prev = 0;
    b->next = bufs;
    if (bufs) {
        bufs->prev = b;
    }
    bufs = b;
    nbufs++;
    index_fit();
    return b;
}

/*
 * The index by device and block: a lookup walks one bucket and not the
 * whole list, which runs to hundreds of thousands of buffers on a machine
 * with memory. A buffer is in the index from the moment it is keyed to a
 * block until it is emptied.
 *
 * The index has a bucket for each buffer the cache's limit allows, so that
 * a bucket holds one buffer on average when the cache is full. It starts
 * with the BHASH_FIRST buckets of `bhash_first`, which need no heap at
 * boot, and when the buffers outnumber the buckets `index_fit` moves it to
 * a table from the heap sized from the limit. A heap that has no run for
 * the table leaves the index as it is until the buffers have doubled.
 */
#define BHASH_FIRST       4096u
#define BHASH_FIRST_SHIFT 12
/* The most buckets: the limit of a machine with 64 GiB of memory. */
#define BHASH_MOST        (1u << 22)
static struct bbuf *bhash_first[BHASH_FIRST];
static struct bbuf **bhash = bhash_first;
static uint32_t bhash_size = BHASH_FIRST; /* a power of two */
static uint32_t bhash_shift = BHASH_FIRST_SHIFT;
static uint32_t bhash_retry; /* buffers below which a refused table is not asked again */

uint32_t bcache_buckets(void)
{
    return bhash_size;
}

/** The bucket of the index for block `blk` of `dev`. The block's number,
 * its high bits folded onto the low, gives consecutive blocks consecutive
 * buckets, which no two of them share; the device moves its blocks to a
 * place of their own, the top bits of its address's product with the golden
 * ratio's fraction (Knuth's multiplicative hashing), which every bit of the
 * address reaches, so that devices whose records lie a power of two apart
 * are not put on the same buckets. */
static uint32_t bucket_of(const block_device_t *dev, uint64_t blk)
{
    uint32_t lo = (uint32_t)blk;
    uint32_t at = lo ^ (lo >> bhash_shift) ^ (uint32_t)(blk >> 32);
    uint32_t place = ((uint32_t)((uintptr_t)dev >> 4) * 0x9E3779B1u) >> (32 - bhash_shift);

    return (at + place) & (bhash_size - 1u);
}

/** Give the index a bucket for each buffer the limit allows, once the
 * buffers outnumber its buckets: a table from the heap, every keyed buffer
 * moved to it, and the old table given back unless it is the first. */
static void index_fit(void)
{
    uint32_t most = bcache_limit(), size = bhash_size, shift = bhash_shift;
    uint32_t pages, old_pages = 0;
    struct bbuf **table, **old = bhash;
    struct bbuf *b;

    if (nbufs <= bhash_size || nbufs < bhash_retry) {
        return;
    }
    if (most < nbufs) {
        most = nbufs;
    }
    while (size < most && size < BHASH_MOST) {
        size <<= 1;
        shift++;
    }
    if (size == bhash_size) {
        return;
    }
    pages = (uint32_t)((size * sizeof(*table) + PAGE_SIZE - 1) / PAGE_SIZE);
    table = kalloc_pages_try(pages, 0);
    if (!table) {
        bhash_retry = nbufs * 2u;
        return;
    }
    k_memset(table, 0, (size_t)pages * PAGE_SIZE);
    if (old != bhash_first) {
        old_pages = (uint32_t)((bhash_size * sizeof(*table) + PAGE_SIZE - 1) / PAGE_SIZE);
    }
    bhash = table;
    bhash_size = size;
    bhash_shift = shift;
    for (b = bufs; b; b = b->next) {
        stat_visits++;
        if (b->dev) {
            uint32_t h = bucket_of(b->dev, b->blk);

            b->hnext = bhash[h];
            bhash[h] = b;
        }
    }
    if (old_pages) {
        kfree_pages(old, old_pages);
    }
}

/** Put the keyed buffer `b` in its bucket of the index. */
static void index_add(struct bbuf *b)
{
    uint32_t h = bucket_of(b->dev, b->blk);

    b->hnext = bhash[h];
    bhash[h] = b;
}

/** Take `b` out of the index before its device is cleared; an empty
 * buffer is in no bucket. */
static void index_del(struct bbuf *b)
{
    struct bbuf **link;

    if (!b->dev) {
        return;
    }
    for (link = &bhash[bucket_of(b->dev, b->blk)]; *link; link = &(*link)->hnext) {
        stat_visits++;
        if (*link == b) {
            *link = b->hnext;
            b->hnext = 0;
            return;
        }
    }
}

/** The buffer keyed to `blk` of `dev`, or NULL. */
static struct bbuf *index_find(const block_device_t *dev, uint64_t blk)
{
    struct bbuf *b;

    for (b = bhash[bucket_of(dev, blk)]; b; b = b->hnext) {
        stat_visits++;
        if (b->dev == dev && b->blk == blk) {
            return b;
        }
    }
    return 0;
}

/*
 * The release order and the empties.
 *
 * A keyed buffer is on the release-order list from its keying until it is
 * emptied, moved to the tail at each release, so the least recently
 * released buffer is at the head and a victim search reads from there
 * instead of comparing every buffer's stamp. A buffer keyed to nothing is
 * on the empties list and is the first choice for a fill.
 */

/** Take `b` off the release order; a buffer not on it is left alone. */
static void lru_del(struct bbuf *b)
{
    if (b->lru_prev) {
        b->lru_prev->lru_next = b->lru_next;
    } else if (lru_head == b) {
        lru_head = b->lru_next;
    } else {
        return;
    }
    if (b->lru_next) {
        b->lru_next->lru_prev = b->lru_prev;
    } else {
        lru_tail = b->lru_prev;
    }
    b->lru_prev = 0;
    b->lru_next = 0;
}

/** Put `b` at the tail of the release order, the most recently released. */
static void lru_add_tail(struct bbuf *b)
{
    b->lru_prev = lru_tail;
    b->lru_next = 0;
    if (lru_tail) {
        lru_tail->lru_next = b;
    } else {
        lru_head = b;
    }
    lru_tail = b;
}

/*
 * The dirty lists. A buffer is on one while it is dirty and at no other
 * time, so a sync and the ordered writes read the buffers waiting to be
 * written and not the whole cache, which on a machine with memory is
 * hundreds of thousands of buffers. Each list runs from the latest
 * `dirtied` to the earliest, so a walk for the blocks dirty since before a
 * mark starts at the tail. Those of the first class have a list of their
 * own: they are few, and a look for them comes before every write of an
 * `after` and for every dirty victim, with the big lock held, while a
 * writer can have the rest of the cache dirty.
 */

/** The dirty list `b` belongs on, by its class. */
static struct dirty_list *dirty_list_of(const struct bbuf *b)
{
    return b->order == BCACHE_ORDER_FIRST ? &firsts : &dirties;
}

/** Put the dirty `b` on its list, once, in its place by `dirtied`: at the
 * head for a buffer that has just gone dirty. */
static void dirty_link(struct bbuf *b)
{
    struct dirty_list *list = dirty_list_of(b);
    struct bbuf *prev = 0, *next = list->head;

    while (next && next->dirtied > b->dirtied) {
        stat_visits++;
        prev = next;
        next = next->dirty_next;
    }
    b->dirty_prev = prev;
    b->dirty_next = next;
    if (prev) {
        prev->dirty_next = b;
    } else {
        list->head = b;
    }
    if (next) {
        next->dirty_prev = b;
    } else {
        list->tail = b;
    }
}

/** Take `b` off its dirty list. */
static void dirty_unlink(struct bbuf *b)
{
    struct dirty_list *list = dirty_list_of(b);

    if (b->dirty_prev) {
        b->dirty_prev->dirty_next = b->dirty_next;
    } else {
        list->head = b->dirty_next;
    }
    if (b->dirty_next) {
        b->dirty_next->dirty_prev = b->dirty_prev;
    } else {
        list->tail = b->dirty_prev;
    }
    b->dirty_prev = 0;
    b->dirty_next = 0;
}

/** Mark `b` dirty, stamped with the next count of marks, and put it on its
 * dirty list. */
static void set_dirty(struct bbuf *b)
{
    b->touched = ++marks;
    if (b->dirty) {
        return;
    }
    b->dirty = 1;
    b->dirtied = b->touched;
    ndirty++;
    dirty_link(b);
}

/** Mark `b` clean and take it off its dirty list. */
static void set_clean(struct bbuf *b)
{
    if (!b->dirty) {
        return;
    }
    b->dirty = 0;
    b->owner = 0;
    b->level = 0;
    b->dirtied = 0;
    ndirty--;
    dirty_unlink(b);
}

/** Take `b` off the index and the release order, keyed to nothing, and put
 * it on the empties. */
static void make_empty(struct bbuf *b)
{
    index_del(b);
    lru_del(b);
    b->dev = 0;
    set_clean(b);
    b->discard = 0;
    b->order = 0;
    b->empty_next = empties;
    empties = b;
}

/** An empty buffer off the empties, or NULL when there is none. */
static struct bbuf *take_empty(void)
{
    struct bbuf *b = empties;

    if (b) {
        empties = b->empty_next;
        b->empty_next = 0;
    }
    return b;
}

/** An unheld buffer off the cache's lists and back to the heap: a clean
 * keyed one, or an empty one the caller has taken off the empties. */
static void buffer_free(struct bbuf *b)
{
    stat_visits++;
    if (b->prev) {
        b->prev->next = b->next;
    } else {
        bufs = b->next;
    }
    if (b->next) {
        b->next->prev = b->prev;
    }
    nbufs--;
    index_del(b);
    lru_del(b);
    kfree_pages(b->data, 1);
    kfree(b);
}

/** The pid that holds a buffer for the caller: the current process, or 0
 * with no process current. */
static int caller_pid(void)
{
    return current_proc ? (int)current_proc->pid : 0;
}

/*
 * The holds.
 *
 * The buffers a process holds are on a list of its own, so the hold rules,
 * checked at every acquisition, read the few it holds and not every buffer
 * in the cache, which runs to thousands on a machine with memory.
 */

/** The head of the caller's list of held buffers. */
static struct bbuf **held_list(void)
{
    if (current_proc) {
        return &current_proc->bcache_held;
    }
    return &kernel_held;
}

/** Mark `b` held by the caller and put it on the caller's list. */
static void hold(struct bbuf *b)
{
    struct bbuf **head = held_list();

    b->holder = caller_pid();
    b->hold_next = *head;
    *head = b;
}

/** Take `b` off the caller's list and mark it unheld. */
static void unhold(struct bbuf *b)
{
    struct bbuf **link;

    for (link = held_list(); *link; link = &(*link)->hold_next) {
        stat_visits++;
        if (*link == b) {
            *link = b->hold_next;
            break;
        }
    }
    b->hold_next = 0;
    b->holder = -1;
}

/** Panic when the caller, taking block `blk` of `dev`, would break a rule
 * that keeps holders out of deadlock; called before a wait or a fill. A
 * violation is a driver defect, not a runtime condition.
 *
 * Blocks of one device are taken in order, so two processes holding one each
 * cannot each be waiting for the other's. A second device is allowed only
 * downward: a device that is under everything the caller holds, which is what
 * a loop device's read does when it reads the file it is made of. Downward is
 * the only direction there is, since the chain of backing devices has no
 * cycle in it, so the same argument holds across devices as within one. And
 * no caller holds more than `BCACHE_HOLD_MAX` buffers at once.
 */
static void check_hold_rules(block_device_t *dev, uint64_t blk)
{
    int held = 0;
    const struct bbuf *b;

    for (b = *held_list(); b; b = b->hold_next) {
        stat_visits++;
        if (!b->dev) {
            continue;
        }
        held++;
        if (b->dev != dev) {
            if (!block_is_below(dev, b->dev)) {
                k_panic("bcache: hold rule: a block on a device that is not under the ones held");
            }
            continue;
        }
        if (b->blk >= blk) {
            k_panic("bcache: hold rule: a block taken that is not above every held block");
        }
    }
    if (held >= BCACHE_HOLD_MAX) {
        k_panic("bcache: hold rule: a fifth buffer taken");
    }
}

/**
 * Whether a dirty buffer of `victim` may be written while this caller holds
 * what it holds, on the way to a buffer of `filling`.
 *
 * Writing a buffer re-enters its device, and a device with a backing reaches
 * this cache again to do it. That is allowed only where the hold rules allow
 * it: the backing must be under everything held, the victim included. A
 * victim that fails this is passed over, and another buffer takes the
 * eviction, which is why this answers instead of stopping the machine.
 *
 * The caller needs room for two more holds, the victim, held for the write,
 * and the block the write takes on the device under it; with less, another
 * victim is chosen rather than the fifth buffer stopping the machine. And
 * the locks, which this cache cannot see: writing such a buffer goes through
 * the file system holding its file and takes that mount's lock, so a caller
 * that already holds a mount's lock could wait for a lock another caller
 * holds while that one waits for this one. Its own chain is what it may
 * write: a volume on a loop device evicting the device's own buffers is the
 * ordinary case, and the ordering rules cover it.
 */
static int victim_may_be_written(block_device_t *victim, block_device_t *filling)
{
    int held = 0;
    const struct bbuf *b;

    if (!victim || !victim->backing) {
        return 1;
    }
    for (b = *held_list(); b; b = b->hold_next) {
        stat_visits++;
        if (!b->dev) {
            continue;
        }
        held++;
        if (b->dev != victim && !block_is_below(victim->backing, b->dev)) {
            return 0;
        }
    }
    if (held + 2 > BCACHE_HOLD_MAX) {
        return 0;
    }
    if (victim == filling) {
        return 1;
    }
    return !vfs_ns_held_by_caller();
}

/** The wait condition for a busy buffer: nobody holds it. */
static int busy_gone(void *arg)
{
    struct bbuf *b = arg;
    return b->holder == -1;
}

/** Sleep until nobody holds `b`, counted among its waiters until this runs
 * again, so that reclaim leaves it. */
static void wait_unheld(struct bbuf *b)
{
    b->waiters++;
    wq_wait_event_nointr(&b->wq, busy_gone, b);
    b->waiters--;
}

/* What a sync writes and how. */
struct sync_scope {
    uint64_t owner;       /* the owner whose blocks it writes, or 0 for all */
    uint64_t since;       /* the last `dirtied` it owes, or 0 for any */
    uint32_t flags;       /* the request flags of a run */
    uint32_t first_flags; /* those of a run of `first` buffers */
    int hold;             /* it leaves the device's last blocks for a later sync */
    int hold_afters;      /* and its `after` blocks, which a `first` left must precede */
    int reclaim;          /* it leaves what a reclaim may not write (`write_barred`) */
};

/** Whether `b` is a block its device writes last (`bcache_sync_last`). */
static int block_last(const struct bbuf *b)
{
    return b->dev->sync_last && b->dev->sync_last(b->dev->sync_last_arg, b->blk);
}

/** Whether the block `t` of the class `block_last` names may name blocks
 * tagged `owner` (`bcache_sync_last`). */
static int last_names(const struct bbuf *t, uint64_t owner)
{
    block_device_t *dev = t->dev;

    return !dev->sync_names || dev->sync_names(dev->sync_last_arg, t->blk, owner);
}

/** Whether `b` names other blocks, which reach the device before it: a
 * block its device writes last, or a pointer block (`bcache_dirty_pointer`). */
static int names_blocks(const struct bbuf *b)
{
    return b->level > 0 || block_last(b);
}

/** Whether `b` waits for other dirty blocks before its write: an `after`,
 * or a block that names others. */
static int follows(const struct bbuf *b)
{
    return b->order == BCACHE_ORDER_AFTER || names_blocks(b);
}

/**
 * Whether the dirty block `d` must reach the device before `b`, which
 * `follows`, with `last` saying whether `b` is a block written last. Only a
 * block dirty since before `b`'s last mark can be what that mark wrote into
 * `b`: a `first` before an `after`; before a block written last any block
 * it may name, one not of its class nor an `after`, shared or of an owner
 * the block names; and before a pointer block that is no `after` a block of
 * its owner below its level, of neither class.
 *
 * Each rule leads to a higher rank: a pointer block's level, then the class
 * written last, then the `after` class. So a chain of waits has at most
 * six blocks and never comes back to one.
 */
static int must_precede(const struct bbuf *d, const struct bbuf *b, int last)
{
    if (d == b || d->dev != b->dev || d->dirtied > b->touched) {
        return 0;
    }
    if (b->order == BCACHE_ORDER_AFTER && d->order == BCACHE_ORDER_FIRST) {
        return 1;
    }
    if (d->order == BCACHE_ORDER_AFTER || block_last(d)) {
        return 0;
    }
    if (last) {
        return d->owner == 0 || last_names(b, d->owner);
    }
    return b->order != BCACHE_ORDER_AFTER && d->owner == b->owner && d->level < b->level;
}

/** The earliest dirtied block of `list` that must reach the device before
 * the dirty block `b` (`must_precede`), or NULL. The walk reads the list
 * from its tail as far as `b`'s last mark. */
static struct bbuf *earliest_before(const struct dirty_list *list, const struct bbuf *b, int last)
{
    struct bbuf *d;

    for (d = list->tail; d && d->dirtied <= b->touched; d = d->dirty_prev) {
        stat_visits++;
        if (must_precede(d, b, last)) {
            return d;
        }
    }
    return 0;
}

/** The earliest dirtied block that must reach the device before the dirty
 * block `b` (`must_precede`), or NULL. An `after` alone waits only for the
 * first class; a block that names others reads the other list too. */
static struct bbuf *first_before(const struct bbuf *b)
{
    struct bbuf *d, *e;
    int last;

    if (!b->dirty || !follows(b)) {
        return 0;
    }
    last = block_last(b);
    d = earliest_before(&firsts, b, last);
    e = names_blocks(b) ? earliest_before(&dirties, b, last) : 0;
    return !d || (e && e->dirtied < d->dirtied) ? e : d;
}

/** The block to write next on the way to `b`: what `b` waits for, or what
 * that waits for in turn, or NULL when `b` may be written now. */
static struct bbuf *way_to(const struct bbuf *b)
{
    struct bbuf *d = first_before(b), *e;

    while (d && (e = first_before(d)) != 0) {
        d = e;
    }
    return d;
}

/** Whether a write of `b` is kept from an eviction, a shrink or a reserve:
 * its own last write failed, or a write of its device did and it is a block
 * that names others, which may name the block that failed. */
static int write_barred(const struct bbuf *b)
{
    return b->write_failed || ((b->dev->flags & BLOCK_WRITE_FAILED) && names_blocks(b));
}

/** Whether a write of `scope` leaves `b` for a later one: the reclaim's
 * barred blocks, and the blocks a sync's first round holds back. */
static int sync_leaves(const struct bbuf *b, const struct sync_scope *scope)
{
    if (scope->reclaim && write_barred(b)) {
        return 1;
    }
    return scope->hold && (block_last(b) || (scope->hold_afters && b->order == BCACHE_ORDER_AFTER));
}

/** Write the buffer `b` to the device with the request `flags`. A failure
 * is recorded against the device, through `vfs_device_failed`, and leaves
 * the buffer dirty and marked failed. */
static int write_buffer_flags(struct bbuf *b, uint32_t flags)
{
    int rc = block_request_flags(b->dev, 1, b->blk * BCACHE_BLOCK_SECTORS,
                                 b->bytes / BLOCK_SECTOR_SIZE, b->data, flags);

    if (rc != 0) {
        klog(KLOG_WARN, "bcache", "write: sector %d failed (%d), kept dirty",
             (int)(b->blk * BCACHE_BLOCK_SECTORS), rc);
        b->dev->flags |= BLOCK_WRITE_FAILED;
        b->write_failed = 1;
        vfs_device_failed(b->dev, b->blk * BCACHE_BLOCK_SECTORS, rc);
        return rc;
    }
    set_clean(b);
    b->write_failed = 0;
    b->order = 0;
    write_gen++;
    return 0;
}

/** The request flags of a write that leaves `dev`'s own cache to a later
 * flush; 0 on a disk that has no such write. */
static uint32_t unflushed_flags(block_device_t *dev)
{
    return (block_flags(dev) & BLOCK_WRITE_THROUGH) ? BLOCK_REQ_NOFLUSH : 0;
}

/** Put what a write-through disk under `dev` holds in its cache on the
 * medium, before a block that must follow what went before it; a disk with
 * nothing unflushed answers at once. A failure is recorded as a failed
 * write's. 0, or -errno. */
static int order_flush(block_device_t *dev)
{
    int rc;

    if (!unflushed_flags(dev)) {
        return 0;
    }
    rc = block_flush(dev);
    if (rc != 0) {
        klog(KLOG_WARN, "bcache", "write: flush failed (%d)", rc);
        dev->flags |= BLOCK_WRITE_FAILED;
        vfs_device_failed(dev, 0, rc);
    }
    return rc;
}

/** Let go of `b`, held by the caller, and wake whoever waits for it. */
static void let_go(struct bbuf *b)
{
    unhold(b);
    wq_wake_all(&b->wq);
}

static int write_one(struct bbuf *b, uint32_t flags);
static int write_run(struct bbuf *first, uint64_t *last, uint32_t flags,
                     const struct sync_scope *scope);

/**
 * Write the unheld dirty buffer `b`, which `follows`, after what it waits
 * for, with the request `flags`.
 *
 * `b` is held from the first look until its write ends, so that nothing is
 * stored in it meanwhile and what it waits for cannot grow. The blocks are
 * written the earliest dirtied first (`way_to`), each in a run of its
 * neighbours when it waits for nothing itself; then, on a
 * write-through disk, the flush; then `b`. One of them held by another
 * process lets `b` go for the wait, since that holder may be waiting for
 * `b`, and the caller looks again. At most two buffers are held at once:
 * `b`, and the `first` an `after` waits for while it is written.
 *
 * @return 0 with `b` written; 1 after a wait, with `b` unwritten; or the
 *         -errno of a failed write or flush, which leaves `b` dirty, or
 *         -EIO for a block the caller holds itself.
 */
static int write_follower(struct bbuf *b, uint32_t flags)
{
    struct sync_scope any;
    struct bbuf *d;
    uint64_t end;
    int rc = 0;

    k_memset(&any, 0, sizeof(any));

    hold(b);
    while (rc == 0 && (d = way_to(b)) != 0) {
        if (d->holder == caller_pid()) {
            rc = -EIO;
        } else if (d->holder != -1) {
            let_go(b);
            wait_unheld(d);
            return 1;
        } else if (follows(d)) {
            rc = write_one(d, 0);
        } else {
            rc = write_run(d, &end, d->order == BCACHE_ORDER_FIRST ? 0 : unflushed_flags(b->dev),
                           &any);
        }
    }
    if (rc == 0) {
        rc = order_flush(b->dev);
    }
    if (rc == 0) {
        rc = write_buffer_flags(b, flags);
    }
    let_go(b);
    return rc;
}

/** Write the unheld buffer `b` alone, held across the write, with the
 * request `flags`, and wake whoever waits for it; one that `follows` goes
 * after what it waits for (`write_follower`), which may answer 1. */
static int write_one(struct bbuf *b, uint32_t flags)
{
    int rc;

    if (follows(b)) {
        return write_follower(b, flags);
    }
    hold(b);
    rc = write_buffer_flags(b, flags);
    let_go(b);
    return rc;
}

/* The staging buffer a run of blocks is written or read through, the most
 * one request moves; taken from the heap at the first run. One run at a time
 * uses it, since the request it hands the bytes to sleeps, and a second run
 * would overwrite them in that window; a writer that finds it in use writes
 * its blocks one at a time, and a read fills its block alone. */
static uint8_t *run_stage;
static int run_stage_busy;
/* The block a run is being filled for: not a victim while the run's later
 * blocks are keyed, since the caller has let it go to key them and would
 * otherwise find it gone. */
static struct bbuf *fill_keep;
#define RUN_BLOCKS (BLOCK_REQ_MAX_SECTORS / BCACHE_BLOCK_SECTORS)

/**
 * Gather in `run` the blocks a run from `first` writes (`write_run`):
 * `first`, then the dirty, unheld blocks after it that `scope` takes, of its
 * place in the order and its kind, as far as one request moves and half the
 * cache, while the staging buffer is free. Returns how many, at least 1.
 */
static int run_gather(struct bbuf *first, struct bbuf **run, const struct sync_scope *scope)
{
    block_device_t *dev = first->dev;
    uint32_t most = block_max_sectors(dev) / BCACHE_BLOCK_SECTORS;
    int n = 1;

    run[0] = first;
    if (most > RUN_BLOCKS) {
        most = RUN_BLOCKS;
    }
    if (most > bcache_limit() / 2u) {
        most = bcache_limit() / 2u;
    }
    if (!run_stage && most > 1 && !run_stage_busy) {
        run_stage = kalloc_pages_try(RUN_BLOCKS * BCACHE_BLOCK_BYTES / PAGE_SIZE, 0);
    }
    if (run_stage_busy || (follows(first) && way_to(first))) {
        most = 1;
    }
    while ((uint32_t)n < most && run_stage && run[n - 1]->bytes == BCACHE_BLOCK_BYTES) {
        struct bbuf *b = index_find(dev, first->blk + (uint64_t)n);

        if (!b || !b->dirty || b->holder != -1 || b->order != first->order ||
            (scope->owner && b->owner != scope->owner) || sync_leaves(b, scope) ||
            follows(b) != follows(first) || (follows(b) && way_to(b))) {
            break;
        }
        run[n++] = b;
    }
    return n;
}

/**
 * Write `first` and the dirty, unheld blocks that follow it on its device
 * in one request, with the request `flags`, so that a file written back
 * goes to the disk in runs and not a page at a time. `*last` receives the
 * last block written. A run stops at a block `scope` leaves out: of another
 * owner, so that one file's sync takes no neighbour's blocks with it, one
 * its device wants written last, or one a reclaim may not write.
 *
 * A run of blocks that wait for others (`follows`) holds only those whose
 * way is clear, which `first`'s is, and no block of the other kind, since
 * one request reaches the device as a whole; it is held while a
 * write-through disk is flushed before it. A `first` that waits is written
 * after what it waits for and alone (`write_one`), which may answer 1.
 *
 * A run is as long as the device takes at once and at most half the cache,
 * as a read run is: its buffers are held across the request's sleep and let
 * go together, and a run that held every buffer would leave a miss
 * meanwhile with no buffer to take. Every block of a run has the place in
 * the order that `first` has, since a `first` and an `after` in one request
 * would reach the device together. A failure is recorded on `first` as
 * `write_buffer_flags` does. A run of one, or a device that takes a page,
 * is `write_buffer_flags`.
 */
static int write_run(struct bbuf *first, uint64_t *last, uint32_t flags,
                     const struct sync_scope *scope)
{
    struct bbuf *run[RUN_BLOCKS];
    block_device_t *dev = first->dev;
    uint32_t bytes = 0;
    int n, i, rc;

    n = run_gather(first, run, scope);
    *last = first->blk;
    if (n == 1) {
        return write_one(first, flags);
    }
    run_stage_busy = 1;
    for (i = 0; i < n; i++) {
        hold(run[i]);
    }
    rc = follows(first) ? order_flush(dev) : 0;
    if (rc != 0) {
        run_stage_busy = 0;
        for (i = 0; i < n; i++) {
            let_go(run[i]);
        }
        return rc;
    }
    for (i = 0; i < n; i++) {
        k_memcpy(run_stage + bytes, run[i]->data, run[i]->bytes);
        bytes += run[i]->bytes;
    }
    rc = block_request_flags(dev, 1, first->blk * BCACHE_BLOCK_SECTORS, bytes / BLOCK_SECTOR_SIZE,
                             run_stage, flags);
    run_stage_busy = 0;
    if (rc != 0) {
        klog(KLOG_WARN, "bcache", "write: sector %d failed (%d), %d blocks kept dirty",
             (int)(first->blk * BCACHE_BLOCK_SECTORS), rc, n);
        dev->flags |= BLOCK_WRITE_FAILED;
        first->write_failed = 1;
        vfs_device_failed(dev, first->blk * BCACHE_BLOCK_SECTORS, rc);
    }
    for (i = 0; i < n; i++) {
        if (rc == 0) {
            set_clean(run[i]);
            run[i]->write_failed = 0;
            run[i]->order = 0;
        }
        let_go(run[i]);
    }
    if (rc == 0) {
        write_gen++;
    }
    *last = run[n - 1]->blk;
    return rc;
}

/** The valid byte count of block `blk`: whole, or the device's short tail. */
static uint32_t block_bytes(block_device_t *dev, uint64_t blk)
{
    uint64_t first = blk * BCACHE_BLOCK_SECTORS;
    uint64_t left = dev->sector_count - first;

    if (left >= BCACHE_BLOCK_SECTORS) {
        return BCACHE_BLOCK_BYTES;
    }
    return (uint32_t)left * BLOCK_SECTOR_SIZE;
}

static struct bbuf *get(block_device_t *dev, uint64_t blk, int blank, int *fresh);
static int fill_run(block_device_t *dev, uint64_t blk);
static int firsts_pending(const block_device_t *dev);

/* The answer of `firsts_pending` for the device last asked about, kept for
 * one walk of the release order, since it cannot change before the walk
 * sleeps. */
struct first_ask {
    const block_device_t *dev;
    int pending;
};

/**
 * Whether a miss for a block of `filling` may take `b`. Not one that is held
 * or kept for a fill, nor a dirty one whose last write failed, whose write
 * the caller may not make (`victim_may_be_written`), a block that names
 * others and waits for one (`way_to`), or an `after` whose device has a
 * `first` that cannot be written now.
 */
static int victim_fit(const struct bbuf *b, block_device_t *filling, struct first_ask *ask)
{
    if (b->holder != -1 || b == fill_keep) {
        return 0;
    }
    if (!b->dirty) {
        return 1;
    }
    if (write_barred(b) || !victim_may_be_written(b->dev, filling)) {
        return 0;
    }
    if (names_blocks(b) && way_to(b)) {
        return 0;
    }
    if (b->order != BCACHE_ORDER_AFTER) {
        return 1;
    }
    if (b->dev != ask->dev) {
        ask->dev = b->dev;
        ask->pending = firsts_pending(b->dev);
    }
    return !ask->pending;
}

/**
 * The least recently released buffer a miss for a block of `filling` may
 * take (`victim_fit`), or NULL when there is none.
 *
 * Each buffer passed over is moved to the tail of the release order, so
 * that the next search does not read it again before every buffer now
 * ahead of it has been taken or passed over too. A miss on a full cache
 * then reads the buffer it takes and those it passes over, and a buffer
 * that stays unfit, such as one whose device fails its writes, costs a
 * read for each turn of the whole cache and not one for every miss. The
 * search stops at the first buffer it moved, which means every buffer was
 * passed over.
 */
static struct bbuf *victim_find(block_device_t *filling)
{
    struct first_ask ask = {0, 0};
    struct bbuf *b, *moved = 0;

    while ((b = lru_head) != 0 && b != moved) {
        stat_visits++;
        if (victim_fit(b, filling, &ask)) {
            return b;
        }
        lru_del(b);
        lru_add_tail(b);
        if (!moved) {
            moved = b;
        }
    }
    return 0;
}

/**
 * Whether one of `dev`'s dirty `first` buffers cannot be written now: held,
 * barred (`write_barred`), or a block that names others and waits for one.
 * An `after` buffer of the device is then not a victim, since its write
 * might pass the first: a reclaim does not ask which firsts it follows.
 */
static int firsts_pending(const block_device_t *dev)
{
    const struct bbuf *b;

    for (b = firsts.head; b; b = b->dirty_next) {
        stat_visits++;
        if (b->dev == dev &&
            (b->holder != -1 || write_barred(b) || (names_blocks(b) && way_to(b)))) {
            return 1;
        }
    }
    return 0;
}

/**
 * Write `dev`'s unheld dirty `first` buffers, each held across its write,
 * before a reclaim writes an `after` buffer of the device. Returns 0 with
 * nothing to write, 1 when something was written, and the first failure's
 * -errno, which marks that buffer and leaves the rest. Every write sleeps,
 * so the scan starts over after each one. A barred buffer (`write_barred`),
 * which may become barred while an earlier write sleeps, is left, and so is
 * a block that names others and waits for one.
 */
static int write_firsts(block_device_t *dev)
{
    struct bbuf *b;
    int wrote = 0;

again:
    for (b = firsts.head; b; b = b->dirty_next) {
        int rc;

        stat_visits++;
        if (b->dev != dev || b->holder != -1 || write_barred(b) ||
            (names_blocks(b) && way_to(b))) {
            continue;
        }
        rc = write_one(b, 0);
        if (rc < 0) {
            return rc;
        }
        wrote = 1;
        goto again;
    }
    return wrote;
}

int bcache_cached(block_device_t *dev, uint64_t blk)
{
    return index_find(dev, blk) != 0;
}

struct bbuf *bcache_get(block_device_t *dev, uint64_t blk)
{
    return get(dev, blk, 0, 0);
}

struct bbuf *bcache_get_blank(block_device_t *dev, uint64_t blk)
{
    return get(dev, blk, 1, 0);
}

/**
 * The two gets: `bcache_get`, and with `blank` set `bcache_get_blank`.
 *
 * A cached block is returned as it is either way; a miss is filled from the
 * device, or, for the blank get, zeroed without a read. `*fresh`, when
 * asked for, says whether the buffer was keyed to the block by this call.
 *
 * A miss takes an empty buffer, or a new one while the cache is under its
 * limit, or else the least recently released unheld buffer this caller may
 * take, a dirty one written first. A dirty buffer whose last write failed,
 * whose write would take a block this caller may not take while it holds
 * what it holds, or an `after` whose device has a `first` that cannot be
 * written now, is not a candidate. The waits and the writes sleep; the
 * lookup starts over after each, since another caller may have filled the
 * block meanwhile. The buffer is keyed and held before the read sleeps, so
 * a second process asking for the block waits for it instead of filling a
 * second one. Every buffer changes hands here, so here a discard mark left
 * by a device that went while the buffer was held is cleared. NULL for a
 * block past the device's end, a cache with no buffer to give, or a failed
 * read.
 */
static struct bbuf *get(block_device_t *dev, uint64_t blk, int blank, int *fresh)
{
    struct bbuf *victim = 0;
    struct bbuf *b;

    if (fresh) {
        *fresh = 0;
    }
    if (!dev || blk >= (dev->sector_count + BCACHE_BLOCK_SECTORS - 1) / BCACHE_BLOCK_SECTORS) {
        return 0;
    }
    check_hold_rules(dev, blk);

again:
    b = index_find(dev, blk);
    if (b) {
        while (b->holder != -1) {
            stat_waits++;
            wait_unheld(b);
        }
        if (b->dev == dev && b->blk == blk) {
            hold(b);
            stat_hits++;
            return b;
        }
        goto again;
    }
    if (!blank && dev == ahead_dev && blk == ahead_next && fill_run(dev, blk)) {
        goto again;
    }

    for (;;) {
        victim = take_empty();
        if (!victim && nbufs < bcache_limit()) {
            victim = buffer_new();
        }
        if (!victim) {
            victim = victim_find(dev);
        }
        if (!victim) {
            klog(KLOG_WARN, "bcache", "get: no free buffer");
            return 0;
        }
        if (!victim->dirty) {
            break;
        }
        if (victim->order == BCACHE_ORDER_AFTER && write_firsts(victim->dev) != 0) {
            goto again;
        }
        if (write_one(victim, 0) >= 0) {
            goto again;
        }
    }
    index_del(victim);
    victim->dev = dev;
    victim->blk = blk;
    index_add(victim);
    if (!victim->lru_prev && lru_head != victim) {
        lru_add_tail(victim);
    }
    set_clean(victim);
    victim->order = 0;
    victim->discard = 0;
    hold(victim);
    victim->bytes = block_bytes(dev, blk);
    if (fresh) {
        *fresh = 1;
    }
    if (blank) {
        k_memset(victim->data, 0, victim->bytes);
        return victim;
    }
    if (block_request(dev, 0, blk * BCACHE_BLOCK_SECTORS, victim->bytes / BLOCK_SECTOR_SIZE,
                      victim->data) != 0) {
        unhold(victim);
        make_empty(victim);
        wq_wake_all(&victim->wq);
        return 0;
    }
    stat_fills++;
    ahead_dev = dev;
    ahead_next = blk + 1;
    return victim;
}

/**
 * Fill block `blk` of `dev` and the whole blocks after it that are not in
 * the cache, as many as one request moves, from one read into the staging
 * buffer: a file read in order then costs one request per run and not one
 * per page. Returns 1 when `blk` is in the cache afterwards, 0 when the
 * run could not be read and the block is to be filled on its own. Each
 * block is taken with the blank get and released at once, so the hold
 * rules see one buffer at a time. The blank get sleeps when it writes a
 * dirty victim, and a block another process filled meanwhile comes back
 * from it keyed already, with bytes newer than the staged ones: only a
 * buffer the get keyed fresh takes the copy. A write that reached any
 * device since the staged read, a dirty buffer of the run evicted by one
 * of these gets included, may have made the staged bytes of a block older
 * than the disk's, and no block is filled from them after one: the fill
 * stops, and a buffer keyed fresh for it is emptied again. A run is at most
 * half the cache's limit, so that its last blocks cannot evict its first,
 * and the caller's block, the first, is no victim until every block is in;
 * it is not held, since the caller may hold three buffers already and a
 * fourth would leave none for the run's own keying.
 */
static int fill_run(block_device_t *dev, uint64_t blk)
{
    uint32_t most = block_max_sectors(dev) / BCACHE_BLOCK_SECTORS;
    uint64_t whole = dev->sector_count / BCACHE_BLOCK_SECTORS;
    uint32_t n, i, gen;

    if (most > RUN_BLOCKS) {
        most = RUN_BLOCKS;
    }
    if (most > bcache_limit() / 2u) {
        most = bcache_limit() / 2u;
    }
    if (most <= 1 || run_stage_busy || blk >= whole) {
        return 0;
    }
    if (!run_stage) {
        run_stage = kalloc_pages_try(RUN_BLOCKS * BCACHE_BLOCK_BYTES / PAGE_SIZE, 0);
        if (!run_stage) {
            return 0;
        }
    }
    n = whole - blk < most ? (uint32_t)(whole - blk) : most;
    if (n <= 1) {
        return 0;
    }
    run_stage_busy = 1;
    gen = write_gen;
    if (block_request(dev, 0, blk * BCACHE_BLOCK_SECTORS, n * BCACHE_BLOCK_SECTORS, run_stage) !=
        0) {
        run_stage_busy = 0;
        return 0;
    }
    for (i = 0; i < n; i++) {
        struct bbuf *b;
        int fresh;

        if (index_find(dev, blk + i)) {
            continue;
        }
        b = get(dev, blk + i, 1, &fresh);
        if (!b) {
            break;
        }
        if (fresh && write_gen != gen) {
            unhold(b);
            make_empty(b);
            wq_wake_all(&b->wq);
            break;
        }
        if (fresh) {
            k_memcpy(b->data, run_stage + i * BCACHE_BLOCK_BYTES, BCACHE_BLOCK_BYTES);
            stat_fills++;
        }
        if (i == 0) {
            fill_keep = b;
        }
        bcache_release(b);
    }
    fill_keep = 0;
    run_stage_busy = 0;
    ahead_dev = dev;
    ahead_next = blk + n;
    return index_find(dev, blk) != 0;
}

/* The stamp orders the buffers for the shrink; the release order is the
 * list. */
void bcache_release(struct bbuf *b)
{
    if (!b || b->holder != caller_pid()) {
        k_panic("bcache: hold rule: a release by a process that does not hold the buffer");
        return;
    }
    unhold(b);
    b->stamp = ++clock_stamp;
    if (b->discard) {
        make_empty(b);
    } else if (b->dev) {
        lru_del(b);
        lru_add_tail(b);
    }
    wq_wake_all(&b->wq);
}

void bcache_dirty(struct bbuf *b)
{
    if (!b || b->holder != caller_pid()) {
        k_panic("bcache: hold rule: a dirty mark by a process that does not hold the buffer");
        return;
    }
    set_dirty(b);
}

void bcache_dirty_ordered(struct bbuf *b, int order)
{
    bcache_dirty(b);
    if (!b) {
        return;
    }
    if ((b->order == BCACHE_ORDER_FIRST) == (order == BCACHE_ORDER_FIRST)) {
        b->order = order;
        return;
    }
    dirty_unlink(b);
    b->order = order;
    dirty_link(b);
}

void bcache_dirty_owned(struct bbuf *b, uint64_t owner)
{
    bcache_dirty(b);
    if (b) {
        b->owner = owner;
        b->level = 0;
    }
}

void bcache_dirty_pointer(struct bbuf *b, uint64_t owner, int level)
{
    bcache_dirty_owned(b, owner);
    if (b) {
        b->level = level;
    }
}

/** The two writes through, `bcache_write` and `bcache_write_unflushed`: a
 * held buffer, with the request `flags`. */
static int write_held(struct bbuf *b, uint32_t flags)
{
    if (!b || b->holder != caller_pid()) {
        k_panic("bcache: hold rule: a write by a process that does not hold the buffer");
        return -EIO;
    }
    if (b->dev->flags & BLOCK_WRITE_FAILED) {
        return -EROFS;
    }
    set_dirty(b);
    return write_buffer_flags(b, flags);
}

/* The -EROFS after a failed write keeps an operation in progress from
 * writing across the mount's change to read-only. */
int bcache_write(struct bbuf *b)
{
    return write_held(b, 0);
}

int bcache_write_unflushed(struct bbuf *b)
{
    return write_held(b, BLOCK_REQ_NOFLUSH);
}

/** A dirty `first` buffer of `dev`, or NULL when it has none. */
static struct bbuf *first_dirty(const block_device_t *dev)
{
    struct bbuf *b;

    for (b = firsts.head; b; b = b->dirty_next) {
        stat_visits++;
        if (b->dev == dev) {
            return b;
        }
    }
    return 0;
}

/* The dirty blocks one walk of the dirty buffers gathers for a sync when
 * the heap has no room for a batch of all of them: a walk per run written
 * would read every dirty buffer for every run. This batch is on the stack,
 * and a larger one is the sync's own, since a write through a loop device
 * syncs the device under it from inside the sync. */
#define SYNC_BATCH       128
/* The most pages a sync asks of the heap for its batch: a reserve run of
 * the heap, which holds 131072 blocks. */
#define SYNC_BATCH_PAGES 256u

/**
 * Move the entry at `at` of the max-heap `heap` of `n` blocks down to its
 * place. Each level is a step of a sync's bookkeeping, counted as a visit.
 */
static void batch_sift_down(uint64_t *heap, int n, int at)
{
    for (;;) {
        int child = 2 * at + 1;
        uint64_t moved;

        stat_visits++;
        if (child >= n) {
            return;
        }
        if (child + 1 < n && heap[child + 1] > heap[child]) {
            child++;
        }
        if (heap[at] >= heap[child]) {
            return;
        }
        moved = heap[at];
        heap[at] = heap[child];
        heap[child] = moved;
        at = child;
    }
}

/** Move the entry at `at` of the max-heap `heap` up to its place. */
static void batch_sift_up(uint64_t *heap, int at)
{
    while (at > 0) {
        int parent = (at - 1) / 2;
        uint64_t moved;

        stat_visits++;
        if (heap[parent] >= heap[at]) {
            return;
        }
        moved = heap[at];
        heap[at] = heap[parent];
        heap[parent] = moved;
        at = parent;
    }
}

/** Whether pass `pass` of a sync of `scope` writes `b`: dirty since before
 * the sync began, of its owner, and not left for a later sync. */
static int sync_wants(const struct bbuf *b, int pass, const struct sync_scope *scope)
{
    return b->dirty && (pass != 0 || b->order == BCACHE_ORDER_FIRST) &&
           (!scope->since || b->dirtied <= scope->since) &&
           (!scope->owner || b->owner == scope->owner) && !sync_leaves(b, scope);
}

/**
 * Gather the lowest `cap` dirty blocks of `dev` from block `from` on that
 * the pass writes (`sync_wants`), in ascending order in `out`. Returns how
 * many. Block numbers and not buffers: a write
 * of the batch sleeps, and a buffer let go of meanwhile can be given to
 * another block or to the heap.
 *
 * The batch is gathered in a max-heap, its highest block at `out[0]`, so a
 * block below that one takes its place in at most log2(cap) steps and a
 * block above it costs one comparison, and then sorted in place. The dirty
 * list has its newest buffer first, so a file written in order lists its
 * blocks in descending order, and each of them would shift a sorted batch
 * whole.
 */
static int gather_dirty(block_device_t *dev, int pass, const struct sync_scope *scope,
                        uint64_t from, uint64_t *out, int cap)
{
    const struct bbuf *b = firsts.head;
    int rest = pass != 0;
    int n = 0, i;

    for (;;) {
        if (!b) {
            if (!rest) {
                break;
            }
            rest = 0;
            b = dirties.head;
            continue;
        }
        stat_visits++;
        if (b->dev == dev && sync_wants(b, pass, scope) && b->blk >= from) {
            if (n < cap) {
                out[n] = b->blk;
                batch_sift_up(out, n);
                n++;
            } else if (b->blk < out[0]) {
                out[0] = b->blk;
                batch_sift_down(out, n, 0);
            }
        }
        b = b->dirty_next;
    }
    for (i = n - 1; i > 0; i--) {
        uint64_t highest = out[0];

        out[0] = out[i];
        out[i] = highest;
        batch_sift_down(out, i, 0);
    }
    return n;
}

/**
 * Panic when the caller could not write `b` of `dev` for a sync: it holds
 * the buffer, or holds another device's that forbids the write.
 */
static void check_sync_holds(const struct bbuf *b, block_device_t *dev)
{
    if (b->holder == caller_pid()) {
        k_panic("bcache: hold rule: a sync while holding a block of the device");
    }
    if (!victim_may_be_written(b->dev, dev)) {
        k_panic("bcache: hold rule: a sync while holding a block of another device");
    }
}

/**
 * The writes of one pass of a sync, from batches of `cap` blocks in `batch`,
 * as `sync_pass` describes.
 *
 * The batch is gathered again from the start when a scan past the last
 * block written finds nothing, for what a wait passed over. After a wait
 * the batch goes on from the same block, since a gather reads every dirty
 * buffer with the big lock held. A block written, emptied or keyed again
 * while the batch's writes slept is skipped. A block that waits for others
 * goes after them (`write_run`). A sync that holds the last blocks leaves
 * the afters too while any `first` is dirty, since their firsts may be
 * among the last blocks, and gathers again after a wait to see them. Each
 * block owed is written once, and one dirtied again since is not owed, so
 * the pass ends whatever other writers do.
 */
static int sync_batches(block_device_t *dev, int pass, struct sync_scope *scope, uint64_t *batch,
                        int cap)
{
    uint64_t from = 0;

    for (;;) {
        int n, i;

        scope->hold_afters = scope->hold && first_dirty(dev);
        n = gather_dirty(dev, pass, scope, from, batch, cap);
        if (n == 0) {
            if (from == 0) {
                return 0;
            }
            from = 0;
            continue;
        }
        for (i = 0; i < n; i++) {
            struct bbuf *next = index_find(dev, batch[i]);
            uint64_t last = 0;
            int rc;

            if (!next || !sync_wants(next, pass, scope) || next->blk < from) {
                continue;
            }
            if (next->holder != -1 && next->holder != caller_pid()) {
                wait_unheld(next);
                rc = 1;
            } else {
                check_sync_holds(next, dev);
                rc = write_run(next, &last,
                               next->order == BCACHE_ORDER_FIRST ? scope->first_flags
                                                                 : scope->flags,
                               scope);
            }
            if (rc < 0) {
                return rc;
            }
            if (rc > 0 && scope->hold) {
                break;
            }
            if (rc > 0) {
                i--;
                continue;
            }
            from = last + 1;
        }
    }
}

/** The dirty buffers of `owner`, of any device. */
static uint32_t owned_dirty(uint64_t owner)
{
    const struct bbuf *b;
    uint32_t n = 0;
    int list;

    for (list = 0; list < 2; list++) {
        for (b = list ? dirties.head : firsts.head; b; b = b->dirty_next) {
            stat_visits++;
            if (b->owner == owner) {
                n++;
            }
        }
    }
    return n;
}

/**
 * One pass of a sync: the `first` buffers `scope` covers in pass 0 and every
 * dirty one it covers in pass 1. Returns 0 or the first failure's -errno.
 *
 * The pass gathers every dirty block in one walk when the heap gives a batch
 * that holds them, since a run writes as many as 256 blocks and a batch of
 * 128 contiguous ones is spent by one run: with tens of thousands dirty, a
 * walk for each run would read every dirty buffer for every run. Without
 * the room it gathers `SYNC_BATCH` blocks a walk. An owner's sync sizes the
 * batch to the owner's blocks, however many its neighbours have dirty.
 */
static int sync_pass(block_device_t *dev, int pass, struct sync_scope *scope)
{
    uint64_t small[SYNC_BATCH];
    uint64_t *batch = 0;
    uint32_t pages = 0;
    uint32_t count = ndirty;
    int rc;

    if (scope->owner && count > SYNC_BATCH) {
        count = owned_dirty(scope->owner);
    }
    if (count > SYNC_BATCH) {
        pages = (count + PAGE_SIZE / 8u - 1u) / (PAGE_SIZE / 8u);
        if (pages > SYNC_BATCH_PAGES) {
            pages = SYNC_BATCH_PAGES;
        }
        batch = kalloc_pages_try(pages, 0);
    }
    if (!batch) {
        return sync_batches(dev, pass, scope, small, SYNC_BATCH);
    }
    rc = sync_batches(dev, pass, scope, batch, (int)(pages * (PAGE_SIZE / 8u)));
    kfree_pages(batch, pages);
    return rc;
}

/** The two passes of a sync. */
static int sync_passes(block_device_t *dev, struct sync_scope *scope)
{
    int rc = sync_pass(dev, 0, scope);

    return rc != 0 ? rc : sync_pass(dev, 1, scope);
}

static int sync_device(block_device_t *dev, int durable);

/*
 * A block that waits for others is written after them, whatever their block
 * numbers, and is held from the look that finds its way clear until its
 * write ends, so the order the marks promise holds across the writes'
 * sleeps. A dirty buffer another process holds is waited for, not passed
 * over, since a sync that returned over one would report clean what is still
 * in flight. The place reached is kept as a block number, since a buffer let
 * go of during a wait can be given to another block.
 *
 * On a write-through disk a `first` buffer is written with the disk's flush,
 * and a block that waits for others has the disk flushed before it, so what
 * it waits for is on the medium first, whether this sync, another or a
 * reclaim wrote it. The other writes leave the disk's cache to one flush at
 * the sync's end, which makes the sync durable when it returns, even when a
 * write failed.
 *
 * On a device that names blocks to write last (`bcache_sync_last`), the
 * passes run twice: without those blocks, then, when that went well, with
 * them. A failure so leaves them dirty, and the `after` blocks with them
 * while a `first` is among them; and what one waits for is mostly written
 * by then, in the first round's runs.
 */
int bcache_sync(block_device_t *dev)
{
    return sync_device(dev, 1);
}

void bcache_sync_last(block_device_t *dev, int (*last)(void *arg, uint64_t blk),
                      int (*names)(void *arg, uint64_t blk, uint64_t owner), void *arg)
{
    dev->sync_last = last;
    dev->sync_names = names;
    dev->sync_last_arg = arg;
}

int bcache_sync_unflushed(block_device_t *dev)
{
    return sync_device(dev, 0);
}

/** `bcache_sync`, durable with `durable` set, when a run of `first` buffers
 * is written with the disk's flush and the sync ends with one; without it
 * every write leaves the disk's cache to the caller's next flush, in the
 * order written. */
static int sync_device(block_device_t *dev, int durable)
{
    int through = (block_flags(dev) & BLOCK_WRITE_THROUGH) != 0;
    struct sync_scope scope;
    int rc, flushed;

    k_memset(&scope, 0, sizeof(scope));
    scope.since = marks;
    scope.flags = unflushed_flags(dev);
    scope.first_flags = durable ? 0 : scope.flags;
    scope.hold = dev->sync_last != 0;
    rc = sync_passes(dev, &scope);
    if (rc == 0 && scope.hold) {
        scope.hold = 0;
        rc = sync_passes(dev, &scope);
    }
    if (!through || !durable) {
        return rc;
    }
    flushed = block_flush(dev);
    if (flushed != 0) {
        klog(KLOG_WARN, "bcache", "sync: flush failed (%d)", flushed);
        dev->flags |= BLOCK_WRITE_FAILED;
        vfs_device_failed(dev, 0, flushed);
    }
    return rc != 0 ? rc : flushed;
}

/* A `first` keeps its flush here, unlike in `bcache_sync_unflushed`: nothing
 * says that the caller's flush comes before another caller's `after`, and a
 * file's sync is no reason to give up the order the marks promise. */
int bcache_sync_owned(block_device_t *dev, uint64_t owner)
{
    struct sync_scope scope;

    k_memset(&scope, 0, sizeof(scope));
    scope.owner = owner;
    scope.since = marks;
    scope.flags = unflushed_flags(dev);
    return sync_passes(dev, &scope);
}

/* What the block waits for is written here one block a call, and not held:
 * the caller looks at its own state between the calls. */
int bcache_sync_block_ready(block_device_t *dev, uint64_t blk)
{
    struct bbuf *b = index_find(dev, blk), *d;
    int rc;

    if (!b || !b->dirty) {
        return 0;
    }
    if (b->holder != -1 && b->holder != caller_pid()) {
        wait_unheld(b);
        return 1;
    }
    d = way_to(b);
    if (!d) {
        return 0;
    }
    if (d->holder == caller_pid()) {
        return -EIO;
    }
    if (d->holder != -1) {
        wait_unheld(d);
        return 1;
    }
    rc = write_one(d, d->order == BCACHE_ORDER_FIRST ? 0 : unflushed_flags(dev));
    return rc < 0 ? rc : 1;
}

/* A block that waits is held while what it waits for is written
 * (`write_follower`), so a steady writer cannot keep the call: what the
 * block waits for stops growing. A wait for another holder looks again. */
int bcache_sync_block(block_device_t *dev, uint64_t blk)
{
    for (;;) {
        struct bbuf *b = index_find(dev, blk);
        int rc;

        if (!b || !b->dirty) {
            return 0;
        }
        if (b->holder != -1 && b->holder != caller_pid()) {
            wait_unheld(b);
            continue;
        }
        check_sync_holds(b, dev);
        rc = write_one(b, b->order == BCACHE_ORDER_FIRST ? 0 : unflushed_flags(dev));
        if (rc <= 0) {
            return rc;
        }
    }
}

void bcache_device_gone(block_device_t *dev)
{
    struct bbuf *b;

    vfs_device_failed(dev, 0, -EIO);
    dev->flags |= BLOCK_GONE;
    for (b = bufs; b; b = b->next) {
        stat_visits++;
        if (b->dev != dev) {
            continue;
        }
        set_clean(b);
        if (b->holder != -1) {
            b->discard = 1;
            continue;
        }
        make_empty(b);
    }
}

int bcache_device_holds(block_device_t *dev)
{
    const struct bbuf *b;
    int held = 0;

    for (b = bufs; b; b = b->next) {
        stat_visits++;
        if (b->dev == dev && b->holder != -1) {
            held++;
        }
    }
    return held;
}

/** The least recently released dirty buffer the shrink may write and
 * free, or NULL: unheld, unwaited for, and one the eviction too could
 * write. */
static struct bbuf *shrink_dirty(void)
{
    struct bbuf *b;

    for (b = lru_head; b; b = b->lru_next) {
        stat_visits++;
        if (!b->dirty || b->holder != -1 || b->waiters > 0) {
            continue;
        }
        if (write_barred(b) || !victim_may_be_written(b->dev, 0)) {
            continue;
        }
        if ((names_blocks(b) && way_to(b)) ||
            (b->order == BCACHE_ORDER_AFTER && firsts_pending(b->dev))) {
            continue;
        }
        return b;
    }
    return 0;
}

/*
 * Three passes, each a walk of one list: the empties; the release order
 * for the clean buffers, oldest first, in one walk, since freeing a clean
 * buffer does not sleep; and with `can_write` the release order again for
 * the dirty ones, from its head after each write, which sleeps. A buffer a
 * process waits for is left, woken or not, since the waiter has its
 * address; so is one dirtied again or waited for while its write slept.
 * The writing pass has a budget, so a buffer dirtied again as fast as it is
 * written cannot keep the shrink here.
 */
uint32_t bcache_shrink(uint32_t want, int can_write, uint32_t *written)
{
    struct bbuf **link = &empties;
    struct bbuf *b, *next;
    uint32_t got = 0;
    int budget = (int)nbufs * 2;

    while (got < want && *link) {
        b = *link;
        stat_visits++;
        if (b->holder != -1 || b->waiters > 0) {
            link = &b->empty_next;
            continue;
        }
        *link = b->empty_next;
        b->empty_next = 0;
        buffer_free(b);
        got++;
    }
    for (b = lru_head; b && got < want; b = next) {
        next = b->lru_next;
        stat_visits++;
        if (b->dirty || b->holder != -1 || b->waiters > 0) {
            continue;
        }
        buffer_free(b);
        got++;
    }
    while (can_write && got < want && budget-- > 0) {
        int rc;

        b = shrink_dirty();
        if (!b) {
            break;
        }
        if (b->order == BCACHE_ORDER_AFTER && write_firsts(b->dev) != 0) {
            continue;
        }
        rc = write_one(b, 0);
        if (rc != 0 || b->dirty || b->waiters > 0) {
            continue;
        }
        (*written)++;
        buffer_free(b);
        got++;
    }
    return got;
}

/**
 * Count, in `*ready`, the buffers the next misses for `dev` take without a
 * write, as far as `want`: the empties, the room under the limit, and the
 * clean buffers `victim_find` reaches first. Returns the first dirty buffer
 * in the way, which a miss would write, or NULL when `want` were found or
 * the release order has ended.
 */
static struct bbuf *reserve_walk(block_device_t *dev, uint32_t want, uint32_t *ready)
{
    struct first_ask ask = {0, 0};
    uint32_t limit = bcache_limit();
    uint32_t have = nbufs < limit ? limit - nbufs : 0;
    const struct bbuf *e;
    struct bbuf *b;

    if (have > want) {
        have = want;
    }
    for (e = empties; e && have < want; e = e->empty_next) {
        stat_visits++;
        have++;
    }
    for (b = lru_head; b && have < want; b = b->lru_next) {
        stat_visits++;
        if (!victim_fit(b, dev, &ask)) {
            continue;
        }
        if (b->dirty) {
            *ready = have;
            return b;
        }
        have++;
    }
    *ready = have;
    return 0;
}

/* The budget stops a buffer dirtied again as fast as it is written from
 * keeping the caller here, as it does in the shrink. The walk starts over
 * after each write, which sleeps. A run takes the victim's dirty neighbours
 * too, which saves a request when the walk reaches them, but none a miss
 * could not write. */
uint32_t bcache_reserve(block_device_t *dev, uint32_t n)
{
    uint32_t limit = bcache_limit();
    uint32_t want = n < limit ? n : limit;
    uint32_t ready = 0;
    int budget = (int)want * 2;
    const struct sync_scope reclaim = {0, 0, 0, 0, 0, 0, 1};

    for (;;) {
        struct bbuf *b = reserve_walk(dev, want, &ready);
        uint64_t last;
        int rc;

        if (!b || budget-- <= 0) {
            return ready;
        }
        if (b->order == BCACHE_ORDER_AFTER) {
            rc = write_firsts(b->dev);
            if (rc < 0) {
                return ready;
            }
            if (rc > 0) {
                continue;
            }
        }
        rc = write_run(b, &last, b->order == BCACHE_ORDER_FIRST ? 0 : unflushed_flags(b->dev),
                       &reclaim);
        if (rc < 0) {
            return ready;
        }
    }
}

int bcache_invalidate(block_device_t *dev)
{
    struct bbuf *b;

    for (b = bufs; b; b = b->next) {
        stat_visits++;
        if (b->dev == dev && (b->holder != -1 || b->dirty)) {
            return -EBUSY;
        }
    }
    for (b = bufs; b; b = b->next) {
        stat_visits++;
        if (b->dev == dev) {
            make_empty(b);
        }
    }
    dev->flags &= ~BLOCK_WRITE_FAILED;
    return 0;
}
