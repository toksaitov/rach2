/*
 * request.c: the one entry every transfer takes, and the requests behind it.
 *
 * A fault-safe device is called directly. A device that runs the transfer in
 * its caller, `BLOCK_CALLER_SYNC`, is called directly too and takes no
 * request. A sleeping device is given a request allocated for the transfer,
 * with a buffer of its own, a frame of the DMA pool for a transfer of a page
 * or less and heap pages for a larger one, since the device is given a
 * physical address: a write is copied into the buffer before `submit` and a
 * read is copied out after, so a completion that arrives after the caller
 * gave up lands in memory nothing else owns. A request is held by its caller
 * and by its device, and is freed when both have let go: a request given up
 * on is quarantined with its buffer until the device answers late.
 *
 * At most `rach2.blockpool=N` requests, sixteen unless set, are live at once,
 * and a caller that finds the limit reached waits; running out is not an
 * error. A transfer that failed answers `EIO`, and so does a request the heap
 * cannot hold even at one page. The deadline of a request starts when it is
 * taken, not when its caller began to wait. A request past its deadline is
 * still waited for: the deadline is a period, and only a request unanswered
 * through a bounded number of periods in a row is given up.
 *
 * Two rules keep a waiter from waiting for ever on requests that only a late
 * answer could free. Failure is per device, where a device is the one whose
 * `submit` answers, a stick and not each of its partition views: a device
 * whose requests are all quarantined is stalled, and its waiters and later
 * callers get `EIO` until it answers something late. And no device may hold
 * more than half of the limit, rounded down and at least one, so a stalled
 * device leaves the other half to the healthy ones; at `rach2.blockpool=1`
 * the half is the whole pool. A waiter that finds every live request
 * quarantined gets `EIO` too.
 *
 * The pool needs no big lock. Its list, counts and kept buffers are under
 * `req_lock`, a spinlock taken with interrupts off, which a submit takes and
 * a completion takes from the interrupt. A caller sleeps for its answer on
 * a queue of its own, as Linux's `submit_bio_wait` sleeps on a completion
 * on its stack that only that bio's end completes (block/bio.c:1453-1481),
 * and only that request's answer or quarantine wakes it. A caller waiting
 * for room in the pool sleeps on the pool's one queue, which every answer,
 * freed request and quarantine wakes, since each may make room. Every
 * driver serializes on locks of its own, as Linux's do, and may be called
 * with the big lock or without it; USB storage and loop take it themselves
 * where they need it (block.h).
 */

#include "../lib/kirq.h"
#include "../lib/spinlock.h"
#include "../arch/arch.h"
#include "block.h"

#include "../cmdline.h"
#include "../klog.h"
#include "../kio.h"
#include "../lib/kerrno.h"
#include "../latency.h"
#include "../lib/kstring.h"
#include "../mm/kheap.h"
#include "../mm/paging.h"
#include "../mm/pressure.h"
#include "../panic.h"
#include "../timer.h"
#include "../proc/process.h"
#include "../proc/sched.h"
#include "../proc/wait_queue.h"

/* Policy: how many requests may be live at once unless `rach2.blockpool`
 * says otherwise, and the most the option accepts. The limit is what the
 * stall rules divide between devices; it is not a table. */
#define REQ_LIMIT             16
/* The deadline's period, thirty seconds of ticks; how many periods
 * in a row a request is waited for before it is given up, two minutes in
 * all; and the most seconds `rach2.blockdeadline=S` may set the period to.
 * The deadline answers a caller whose device has gone and does not judge a
 * slow one: an emulated disk on a loaded host can take more than one period
 * to answer a write. The lag device sets a single period of five seconds of
 * its own, to test a device that never answers; `rach2.blockdeadline`
 * lets a test see a late answer without waiting thirty seconds. */
#define REQ_DEFAULT_DEADLINE  MS_TO_TICKS(30000)
#define REQ_DEFAULT_DEADLINES 4u
#define REQ_DEADLINE_MAX_S    600u
static uint32_t default_deadline = REQ_DEFAULT_DEADLINE;

/* The pool's lock: the list of live requests and every field the pool
 * keeps on one, the counts and the clock below, the kept buffers, each
 * device's `stalled` and `stats`, and each request's `waiter`. Taken with
 * interrupts off, inside a driver's own lock when a completion comes from
 * under it, and before the heap's and the drivers' counters' (block.c) and
 * a request's own queue's, which its caller's wake takes under it; a wake
 * of `pool_wq` comes after it is given back. */
static struct spinlock req_lock = SPINLOCK_INIT_NAMED("block_req_lock");
static struct block_request *reqs; /* every live request */
static uint32_t live;              /* how many are on the list */
static uint32_t next_generation;   /* rising; see req_new */
static wait_queue_t
    pool_wq; /* the waiters for room: woken on any completion, freed request or quarantine */
