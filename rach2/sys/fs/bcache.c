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

static struct bbuf *bufs;     /* every buffer, newest first */
static uint32_t nbufs;        /* how many */
static struct bbuf *lru_head; /* the keyed buffers, least recently released first */
static struct bbuf *lru_tail;
static struct bbuf *empties;      /* the buffers keyed to nothing */
static struct bbuf *dirties;      /* the dirty buffers, in no order */
static uint32_t ndirty;           /* how many */
static struct bbuf *kernel_held;  /* held with no process current */
static block_device_t *ahead_dev; /* the device and block a sequential reader */
static uint64_t ahead_next;       /* reaches next; a miss there fills a run */
static uint32_t write_gen;        /* counts the writes that reached a device */
static uint32_t limit_set;        /* a limit given by `bcache_set_limit`, or 0 */
static uint32_t clock_stamp;
static uint64_t stat_hits, stat_fills, stat_waits;
/* Every buffer a walk of the cache's lists reads: the whole list, the release
 * order, the empties, a bucket of the index and a process's holds. It is what
 * a get, a victim's choice and a sync cost in bookkeeping, and a host test
 * holds it under a bound. */
static uint64_t stat_visits;

/* The most buffers the cache makes: half of what the caches together may
 * hold under pressure, and never fewer than the floor, since a file system
 * holds up to four at once and a write of one may take one more. Each
 * buffer is a page of the kernel heap, which is KVA_HEAP_SIZE whatever the
 * machine's memory, so the cache also stops at half the heap's pages: on a
 * machine with more than 2 GiB the memory share alone is more than the
 * whole heap, and a cache that took it all would leave fork with no page for
 * a stack. */
#define BCACHE_HEAP_SHARE ((KVA_HEAP_SIZE / PAGE_SIZE) / 2u)

