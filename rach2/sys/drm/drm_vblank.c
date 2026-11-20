/*
 * drm_vblank.c: vblank counts, waits, events and sequences, Linux's
 * drm_vblank.c, and the CRC entries of debugfs's crc files, Linux's
 * drm_debugfs_crc.c, which a driver fills from its vblank path.
 */

#include "drm.h"

#include "../lib/kerrno.h"
#include "../lib/kirq.h"
#include "../lib/kstring.h"
#include "../mm/kheap.h"
#include "../proc/percpu.h"
#include "../proc/process.h"
#include "../rtc.h"
#include "../timer.h"

/* How long WAIT_VBLANK waits for its count, Linux's 3 s; past it, EBUSY. */
#define VBLANK_WAIT_MS 3000

/** CLOCK_MONOTONIC in nanoseconds. */
static int64_t vblank_now(void)
{
    int64_t sec, nsec;

    rtc_monotonic(&sec, &nsec);
    return sec * 1000000000ll + nsec;
}

/* Linux's drm_timestamp_precision, in nanoseconds, and
 * DRM_TIMESTAMP_MAXRETRIES: how long a scanout position's reading may take
 * and how often it is tried. */
#define VBLANK_MAX_ERROR_NS    20000
#define VBLANK_TIMESTAMP_TRIES 3

int64_t drm_vblank_ktime(void)
{
    return vblank_now();
}

int drm_crtc_vblank_helper_get_vblank_timestamp_internal(
    struct drm_crtc *crtc, int64_t *time_ns, int in_vblank_irq,
    drm_vblank_get_scanout_position_func get_scanout_position, const struct drm_mode_modeinfo *mode)
{
    int64_t stime = 0, etime = 0, pixels;
    int32_t vpos = 0, hpos = 0;
    uint64_t delta;
    int i;

    if (!get_scanout_position || !mode || mode->clock == 0) {
        return 0;
    }
    for (i = 0; i < VBLANK_TIMESTAMP_TRIES; i++) {
        if (!get_scanout_position(crtc, in_vblank_irq, &vpos, &hpos, &stime, &etime, mode)) {
            return 0;
        }
        if (etime - stime <= VBLANK_MAX_ERROR_NS) {
            break;
        }
    }
    /* Linux's div_s64, which rounds toward zero, on the magnitude. */
    pixels = (int64_t)vpos * mode->htotal + hpos;
    delta = drm_div64((uint64_t)(pixels < 0 ? -pixels : pixels) * 1000000u, mode->clock);
    *time_ns = pixels < 0 ? etime + (int64_t)delta : etime - (int64_t)delta;
    return 1;
}

/**
 * The time of `crtc`'s latest vblank, Linux's drm_get_last_vbltimestamp:
 * the driver's from where the CRTC scans when it can say, with `*precise`
 * set, else the clock now.
 */
static int64_t vblank_timestamp(struct drm_crtc *crtc, int in_vblank_irq, int *precise)
{
    int64_t t;

    if (crtc->funcs && crtc->funcs->get_vblank_timestamp &&
        crtc->funcs->get_vblank_timestamp(crtc, &t, in_vblank_irq)) {
        *precise = 1;
        return t;
    }
    *precise = 0;
    return vblank_now();
}

/**
 * The length of a frame of `mode` in nanoseconds, Linux's
 * drm_calc_timestamping_constants: the pixels of a frame, blanking
 * included, over the clock, halved for an interlaced mode; 0 for a mode
 * with no clock.
 */
static int64_t vblank_framedur(const struct drm_mode_modeinfo *mode)
{
    uint64_t ns;

    if (mode->clock == 0 || mode->htotal == 0 || mode->vtotal == 0) {
        return 0;
    }
    ns = drm_div64((uint64_t)mode->htotal * mode->vtotal * 1000000u, mode->clock);
    if (mode->flags & DRM_MODE_FLAG_INTERLACE) {
        ns /= 2;
    }
    return (int64_t)ns;
}

/** A monotonic time `ns`, not negative, as the seconds and microseconds of
 * an event or a reply, without the 64-bit division i686 has no routine
 * for. */
static void ns_split(int64_t ns, uint32_t *sec, uint32_t *usec)
{
    uint64_t s = drm_div64((uint64_t)ns, 1000000000u);

    *sec = (uint32_t)s;
    *usec = (uint32_t)drm_div64((uint64_t)ns - s * 1000000000u, 1000u);
}

/** Whether the count `seq` has reached `ref`, Linux's drm_vblank_passed:
 * within 2^23 at or past it. */
static int vblank_passed(uint64_t seq, uint64_t ref)
{
    return seq - ref <= ((uint64_t)1 << 23);
}

int drm_dev_has_vblank(const struct drm_device *dev)
{
    uint32_t i;

    for (i = 0; i < dev->mode_config.num_crtc; i++) {
        if (drm_crtc_has_vblank(dev->mode_config.crtcs[i])) {
            return 1;
        }
    }
    return 0;
}

void drm_crtc_vblank_init(struct drm_crtc *crtc)
{
    wait_queue_init(&crtc->vblank.wait);
    wait_queue_init(&crtc->crc.wait);
    spin_lock_init(&crtc->crc.lock, "drm_crc_lock");
    k_memcpy(crtc->crc.source, "auto", 5);
    /* Linux's drm_crtc_vblank_reset: off, with the hold that keeps it so. */
    crtc->vblank.inmodeset = 1;
    crtc->vblank.refs = 1;
}

