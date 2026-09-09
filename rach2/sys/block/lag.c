/*
 * lag.c: the sleeping test device.
 *
 * `rach2.blocklag=N` wraps the boot disk in a device without the fault-safe
 * flag whose requests complete N ticks later, from the timer. Only the
 * partition nodes and the mounts use the wrapper; the root window keeps the
 * real device. It exists so the sleeping path is proved by a device that
 * behaves and by one that does not, before a USB disk exists: with N above
 * the deadline it is a device that never answers in time, which is how the
 * quarantine is exercised without a mutation. `rach2.blockfail=N` beside it
 * makes the Nth write through the wrapper fail with EIO, which is how a
 * mount is seen to go read-only. `rach2.blockdead=P` builds the view of
 * MBR partition P, read from the disk when the wrapper is installed, over a
 * second device that accepts every request and answers none, so a device
 * that stalls has a healthy neighbour on the same pool. The wrapper has a
 * `flush` too, answered after the lag with nothing under it to flush and
 * recorded when it is, so a sync takes a slot of the pool the way a stick's
 * flush does.
 */

#include "block.h"

#include "../lib/kstring.h"

#include "../cmdline.h"
#include "../klog.h"
#include "../lib/kerrno.h"

/* Enough for every slot of the pool and the two-tick duplicate each
 * completion leaves behind. */
#define LAG_QUEUE 48

struct lag_entry {
    struct block_request *request;
    uint32_t generation;
    uint32_t due;
    int status;
    int delivered;
    int flush; /* a flush, recorded when answered */
    int used;
    /* A read lands here at submit and is copied into the request's page
     * only when the completion is delivered, so a kernel that copied out
     * before the completion would read what the page held before. */
    uint8_t staged[BLOCK_REQ_SECTORS * BLOCK_SECTOR_SIZE];
};

static block_device_t lag_device;
static block_device_t *lag_backing;
static struct lag_entry queue[LAG_QUEUE];
static uint32_t lag_ticks;
static uint32_t lag_delay;
static uint32_t fail_write_at; /* rach2.blockfail=N: the Nth write answers EIO */
static uint32_t writes_seen;
static block_device_t dead_device; /* rach2.blockdead=P: the partition's own device */
static uint64_t dead_start;        /* where that partition starts, or 0 for none */
static int dead_set;

/** Hold a flush, to be answered after the lag: nothing under it to flush. */
static int lag_flush(block_device_t *device, struct block_request *request)
{
    int i;

    (void)device;
    for (i = 0; i < LAG_QUEUE; i++) {
        if (!queue[i].used) {
            queue[i].status = 0;
            queue[i].request = request;
            queue[i].generation = request->generation;
            queue[i].due = lag_ticks + lag_delay;
            queue[i].delivered = 0;
            queue[i].flush = 1;
            queue[i].used = 1;
            return 0;
        }
    }
    return -EIO;
}

/** The never-answering device: every request is accepted and none ends. */
static int dead_submit(block_device_t *device, struct block_request *request)
{
    (void)device;
    (void)request;
    return 0;
}

static int lag_submit(block_device_t *device, struct block_request *request)
{
    int i;

    (void)device;
    for (i = 0; i < LAG_QUEUE; i++) {
        if (!queue[i].used) {
            /* The transfer runs now, polled, in the caller's context; only
             * the answer is held back. The timer must not touch the disk:
             * a polled ATA read inside the tick handler waits on a clock
             * that cannot advance there. */
            if (request->write && fail_write_at && ++writes_seen == fail_write_at) {
                queue[i].status = -EIO; /* the write the boot option fails */
            } else if (request->write) {
                queue[i].status = lag_backing->write(lag_backing, request->lba,
                                                     request->count,
                                                     request->page);
            } else {
                /* The page is scrubbed so a copy-out before the completion
                 * returns this and not what the page held before. */
                k_memset(request->page, 0xEE, request->count * BLOCK_SECTOR_SIZE);
                queue[i].status = lag_backing->read(lag_backing, request->lba,
                                                    request->count,
                                                    queue[i].staged);
            }
            queue[i].request = request;
            queue[i].generation = request->generation;
            queue[i].due = lag_ticks + lag_delay;
            queue[i].delivered = 0;
            queue[i].flush = 0;
            queue[i].used = 1;
            return 0;
        }
    }
    return -EIO;
}