static volatile uint32_t req_ticks; /* the deadlines' clock, in ticks */
static uint32_t req_seen;           /* `kernel_ticks` at the last tick's call */
static uint32_t quarantined_count;
static uint32_t overlap_count; /* completions with another request in flight */
/* Cumulative, for /proc/disk: quarantines made, callers released with EIO
 * by a deadline or a stalled device, and the tick of the last release, so a
 * test can require two callers to have been released on one tick. */
static uint32_t quarantine_total;
static uint32_t release_total;
static uint32_t release_tick;
static uint32_t pool_size;  /* the limit: REQ_LIMIT, or rach2.blockpool=N */
static uint32_t device_cap; /* the most requests one device may hold */

/* Buffers of REQ_KEEP_PAGES heap pages that freed requests left, kept
 * mapped for the next request of more than a page and at most that many,
 * up to REQ_KEEP of them, in `kept_bufs[0..keep_count)`. A buffer mapped
 * for each request and unmapped after it, which flushes every processor's
 * TLB, costs more than the transfer itself under a hypervisor; Linux reads
 * into the page cache's own pages and maps none, and its mempools keep
 * freed elements in the same way (mm/mempool.c:560-561). Reclaim takes the
 * kept buffers back (`req_keep_shrink`). The list is under `req_lock`, as
 * the list of live requests is. */
#define REQ_KEEP_PAGES 32u
#define REQ_KEEP       4u
static void *kept_bufs[REQ_KEEP];
static uint32_t keep_count;
static uint32_t req_keep_shrink(struct mem_shrinker *s, uint32_t want, int may_sleep);
static struct mem_shrinker keep_shrinker = {req_keep_shrink, NULL};

/** The deadlines' clock as it stands now: `req_ticks` and the ticks
 * `kernel_ticks` has counted since the tick's last call, which a boot
 * processor's tick that found the big lock held left out. */
static uint32_t req_clock(void)
{
    return req_ticks + ((uint32_t)kernel_ticks - req_seen);
}

/**
 * `req_lock` held. Count toward `disk-wait-ticks` the ticks request `r`,
 * leaving flight now, spent in flight since `block_request_tick` last
 * counted: from the later of `taken_tick` and that call to the clock now,
 * which the next call, finding it no longer in flight, would leave out.
 */
static void req_count_rest(const struct block_request *r)
{
    uint32_t from = (int32_t)(r->taken_tick - req_ticks) > 0 ? r->taken_tick : req_ticks;
    uint32_t now = req_clock();

    if ((int32_t)(now - from) > 0) {
        block_count_wait_ticks(now - from);
    }
}

/** Take `req_lock` with interrupts off and return the state `pool_unlock`
 * puts back. */
static uint32_t pool_lock(void)
{
    return spin_lock_irqsave(&req_lock);
}

/** Give `req_lock` back and put back the interrupt state `pool_lock`
 * returned. */
static void pool_unlock(uint32_t flags)
{
    spin_unlock_irqrestore(&req_lock, flags);
}

/**
 * The share of each device, half of the limit, is stored before the limit:
 * a caller from the USB pump may read both between the two stores, and a
 * limit with no share would refuse it.
 */
void block_request_init(void)
{
    char value[8];
    uint32_t n = REQ_LIMIT;
    uint32_t cap;
    int i;

    if (pool_size) {
        return;
    }
    if (kcmdline_value("rach2.blockpool", value, sizeof(value))) {
        uint32_t asked = 0;

        for (i = 0; value[i] >= '0' && value[i] <= '9'; i++) {
            asked = asked * 10u + (uint32_t)(value[i] - '0');
        }
        if (i == 0 || value[i] != '\0' || asked == 0 || asked > REQ_LIMIT) {
            klog(KLOG_WARN, "block", "rach2.blockpool: not a count of 1 to %d; ignored", REQ_LIMIT);
        } else {
            n = asked;
            klog(KLOG_INFO, "block", "request pool: %d slots", (int)n);
        }
    }
    if (kcmdline_value("rach2.blockdeadline", value, sizeof(value))) {
        uint32_t asked = 0;

        for (i = 0; value[i] >= '0' && value[i] <= '9'; i++) {
            asked = asked * 10u + (uint32_t)(value[i] - '0');
        }
        if (i == 0 || value[i] != '\0' || asked == 0 || asked > REQ_DEADLINE_MAX_S) {
            klog(KLOG_WARN, "block", "rach2.blockdeadline: not a count of 1 to %d seconds; ignored",
                 (int)REQ_DEADLINE_MAX_S);
        } else {
            default_deadline = asked * TICK_HZ;
            klog(KLOG_INFO, "block", "request deadline: %d s a period", (int)asked);
        }
    }
    cap = n / 2;
    if (cap == 0) {
        cap = 1;
    }
    mem_shrinker_register(&keep_shrinker);
    device_cap = cap;
    pool_size = n;
}