/* ------------------------------------------------------------------------
 * The interrupt and its holders
 *
 * Two of the device's spinlocks keep this file's state, as Linux's
 * dev->event_lock and dev->vbl_lock do, so that the vblank interrupt, a
 * commit and a wait on other processors, none of them under the big lock,
 * see it whole. `event_lock` is the outer: each CRTC's queued events and
 * its `flip_pending`, with the files' events it sends them to. `vbl_lock`
 * is the inner: a CRTC's holds, whether its interrupt is on, and the
 * stores of its count and time, with the driver's `enable_vblank`,
 * `disable_vblank` and counter and time hooks, which run under it and may
 * take only the driver's hardware locks. A reader of the count and the
 * time takes neither, but reads them as one pair against `seq`.
 */

/**
 * Store `diff` more vblanks of `crtc`, the time `t` of the latest and the
 * frame counter `last` they were counted to, Linux's store_vblank. A time
 * of 0 marks none until the next vblank. A time before one stored earlier
 * is counted in `backwards`, which no event or reply should ever carry,
 * since a compositor takes each as the next frame's. The count and the
 * time change inside an odd `seq`, as under Linux's write_seqlock
 * (drm_vblank.c:192-195), so that drm_crtc_vblank_count_and_time, which
 * runs with interrupts on, never pairs a count with another vblank's time
 * or takes half of a time i686 stores in two moves. The device's
 * `vbl_lock` is held.
 */
static void store_vblank(struct drm_crtc *crtc, uint32_t diff, int64_t t, uint32_t last)
{
    struct drm_vblank *v = &crtc->vblank;

    v->last = last;
    v->seq++;
    k_barrier();
    v->time_ns = t;
    v->count += diff;
    k_barrier();
    v->seq++;
    if (t != 0) {
        if (t < v->stamp_max_ns) {
            v->backwards++;
        } else {
            v->stamp_max_ns = t;
        }
    }
}

/**
 * Read `crtc`'s frame counter and the time of its latest vblank into `*t`,
 * reading both again while the counter moved between them, at most
 * VBLANK_TIMESTAMP_TRIES times, as the loops of Linux's
 * drm_update_vblank_count and drm_reset_vblank_timestamp do: a vblank that
 * begins between the two reads would pair the count before it with its own
 * time, which the next vblank's reading of the same frame may then put a
 * few microseconds earlier. Returns the counter.
 */
static uint32_t vblank_sample(struct drm_crtc *crtc, int in_vblank_irq, int64_t *t, int *precise)
{
    uint32_t cur;
    int tries = VBLANK_TIMESTAMP_TRIES;

    do {
        cur = crtc->funcs->get_vblank_counter(crtc);
        *t = vblank_timestamp(crtc, in_vblank_irq, precise);
    } while (cur != crtc->funcs->get_vblank_counter(crtc) && --tries > 0);
    return cur;
}

/**
 * Take in the vblanks of `crtc` since the count last did, Linux's
 * drm_update_vblank_count with a counter of 32 bits: the frame counter's
 * difference; for a CRTC without a counter, the frames between the
 * driver's time of its latest vblank and the time stored, rounded to the
 * nearest, when the driver gives that time, the mode has a frame and a
 * time is stored; else 1 at an interrupt. When none has come, as at an
 * interrupt whose frame was already counted, the count and its time stay
 * as they are, since a second reading of the same frame may come out
 * earlier than the first. Outside the interrupt the time is kept only from
 * where the CRTC scans, else 0 until the next vblank. The device's
 * `vbl_lock` is held.
 */
static void vblank_update_count(struct drm_crtc *crtc, int in_vblank_irq)
{
    struct drm_vblank *v = &crtc->vblank;
    uint32_t cur = v->last, diff;
    int64_t t = 0;
    int precise = 0;

    if (crtc->funcs->get_vblank_counter) {
        cur = vblank_sample(crtc, in_vblank_irq, &t, &precise);
        diff = cur - v->last;
    } else {
        t = vblank_timestamp(crtc, in_vblank_irq, &precise);
        if (precise && v->framedur_ns > 0 && v->time_ns != 0) {
            diff = t <= v->time_ns ? 0
                                   : (uint32_t)drm_div64((uint64_t)(t - v->time_ns) +
                                                             (uint64_t)v->framedur_ns / 2u,
                                                         (uint64_t)v->framedur_ns);
        } else {
            diff = in_vblank_irq ? 1 : 0;
        }
    }
    if (diff == 0) {
        return;
    }
    if (!precise && !in_vblank_irq) {
        t = 0;
    }
    store_vblank(crtc, diff, t, cur);
}

/** Turn the interrupt on, Linux's drm_vblank_enable, then count the
 * vblanks that came while it was off: 0 or the driver's error. The
 * device's `vbl_lock` is held. */
static int32_t vblank_enable(struct drm_crtc *crtc)
{
    int32_t rc;

    if (crtc->vblank.enabled) {
        return 0;
    }
    rc = crtc->funcs->enable_vblank(crtc);
    if (rc == 0) {
        vblank_update_count(crtc, 0);
        crtc->vblank.enabled = 1;
    }
    return rc;
}

