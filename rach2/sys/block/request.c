/*
 * request.c: the one entry every transfer takes, and the pool behind it.
 *
 * A polled device is called and done. A sleeping device is handed a request
 * from a fixed pool, one per process, each with its own page: a write is
 * copied into the page before submit and a read is copied out after, so a
 * completion that arrives after the caller gave up lands in a page nothing
 * else owns. A timed-out slot is quarantined with its page until the device
 * answers late, so a late answer never reaches memory that has been reused.
 */

#include "block.h"

#include "../klog.h"
#include "../kio.h"
#include "../lib/kerrno.h"
#include "../lib/kstring.h"
#include "../panic.h"
#include "../proc/process.h"
#include "../proc/wait_queue.h"

#define REQ_POOL             16
#define REQ_DEFAULT_DEADLINE 500u /* five seconds of 100 Hz ticks */

static struct block_request pool[REQ_POOL];
static uint8_t pool_pages[REQ_POOL][BLOCK_REQ_SECTORS * BLOCK_SECTOR_SIZE]
    __attribute__((aligned(4096)));
static wait_queue_t pool_wq; /* woken on any completion or freed slot */
static volatile uint32_t req_ticks;
static uint32_t quarantined_count;

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

/** A slot to submit with, or NULL when the pool is exhausted. */
static struct block_request *req_take(block_device_t *device, int write, uint64_t lba,
                                      uint32_t count)
{
    uint32_t flags = irq_save();
    int i;

    for (i = 0; i < REQ_POOL; i++) {
        if (pool[i].state == BLOCK_REQ_FREE) {
            struct block_request *r = &pool[i];

            r->device = device;
            r->write = write;
            r->lba = lba;
            r->count = count;
            r->page = pool_pages[i];
            r->status = -EIO;
            r->state = BLOCK_REQ_IN_FLIGHT;
            r->generation++;
            r->deadline = req_ticks +
                          (device->deadline_ticks ? device->deadline_ticks : REQ_DEFAULT_DEADLINE);
            irq_restore(flags);
            return r;
        }
    }
    irq_restore(flags);
    klog(KLOG_WARN, "block", "request pool exhausted: %d quarantined", (int)quarantined_count);
    return NULL;
}

/* What one caller waits for: its own request, by slot and generation. */
struct req_wait {
    struct block_request *r;
    uint32_t generation;
};

static int req_done(void *arg)
{
    const struct req_wait *w = arg;
    return w->r->state != BLOCK_REQ_IN_FLIGHT || w->r->generation != w->generation;
}

/**
 * One request of at most a page on a sleeping device.
 *
 * Sleeps until the completion or the deadline. A signal does not end the
 * wait: the device owns the page until it answers, so the wait is the
 * uninterruptible kind, bounded by the deadline instead.
 */
static int req_run_one(block_device_t *device, int write, uint64_t lba, uint32_t count,
                       uint8_t *buffer)
{
    struct block_request *r = req_take(device, write, lba, count);
    struct req_wait w;
    uint32_t flags;
    int status;

    if (!r) {
        return -EIO;
    }
    w.r = r;
    w.generation = r->generation;
    if (write) {
        k_memcpy(r->page, buffer, count * BLOCK_SECTOR_SIZE);
    }
    if (device->submit(device, r) != 0) {
        flags = irq_save();
        r->state = BLOCK_REQ_FREE;
        irq_restore(flags);
        wq_wake_all(&pool_wq);
        return -EIO;
    }
    while (!req_done(&w)) {
        wq_wait_event_nointr(&pool_wq, req_done, &w);
    }
    flags = irq_save();
    /* A quarantined slot, or one the late completion freed and another
     * caller took (the generation moved on): the request is not ours to
     * touch any more. */
    if (r->state == BLOCK_REQ_QUARANTINED || r->generation != w.generation) {
        irq_restore(flags);
        return -EIO;
    }
    status = r->status;
    if (status == 0 && !write) {
        k_memcpy(buffer, r->page, count * BLOCK_SECTOR_SIZE);
    }
    r->state = BLOCK_REQ_FREE;
    irq_restore(flags);
    wq_wake_all(&pool_wq);
    return status;
}

