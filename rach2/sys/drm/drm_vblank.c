/*
 * drm_vblank.c: vblank counts, waits, events and sequences, Linux's
 * drm_vblank.c, and the CRC entries of debugfs's crc files, Linux's
 * drm_debugfs_crc.c, which a driver fills from its vblank path.
 */

#include "drm.h"

#include "../lib/kerrno.h"
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
    k_memcpy(crtc->crc.source, "auto", 5);
    /* Linux's drm_crtc_vblank_reset: off, with the hold that keeps it so. */
    crtc->vblank.inmodeset = 1;
    crtc->vblank.refs = 1;
}

/* ------------------------------------------------------------------------
 * The interrupt and its holders
 */

/** Turn the interrupt on, Linux's drm_vblank_enable: 0 or the driver's
 * error. */
static int32_t vblank_enable(struct drm_crtc *crtc)
{
    int32_t rc;

    if (crtc->vblank.enabled) {
        return 0;
    }
    rc = crtc->funcs->enable_vblank(crtc);
    if (rc == 0) {
        crtc->vblank.enabled = 1;
    }
    return rc;
}

/** Turn the interrupt off, Linux's drm_vblank_disable_and_save. */
static void vblank_disable(struct drm_crtc *crtc)
{
    if (!crtc->vblank.enabled) {
        return;
    }
    crtc->vblank.enabled = 0;
    if (crtc->funcs->disable_vblank) {
        crtc->funcs->disable_vblank(crtc);
    }
}

int32_t drm_crtc_vblank_get(struct drm_crtc *crtc)
{
    struct drm_vblank *v = &crtc->vblank;
    int32_t rc = 0;

    if (!drm_crtc_has_vblank(crtc)) {
        return -EINVAL;
    }
    if (++v->refs == 1) {
        rc = vblank_enable(crtc);
        if (rc != 0) {
            v->refs--;
        }
    } else if (!v->enabled) {
        v->refs--;
        rc = -EINVAL;
    }
    return rc;
}

void drm_crtc_vblank_put(struct drm_crtc *crtc)
{
    struct drm_vblank *v = &crtc->vblank;

    if (v->refs == 0) {
        return;
    }
    if (--v->refs == 0) {
        v->put_ns = vblank_now();
    }
}

uint64_t drm_crtc_vblank_count_and_time(struct drm_crtc *crtc, int64_t *time_ns)
{
    if (time_ns) {
        *time_ns = crtc->vblank.time_ns;
    }
    return crtc->vblank.count;
}

/* ------------------------------------------------------------------------
 * Events
 */

/**
 * Stamp and send `e`, Linux's send_vblank_event: a vblank or flip event
 * takes the count's low 32 bits and the time in seconds and microseconds, a
 * sequence event the count, unless it is 0, and the time in nanoseconds.
 * A commit's event first ends its CRTC's pending flip. Any context.
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
    drm_send_event(e);
}

/** Queue `e`, reserved, for the count `seq` of `crtc`, after those queued
 * before it; the caller's hold on the vblanks goes with it. */
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
 * when `all`, letting go of the hold each kept. */
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
    queue_event(crtc, e, crtc->vblank.count + 1);
}

void drm_crtc_send_vblank_event(struct drm_crtc *crtc, struct drm_pending_event *e)
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

int drm_crtc_handle_vblank(struct drm_crtc *crtc)
{
    struct drm_vblank *v = &crtc->vblank;
    int64_t now;

    if (!v->enabled) {
        return 0;
    }
    now = vblank_now();
    v->count++;
    v->time_ns = now;
    wq_wake_all(&v->wait);
    send_due_events(crtc, 0);
    if (v->refs == 0 && now - v->put_ns >= (int64_t)DRM_VBLANK_OFFDELAY_MS * 1000000) {
        vblank_disable(crtc);
        return 0;
    }
    return 1;
}

void drm_crtc_vblank_off(struct drm_crtc *crtc)
{
    struct drm_vblank *v = &crtc->vblank;

    vblank_disable(crtc);
    wq_wake_all(&v->wait);
    if (!v->inmodeset) {
        v->refs++;
        v->inmodeset = 1;
    }
    /* Linux sends what was queued at once, "lest the natives grow
     * disquiet", with the count reached. */
    send_due_events(crtc, 1);
}

void drm_crtc_vblank_on(struct drm_crtc *crtc)
{
    struct drm_vblank *v = &crtc->vblank;

    if (v->inmodeset) {
        v->refs--;
        v->inmodeset = 0;
    }
    /* Linux's drm_reset_vblank_timestamp without a precise timestamp: one
     * more count, and no time until the next vblank. */
    v->count++;
    v->time_ns = 0;
    if (v->refs != 0 && drm_crtc_has_vblank(crtc)) {
        (void)vblank_enable(crtc);
    }
}

void drm_vblank_unplug(struct drm_device *dev)
{
    struct drm_pending_event *e;
    uint32_t i;

    for (i = 0; i < dev->mode_config.num_crtc; i++) {
        struct drm_crtc *crtc = dev->mode_config.crtcs[i];

        for (e = crtc->vblank.events; e; e = e->next) {
            dma_fence_set_error(e->fence, -ENODEV);
        }
        drm_crtc_vblank_off(crtc);
        wq_wake_all(&crtc->vblank.wait);
        wq_wake_all(&crtc->crc.wait);
    }
}