/** `req_lock` held. Whether `r` is on the list of live requests. */
static int req_is_live(const struct block_request *r)
{
    const struct block_request *p;

    for (p = reqs; p; p = p->next) {
        if (p == r) {
            return 1;
        }
    }
    return 0;
}

/** `req_lock` held. A kept buffer, or NULL when none is kept. */
static void *keep_take(void)
{
    return keep_count ? kept_bufs[--keep_count] : NULL;
}

/**
 * Reclaim's call of the request pool (`struct mem_shrinker`): every kept
 * buffer given back, whatever `want` is, since they are few; the frames
 * that came back. It does not sleep.
 */
static uint32_t req_keep_shrink(struct mem_shrinker *s, uint32_t want, int may_sleep)
{
    uint32_t pages = 0;

    (void)s;
    (void)want;
    (void)may_sleep;
    for (;;) {
        uint32_t flags = pool_lock();
        void *buf = keep_take();

        pool_unlock(flags);
        if (!buf) {
            return pages;
        }
        kfree_pages(buf, REQ_KEEP_PAGES);
        pages += REQ_KEEP_PAGES;
    }
}

/** `req_lock` held. Take `r` off the list and give its page and record back:
 * a buffer of REQ_KEEP_PAGES to the kept ones while they are fewer than
 * REQ_KEEP. */
static void req_drop(struct block_request *r)
{
    struct block_request **link;

    for (link = &reqs; *link != r; link = &(*link)->next) {
    }
    *link = r->next;
    live--;
    if (r->pages == REQ_KEEP_PAGES && keep_count < REQ_KEEP) {
        kept_bufs[keep_count++] = r->page;
    } else if (r->pages > 1) {
        kfree_pages(r->page, r->pages);
    } else {
        free_kernel_frame(r->page);
    }
    kfree(r);
}

/**
 * `req_lock` held. One holder lets go: the caller when it has read the
 * answer or given up, the device when it has answered. The last one out
 * frees the request.
 */
static void req_let_go(struct block_request *r, int caller)
{
    if (caller) {
        r->held_by_caller = 0;
    } else {
        r->held_by_device = 0;
    }
    if (!r->held_by_caller && !r->held_by_device) {
        req_drop(r);
    }
}

/**
 * `req_lock` held. How many requests `owner` holds: in flight or quarantined.
 *
 * `owner` is the device a request is accounted to, `block_submitter` of the
 * device the request named: a stick's partition views share the stick's
 * share of the limit and its stall, since it is the stick that answers.
 */
static uint32_t device_held(const block_device_t *owner)
{
    const struct block_request *r;
    uint32_t held = 0;

    for (r = reqs; r; r = r->next) {
        if (r->owner == owner &&
            (r->state == BLOCK_REQ_IN_FLIGHT || r->state == BLOCK_REQ_QUARANTINED)) {
            held++;
        }
    }
    return held;
}

/** `req_lock` held. How many of `owner`'s requests are still in flight. */
static uint32_t device_in_flight(const block_device_t *owner)
{
    const struct block_request *r;
    uint32_t n = 0;

    for (r = reqs; r; r = r->next) {
        if (r->owner == owner && r->state == BLOCK_REQ_IN_FLIGHT) {
            n++;
        }
    }
    return n;
}

/**
 * `req_lock` held. Whether no request is in flight or answered and not yet
 * freed: every live request is quarantined, and nothing but a late answer
 * can free one.
 */
static int nothing_pending(void)
{
    const struct block_request *r;

    for (r = reqs; r; r = r->next) {
        if (r->state == BLOCK_REQ_IN_FLIGHT || r->state == BLOCK_REQ_DONE) {
            return 0;
        }
    }
    return 1;
}

/**
 * `req_lock` held. A fresh request on the list for `count` sectors, held by
 * its caller and its device, or NULL when the heap or the DMA pool has no
 * room for one. A transfer of a page or less takes a frame of the DMA pool,
 * below `HARD_MEMORY_LIMIT`, whose address every device can reach; a larger
 * one takes heap pages, which the devices that describe a buffer page by page
 * take: AHCI and NVMe, up to 2048 sectors, and USB mass storage, up to 1024.
 * One of REQ_KEEP_PAGES or fewer takes a kept buffer of REQ_KEEP_PAGES, or a
 * new one of that size, and only when the heap has none its own length.
 *
 * The caller sets the generation, which rises with every request and is what
 * a late completion is matched against. It is 32 bits and wraps; a wrong
 * match would need a quarantined request still live after 4 billion others.
 */
static struct block_request *req_new(uint32_t count)
{
    struct block_request *r = kzalloc(sizeof(*r));
    uint32_t pages = (count * BLOCK_SECTOR_SIZE + PAGE_SIZE - 1) / PAGE_SIZE;

