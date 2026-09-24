#include "usb.h"

#include "../klog.h"
#include "../lib/kirq.h"
#include "../pit.h"

static int recording;
static int pump_took;
static uint32_t pump_entry, pump_event;
static int pump_msi;
static uint32_t reader;
static int waiting;
static int pending;

struct usb_stamp {
    uint32_t entry, event, wake;
    uint32_t pumps, work;
    int msi, tracked, dispatched;
};

static struct usb_stamp stamp;

/** Turn diagnostic records on or off and discard any earlier reader or
 * keystroke. */
void usb_stamp_init(int enabled)
{
    recording = enabled;
    waiting = pending = pump_took = 0;
}

/** Record a pump and count pumps after a wake but before its dispatch. */
void usb_stamp_pump(uint32_t entry, uint32_t event, int msi, int took)
{
    if (pending && stamp.tracked && !stamp.dispatched && stamp.pumps != 0xffffffffu) {
        stamp.pumps++;
    }
    pump_entry = entry;
    pump_event = event;
    pump_msi = msi;
    pump_took = took;
}

/** Arm one blocked console reader while interrupts are disabled. */
void usb_stamp_wait(uint32_t pid)
{
    reader = pid;
    waiting = 1;
    pending = 0;
}

/** Freeze the work counts when the scheduler selects the recorded reader. */
void usb_stamp_dispatch(uint32_t pid)
{
    if (pending && stamp.tracked && reader == pid) {
        stamp.dispatched = 1;
        waiting = 0;
    }
}

/** Count bytes completed by getrandom before the woken reader is selected. */
void usb_stamp_work(uint32_t bytes)
{
    uint32_t flags;

    if (!recording) {
        return;
    }
    flags = kirq_save();
    if (pending && stamp.tracked && !stamp.dispatched) {
        if (bytes > 0xffffffffu - stamp.work) {
            stamp.work = 0xffffffffu;
        } else {
            stamp.work += bytes;
        }
    }
    kirq_restore(flags);
}

/** Snapshot the pump that delivered the key, keeping only the latest key. */
void usb_stamp_wake(void)
{
    if (!pump_took) {
        pending = 0;
        return;
    }
    stamp.entry = pump_entry;
    stamp.event = pump_event;
    stamp.wake = pit_now_us();
    stamp.msi = pump_msi;
    stamp.tracked = waiting;
    stamp.dispatched = 0;
    stamp.pumps = stamp.work = 0;
    pending = 1;
}

/**
 * Print the latest keystroke once, with elapsed times as diagnostics only.
 *
 * The controller supplies no timestamp. Times are relative to the software
 * drain, including negative entry offsets. A slow read remains reportable.
 * Counts stop at the recorded reader's dispatch, before interrupts can delay
 * its return through the terminal code. One console reader and one keystroke
 * are tracked; type-ahead and concurrent readers are not per-byte records.
 */
void usb_stamp_read(void)
{
    uint32_t flags;
    uint32_t read_us;
    struct usb_stamp result;

    flags = kirq_save();
    if (!pending || !recording) {
        pending = 0;
        kirq_restore(flags);
        return;
    }
    pending = 0;
    waiting = 0;
    read_us = pit_now_us();
    result = stamp;
    kirq_restore(flags);
    klog(KLOG_INFO, "usb",
         "latency: msi %d, entry %d, wake %d, read %d, us from the drain; "
         "tracked %d, dispatch %d, pumps %d, work %d",
         result.msi, result.msi ? (int)(result.entry - result.event) : 0,
         (int)(result.wake - result.event), (int)(read_us - result.event), result.tracked,
         result.dispatched, (int)result.pumps, (int)result.work);
}

/** Return whether the boot enabled keystroke diagnostics. */
int usb_stamp_recording(void)
{
    return recording;
}