static uint32_t bcache_limit(void)
{
    uint32_t n = limit_set ? limit_set : (uint32_t)(mem_cache_target() / 2u);

    if (n > BCACHE_HEAP_SHARE) {
        n = BCACHE_HEAP_SHARE;
    }
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

/* A buffer from the heap, empty and unheld, on the list; NULL when the
 * heap has no room for it. */
static struct bbuf *buffer_new(void)
{
    struct bbuf *b = kzalloc(sizeof(*b));

    if (!b) {
        return 0;
    }
    b->data = kalloc_pages(1, 0);
    if (!b->data) {
        kfree(b);
        return 0;
    }
    b->holder = -1;
    wait_queue_init(&b->wq);
    b->next = bufs;
    bufs = b;
    nbufs++;
    return b;
}

/* The index by device and block: a lookup walks one bucket and not the
 * whole list, which runs to thousands of buffers on a machine with memory.
 * A buffer is in the index from the moment it is keyed to a block until it
 * is emptied. */
#define BHASH 4096
static struct bbuf *bhash[BHASH];

static unsigned bucket_of(const block_device_t *dev, uint64_t blk)
{
    return (unsigned)(((uintptr_t)dev >> 4) ^ (uintptr_t)blk ^ (uintptr_t)(blk >> 12)) &
           (BHASH - 1);
}

static void index_add(struct bbuf *b)
{
    unsigned h = bucket_of(b->dev, b->blk);

    b->hnext = bhash[h];
    bhash[h] = b;
}

/* Take `b` out of the index before its device is cleared. */
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

/* The buffer keyed to `blk` of `dev`, or NULL. */
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
        return; /* not on the list */
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
 * The dirty list. A buffer is on it exactly while it is dirty, so a sync and
 * the ordered writes read the few buffers waiting to be written and not the
 * whole cache, which on a machine with memory is tens of thousands of
 * buffers; a sync walked the whole cache once for every metadata write.
 */

/** Mark `b` dirty and put it on the dirty list, where it is once. */
static void set_dirty(struct bbuf *b)
{
    if (b->dirty) {
        return;
    }
    b->dirty = 1;
    ndirty++;
    b->dirty_prev = 0;
    b->dirty_next = dirties;
    if (dirties) {
        dirties->dirty_prev = b;
    }
    dirties = b;
}

/** Mark `b` clean and take it off the dirty list. */
static void set_clean(struct bbuf *b)
{
    if (!b->dirty) {
        return;
    }
    b->dirty = 0;
    ndirty--;
    if (b->dirty_prev) {
        b->dirty_prev->dirty_next = b->dirty_next;
    } else {
        dirties = b->dirty_next;
    }
    if (b->dirty_next) {
        b->dirty_next->dirty_prev = b->dirty_prev;
    }
    b->dirty_prev = 0;
    b->dirty_next = 0;
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

/** An empty, unheld buffer off every list and back to the heap. */
static void buffer_free(struct bbuf *b)
{
    struct bbuf **link;

    for (link = &bufs; *link != b; link = &(*link)->next) {
        stat_visits++;
    }
    stat_visits++;
    *link = b->next;
    for (link = &empties; *link; link = &(*link)->empty_next) {
        stat_visits++;
        if (*link == b) {
            *link = b->empty_next;
            break;
        }
    }
    nbufs--;
    index_del(b);
    lru_del(b);
    kfree_pages(b->data, 1);
    kfree(b);
}

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

/* The rules that keep holders out of deadlock, checked before a wait or a
 * fill. A violation is a driver defect, not a runtime condition.
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
            continue; /* below it: its blocks are ordered among themselves */
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
        return 1; /* its write does not come back through here */
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

/* Wait predicate: nobody holds the buffer. */
static int busy_gone(void *arg)
{
    struct bbuf *b = arg;
    return b->holder == -1;
}

/* One buffer to the device, with the request `flags`; a failure is
 * recorded against the device and leaves the buffer dirty. */
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

/* One buffer to the device, on the medium when the write is answered. */
static int write_buffer(struct bbuf *b)
{
    return write_buffer_flags(b, 0);
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
 * Write `first` and the dirty, unheld blocks that follow it on its device
 * in one request, so that a file written back goes to the disk in runs and
 * not a page at a time. A run is as long as the device takes at once and at
 * most half the cache, as a read run is: its buffers are held across the
 * request's sleep and let go together, and a run that held every buffer
 * would leave a miss meanwhile with no buffer to take. A failure is recorded
 * on the first as `write_buffer` does. A run of one, or a device that takes
 * a page, is `write_buffer`. `*last` receives the last block written.
 */
static int write_run(struct bbuf *first, uint64_t *last, uint32_t flags)
{
    struct bbuf *run[RUN_BLOCKS];
    block_device_t *dev = first->dev;
    uint32_t most = block_max_sectors(dev) / BCACHE_BLOCK_SECTORS, bytes = 0;
    int n = 1, i, rc;

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
    if (run_stage_busy) {
        most = 1;
    }
    while ((uint32_t)n < most && run_stage && run[n - 1]->bytes == BCACHE_BLOCK_BYTES) {
        struct bbuf *b = index_find(dev, first->blk + (uint64_t)n);

        /* A run has one place in the order: a first and an after in one
         * request would reach the device together. */
        if (!b || !b->dirty || b->holder != -1 || b->order != first->order) {
            break;
        }
        run[n++] = b;
    }
    if (n == 1) {
        hold(first); /* held across the request's sleep */
        rc = write_buffer_flags(first, flags);
        unhold(first);
        wq_wake_all(&first->wq);
        *last = first->blk;
        return rc;
    }
    run_stage_busy = 1;
    for (i = 0; i < n; i++) {
        hold(run[i]);
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
        unhold(run[i]);
        wq_wake_all(&run[i]->wq);
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

/**
 * Whether one of `dev`'s dirty `first` buffers cannot be written now, held
 * or failed its last write. An `after` buffer of the device is then not a
 * victim, since its write would pass the first.
 */
static int firsts_pending(const block_device_t *dev)
{
    const struct bbuf *b;

    for (b = dirties; b; b = b->dirty_next) {
        stat_visits++;
        if (b->dev == dev && b->dirty && b->order == BCACHE_ORDER_FIRST &&
            (b->holder != -1 || b->write_failed)) {
            return 1;
        }
    }
    return 0;
}

/**
 * Write `dev`'s unheld dirty `first` buffers, each held across its write,
 * before an `after` buffer of the device is written. Returns 0 with nothing
 * to write, 1 when something was written, and the first failure's -errno,
 * which marks that buffer and leaves the rest. Every write sleeps, so the
 * scan starts over after each one.
 */
static int write_firsts(block_device_t *dev)
{
    struct bbuf *b;
    int wrote = 0;

again:
    for (b = dirties; b; b = b->dirty_next) {
        int rc;

        stat_visits++;
        if (b->dev != dev || !b->dirty || b->order != BCACHE_ORDER_FIRST || b->holder != -1 ||
            b->write_failed) {
            continue;
        }
        hold(b);
        rc = write_buffer(b);
        unhold(b);
        wq_wake_all(&b->wq);
        if (rc != 0) {
            return rc;
        }
        wrote = 1;
        goto again;
    }
    return wrote;
}

struct bbuf *bcache_get(block_device_t *dev, uint64_t blk)
{
    return get(dev, blk, 0, 0);
}

struct bbuf *bcache_get_blank(block_device_t *dev, uint64_t blk)
{
    return get(dev, blk, 1, 0);
}

/*
 * The two gets. A cached block is returned as it is either way; a miss is
 * filled from the device, or, for the blank get, zeroed without a read: a
 * caller about to write the whole block, or wanting zeros in it, would only
 * throw the device's bytes away, and on a stick the read costs as much as
 * the write that follows it. `*fresh`, when asked for, says whether the
 * buffer was keyed to the block by this call.
 *
 * A miss takes an empty buffer, or a new one while the cache is under its
 * limit, or else the least recently released unheld buffer this caller may
 * take, a dirty one written first. A dirty buffer whose last write failed, or
 * whose write would take a block this caller may not take while it holds
 * what it holds, is not a candidate. The waits and the writes sleep; the
 * lookup starts over after each, since another caller may have filled the
 * block meanwhile. NULL for a block past the device's end, a cache with no
 * buffer to give, or a failed read.
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
            b->waiters++; /* counted until this runs again, so reclaim leaves it */
            wq_wait_event_nointr(&b->wq, busy_gone, b);
            b->waiters--;
        }
        if (b->dev == dev && b->blk == blk) {
            hold(b);
            stat_hits++;
            return b;
        }
        goto again;
    }
    /* The block after the last one filled: a reader going through the
     * device in order, whose next blocks are read with this one. */
    if (!blank && dev == ahead_dev && blk == ahead_next && fill_run(dev, blk)) {
        goto again;
    }

    for (;;) {
        victim = take_empty();
        if (!victim && nbufs < bcache_limit()) {
            victim = buffer_new();
        }
        if (!victim) {
            for (b = lru_head; b; b = b->lru_next) {
                stat_visits++;
                if (b->holder != -1 || b == fill_keep) {
                    continue;
                }
                if (b->dirty && (b->write_failed || !victim_may_be_written(b->dev, dev))) {
                    continue;
                }
                if (b->dirty && b->order == BCACHE_ORDER_AFTER && firsts_pending(b->dev)) {
                    continue;
                }
                victim = b;
                break;
            }
        }
        if (!victim) {
            klog(KLOG_WARN, "bcache", "get: no free buffer");
            return 0;
        }
        if (!victim->dirty) {
            break;
        }
        if (victim->order == BCACHE_ORDER_AFTER && write_firsts(victim->dev) != 0) {
            /* The firsts were written, or one failed and is marked: either
             * way the scan starts over, since the writes slept. */
            goto again;
        }
        hold(victim); /* held for the write */
        if (write_buffer(victim) == 0) {
            unhold(victim);
            wq_wake_all(&victim->wq);
            /* The write slept: look again rather than key the block twice. */
            goto again;
        }
        unhold(victim);
        wq_wake_all(&victim->wq);
        /* The write failed and marked the buffer; the next scan leaves it. */
    }
    /* Keyed and held before the request sleeps, so a second process asking
     * for this block waits for this buffer instead of filling a second one. */
    index_del(victim);
    victim->dev = dev;
    victim->blk = blk;
    index_add(victim);
    if (!victim->lru_prev && lru_head != victim) {
        lru_add_tail(victim);
    }
    set_clean(victim);
    victim->order = 0;
    /* A mark left by a device that went while this buffer was held for a
     * fill or a writeback: every buffer changes hands here, so this is the
     * one place the mark must go. */
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
            fill_keep = b; /* the caller's block: no victim while the run is in */
        }
        bcache_release(b);
    }
    fill_keep = 0;
    run_stage_busy = 0;
    ahead_dev = dev;
    ahead_next = blk + n;
    return index_find(dev, blk) != 0;
}