    if (!r) {
        return NULL;
    }
    if (pages <= 1) {
        r->page = (uint8_t *)alloc_kernel_frame();
        pages = 1;
    } else if (pages <= REQ_KEEP_PAGES) {
        r->page = keep_take();
        if (!r->page) {
            r->page = kalloc_pages_try(REQ_KEEP_PAGES, 0);
        }
        if (r->page) {
            pages = REQ_KEEP_PAGES;
        } else {
            r->page = kalloc_pages_try(pages, 0);
        }
    } else {
        r->page = kalloc_pages_try(pages, 0); /* the caller halves and asks again */
    }
    if (!r->page) {
        kfree(r);
        return NULL;
    }
    r->pages = pages;
    r->held_by_caller = 1;
    r->held_by_device = 1;
    r->next = reqs;
    reqs = r;
    live++;
    return r;
}

/**
 * What a waiter for a request wakes on: room for one it may take, its
 * device stalled, or nothing left in flight that could free one. `arg` is
 * the owner.
 */
static int slot_ready(void *arg)
{
    block_device_t *owner = arg;
    uint32_t flags = pool_lock();
    int ready = owner->stalled || nothing_pending() ||
                (live < pool_size && device_held(owner) < device_cap);

    pool_unlock(flags);
    return ready;
}

/** The deadline's period for `device`, in ticks. */
static uint32_t req_period(const block_device_t *device)
{
    return device->deadline_ticks ? device->deadline_ticks : default_deadline;
}

/** How many periods in a row `device` is waited for before a request is
 * given up. */
static uint32_t req_periods(const block_device_t *device)
{
    return device->deadline_limit ? device->deadline_limit : REQ_DEFAULT_DEADLINES;
}

/**
 * A request to submit, taken for `device` and accounted to its submitter, or
 * NULL.
 *
 * With `wait` the caller sleeps until a request is its to take, and NULL
 * then means the device is stalled, the limit is reached with nothing in
 * flight, so only a late answer could free one, or there is no memory for
 * one. Without `wait`, for a caller that cannot sleep, NULL means no
 * request now. `why`, when not NULL, is set to `-ENOMEM` for no memory and
 * `-EIO` otherwise.
 */
static struct block_request *req_take(block_device_t *device, int write, uint64_t lba,
                                      uint32_t count, int wait, int *why)
{
    block_device_t *owner = block_submitter(device);
    int waited = 0;
    uint32_t wait_from = timer_now_us();

    if (why) {
        *why = -EIO;
    }

    block_request_init();
    for (;;) {
        uint32_t flags = pool_lock();
        struct block_request *r = NULL;
        int share_taken;

        if (owner->stalled) {
            if (waited) {
                release_total++;
                release_tick = req_ticks;
            }
            pool_unlock(flags);
            klog(KLOG_WARN, "block", "device stalled: %s",
                 waited ? "waiter released with EIO" : "request refused");
            return NULL;
        }
        share_taken = device_held(owner) >= device_cap;
        if (!share_taken && live < pool_size) {
            r = req_new(count);
            if (!r) {
                pool_unlock(flags);
                if (why) {
                    *why = -ENOMEM; /* the caller asks again at half the size */
                }
                return NULL;
            }
        }
        if (r) {
            r->device = device;
            r->owner = owner;
            r->write = write;
            r->lba = lba;
            r->count = count;
            r->status = -EIO;
            r->state = BLOCK_REQ_IN_FLIGHT;
            r->generation = ++next_generation;
            r->taken_tick = req_clock();
            r->deadline = r->taken_tick + req_period(device);
            r->overdue = 0;
            r->taken_us = timer_now_us();
            r->flags = 0;
            if (waited) {
                latency_span(LAT_DISK_WAIT, wait_from, r->taken_us);
            }
            pool_unlock(flags);
            return r;
        }
        if (!wait) {
            pool_unlock(flags);
            return NULL;
        }
        if (nothing_pending()) {
            pool_unlock(flags);
            klog(KLOG_WARN, "block", "request pool stalled: %d quarantined, waiter released",
                 (int)quarantined_count);
            return NULL;
        }
        pool_unlock(flags);
        if (!waited) {
            waited = 1;
            klog(KLOG_INFO, "block", "request pool: waiting for a slot: %s",
                 share_taken ? "the device holds its share" : "the pool is full");
        }
        wq_wait_event_nointr(&pool_wq, slot_ready, owner);
    }
}

/**
 * `req_lock` held. The device refused `submit`, so it never held the
 * request: the caller alone lets go and the request is freed.
 */
static void req_refused(struct block_request *r)
{
    r->held_by_device = 0;
    req_let_go(r, 1);
}

/** The wait condition of a request's caller: `arg` is no longer in flight. */
static int req_done(void *arg)
{
    const struct block_request *r = arg;
    return r->state != BLOCK_REQ_IN_FLIGHT;
}

