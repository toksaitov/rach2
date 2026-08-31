/*
 * lag.c: the sleeping test device.
 *
 * `rach2.blocklag=N` wraps the boot disk in a device without the fault-safe
 * flag whose requests complete N ticks later, from the timer. Only the
 * partition nodes and the mounts use the wrapper; the root window keeps the
 * real device. It exists so the sleeping path is proved by a device that
 * behaves and by one that does not, before a USB disk exists: with N above
 * the deadline it is a device that never answers in time, which is how the
 * quarantine is exercised without a mutation.
 */

#include "block.h"

#include "../cmdline.h"
#include "../klog.h"
#include "../lib/kerrno.h"

#define LAG_QUEUE 16

struct lag_entry {
    struct block_request *request;
    uint32_t generation;
    uint32_t due;
    int status;
    int delivered;
    int used;
};

static block_device_t lag_device;
static block_device_t *lag_backing;
static struct lag_entry queue[LAG_QUEUE];
static uint32_t lag_ticks;
static uint32_t lag_delay;

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
            if (request->write) {
                queue[i].status = lag_backing->write(lag_backing, request->lba,
                                                     request->count,
                                                     request->page);
            } else {
                queue[i].status = lag_backing->read(lag_backing, request->lba,
                                                    request->count,
                                                    request->page);
            }
            queue[i].request = request;
            queue[i].generation = request->generation;
            queue[i].due = lag_ticks + lag_delay;
            queue[i].delivered = 0;
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
         * second copy claims an error. A slot freed and retaken between the
         * copies has a new generation, so the duplicate must be dropped; a
         * kernel that believed it would fail a healthy request, which is
         * what makes the generation check's absence visible. */
        if (!e->delivered) {
            e->delivered = 1;
            e->due = lag_ticks + 2;
            block_request_complete(r, e->generation, e->status);
            continue;
        }
        e->used = 0;
        block_request_complete(r, e->generation, -EIO);
    }
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
    lag_device = *backing;
    lag_device.flags &= ~BLOCK_FAULT_SAFE;
    lag_device.read = 0;
    lag_device.write = 0;
    lag_device.submit = lag_submit;
    lag_device.drvdata = &lag_device;
    klog(KLOG_INFO, "block", "lag: %d ticks behind the boot disk", (int)n);
    return &lag_device;
}
