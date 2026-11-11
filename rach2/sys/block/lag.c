/*
 * lag.c: the sleeping test device.
 *
 * `rach2.blocklag=N` wraps disk 0 in a device whose requests complete N
 * hundredths of a second later, by the timer's ticks. It has no polled
 * calls and no cache, so it keeps none of the disk's fault-safe, pollable
 * and write-through flags, and every transfer and sync on it takes a
 * request. Only the partition views and the mounts on them use the wrapper;
 * the whole-disk window keeps the real device. The sleeping path is tested
 * on it with a device that answers and with one that does not: with N above
 * the deadline it is a device that never answers in time, which tests the
 * quarantine with the driver unchanged. `rach2.blockfail=N` makes the Nth
 * write through the wrapper fail with EIO, so a test can see a mount go
 * read-only.
 * `rach2.blockdead=P` builds the view of MBR partition P, read from the disk
 * when the wrapper is installed, over a second device that accepts every
 * request and answers none, so a device that stalls has a healthy neighbour
 * on the same pool.
 * The wrapper has a `flush` too, answered after the lag with nothing under it
 * to flush, so a sync takes a request of the pool as a stick's flush does.
 */

#include "block.h"

#include "../lib/kstring.h"

#include "../cmdline.h"
#include "../lib/kirq.h"
#include "../klog.h"
#include "../lib/kerrno.h"
#include "../timer.h"

/* Enough for every slot of the pool and the duplicate each completion
 * leaves behind. */
#define LAG_QUEUE 48

/* The ticks in the unit of the options' times, a hundredth of a second. */
#define LAG_UNIT (TICK_HZ / 100u)

struct lag_entry {
    struct block_request *request;
    uint32_t generation;
    uint32_t due;
    int status;
    int delivered;
    int flush; /* a flush, recorded when answered */
    int used;
    int ready; /* the transfer has run; the tick may deliver */
    /* A read lands here at submit and is copied into the request's page
     * only when the completion is delivered, so a kernel that copied out
     * before the completion would read the 0xEE the page was filled with. */
    uint8_t staged[BLOCK_REQ_SECTORS * BLOCK_SECTOR_SIZE];
};

static block_device_t lag_device;
static block_device_t *lag_backing;
static struct lag_entry queue[LAG_QUEUE];
static uint32_t lag_ticks; /* the lag clock, in ticks */
static uint32_t lag_seen;  /* `kernel_ticks` at the last tick's call */
static uint32_t lag_delay;
static int lag_held;             /* explicit completion control for kernel test fixtures */
static uint32_t lag_completions; /* answers delivered after their lag, for /proc/disk */
static uint32_t fail_write_at;   /* rach2.blockfail=N: the Nth write answers EIO */
static uint32_t writes_seen;
static block_device_t dead_device; /* rach2.blockdead=P: the partition's own device */
static uint64_t dead_start;        /* where that partition starts, or 0 for none */
static int dead_set;

/** The lag clock as it stands now: `lag_ticks` and the ticks `kernel_ticks`
 * has counted since the tick's last call, which a boot processor's tick
 * that found the big lock held left out; `lag_ticks` alone while a fixture
 * holds the queue, since the tick then does not advance it. */
static uint32_t lag_clock(void)
{
    return lag_held ? lag_ticks : lag_ticks + ((uint32_t)kernel_ticks - lag_seen);
}

/**
 * Claim a free slot for `request` before anything runs for it.
 *
 * The claim comes first because a transfer can sleep in the driver under
 * it, and a second caller entering meanwhile would otherwise find the same
 * slot free, take it, and one of the two requests would never be answered.
 * A claimed slot is not the tick's to deliver until `ready` is set.
 */
static struct lag_entry *lag_claim(struct block_request *request, int flush)
{
    uint32_t flags = kirq_save();
    int i;

    for (i = 0; i < LAG_QUEUE; i++) {
        if (!queue[i].used) {
            queue[i].used = 1;
            queue[i].ready = 0;
            queue[i].status = 0;
            queue[i].request = request;
            queue[i].generation = request->generation;
            queue[i].delivered = 0;
            queue[i].flush = flush;
            kirq_restore(flags);
            return &queue[i];
        }
    }
    kirq_restore(flags);
    return NULL;
}