void block_lag_tick(void)
{
    int i;

    if (!lag_backing) {
        return;
    }
    lag_ticks++;
    for (i = 0; i < LAG_QUEUE; i++) {
        struct lag_entry *e = &queue[i];
        struct block_request *r;

        if (!e->used || (int32_t)(lag_ticks - e->due) < 0) {
            continue;
        }
        r = e->request;
        /* Every completion is delivered twice, two ticks apart, and the
         * second copy claims an error. A request freed between the copies
         * is not live any more, and one made since at the same address has
         * a generation of its own, so the duplicate must be dropped; one
         * accepted would fail a healthy request, which is what makes the
         * check's absence visible. */
        if (!e->delivered) {
            e->delivered = 1;
            e->due = lag_ticks + 2;
            if (e->flush) {
                klog(KLOG_INFO, "block", "lag: flush answered");
            } else if (!r->write && e->status == 0) {
                k_memcpy(r->page, e->staged, r->count * BLOCK_SECTOR_SIZE);
            }
            block_request_complete(r, e->generation, e->status);
            continue;
        }
        e->used = 0;
        block_request_complete(r, e->generation, -EIO);
    }
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

block_device_t *block_lag_wrap(block_device_t *backing)
{
    char value[16];
    uint32_t n = 0;
    int i;

    if (lag_backing) {
        return &lag_device; /* already installed */
    }
    if (!kcmdline_value("rach2.blocklag", value, sizeof(value))) {
        return backing;
    }
    for (i = 0; value[i] >= '0' && value[i] <= '9'; i++) {
        n = n * 10u + (uint32_t)(value[i] - '0');
    }
    if (i == 0 || value[i] != '\0' || n == 0) {
        klog(KLOG_WARN, "block", "rach2.blocklag: not a tick count; ignored");
        return backing;
    }
    lag_backing = backing;
    lag_delay = n;
    if (kcmdline_value("rach2.blockfail", value, sizeof(value))) {
        for (i = 0; value[i] >= '0' && value[i] <= '9'; i++) {
            fail_write_at = fail_write_at * 10u + (uint32_t)(value[i] - '0');
        }
        klog(KLOG_INFO, "block", "lag: write %d fails", (int)fail_write_at);
    }
    if (kcmdline_value("rach2.blockdead", value, sizeof(value))) {
        uint8_t table[BLOCK_SECTOR_SIZE];
        unsigned p = value[0] >= '1' && value[0] <= '4' && value[1] == '\0'
                         ? (unsigned)(value[0] - '0')
                         : 0;

        if (p == 0 || backing->read(backing, 0, 1, table) != 0) {
            klog(KLOG_WARN, "block", "rach2.blockdead: not an MBR entry of 1 to 4; ignored");
        } else {
            const uint8_t *e = table + 446 + 16 * (p - 1);

            dead_start = (uint64_t)e[8] | ((uint64_t)e[9] << 8) | ((uint64_t)e[10] << 16) |
                         ((uint64_t)e[11] << 24);
            dead_set = 1;
            klog(KLOG_INFO, "block", "lag: partition %u never answers", p);
        }
    }
    lag_device = *backing;
    lag_device.flags &= ~BLOCK_FAULT_SAFE;
    lag_device.max_sectors = 0;      /* a page, the staging it copies through */
    lag_device.deadline_ticks = 500; /* five seconds, the lag its rows are set against */
    lag_device.deadline_limit = 1;   /* one period: its rows test a device that never answers */
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
    klog(KLOG_INFO, "block", "lag: %d ticks behind the boot disk", (int)n);
    return &lag_device;
}
