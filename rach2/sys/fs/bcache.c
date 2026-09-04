/*
 * bcache.c: the fixed buffer cache. The contract is in bcache.h; what is
 * here is the bookkeeping: a linear table of 64 buffers keyed by device and
 * block, least-recently-released replacement, and the two hold rules checked
 * on every acquisition.
 */

#include "bcache.h"
#include "../lib/kstring.h"

#include "vfs.h"

#include "../klog.h"
#include "../lib/kerrno.h"
#include "../panic.h"
#include "../proc/process.h"

static struct bbuf bufs[BCACHE_BUFFERS];
static uint8_t pages[BCACHE_BUFFERS][BCACHE_BLOCK_BYTES] __attribute__((aligned(4096)));
static uint32_t clock_stamp;
static int initialized;
static uint64_t stat_hits, stat_fills, stat_waits;

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

static void bcache_init(void)
{
    int i;

    for (i = 0; i < BCACHE_BUFFERS; i++) {
        bufs[i].data = pages[i];
        bufs[i].holder = -1;
        wait_queue_init(&bufs[i].wq);
    }
    initialized = 1;
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
    int i;

    for (i = 0; i < BCACHE_BUFFERS; i++) {
        if (bufs[i].holder != pid || !bufs[i].dev) {
            continue;
        }
        held++;
        if (bufs[i].dev != dev) {
            if (!block_is_below(dev, bufs[i].dev)) {
                k_panic("bcache: hold rule: a block on a device that is not under the ones held");
            }
            continue; /* under it: its blocks are its own to order */
        }
        if (bufs[i].blk >= blk) {
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
    int i;

    if (!victim || !victim->backing) {
        return 1; /* its write does not come back through here */
    }
    for (i = 0; i < BCACHE_BUFFERS; i++) {
        if (bufs[i].holder != pid || !bufs[i].dev) {
            continue;
        }
        held++;
        if (bufs[i].dev != victim && !block_is_below(victim->backing, bufs[i].dev)) {
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
        vfs_device_failed(b->dev, b->blk * BCACHE_BLOCK_SECTORS, rc);
        return rc;
    }
    b->dirty = 0;
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
    unsigned char tried[BCACHE_BUFFERS];
    int i;

    for (i = 0; i < BCACHE_BUFFERS; i++) {
        tried[i] = 0;
    }
    if (!initialized) {
        bcache_init();
    }
    if (!dev || blk >= (dev->sector_count + BCACHE_BLOCK_SECTORS - 1) / BCACHE_BLOCK_SECTORS) {
        return 0;
    }
    check_hold_rules(dev, blk);

again:
    for (i = 0; i < BCACHE_BUFFERS; i++) {
        if (bufs[i].dev == dev && bufs[i].blk == blk) {
            while (bufs[i].holder != -1) {
                stat_waits++;
                wq_wait_event_nointr(&bufs[i].wq, busy_gone, &bufs[i]);
            }
            if (bufs[i].dev == dev && bufs[i].blk == blk) {
                bufs[i].holder = caller_pid();
                stat_hits++;
                return &bufs[i];
            }
            goto again; /* the release invalidated or repurposed it */
        }
    }

    /* The least recently released unheld buffer, a dirty one written first;
     * a buffer whose write fails stays dirty and is passed over this time. */
    for (;;) {
        victim = 0;
        for (i = 0; i < BCACHE_BUFFERS; i++) {
            if (bufs[i].holder != -1 || tried[i]) {
                continue;
            }
            if (!bufs[i].dev) {
                victim = &bufs[i];
                break;
            }
            if (!victim || (int32_t)(bufs[i].stamp - victim->stamp) < 0) {
                victim = &bufs[i];
            }
        }
        if (!victim) {
            klog(KLOG_WARN, "bcache", "get: no free buffer");
            return 0;
        }
        if (!victim->dirty) {
            break;
        }
        if (!victim_may_be_written(victim->dev, dev)) {
            /* Its write would take a block this caller may not take while it
             * holds what it holds; another buffer can be the victim. */
            tried[victim - bufs] = 1;
            continue;
        }
        victim->holder = caller_pid(); /* held for the write, as a sync would */
        if (write_buffer(victim) == 0) {
            victim->holder = -1;
            wq_wake_all(&victim->wq);
            /* The write may have gone through a file system and back into
             * this cache, which could have filled the very block this call
             * is about to key. Look again rather than key it twice. */
            if (victim->dev && victim->dev->backing) {
                goto again;
            }
            break;
        }
        victim->holder = -1;
        wq_wake_all(&victim->wq); /* a waiter for its old key looks again */
        tried[victim - bufs] = 1;
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
        int rc;
        int i;

        /* The place reached is a block number and not a buffer: a buffer let
         * go of while this waits can be given to another block, and a cutoff
         * read from it afterwards would step over blocks still to write. */
        for (i = 0; i < BCACHE_BUFFERS; i++) {
            if (bufs[i].dev == dev && bufs[i].dirty && (!started || bufs[i].blk > last) &&
                (!next || bufs[i].blk < next->blk)) {
                next = &bufs[i];
            }
        }
        if (!next) {
            return 0;
        }
        /* A held dirty buffer is waited for, not passed over: a sync that
         * returned over one would report clean what is still in flight. */
        if (next->holder != -1 && next->holder != caller_pid()) {
            wq_wait_event_nointr(&next->wq, busy_gone, next);
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
    int i;
    int failed = 0;

    dev->flags |= BLOCK_GONE;
    for (i = 0; i < BCACHE_BUFFERS; i++) {
        if (bufs[i].dev != dev) {
            continue;
        }
        if (bufs[i].dirty) {
            if (!failed) {
                vfs_device_failed(dev, bufs[i].blk * BCACHE_BLOCK_SECTORS, -EIO);
                failed = 1;
            }
            bufs[i].dirty = 0;
        }
        if (bufs[i].holder != -1) {
            bufs[i].discard = 1;
            continue;
        }
        bufs[i].dev = 0;
        bufs[i].discard = 0;
    }
}

int bcache_device_holds(block_device_t *dev)
{
    int i;
    int held = 0;

    for (i = 0; i < BCACHE_BUFFERS; i++) {
        if (bufs[i].dev == dev && bufs[i].holder != -1) {
            held++;
        }
    }
    return held;
}

int bcache_invalidate(block_device_t *dev)
{
    int i;

    for (i = 0; i < BCACHE_BUFFERS; i++) {
        if (bufs[i].dev == dev && (bufs[i].holder != -1 || bufs[i].dirty)) {
            return -EBUSY;
        }
    }
    for (i = 0; i < BCACHE_BUFFERS; i++) {
        if (bufs[i].dev == dev) {
            bufs[i].dev = 0;
        }
    }
    dev->flags &= ~BLOCK_WRITE_FAILED; /* the next mount starts clean */
    return 0;
}