/** Turn the interrupt off, Linux's drm_vblank_disable_and_save, with the
 * vblanks counted up to now. The device's `vbl_lock` is held. */
static void vblank_disable(struct drm_crtc *crtc)
{
    if (!crtc->vblank.enabled) {
        return;
    }
    vblank_update_count(crtc, 0);
    crtc->vblank.enabled = 0;
    if (crtc->funcs->disable_vblank) {
        crtc->funcs->disable_vblank(crtc);
    }
}

int32_t drm_crtc_vblank_get(struct drm_crtc *crtc)
{
    struct drm_vblank *v = &crtc->vblank;
    int32_t rc = 0;
    uint32_t flags;

    if (!drm_crtc_has_vblank(crtc)) {
        return -EINVAL;
    }
    flags = spin_lock_irqsave(&crtc->dev->vbl_lock);
    if (++v->refs == 1) {
        rc = vblank_enable(crtc);
        if (rc != 0) {
            v->refs--;
        }
    } else if (!v->enabled) {
        v->refs--;
        rc = -EINVAL;
    }
    spin_unlock_irqrestore(&crtc->dev->vbl_lock, flags);
    return rc;
}

void drm_crtc_vblank_put(struct drm_crtc *crtc)
{
    struct drm_vblank *v = &crtc->vblank;
    uint32_t flags = spin_lock_irqsave(&crtc->dev->vbl_lock);

    if (v->refs != 0 && --v->refs == 0) {
        v->put_ns = vblank_now();
    }
    spin_unlock_irqrestore(&crtc->dev->vbl_lock, flags);
}

/**
 * Read the count and the time of `crtc`'s latest vblank as one pair, Linux's
 * read_seqbegin and read_seqretry loop of drm_vblank_count_and_time
 * (drm_vblank.c:953-957): read again while store_vblank was in the middle
 * of a store, its `seq` odd, or made one between the reads, its `seq`
 * moved. A compiler barrier is the whole of Linux's smp_rmb and smp_wmb
 * on x86, which keeps each processor's loads, and its stores, in order.
 */
uint64_t drm_crtc_vblank_count_and_time(struct drm_crtc *crtc, int64_t *time_ns)
{
    const volatile uint32_t *seqp = &crtc->vblank.seq;
    uint64_t count;
    int64_t t;
    uint32_t seq;

    do {
        while ((seq = *seqp) & 1u) {
        }
        k_barrier();
        count = crtc->vblank.count;
        t = crtc->vblank.time_ns;
        k_barrier();
    } while (*seqp != seq);
    if (time_ns) {
        *time_ns = t;
    }
    return count;
}

/* ------------------------------------------------------------------------
 * Events
 */

/**
 * Stamp and send `e`, Linux's send_vblank_event: a vblank or flip event
 * takes the count's low 32 bits and the time in seconds and microseconds, a
 * sequence event the count, unless it is 0, and the time in nanoseconds.
 * A commit's event first ends its CRTC's pending flip. The device's
 * `event_lock` is held.
 */
static void send_vblank_event(struct drm_pending_event *e, uint64_t seq, int64_t now)
{
    struct drm_crtc *flip = e->flip_crtc;

    switch (e->event.base.type) {
    case DRM_EVENT_VBLANK:
    case DRM_EVENT_FLIP_COMPLETE:
        e->event.vbl.sequence = (uint32_t)seq;
        ns_split(now, &e->event.vbl.tv_sec, &e->event.vbl.tv_usec);
        break;
    case DRM_EVENT_CRTC_SEQUENCE:
        if (seq) {
            e->event.seq.sequence = seq;
        }
        e->event.seq.time_ns = now;
        break;
    default:
        break;
    }
    e->flip_crtc = NULL;
    if (flip) {
        flip->flip_pending = DRM_FLIP_NONE;
        wq_wake_all(&flip->vblank.wait);
    }
    drm_send_event_locked(e);
}

/** Queue `e`, reserved, for the count `seq` of `crtc`, after those queued
 * before it; the caller's hold on the vblanks goes with it. The device's
 * `event_lock` is held, since the vblank interrupt sends and frees the
 * queue's events. */
static void queue_event(struct drm_crtc *crtc, struct drm_pending_event *e, uint64_t seq)
{
    struct drm_vblank *v = &crtc->vblank;

    e->sequence = seq;
    e->next = NULL;
    if (v->events_tail) {
        v->events_tail->next = e;
    } else {
        v->events = e;
    }
    v->events_tail = e;
}

/** Send each queued event of `crtc` whose count has come, or every one
 * when `all`, letting go of the hold each kept. The device's `event_lock`
 * is held. */
static void send_due_events(struct drm_crtc *crtc, int all)
{
    struct drm_vblank *v = &crtc->vblank;
    struct drm_pending_event **link = &v->events, *e;
    uint64_t seq = v->count;
    int64_t now = v->time_ns;

    v->events_tail = NULL;
    while ((e = *link) != NULL) {
        if (!all && !vblank_passed(seq, e->sequence)) {
            v->events_tail = e;
            link = &e->next;
            continue;
        }
        *link = e->next;
        e->next = NULL;
        drm_crtc_vblank_put(crtc);
        send_vblank_event(e, seq, now);
    }
}

void drm_crtc_arm_vblank_event(struct drm_crtc *crtc, struct drm_pending_event *e)
{
    uint32_t flags = spin_lock_irqsave(&crtc->dev->event_lock);

    queue_event(crtc, e, crtc->vblank.count + 1);
    spin_unlock_irqrestore(&crtc->dev->event_lock, flags);
}