/**
 * `req_lock` held. Wake `r`'s caller if it sleeps for the answer, adding the
 * processor to interrupt to `*cpus`, which the waker passes to
 * `sched_kick_cpus` once the lock is given back (`wq_wake_all_deferred`).
 * The wake is under the lock because the queue is on the caller's stack,
 * and the caller takes it back under the lock before it returns.
 */
static void req_wake_caller(struct block_request *r, uint64_t *cpus)
{
    if (r->waiter) {
        wq_wake_all_deferred(r->waiter, cpus);
    }
}

/**
 * Sleep until `r` is no longer in flight, and count the wait the caller
 * finished, which a file lock held across it records (fs/sleeplock.h).
 *
 * The caller sleeps on a queue on its own stack, as `submit_bio_wait`
 * declares its completion on its stack (block/bio.c:1471-1478), so that
 * only `r`'s answer or quarantine wakes it and no other caller's does. The
 * queue is published in `r->waiter` under `req_lock` before the sleep and
 * taken back under it after, and the waker wakes it under the same lock
 * (`req_wake_caller`), so the waker is done with the queue before it goes.
 * An answer before the queue is published is seen by the condition.
 */
static void req_wait(struct block_request *r)
{
    wait_queue_t done;
    uint32_t flags;

    wait_queue_init(&done);
    flags = pool_lock();
    r->waiter = &done;
    pool_unlock(flags);
    while (!req_done(r)) {
        wq_wait_event_nointr(&done, req_done, r);
    }
    flags = pool_lock();
    r->waiter = NULL;
    pool_unlock(flags);
    current_proc->disk_waits++;
}

/**
 * One request of at most the device's `max_sectors` on a sleeping device.
 *
 * Sleeps until the completion, or until the request has passed its deadline
 * as many times as the device's periods allow. A signal does not end the
 * wait: the device owns the buffer until it answers, so the wait is the
 * uninterruptible kind, bounded by those periods instead. A quarantined
 * request is left to the late completion to free, and its caller gets
 * `-EIO`. A read's bytes are copied out with interrupts on, once the device
 * has answered and let the buffer go: up to a mebibyte, too long to hold the
 * machine's interrupts for. Returns -ENOMEM when the heap had no buffer for
 * the request, which the caller may retry smaller.
 */
static int req_run_one(block_device_t *device, int write, uint64_t lba, uint32_t count,
                       uint8_t *buffer, uint32_t req_flags)
{
    int why;
    struct block_request *r = req_take(device, write, lba, count, 1, &why);
    uint32_t flags;
    int status;

    if (!r) {
        return why;
    }
    r->flags = req_flags;
    if (write) {
        k_memcpy(r->page, buffer, count * BLOCK_SECTOR_SIZE);
    }
    if (device->submit(device, r) != 0) {
        flags = pool_lock();
        req_refused(r);
        pool_unlock(flags);
        wq_wake_all(&pool_wq);
        return -EIO;
    }
    req_wait(r);
    flags = pool_lock();
    if (r->state == BLOCK_REQ_QUARANTINED) {
        release_total++;
        release_tick = req_ticks;
        req_let_go(r, 1);
        pool_unlock(flags);
        return -EIO;
    }
    status = r->status;
    pool_unlock(flags);
    if (status == 0 && !write) {
        k_memcpy(buffer, r->page, count * BLOCK_SECTOR_SIZE);
    }
    flags = pool_lock();
    req_let_go(r, 1);
    pool_unlock(flags);
    wq_wake_all(&pool_wq);
    return status;
}

int block_request(block_device_t *device, int write, uint64_t lba, uint32_t count, void *buffer)
{
    return block_request_flags(device, write, lba, count, buffer, 0);
}

static int transfer(block_device_t *device, int write, uint64_t lba, uint32_t count, void *buffer,
                    uint32_t req_flags);

/* Count a transfer of `count` sectors that took `us` microseconds in the
 * statistics of `s`, as Linux counts one in /proc/diskstats. */
static void count_transfer(struct block_stats *s, int write, uint32_t count, uint32_t us)
{
    uint32_t ms = us / 1000u;

    if (write) {
        s->writes++;
        s->write_sectors += count;
        s->write_ms += ms;
    } else {
        s->reads++;
        s->read_sectors += count;
        s->read_ms += ms;
    }
    s->busy_ms += ms;
}

/** Add `delta`, 1 or -1, to the requests in progress on `device` and on
 * `disk` under it, when that is another device. */
static void count_in_flight(block_device_t *device, block_device_t *disk, int delta)
{
    uint32_t flags = pool_lock();

    device->stats.in_flight += (uint32_t)delta;
    if (disk && disk != device) {
        disk->stats.in_flight += (uint32_t)delta;
    }
    pool_unlock(flags);
}