/** Hold a flush, to be answered after the lag: nothing under it to flush. */
static int lag_flush(block_device_t *device, struct block_request *request)
{
    struct lag_entry *e = lag_claim(request, 1);

    (void)device;
    if (!e) {
        return -EIO;
    }
    e->due = lag_clock() + lag_delay;
    e->ready = 1;
    return 0;
}

/** The never-answering device: every request is accepted and none ends. */
static int dead_submit(block_device_t *device, struct block_request *request)
{
    (void)device;
    (void)request;
    return 0;
}

/**
 * Run the transfer now, in the caller's context, which the driver under it
 * may put to sleep, and hold back only the answer. The timer must not touch
 * the disk: a polled ATA read inside the tick handler waits on a clock that
 * cannot advance there. A read's page is filled with 0xEE until the answer
 * is delivered, and the bytes wait in the slot.
 */
static int lag_submit(block_device_t *device, struct block_request *request)
{
    struct lag_entry *e = lag_claim(request, 0);

    (void)device;
    if (!e) {
        return -EIO;
    }
    if (request->write && fail_write_at && ++writes_seen == fail_write_at) {
        e->status = -EIO; /* the write the boot option fails */
    } else if (request->write) {
        e->status = lag_backing->write(lag_backing, request->lba, request->count, request->page);
    } else {
        k_memset(request->page, 0xEE, request->count * BLOCK_SECTOR_SIZE);
        e->status = lag_backing->read(lag_backing, request->lba, request->count, e->staged);
    }
    e->due = lag_clock() + lag_delay;
    e->ready = 1;
    return 0;
}

/**
 * Deliver one answer, retaining its address and generation for a duplicate.
 * The duplicate never reads the request, whose memory may have been reused.
 * Call with interrupts masked, after the backing transfer has returned.
 */
static void lag_deliver(struct lag_entry *e)
{
    struct block_request *r = e->request;

    if (e->delivered) {
        e->used = 0;
        block_request_complete(r, e->generation, -EIO);
        return;
    }
    e->delivered = 1;
    e->due = lag_clock() + 2u * LAG_UNIT;
    lag_completions++;
    if (e->flush) {
        klog(KLOG_INFO, "block", "lag: flush answered");
    } else if (!r->write && e->status == 0) {
        k_memcpy(r->page, e->staged, r->count * BLOCK_SECTOR_SIZE);
    }
    block_request_complete(r, e->generation, e->status);
}

int block_lag_hold(int hold)
{
    uint32_t flags = kirq_save();

    if (!lag_backing) {
        kirq_restore(flags);
        return -ENODEV;
    }
    lag_held = hold != 0;
    kirq_restore(flags);
    return 0;
}

int block_lag_answer(uint32_t generation, int duplicate)
{
    uint32_t flags = kirq_save();
    int i;

    if (!lag_held) {
        kirq_restore(flags);
        return -EPERM;
    }
    for (i = 0; i < LAG_QUEUE; i++) {
        struct lag_entry *e = &queue[i];

        if (e->used && e->generation == generation) {
            if (!e->ready || e->delivered != (duplicate != 0)) {
                kirq_restore(flags);
                return -EAGAIN;
            }
            lag_deliver(e);
            kirq_restore(flags);
            return 0;
        }
    }
    kirq_restore(flags);
    return -ENOENT;
}

/**
 * Deliver ready answers at their due tick, and error duplicates two ticks
 * later. The lag clock advances by the ticks counted since the last call,
 * since the boot processor's tick does not call this on every tick
 * (`tick_waits`, isr.c). Explicitly held fixtures use `block_lag_answer`
 * instead; timer ticks then change neither the lag clock nor its queue.
 */
void block_lag_tick(void)
{
    uint32_t now = (uint32_t)kernel_ticks, passed = now - lag_seen;
    int i;

    lag_seen = now;
    if (!lag_backing || lag_held) {
        return;
    }
    lag_ticks += passed;
    for (i = 0; i < LAG_QUEUE; i++) {
        struct lag_entry *e = &queue[i];

        if (e->used && e->ready && (int32_t)(lag_ticks - e->due) >= 0) {
            lag_deliver(e);
        }
    }
}

uint32_t block_lag_completions(void)
{
    return lag_completions;
}

block_device_t *block_lag_backing(block_device_t *dev)
{
    return dev == &lag_device || (dead_set && dev == &dead_device) ? lag_backing : 0;
}

block_device_t *block_lag_partition_backing(block_device_t *backing, uint64_t start_lba)
{
    if (dead_set && backing == &lag_device && start_lba == dead_start) {
        return &dead_device;
    }
    return backing;
}