void bcache_release(struct bbuf *b)
{
    if (!b || b->holder != caller_pid()) {
        k_panic("bcache: hold rule: a release by a process that does not hold the buffer");
        return;
    }
    unhold(b);
    b->stamp = ++clock_stamp;
    if (b->discard) {
        /* The device went while this was held; it may be freed once unheld. */
        make_empty(b);
    } else if (b->dev) {
        lru_del(b);
        lru_add_tail(b);
    }
    wq_wake_all(&b->wq);
}

/** Mark a held buffer dirty, for the writeback paths to reach the device. */
void bcache_dirty(struct bbuf *b)
{
    if (!b || b->holder != caller_pid()) {
        k_panic("bcache: hold rule: a dirty mark by a process that does not hold the buffer");
        return;
    }
    set_dirty(b);
}

/** Mark a held buffer dirty with its place in the device's order. */
void bcache_dirty_ordered(struct bbuf *b, int order)
{
    bcache_dirty(b);
    if (b) {
        b->order = order;
    }
}

/* The two writes through: a held buffer, with the request `flags`. */
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

/**
 * Write a held buffer through to the device now.
 *
 * After a failed write on the device this answers -EROFS without writing, so
 * an operation in progress when the device failed stops at its next metadata
 * write and the read-only transition is not written across.
 */
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

    for (b = dirties; b; b = b->dirty_next) {
        stat_visits++;
        if (b->dev == dev && b->dirty && b->order == BCACHE_ORDER_FIRST) {
            return b;
        }
    }
    return 0;
}