int block_request_flags(block_device_t *device, int write, uint64_t lba, uint32_t count,
                        void *buffer, uint32_t req_flags)
{
    block_device_t *disk = device ? block_physical(device) : 0;
    uint32_t start = timer_now_us();
    uint32_t flags;
    int rc;

    if (!device) {
        return -EIO;
    }
    count_in_flight(device, disk, 1);
    rc = transfer(device, write, lba, count, buffer, req_flags);
    count_in_flight(device, disk, -1);
    if (rc == 0) {
        uint32_t us = timer_now_us() - start;

        flags = pool_lock();
        count_transfer(&device->stats, write, count, us);
        if (disk && disk != device) {
            count_transfer(&disk->stats, write, count, us);
        }
        pool_unlock(flags);
    }
    return rc;
}

/**
 * Whether a caller outside a system call may still call a device that sleeps
 * its caller (`BLOCK_CALLER_SYNC`): only the boot, before the scheduler is
 * on, when no other context runs, so that nothing the device's chain waits
 * for can be held by another and nothing in it sleeps. It reaches the device
 * below by the same rules as any caller that cannot sleep. That is how the
 * boot mounts a root from an image file and loads /sbin/init from it
 * (`rach2.rootimage=`).
 */
static int caller_sync_in_place(void)
{
    return !sched_is_enabled();
}

/**
 * The transfer of a device that sleeps its caller and answers in the same
 * call, `BLOCK_CALLER_SYNC`, which is called directly and takes no request.
 * On a `BLOCK_WRITE_THROUGH` one, a write without `BLOCK_REQ_NOFLUSH` is
 * followed by the device's flush in the same call.
 */
static int transfer_caller_sync(block_device_t *device, int write, uint64_t lba, uint32_t count,
                                void *buffer, uint32_t req_flags)
{
    int rc;

    if (!kirq_enabled() || !current_proc || !current_proc->in_syscall) {
        if (!caller_sync_in_place()) {
            k_panic("block: a sleeping device was reached outside syscall context");
        }
    } else {
        current_proc->disk_waits++; /* the device sleeps the caller in the call */
    }
    if (!write) {
        return device->read ? device->read(device, lba, count, buffer) : -EIO;
    }
    rc = device->write ? device->write(device, lba, count, buffer) : -EIO;
    if (rc == 0 && (device->flags & BLOCK_WRITE_THROUGH) && !(req_flags & BLOCK_REQ_NOFLUSH)) {
        rc = device->flush(device, 0);
    }
    return rc;
}

/**
 * `block_request_flags` without the counting: choose the path by the
 * device's flags. A device that sleeps its caller and answers in the same
 * call takes no request, since the device under it takes one while it
 * waits, and two requests for one transfer can fill the pool with callers
 * waiting on each other. A caller outside a system call, such as a page
 * fault with interrupts off, polls a `BLOCK_POLLABLE` device; any other
 * sleeping device reached there stops the kernel.
 */
static int transfer(block_device_t *device, int write, uint64_t lba, uint32_t count, void *buffer,
                    uint32_t req_flags)
{
    uint8_t *bytes = buffer;
    uint32_t most;

    if (!device || count == 0 || lba >= device->sector_count ||
        count > device->sector_count - lba) {
        return -EIO;
    }
    if (block_flags(device) & BLOCK_FAULT_SAFE) {
        most = block_max_sectors(device);
        while (count > 0) {
            uint32_t chunk = count > most ? most : count;
            int rc = write ? device->write(device, lba, chunk, bytes)
                           : device->read(device, lba, chunk, bytes);

            if (rc != 0) {
                return rc;
            }
            lba += chunk;
            bytes += chunk * BLOCK_SECTOR_SIZE;
            count -= chunk;
        }
        return 0;
    }
    if (device->flags & BLOCK_CALLER_SYNC) {
        return transfer_caller_sync(device, write, lba, count, buffer, req_flags);
    }
    if (!device->submit) {
        return -EIO;
    }
    if (!kirq_enabled() || !current_proc || !current_proc->in_syscall) {
        if (block_flags(device) & BLOCK_POLLABLE) {
            if (write) {
                return device->write(device, lba, count, buffer);
            }
            return device->read(device, lba, count, buffer);
        }
        k_panic("block: a sleeping device was reached outside syscall context");
    }
    most = block_max_sectors(device);
    while (count > 0) {
        uint32_t chunk = count > most ? most : count;
        int rc = req_run_one(device, write, lba, chunk, bytes, req_flags);

        if (rc == -ENOMEM && chunk > BLOCK_REQ_SECTORS) {
            most = chunk / 2 < BLOCK_REQ_SECTORS ? BLOCK_REQ_SECTORS : chunk / 2;
            continue;
        }
        if (rc == -ENOMEM) {
            return -EIO; /* no buffer even at a page's size */
        }
        if (rc != 0) {
            return rc;
        }
        lba += chunk;
        bytes += chunk * BLOCK_SECTOR_SIZE;
        count -= chunk;
    }
    return 0;
}