/**
 * Read the boot option `name` as a whole decimal number from 1 to a million
 * ticks, periods or writes, into `*out`; 1 when it was, 0 when it is absent
 * or refused. A value with anything after its digits, or past the bound, is
 * refused, since the lag's and the deadline's arithmetic is signed and a
 * value near the top of the range would answer or expire a request as it
 * was made.
 */
static int lag_knob(const char *name, uint32_t *out)
{
    char value[16];
    uint32_t n = 0;
    int i;

    if (!kcmdline_value(name, value, sizeof(value))) {
        return 0;
    }
    for (i = 0; value[i] >= '0' && value[i] <= '9'; i++) {
        n = n * 10u + (uint32_t)(value[i] - '0');
        if (n > 1000000u) {
            break;
        }
    }
    if (i == 0 || value[i] != '\0' || n == 0 || n > 1000000u) {
        klog(KLOG_WARN, "block", "lag: %s: not a count from 1 to 1000000", name);
        return 0;
    }
    *out = n;
    return 1;
}

/**
 * Return the lag wrapper for `backing`, installing it on the first call
 * that finds `rach2.blocklag` set to a count of hundredths of a second.
 *
 * There is one wrapper, over the device of the call that installed it. A
 * later call returns the wrapper when `backing` is that device and `backing`
 * itself for any other, so a second disk keeps its own device. Without the
 * option, or with a value `lag_knob` refuses, `backing` is returned.
 * The wrapper's deadline is one period of five seconds, since its tests are
 * of a device that never answers; a test of the deadline's boundary sets the
 * period with `rach2.lagdeadline=T` hundredths of a second and the count
 * with `rach2.lagperiods=N`.
 */
block_device_t *block_lag_wrap(block_device_t *backing)
{
    char value[16];
    uint32_t n;

    if (lag_backing) {
        return backing == lag_backing ? &lag_device : backing;
    }
    if (!lag_knob("rach2.blocklag", &n)) {
        return backing;
    }
    lag_backing = backing;
    lag_delay = n * LAG_UNIT;
    if (lag_knob("rach2.blockfail", &fail_write_at)) {
        klog(KLOG_INFO, "block", "lag: write %u fails", (unsigned)fail_write_at);
    }
    if (kcmdline_value("rach2.blockdead", value, sizeof(value))) {
        uint8_t table[BLOCK_SECTOR_SIZE];
        unsigned p =
            value[0] >= '1' && value[0] <= '4' && value[1] == '\0' ? (unsigned)(value[0] - '0') : 0;

        if (p == 0 || backing->read(backing, 0, 1, table) != 0) {
            klog(KLOG_WARN, "block", "rach2.blockdead: not an MBR entry of 1 to 4; ignored");
        } else {
            const uint8_t *e = table + 446 + 16 * (p - 1);

            dead_start = (uint64_t)e[8] | ((uint64_t)e[9] << 8) | ((uint64_t)e[10] << 16) |
                         ((uint64_t)e[11] << 24);
            dead_set = 1;
            klog(KLOG_INFO, "block", "lag: partition %d never answers", (int)p);
        }
    }
    lag_device = *backing;
    lag_device.flags = backing->flags & BLOCK_READ_ONLY;
    lag_device.max_sectors = 0; /* a page, the staging it copies through */
    lag_device.deadline_ticks =
        500u * LAG_UNIT;           /* five seconds, the lag its rows are set against */
    lag_device.deadline_limit = 1; /* one period: its rows test a device that never answers */
    if (lag_knob("rach2.lagdeadline", &n)) {
        lag_device.deadline_ticks = n * LAG_UNIT;
    }
    if (lag_knob("rach2.lagperiods", &n)) {
        lag_device.deadline_limit = n;
    }
    lag_device.read = 0;
    lag_device.write = 0;
    lag_device.submit = lag_submit;
    lag_device.flush = lag_flush;
    if (dead_set) {
        dead_device = lag_device;
        dead_device.submit = dead_submit;
        dead_device.flush = dead_submit;
        dead_device.drvdata = &dead_device;
    }
    lag_device.drvdata = &lag_device;
    klog(KLOG_INFO, "block", "lag: %d hundredths of a second behind the boot disk",
         (int)(lag_delay / LAG_UNIT));
    return &lag_device;
}
