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
#include "../mm/pressure.h"
#include "../panic.h"
#include "../proc/process.h"

static struct bbuf *bufs;     /* every buffer, newest first */
static uint32_t nbufs;        /* how many */
static uint32_t limit_set;    /* a limit given by `bcache_set_limit`, or 0 */
static uint32_t clock_stamp;
static uint64_t stat_hits, stat_fills, stat_waits;

/* The most buffers the cache makes: half of what the caches together may
 * hold under pressure, and never fewer than the floor, since a file system
 * holds up to four at once and a write of one may take one more. */
static uint32_t bcache_limit(void)
{
    uint32_t n = limit_set ? limit_set : mem_cache_target() / 2u;

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

/* An empty, unheld buffer off the list and back to the heap. */
static void buffer_free(struct bbuf *b)
{
    struct bbuf **link;

    for (link = &bufs; *link != b; link = &(*link)->next) {
    }
    *link = b->next;
    nbufs--;
    kfree_pages(b->data, 1);
    kfree(b);
}

static int caller_pid(void)
{
    return current_proc ? (int)current_proc->pid : 0;
}

/* The rules that keep holders out of deadlock, checked before a wait or a
 * fill. A violation is a driver defect, not a runtime condition.
 *
 * Blocks of one device are taken in order, so two processes holding one each
 * cannot each be waiting for the other's. A second device is allowed only
 * downward: a device that is under everything the caller holds, which is what
 * a loop device's read does when it reads the file it is made of. Downward is
 * the only direction there is, since the chain of backing devices has no
 * cycle in it, so the same argument holds across devices as within one.
 */
static void check_hold_rules(block_device_t *dev, uint64_t blk)
{
    int pid = caller_pid();
    int held = 0;
    const struct bbuf *b;

    for (b = bufs; b; b = b->next) {
        if (b->holder != pid || !b->dev) {
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
 */
static int victim_may_be_written(block_device_t *victim, block_device_t *filling)
{
    int pid = caller_pid();
    int held = 0;
    const struct bbuf *b;

    if (!victim || !victim->backing) {
        return 1; /* its write does not come back through here */
    }
    for (b = bufs; b; b = b->next) {
        if (b->holder != pid || !b->dev) {
            continue;
        }
        held++;
        if (b->dev != victim && !block_is_below(victim->backing, b->dev)) {
            return 0;
        }
    }
    /* Room for two more: the victim, held for the write, and the block the
     * write takes on the device under it. A caller with less room than that
     * is given another victim rather than a machine that stops on the fifth
     * buffer. */
    if (held + 2 > BCACHE_HOLD_MAX) {
        return 0;
    }
    /* And the locks, which this cache cannot see: writing such a buffer goes
     * through the file system holding its file and takes that mount's lock.
     * A caller that already holds a mount's lock could then be waiting for a
     * lock another caller holds while that one waits for this one. Its own
     * chain is what it may write: a volume on a loop device evicting the
     * device's own buffers is the ordinary case, and the ordering rules
     * above cover it. */
    if (victim == filling) {
        return 1;
    }
    return !vfs_ns_held_by_caller();
}

static int busy_gone(void *arg)
{
    struct bbuf *b = arg;
    return b->holder == -1;
}

/* One buffer to the device; a failure is recorded against the device and
 * leaves the buffer dirty. */
static int write_buffer(struct bbuf *b)
{
    int rc = block_request(b->dev, 1, b->blk * BCACHE_BLOCK_SECTORS, b->bytes / BLOCK_SECTOR_SIZE,
                           b->data);

    if (rc != 0) {
        klog(KLOG_WARN, "bcache", "write: sector %d failed (%d), kept dirty",
             (int)(b->blk * BCACHE_BLOCK_SECTORS), rc);
        b->dev->flags |= BLOCK_WRITE_FAILED;
        b->write_failed = 1;
        vfs_device_failed(b->dev, b->blk * BCACHE_BLOCK_SECTORS, rc);
        return rc;
    }
    b->dirty = 0;
    b->write_failed = 0;
    return 0;
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

static struct bbuf *get(block_device_t *dev, uint64_t blk, int blank);

struct bbuf *bcache_get(block_device_t *dev, uint64_t blk)
{
    return get(dev, blk, 0);
}

struct bbuf *bcache_get_blank(block_device_t *dev, uint64_t blk)
{
    return get(dev, blk, 1);
}

/*
 * The two gets. A cached block is returned as it is either way; a miss is
 * filled from the device, or, for the blank get, zeroed without a read: a
 * caller about to write the whole block, or wanting zeros in it, would only
 * throw the device's bytes away, and on a stick the read costs as much as
 * the write that follows it.
 */
static struct bbuf *get(block_device_t *dev, uint64_t blk, int blank)
{
    struct bbuf *victim = 0;
    struct bbuf *b;

    if (!dev || blk >= (dev->sector_count + BCACHE_BLOCK_SECTORS - 1) / BCACHE_BLOCK_SECTORS) {
        return 0;
    }
    check_hold_rules(dev, blk);

again:
    for (b = bufs; b; b = b->next) {
        if (b->dev == dev && b->blk == blk) {
            while (b->holder != -1) {
                stat_waits++;
                b->waiters++; /* counted until this runs again, so reclaim leaves it */
                wq_wait_event_nointr(&b->wq, busy_gone, b);
                b->waiters--;
            }
            if (b->dev == dev && b->blk == blk) {
                b->holder = caller_pid();
                stat_hits++;
                return b;
            }
            goto again;
        }
    }

    /* An empty buffer, or a new one while the cache is under its limit;
     * otherwise the least recently released unheld buffer this caller may
     * take, a dirty one written first. A dirty buffer whose last write
     * failed, or whose write would take a block this caller may not take
     * while it holds what it holds, is not a candidate. */
    for (;;) {
        victim = 0;
        for (b = bufs; b; b = b->next) {
            if (b->holder != -1) {
                continue;
            }
            if (!b->dev) {
                victim = b;
                break;
            }
            if (b->dirty && (b->write_failed || !victim_may_be_written(b->dev, dev))) {
                continue;
            }
            if (!victim || (int32_t)(b->stamp - victim->stamp) < 0) {
                victim = b;
            }
        }
        if ((!victim || victim->dev) && nbufs < bcache_limit()) {
            struct bbuf *fresh = buffer_new();

            if (fresh) {
                victim = fresh;
            }
        }
        if (!victim) {
            klog(KLOG_WARN, "bcache", "get: no free buffer");
            return 0;
        }
        if (!victim->dirty) {
            break;
        }
        victim->holder = caller_pid(); /* held for the write */
        if (write_buffer(victim) == 0) {
            victim->holder = -1;
            wq_wake_all(&victim->wq);
            /* The write slept, whether it went through a file system and
             * back into this cache or waited for a controller, and another
             * caller could have filled the very block this one is about to
             * key. Look again rather than key it twice. */
            goto again;
        }
        victim->holder = -1;
        wq_wake_all(&victim->wq);
        /* The write failed and marked the buffer; the next scan leaves it. */
    }
    /* Keyed and held before the request sleeps, so a second process asking
     * for this block waits for this buffer instead of filling a second one. */
    victim->dev = dev;
    victim->blk = blk;
    victim->dirty = 0;
    /* A mark left by a device that went while this buffer was held for a
     * fill or a writeback: every buffer changes hands here, so this is the
     * one place the mark must go. */
    victim->discard = 0;
    victim->holder = caller_pid();
    victim->bytes = block_bytes(dev, blk);
    if (blank) {
        k_memset(victim->data, 0, victim->bytes);
        return victim;
    }
    if (block_request(dev, 0, blk * BCACHE_BLOCK_SECTORS, victim->bytes / BLOCK_SECTOR_SIZE,
                      victim->data) != 0) {
        victim->dev = 0;
        victim->holder = -1;
        wq_wake_all(&victim->wq);
        return 0;
    }
    stat_fills++;
    return victim;
}

void bcache_release(struct bbuf *b)
{
    if (!b || b->holder != caller_pid()) {
        k_panic("bcache: hold rule: a release by a process that does not hold the buffer");
        return;
    }
    b->holder = -1;
    b->stamp = ++clock_stamp;
    if (b->discard) {
        /* The device went while this was held. Nothing in it is worth
         * keeping and the device it names may be freed the moment the last
         * hold goes, so the buffer is emptied here and not later. */
        b->dev = 0;
        b->dirty = 0;
        b->discard = 0;
    }
    wq_wake_all(&b->wq);
}

void bcache_dirty(struct bbuf *b)
{
    if (!b || b->holder != caller_pid()) {
        k_panic("bcache: hold rule: a dirty mark by a process that does not hold the buffer");
        return;
    }
    b->dirty = 1;
}

int bcache_write(struct bbuf *b)
{
    if (!b || b->holder != caller_pid()) {
        k_panic("bcache: hold rule: a write by a process that does not hold the buffer");
        return -EIO;
    }
    /* An operation in progress when the device failed stops at its next
     * metadata write, so the read-only transition is not written across. */
    if (b->dev->flags & BLOCK_WRITE_FAILED) {
        return -EROFS;
    }
    b->dirty = 1;
    return write_buffer(b);
}

int bcache_sync(block_device_t *dev)
{
    uint64_t last = 0;
    int started = 0;

    for (;;) {
        struct bbuf *next = 0;
        struct bbuf *b;
        int rc;

        /* The place reached is a block number and not a buffer: a buffer let
         * go of while this waits can be given to another block, and a cutoff
         * read from it afterwards would step over blocks still to write. */
        for (b = bufs; b; b = b->next) {
            if (b->dev == dev && b->dirty && (!started || b->blk > last) &&
                (!next || b->blk < next->blk)) {
                next = b;
            }
        }
        if (!next) {
            return 0;
        }
        /* A held dirty buffer is waited for, not passed over: a sync that
         * returned over one would report clean what is still in flight. */
        if (next->holder != -1 && next->holder != caller_pid()) {
            next->waiters++;
            wq_wait_event_nointr(&next->wq, busy_gone, next);
            next->waiters--;
            continue;
        }
        if (next->holder == caller_pid()) {
            /* Waiting below would be waiting for ourselves: a sync is called
             * with nothing of the device held. */
            k_panic("bcache: hold rule: a sync while holding a block of the device");
        }
        if (!victim_may_be_written(next->dev, dev)) {
            /* A sync is called with nothing else held; one that is not would
             * write a block whose device re-enters this cache from under a
             * hold that forbids it. */
            k_panic("bcache: hold rule: a sync while holding a block of another device");
        }
        next->holder = caller_pid(); /* held across the request's sleep */
        rc = write_buffer(next);
        next->holder = -1;
        wq_wake_all(&next->wq);
        if (rc != 0) {
            return rc;
        }
        last = next->blk;
        started = 1;
    }
}

void bcache_device_gone(block_device_t *dev)
{
    struct bbuf *b;
    int failed = 0;

    dev->flags |= BLOCK_GONE;
    for (b = bufs; b; b = b->next) {
        if (b->dev != dev) {
            continue;
        }
        if (b->dirty) {
            if (!failed) {
                vfs_device_failed(dev, b->blk * BCACHE_BLOCK_SECTORS, -EIO);
                failed = 1;
            }
            b->dirty = 0;
        }
        if (b->holder != -1) {
            b->discard = 1;
            continue;
        }
        b->dev = 0;
        b->discard = 0;
    }
}

int bcache_device_holds(block_device_t *dev)
{
    const struct bbuf *b;
    int held = 0;

    for (b = bufs; b; b = b->next) {
        if (b->dev == dev && b->holder != -1) {
            held++;
        }
    }
    return held;
}

uint32_t bcache_shrink(uint32_t want, int can_write, uint32_t *written)
{
    uint32_t got = 0;
    int pass;

    /* Empty buffers go first, then clean ones oldest first, then dirty ones
     * written back where this caller may write them. A buffer a process
     * waits for is left, woken or not: the waiter holds its address. Each
     * pass has a budget, so a buffer dirtied again as fast as it is written
     * cannot keep this here. */
    for (pass = 0; pass < (can_write ? 3 : 2) && got < want; pass++) {
        int budget = (int)nbufs * 2;

        while (got < want && budget-- > 0) {
            struct bbuf *b, *victim = 0;

            for (b = bufs; b; b = b->next) {
                if (b->holder != -1 || b->waiters > 0) {
                    continue;
                }
                if (pass == 0 ? b->dev != 0 : (pass == 1 ? b->dirty || !b->dev : !b->dirty)) {
                    continue;
                }
                if (pass == 2 && (b->write_failed || !victim_may_be_written(b->dev, 0))) {
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

                victim->holder = caller_pid();
                rc = write_buffer(victim);
                victim->holder = -1;
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
        if (b->dev == dev && (b->holder != -1 || b->dirty)) {
            return -EBUSY;
        }
    }
    for (b = bufs; b; b = b->next) {
        if (b->dev == dev) {
            b->dev = 0;
        }
    }
    dev->flags &= ~BLOCK_WRITE_FAILED;
    return 0;
}