/**
 * `block_flush` on `device`, the submitter. A caller outside a system call,
 * such as a page fault writing through the page cache with interrupts off,
 * flushes a `BLOCK_POLLABLE` disk by polling.
 */
static int flush_device(block_device_t *device)
{
    struct block_request *r;
    uint32_t flags;
    int status;

    if ((device->flags & BLOCK_WRITE_THROUGH) && !device->unflushed) {
        return 0; /* every write it answered was flushed before the answer */
    }
    if (!kirq_enabled() || !current_proc || !current_proc->in_syscall) {
        if ((block_flags(device) & BLOCK_POLLABLE) ||
            ((device->flags & BLOCK_CALLER_SYNC) && caller_sync_in_place())) {
            return device->flush(device, 0);
        }
        k_panic("block: a sleeping device was reached outside syscall context");
    }
    if (device->flags & BLOCK_CALLER_SYNC) {
        current_proc->disk_waits++;
        return device->flush(device, 0); /* answers in this call, no request */
    }
    r = req_take(device, 1, 0, 1, 1, 0);
    if (!r) {
        return -EIO;
    }
    r->flags = BLOCK_REQ_FLUSH;
    if (device->flush(device, r) != 0) {
        flags = pool_lock();
        req_refused(r);
        pool_unlock(flags);
        wq_wake_all(&pool_wq);
        return -EIO;
    }
    req_wait(r);
    flags = pool_lock();
    if (r->state == BLOCK_REQ_QUARANTINED) {
        release_total++;
        release_tick = req_ticks;
        req_let_go(r, 1);
        pool_unlock(flags);
        return -EIO;
    }
    status = r->status;
    req_let_go(r, 1);
    pool_unlock(flags);
    wq_wake_all(&pool_wq);
    return status;
}

int block_flush(block_device_t *device)
{
    device = block_submitter(device);
    if (!device || !device->flush) {
        return 0; /* nothing of its own to flush: what was written is on it */
    }
    return flush_device(device);
}

struct block_request *block_request_begin(block_device_t *device, uint64_t lba, uint32_t count,
                                          uint32_t *generation)
{
    struct block_request *r;
    uint32_t flags;

    if (!device || !device->submit || count == 0 || count > block_max_sectors(device) ||
        lba >= device->sector_count || count > device->sector_count - lba) {
        return NULL;
    }
    r = req_take(device, 0, lba, count, 0, 0);
    if (!r) {
        return NULL;
    }
    *generation = r->generation;
    if (device->submit(device, r) != 0) {
        flags = pool_lock();
        req_refused(r);
        pool_unlock(flags);
        wq_wake_all(&pool_wq);
        return NULL;
    }
    return r;
}

int block_request_ended(struct block_request *r, uint32_t generation, void *buffer, int *status)
{
    uint32_t flags = pool_lock();

    if (r->state == BLOCK_REQ_IN_FLIGHT && r->generation == generation) {
        pool_unlock(flags);
        return 0;
    }
    if (r->state != BLOCK_REQ_DONE || r->generation != generation) {
        req_let_go(r, 1);
        pool_unlock(flags);
        *status = -EIO;
        return 1;
    }
    *status = r->status;
    if (r->status == 0) {
        k_memcpy(buffer, r->page, r->count * BLOCK_SECTOR_SIZE);
    }
    req_let_go(r, 1);
    pool_unlock(flags);
    wq_wake_all(&pool_wq);
    return 1;
}

/**
 * A request freed since the device captured its address is not on the list,
 * and one made since at the same address has a generation of its own: the
 * completion is stale either way. A late answer to a quarantined request
 * clears its device's stall. A completion that finds another request in
 * flight is counted for `disk-overlaps`, which a test reads to show that one
 * device's queue did not wait on another's. An answer wakes its own caller
 * alone (`req_wake_caller`), and the waiters for room in the pool.
 */