/**
 * Send `e`, reserved, now if the count `req_seq` of `crtc` has come, else
 * queue it for that count, in one step against the vblank interrupt, as
 * Linux does under its event lock; the caller's hold goes with the event.
 * Returns 0 with the count reached in `*reply`, or `req_seq` when queued;
 * or -EINVAL, with `e` cancelled and the hold let go, when the CRTC's
 * vblanks went off since the hold was taken, as Linux's
 * drm_queue_vblank_event looks under the lock.
 */
static int32_t send_or_queue(struct drm_crtc *crtc, struct drm_pending_event *e, uint64_t req_seq,
                             uint64_t *reply)
{
    uint64_t seq;
    int64_t now;
    uint32_t flags = spin_lock_irqsave(&crtc->dev->event_lock);

    if (!crtc->vblank.enabled) {
        spin_unlock_irqrestore(&crtc->dev->event_lock, flags);
        drm_event_cancel_free(e);
        drm_crtc_vblank_put(crtc);
        return -EINVAL;
    }
    seq = drm_crtc_vblank_count_and_time(crtc, &now);
    if (vblank_passed(seq, req_seq)) {
        drm_crtc_vblank_put(crtc);
        send_vblank_event(e, seq, now);
    } else {
        queue_event(crtc, e, req_seq);
        seq = req_seq;
    }
    spin_unlock_irqrestore(&crtc->dev->event_lock, flags);
    *reply = seq;
    return 0;
}

void drm_crtc_send_vblank_event_locked(struct drm_crtc *crtc, struct drm_pending_event *e)
{
    uint64_t seq = 0;
    int64_t now;

    if (drm_dev_has_vblank(crtc->dev)) {
        seq = drm_crtc_vblank_count_and_time(crtc, &now);
    } else {
        now = vblank_now();
    }
    send_vblank_event(e, seq, now);
}

void drm_crtc_send_vblank_event(struct drm_crtc *crtc, struct drm_pending_event *e)
{
    uint32_t flags = spin_lock_irqsave(&crtc->dev->event_lock);

    drm_crtc_send_vblank_event_locked(crtc, e);
    spin_unlock_irqrestore(&crtc->dev->event_lock, flags);
}

int drm_crtc_handle_vblank(struct drm_crtc *crtc)
{
    struct drm_device *dev = crtc->dev;
    struct drm_vblank *v = &crtc->vblank;
    uint32_t flags = spin_lock_irqsave(&dev->event_lock);
    int64_t now = 0;
    int on = 0;

    spin_lock(&dev->vbl_lock);
    if (v->enabled) {
        now = vblank_now();
        vblank_update_count(crtc, 1);
        on = 1;
    }
    spin_unlock(&dev->vbl_lock);
    if (!on) {
        spin_unlock_irqrestore(&dev->event_lock, flags);
        return 0;
    }
    wq_wake_all(&v->wait);
    send_due_events(crtc, 0);
    spin_lock(&dev->vbl_lock);
    if (v->enabled && v->refs == 0 &&
        now - v->put_ns >= (int64_t)DRM_VBLANK_OFFDELAY_MS * 1000000) {
        vblank_disable(crtc);
        on = 0;
    }
    spin_unlock(&dev->vbl_lock);
    spin_unlock_irqrestore(&dev->event_lock, flags);
    return on;
}

void drm_crtc_vblank_off(struct drm_crtc *crtc)
{
    struct drm_device *dev = crtc->dev;
    struct drm_vblank *v = &crtc->vblank;
    uint32_t flags = spin_lock_irqsave(&dev->event_lock);

    spin_lock(&dev->vbl_lock);
    vblank_disable(crtc);
    if (!v->inmodeset) {
        v->refs++;
        v->inmodeset = 1;
    }
    spin_unlock(&dev->vbl_lock);
    wq_wake_all(&v->wait);
    /* Linux sends what was queued at once, "lest the natives grow
     * disquiet", with the count reached. */
    send_due_events(crtc, 1);
    spin_unlock_irqrestore(&dev->event_lock, flags);
}

void drm_crtc_vblank_on(struct drm_crtc *crtc)
{
    struct drm_vblank *v = &crtc->vblank;
    int64_t t;
    int precise;
    uint32_t cur, flags = spin_lock_irqsave(&crtc->dev->vbl_lock);

    if (v->inmodeset) {
        v->refs--;
        v->inmodeset = 0;
    }
    if (crtc->state) {
        v->framedur_ns = vblank_framedur(&crtc->state->mode);
    }
    /* Linux's drm_reset_vblank_timestamp: one more count, with the time
     * from where the CRTC scans or none until the next vblank; a hardware
     * counter is taken in from here. */
    if (crtc->funcs && crtc->funcs->get_vblank_counter) {
        cur = vblank_sample(crtc, 0, &t, &precise);
    } else {
        cur = v->last;
        t = vblank_timestamp(crtc, 0, &precise);
    }
    store_vblank(crtc, 1, precise ? t : 0, cur);
    if (v->refs != 0 && drm_crtc_has_vblank(crtc)) {
        (void)vblank_enable(crtc);
    }
    spin_unlock_irqrestore(&crtc->dev->vbl_lock, flags);
}

