/*
 * request.c: the one entry every transfer takes, and the requests behind it.
 *
 * A device that runs the transfer in its caller, `BLOCK_CALLER_SYNC`, is
 * called and done and takes no request. A sleeping device is handed a request
 * allocated for the transfer, with a buffer of its own from the
 * identity-mapped window, one page for a transfer of a page or less and heap
 * pages for a larger one, since the device is given a physical address: a
 * write is
 * copied into the page before submit and a read is copied out after, so a
 * completion that arrives after the caller gave up lands in a page nothing
 * else owns. A request is held by its caller and by its device, and is freed
 * when both have let go: a timed-out request is quarantined with its page
 * until the device answers late, so a late answer never reaches memory that
 * has been reused.
 *
 * How many requests may be live at once is a policy, `rach2.blockpool=N`,
 * sixteen unless set, and a caller that finds the limit reached waits.
 * Running out is not a failure of the device: a caller waits. A transfer
 * that failed answers `EIO`, and so does a request the heap cannot hold even
 * at one page, which is the memory running out and not the disk. The
 * deadline of a request starts when it is taken, which is when the transfer
 * starts, and not when the caller began to wait.
 * A request past its deadline is still waited for: the deadline is a period,
 * and only a request unanswered through a bounded number of periods in a
 * row is given up, since a disk that answers late is not a disk that went
 * away. Two rules keep a waiter from waiting for ever on requests that will
 * never free themselves. Failure is per device, where a device is the one
 * whose `submit` answers, a stick and not each of its partition views: a
 * device whose every request is quarantined past its periods is stalled,
 * and its waiters and its later callers answer `EIO` until it answers
 * something late. And no device may hold more than half of the limit,
 * rounded down but never to zero, so a stalled device leaves the other half
 * to the healthy ones; at `rach2.blockpool=1` the half is the whole pool and
 * that separation is gone. A waiter that
 * finds every live request quarantined is released with `EIO` too, since
 * nothing but a late answer could free one.
 */

#include "block.h"

#include "../cmdline.h"
#include "../klog.h"
#include "../kio.h"
#include "../lib/kerrno.h"
#include "../latency.h"
#include "../lib/kstring.h"
#include "../mm/kheap.h"
#include "../mm/paging.h"
#include "../panic.h"
#include "../pit.h"
#include "../proc/process.h"
#include "../proc/wait_queue.h"

/* Policy: how many requests may be live at once unless `rach2.blockpool`
 * says otherwise, and the most the option accepts. The limit is what the
 * stall rules divide between devices; it is not a table. */
#define REQ_LIMIT             16
/* The deadline's period, thirty seconds of 100 Hz ticks, and how many
 * periods in a row a request is waited for before it is given up: two
 * minutes in all. The deadline is here to answer a caller whose device has
 * gone, not to judge a slow one: an emulated disk on a host running twenty
 * machines at once has taken more than thirty seconds to answer a write,
 * and with one period the mount was lost to it; an answer inside the
 * periods is the answer. A device that truly never answers is what the lag
 * device tests, and it sets its own period of five seconds, one of them.
 * `rach2.blockdeadline=S` sets the period in seconds, so a row can see a
 * request answered after its deadline without waiting thirty seconds. */
#define REQ_DEFAULT_DEADLINE  3000u
#define REQ_DEFAULT_DEADLINES 4u
#define REQ_DEADLINE_MAX_S    600u
static uint32_t default_deadline = REQ_DEFAULT_DEADLINE;

static struct block_request *reqs; /* every live request */
static uint32_t live;              /* how many are on the list */
static uint32_t next_generation;   /* rising; see req_new */
static wait_queue_t pool_wq;       /* woken on any completion, freed request or quarantine */
static volatile uint32_t req_ticks;
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

static uint32_t irq_save(void)
{
    uint32_t flags;
    __asm__ __volatile__("pushfl; popl %0; cli" : "=r"(flags)::"memory");
    return flags;
}

static void irq_restore(uint32_t flags)
{
    if (flags & 0x200) {
        __asm__ __volatile__("sti");
    }
}

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
            default_deadline = asked * 100u;
            klog(KLOG_INFO, "block", "request deadline: %d s a period", (int)asked);
        }
    }
    cap = n / 2;
    if (cap == 0) {
        cap = 1;
    }
    /* Half of the limit each, so a stalled device leaves room for a healthy
     * one. The share first and the size last: a caller from the USB pump
     * reads both between two stores, and a size with no share would refuse
     * it. */
    device_cap = cap;
    pool_size = n;
}

/** Interrupts off. Whether `r` is on the list of live requests. */
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

/** Interrupts off. Take `r` off the list and give its page and record back. */
static void req_drop(struct block_request *r)
{
    struct block_request **link;

    for (link = &reqs; *link != r; link = &(*link)->next) {
    }
    *link = r->next;
    live--;
    if (r->pages > 1) {
        kfree_pages(r->page, r->pages);
    } else {
        free_kernel_frame(r->page);
    }
    kfree(r);
}

