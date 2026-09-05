/*
 * request.c: the one entry every transfer takes, and the pool behind it.
 *
 * A polled device is called and done. A sleeping device is handed a request
 * from a fixed pool, each slot with its own page: a write is copied into the
 * page before submit and a read is copied out after, so a completion that
 * arrives after the caller gave up lands in a page nothing else owns. A
 * timed-out slot is quarantined with its page until the device answers late,
 * so a late answer never reaches memory that has been reused.
 *
 * A caller that finds no slot waits for one. Running out is not a failure
 * of the device, and only a transfer that failed answers `EIO`; the deadline
 * of a request starts when its slot is taken, which is when the transfer
 * starts, and not when the caller began to wait. Two rules keep a waiter
 * from waiting for ever on slots that will never free themselves. Failure
 * is per device: a device whose every slot is quarantined past its deadline
 * is stalled, and its waiters and its later callers answer `EIO` until it
 * answers something late. And no device may hold more than half of the
 * pool, so a stalled device leaves the other half to the healthy ones; a
 * waiter that finds no slot in flight anywhere is released with `EIO` too,
 * since nothing but a late answer could free one.
 */

#include "block.h"

#include "../cmdline.h"
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
static wait_queue_t pool_wq; /* woken on any completion, freed slot or quarantine */
static volatile uint32_t req_ticks;
static uint32_t quarantined_count;
static uint32_t pool_size;       /* slots in use: REQ_POOL, or rach2.blockpool=N */
static uint32_t device_cap;      /* the most slots one device may hold */

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

/* The pool's size, from the boot line the first time it is asked. The
 * option exists so a test can see a request wait without sixteen devices. */
static void pool_configure(void)
{
    char value[8];
    uint32_t n = 0;
    int i;

    if (pool_size) {
        return;
    }
    pool_size = REQ_POOL;
    if (kcmdline_value("rach2.blockpool", value, sizeof(value))) {
        for (i = 0; value[i] >= '0' && value[i] <= '9'; i++) {
            n = n * 10u + (uint32_t)(value[i] - '0');
        }
        if (i == 0 || value[i] != '\0' || n == 0 || n > REQ_POOL) {
            klog(KLOG_WARN, "block", "rach2.blockpool: not a count of 1 to %d; ignored",
                 REQ_POOL);
        } else {
            pool_size = n;
            klog(KLOG_INFO, "block", "request pool: %d slots", (int)n);
        }
    }
    device_cap = pool_size / 2;
    if (device_cap == 0) {
        device_cap = 1;
    }
}

/* Interrupts off. How many slots `device` holds: in flight or quarantined. */
static uint32_t device_held(const block_device_t *device)
{
    uint32_t i;
    uint32_t held = 0;

    for (i = 0; i < pool_size; i++) {
        if (pool[i].device == device &&
            (pool[i].state == BLOCK_REQ_IN_FLIGHT || pool[i].state == BLOCK_REQ_QUARANTINED)) {
            held++;
        }
    }
    return held;
}

/* Interrupts off. How many of `device`'s slots are still in flight. */
static uint32_t device_in_flight(const block_device_t *device)
{
    uint32_t i;
    uint32_t n = 0;

    for (i = 0; i < pool_size; i++) {
        if (pool[i].device == device && pool[i].state == BLOCK_REQ_IN_FLIGHT) {
            n++;
        }
    }
    return n;
}

/* Interrupts off. Whether `device` holds slots and none of them is in
 * flight: nothing but a late answer can free one, so a caller does not wait
 * on it. The flag records the tick that found it so, for the record; the
 * condition is what decides. */
static int device_stalled(block_device_t *device)
{
    if (device->flags & BLOCK_STALLED) {
        return 1;
    }
    return device_in_flight(device) == 0 && device_held(device) > 0;
}

/* Interrupts off. Whether no slot is in flight or answered and not yet
 * freed: every taken slot is quarantined, and nothing but a late answer
 * can free one. */
static int nothing_pending(void)
{
    uint32_t i;

    for (i = 0; i < pool_size; i++) {
        if (pool[i].state == BLOCK_REQ_IN_FLIGHT || pool[i].state == BLOCK_REQ_DONE) {
            return 0;
        }
    }
    return 1;
}

/* Interrupts off. A free slot, or NULL. */
static struct block_request *free_slot(void)
{
    uint32_t i;

    for (i = 0; i < pool_size; i++) {
        if (pool[i].state == BLOCK_REQ_FREE) {
            return &pool[i];
        }
    }
    return NULL;
}

/* What a waiter for a slot wakes on: one it may take, its device stalled,
 * or nothing left in flight that could free one. */