void drm_vblank_unplug(struct drm_device *dev)
{
    struct drm_pending_event *e;
    uint32_t i;

    for (i = 0; i < dev->mode_config.num_crtc; i++) {
        struct drm_crtc *crtc = dev->mode_config.crtcs[i];
        uint32_t flags = spin_lock_irqsave(&dev->event_lock);

        for (e = crtc->vblank.events; e; e = e->next) {
            dma_fence_set_error(e->fence, -ENODEV);
        }
        spin_unlock_irqrestore(&dev->event_lock, flags);
        drm_crtc_vblank_off(crtc);
        wq_wake_all(&crtc->vblank.wait);
        wq_wake_all(&crtc->crc.wait);
    }
}

void drm_crtc_vblank_cleanup(struct drm_crtc *crtc)
{
    struct drm_pending_event *e, *queued;
    struct drm_crtc_crc_entry *entries;
    uint32_t flags = spin_lock_irqsave(&crtc->dev->event_lock);

    queued = crtc->vblank.events;
    crtc->vblank.events = NULL;
    crtc->vblank.events_tail = NULL;
    spin_unlock_irqrestore(&crtc->dev->event_lock, flags);
    while ((e = queued) != NULL) {
        queued = e->next;
        drm_event_cancel_free(e);
    }
    flags = spin_lock_irqsave(&crtc->crc.lock);
    entries = crtc->crc.entries;
    crtc->crc.entries = NULL;
    spin_unlock_irqrestore(&crtc->crc.lock, flags);
    kfree(entries);
}

/* ------------------------------------------------------------------------
 * Waits
 */

/* A wait on a CRTC's queue: until `ready(crtc, arg)` or the tick
 * `deadline`, 0 for none. */
struct crtc_wait {
    struct drm_crtc *crtc;
    int (*ready)(struct drm_crtc *crtc, uint64_t arg);
    uint64_t arg;
    uint64_t deadline;
};

/** Whether a CRTC wait `arg` may end: its condition, or its deadline. */
static int crtc_wait_done(void *arg)
{
    struct crtc_wait *w = (struct crtc_wait *)arg;

    return w->ready(w->crtc, w->arg) || (w->deadline && rtc_ticks_now() >= w->deadline);
}

/**
 * Sleep on `crtc`'s vblank queue until `ready(crtc, arg)` holds or
 * `timeout_ms` passes, 0 for no limit. 0 when it holds, -ETIME at the
 * limit, or -EINTR for a signal when `intr` is set.
 */
static int32_t crtc_wait(struct drm_crtc *crtc, int (*ready)(struct drm_crtc *, uint64_t),
                         uint64_t arg, int intr, uint32_t timeout_ms)
{
    struct crtc_wait w;
    uint64_t saved;
    int rc;

    if (ready(crtc, arg)) {
        return 0;
    }
    if (!current_proc) {
        return -ETIME;
    }
    w.crtc = crtc;
    w.ready = ready;
    w.arg = arg;
    w.deadline = timeout_ms ? rtc_ticks_now() + MS_TO_TICKS(timeout_ms) + 1 : 0;
    saved = current_proc->wakeup_tick;
    current_proc->wakeup_tick = w.deadline;
    rc = intr ? wq_wait_event(&crtc->vblank.wait, crtc_wait_done, &w)
              : wq_wait_event_nointr(&crtc->vblank.wait, crtc_wait_done, &w);
    current_proc->wakeup_tick = saved;
    if (rc < 0) {
        return -EINTR;
    }
    return ready(crtc, arg) ? 0 : -ETIME;
}

/** Whether `crtc`'s flip is below the stage `below`. */
static int flip_below(struct drm_crtc *crtc, uint64_t below)
{
    return (uint64_t)crtc->flip_pending < below;
}

int32_t drm_crtc_wait_flip(struct drm_crtc *crtc, int below, int intr, uint32_t timeout_ms)
{
    return crtc_wait(crtc, flip_below, (uint64_t)below, intr, timeout_ms);
}

/** Whether `crtc`'s count has reached `seq`, or its vblanks have
 * stopped. */
static int count_reached(struct drm_crtc *crtc, uint64_t seq)
{
    return vblank_passed(drm_crtc_vblank_count_and_time(crtc, NULL), seq) || !crtc->vblank.enabled;
}

/* ------------------------------------------------------------------------
 * The ioctls
 */

/** A 32-bit count `x` widened to 64 bits near `y`, Linux's
 * widen_32_to_64. */
static uint64_t widen_32_to_64(uint32_t x, uint64_t y)
{
    return y + (uint64_t)(int64_t)(int32_t)(x - (uint32_t)y);
}

/** WAIT_VBLANK's event: a VBLANK event of `crtc` with the request's
 * `signal` as its user data, for the count `req_seq`, sent now when that
 * count has come. Takes over the caller's hold on the vblanks. */
static int32_t queue_vblank_event(struct drm_crtc *crtc, uint64_t req_seq,
                                  union drm_wait_vblank *vblwait, struct drm_file *file)
{
    struct drm_pending_event *e = (struct drm_pending_event *)kzalloc(sizeof(*e));
    uint64_t seq;
    int32_t rc;

