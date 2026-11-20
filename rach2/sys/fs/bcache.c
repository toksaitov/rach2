/*
 * bcache.c: the buffer cache. The contract is in bcache.h; what is here is
 * the bookkeeping: a list of buffers from the heap keyed by device and
 * block, made as they are needed up to the cache's limit and given back
 * under memory pressure, least-recently-released replacement, and the two
 * hold rules checked on every acquisition.
 *
 * Two locks guard the state, and neither is the big lock. `cache_spin`
 * covers what a reader touches: the index and each buffer's key, the release
 * order, the empties and the list of all buffers, each buffer's holder,
 * release count, waiters, stamp and discard mark, and the run state of a
 * fill. It is held only while those change or are read for a decision,
 * never across a request
 * or a wait. `wb_lock`, a mutex, covers the write side: the dirty lists and
 * every field of the order the dirty marks give, and so every dirty mark,
 * write, sync, reserve and eviction of a dirty buffer. It is given up around
 * every sleep, a request, a wait for a buffer, a service point, and taken
 * again after, as the big lock it replaces was given up at each, so a step
 * between two sleeps sees the lists whole and the code after a sleep looks
 * again. A field a holder of `cache_spin` alone reads, a buffer's dirty bit,
 * failed write and level, a device's `BLOCK_WRITE_FAILED` and
 * `BLOCK_WRITES_HALTED` and the blocks it writes last, changes under both
 * locks. Linux's `mark_buffer_dirty` takes no global lock either: it sets
 * the buffer's bit, the folio's under the mapping's `i_pages` lock, and the
 * inode's place on the writeback list under `wb->list_lock`
 * (fs/buffer.c:1179-1210, mm/page-writeback.c:2789-2802,
 * fs/fs-writeback.c:2654), and writeback lets `list_lock` go around each
 * inode's write (fs/fs-writeback.c:2003-2016). The block layer's requests
 * need no lock of the cache's: a miss reads with neither, and a driver that
 * needs the big lock, USB storage or loop, takes it itself (block/block.h).
 * A holder marks its buffer dirty only under `wb_lock` and `cache_spin`, so
 * a buffer that no one holds keeps its dirty bit while either is held, and a
 * dirty one keeps its key while `wb_lock` is.
 */

#include "bcache.h"
#include "../lib/kstring.h"

#include "vfs.h"

#include "../klog.h"
#include "../lib/lockdep.h"
#include "../lib/kerrno.h"
#include "../lib/kirq.h"
#include "../lib/ksignal.h"
#include "../mm/kheap.h"
#include "../mm/paging.h"
#include "../mm/pressure.h"
#include "../panic.h"
#include "../proc/mutex.h"
#include "../proc/process.h"
#include "../proc/sched.h"
#include "../proc/smp.h"
#include "../proc/tgroup.h"

/* Dirty buffers linked both ways, the latest to go dirty at the head. */
struct dirty_list {
    struct bbuf *head;
    struct bbuf *tail;
};

static int version_done(const struct bbuf *v);
static void version_free(struct bbuf *v);

/* The cache's spinlock, as the comment at the top of the file says. */
static struct spinlock cache_spin = SPINLOCK_INIT_NAMED("bcache");

/** Take `cache_spin` with interrupts off; the flags to give back. */
static uint32_t cache_lock(void)
{
    return spin_lock_irqsave(&cache_spin);
}

/** Give back `cache_spin`, which `cache_lock` took with `flags`. */
static void cache_unlock(uint32_t flags)
{
    spin_unlock_irqrestore(&cache_spin, flags);
}

/* The write side's lock, as the comment at the top of the file says. Its
 * place in the kernel's lock order is after every sleeping lock a caller
 * holds and a buffer's hold, and before `cache_spin` (proc/Readme.md,
 * Locking): nothing is waited for while it is held. */
static struct mutex wb_lock = {0,
                               -1,
                               0,
                               {0}
#ifdef RACH2_LOCKDEP
                               ,
                               LOCKDEP_MAP_STATIC("bcache_wb")
#endif
};

/** Take `wb_lock`, sleeping while another holds it; from a context that may
 * sleep, never with it held already. */
static void wb_take(void)
{
    mutex_lock(&wb_lock);
}

/** Give `wb_lock` back. */
static void wb_give(void)
{
    mutex_unlock(&wb_lock);
}

/** Give `wb_lock` up for a sleep when the caller holds it, as every sleep
 * gave up the big lock: 1 when it did, for `wb_back`. */
static int wb_out(void)
{
    if (!mutex_held(&wb_lock)) {
        return 0;
    }
    mutex_unlock(&wb_lock);
    return 1;
}

/** Take `wb_lock` again after a sleep when `wb_out` gave it up (`held`). */
static void wb_back(int held)
{
    if (held) {
        mutex_lock(&wb_lock);
    }
}

static struct bbuf *bufs;     /* every buffer, newest first, linked both ways */
static uint32_t nbufs;        /* how many */
static struct bbuf *lru_head; /* the keyed buffers, least recently released first */
static struct bbuf *lru_tail;
static struct bbuf *empties;      /* the buffers keyed to nothing */
static uint32_t nempties;         /* how many */
static struct dirty_list dirties; /* the dirty buffers but the firsts */
static struct dirty_list firsts;  /* the dirty buffers of the first class */
static uint32_t ndirty;           /* how many */
static uint64_t marks;            /* counts the dirty marks, for `dirtied` and `touched` */
static struct bbuf *kernel_held;  /* held with no process current */
static block_device_t *ahead_dev; /* the device and block a sequential reader */
static uint64_t ahead_next;       /* reaches next; a miss there fills a run */
/* Counts the writes that reached a device, under `cache_spin`, which a fill
 * whose staged read one may have overtaken checks (`fill_run`). */
static uint32_t write_gen;
/* The block a run is being filled for: not a victim while the run's later
 * blocks are keyed, since the caller has let it go to key them and would
 * otherwise find it gone. Under `cache_spin`. */
static struct bbuf *fill_keep;
/* Counts the buffers let go that a miss may take, under `cache_spin`: each
 * one not kept for a fill that is clean, or dirty with its write not barred
 * (`write_barred`), and the fills that ended the keep of a clean one. A
 * search that lets the lock go reads it to tell a turn in which such a
 * buffer was let go, perhaps behind the turn, from one in which none was
 * (`victim_any`). */
static uint32_t fit_gos;
static uint32_t limit_set; /* a limit given by `bcache_set_limit`, or 0 */
/* Who wakes the flusher at the first dirty buffer of a clean cache and once
 * the dirty buffers pass the background threshold (`bcache_set_flusher`),
 * or NULL. */
static void (*flusher_kick)(void);

/* Linux's names and defaults (mm/page-writeback.c:70-95): the share of the
 * cache's limit, or the bytes, past which the flusher writes back, and past
 * which a writer writes back itself. A ratio of 0 gives way to the bytes,
 * and the bytes of 0 to the ratio. */
int dirty_background_ratio = 10;
unsigned long dirty_background_bytes;
int vm_dirty_ratio = 20;
unsigned long vm_dirty_bytes;
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

/** `n` bytes in buffers, rounded up, or `limit` buffers times `ratio` per
 * cent, as Linux's `domain_dirty_limits` takes the bytes when set. */
static uint32_t dirty_share(unsigned long n, int ratio, uint32_t limit)
{
    if (n) {
        unsigned long want = (n + BCACHE_BLOCK_BYTES - 1u) / BCACHE_BLOCK_BYTES;

        return want > limit ? limit : (uint32_t)want;
    }
    return (uint32_t)(((uint64_t)limit * (uint32_t)ratio) / 100u);
}

/* As Linux's `domain_dirty_limits` (mm/page-writeback.c:398-447): the
 * background threshold is half the limit when it is not below it. */
static void dirty_limits(uint32_t *background, uint32_t *hard)
{
    uint32_t limit = bcache_limit();
    uint32_t bg = dirty_share(dirty_background_bytes, dirty_background_ratio, limit);
    uint32_t th = dirty_share(vm_dirty_bytes, vm_dirty_ratio, limit);

    if (bg >= th) {
        bg = th / 2u;
    }
    *background = bg;
    *hard = th;
}

int bcache_dirty_level(void)
{
    uint32_t bg, th, n = ndirty;

    dirty_limits(&bg, &th);
    return n >= th ? 2 : n >= bg ? 1 : 0;
}

uint32_t bcache_dirty_count(void)
{
    return ndirty;
}

void bcache_set_flusher(void (*kick)(void))
{
    flusher_kick = kick;
}