void block_request_complete(struct block_request *request, uint32_t generation, int status)
{
    uint32_t flags = pool_lock();
    int answered = 0;
    uint32_t overdue;
    uint64_t cpus = 0;

    if (!request || !req_is_live(request) || request->generation != generation) {
        pool_unlock(flags);
        klog(KLOG_WARN, "block", "completion dropped: stale generation");
        return;
    }
    if (request->state == BLOCK_REQ_QUARANTINED) {
        quarantined_count--;
        if (request->owner->stalled) {
            request->owner->stalled = 0;
            answered = 1;
        }
        request->state = BLOCK_REQ_DONE;
        req_let_go(request, 0);
        pool_unlock(flags);
        klog(KLOG_INFO, "block", "late completion: quarantined slot freed");
        if (answered) {
            klog(KLOG_INFO, "block", "device answered: stall cleared");
        }
        wq_wake_all(&pool_wq);
        return;
    }
    if (request->state != BLOCK_REQ_IN_FLIGHT) {
        pool_unlock(flags);
        return;
    }
    request->status = status;
    request->state = BLOCK_REQ_DONE;
    req_wake_caller(request, &cpus);
    req_count_rest(request);
    latency_span(LAT_DISK, request->taken_us, timer_now_us());
    {
        const struct block_request *r;

        for (r = reqs; r; r = r->next) {
            if (r != request && r->state == BLOCK_REQ_IN_FLIGHT) {
                overlap_count++;
                break;
            }
        }
    }
    overdue = request->overdue;
    req_let_go(request, 0);
    pool_unlock(flags);
    sched_kick_cpus(cpus);
    if (overdue) {
        klog(KLOG_INFO, "block", "late answer: taken after %d deadlines", (int)overdue);
    }
    wq_wake_all(&pool_wq);
}

int block_device_holds(block_device_t *device)
{
    uint32_t flags = pool_lock();
    const struct block_request *r;
    int held = 0;

    for (r = reqs; r; r = r->next) {
        if (r->device == device &&
            (r->state == BLOCK_REQ_IN_FLIGHT || r->state == BLOCK_REQ_QUARANTINED)) {
            held++;
        }
    }
    pool_unlock(flags);
    return held;
}

/**
 * `req_lock` held. A device holding requests with none of them in flight has
 * stopped answering: it is marked stalled, once, so its waiters are
 * released and its later callers refused until a late answer clears it.
 * Returns how many devices were marked by this tick.
 */
static int mark_stalled(void)
{
    const struct block_request *r;
    int marked = 0;

    for (r = reqs; r; r = r->next) {
        block_device_t *owner = r->owner;

        if (r->state != BLOCK_REQ_QUARANTINED || owner->stalled) {
            continue;
        }
        if (device_in_flight(owner) == 0) {
            owner->stalled = 1;
            marked++;
        }
    }
    return marked;
}

uint32_t block_overlaps(void)
{
    return overlap_count;
}

uint32_t block_in_flight(void)
{
    uint32_t flags = pool_lock();
    const struct block_request *r;
    uint32_t n = 0;

    for (r = reqs; r; r = r->next) {
        if (r->state == BLOCK_REQ_IN_FLIGHT) {
            n++;
        }
    }
    pool_unlock(flags);
    return n;
}

uint32_t block_quarantines(void)
{
    return quarantine_total;
}

uint32_t block_releases(void)
{
    return release_total;
}

uint32_t block_release_tick(void)
{
    return release_tick;
}

/**
 * Every tick a request spends in flight is counted as a wait tick, whether a
 * caller sleeps for it or the USB pump left it running, here or, for the ticks
 * since the last call, at its completion (`req_count_rest`); the suites bound
 * that count on every device. A request quarantined keeps its generation, so
 * the late completion still matches it. The clock advances by the ticks counted
 * since the last call, not by one a call: the boot processor's tick that finds
 * the big lock held ends without calling this (`timer_lock_take`, kernel.c),
 * and a late interrupt counts several ticks. A request taken between two
 * calls counts only the ticks since it was taken, read from the clock as
 * `req_clock` gives it then.
 */
void block_request_tick(void)
{
    struct block_request *r;
    uint32_t flags = pool_lock();
    uint32_t now = (uint32_t)kernel_ticks, passed = now - req_seen;
    int overdue = 0;
    int quarantined = 0;
    int stalled = 0;
    uint64_t cpus = 0;

    req_seen = now;
    req_ticks += passed;
    for (r = reqs; r; r = r->next) {
        if (r->state != BLOCK_REQ_IN_FLIGHT) {
            continue;
        }
        block_count_wait_ticks(req_ticks - r->taken_tick < passed ? req_ticks - r->taken_tick
                                                                  : passed);
        if ((int32_t)(req_ticks - r->deadline) < 0) {
            continue;
        }
        r->overdue++;
        if (r->overdue < req_periods(r->device)) {
            r->deadline += req_period(r->device);
            overdue++;
            continue;
        }
        r->state = BLOCK_REQ_QUARANTINED;
        req_wake_caller(r, &cpus);
        quarantined_count++;
        quarantine_total++;
        quarantined++;
    }
    if (quarantined) {
        stalled = mark_stalled();
    }
    pool_unlock(flags);
    sched_kick_cpus(cpus);
    if (overdue) {
        klog(KLOG_WARN, "block", "request overdue: %d past a deadline, still waited for", overdue);
    }
    if (!quarantined) {
        return;
    }
    klog(KLOG_WARN, "block", "request deadline: %d slots quarantined", quarantined);
    if (stalled) {
        klog(KLOG_WARN, "block", "device stalled: every slot it holds is past its deadline");
    }
    wq_wake_all(&pool_wq);
}