/* The most dirty blocks one scan of the cache gathers for a sync: a scan
 * per run written would read every buffer for every run. The batch is on
 * the stack, since a write through a loop device syncs the device under it
 * from inside this call. */
#define SYNC_BATCH 128

/**
 * The lowest `SYNC_BATCH` dirty blocks of `dev` above block `after` (all of
 * them when `started` is 0), the `first` ones alone in pass 0, in ascending
 * order in `out`. Returns how many. Block numbers and not buffers: a write
 * of the batch sleeps, and a buffer let go of meanwhile can be given to
 * another block or to the heap.
 */
static int gather_dirty(block_device_t *dev, int pass, int started, uint64_t after, uint64_t *out)
{
    const struct bbuf *b;
    int n = 0;

    for (b = dirties; b; b = b->dirty_next) {
        int i;

        stat_visits++;
        if (b->dev != dev || !b->dirty || (pass == 0 && b->order != BCACHE_ORDER_FIRST) ||
            (started && b->blk <= after)) {
            continue;
        }
        if (n == SYNC_BATCH) {
            if (b->blk >= out[n - 1]) {
                continue;
            }
            n--;
        }
        for (i = n; i > 0 && out[i - 1] > b->blk; i--) {
            out[i] = out[i - 1];
        }
        out[i] = b->blk;
        n++;
    }
    return n;
}

static int sync_device(block_device_t *dev, int durable);

/*
 * One pass of `bcache_sync`: the `first` buffers of `dev` in pass 0 and every
 * dirty one in pass 1, written with the request `flags`, except that a run of
 * `first` buffers is written with `first_flags`. Returns 0 or the first
 * failure's -errno.
 */
static int sync_pass(block_device_t *dev, int pass, uint32_t flags, uint32_t first_flags)
{
    uint64_t batch[SYNC_BATCH];
    uint64_t last = 0;
    int started = 0;

    for (;;) {
        int n, i;

        n = gather_dirty(dev, pass, started, last, batch);
        if (n == 0) {
            if (!started) {
                return 0; /* a scan from the start found nothing */
            }
            started = 0; /* again from the start, for what a wait passed over */
            continue;
        }
        for (i = 0; i < n; i++) {
            struct bbuf *next = index_find(dev, batch[i]);
            int rc;

            if (!next || !next->dirty || (pass == 0 && next->order != BCACHE_ORDER_FIRST) ||
                (started && next->blk <= last)) {
                continue; /* written, emptied or rekeyed while the batch's writes slept */
            }
            if (pass == 1 && next->order == BCACHE_ORDER_AFTER) {
                struct bbuf *f = first_dirty(dev);

                if (f && f->holder != -1 && f->holder != caller_pid()) {
                    f->waiters++;
                    wq_wait_event_nointr(&f->wq, busy_gone, f);
                    f->waiters--;
                    break; /* gathered again after the wait */
                }
                if (f) {
                    rc = write_firsts(dev);
                    if (rc < 0) {
                        return rc;
                    }
                    if (rc == 0) {
                        return -EIO; /* the firsts left are ones whose writes failed */
                    }
                    break; /* the firsts slept: gathered again */
                }
            }
            if (next->holder != -1 && next->holder != caller_pid()) {
                next->waiters++;
                wq_wait_event_nointr(&next->wq, busy_gone, next);
                next->waiters--;
                break; /* the batch is stale after the wait: gathered again */
            }
            if (next->holder == caller_pid()) {
                k_panic("bcache: hold rule: a sync while holding a block of the device");
            }
            if (!victim_may_be_written(next->dev, dev)) {
                k_panic("bcache: hold rule: a sync while holding a block of another device");
            }
            rc = write_run(next, &last, next->order == BCACHE_ORDER_FIRST ? first_flags : flags);
            if (rc != 0) {
                return rc;
            }
            started = 1;
        }
    }
}