    if (!e) {
        drm_crtc_vblank_put(crtc);
        return -ENOMEM;
    }
    e->length = sizeof(e->event.vbl);
    e->event.vbl.base.type = DRM_EVENT_VBLANK;
    e->event.vbl.base.length = sizeof(e->event.vbl);
    e->event.vbl.user_data = vblwait->request.signal;
    e->event.vbl.crtc_id = crtc->base.id;
    rc = drm_event_reserve(file, e);
    if (rc != 0) {
        kfree(e);
        drm_crtc_vblank_put(crtc);
        return rc;
    }
    rc = send_or_queue(crtc, e, req_seq, &seq);
    if (rc == 0) {
        vblwait->reply.sequence = (unsigned int)seq;
    }
    return rc;
}

/**
 * WAIT_VBLANK, Linux's drm_wait_vblank_ioctl: on the CRTC of the index the
 * request's high bits or SECONDARY give, among those a lessee leases, as
 * Linux counts a lessee's index, wait for an absolute count or one
 * relative to now, the next on a miss when asked, at most 3 s (EBUSY), or
 * queue a VBLANK event for it with EVENT; the reply has the count reached
 * and its time, or with EVENT the count the event waits for.
 */
int32_t drm_wait_vblank_ioctl(struct drm_device *dev, void *data, struct drm_file *file)
{
    union drm_wait_vblank *vblwait = (union drm_wait_vblank *)data;
    uint32_t type = (uint32_t)vblwait->request.type;
    uint32_t flags, high, index, pipe, sec, usec;
    uint64_t req_seq, seq;
    struct drm_crtc *crtc;
    int64_t now;
    int32_t rc;

    if (!drm_dev_has_vblank(dev)) {
        return -EOPNOTSUPP;
    }
    if (type & _DRM_VBLANK_SIGNAL) {
        return -EINVAL;
    }
    if (type &
        ~(uint32_t)(_DRM_VBLANK_TYPES_MASK | _DRM_VBLANK_FLAGS_MASK | _DRM_VBLANK_HIGH_CRTC_MASK)) {
        return -EINVAL;
    }
    flags = type & _DRM_VBLANK_FLAGS_MASK;
    high = type & _DRM_VBLANK_HIGH_CRTC_MASK;
    index = high ? high >> _DRM_VBLANK_HIGH_CRTC_SHIFT : (flags & _DRM_VBLANK_SECONDARY ? 1 : 0);
    /* A lessee's index counts the CRTCs it leases alone. */
    for (pipe = 0; pipe < dev->mode_config.num_crtc; pipe++) {
        if (drm_lease_held(file, dev->mode_config.crtcs[pipe]->base.id) && index-- == 0) {
            break;
        }
    }
    if (pipe >= dev->mode_config.num_crtc) {
        return -EINVAL;
    }
    crtc = dev->mode_config.crtcs[pipe];
    rc = drm_crtc_vblank_get(crtc);
    if (rc != 0) {
        return rc;
    }
    seq = drm_crtc_vblank_count_and_time(crtc, NULL);
    if (type & _DRM_VBLANK_RELATIVE) {
        req_seq = seq + vblwait->request.sequence;
        vblwait->request.sequence = (unsigned int)req_seq;
        vblwait->request.type = (enum drm_vblank_seq_type)(type & ~(uint32_t)_DRM_VBLANK_RELATIVE);
    } else {
        req_seq = widen_32_to_64(vblwait->request.sequence, seq);
    }
    if ((flags & _DRM_VBLANK_NEXTONMISS) && vblank_passed(seq, req_seq)) {
        req_seq = seq + 1;
        vblwait->request.type = (enum drm_vblank_seq_type)((uint32_t)vblwait->request.type &
                                                           ~(uint32_t)_DRM_VBLANK_NEXTONMISS);
        vblwait->request.sequence = (unsigned int)req_seq;
    }
    if (flags & _DRM_VBLANK_EVENT) {
        return queue_vblank_event(crtc, req_seq, vblwait, file);
    }
    if (req_seq != seq) {
        rc = crtc_wait(crtc, count_reached, req_seq, 1, VBLANK_WAIT_MS);
        if (rc == -ETIME) {
            rc = -EBUSY;
        }
    }
    if (rc != -EINTR) {
        seq = drm_crtc_vblank_count_and_time(crtc, &now);
        ns_split(now, &sec, &usec);
        vblwait->reply.sequence = (unsigned int)seq;
        vblwait->reply.tval_sec = (intptr_t)sec;
        vblwait->reply.tval_usec = (intptr_t)usec;
    }
    drm_crtc_vblank_put(crtc);
    return rc;
}

/**
 * CRTC_GET_SEQUENCE, Linux's drm_crtc_get_sequence_ioctl: whether the CRTC
 * is enabled, its vblank count and the time of the last vblank in
 * nanoseconds, with the interrupt on while it reads.
 */
int32_t drm_crtc_get_sequence_ioctl(struct drm_device *dev, void *data, struct drm_file *file)
{
    struct drm_crtc_get_sequence *get_seq = (struct drm_crtc_get_sequence *)data;
    struct drm_crtc *crtc;
    int64_t now;
    int32_t rc;

    if (!drm_core_check_feature(dev, DRIVER_MODESET) || !drm_dev_has_vblank(dev)) {
        return -EOPNOTSUPP;
    }
    crtc = drm_crtc_find(dev, file, get_seq->crtc_id);
    if (!crtc) {
        return -ENOENT;
    }
    rc = drm_crtc_vblank_get(crtc);
    if (rc != 0) {
        return rc;
    }
    get_seq->active = (uint32_t)crtc->state->enable;
    get_seq->sequence = drm_crtc_vblank_count_and_time(crtc, &now);
    get_seq->sequence_ns = now;
    drm_crtc_vblank_put(crtc);
    return 0;
}

