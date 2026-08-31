/*
 * bcache.c: the fixed buffer cache. The contract is in bcache.h; what is
 * here is the bookkeeping: a linear table of 64 buffers keyed by device and
 * block, least-recently-released replacement, and the two hold rules checked
 * on every acquisition.
 */

#include "bcache.h"

#include "../klog.h"
#include "../lib/kerrno.h"
#include "../panic.h"
#include "../proc/process.h"

static struct bbuf bufs[BCACHE_BUFFERS];
static uint8_t pages[BCACHE_BUFFERS][BCACHE_BLOCK_BYTES]
    __attribute__((aligned(4096)));
static uint32_t clock_stamp;
static int initialized;
static uint64_t stat_hits, stat_fills, stat_waits;

uint64_t bcache_stat_hits(void) { return stat_hits; }
uint64_t bcache_stat_fills(void) { return stat_fills; }
uint64_t bcache_stat_waits(void) { return stat_waits; }

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

/* The two rules that keep holders out of deadlock, checked before a wait or
 * a fill. A violation is a driver defect, not a runtime condition. */
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
        if (bufs[i].dev == dev && bufs[i].blk >= blk) {
            k_panic("bcache: a lower or equal block taken while holding a higher one");
        }
    }
    if (held >= BCACHE_HOLD_MAX) {
        k_panic("bcache: a process held more buffers than the limit");
    }
}

static int busy_gone(void *arg)
{
    struct bbuf *b = arg;
    return b->holder == -1;
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

struct bbuf *bcache_get(block_device_t *dev, uint64_t blk)
{
    struct bbuf *victim = 0;
    int i;

    if (!initialized) {
        bcache_init();
    }
    if (!dev || blk * BCACHE_BLOCK_SECTORS >= dev->sector_count) {
        return 0;
    }
    check_hold_rules(dev, blk);

    for (i = 0; i < BCACHE_BUFFERS; i++) {
        if (bufs[i].dev == dev && bufs[i].blk == blk) {
            while (bufs[i].holder != -1) {
                stat_waits++;
                if (wq_wait_event(&bufs[i].wq, busy_gone, &bufs[i]) != 0 &&
                    bufs[i].holder != -1) {
                    continue; /* EINTR: the buffer is still the goal */
                }
            }
            /* The release may have invalidated or repurposed it. */
            if (bufs[i].dev == dev && bufs[i].blk == blk) {
                bufs[i].holder = caller_pid();
                stat_hits++;
                return &bufs[i];
            }
            return bcache_get(dev, blk);
        }
    }

    /* A miss: the oldest unheld clean buffer is refilled. Dirty victims are
     * milestone 37's rule; nothing dirties a buffer yet, and one that is
     * dirty anyway is skipped rather than lost. */
    for (i = 0; i < BCACHE_BUFFERS; i++) {
        if (bufs[i].holder != -1 || bufs[i].dirty) {
            continue;
        }
        if (!bufs[i].dev) {
            victim = &bufs[i];
            break;
        }
        if (!victim || bufs[i].stamp < victim->stamp) {
            victim = &bufs[i];
        }
    }
    if (!victim) {
        klog(KLOG_WARN, "bcache", "no free buffer");
        return 0;
    }
    victim->bytes = block_bytes(dev, blk);
    if (block_request(dev, 0, blk * BCACHE_BLOCK_SECTORS,
                      victim->bytes / BLOCK_SECTOR_SIZE, victim->data) != 0) {
        victim->dev = 0;
        return 0;
    }
    stat_fills++;
    victim->dev = dev;
    victim->blk = blk;
    victim->dirty = 0;
    victim->failed = 0;
    victim->holder = caller_pid();
    return victim;
}

void bcache_release(struct bbuf *b)
{
    if (!b || b->holder == -1) {
        return;
    }
    b->holder = -1;
    b->stamp = ++clock_stamp;
    wq_wake_all(&b->wq);
}

void bcache_dirty(struct bbuf *b)
{
    if (b && b->holder != -1) {
        b->dirty = 1;
    }
}

int bcache_sync(block_device_t *dev)
{
    int rc = 0;
    int wrote;

    /* Ascending block order, restarted after each write so the scan stays a
     * plain table walk; among data buffers the order carries no meaning, but
     * a deterministic order is what the tests can see. */
    do {
        struct bbuf *next = 0;
        int i;

        wrote = 0;
        for (i = 0; i < BCACHE_BUFFERS; i++) {
            if (bufs[i].dev == dev && bufs[i].dirty && !bufs[i].failed &&
                (!next || bufs[i].blk < next->blk)) {
                next = &bufs[i];
            }
        }
        if (next) {
            if (block_request(dev, 1, next->blk * BCACHE_BLOCK_SECTORS,
                              next->bytes / BLOCK_SECTOR_SIZE,
                              next->data) == 0) {
                next->dirty = 0;
                wrote = 1;
            } else {
                next->failed = 1;
                if (rc == 0) {
                    rc = -EIO;
                }
                klog(KLOG_WARN, "bcache", "writeback failed; kept dirty");
                wrote = 1; /* keep scanning past it via `failed` */
            }
        }
    } while (wrote);
    return rc;
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
    return 0;
}