uint64_t bcache_marks(void)
{
    uint32_t flags = cache_lock();
    uint64_t n = marks;

    cache_unlock(flags);
    return n;
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
 *
 * The move goes a bucket of the old table at a time and lets `cache_spin`
 * go between batches, as Linux's rhashtable moves its buckets to a new
 * table with a `cond_resched` between (lib/rhashtable.c:319-355). Until
 * the move ends, a buffer whose bucket of the old table has not been moved
 * is found there, and every other one in the new table (`bucket_head`).
 */
#define BHASH_FIRST       4096u
#define BHASH_FIRST_SHIFT 12
/* The most buckets: the limit of a machine with 64 GiB of memory. */
#define BHASH_MOST        (1u << 22)
static struct bbuf *bhash_first[BHASH_FIRST];
static struct bbuf **bhash = bhash_first;
static uint32_t bhash_size = BHASH_FIRST; /* a power of two */
static uint32_t bhash_shift = BHASH_FIRST_SHIFT;
static uint32_t bhash_retry;     /* buffers below which a refused table is not asked again */
static struct bbuf **bhash_from; /* the table a move empties, or NULL with none under way */
static uint32_t from_shift;      /* its buckets' shift; it has 1 << from_shift */
static uint32_t from_moved;      /* its buckets below this one have been moved */

uint32_t bcache_buckets(void)
{
    return bhash_size;
}

/** The bucket for block `blk` of `dev` in a table of `1 << shift`
 * buckets. The block's number, its high bits folded onto the low, gives
 * consecutive blocks consecutive buckets, which no two of them share; the
 * device moves its blocks to a place of their own, the top bits of its
 * address's product with the golden ratio's fraction (Knuth's
 * multiplicative hashing), which every bit of the address reaches, so that
 * devices whose records lie a power of two apart are not put on the same
 * buckets. */
static uint32_t bucket_in(const block_device_t *dev, uint64_t blk, uint32_t shift)
{
    uint32_t lo = (uint32_t)blk;
    uint32_t at = lo ^ (lo >> shift) ^ (uint32_t)(blk >> 32);
    uint32_t place = ((uint32_t)((uintptr_t)dev >> 4) * 0x9E3779B1u) >> (32 - shift);

    return (at + place) & ((1u << shift) - 1u);
}

/** The head of the bucket that holds block `blk` of `dev`: in the table a
 * move empties while the block's bucket there has not been moved, in the
 * index's table otherwise. `cache_spin` is held. */
static struct bbuf **bucket_head(const block_device_t *dev, uint64_t blk)
{
    if (bhash_from) {
        uint32_t h = bucket_in(dev, blk, from_shift);

        if (h >= from_moved) {
            return &bhash_from[h];
        }
    }
    return &bhash[bucket_in(dev, blk, bhash_shift)];
}

/** Put the keyed buffer `b` in its bucket of the index. */
static void index_add(struct bbuf *b)
{
    struct bbuf **head = bucket_head(b->dev, b->blk);

    b->hnext = *head;
    *head = b;
}

/** Take `b` out of the index before its device is cleared; an empty
 * buffer is in no bucket. */
static void index_del(struct bbuf *b)
{
    struct bbuf **link;

    if (!b->dev) {
        return;
    }
    for (link = bucket_head(b->dev, b->blk); *link; link = &(*link)->hnext) {
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

    for (b = *bucket_head(dev, blk); b; b = b->hnext) {
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
 * `after` and for every dirty victim, while a writer can have the rest of
 * the cache dirty. The lists and their links are `wb_lock`'s.
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
 * dirty list, at the head; `wb_lock` and `cache_spin` are held. */
static void set_dirty(struct bbuf *b)
{
    lockdep_assert_held(&wb_lock);
    b->touched = ++marks;
    if (b->dirty) {
        return;
    }
    b->dirty = 1;
    b->dirtied = b->touched;
    ndirty++;
    dirty_link(b);
}

/** Let go of the orders `b` waits for (`waits`): each buffer it waited for
 * loses the count in its waiters that kept it. The array stays with `b`
 * for its next waits, and goes back to the heap with it. `cache_spin` is
 * held, and `wb_lock`. */
static void waits_clear(struct bbuf *b)
{
    uint32_t i;

    if (!b->waits) {
        return;
    }
    for (i = 0; i < b->waits->n; i++) {
        b->waits->w[i].t->waiters--;
    }
    b->waits->n = 0;
}

/** Mark `b` clean and take it off its dirty list, with the orders it waits
 * for let go; `cache_spin` is held, and `wb_lock` too unless `b` is
 * clean. */
static void set_clean(struct bbuf *b)
{
    if (!b->dirty) {
        return;
    }
    lockdep_assert_held(&wb_lock);
    b->dirty = 0;
    b->owner = 0;
    b->level = 0;
    b->dirtied = 0;
    b->entry = 0;
    b->need = 0;
    waits_clear(b);
    ndirty--;
    dirty_unlink(b);
}

/** Take `b` off the index and the release order, keyed to nothing, and put
 * it on the empties; `cache_spin` is held, and `wb_lock` too unless `b` is
 * clean. */
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
    nempties++;
}

/** An empty buffer off the empties, or NULL when there is none;
 * `cache_spin` is held. */
static struct bbuf *take_empty(void)
{
    struct bbuf *b = empties;

    if (b) {
        empties = b->empty_next;
        b->empty_next = 0;
        nempties--;
    }
    return b;
}

/** An unheld buffer off the cache's lists and onto `*reap`, through its
 * `next`, for `buffers_reap` to give back to the heap once `cache_spin` is
 * let go: a clean keyed one, or an empty one the caller has taken off the
 * empties; `cache_spin` is held. */
static void buffer_unlink(struct bbuf *b, struct bbuf **reap)
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
    b->prev = 0;
    b->next = *reap;
    *reap = b;
}

/** Give the buffers `buffer_unlink` put on `*reap` back to the heap, with
 * `cache_spin` let go, since a free of a page flushes every processor's
 * translations (`kfree_pages`), as Linux frees what its LRU scan isolated
 * after it lets `lru_lock` go (mm/vmscan.c:1966-1985). */
static void buffers_reap(struct bbuf **reap)
{
    while (*reap) {
        struct bbuf *b = *reap;

        *reap = b->next;
        kfree_pages(b->data, 1);
        kfree(b->waits);
        kfree(b);
    }
}

/* The buffers a walk of the release order or the empties reads in one hold
 * of `cache_spin`, which keeps interrupts off, before it lets the lock go:
 * Linux's LRU scan takes SWAP_CLUSTER_MAX pages at a time under `lru_lock`
 * (mm/vmscan.c:5729). */
#define WALK_BATCH 32

/** Whether `b` is on the release order, which a buffer made empty or freed
 * leaves; `cache_spin` is held. */
static int on_lru(const struct bbuf *b)
{
    return b->lru_prev != 0 || lru_head == b;
}

/**
 * Let `cache_spin`, taken with `flags`, go between two batches of a walk,
 * and take it again: `keep`, the buffer the walk goes on from, or NULL, is
 * counted among its waiters meanwhile, so that no shrink frees it, and the
 * buffers on `*reap`, when `reap` is not NULL, go back to the heap. Returns
 * the flags of the new take. The walk reads `keep` again under the lock,
 * since anything may have changed it but its address.
 */
static uint32_t walk_pause(struct bbuf *keep, struct bbuf **reap, uint32_t flags)
{
    if (keep) {
        keep->waiters++;
    }
    cache_unlock(flags);
    if (reap) {
        buffers_reap(reap);
    }
    flags = cache_lock();
    if (keep) {
        keep->waiters--;
    }
    return flags;
}

/**
 * The buffer after `b` on the list of all buffers, for a walk of the list
 * under `cache_spin`, taken with `*flags`, that lets the lock go every
 * WALK_BATCH buffers, counted in `*n` (`walk_pause`). The buffer returned is
 * kept meanwhile, so no shrink takes it off the list; a buffer made
 * meanwhile goes on at the list's head, behind the walk.
 */
static struct bbuf *bufs_next(struct bbuf *b, uint32_t *n, uint32_t *flags)
{
    struct bbuf *next = b->next;

    if (next && ++*n % WALK_BATCH == 0) {
        *flags = walk_pause(next, 0, *flags);
    }
    return next;
}

/** Whether the buffers outnumber the index's buckets with no move under
 * way, for the caller to call `index_fit` once `cache_spin`, held, is let
 * go. */
static int index_due(void)
{
    return !bhash_from && nbufs > bhash_size && nbufs >= bhash_retry;
}

/**
 * Give the index a bucket for each buffer the limit allows, once the
 * buffers outnumber its buckets: a table from the heap, every keyed buffer
 * moved to it a bucket of the old table at a time, and the old table given
 * back unless it is the first. The table is taken and the old one given
 * back with `cache_spin` let go, since a free of pages flushes every
 * processor's translations (`kfree_pages`), and the move lets the lock go
 * every WALK_BATCH buckets and buffers (`walk_pause`). A second caller
 * while a move is under way leaves it to the first. Called without
 * `cache_spin`.
 */
static void index_fit(void)
{
    uint32_t flags = cache_lock();
    uint32_t most = bcache_limit(), size = bhash_size, shift = bhash_shift;
    uint32_t pages, from_pages = 0, n = 0;
    struct bbuf **table, **from;

    if (!index_due()) {
        cache_unlock(flags);
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
        cache_unlock(flags);
        return;
    }
    cache_unlock(flags);
    pages = (uint32_t)((size * sizeof(*table) + PAGE_SIZE - 1) / PAGE_SIZE);
    table = kalloc_pages_try(pages, 0);
    if (table) {
        k_memset(table, 0, (size_t)pages * PAGE_SIZE);
    }
    flags = cache_lock();
    if (!table) {
        bhash_retry = nbufs * 2u;
        cache_unlock(flags);
        return;
    }
    if (bhash_from || bhash_size >= size) {
        cache_unlock(flags);
        kfree_pages(table, pages);
        return;
    }
    bhash_from = bhash;
    from_shift = bhash_shift;
    from_moved = 0;
    bhash = table;
    bhash_size = size;
    bhash_shift = shift;
    while (from_moved < (1u << from_shift)) {
        struct bbuf *b = bhash_from[from_moved];

        bhash_from[from_moved++] = 0;
        while (b) {
            struct bbuf *next = b->hnext;
            uint32_t h = bucket_in(b->dev, b->blk, bhash_shift);

            stat_visits++;
            n++;
            b->hnext = bhash[h];
            bhash[h] = b;
            b = next;
        }
        if (++n >= WALK_BATCH) {
            n = 0;
            flags = walk_pause(0, 0, flags);
        }
    }
    from = bhash_from;
    if (from != bhash_first) {
        from_pages =
            (uint32_t)((((size_t)1 << from_shift) * sizeof(*table) + PAGE_SIZE - 1) / PAGE_SIZE);
    }
    bhash_from = 0;
    cache_unlock(flags);
    if (from_pages) {
        kfree_pages(from, from_pages);
    }
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

/** Mark `b`, which nobody holds, held by the caller and put it on the
 * caller's list; `cache_spin` is held. */
static void hold(struct bbuf *b)
{
    struct bbuf **head = held_list();

    b->holder = caller_pid();
    b->hold_next = *head;
    *head = b;
}

static int write_barred(const struct bbuf *b);

/** Take `b` off the caller's list and mark it unheld, counted in `fit_gos`
 * when it is not kept for a fill and is clean or dirty with a write that is
 * not barred; `cache_spin` is held. */
static void unhold(struct bbuf *b)
{
    struct bbuf **link;

    if (b != fill_keep && (!b->dirty || !write_barred(b))) {
        fit_gos++;
    }

    for (link = held_list(); *link; link = &(*link)->hold_next) {
        stat_visits++;
        if (*link == b) {
            *link = b->hold_next;
            break;
        }
    }
    b->hold_next = 0;
    b->holder = -1;
    b->releases++;
}

/**
 * Let go of `b`, held by the caller, and wake whoever waits for it, under
 * `cache_spin`, which the caller keeps until the wakes are made: once the
 * lock goes, an unheld buffer whose waiters have left may be freed by a
 * shrink (`buffers_reap`), so a wake after it would touch freed memory.
 * Linux's `unlock_buffer` wakes while the holder still has its reference
 * (fs/buffer.c:75-80, `brelse` after it), and wakes only a queue with a
 * sleeper (`__wake_up_bit`, kernel/sched/wait_bit.c:121-127); every
 * sleeper here is counted in `waiters` under the lock before it sleeps, so
 * a buffer with none needs no wake. The processors the wakes interrupt are
 * added to `*cpus`, for `sched_kick_cpus` once the lock is let go, as
 * `wq_wake_all_deferred` asks.
 */
static void unhold_wake(struct bbuf *b, uint64_t *cpus)
{
    unhold(b);
    if (b->waiters > 0) {
        wq_wake_all_deferred(&b->wq, cpus);
    }
}

/**
 * Read `sectors` sectors of `dev` at `lba` into `buf` for a miss, with or
 * without the big lock: the request waits on its own completion, as Linux's
 * `submit_bio_wait` (block/bio.c:1469-1481), and a driver that needs the big
 * lock, USB storage or loop, takes it itself (block/block.h). 0, or the
 * request's error.
 */
static int device_read(block_device_t *dev, uint64_t lba, uint32_t sectors, void *buf)
{
    return block_request(dev, 0, lba, sectors, buf);
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

/* A wait keeps the buffer alive and names the hold whose end it awaits. */
struct buffer_wait {
    struct bbuf *buf;  /* counted in waiters until the wait ends, or NULL */
    uint64_t releases; /* the release count when the held buffer was met */
};

/** Whether the observed hold ended, even if the buffer was reused before
 * this look. The wait keeps the buffer alive; `cache_spin` covers the
 * release count, including its two-word read on i686. */
static int busy_gone(void *arg)
{
    const struct buffer_wait *wait = arg;
    uint32_t flags = cache_lock();
    int gone = wait->buf->releases != wait->releases;

    cache_unlock(flags);
    return gone;
}

/** Whether the calling thread has SIGKILL pending, its own or its
 * process's; 0 with no process. */
static int caller_killed(void)
{
    return current_proc &&
           ((current_proc->sig_pending | current_proc->tg->pending) & KSIG_BIT(SIGKILL)) != 0;
}

/** The condition of a killable wait for a hold's end: `busy_gone`, or
 * SIGKILL pending, which `wq_wait_event_killable` needs of its condition. */
static int busy_gone_or_killed(void *arg)
{
    return busy_gone(arg) || caller_killed();
}

/** Sleep until the hold observed in `wait` ends, or, `killable`, until
 * SIGKILL is pending, and drop its waiter pin. No cache spinlock is held.
 * Give up `wb_lock` while sleeping, and take it again afterwards when the
 * caller held it. The caller must look up its block again, since another
 * holder may already have reused the buffer. */
static void wait_release(struct buffer_wait *wait, int killable)
{
    int held = wb_out();
    uint32_t flags;
    int done;

    if (killable) {
        (void)wq_wait_event_killable(&wait->buf->wq, busy_gone_or_killed, wait, "__wait_on_buffer");
    } else {
        (void)wq_wait_event_nointr(&wait->buf->wq, busy_gone, wait);
    }
    flags = cache_lock();
    wait->buf->waiters--;
    done = version_done(wait->buf);
    cache_unlock(flags);
    if (done) {
        version_free(wait->buf);
    }
    wb_back(held);
}

/** Sleep until the present holder releases `b`, found held under
 * `cache_spin`, taken with `flags`, and counted among its waiters from that
 * look until the wait ends, so that a shrink does not free it meanwhile;
 * `killable`, SIGKILL pending ends the wait too. Returns with `cache_spin`
 * given back; `wb_lock`, when the caller holds it, is given up for the
 * sleep, since the holder may need it to let `b` go. */
static void wait_unheld_how(struct bbuf *b, uint32_t flags, int killable)
{
    struct buffer_wait wait;

    wait.buf = b;
    wait.releases = b->releases;
    b->waiters++;
    cache_unlock(flags);
    wait_release(&wait, killable);
}

/** `wait_unheld_how` that no signal ends. */
static void wait_unheld(struct bbuf *b, uint32_t flags)
{
    wait_unheld_how(b, flags, 0);
}

/** Hold `b` for the caller when nobody holds it, under `cache_spin`: 1 when
 * held, 0 when another holds it. */
static int try_hold(struct bbuf *b)
{
    uint32_t flags = cache_lock();
    int got = b->holder == -1;

    if (got) {
        hold(b);
    }
    cache_unlock(flags);
    return got;
}

/** Sleep until the present holder releases `b`, unless the caller holds
 * it or nobody holds it now: 1 after a wait, 0 with nothing done. */
static int wait_other(struct bbuf *b)
{
    uint32_t flags = cache_lock();

    if (b->holder == -1 || b->holder == caller_pid()) {
        cache_unlock(flags);
        return 0;
    }
    wait_unheld(b, flags);
    return 1;
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

/** Whether `b` waits for other dirty blocks by the rules of the classes:
 * an `after`, or a block that names others. */
static int follows_classes(const struct bbuf *b)
{
    return b->order == BCACHE_ORDER_AFTER || names_blocks(b);
}

/** Whether `b` may wait by orders (`bcache_order_after`): an older image of
 * its block is not yet written, or it keeps waits, met or not. */
static int ordered(const struct bbuf *b)
{
    return b->older != 0 || (b->waits && b->waits->n > 0);
}

/** Whether `b` may wait for other dirty blocks before its write, by the
 * classes or by orders. */
static int follows(const struct bbuf *b)
{
    return follows_classes(b) || ordered(b);
}

/**
 * The image or dirty buffer to write first for the wait `w`: the oldest
 * of its block not yet written, when that one went dirty at or before the
 * mark the wait names, and NULL when the block as it stood then is on the
 * device. A buffer keyed to another block since was clean when it was
 * taken, and so written. `wb_lock` is held, which keeps a dirty buffer's
 * key and the images of a block.
 */
static struct bbuf *wait_way(const struct bcache_wait *w)
{
    struct bbuf *v = w->t;

    if (v->dev != w->dev || v->blk != w->blk || v->version) {
        return 0;
    }
    while (v->older) {
        v = v->older;
    }
    return v->dirty && v->dirtied <= w->k ? v : 0;
}

/** What `b` waits for by orders: the next older image of its block, or the
 * first of its waits not met (`wait_way`), or NULL. `wb_lock` is held. */
static struct bbuf *order_before(const struct bbuf *b)
{
    uint32_t i;

    if (b->older) {
        return b->older;
    }
    for (i = 0; b->waits && i < b->waits->n; i++) {
        struct bbuf *v = wait_way(&b->waits->w[i]);

        if (v) {
            return v;
        }
    }
    return 0;
}

/**
 * Whether the dirty block `d` must reach the device before `b`, which
 * `follows`, with `last` saying whether `b` is a block written last. Only a
 * block dirty since before `b`'s last mark can be what that mark wrote into
 * `b`: a `first` before an `after`; before a block written last any block
 * it may name, one not of its class nor an `after`, shared or of an owner
 * the block names; and before a pointer block that is no `after` a block of
 * its owner below its level, of neither class, and before one of a file's
 * tree a shared block that is no pointer block, such as the bitmap that
 * took a block the pointer block names.
 *
 * Each rule leads to a higher rank: a shared block that names nothing, then
 * a pointer block's level, then the class written last, then the `after`
 * class. So a chain of waits has at most six blocks and never comes back to
 * one.
 */
static int must_precede(const struct bbuf *d, const struct bbuf *b, int last)
{
    if (d == b || d->dev != b->dev || d->dirtied > b->touched || d->entry) {
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
    if (b->order == BCACHE_ORDER_AFTER) {
        return 0;
    }
    if (d->owner == b->owner) {
        return d->level < b->level;
    }
    return d->owner == 0 && d->level == 0;
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

/** A block that must reach the device before the dirty block `b`, or NULL:
 * what it waits for by orders (`order_before`), else the earliest dirtied
 * block of the classes' rules (`must_precede`). An `after` alone waits
 * only for the first class; a block that names others reads the other list
 * too. */
static struct bbuf *first_before(const struct bbuf *b)
{
    struct bbuf *d, *e;
    int last;

    if (!b->dirty) {
        return 0;
    }
    d = order_before(b);
    if (d || !follows_classes(b)) {
        return d;
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
    uint32_t steps = 0;

    while (d && (e = first_before(d)) != 0) {
        d = e;
        if (++steps > ndirty) {
            klog(KLOG_CRIT, "bcache", "order: blocks wait for each other at block %d", (int)d->blk);
            return d;
        }
    }
    return d;
}

/** Whether a write of `b` is kept from an eviction, a shrink or a reserve:
 * its own last write failed, its device's writes are halted, or a write of
 * its device failed and it is a block that names others, which may name the
 * block that failed. */
static int write_barred(const struct bbuf *b)
{
    return b->write_failed || (b->dev->flags & BLOCK_WRITES_HALTED) ||
           ((b->dev->flags & BLOCK_WRITE_FAILED) && (names_blocks(b) || ordered(b)));
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

/**
 * Record a failed write or flush of `dev` at sector `lba` against its mounts,
 * which go read-only (`vfs_device_failed`). Their records change under the
 * big lock, which a caller without it takes here, with `wb_lock` given up
 * meanwhile, since the big lock comes before it in the order.
 */
static void device_failed(block_device_t *dev, uint64_t lba, int rc)
{
    int held = wb_out();
    int big = !bkl_held();

    if (big) {
        bkl_enter();
    }
    vfs_device_failed(dev, lba, rc);
    if (big) {
        bkl_exit();
    }
    wb_back(held);
}

/**
 * Mark `dev` as failed in a write, and with it `b`, the first buffer of the
 * request that failed, or NULL for a flush, under `cache_spin`, since a
 * release reads both (`write_barred`); `wb_lock` is held.
 */
static void write_failed_mark(block_device_t *dev, struct bbuf *b)
{
    uint32_t flags = cache_lock();

    dev->flags |= BLOCK_WRITE_FAILED;
    if (dev->flags & BLOCK_RECOVERING) {
        dev->flags |= BLOCK_WRITES_HALTED;
    }
    if (b) {
        b->write_failed = 1;
    }
    cache_unlock(flags);
}

/*
 * The images of a block (`version`): its bytes as they stood when a change
 * that carries orders came to them while an order waited for them
 * (`bcache_change`). An image is a buffer of its own, from the heap, on the
 * dirty lists with the marks, class, owner and level the block had then,
 * but keyed to nothing: a lookup finds the keyed buffer, whose bytes are
 * the newest, and a block's images, oldest first, are written before it
 * (`order_before`). A written image is retired, taken off its block's
 * chain, and goes back to the heap at the end of its last hold or wait,
 * whichever comes last (`let_go`, `wait_release`).
 */

/** Take the image `v` off its block's chain and mark it retired, written or
 * dropped. `cache_spin` and `wb_lock` are held. */
static void version_retire(struct bbuf *v)
{
    if (v->version != 1) {
        return;
    }
    if (v->newer) {
        v->newer->older = v->older;
    }
    if (v->older) {
        v->older->newer = v->newer;
    }
    v->older = 0;
    v->newer = 0;
    v->version = 2;
}

/** Whether the retired image `v` may go back to the heap: nobody holds it
 * or waits for it. `cache_spin` is held. */
static int version_done(const struct bbuf *v)
{
    return v->version == 2 && v->holder == -1 && v->waiters == 0;
}

/**
 * Drop the images of the keyed buffer `b`, unwritten, for a device that has
 * gone or an unmount that gives up its dirty buffers: each is made clean
 * and retired, and one nobody holds or waits for goes on `*reap`, through
 * its `next`, for `buffers_reap`, or is left to its last hold or wait. With
 * `reap` NULL every image is left so. The count of dirty images dropped.
 * `cache_spin` and `wb_lock` are held.
 */
static int versions_drop(struct bbuf *b, struct bbuf **reap)
{
    int dropped = 0;

    while (b->older) {
        struct bbuf *v = b->older;

        if (v->dirty) {
            dropped++;
        }
        set_clean(v);
        version_retire(v);
        if (reap && version_done(v)) {
            v->next = *reap;
            *reap = v;
        }
    }
    return dropped;
}

/** Give the retired image `v` back to the heap when `version_done` says it
 * may go, as the caller found under `cache_spin` and has let go since:
 * nobody else reaches it then. */
static void version_free(struct bbuf *v)
{
    v->next = 0;
    buffers_reap(&v);
}

/**
 * Mark the `n` buffers of `run`, just written, clean and in no place of
 * the order, and count the write in `write_gen`, in one step under
 * `cache_spin`; `wb_lock` is held. The count comes before the buffers are
 * let go, as `write_run_met` says.
 */
static void written_clean(struct bbuf **run, int n)
{
    struct bbuf *reap = 0;
    uint32_t flags = cache_lock();
    int i;

    for (i = 0; i < n; i++) {
        if (run[i]->version) {
            version_retire(run[i]);
        } else if (run[i]->older) {
            klog(KLOG_CRIT, "bcache", "order: block %d written before its older image",
                 (int)run[i]->blk);
            versions_drop(run[i], &reap);
        }
        set_clean(run[i]);
        run[i]->write_failed = 0;
        run[i]->order = 0;
    }
    write_gen++;
    cache_unlock(flags);
    buffers_reap(&reap);
}

/* A write's flag, never a request's: what the block waits for is written
 * and answered before it, but no flush puts it on the medium first, except
 * before an `after`, whose firsts a free cleared
 * (`bcache_sync_block_ordered`). */
#define WRITE_ORDER_ONLY 0x80000000u

/** Whether a write-through disk is flushed before the write of `b` with
 * the request `flags`: before every block that `follows`, and under
 * `WRITE_ORDER_ONLY` before an `after` alone, so that a free's cleared
 * pointers and inodes stay on the medium ahead of its cleared bits through
 * a loss of power, as the cache's reclaim keeps them; `order_flush` leaves
 * the flush out before an `after` whose firsts are on the medium already. */
static int flush_before(const struct bbuf *b, uint32_t flags)
{
    return follows_classes(b) && (!(flags & WRITE_ORDER_ONLY) || b->order == BCACHE_ORDER_AFTER);
}

/** Count a write of `n` buffers from `run` that `dev` answered with the
 * request `flags`: one more `first` write answered without the disk's
 * flush, when the request had `BLOCK_REQ_NOFLUSH` and a `first` among its
 * buffers. Called before
 * `written_clean` clears their order. */
static void count_firsts(block_device_t *dev, struct bbuf *const *run, int n, uint32_t flags)
{
    uint32_t lf;
    int i;

    if (!(flags & BLOCK_REQ_NOFLUSH)) {
        return;
    }
    for (i = 0; i < n && run[i]->order != BCACHE_ORDER_FIRST; i++) {
    }
    if (i == n) {
        return;
    }
    lf = cache_lock();
    dev->firsts_unflushed++;
    cache_unlock(lf);
}

/** Write the buffer `b`, which the caller holds, to the device with the
 * request `flags`, with `wb_lock` given up for the request. A failure is
 * recorded against the device, through `vfs_device_failed`, and leaves the
 * buffer dirty and marked failed. */
static int write_buffer_flags(struct bbuf *b, uint32_t flags)
{
    int held;
    int rc;

    if (b->dev->flags & BLOCK_WRITES_HALTED) {
        return -EIO;
    }
    held = wb_out();
    rc = block_request_flags(b->dev, 1, b->blk * BCACHE_BLOCK_SECTORS, b->bytes / BLOCK_SECTOR_SIZE,
                             b->data, flags & ~WRITE_ORDER_ONLY);

    wb_back(held);
    if (rc == 0) {
        count_firsts(b->dev, &b, 1, flags);
    }
    if (rc != 0) {
        klog(KLOG_WARN, "bcache", "write: sector %d failed (%d), kept dirty",
             (int)(b->blk * BCACHE_BLOCK_SECTORS), rc);
        write_failed_mark(b->dev, b);
        device_failed(b->dev, b->blk * BCACHE_BLOCK_SECTORS, rc);
        return rc;
    }
    written_clean(&b, 1);
    return 0;
}

/** The request flags of a write that leaves `dev`'s own cache to a later
 * flush; 0 on a disk that has no such write. */
static uint32_t unflushed_flags(block_device_t *dev)
{
    return (block_flags(dev) & BLOCK_WRITE_THROUGH) ? BLOCK_REQ_NOFLUSH : 0;
}

/** The request flags of a reclaim's write of the dirty `b`: without the
 * disk's flush, as Linux's `pageout` writes a page (mm/vmscan.c), but for a
 * `first`, whose flush the order keeps. A block that waits for others has
 * the disk flushed before it whatever wrote them (`write_follower`), so the
 * order holds, and a sync, `fsync` or unmount flushes at its end. */
static uint32_t reclaim_flags(const struct bbuf *b)
{
    return b->order == BCACHE_ORDER_FIRST ? 0 : unflushed_flags(b->dev);
}

/** Flush `dev`, a write-through disk, for the cache, and on success record
 * that every `first` answered before the flush began is on the medium. A
 * failure is the caller's to record. 0, or -errno. */
static int flush_firsts(block_device_t *dev)
{
    uint32_t lf = cache_lock();
    uint64_t seen = dev->firsts_unflushed;
    int rc;

    if (dev->flags & BLOCK_WRITES_HALTED) {
        cache_unlock(lf);
        return -EIO;
    }
    cache_unlock(lf);
    rc = block_flush(dev);
    if (rc == 0) {
        lf = cache_lock();
        if (seen > dev->firsts_flushed) {
            dev->firsts_flushed = seen;
        }
        cache_unlock(lf);
    }
    return rc;
}

/** Whether every buffer of the `n` in `run` that waits for others is an
 * `after` that names no blocks, and so waits for `first` buffers alone. */
static int firsts_only(struct bbuf *const *run, int n)
{
    int i;

    for (i = 0; i < n; i++) {
        if (names_blocks(run[i]) ||
            (follows_classes(run[i]) && run[i]->order != BCACHE_ORDER_AFTER)) {
            return 0;
        }
    }
    return 1;
}

/** Put what a write-through disk under `dev` holds in its cache on the
 * medium, before the `n` buffers of `run`, one of which must follow what
 * went before it; a disk with nothing unflushed answers at once. An `after`
 * that names no blocks follows only the `first` buffers, so a run of such
 * blocks needs the flush only while a first was answered without it since
 * the last flush began: a `first` written with the flush is on the medium
 * when answered. A failure is recorded as a failed write's. 0, or -errno. */
static int order_flush(block_device_t *dev, struct bbuf *const *run, int n)
{
    int rc, held;

    if (!unflushed_flags(dev)) {
        return 0;
    }
    if (firsts_only(run, n)) {
        uint32_t lf = cache_lock();
        int done = dev->firsts_unflushed == dev->firsts_flushed;

        cache_unlock(lf);
        if (done) {
            return 0;
        }
    }
    held = wb_out();
    rc = flush_firsts(dev);
    wb_back(held);
    if (rc != 0) {
        klog(KLOG_WARN, "bcache", "write: flush failed (%d)", rc);
        write_failed_mark(dev, 0);
        device_failed(dev, 0, rc);
    }
    return rc;
}

/** Let go of `b`, held by the caller, and wake whoever waits for it. */
static void let_go(struct bbuf *b)
{
    uint64_t cpus = 0;
    uint32_t flags = cache_lock();
    int done;

    unhold_wake(b, &cpus);
    done = version_done(b);
    cache_unlock(flags);
    sched_kick_cpus(cpus);
    if (done) {
        version_free(b);
    }
}

static int write_one_met(struct bbuf *b, uint32_t flags, struct buffer_wait *met);
static int write_run_met(struct bbuf *first, uint64_t *last, uint32_t flags,
                         const struct sync_scope *scope, struct buffer_wait *met);

/**
 * The end of a write that met `b`, which another process holds, as `met`
 * asks: with `met` NULL, sleep until its holder releases `b`, unless the caller
 * does or nobody holds it now (`wait_other`); with `met`, sleep for
 * nothing, and when another process holds `b`, count it among `b`'s waiters
 * and save its release count in `*met` for a wait once it has let its own
 * buffers go (`met_wait`). Returns 1, a write's answer after a wait.
 */
static int held_met(struct bbuf *b, struct buffer_wait *met)
{
    uint32_t flags;

    if (!met) {
        (void)wait_other(b);
        return 1;
    }
    flags = cache_lock();
    if (b->holder != -1 && b->holder != caller_pid()) {
        b->waiters++;
        met->buf = b;
        met->releases = b->releases;
    }
    cache_unlock(flags);
    return 1;
}

/**
 * Finish the deferred wait in `wait`, whose buffer is pinned, now that
 * the caller holds nothing; with `met`, pass the wait to the caller's
 * caller instead, keeping both its pin and observed release count.
 * Returns 1, also when no buffer was met.
 */
static int met_wait(struct buffer_wait *wait, struct buffer_wait *met)
{
    if (met) {
        *met = *wait;
    } else if (wait->buf) {
        wait_release(wait, 0);
    }
    return 1;
}

/**
 * Write the unheld dirty buffer `b`, which `follows`, after what it waits
 * for, with the request `flags`.
 *
 * `b` is held from the first look until its write ends, so that nothing is
 * stored in it meanwhile and what it waits for cannot grow. The blocks are
 * written the earliest dirtied first (`way_to`), each in a run of its
 * neighbours when it waits for nothing itself; then, on a
 * write-through disk, the flush; then `b`. With `WRITE_ORDER_ONLY` in
 * `flags`, each of them and `b` is written without the disk's flush, in
 * the same order, each request answered before the next is sent, and the flush
 * comes only before an `after` (`flush_before`). One of them held by another
 * process lets `b` go for the wait, since that holder may be waiting for
 * `b`, and the caller looks again; so does a `b` that a reader took since
 * the caller's look. At most two buffers are held at once: `b`, and the
 * `first` an `after` waits for while it is written. The caller holds
 * `wb_lock`.
 *
 * A write of what `b` waits for that meets a buffer another process holds
 * waits for nothing itself (`held_met`): it gives the buffer back, and
 * `b` is let go before the wait, as is every buffer the callers hold that
 * passed `met`, which then receives the buffer to wait on (`met_wait`), so
 * that no wait is made with `b` held.
 *
 * @return 0 with `b` written; 1 after a wait, or with `met` a buffer to
 *         wait on, with `b` unwritten; or the -errno of a failed write or
 *         flush, which leaves `b` dirty, or -EIO for a block the caller
 *         holds itself.
 */
static int write_follower(struct bbuf *b, uint32_t flags, struct buffer_wait *met)
{
    struct sync_scope any;
    struct bbuf *d;
    struct buffer_wait busy;
    uint64_t end;
    uint32_t quiet = flags & WRITE_ORDER_ONLY;
    int rc = 0;

    k_memset(&any, 0, sizeof(any));

    if (!try_hold(b)) {
        return held_met(b, met);
    }
    while (rc == 0 && (d = way_to(b)) != 0) {
        uint32_t lf = cache_lock();
        int holder = d->holder;

        if (holder != -1 && holder != caller_pid()) {
            uint64_t cpus = 0;

            unhold_wake(b, &cpus);
            d->waiters++;
            busy.buf = d;
            busy.releases = d->releases;
            cache_unlock(lf);
            sched_kick_cpus(cpus);
            return met_wait(&busy, met);
        }
        cache_unlock(lf);
        busy.buf = 0;
        busy.releases = 0;
        if (holder != -1) {
            rc = -EIO;
        } else if (follows(d)) {
            rc = write_one_met(d, quiet ? quiet | unflushed_flags(b->dev) : 0, &busy);
        } else {
            rc = write_run_met(
                d, &end,
                quiet || d->order != BCACHE_ORDER_FIRST ? quiet | unflushed_flags(b->dev) : 0, &any,
                &busy);
        }
        if (rc == 1) {
            let_go(b);
            return met_wait(&busy, met);
        }
    }
    if (rc == 0 && flush_before(b, flags)) {
        rc = order_flush(b->dev, &b, 1);
    }
    if (rc == 0) {
        rc = write_buffer_flags(b, flags);
    }
    let_go(b);
    return rc;
}

/** Write the unheld buffer `b` alone, held across the write, with the
 * request `flags`, and wake whoever waits for it; one that `follows` goes
 * after what it waits for (`write_follower`). 1 after a wait for a holder
 * that took `b` since the caller's look, or one `write_follower` gave;
 * with `met`, no wait is made, and the buffer to wait on is given in
 * `*met` (`held_met`). The caller holds `wb_lock`. */
static int write_one_met(struct bbuf *b, uint32_t flags, struct buffer_wait *met)
{
    int rc;

    if (follows(b)) {
        return write_follower(b, flags, met);
    }
    if (!try_hold(b)) {
        return held_met(b, met);
    }
    rc = write_buffer_flags(b, flags);
    let_go(b);
    return rc;
}

/** `write_one_met` that waits itself. */
static int write_one(struct bbuf *b, uint32_t flags)
{
    return write_one_met(b, flags, 0);
}

/* The staging buffer a run of blocks is written or read through, the most
 * one request moves; taken from the heap at the first run. One run at a time
 * uses it, since the request it hands the bytes to sleeps, and a second run
 * would overwrite them in that window; a writer that finds it in use writes
 * its blocks one at a time, and a read fills its block alone. */
static uint8_t *run_stage;
static int run_stage_busy; /* under `cache_spin`, as `run_stage`'s first set */
#define RUN_BLOCKS (BLOCK_REQ_MAX_SECTORS / BCACHE_BLOCK_SECTORS)

/** Give the staging buffer back for the next run. */
static void stage_free(void)
{
    uint32_t flags = cache_lock();

    run_stage_busy = 0;
    cache_unlock(flags);
}
/* The longest run a read fills: Linux's default readahead window of 128 KiB
 * (VM_READAHEAD_PAGES, include/linux/pagemap.h:1375, which bdi_init gives
 * every device, mm/backing-dev.c:1037). */
#define READ_RUN_BLOCKS (131072u / BCACHE_BLOCK_BYTES)

/**
 * Gather in `run` the blocks a run from `first` writes (`write_run`):
 * `first`, then the dirty, unheld blocks after it that `scope` takes, of its
 * place in the order and its kind, as far as one request moves and half the
 * cache, while the staging buffer is free and nobody holds `first`. Returns
 * how many, at least 1; a run of two or more is held whole and has the
 * staging buffer, all in one step under `cache_spin`, so that no reader
 * takes one of its blocks between the look and the hold.
 */
static int run_gather(struct bbuf *first, struct bbuf **run, const struct sync_scope *scope)
{
    block_device_t *dev = first->dev;
    uint32_t most = block_max_sectors(dev) / BCACHE_BLOCK_SECTORS;
    uint32_t flags;
    int n = 1, i;

    run[0] = first;
    if (most > RUN_BLOCKS) {
        most = RUN_BLOCKS;
    }
    if (most > bcache_limit() / 2u) {
        most = bcache_limit() / 2u;
    }
    flags = cache_lock();
    if (!run_stage && most > 1 && !run_stage_busy) {
        run_stage = kalloc_pages_try(RUN_BLOCKS * BCACHE_BLOCK_BYTES / PAGE_SIZE, 0);
    }
    if (run_stage_busy || first->holder != -1 || (follows(first) && way_to(first))) {
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
    if (n > 1) {
        for (i = 0; i < n; i++) {
            hold(run[i]);
        }
        run_stage_busy = 1;
    }
    cache_unlock(flags);
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
 * after what it waits for and alone (`write_one_met`), which may answer 1,
 * and with `met` makes no wait itself.
 *
 * A run is as long as the device takes at once and at most half the cache,
 * as a read run is: its buffers are held across the request's sleep and let
 * go together, and a run that held every buffer would leave a miss
 * meanwhile with no buffer to take. Every block of a run has the place in
 * the order that `first` has, since a `first` and an `after` in one request
 * would reach the device together. A failure is recorded on `first` as
 * `write_buffer_flags` does. A run of one, or a device that takes a page,
 * is `write_buffer_flags`.
 *
 * The write is counted (`written_clean`) before the run's buffers are let
 * go, as `write_buffer_flags` counts it before its caller's `let_go`: a fill
 * without `wb_lock` may take a written buffer as its victim once it is
 * let go, find the block uncached and, with the count unmoved, keep bytes
 * its staged read took before the write, where Linux's readahead puts each
 * folio in the page cache, locked, before it reads (mm/readahead.c:268,
 * 273, 290).
 */
static int write_run_met(struct bbuf *first, uint64_t *last, uint32_t flags,
                         const struct sync_scope *scope, struct buffer_wait *met)
{
    struct bbuf *run[RUN_BLOCKS];
    block_device_t *dev = first->dev;
    uint32_t bytes = 0;
    int n, i, rc, held;

    n = run_gather(first, run, scope);
    *last = first->blk;
    if (n == 1) {
        return write_one_met(first, flags, met);
    }
    rc = flush_before(first, flags) ? order_flush(dev, run, n) : 0;
    if (rc != 0) {
        stage_free();
        for (i = 0; i < n; i++) {
            let_go(run[i]);
        }
        return rc;
    }
    if (dev->flags & BLOCK_WRITES_HALTED) {
        stage_free();
        for (i = 0; i < n; i++) {
            let_go(run[i]);
        }
        return -EIO;
    }
    for (i = 0; i < n; i++) {
        k_memcpy(run_stage + bytes, run[i]->data, run[i]->bytes);
        bytes += run[i]->bytes;
    }
    held = wb_out();
    rc = block_request_flags(dev, 1, first->blk * BCACHE_BLOCK_SECTORS, bytes / BLOCK_SECTOR_SIZE,
                             run_stage, flags & ~WRITE_ORDER_ONLY);
    stage_free();
    wb_back(held);
    if (rc != 0) {
        klog(KLOG_WARN, "bcache", "write: sector %d failed (%d), %d blocks kept dirty",
             (int)(first->blk * BCACHE_BLOCK_SECTORS), rc, n);
        write_failed_mark(dev, first);
        device_failed(dev, first->blk * BCACHE_BLOCK_SECTORS, rc);
    }
    if (rc == 0) {
        count_firsts(dev, run, n, flags);
        written_clean(run, n); /* before the let_go: a fill may take a buffer once let go */
    }
    *last = run[n - 1]->blk;
    for (i = 0; i < n; i++) {
        let_go(run[i]);
    }
    return rc;
}

/** `write_run_met` that waits itself. */
static int write_run(struct bbuf *first, uint64_t *last, uint32_t flags,
                     const struct sync_scope *scope)
{
    return write_run_met(first, last, flags, scope, 0);
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
 * one walk of the release order. It is a hint: a walk that lets
 * `cache_spin` go keeps it meanwhile, when a reader without `wb_lock` may
 * take or let go of a `first`, and the write of an `after` checks again
 * through `write_firsts`. */
struct first_ask {
    const block_device_t *dev;
    int pending;
};

/**
 * Whether a miss for a block of `filling` may take `b`. Not one that is held
 * or kept for a fill, nor a dirty one when `clean` asks for a clean one, or
 * whose last write failed, whose write the caller may not make
 * (`victim_may_be_written`), a block that names others and waits for one
 * (`way_to`), or an `after` whose device has a `first` that cannot be
 * written now. `cache_spin` is held, and `wb_lock` too unless `clean` is
 * set.
 */
static int victim_fit(const struct bbuf *b, block_device_t *filling, struct first_ask *ask,
                      int clean)
{
    if (b->holder != -1 || b == fill_keep) {
        return 0;
    }
    if (!b->dirty) {
        return 1;
    }
    if (clean || write_barred(b) || !victim_may_be_written(b->dev, filling)) {
        return 0;
    }
    if ((names_blocks(b) || ordered(b)) && way_to(b)) {
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
 * search stops once `*passed`, the buffers the caller's search has passed
 * over, reaches the count of buffers, which means every buffer was passed
 * over. It reads at most WALK_BATCH buffers in one call and then answers
 * NULL with `*more` set, for the caller to let `cache_spin` go and search
 * again, as Linux's LRU scan goes a batch at a time. A buffer let go
 * meanwhile may have been passed over and moved behind the search, fit
 * now, so a turn that ends with nothing taken is followed by another when
 * something was let go during it (`victim_any`). With `clean` set only a
 * clean buffer is taken. `cache_spin` is held, and `wb_lock`, which the
 * tests of a dirty buffer read under, unless `clean` is set.
 */
static struct bbuf *victim_find(block_device_t *filling, uint32_t *passed, int *more, int clean)
{
    struct first_ask ask = {0, 0};
    struct bbuf *b;
    int n = 0;

    *more = 0;
    while ((b = lru_head) != 0 && *passed < nbufs) {
        if (n++ == WALK_BATCH) {
            *more = 1;
            return 0;
        }
        stat_visits++;
        if (victim_fit(b, filling, &ask, clean)) {
            return b;
        }
        lru_del(b);
        lru_add_tail(b);
        (*passed)++;
    }
    return 0;
}

/* How many times a victim search retries a turn in which no buffer went fit
 * before a miss looks for a holder to wait for. Linux's allocator counts
 * only reclaim loops that made no progress, setting the count to 0 after one
 * that made some, and retries MAX_RECLAIM_RETRIES (16) of them
 * (mm/internal.h:460-464, mm/page_alloc.c:4179-4185). */
#define SEARCH_TURNS 16

/* A miss's search for a victim, kept while it lets `cache_spin` go between
 * batches. A turn begins with `passed` 0. */
struct victim_search {
    uint32_t passed; /* the buffers this turn has passed over */
    uint32_t begun;  /* `fit_gos` when this turn began */
    int idle;        /* the turns in a row in which no buffer went fit */
    int ended;       /* a turn has just ended: the caller gives way */
    int clean;       /* it takes a clean buffer alone, without `wb_lock` */
};

/**
 * The victim a miss takes under `wb_lock`, or with interrupts off without
 * it: an empty buffer, a new one while the cache is under its limit, or the
 * least recently released one `filling` may take (`victim_find`), clean
 * when `s->clean` says so, with `*more` set and NULL when the search lets
 * `cache_spin` go before its next batch.
 *
 * A turn that passes over every buffer is progress when a buffer a miss
 * may take was let go during it (`fit_gos`), perhaps behind the turn, which
 * the next turn reads: a clean one, or a dirty one whose write is not
 * barred, which the next turn writes and takes. Linux's
 * `should_reclaim_retry` sets `no_progress_loops` to 0 after a reclaim
 * that made progress (mm/page_alloc.c:4179-4180), and retries while the
 * reclaimable pages, its dirty file pages among them, could meet the
 * watermark (mm/page_alloc.c:4206-4219); its getblk never fails
 * (`__getblk_slow`, fs/buffer.c:1131-1140). A release of a buffer whose
 * write is barred is not progress, since no search takes it. A turn in
 * which no such buffer was let go is counted, and is retried SEARCH_TURNS
 * times in a row before the search ends. A miss then waits for a dirty
 * buffer another caller holds when the hold order permits (`victim_busy`):
 * scans without a release do not prove that outstanding I/O cannot finish.
 * Linux's __bread_gfp uses __GFP_NOFAIL for its buffer allocation
 * (fs/buffer.c), rather than giving up on a count of scans.
 *
 * A buffer let go at every turn and taken again before the search reaches
 * it keeps the search going, so every turn ends with `s->ended` set, for
 * `get` to give way before the next one, as `should_reclaim_retry` calls
 * cond_resched before each retry (mm/page_alloc.c:4226-4233): what could
 * make a buffer fit, a write or a sync, then runs, and the search holds
 * `wb_lock` for one turn at a time.
 * `cache_spin` is held.
 */
static struct bbuf *victim_any(block_device_t *filling, struct victim_search *s, int *more)
{
    struct bbuf *b = take_empty();

    *more = 0;
    if (!b && nbufs < bcache_limit()) {
        b = buffer_new();
    }
    if (b) {
        return b;
    }
    if (s->passed == 0) {
        s->begun = fit_gos;
    }
    b = victim_find(filling, &s->passed, more, s->clean);
    if (b || *more || s->passed == 0) {
        return b;
    }
    if (fit_gos != s->begun) {
        s->idle = 0;
    } else if (++s->idle > SEARCH_TURNS) {
        return 0;
    }
    s->passed = 0;
    s->ended = 1;
    *more = 1;
    return 0;
}

/**
 * A dirty buffer another caller holds whose release can let reclaim
 * proceed, or NULL. Failed writes and devices this miss may not write are
 * left. The buffer must follow every buffer the caller holds in the hold
 * order, so waiting cannot make a cycle with its holder. The caller holds
 * `wb_lock` and `cache_spin`, taken with `*flags`; the walk gives the
 * spinlock up every WALK_BATCH buffers. A returned buffer stays held under
 * that lock until the caller files its wait with `wait_unheld`.
 */
static struct bbuf *victim_busy(block_device_t *filling, uint32_t *flags)
{
    struct bbuf *b;
    const struct bbuf *held;
    uint32_t n = 0;

    for (b = bufs; b; b = bufs_next(b, &n, flags)) {
        stat_visits++;
        if (!b->dirty || b->holder == -1 || b->holder == caller_pid() || write_barred(b) ||
            !victim_may_be_written(b->dev, filling)) {
            continue;
        }
        for (held = *held_list(); held; held = held->hold_next) {
            stat_visits++;
            if (held->dev &&
                (held->dev == b->dev ? held->blk >= b->blk : !block_is_below(b->dev, held->dev))) {
                break;
            }
        }
        if (!held) {
            return b;
        }
    }
    return 0;
}

/**
 * Retry an exhausted search `s` after a dirty holder's release, or a
 * release during the look for one. The caller holds `cache_spin`, taken
 * with `*flags`, and `wb_lock` unless `s->clean` forbids sleeping. Returns
 * 1 with the spinlock given up and the search reset, or 0 with it still
 * held and no holder to wait for. The write lock is given up for a wait.
 */
static int victim_retry(block_device_t *filling, struct victim_search *s, uint32_t *flags)
{
    uint32_t seen = fit_gos;
    struct bbuf *busy = s->clean ? 0 : victim_busy(filling, flags);

    if (busy) {
        stat_waits++;
        wait_unheld(busy, *flags);
    } else if (fit_gos != seen) {
        cache_unlock(*flags);
    } else {
        return 0;
    }
    s->passed = 0;
    s->idle = 0;
    return 1;
}

/** Let `cache_spin`, taken with `flags`, go between two batches of a victim
 * search, and at a turn's end give way (`victim_any`): pass `wb_lock`, and
 * the big lock where the caller holds it, to a processor waiting for them
 * and take the switch the tick asked for, unless the caller runs with
 * interrupts off, as a fault on a mapping inside a call with interrupts off
 * reads its block in place (`filemap_read`), and must not sleep. Linux's
 * cond_resched does nothing then (`__cond_resched`,
 * kernel/sched/core.c:7287-7292). */
static void search_pause(struct victim_search *s, uint32_t flags)
{
    int held;

    cache_unlock(flags);
    if (s->ended) {
        s->ended = 0;
        held = wb_out();
        sched_give_way();
        wb_back(held);
    }
}

/* The buffers at the head of the release order a miss reads for a clean
 * victim, before it takes `wb_lock` for the whole search. */
#define CLEAN_LOOK 32

/**
 * A victim a miss may take without `wb_lock`: an empty buffer, a new one
 * while the cache is under its limit, or, with `look` set, the least
 * recently released clean buffer among the first CLEAN_LOOK of the release
 * order that nobody holds and no fill keeps; NULL otherwise, for the search
 * under `wb_lock`, which writes a dirty one. A buffer nobody holds stays
 * clean while `cache_spin` is held, since only a holder marks one dirty.
 * `cache_spin` is held.
 */
static struct bbuf *victim_clean(int look)
{
    struct bbuf *b = take_empty();
    int n = 0;

    if (!b && nbufs < bcache_limit()) {
        b = buffer_new();
    }
    if (b || !look) {
        return b;
    }
    for (b = lru_head; b && n < CLEAN_LOOK; b = b->lru_next, n++) {
        stat_visits++;
        if (b->holder == -1 && b != fill_keep && !b->dirty) {
            return b;
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
        if (b->dev == dev && (b->holder != -1 || write_barred(b) ||
                              ((names_blocks(b) || ordered(b)) && way_to(b)))) {
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
            ((names_blocks(b) || ordered(b)) && way_to(b))) {
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
    uint32_t flags = cache_lock();
    int cached = index_find(dev, blk) != 0;

    cache_unlock(flags);
    return cached;
}

void bcache_forget(block_device_t *dev, uint64_t blk)
{
    uint32_t flags = cache_lock();
    struct bbuf *b = index_find(dev, blk);

    if (b && b->holder == -1 && !b->dirty) {
        make_empty(b);
    }
    cache_unlock(flags);
}

int bcache_state(block_device_t *dev, uint64_t blk)
{
    uint32_t flags = cache_lock();
    struct bbuf *b = index_find(dev, blk);
    int state = b == 0 ? 0 : b->dirty ? 2 : 1;

    cache_unlock(flags);
    return state;
}

/* The place changes under `wb_lock` alone (`bcache_dirty_ordered`), but only
 * by a mark of the caller's, who alone dirties the block, and the write
 * that cleans it clears it under both locks, so `cache_spin` is enough. */
int bcache_order(block_device_t *dev, uint64_t blk)
{
    uint32_t flags = cache_lock();
    struct bbuf *b = index_find(dev, blk);
    int order = b != 0 && b->dirty ? b->order : -1;

    cache_unlock(flags);
    return order;
}

int bcache_copy_cached(block_device_t *dev, uint64_t blk, uint32_t offset, void *out,
                       uint32_t count)
{
    uint32_t flags = cache_lock();
    struct bbuf *b = index_find(dev, blk);
    int copied = 0;

    if (b && !b->discard && b->holder == -1 && offset <= b->bytes && count <= b->bytes - offset) {
        k_memcpy(out, b->data + offset, count);
        copied = 1;
    }
    cache_unlock(flags);
    return copied;
}

int bcache_busy(block_device_t *dev, uint64_t blk)
{
    uint32_t flags = cache_lock();
    struct bbuf *b = index_find(dev, blk);
    int busy = b != 0 && b->holder != -1;

    cache_unlock(flags);
    return busy;
}

/** Sleep until nobody holds `dev`'s block `blk`, for `bcache_wait_unheld`
 * and, `killable`, for `bcache_wait_unheld_killable`: 0, or -EINTR once
 * SIGKILL is pending in a killable wait. */
static int wait_block_unheld(block_device_t *dev, uint64_t blk, int killable)
{
    for (;;) {
        uint32_t flags = cache_lock();
        struct bbuf *b = index_find(dev, blk);

        if (!b || b->holder == -1) {
            cache_unlock(flags);
            return 0;
        }
        if (killable && caller_killed()) {
            cache_unlock(flags);
            return -EINTR;
        }
        stat_waits++;
        wait_unheld_how(b, flags, killable);
    }
}

void bcache_wait_unheld(block_device_t *dev, uint64_t blk)
{
    (void)wait_block_unheld(dev, blk, 0);
}

int bcache_wait_unheld_killable(block_device_t *dev, uint64_t blk)
{
    return wait_block_unheld(dev, blk, 1);
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
 * Key clean buffer `b` to block `blk` of `dev` and hold it, under
 * `cache_spin`; its bytes are not yet the block's.
 */
static void key_held(struct bbuf *b, block_device_t *dev, uint64_t blk)
{
    index_del(b);
    b->dev = dev;
    b->blk = blk;
    index_add(b);
    if (!b->lru_prev && lru_head != b) {
        lru_add_tail(b);
    }
    set_clean(b);
    b->order = 0;
    b->discard = 0;
    hold(b);
    b->bytes = block_bytes(dev, blk);
}

/**
 * Fill the victim `b`, which the caller holds and has just keyed to block
 * `blk` of `dev`, with zeros when `blank` is set and otherwise from the
 * device. The caller holds no lock of the cache's, since the read sleeps.
 * Returns `b`, or NULL when the read failed, with `b` let go and emptied.
 */
static struct bbuf *fill_victim(block_device_t *dev, uint64_t blk, struct bbuf *b, int blank)
{
    uint64_t cpus = 0;
    uint32_t flags;

    if (blank) {
        k_memset(b->data, 0, b->bytes);
        return b;
    }
    if (device_read(dev, blk * BCACHE_BLOCK_SECTORS, b->bytes / BLOCK_SECTOR_SIZE, b->data) != 0) {
        flags = cache_lock();
        unhold_wake(b, &cpus);
        make_empty(b);
        cache_unlock(flags);
        sched_kick_cpus(cpus);
        return 0;
    }
    flags = cache_lock();
    stat_fills++;
    ahead_dev = dev;
    ahead_next = blk + 1;
    cache_unlock(flags);
    return b;
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
 *
 * A hit and a miss that finds a clean victim near the head of the release
 * order (`victim_clean`) take `cache_spin` alone, so a caller reads a cached
 * block, and fills one that the cache can make room for with no write, with
 * no other lock (`device_read`), as Linux's `__getblk_slow`
 * (fs/buffer.c:1117) needs no global lock. A miss that finds no clean
 * victim there takes `wb_lock` for the search, whose writes of a dirty
 * victim keep the order the dirty marks give, and lets it go before the
 * read. A caller with the big lock goes to that search at once, as it
 * always has, past the empties and the room under the limit. Each turn of
 * the search ends by giving way, which lets `wb_lock` go to a processor
 * waiting for it (`search_pause`), as a sleep does. A caller with
 * interrupts off, which may not sleep for `wb_lock`, searches for a clean
 * buffer alone and fails when there is none, as Linux reads nothing in a
 * fault taken with page faults off (`faulthandler_disabled`,
 * arch/x86/mm/fault.c:1270-1273).
 */
static struct bbuf *get(block_device_t *dev, uint64_t blk, int blank, int *fresh)
{
    struct victim_search search = {0, 0, 0, 0, 0};
    struct bbuf *victim, *b;
    uint32_t flags;
    int wb = 0, filled = blank, more, grow;

    if (fresh) {
        *fresh = 0;
    }
    if (!dev || blk >= (dev->sector_count + BCACHE_BLOCK_SECTORS - 1) / BCACHE_BLOCK_SECTORS) {
        return 0;
    }
    check_hold_rules(dev, blk);
    search.clean = !kirq_enabled();

again:
    flags = cache_lock();
    b = index_find(dev, blk);
    if (b) {
        if (b->holder != -1) {
            stat_waits++;
            wait_unheld(b, flags);
            goto again;
        }
        hold(b);
        stat_hits++;
        cache_unlock(flags);
        goto done;
    }
    if (!filled && dev == ahead_dev && blk == ahead_next) {
        cache_unlock(flags);
        filled = 1;
        if (wb) {
            wb_give();
            wb = 0;
        }
        (void)fill_run(dev, blk);
        goto again;
    }
    if (!wb && !search.clean) {
        victim = victim_clean(!bkl_held());
        if (!victim) {
            cache_unlock(flags);
            wb_take();
            wb = 1;
            goto again;
        }
    } else {
        victim = victim_any(dev, &search, &more);
        if (more) {
            search_pause(&search, flags);
            goto again;
        }
        if (!victim && victim_retry(dev, &search, &flags)) {
            goto again;
        }
        if (!victim) {
            cache_unlock(flags);
            klog(KLOG_WARN, "bcache", "get: no free buffer");
            goto done;
        }
        if (victim->dirty) {
            cache_unlock(flags);
            if (victim->order != BCACHE_ORDER_AFTER || write_firsts(victim->dev) == 0) {
                (void)write_one(victim, reclaim_flags(victim));
            }
            search.passed = 0;
            search.idle = 0;
            goto again;
        }
    }
    key_held(victim, dev, blk);
    grow = index_due();
    cache_unlock(flags);
    if (wb) {
        wb_give();
        wb = 0;
    }
    if (grow) {
        index_fit();
    }
    if (fresh) {
        *fresh = 1;
    }
    b = fill_victim(dev, blk, victim, blank);
done:
    if (wb) {
        wb_give();
    }
    return b;
}

/**
 * Fill block `blk` of `dev` and the whole blocks after it that are not in
 * the cache, as many as one request moves up to 128 KiB, from one read into
 * the staging buffer: a file read in order then costs one request per run
 * and not one per page. The run is Linux's readahead window at its most,
 * which is also Linux's first window for a read of more than 32 KiB
 * (`get_init_ra_size`, mm/readahead.c:361-372); the cache does not see the
 * size of the read that missed, only its pages. A longer run reads what a
 * reader that wanders through a large file never asks for: a mebibyte for
 * each two blocks it happens to take in order. Returns 1 when `blk` is in
 * the cache afterwards, 0 when the run could not be read and the block is
 * to be filled on its own. Each block is taken with the blank get and
 * released at once, so the hold rules see one buffer at a time. The blank
 * get sleeps when it writes a dirty victim, and a block another process
 * filled meanwhile comes back from it keyed already, with bytes newer than
 * the staged ones: only a buffer the get keyed fresh takes the copy. A
 * write that reached any device since the staged read, a dirty buffer of
 * the run evicted by one of these gets included, may have made the staged
 * bytes of a block older than the disk's, and no block is filled from them
 * after one: the fill stops, and a buffer keyed fresh for it is emptied
 * again. A run is at most half the cache's limit, so that its last blocks
 * cannot evict its first, and the caller's block, the first, is no victim
 * until every block is in; it is not held, since the caller may hold three
 * buffers already and a fourth would leave none for the run's own keying.
 * The staging buffer and the count of writes are taken under `cache_spin`,
 * so a fill runs with or without the big lock.
 */
static int fill_run(block_device_t *dev, uint64_t blk)
{
    uint32_t most = block_max_sectors(dev) / BCACHE_BLOCK_SECTORS;
    uint64_t whole = dev->sector_count / BCACHE_BLOCK_SECTORS;
    uint32_t n, i, gen, flags;
    struct bbuf *was, *kept;

    if (most > READ_RUN_BLOCKS) {
        most = READ_RUN_BLOCKS;
    }
    if (most > bcache_limit() / 2u) {
        most = bcache_limit() / 2u;
    }
    if (most <= 1 || blk >= whole) {
        return 0;
    }
    n = whole - blk < most ? (uint32_t)(whole - blk) : most;
    if (n <= 1) {
        return 0;
    }
    flags = cache_lock();
    if (!run_stage && !run_stage_busy) {
        run_stage = kalloc_pages_try(RUN_BLOCKS * BCACHE_BLOCK_BYTES / PAGE_SIZE, 0);
    }
    if (run_stage_busy || !run_stage) {
        cache_unlock(flags);
        return 0;
    }
    run_stage_busy = 1;
    gen = write_gen;
    cache_unlock(flags);
    if (device_read(dev, blk * BCACHE_BLOCK_SECTORS, n * BCACHE_BLOCK_SECTORS, run_stage) != 0) {
        stage_free();
        return 0;
    }
    for (i = 0; i < n; i++) {
        struct bbuf *b;
        int fresh;

        if (bcache_cached(dev, blk + i)) {
            continue;
        }
        b = get(dev, blk + i, 1, &fresh);
        if (!b) {
            break;
        }
        flags = cache_lock();
        if (fresh && write_gen != gen) {
            uint64_t cpus = 0;

            unhold_wake(b, &cpus);
            make_empty(b);
            cache_unlock(flags);
            sched_kick_cpus(cpus);
            break;
        }
        if (fresh) {
            stat_fills++;
        }
        if (i == 0) {
            fill_keep = b;
        }
        cache_unlock(flags);
        if (fresh) {
            k_memcpy(b->data, run_stage + i * BCACHE_BLOCK_BYTES, BCACHE_BLOCK_BYTES);
        }
        bcache_release(b);
    }
    flags = cache_lock();
    was = fill_keep;
    fill_keep = 0;
    kept = index_find(dev, blk);
    if (was && kept == was && kept->holder == -1 && !kept->dirty) {
        fit_gos++;
    }
    run_stage_busy = 0;
    ahead_dev = dev;
    ahead_next = blk + n;
    cache_unlock(flags);
    return kept != 0;
}

/* The stamp orders the buffers for the shrink; the release order is the
 * list. A discarded buffer is emptied, which drops a dirty one's mark and
 * so takes `wb_lock` first; while the caller holds the buffer only its own
 * mark makes it dirty, so a look under `cache_spin` that finds it clean
 * holds. */
void bcache_release(struct bbuf *b)
{
    uint64_t cpus = 0;
    uint32_t flags;
    int wb = 0;

    if (!b || b->holder != caller_pid()) {
        k_panic("bcache: hold rule: a release by a process that does not hold the buffer");
        return;
    }
again:
    flags = cache_lock();
    if (b->discard && b->dirty && !wb) {
        cache_unlock(flags);
        wb_take();
        wb = 1;
        goto again;
    }
    unhold_wake(b, &cpus);
    b->stamp = ++clock_stamp;
    if (b->discard) {
        make_empty(b);
    } else if (b->dev) {
        lru_del(b);
        lru_add_tail(b);
    }
    cache_unlock(flags);
    if (wb) {
        wb_give();
    }
    sched_kick_cpus(cpus);
}

/**
 * Mark the held buffer `b` dirty, and with `tag` set give it `owner` and
 * `level` (`bcache_dirty_owned`, `bcache_dirty_pointer`), in one step under
 * `cache_spin` with `wb_lock` taken, which the caller then holds and gives
 * back. The flusher is woken by the first dirty buffer of a clean cache,
 * so that its readings of the marks start then, and past the background
 * threshold, as Linux's `balance_dirty_pages` starts background writeback.
 * Returns 1, or panics when the caller does not hold `b`.
 */
static int mark(struct bbuf *b, int tag, uint64_t owner, int level)
{
    uint32_t flags;
    int first;

    if (!b || b->holder != caller_pid()) {
        k_panic("bcache: hold rule: a dirty mark by a process that does not hold the buffer");
        return 0;
    }
    wb_take();
    flags = cache_lock();
    first = ndirty == 0;
    set_dirty(b);
    first = first && ndirty > 0;
    if (tag) {
        b->owner = owner;
        b->level = level;
        b->entry = 0;
    }
    cache_unlock(flags);
    if (flusher_kick && (first || bcache_dirty_level() > 0)) {
        flusher_kick();
    }
    return 1;
}

void bcache_dirty(struct bbuf *b)
{
    if (mark(b, 0, 0, 0)) {
        wb_give();
    }
}

/* The lists and the places are `wb_lock`'s, and nothing a holder of
 * `cache_spin` alone reads changes with the class, so a move to the other
 * list, whose walk may be long, needs no `cache_spin`. */
void bcache_dirty_ordered(struct bbuf *b, int order)
{
    if (!mark(b, 0, 0, 0)) {
        return;
    }
    if ((b->order == BCACHE_ORDER_FIRST) == (order == BCACHE_ORDER_FIRST)) {
        b->order = order;
    } else {
        dirty_unlink(b);
        b->order = order;
        dirty_link(b);
    }
    wb_give();
}

void bcache_dirty_owned(struct bbuf *b, uint64_t owner)
{
    if (mark(b, 1, owner, 0)) {
        wb_give();
    }
}

void bcache_dirty_pointer(struct bbuf *b, uint64_t owner, int level)
{
    if (mark(b, 1, owner, level)) {
        wb_give();
    }
}

/* The mark is `wb_lock`'s, which `mark` takes, and a holder of
 * `cache_spin` alone does not read it. */
void bcache_dirty_entry(struct bbuf *b)
{
    if (!mark(b, 0, 0, 0)) {
        return;
    }
    b->entry = 1;
    wb_give();
}

/**
 * Whether the wait on `t`, keyed to block `blk` of `dev`, as it stood at
 * the mark `k`, is not met (`wait_way`). `wb_lock` is held.
 */
static int order_open(struct bbuf *t, block_device_t *dev, uint64_t blk, uint64_t k)
{
    struct bcache_wait w;

    w.t = t;
    w.dev = dev;
    w.blk = blk;
    w.k = k;
    return wait_way(&w) != 0;
}

/*
 * The order is kept by the caller's process, with a count in the waiters of
 * the block's buffer, so that a shrink does not give it back to the heap
 * while the order names it; a buffer keyed to another block meanwhile was
 * written, which the waits tell by its key (`wait_way`). A second order on
 * one block takes the later mark.
 */
int bcache_order_after(block_device_t *dev, uint64_t blk)
{
    struct process *p = current_proc;
    struct bcache_token *tk;
    struct bbuf *t;
    uint32_t flags, i;
    int rc = 0;

    if (!p) {
        return -ENOSPC;
    }
    wb_take();
    flags = cache_lock();
    t = index_find(dev, blk);
    if (t && order_open(t, dev, blk, marks)) {
        for (i = 0; i < p->bcache_ntokens; i++) {
            tk = &p->bcache_tokens[i];
            if (tk->t == t && tk->dev == dev && tk->blk == blk) {
                tk->k = marks;
                break;
            }
        }
        if (i < p->bcache_ntokens) {
        } else if (p->bcache_ntokens == BCACHE_TOKENS) {
            rc = -ENOSPC;
        } else {
            tk = &p->bcache_tokens[p->bcache_ntokens++];
            tk->t = t;
            tk->dev = dev;
            tk->blk = blk;
            tk->k = marks;
            t->waiters++;
        }
    }
    cache_unlock(flags);
    wb_give();
    return rc;
}

void bcache_order_end(void)
{
    struct process *p = current_proc;
    uint32_t flags, i;

    if (!p || p->bcache_ntokens == 0) {
        return;
    }
    flags = cache_lock();
    for (i = 0; i < p->bcache_ntokens; i++) {
        p->bcache_tokens[i].t->waiters--;
    }
    p->bcache_ntokens = 0;
    cache_unlock(flags);
}

/**
 * Keep the bytes of the held, dirty `b` in an image (`version`), before a
 * change, into `v`, a buffer from the heap with its page: the image takes
 * `b`'s marks, class, owner, level and waits, and goes between `b` and its
 * older images; `b` keeps its bytes and goes dirty again at a new mark,
 * with no waits and nothing needed of it. `cache_spin` and `wb_lock` are
 * held.
 */
static void version_keep(struct bbuf *b, struct bbuf *v)
{
    k_memcpy(v->data, b->data, b->bytes);
    v->dev = b->dev;
    v->blk = b->blk;
    v->bytes = b->bytes;
    v->holder = -1;
    v->version = 1;
    v->dirty = 1;
    v->order = b->order;
    v->owner = b->owner;
    v->level = b->level;
    v->entry = b->entry;
    v->dirtied = b->dirtied;
    v->touched = b->touched;
    v->waits = b->waits;
    v->older = b->older;
    if (v->older) {
        v->older->newer = v;
    }
    v->newer = b;
    b->older = v;
    b->waits = 0;
    b->need = 0;
    ndirty++;
    dirty_link(v);
    dirty_unlink(b);
    b->dirtied = b->touched = ++marks;
    dirty_link(b);
}

/** Room in `b`'s waits for `more` more, from `spare`, an array of `cap`
 * entries the caller took from the heap, or NULL: the waits move into it
 * when it is needed. Returns whether `spare` was taken. `cache_spin` and
 * `wb_lock` are held. */
static int waits_room(struct bbuf *b, uint32_t more, struct bcache_waits *spare, uint32_t cap)
{
    uint32_t n = b->waits ? b->waits->n : 0;

    if (b->waits && n + more <= b->waits->cap) {
        return 0;
    }
    spare->cap = cap;
    spare->n = n;
    if (n) {
        k_memcpy(spare->w, b->waits->w, n * sizeof(spare->w[0]));
    }
    b->waits = spare;
    return 1;
}

/** Add the wait on `t`, keyed to block `blk` of `dev`, as it stood at mark
 * `k`, to `b`, which has room, or take the later mark for one it has, and
 * mark in `t` the bytes it needs when they are the newest, `t`'s own and
 * no image's. `cache_spin` and `wb_lock` are held. */
static void wait_add(struct bbuf *b, struct bbuf *t, block_device_t *dev, uint64_t blk, uint64_t k)
{
    struct bcache_wait *w;
    uint32_t i;

    for (i = 0; i < b->waits->n; i++) {
        w = &b->waits->w[i];
        if (w->t == t && w->dev == dev && w->blk == blk) {
            if (w->k < k) {
                w->k = k;
            }
            break;
        }
    }
    if (i == b->waits->n) {
        w = &b->waits->w[b->waits->n++];
        w->t = t;
        w->dev = dev;
        w->blk = blk;
        w->k = k;
        t->waiters++;
    }
    if (t->dirty && t->dirtied <= k && t->need < k) {
        t->need = k;
    }
}

/** Read the caller's orders `p` for a change of `b`: whether one on `b`'s
 * device is not met, in `*self` the latest mark of those on `b`'s own block
 * and in `*more` the count of those on others. `wb_lock` is held. */
static int change_scan(const struct process *p, const struct bbuf *b, uint64_t *self,
                       uint32_t *more)
{
    uint32_t i;
    int open = 0;

    *self = 0;
    *more = 0;
    for (i = 0; i < p->bcache_ntokens; i++) {
        const struct bcache_token *tk = &p->bcache_tokens[i];

        if (tk->dev != b->dev || !order_open(tk->t, b->dev, tk->blk, tk->k)) {
            continue;
        }
        open = 1;
        if (tk->blk == b->blk) {
            *self = tk->k > *self ? tk->k : *self;
        } else {
            (*more)++;
        }
    }
    return open;
}

/** Take from the heap what a change of `b` needs: with `keep`, an image's
 * buffer and page in `*v`, and when `b`'s waits have no room for `more`
 * after it, a larger array in `*spare`, of `*cap` entries. 0, or -ENOMEM
 * with nothing taken. `wb_lock` is held, and nothing else of the cache. */
static int change_alloc(const struct bbuf *b, int keep, uint32_t more, struct bbuf **v,
                        struct bcache_waits **spare, uint32_t *cap)
{
    *v = 0;
    *spare = 0;
    *cap = 0;
    if (keep) {
        *v = kzalloc(sizeof(**v));
        if (*v) {
            (*v)->data = kalloc_pages_try(1, 0);
        }
        if (!*v || !(*v)->data) {
            kfree(*v);
            *v = 0;
            return -ENOMEM;
        }
        wait_queue_init(&(*v)->wq);
    }
    if (more && (keep || !b->waits || b->waits->n + more > b->waits->cap)) {
        *cap = (keep || !b->waits ? 0 : b->waits->n) + more;
        *cap = *cap < 4 ? 4 : *cap * 2;
        *spare = kmalloc(sizeof(**spare) + (*cap - 1) * sizeof((*spare)->w[0]));
        if (!*spare) {
            if (*v) {
                kfree_pages((*v)->data, 1);
                kfree(*v);
                *v = 0;
            }
            return -ENOMEM;
        }
    }
    return 0;
}

/** Make the change of `b` that `change_scan` and `change_alloc` prepared:
 * the image `v`, when not NULL, kept (`version_keep`), and the caller's
 * orders `p` on other blocks not yet met added to `b`'s waits, in `spare`
 * when they need it. Returns the array the heap takes back: `b`'s old one
 * when `spare` replaced it, else `spare`, or NULL. `cache_spin` and
 * `wb_lock` are held. */
static struct bcache_waits *change_apply(const struct process *p, struct bbuf *b, struct bbuf *v,
                                         struct bcache_waits *spare, uint32_t cap, uint32_t more)
{
    struct bcache_waits *old;
    uint32_t i;

    if (v) {
        version_keep(b, v);
    }
    if (!more) {
        return spare;
    }
    old = b->waits;
    if (spare && waits_room(b, more, spare, cap)) {
        spare = old;
    }
    for (i = 0; i < p->bcache_ntokens; i++) {
        const struct bcache_token *tk = &p->bcache_tokens[i];

        if (tk->dev == b->dev && tk->blk != b->blk && order_open(tk->t, b->dev, tk->blk, tk->k)) {
            wait_add(b, tk->t, b->dev, tk->blk, tk->k);
        }
    }
    return spare;
}

/*
 * The orders the caller keeps on other blocks not yet met become waits of
 * `b`; one on `b`'s own block is met by the order of its images. The image
 * is taken when an order waits for the bytes `b` has now, another's
 * (`need`) or the caller's own, and the change carries an order, so that
 * the waits the change makes are the newest bytes' alone. What it needs
 * from the heap is taken first, under `wb_lock` alone, which nothing in
 * the decision changes without. A buffer marked to be dropped at its
 * release takes neither: its device is gone or given up, and its images
 * were dropped with its mark (`versions_drop`).
 */
int bcache_change(struct bbuf *b)
{
    struct process *p = current_proc;
    struct bcache_waits *spare;
    struct bbuf *v;
    uint64_t self;
    uint32_t flags, more, cap;
    int keep, rc;

    if (!b || b->holder != caller_pid()) {
        k_panic("bcache: hold rule: a change by a process that does not hold the buffer");
        return -EIO;
    }
    if (!p || p->bcache_ntokens == 0) {
        return 0;
    }
    wb_take();
    if (b->discard || !change_scan(p, b, &self, &more)) {
        wb_give();
        return 0;
    }
    keep = b->dirty && (b->need > self ? b->need : self) >= b->dirtied;
    rc = change_alloc(b, keep, more, &v, &spare, &cap);
    if (rc != 0) {
        wb_give();
        return rc;
    }
    flags = cache_lock();
    spare = change_apply(p, b, v, spare, cap, more);
    cache_unlock(flags);
    wb_give();
    kfree(spare);
    return 0;
}

/** The two writes through, `bcache_write` and `bcache_write_unflushed`: a
 * held buffer, with the request `flags`, under `wb_lock`, which the request
 * gives up. */
static int write_held(struct bbuf *b, uint32_t flags)
{
    uint32_t spin;
    int rc;

    if (!b || b->holder != caller_pid()) {
        k_panic("bcache: hold rule: a write by a process that does not hold the buffer");
        return -EIO;
    }
    wb_take();
    if (b->dev->flags & BLOCK_WRITE_FAILED) {
        wb_give();
        return -EROFS;
    }
    spin = cache_lock();
    set_dirty(b);
    cache_unlock(spin);
    rc = write_buffer_flags(b, flags);
    wb_give();
    return rc;
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
/* The blocks of a large batch sorted between service points; a power of 2. */
#define SYNC_SORT_STEP   4096

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
 *
 * Sorting a batch of 100,000 blocks is tens of milliseconds, so it passes
 * service points, with `wb_lock` given up at each: the numbers in `out` are
 * the caller's alone, and the writes that follow them sleep in any case.
 * The walk of the dirty lists has none, since a list may change under a
 * saved link. `wb_lock` is held.
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
        if ((i & (SYNC_SORT_STEP - 1)) == 0) {
            int held = wb_out();

            sched_service_point();
            wb_back(held);
        }
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
 * buffer. A block written, emptied or keyed again
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
        /* The sort passed service points, where a `first` may have been
         * dirtied behind the choice above. */
        if (scope->hold && !scope->hold_afters && first_dirty(dev)) {
            continue;
        }
        if (n == 0) {
            if (from == 0) {
                return 0;
            }
            from = 0;
            continue;
        }
        for (i = 0; i < n; i++) {
            uint32_t flags = cache_lock();
            struct bbuf *next = index_find(dev, batch[i]);
            uint64_t last = 0;
            int rc, image = 0;

            if (next) {
                struct bbuf *v = next;

                while (v->older) {
                    v = v->older;
                }
                while (v != next && !sync_wants(v, pass, scope)) {
                    v = v->newer;
                }
                image = v != next;
                next = v;
            }
            if (!next || !sync_wants(next, pass, scope) || next->blk < from) {
                cache_unlock(flags);
                continue;
            }
            if (next->holder != -1 && next->holder != caller_pid()) {
                wait_unheld(next, flags);
                rc = 1;
            } else {
                cache_unlock(flags);
                check_sync_holds(next, dev);
                rc = write_run(
                    next, &last,
                    next->order == BCACHE_ORDER_FIRST ? scope->first_flags : scope->flags, scope);
            }
            if (rc < 0) {
                return rc;
            }
            if (rc > 0 && scope->hold) {
                break;
            }
            if (rc > 0 || image) {
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

static int sync_device(block_device_t *dev, int durable, uint64_t since);

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
    return sync_device(dev, 1, 0);
}

int bcache_flush(block_device_t *dev)
{
    int rc = flush_firsts(dev);

    if (rc != 0) {
        klog(KLOG_WARN, "bcache", "flush: failed (%d)", rc);
        wb_take();
        write_failed_mark(dev, 0);
        wb_give();
        device_failed(dev, 0, rc);
    }
    return rc;
}

/* Set under both locks, since a release reads `last` under `cache_spin`
 * (`write_barred`) and the walks of the dirty lists under `wb_lock`, so
 * that no walk still runs the callbacks once the caller has cleared them. */
void bcache_sync_last(block_device_t *dev, int (*last)(void *arg, uint64_t blk),
                      int (*names)(void *arg, uint64_t blk, uint64_t owner), void *arg)
{
    uint32_t flags;

    wb_take();
    flags = cache_lock();
    dev->sync_last = last;
    dev->sync_names = names;
    dev->sync_last_arg = arg;
    cache_unlock(flags);
    wb_give();
}

int bcache_sync_unflushed(block_device_t *dev)
{
    return sync_device(dev, 0, 0);
}

/* A device whose write failed is left to a sync, which tries its blocks
 * again: the flusher's turns would otherwise try them every few seconds. */
int bcache_writeback(block_device_t *dev, uint64_t since)
{
    if (since == 0 || (dev->flags & (BLOCK_WRITE_FAILED | BLOCK_WRITES_HALTED))) {
        return 0;
    }
    return sync_device(dev, 0, since);
}

/** `bcache_sync`, durable with `durable` set, when a run of `first` buffers
 * is written with the disk's flush and the sync ends with one; without it
 * every write leaves the disk's cache to the caller's next flush, in the
 * order written. `since` bounds the blocks written to those that went dirty
 * at or before that count of marks, and 0 or a count not reached yet to
 * those dirty when the call began. */
static int sync_device(block_device_t *dev, int durable, uint64_t since)
{
    int through = (block_flags(dev) & BLOCK_WRITE_THROUGH) != 0;
    struct sync_scope scope;
    int rc, flushed;

    if (dev->flags & BLOCK_WRITES_HALTED) {
        return -EIO;
    }
    k_memset(&scope, 0, sizeof(scope));
    wb_take();
    scope.since = since && since < marks ? since : marks;
    scope.flags = unflushed_flags(dev);
    scope.first_flags = durable ? 0 : scope.flags;
    scope.hold = dev->sync_last != 0;
    rc = sync_passes(dev, &scope);
    if (rc == 0 && scope.hold) {
        scope.hold = 0;
        rc = sync_passes(dev, &scope);
    }
    wb_give();
    if (!through || !durable) {
        return rc;
    }
    flushed = bcache_flush(dev);
    return rc != 0 ? rc : flushed;
}

/* Set under both locks, as the failed-write mark is: a release reads it
 * under `cache_spin` (`write_barred`), a sync under `wb_lock`. */
void bcache_halt_writes(block_device_t *dev)
{
    uint32_t flags;

    wb_take();
    flags = cache_lock();
    dev->flags |= BLOCK_WRITES_HALTED;
    cache_unlock(flags);
    wb_give();
}

/** Serialize the recovery interval with the failed-write mark and syncs. */
void bcache_recovering(block_device_t *dev, int active)
{
    uint32_t flags;

    wb_take();
    flags = cache_lock();
    if (active) {
        dev->flags |= BLOCK_RECOVERING;
        if (dev->flags & BLOCK_WRITE_FAILED) {
            dev->flags |= BLOCK_WRITES_HALTED;
        }
    } else {
        dev->flags &= ~BLOCK_RECOVERING;
    }
    cache_unlock(flags);
    wb_give();
}

/* A `first` keeps its flush here, unlike in `bcache_sync_unflushed`: nothing
 * says that the caller's flush comes before another caller's `after`, and a
 * file's sync is no reason to give up the order the marks promise. */
int bcache_sync_owned(block_device_t *dev, uint64_t owner)
{
    struct sync_scope scope;

    int rc;

    if (dev->flags & BLOCK_WRITES_HALTED) {
        return -EIO;
    }
    k_memset(&scope, 0, sizeof(scope));
    wb_take();
    scope.owner = owner;
    scope.since = marks;
    scope.flags = unflushed_flags(dev);
    rc = sync_passes(dev, &scope);
    wb_give();
    return rc;
}

/** `bcache_sync_block_ready` under `wb_lock`. What the block waits for is
 * written here one block a call, and not held: the caller looks at its own
 * state between the calls. */
static int sync_block_ready(block_device_t *dev, uint64_t blk)
{
    uint32_t flags = cache_lock();
    struct bbuf *b = index_find(dev, blk), *d;
    int rc;

    if (!b || !b->dirty) {
        cache_unlock(flags);
        return 0;
    }
    if (b->holder != -1 && b->holder != caller_pid()) {
        wait_unheld(b, flags);
        return 1;
    }
    cache_unlock(flags);
    d = way_to(b);
    if (!d) {
        return 0;
    }
    if (d->holder == caller_pid()) {
        return -EIO;
    }
    if (wait_other(d)) {
        return 1;
    }
    rc = write_one(d, d->order == BCACHE_ORDER_FIRST ? 0 : unflushed_flags(dev));
    return rc < 0 ? rc : 1;
}

int bcache_sync_block_ready(block_device_t *dev, uint64_t blk)
{
    int rc;

    wb_take();
    rc = sync_block_ready(dev, blk);
    wb_give();
    return rc;
}

/* A block that waits is held while what it waits for is written
 * (`write_follower`), so a steady writer cannot keep the call: what the
 * block waits for stops growing. A wait for another holder looks again. */
static int sync_block(block_device_t *dev, uint64_t blk, uint32_t quiet)
{
    int rc = 1;

    wb_take();
    while (rc > 0) {
        uint32_t flags = cache_lock();
        struct bbuf *b = index_find(dev, blk);

        if (!b || !b->dirty) {
            cache_unlock(flags);
            rc = 0;
            break;
        }
        if (b->holder != -1 && b->holder != caller_pid()) {
            wait_unheld(b, flags);
            continue;
        }
        cache_unlock(flags);
        check_sync_holds(b, dev);
        rc = write_one(b,
                       quiet || b->order != BCACHE_ORDER_FIRST ? quiet | unflushed_flags(dev) : 0);
    }
    wb_give();
    return rc;
}

int bcache_sync_block(block_device_t *dev, uint64_t blk)
{
    return sync_block(dev, blk, 0);
}

int bcache_sync_block_ordered(block_device_t *dev, uint64_t blk)
{
    return sync_block(dev, blk, WRITE_ORDER_ONLY);
}

/** Whether `b` keeps a wait, met or not, on block `blk` of its device;
 * `wb_lock` is held. */
static int waits_on(const struct bbuf *b, uint64_t blk)
{
    uint32_t i;

    for (i = 0; b->waits && i < b->waits->n; i++) {
        if (b->waits->w[i].blk == blk) {
            return 1;
        }
    }
    return 0;
}

/* A wait is let go only by its buffer's write (`set_clean`), so a dirty
 * buffer that keeps one on `blk` is one ordered after it, met or not. An
 * image's block is written whole, its newer images and the keyed buffer
 * with it. Each round gathers at most BCACHE_WAITING blocks and writes
 * them, and a full round is followed by another. Only a block dirty since
 * before the call began is gathered: each round's writes leave fewer of
 * them, so the rounds end under a steady writer. */
int bcache_sync_waiting(block_device_t *dev, uint64_t blk)
{
    uint64_t found[BCACHE_WAITING];
    const struct dirty_list *lists[2];
    const struct bbuf *b;
    uint64_t start = bcache_marks();
    int n, i, l, rc = 0;

    lists[0] = &firsts;
    lists[1] = &dirties;
    do {
        n = 0;
        wb_take();
        for (l = 0; l < 2; l++) {
            for (b = lists[l]->head; b && n < BCACHE_WAITING; b = b->dirty_next) {
                stat_visits++;
                if (b->dev != dev || b->dirtied > start || !waits_on(b, blk)) {
                    continue;
                }
                i = 0;
                while (i < n && found[i] != b->blk) {
                    i++;
                }
                if (i == n) {
                    found[n++] = b->blk;
                }
            }
        }
        wb_give();
        for (i = 0; rc == 0 && i < n; i++) {
            rc = sync_block(dev, found[i], 0);
        }
    } while (rc == 0 && n == BCACHE_WAITING);
    return rc;
}

/* The walk lets `cache_spin` go every WALK_BATCH buffers (`bufs_next`), as
 * Linux's invalidate goes a batch at a time (mm/truncate.c:509-541).
 * `wb_lock`, held throughout, keeps every buffer's dirty mark; a buffer a
 * reader takes meanwhile is dropped at its release, as one held when the
 * walk reaches it is, a dirty mark its holder makes waits for the walk's
 * end, and a miss for `dev` meanwhile fails its read and empties its
 * buffer. */
void bcache_device_gone(block_device_t *dev)
{
    struct bbuf *b, *reap = 0;
    uint32_t flags, n = 0;

    device_failed(dev, 0, -EIO);
    wb_take();
    flags = cache_lock();
    dev->flags |= BLOCK_GONE;
    for (b = bufs; b; b = bufs_next(b, &n, &flags)) {
        stat_visits++;
        if (b->dev != dev) {
            continue;
        }
        (void)versions_drop(b, &reap);
        set_clean(b);
        if (b->holder != -1) {
            b->discard = 1;
            continue;
        }
        make_empty(b);
    }
    cache_unlock(flags);
    wb_give();
    buffers_reap(&reap);
}

/* The walk lets `cache_spin` go every WALK_BATCH buffers (`bufs_next`),
 * so the count is of each buffer as the walk reaches it. */
int bcache_device_holds(block_device_t *dev)
{
    struct bbuf *b;
    uint32_t flags = cache_lock(), n = 0;
    int held = 0;

    for (b = bufs; b; b = bufs_next(b, &n, &flags)) {
        stat_visits++;
        if (b->dev == dev && b->holder != -1) {
            held++;
        }
    }
    cache_unlock(flags);
    return held;
}

/** The least recently released dirty buffer the shrink may write and
 * free, or NULL: unheld, unwaited for, and one the eviction too could
 * write. `cache_spin` is held, taken with `*flags`, and let go every
 * WALK_BATCH buffers (`walk_pause`); the walk ends where the buffer it
 * goes on from has left the release order meanwhile. `wb_lock` is
 * held. A returned buffer is kept in its waiters until `shrink_written`
 * gives that reference back on every outcome of the write. */
static struct bbuf *shrink_dirty(uint32_t *flags)
{
    struct bbuf *b;
    uint32_t n = 0;

    for (b = lru_head; b; b = b->lru_next) {
        if (++n % WALK_BATCH == 0) {
            *flags = walk_pause(b, 0, *flags);
            if (!on_lru(b)) {
                return 0;
            }
        }
        stat_visits++;
        if (!b->dirty || b->holder != -1 || b->waiters > 0) {
            continue;
        }
        if (write_barred(b) || !victim_may_be_written(b->dev, 0)) {
            continue;
        }
        if (((names_blocks(b) || ordered(b)) && way_to(b)) ||
            (b->order == BCACHE_ORDER_AFTER && firsts_pending(b->dev))) {
            continue;
        }
        b->waiters++;
        return b;
    }
    return 0;
}

/**
 * Drop the reference `shrink_dirty` kept in `b`'s waiters, and, if its
 * write succeeded (`wrote`), free it while still clean, keyed, unheld and
 * unwaited for. The reference keeps its address valid through the write's
 * release and this last look, even while another shrink frees clean
 * buffers. A miss may have reused it meanwhile, so every field is read
 * again under `cache_spin`. Returns 1 when freed. Called without the spinlock.
 */
static int shrink_written(struct bbuf *b, int wrote)
{
    struct bbuf *reap = 0;
    uint32_t flags = cache_lock();

    b->waiters--;
    if (!wrote || b->dirty || b->holder != -1 || b->waiters > 0 || !b->dev) {
        cache_unlock(flags);
        return 0;
    }
    buffer_unlink(b, &reap);
    cache_unlock(flags);
    buffers_reap(&reap);
    return 1;
}

/*
 * Three passes, each a walk of one list: the empties; the release order
 * for the clean buffers, oldest first, in one walk, since freeing a clean
 * buffer does not sleep; and with `can_write` the release order again for
 * the dirty ones, from its head after each write, which sleeps. A buffer a
 * process waits for is left, woken or not, since the waiter has its
 * address; so is one dirtied again or waited for while its write slept.
 * The writing pass has a budget, so a buffer dirtied again as fast as it is
 * written cannot keep the shrink here. A written buffer is freed only if,
 * under `cache_spin` again, it is still keyed, clean and nobody holds it or
 * waits for it (`shrink_written`). The writing pass holds `wb_lock`, which
 * its writes give up; the others need only `cache_spin`.
 *
 * The walks let `cache_spin` go every WALK_BATCH buffers, and the buffers
 * are given back to the heap with it let go (`walk_pause`,
 * `buffers_reap`), as Linux's LRU scan isolates a batch under `lru_lock`
 * and frees it after (mm/vmscan.c:1966-1985, 5729). The walk of the
 * empties goes on from the last buffer it passed over, kept meanwhile,
 * whose link ends the walk once a get took it off the empties; that of the
 * release order ends where the buffer it goes on from left the order.
 */
uint32_t bcache_shrink(uint32_t want, int can_write, uint32_t *written)
{
    struct bbuf **link = &empties;
    struct bbuf *b, *next, *at = 0, *reap = 0;
    uint32_t got = 0, flags, n = 0;
    int budget = (int)nbufs * 2;

    flags = cache_lock();
    while (got < want && *link) {
        if (++n % WALK_BATCH == 0) {
            flags = walk_pause(at, &reap, flags);
            link = at ? &at->empty_next : &empties;
            continue;
        }
        b = *link;
        stat_visits++;
        if (b->holder != -1 || b->waiters > 0) {
            at = b;
            link = &b->empty_next;
            continue;
        }
        *link = b->empty_next;
        b->empty_next = 0;
        nempties--;
        buffer_unlink(b, &reap);
        got++;
    }
    n = 0;
    for (b = lru_head; b && got < want; b = next) {
        next = b->lru_next;
        stat_visits++;
        if (!b->dirty && b->holder == -1 && b->waiters == 0) {
            buffer_unlink(b, &reap);
            got++;
        }
        if (++n % WALK_BATCH == 0 && next) {
            flags = walk_pause(next, &reap, flags);
            if (!on_lru(next)) {
                break;
            }
        }
    }
    cache_unlock(flags);
    buffers_reap(&reap);
    if (!can_write || got >= want) {
        return got;
    }
    wb_take();
    while (got < want && budget-- > 0) {
        flags = cache_lock();
        b = shrink_dirty(&flags);
        cache_unlock(flags);
        if (!b) {
            break;
        }
        if (b->order == BCACHE_ORDER_AFTER && write_firsts(b->dev) != 0) {
            (void)shrink_written(b, 0);
            continue;
        }
        if (shrink_written(b, write_one(b, reclaim_flags(b)) == 0)) {
            (*written)++;
            got++;
        }
    }
    wb_give();
    return got;
}

/**
 * Count, in `*ready`, the buffers the next misses for `dev` take without a
 * write, as far as `want`: the empties, the room under the limit, and the
 * clean buffers `victim_find` reaches first. Returns the first dirty buffer
 * in the way, which a miss would write, or NULL when `want` were found or
 * the release order has ended. The empties are their count, `nempties`,
 * and no walk. `cache_spin` is let go every WALK_BATCH buffers of the
 * release order (`walk_pause`), and the walk ends where the buffer it goes
 * on from has left the order meanwhile. The caller holds `wb_lock`.
 */
static struct bbuf *reserve_walk(block_device_t *dev, uint32_t want, uint32_t *ready)
{
    struct first_ask ask = {0, 0};
    uint32_t limit = bcache_limit(), flags = cache_lock();
    uint32_t have = nbufs < limit ? limit - nbufs : 0, n = 0;
    struct bbuf *b;

    if (have > want) {
        have = want;
    }
    have += nempties < want - have ? nempties : want - have;
    for (b = lru_head; b && have < want; b = b->lru_next) {
        if (++n % WALK_BATCH == 0) {
            flags = walk_pause(b, 0, flags);
            if (!on_lru(b)) {
                break;
            }
        }
        stat_visits++;
        if (!victim_fit(b, dev, &ask, 0)) {
            continue;
        }
        if (b->dirty) {
            *ready = have;
            cache_unlock(flags);
            return b;
        }
        have++;
    }
    *ready = have;
    cache_unlock(flags);
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

    wb_take();
    for (;;) {
        struct bbuf *b = reserve_walk(dev, want, &ready);
        uint64_t last;
        int rc;

        if (!b || budget-- <= 0) {
            break;
        }
        if (b->order == BCACHE_ORDER_AFTER) {
            rc = write_firsts(b->dev);
            if (rc < 0) {
                break;
            }
            if (rc > 0) {
                continue;
            }
        }
        rc = write_run(b, &last, b->order == BCACHE_ORDER_FIRST ? 0 : unflushed_flags(b->dev),
                       &reclaim);
        if (rc < 0) {
            break;
        }
    }
    wb_give();
    return ready;
}

/* The walk of `bcache_device_gone` without the device's loss: it lets
 * `cache_spin` go every WALK_BATCH buffers (`bufs_next`), and `wb_lock`,
 * held throughout, keeps every dirty mark, so a mark a holder makes waits
 * for the walk's end and the buffer is then dropped at its release. The
 * failed-write mark is cleared under `cache_spin`, as `bcache_invalidate`
 * clears it. */
int bcache_drop(block_device_t *dev)
{
    struct bbuf *b, *reap = 0;
    uint32_t flags, n = 0;
    int dropped = 0;

    wb_take();
    flags = cache_lock();
    for (b = bufs; b; b = bufs_next(b, &n, &flags)) {
        stat_visits++;
        if (b->dev != dev) {
            continue;
        }
        dropped += versions_drop(b, &reap);
        if (b->dirty) {
            dropped++;
            set_clean(b);
        }
        if (b->holder != -1) {
            b->discard = 1;
            continue;
        }
        make_empty(b);
    }
    dev->flags &= ~(BLOCK_WRITE_FAILED | BLOCK_WRITES_HALTED);
    cache_unlock(flags);
    wb_give();
    buffers_reap(&reap);
    return dropped;
}

/* Two walks of every buffer, the first to find one held or dirty and the
 * second to drop them, each letting `cache_spin` go every WALK_BATCH
 * buffers (`bufs_next`), as Linux's invalidate goes a batch at a time with
 * a `cond_resched` between (mm/truncate.c:509-541). A buffer of `dev` a
 * process takes while the second walk has let the lock go is dropped at
 * its release, as at a device's loss, and one dirtied meanwhile is left
 * for its write. The callers have stopped every use of `dev` before: an
 * unmount, a mount the file system refused (`mount_refused`), a loop
 * device's detach, and a disk whose partition table is set or read again
 * (`disk_set_table`, `disk_scan`). The failed-write mark is cleared under
 * `cache_spin`, as every change of it is made. */
int bcache_invalidate(block_device_t *dev)
{
    struct bbuf *b;
    uint32_t flags = cache_lock(), n = 0;

    for (b = bufs; b; b = bufs_next(b, &n, &flags)) {
        stat_visits++;
        if (b->dev == dev && (b->holder != -1 || b->dirty)) {
            cache_unlock(flags);
            return -EBUSY;
        }
    }
    for (b = bufs; b; b = bufs_next(b, &n, &flags)) {
        stat_visits++;
        if (b->dev != dev) {
            continue;
        }
        if (b->holder != -1) {
            b->discard = 1;
        } else if (!b->dirty) {
            make_empty(b);
        }
    }
    dev->flags &= ~(BLOCK_WRITE_FAILED | BLOCK_WRITES_HALTED);
    cache_unlock(flags);
    return 0;
}