/**
 * CRTC_QUEUE_SEQUENCE, Linux's drm_crtc_queue_sequence_ioctl: a
 * CRTC_SEQUENCE event for an absolute count or one relative to now, the
 * next on a miss when asked, sent now when that count has come; the
 * request's `sequence` becomes the count it waits for, or the count
 * reached.
 */
int32_t drm_crtc_queue_sequence_ioctl(struct drm_device *dev, void *data, struct drm_file *file)
{
    struct drm_crtc_queue_sequence *queue_seq = (struct drm_crtc_queue_sequence *)data;
    struct drm_pending_event *e;
    struct drm_crtc *crtc;
    uint64_t seq, req_seq;
    int32_t rc;

    if (!drm_core_check_feature(dev, DRIVER_MODESET) || !drm_dev_has_vblank(dev)) {
        return -EOPNOTSUPP;
    }
    crtc = drm_crtc_find(dev, file, queue_seq->crtc_id);
    if (!crtc) {
        return -ENOENT;
    }
    if (queue_seq->flags &
        ~(uint32_t)(DRM_CRTC_SEQUENCE_RELATIVE | DRM_CRTC_SEQUENCE_NEXT_ON_MISS)) {
        return -EINVAL;
    }
    e = (struct drm_pending_event *)kzalloc(sizeof(*e));
    if (!e) {
        return -ENOMEM;
    }
    rc = drm_crtc_vblank_get(crtc);
    if (rc != 0) {
        kfree(e);
        return rc;
    }
    seq = drm_crtc_vblank_count_and_time(crtc, NULL);
    req_seq = queue_seq->sequence;
    if (queue_seq->flags & DRM_CRTC_SEQUENCE_RELATIVE) {
        req_seq += seq;
    }
    if ((queue_seq->flags & DRM_CRTC_SEQUENCE_NEXT_ON_MISS) && vblank_passed(seq, req_seq)) {
        req_seq = seq + 1;
    }
    e->length = sizeof(e->event.seq);
    e->event.seq.base.type = DRM_EVENT_CRTC_SEQUENCE;
    e->event.seq.base.length = sizeof(e->event.seq);
    e->event.seq.user_data = queue_seq->user_data;
    rc = drm_event_reserve(file, e);
    if (rc != 0) {
        drm_crtc_vblank_put(crtc);
        kfree(e);
        return rc;
    }
    return send_or_queue(crtc, e, req_seq, &queue_seq->sequence);
}

/* ------------------------------------------------------------------------
 * CRCs
 */

/* A line of crc/data with `n` values, its newline and its NUL. */
#define CRC_LINE_LEN(n) (10u + 11u * (n) + 1u + 1u)

/** Whether `crtc`'s driver gives CRCs. */
static int has_crc(const struct drm_crtc *crtc)
{
    return crtc->funcs && crtc->funcs->set_crc_source && crtc->funcs->verify_crc_source;
}

/** The entries waiting in `crc`'s ring. */
static uint32_t crc_count(const struct drm_crtc_crc *crc)
{
    return (crc->head - crc->tail) & (DRM_CRC_ENTRIES_NR - 1);
}

int32_t drm_crtc_crc_set_source(struct drm_crtc *crtc, const char *buf, uint32_t len)
{
    char source[sizeof(crtc->crc.source)];
    uint32_t values = 0, n = len;
    int32_t rc;

    if (len == 0) {
        return 0;
    }
    if (len > PAGE_SIZE - 1) {
        return -E2BIG;
    }
    if (!has_crc(crtc)) {
        return -EINVAL;
    }
    if (buf[n - 1] == '\n') {
        n--;
    }
    /* No driver names a source this long, so its check would refuse it. */
    if (n >= sizeof(source)) {
        return -EINVAL;
    }
    k_memcpy(source, buf, n);
    source[n] = '\0';
    rc = crtc->funcs->verify_crc_source(crtc, source, &values);
    if (rc != 0) {
        return rc;
    }
    if (crtc->crc.opened) {
        return -EBUSY;
    }
    k_memcpy(crtc->crc.source, source, n + 1);
    return (int32_t)len;
}

