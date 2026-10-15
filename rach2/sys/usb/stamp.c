/*
 * stamp.c: the keystroke latency record: for the latest keystroke, when its
 * event was drained, when it reached the input layer and when a reader took
 * it, logged once under `rach2.usblatency`.
 */

#include "usb.h"

#include "../klog.h"
#include "../lib/kirq.h"
#include "../timer.h"

/* Whether the record is on; the last pump's handler entry and drain, in
 * microseconds, whether an interrupt ran it and whether it took events; the
 * console reader about to wait and whether one is; and whether a keystroke
 * waits for its read. */
static int recording;
static int pump_took;
static uint32_t pump_entry, pump_event;
static int pump_msi;
static uint32_t reader;
static int waiting;
static int pending;

/* One keystroke: its pump's handler entry and drain and its wake, in
 * microseconds of `timer_now_us`; the pumps and the `getrandom` bytes between
 * the wake and the reader's dispatch; whether an interrupt ran the pump;
 * whether a waiting console reader was armed for it; and whether that reader
 * has been dispatched. */
struct usb_stamp {
    uint32_t entry, event, wake;
    uint32_t pumps, work;
    int msi, tracked, dispatched;
};

static struct usb_stamp stamp;

void usb_stamp_init(int enabled)
{
    recording = enabled;
    waiting = pending = pump_took = 0;
}

/** A pump between a tracked keystroke's wake and its reader's dispatch is
 * counted, up to the counter's limit. */
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

/** Arming a reader drops any keystroke not yet read. */
void usb_stamp_wait(uint32_t pid)
{
    reader = pid;
    waiting = 1;
    pending = 0;
}

void usb_stamp_dispatch(uint32_t pid)
{
    if (pending && stamp.tracked && reader == pid) {
        stamp.dispatched = 1;
        waiting = 0;
    }
}

/** The count stops at its limit rather than wrapping. */
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

void usb_stamp_wake(void)
{
    if (!pump_took) {
        pending = 0;
        return;
    }
    stamp.entry = pump_entry;
    stamp.event = pump_event;
    stamp.wake = timer_now_us();
    stamp.msi = pump_msi;
    stamp.tracked = waiting;
    stamp.dispatched = 0;
    stamp.pumps = stamp.work = 0;
    pending = 1;
}

/**
 * The controller gives no time of its own, so the times are measured from
 * the drain that took the event: the handler's entry, which came before it,
 * is negative, and a slow read is reported however long it took. The counts
 * stop at the reader's dispatch, before interrupts can delay its return
 * through the terminal. One console reader and one keystroke are followed;
 * type-ahead and concurrent readers are not recorded byte by byte.
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
    read_us = timer_now_us();
    result = stamp;
    kirq_restore(flags);
    klog(KLOG_INFO, "usb",
         "latency: msi %d, entry %d, wake %d, read %d, us from the drain; "
         "tracked %d, dispatch %d, pumps %d, work %d",
         result.msi, result.msi ? (int)(result.entry - result.event) : 0,
         (int)(result.wake - result.event), (int)(read_us - result.event), result.tracked,
         result.dispatched, (int)result.pumps, (int)result.work);
}

int usb_stamp_recording(void)
{
    return recording;
}