static int slot_ready(void *arg)
{
    block_device_t *device = arg;
    uint32_t flags = irq_save();
    int ready = device_stalled(device) || nothing_pending() ||
                (free_slot() != NULL && device_held(device) < device_cap);

    irq_restore(flags);
    return ready;
}

/**
 * A slot to submit with, or NULL.
 *
 * With `wait` the caller sleeps until a slot is its to take, and NULL then
 * means the device is stalled or the pool is: every slot taken and none in
 * flight, so only a late answer could free one. Without `wait`, for a caller
 * that cannot sleep, NULL means no slot now.
 */
static struct block_request *req_take(block_device_t *device, int write, uint64_t lba,
                                      uint32_t count, int wait)
{
    int waited = 0;

    pool_configure();
    for (;;) {
        uint32_t flags = irq_save();
        struct block_request *r = NULL;

        if (device_stalled(device)) {
            irq_restore(flags);
            klog(KLOG_WARN, "block", "device stalled: %s",
                 waited ? "waiter released with EIO" : "request refused");
            return NULL;
        }
        if (device_held(device) < device_cap) {
            r = free_slot();
        }
        if (r) {
            r->device = device;
            r->write = write;
            r->lba = lba;
            r->count = count;
            r->page = pool_pages[r - pool];
            r->status = -EIO;
            r->state = BLOCK_REQ_IN_FLIGHT;
            r->generation++;
            r->deadline = req_ticks + (device->deadline_ticks ? device->deadline_ticks
                                                              : REQ_DEFAULT_DEADLINE);
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
            klog(KLOG_INFO, "block", "request pool: waiting for a slot");
        }
        wq_wait_event_nointr(&pool_wq, slot_ready, device);
    }
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
    struct block_request *r = req_take(device, write, lba, count, 1);
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
    /* A device that sleeps its caller and answers in the same call is called
     * directly and takes no request slot: the device under it takes one from
     * this pool while it waits, and two slots for one transfer is how the
     * pool runs out with everybody waiting for everybody. */
    if (device->flags & BLOCK_CALLER_SYNC) {
        __asm__ __volatile__("pushfl; popl %0" : "=r"(eflags));
        if (!(eflags & 0x200) || !current_proc || !current_proc->in_syscall) {
            k_panic("block: a sleeping device was reached outside syscall context");
        }
        if (write) {
            return device->write ? device->write(device, lba, count, buffer)
                                 : -EIO;
        }
        return device->read ? device->read(device, lba, count, buffer) : -EIO;
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
    if (device->flags & BLOCK_CALLER_SYNC) {
        return device->flush(device, 0); /* answers in this call, no slot */
    }
    r = req_take(device, 1, 0, 1, 1);
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
    r = req_take(device, 0, lba, count, 0);
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
    int answered = 0;

    if (!request || request->generation != generation) {
        irq_restore(flags);
        klog(KLOG_WARN, "block", "completion dropped: stale generation");
        return;
    }
    if (request->state == BLOCK_REQ_QUARANTINED) {
        request->generation++; /* the caller that gave up must not match */
        request->state = BLOCK_REQ_FREE;
        quarantined_count--;
        /* The device answered, late: it is not stalled any more. */
        if (request->device->flags & BLOCK_STALLED) {
            request->device->flags &= ~BLOCK_STALLED;
            answered = 1;
        }
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
    irq_restore(flags);
    wq_wake_all(&pool_wq);
}

int block_device_holds(block_device_t *device)
{
    uint32_t flags = irq_save();
    int held = (int)device_held(device);

    irq_restore(flags);
    return held;
}

/* Interrupts off. A device holding slots with none of them in flight has
 * stopped answering: it is marked stalled, once, so its waiters are
 * released and its later callers refused until a late answer clears it. */
static int mark_stalled(void)
{
    uint32_t i;
    int marked = 0;

    for (i = 0; i < pool_size; i++) {
        block_device_t *device = pool[i].device;

        if (pool[i].state != BLOCK_REQ_QUARANTINED || (device->flags & BLOCK_STALLED)) {
            continue;
        }
        if (device_in_flight(device) == 0) {
            device->flags |= BLOCK_STALLED;
            marked++;
        }
    }
    return marked;
}

void block_request_tick(void)
{
    uint32_t i;
    int quarantined = 0;
    int stalled;

    req_ticks++;
    for (i = 0; i < pool_size; i++) {
        if (pool[i].state == BLOCK_REQ_IN_FLIGHT && (int32_t)(req_ticks - pool[i].deadline) >= 0) {
            /* The caller is woken with EIO; the slot and its page wait for
             * the device, which still owns them. The generation stays so the
             * late completion matches, and moves on when it frees the slot. */
            pool[i].state = BLOCK_REQ_QUARANTINED;
            quarantined_count++;
            quarantined++;
        }
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