int32_t drm_crtc_crc_open(struct drm_crtc *crtc)
{
    struct drm_crtc_crc *crc = &crtc->crc;
    struct drm_crtc_crc_entry *entries;
    uint32_t values = 0, flags;
    int32_t rc;

    if (!has_crc(crtc)) {
        return -EINVAL;
    }
    if (drm_core_check_feature(crtc->dev, DRIVER_ATOMIC)) {
        rc = drm_modeset_lock(crtc->dev);
        if (rc != 0) {
            return rc;
        }
        rc = crtc->state->active ? 0 : -EIO;
        drm_modeset_unlock(crtc->dev);
        if (rc != 0) {
            return rc;
        }
    }
    rc = crtc->funcs->verify_crc_source(crtc, crc->source, &values);
    if (rc != 0) {
        return rc;
    }
    if (values == 0 || values > DRM_MAX_CRC_NR) {
        return -EINVAL;
    }
    entries = (struct drm_crtc_crc_entry *)kzalloc(DRM_CRC_ENTRIES_NR * sizeof(*entries));
    if (!entries) {
        return -ENOMEM;
    }
    flags = spin_lock_irqsave(&crc->lock);
    if (crc->opened) {
        spin_unlock_irqrestore(&crc->lock, flags);
        kfree(entries);
        return -EBUSY;
    }
    crc->opened = 1;
    crc->entries = entries;
    crc->values = values;
    crc->head = crc->tail = 0;
    crc->overflow = 0;
    spin_unlock_irqrestore(&crc->lock, flags);
    rc = crtc->funcs->set_crc_source(crtc, crc->source);
    if (rc != 0) {
        flags = spin_lock_irqsave(&crc->lock);
        crc->opened = 0;
        crc->entries = NULL;
        spin_unlock_irqrestore(&crc->lock, flags);
        kfree(entries);
    }
    return rc;
}

void drm_crtc_crc_release(struct drm_crtc *crtc)
{
    struct drm_crtc_crc *crc = &crtc->crc;
    struct drm_crtc_crc_entry *entries;
    uint32_t flags;

    if (has_crc(crtc)) {
        crtc->funcs->set_crc_source(crtc, NULL);
    }
    flags = spin_lock_irqsave(&crc->lock);
    crc->opened = 0;
    entries = crc->entries;
    crc->entries = NULL;
    crc->head = crc->tail = 0;
    crc->values = 0;
    spin_unlock_irqrestore(&crc->lock, flags);
    kfree(entries);
    wq_wake_all(&crc->wait);
}

int32_t drm_crtc_add_crc_entry(struct drm_crtc *crtc, int has_frame, uint32_t frame,
                               const uint32_t *crcs)
{
    struct drm_crtc_crc *crc = &crtc->crc;
    struct drm_crtc_crc_entry *entry;
    uint32_t flags = spin_lock_irqsave(&crc->lock);
    int32_t rc = 0;

    if (!crc->entries) {
        rc = -EINVAL;
    } else if (crc_count(crc) == DRM_CRC_ENTRIES_NR - 1) {
        crc->overflow = 1;
        rc = -ENOBUFS;
    } else {
        entry = &crc->entries[crc->head];
        entry->frame = frame;
        entry->has_frame_counter = has_frame != 0;
        k_memcpy(entry->crcs, crcs, crc->values * sizeof(*crcs));
        crc->head = (crc->head + 1) & (DRM_CRC_ENTRIES_NR - 1);
    }
    spin_unlock_irqrestore(&crc->lock, flags);
    if (rc == 0) {
        wq_wake_all(&crc->wait);
    }
    return rc;
}

int drm_crtc_crc_ready(struct drm_crtc *crtc)
{
    uint32_t flags = spin_lock_irqsave(&crtc->crc.lock);
    int ready = crtc->crc.entries && crc_count(&crtc->crc) != 0;

    spin_unlock_irqrestore(&crtc->crc.lock, flags);
    return ready;
}

/** Whether crc/data's reader of `arg`, a CRTC, may go on: an entry, or the
 * file's release. */
static int crc_readable(void *arg)
{
    const struct drm_crtc *crtc = (const struct drm_crtc *)arg;

    return !crtc->crc.entries || crc_count(&crtc->crc) != 0;
}

/** Write `v` as "0x%08x" at `out`. */
static void crc_hex(char *out, uint32_t v)
{
    static const char digits[] = "0123456789abcdef";
    int i;

    out[0] = '0';
    out[1] = 'x';
    for (i = 0; i < 8; i++) {
        out[2 + i] = digits[(v >> (28 - 4 * i)) & 0xf];
    }
}

int32_t drm_crtc_crc_read(struct drm_crtc *crtc, char *buf, uint32_t count, int nonblock)
{
    struct drm_crtc_crc *crc = &crtc->crc;
    struct drm_crtc_crc_entry entry;
    uint32_t i, len, values, flags;

    if (!has_crc(crtc)) {
        return 0;
    }
    for (;;) {
        flags = spin_lock_irqsave(&crc->lock);
        if (crc->entries && crc_count(crc) != 0) {
            break;
        }
        spin_unlock_irqrestore(&crc->lock, flags);
        if (nonblock) {
            return -EAGAIN;
        }
        if (wq_wait_event(&crc->wait, crc_readable, crtc) != 0) {
            return -EINTR;
        }
        if (!crc->entries) {
            return -EINVAL;
        }
    }
    values = crc->values;
    len = CRC_LINE_LEN(values);
    if (count < len) {
        spin_unlock_irqrestore(&crc->lock, flags);
        return -EINVAL;
    }
    entry = crc->entries[crc->tail];
    crc->tail = (crc->tail + 1) & (DRM_CRC_ENTRIES_NR - 1);
    spin_unlock_irqrestore(&crc->lock, flags);
    if (entry.has_frame_counter) {
        crc_hex(buf, entry.frame);
    } else {
        k_memcpy(buf, "XXXXXXXXXX", 10);
    }
    for (i = 0; i < values; i++) {
        buf[10 + 11 * i] = ' ';
        crc_hex(buf + 11 + 11 * i, entry.crcs[i]);
    }
    buf[len - 2] = '\n';
    buf[len - 1] = '\0';
    return (int32_t)len;
}