/**
 * Write `dev`'s dirty buffers: its `first` buffers in the first pass and
 * everything dirty in the second, each pass in ascending block order, the
 * blocks taken in gathered batches. A `first` dirtied while the second
 * pass slept is written before the next `after` of the device, whatever
 * their block numbers, so the order the marks promise holds across the
 * writes' sleeps; a `first` another process holds is waited for.
 *
 * Any dirty buffer another process holds is waited for, not passed over: a
 * sync that returned over one would report clean what is still in flight.
 * The place reached is kept as a block number, since a buffer let go of
 * during a wait can be given to another block. A sync is called with
 * nothing held, of this device or of any other that would forbid writing
 * it; a caller that breaks that panics.
 *
 * On a write-through disk a `first` buffer is written with the disk's flush,
 * so it is on the medium before an `after` is written, by this sync, another
 * or a reclaim. The other writes leave the disk's cache to one flush at the
 * sync's end, where a flush after every write was made before; that flush
 * makes the sync durable when it returns, even when a write failed, and its
 * own failure is recorded against the device as a failed write is.
 */
int bcache_sync(block_device_t *dev)
{
    return sync_device(dev, 1);
}

int bcache_sync_unflushed(block_device_t *dev)
{
    return sync_device(dev, 0);
}

/* `bcache_sync`, durable with `durable` set; without it every write leaves
 * the disk's cache to the caller's next flush, in the order written. */
static int sync_device(block_device_t *dev, int durable)
{
    int through = (block_flags(dev) & BLOCK_WRITE_THROUGH) != 0;
    uint32_t flags = through ? BLOCK_REQ_NOFLUSH : 0;
    int rc, flushed;

    /* A durable sync writes a run of firsts with the disk's flush. */
    rc = sync_pass(dev, 0, flags, durable ? 0 : flags);
    if (rc == 0) {
        rc = sync_pass(dev, 1, flags, durable ? 0 : flags);
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

/**
 * Give back up to `want` buffers: empty ones first, then clean ones oldest
 * first, then, with `can_write`, dirty ones written back where this caller
 * may write them. A buffer a process waits for is left, woken or not, since
 * the waiter has its address. Each pass has a budget, so a buffer dirtied
 * again as fast as it is written cannot keep the shrink here.
 */
uint32_t bcache_shrink(uint32_t want, int can_write, uint32_t *written)
{
    uint32_t got = 0;
    int pass;

    for (pass = 0; pass < (can_write ? 3 : 2) && got < want; pass++) {
        int budget = (int)nbufs * 2;

        while (got < want && budget-- > 0) {
            struct bbuf *b, *victim = 0;

            for (b = bufs; b; b = b->next) {
                stat_visits++;
                if (b->holder != -1 || b->waiters > 0) {
                    continue;
                }
                if (pass == 0 ? b->dev != 0 : (pass == 1 ? b->dirty || !b->dev : !b->dirty)) {
                    continue;
                }
                if (pass == 2 && (b->write_failed || !victim_may_be_written(b->dev, 0))) {
                    continue;
                }
                if (pass == 2 && b->order == BCACHE_ORDER_AFTER && firsts_pending(b->dev)) {
                    continue;
                }
                if (!victim || (int32_t)(b->stamp - victim->stamp) < 0) {
                    victim = b;
                }
            }
            if (!victim) {
                break;
            }
            if (victim->dirty) {
                int rc;

                if (victim->order == BCACHE_ORDER_AFTER && write_firsts(victim->dev) != 0) {
                    continue; /* the firsts went out, or one failed: scan again */
                }
                hold(victim);
                rc = write_buffer(victim);
                unhold(victim);
                wq_wake_all(&victim->wq);
                if (rc != 0 || victim->dirty || victim->waiters > 0) {
                    continue; /* failed, dirtied again, or waited for meanwhile */
                }
                (*written)++;
            }
            buffer_free(victim);
            got++;
        }
    }
    return got;
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