void drm_crtc_vblank_cleanup(struct drm_crtc *crtc)
{
    struct drm_pending_event *e;

    while ((e = crtc->vblank.events) != NULL) {
        crtc->vblank.events = e->next;
        drm_event_cancel_free(e);
    }
    crtc->vblank.events_tail = NULL;
    kfree(crtc->crc.entries);
    crtc->crc.entries = NULL;
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
    return vblank_passed(crtc->vblank.count, seq) || !crtc->vblank.enabled;
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
    int64_t now;
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
    /* The CRTC may have gone off since the hold was taken. */
    rc = crtc->vblank.enabled ? drm_event_reserve(file, e) : -EINVAL;
    if (rc != 0) {
        kfree(e);
        drm_crtc_vblank_put(crtc);
        return rc;
    }
    seq = drm_crtc_vblank_count_and_time(crtc, &now);
    if (vblank_passed(seq, req_seq)) {
        drm_crtc_vblank_put(crtc);
        send_vblank_event(e, seq, now);
        vblwait->reply.sequence = (unsigned int)seq;
    } else {
        queue_event(crtc, e, req_seq);
        vblwait->reply.sequence = (unsigned int)req_seq;
    }
    return 0;
}

/**
 * WAIT_VBLANK, Linux's drm_wait_vblank_ioctl: on the CRTC of the index the
 * request's high bits or SECONDARY give, wait for an absolute count or one
 * relative to now, the next on a miss when asked, at most 3 s (EBUSY), or
 * queue a VBLANK event for it with EVENT; the reply has the count reached
 * and its time, or with EVENT the count the event waits for.
 */
int32_t drm_wait_vblank_ioctl(struct drm_device *dev, void *data, struct drm_file *file)
{
    union drm_wait_vblank *vblwait = (union drm_wait_vblank *)data;
    uint32_t type = (uint32_t)vblwait->request.type;
    uint32_t flags, high, pipe, sec, usec;
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
    pipe = high ? high >> _DRM_VBLANK_HIGH_CRTC_SHIFT : (flags & _DRM_VBLANK_SECONDARY ? 1 : 0);
    if (pipe >= dev->mode_config.num_crtc) {
        return -EINVAL;
    }
    crtc = dev->mode_config.crtcs[pipe];
    rc = drm_crtc_vblank_get(crtc);
    if (rc != 0) {
        return rc;
    }
    seq = crtc->vblank.count;
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
    int64_t now;
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
    seq = drm_crtc_vblank_count_and_time(crtc, &now);
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
    rc = crtc->vblank.enabled ? drm_event_reserve(file, e) : -EINVAL;
    if (rc != 0) {
        drm_crtc_vblank_put(crtc);
        kfree(e);
        return rc;
    }
    if (vblank_passed(seq, req_seq)) {
        drm_crtc_vblank_put(crtc);
        send_vblank_event(e, seq, now);
        queue_seq->sequence = seq;
    } else {
        queue_event(crtc, e, req_seq);
        queue_seq->sequence = req_seq;
    }
    return 0;
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
    uint32_t values = 0;
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
    if (crc->opened) {
        kfree(entries);
        return -EBUSY;
    }
    crc->opened = 1;
    crc->entries = entries;
    crc->values = values;
    crc->head = crc->tail = 0;
    crc->overflow = 0;
    rc = crtc->funcs->set_crc_source(crtc, crc->source);
    if (rc != 0) {
        crc->opened = 0;
        crc->entries = NULL;
        kfree(entries);
    }
    return rc;
}

void drm_crtc_crc_release(struct drm_crtc *crtc)
{
    struct drm_crtc_crc *crc = &crtc->crc;

    crc->opened = 0;
    if (has_crc(crtc)) {
        crtc->funcs->set_crc_source(crtc, NULL);
    }
    kfree(crc->entries);
    crc->entries = NULL;
    crc->head = crc->tail = 0;
    crc->values = 0;
    wq_wake_all(&crc->wait);
}

int32_t drm_crtc_add_crc_entry(struct drm_crtc *crtc, int has_frame, uint32_t frame,
                               const uint32_t *crcs)
{
    struct drm_crtc_crc *crc = &crtc->crc;
    struct drm_crtc_crc_entry *entry;

    if (!crc->entries) {
        return -EINVAL;
    }
    if (crc_count(crc) == DRM_CRC_ENTRIES_NR - 1) {
        crc->overflow = 1;
        return -ENOBUFS;
    }
    entry = &crc->entries[crc->head];
    entry->frame = frame;
    entry->has_frame_counter = has_frame != 0;
    k_memcpy(entry->crcs, crcs, crc->values * sizeof(*crcs));
    crc->head = (crc->head + 1) & (DRM_CRC_ENTRIES_NR - 1);
    wq_wake_all(&crc->wait);
    return 0;
}

int drm_crtc_crc_ready(const struct drm_crtc *crtc)
{
    return crtc->crc.entries && crc_count(&crtc->crc) != 0;
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
    struct drm_crtc_crc_entry *entry;
    uint32_t i, len;

    if (!has_crc(crtc)) {
        return 0;
    }
    while (!crc->entries || crc_count(crc) == 0) {
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
    len = CRC_LINE_LEN(crc->values);
    if (count < len) {
        return -EINVAL;
    }
    entry = &crc->entries[crc->tail];
    crc->tail = (crc->tail + 1) & (DRM_CRC_ENTRIES_NR - 1);
    if (entry->has_frame_counter) {
        crc_hex(buf, entry->frame);
    } else {
        k_memcpy(buf, "XXXXXXXXXX", 10);
    }
    for (i = 0; i < crc->values; i++) {
        buf[10 + 11 * i] = ' ';
        crc_hex(buf + 11 + 11 * i, entry->crcs[i]);
    }
    buf[len - 2] = '\n';
    buf[len - 1] = '\0';
    return (int32_t)len;
}