/**
 * Interrupts off. One holder lets go: the caller when it has read the
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
 * Interrupts off. How many requests `owner` holds: in flight or quarantined.
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

/** Interrupts off. How many of `owner`'s requests are still in flight. */
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
 * Interrupts off. Whether no request is in flight or answered and not yet
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
 * Interrupts off. A fresh request on the list for `count` sectors, held by
 * its caller and its device, or NULL when the heap or the window has no
 * room for one. A transfer of a page or less takes a frame of the window,
 * whose address is physical for every device; a larger one takes heap pages,
 * which the devices that describe a buffer page by page ask for: the AHCI
 * controller, up to 2048 sectors, and USB mass storage, up to 1024.
 *
 * The generation rises with every request and is what a late completion is
 * matched against. It is 32 bits and wraps, which would need 4 billion
 * requests and a quarantined request still live from the round before.
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
    uint32_t flags = irq_save();
    int ready = (owner->flags & BLOCK_STALLED) != 0 || nothing_pending() ||
                (live < pool_size && device_held(owner) < device_cap);

    irq_restore(flags);
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
 * A request to submit, or NULL.
 *
 * With `wait` the caller sleeps until a request is its to take, and NULL
 * then means the device is stalled, the limit is reached with nothing in
 * flight, so only a late answer could free one, or there is no memory for
 * one. Without `wait`, for a caller that cannot sleep, NULL means no
 * request now.
 */