int block_request(block_device_t *device, int write, uint64_t lba, uint32_t count, void *buffer)
{
    uint8_t *bytes = buffer;
    uint32_t eflags;

    if (!device || count == 0 || lba >= device->sector_count ||
        count > device->sector_count - lba) {
        return -EIO;
    }
    if (device->flags & BLOCK_FAULT_SAFE) {
        if (write) {
            return device->write(device, lba, count, buffer);
        }
        return device->read(device, lba, count, buffer);
    }
    if (!device->submit) {
        return -EIO;
    }
    __asm__ __volatile__("pushfl; popl %0" : "=r"(eflags));
    if (!(eflags & 0x200) || !current_proc || !current_proc->in_syscall) {
        k_panic("block: a sleeping device was reached outside syscall context");
    }
    while (count > 0) {
        uint32_t chunk = count > BLOCK_REQ_SECTORS ? BLOCK_REQ_SECTORS : count;
        int rc = req_run_one(device, write, lba, chunk, bytes);

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
    struct req_wait w;
    uint32_t flags;
    int status;
    uint32_t eflags;

    if (!device || !device->flush) {
        return 0; /* nothing of its own to flush: what was written is on it */
    }
    __asm__ __volatile__("pushfl; popl %0" : "=r"(eflags));
    if (!(eflags & 0x200) || !current_proc || !current_proc->in_syscall) {
        k_panic("block: a sleeping device was reached outside syscall context");
    }
    r = req_take(device, 1, 0, 1);
    if (!r) {
        return -EIO;
    }
    w.r = r;
    w.generation = r->generation;
    if (device->flush(device, r) != 0) {
        flags = irq_save();
        r->state = BLOCK_REQ_FREE;
        irq_restore(flags);
        wq_wake_all(&pool_wq);
        return -EIO;
    }
    while (!req_done(&w)) {
        wq_wait_event_nointr(&pool_wq, req_done, &w);
    }
    flags = irq_save();
    if (r->state == BLOCK_REQ_QUARANTINED || r->generation != w.generation) {
        irq_restore(flags);
        return -EIO;
    }
    status = r->status;
    r->state = BLOCK_REQ_FREE;
    irq_restore(flags);
    wq_wake_all(&pool_wq);
    return status;
}

struct block_request *block_request_begin(block_device_t *device, uint64_t lba, uint32_t count,
                                          uint32_t *generation)
{
    struct block_request *r;
    uint32_t flags;

    if (!device || !device->submit || count == 0 || count > BLOCK_REQ_SECTORS ||
        lba >= device->sector_count || count > device->sector_count - lba) {
        return NULL;
    }
    r = req_take(device, 0, lba, count);
    if (!r) {
        return NULL;
    }
    *generation = r->generation;
    if (device->submit(device, r) != 0) {
        flags = irq_save();
        r->state = BLOCK_REQ_FREE;
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
    /* Quarantined, or freed by a late completion and taken by another
     * caller: the request is not ours to touch any more, and the slot is
     * the pool's to free. */
    if (r->state != BLOCK_REQ_DONE || r->generation != generation) {
        irq_restore(flags);
        *status = -EIO;
        return 1;
    }
    *status = r->status;
    if (r->status == 0) {
        k_memcpy(buffer, r->page, r->count * BLOCK_SECTOR_SIZE);
    }
    r->state = BLOCK_REQ_FREE;
    irq_restore(flags);
    wq_wake_all(&pool_wq);
    return 1;
}

void block_request_complete(struct block_request *request, uint32_t generation, int status)
{
    uint32_t flags = irq_save();

    if (!request || request->generation != generation) {
        irq_restore(flags);
        klog(KLOG_WARN, "block", "completion dropped: stale generation");
        return;
    }
    if (request->state == BLOCK_REQ_QUARANTINED) {
        request->generation++; /* the caller that gave up must not match */
        request->state = BLOCK_REQ_FREE;
        quarantined_count--;
        irq_restore(flags);
        klog(KLOG_INFO, "block", "late completion: quarantined slot freed");
        wq_wake_all(&pool_wq);
        return;
    }
    if (request->state != BLOCK_REQ_IN_FLIGHT) {
        irq_restore(flags);
        return;
    }
    request->status = status;
    request->state = BLOCK_REQ_DONE;
    irq_restore(flags);
    wq_wake_all(&pool_wq);
}

int block_device_holds(block_device_t *device)
{
    int i;
    int held = 0;
    uint32_t flags = irq_save();

    for (i = 0; i < REQ_POOL; i++) {
        if (pool[i].device != device) {
            continue;
        }
        if (pool[i].state == BLOCK_REQ_IN_FLIGHT || pool[i].state == BLOCK_REQ_QUARANTINED) {
            held++;
        }
    }
    irq_restore(flags);
    return held;
}

void block_request_tick(void)
{
    int i;

    req_ticks++;
    for (i = 0; i < REQ_POOL; i++) {
        if (pool[i].state == BLOCK_REQ_IN_FLIGHT && (int32_t)(req_ticks - pool[i].deadline) >= 0) {
            /* The caller is woken with EIO; the slot and its page wait for
             * the device, which still owns them. The generation stays so the
             * late completion matches, and moves on when it frees the slot. */
            pool[i].state = BLOCK_REQ_QUARANTINED;
            quarantined_count++;
            klog(KLOG_WARN, "block", "request deadline: slot quarantined");
            wq_wake_all(&pool_wq);
        }
    }
}