static struct block_request *req_take(block_device_t *device, int write, uint64_t lba,
                                      uint32_t count, int wait, int *why)
{
    block_device_t *owner = block_submitter(device);
    int waited = 0;
    uint32_t wait_from = pit_now_us();

    if (why) {
        *why = -EIO;
    }

    block_request_init();
    for (;;) {
        uint32_t flags = irq_save();
        struct block_request *r = NULL;
        int share_taken;

        if (owner->flags & BLOCK_STALLED) {
            if (waited) {
                release_total++;
                release_tick = req_ticks;
            }
            irq_restore(flags);
            klog(KLOG_WARN, "block", "device stalled: %s",
                 waited ? "waiter released with EIO" : "request refused");
            return NULL;
        }
        share_taken = device_held(owner) >= device_cap;
        if (!share_taken && live < pool_size) {
            r = req_new(count);
            if (!r) {
                irq_restore(flags);
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
            r->deadline = req_ticks + req_period(device);
            r->overdue = 0;
            r->taken_us = pit_now_us();
            r->flags = 0;
            if (waited) {
                latency_span(LAT_DISK_WAIT, wait_from, r->taken_us);
            }
            irq_restore(flags);
            return r;
        }
        if (!wait) {
            irq_restore(flags);
            return NULL;
        }
        if (nothing_pending()) {
            irq_restore(flags);
            klog(KLOG_WARN, "block", "request pool stalled: %d quarantined, waiter released",
                 (int)quarantined_count);
            return NULL;
        }
        irq_restore(flags);
        if (!waited) {
            waited = 1;
            klog(KLOG_INFO, "block", "request pool: waiting for a slot: %s",
                 share_taken ? "the device holds its share" : "the pool is full");
        }
        wq_wait_event_nointr(&pool_wq, slot_ready, owner);
    }
}

/**
 * Interrupts off. The device refused `submit`, so it never held the
 * request: the caller alone lets go and the request is freed.
 */
static void req_refused(struct block_request *r)
{
    r->held_by_device = 0;
    req_let_go(r, 1);
}

static int req_done(void *arg)
{
    const struct block_request *r = arg;
    return r->state != BLOCK_REQ_IN_FLIGHT;
}

/**
 * One request of at most the device's `max_sectors` on a sleeping device.
 *
 * Sleeps until the completion, or until the request has passed its deadline
 * as many times as the device's periods allow. A signal does not end the
 * wait: the device owns the page until it answers, so the wait is the
 * uninterruptible kind, bounded by those periods instead. A read's bytes
 * are copied out with interrupts on, once the device has answered and let
 * the buffer go: up to a mebibyte, too long to hold the machine's
 * interrupts for. Returns -ENOMEM when the heap had no buffer for the
 * request, which the caller may retry smaller.
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
        flags = irq_save();
        req_refused(r);
        irq_restore(flags);
        wq_wake_all(&pool_wq);
        return -EIO;
    }
    while (!req_done(r)) {
        wq_wait_event_nointr(&pool_wq, req_done, r);
    }
    flags = irq_save();
    /* Quarantined: the device still owns the page, and the request is the
     * late completion's to free. */
    if (r->state == BLOCK_REQ_QUARANTINED) {
        release_total++;
        release_tick = req_ticks;
        req_let_go(r, 1);
        irq_restore(flags);
        return -EIO;
    }
    status = r->status;
    irq_restore(flags);
    if (status == 0 && !write) {
        k_memcpy(buffer, r->page, count * BLOCK_SECTOR_SIZE);
    }
    flags = irq_save();
    req_let_go(r, 1);
    irq_restore(flags);
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

int block_request_flags(block_device_t *device, int write, uint64_t lba, uint32_t count,
                        void *buffer, uint32_t req_flags)
{
    block_device_t *disk = device ? block_physical(device) : 0;
    uint32_t start = pit_now_us();
    int rc;

    if (!device) {
        return -EIO;
    }
    device->stats.in_flight++;
    if (disk && disk != device) {
        disk->stats.in_flight++;
    }
    rc = transfer(device, write, lba, count, buffer, req_flags);
    device->stats.in_flight--;
    if (disk && disk != device) {
        disk->stats.in_flight--;
    }
    if (rc == 0) {
        uint32_t us = pit_now_us() - start;

        count_transfer(&device->stats, write, count, us);
        if (disk && disk != device) {
            count_transfer(&disk->stats, write, count, us);
        }
    }
    return rc;
}

/* `block_request_flags` without the counting. */
static int transfer(block_device_t *device, int write, uint64_t lba, uint32_t count, void *buffer,
                    uint32_t req_flags)
{
    uint8_t *bytes = buffer;
    uint32_t eflags, most;

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
    /* A device that sleeps its caller and answers in the same call is called
     * directly and takes no request: the device under it takes one while it
     * waits, and two requests for one transfer is how the limit is reached
     * with everybody waiting for everybody. */
    if (device->flags & BLOCK_CALLER_SYNC) {
        __asm__ __volatile__("pushfl; popl %0" : "=r"(eflags));
        if (!(eflags & 0x200) || !current_proc || !current_proc->in_syscall) {
            k_panic("block: a sleeping device was reached outside syscall context");
        }
        if (write) {
            return device->write ? device->write(device, lba, count, buffer) : -EIO;
        }
        return device->read ? device->read(device, lba, count, buffer) : -EIO;
    }
    if (!device->submit) {
        return -EIO;
    }
    __asm__ __volatile__("pushfl; popl %0" : "=r"(eflags));
    if (!(eflags & 0x200) || !current_proc || !current_proc->in_syscall) {
        /* A page fault reading a file, with interrupts off: a disk that can
         * be polled from here is, its queue drained first; a device that
         * cannot is the caller's mistake. */
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

int block_flush(block_device_t *device)
{
    struct block_request *r;
    uint32_t flags;
    int status;
    uint32_t eflags;

    device = block_submitter(device);
    if (!device || !device->flush) {
        return 0; /* nothing of its own to flush: what was written is on it */
    }
    if ((device->flags & BLOCK_WRITE_THROUGH) && !device->unflushed) {
        return 0; /* every write it answered was flushed before the answer */
    }
    __asm__ __volatile__("pushfl; popl %0" : "=r"(eflags));
    if (!(eflags & 0x200) || !current_proc || !current_proc->in_syscall) {
        /* A page fault writing through the page cache, with interrupts
         * off: a disk that can be polled flushes by polling. */
        if (block_flags(device) & BLOCK_POLLABLE) {
            return device->flush(device, 0);
        }
        k_panic("block: a sleeping device was reached outside syscall context");
    }
    if (device->flags & BLOCK_CALLER_SYNC) {
        return device->flush(device, 0); /* answers in this call, no request */
    }
    r = req_take(device, 1, 0, 1, 1, 0);
    if (!r) {
        return -EIO;
    }
    r->flags = BLOCK_REQ_FLUSH;
    if (device->flush(device, r) != 0) {
        flags = irq_save();
        req_refused(r);
        irq_restore(flags);
        wq_wake_all(&pool_wq);
        return -EIO;
    }
    while (!req_done(r)) {
        wq_wait_event_nointr(&pool_wq, req_done, r);
    }
    flags = irq_save();
    if (r->state == BLOCK_REQ_QUARANTINED) {
        release_total++;
        release_tick = req_ticks;
        req_let_go(r, 1);
        irq_restore(flags);
        return -EIO;
    }
    status = r->status;
    req_let_go(r, 1);
    irq_restore(flags);
    wq_wake_all(&pool_wq);
    return status;
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
        flags = irq_save();
        req_refused(r);
        irq_restore(flags);
        wq_wake_all(&pool_wq);
        return NULL;
    }
    return r;
}

int block_request_ended(struct block_request *r, uint32_t generation, void *buffer, int *status)
{
    uint32_t flags = irq_save();

    if (r->state == BLOCK_REQ_IN_FLIGHT && r->generation == generation) {
        irq_restore(flags);
        return 0;
    }
    /* Quarantined: the page is the device's until the late completion, and
     * the request is freed when both have let go. */
    if (r->state != BLOCK_REQ_DONE || r->generation != generation) {
        req_let_go(r, 1);
        irq_restore(flags);
        *status = -EIO;
        return 1;
    }
    *status = r->status;
    if (r->status == 0) {
        k_memcpy(buffer, r->page, r->count * BLOCK_SECTOR_SIZE);
    }
    req_let_go(r, 1);
    irq_restore(flags);
    wq_wake_all(&pool_wq);
    return 1;
}

void block_request_complete(struct block_request *request, uint32_t generation, int status)
{
    uint32_t flags = irq_save();
    int answered = 0;
    uint32_t overdue;

    /* A request freed since the device captured its address is not on the
     * list, and one made since at the same address has a generation of its
     * own: the completion is stale either way. */
    if (!request || !req_is_live(request) || request->generation != generation) {
        irq_restore(flags);
        klog(KLOG_WARN, "block", "completion dropped: stale generation");
        return;
    }
    if (request->state == BLOCK_REQ_QUARANTINED) {
        quarantined_count--;
        /* The device answered, late: it is not stalled any more. */
        if (request->owner->flags & BLOCK_STALLED) {
            request->owner->flags &= ~BLOCK_STALLED;
            answered = 1;
        }
        request->state = BLOCK_REQ_DONE;
        req_let_go(request, 0);
        irq_restore(flags);
        klog(KLOG_INFO, "block", "late completion: quarantined slot freed");
        if (answered) {
            klog(KLOG_INFO, "block", "device answered: stall cleared");
        }
        wq_wake_all(&pool_wq);
        return;
    }
    if (request->state != BLOCK_REQ_IN_FLIGHT) {
        irq_restore(flags);
        return;
    }
    request->status = status;
    request->state = BLOCK_REQ_DONE;
    latency_span(LAT_DISK, request->taken_us, pit_now_us());
    {
        /* A completion with another request in flight is a request the
           layer did not serialize behind it, which `disk-overlaps` counts
           for a row that proves one device's queue did not wait on
           another's. */
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
    irq_restore(flags);
    if (overdue) {
        klog(KLOG_INFO, "block", "late answer: taken after %d deadlines", (int)overdue);
    }
    wq_wake_all(&pool_wq);
}

int block_device_holds(block_device_t *device)
{
    uint32_t flags = irq_save();
    const struct block_request *r;
    int held = 0;

    for (r = reqs; r; r = r->next) {
        if (r->device == device &&
            (r->state == BLOCK_REQ_IN_FLIGHT || r->state == BLOCK_REQ_QUARANTINED)) {
            held++;
        }
    }
    irq_restore(flags);
    return held;
}

/**
 * Interrupts off. A device holding requests with none of them in flight has
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

        if (r->state != BLOCK_REQ_QUARANTINED || (owner->flags & BLOCK_STALLED)) {
            continue;
        }
        if (device_in_flight(owner) == 0) {
            owner->flags |= BLOCK_STALLED;
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
    uint32_t flags = irq_save();
    const struct block_request *r;
    uint32_t n = 0;

    for (r = reqs; r; r = r->next) {
        if (r->state == BLOCK_REQ_IN_FLIGHT) {
            n++;
        }
    }
    irq_restore(flags);
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

void block_request_tick(void)
{
    struct block_request *r;
    int overdue = 0;
    int quarantined = 0;
    int stalled;

    req_ticks++;
    for (r = reqs; r; r = r->next) {
        if (r->state != BLOCK_REQ_IN_FLIGHT) {
            continue;
        }
        /* A tick a request spends in flight, whether a caller sleeps for it
           or the pump left it running; the count is what the suites hold
           under a bound on every device. */
        block_count_wait_tick();
        if ((int32_t)(req_ticks - r->deadline) < 0) {
            continue;
        }
        r->overdue++;
        if (r->overdue < req_periods(r->device)) {
            r->deadline += req_period(r->device);
            overdue++;
            continue;
        }
        /* The caller is woken with EIO; the request and its page wait
         * for the device, which still owns them. The generation stays
         * so the late completion matches. */
        r->state = BLOCK_REQ_QUARANTINED;
        quarantined_count++;
        quarantine_total++;
        quarantined++;
    }
    if (overdue) {
        klog(KLOG_WARN, "block", "request overdue: %d past a deadline, still waited for", overdue);
    }
    if (!quarantined) {
        return;
    }
    klog(KLOG_WARN, "block", "request deadline: %d slots quarantined", quarantined);
    stalled = mark_stalled();
    if (stalled) {
        klog(KLOG_WARN, "block", "device stalled: every slot it holds is past its deadline");
    }
    wq_wake_all(&pool_wq);
}
