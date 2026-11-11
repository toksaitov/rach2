/*
 * drm_syncobj.c: sync objects, Linux's drm_syncobj: a handle that holds a
 * fence, binary, or the newest point of a timeline, which programs wait
 * on, signal, reset, move between objects and share as files or sync files.
 */

#include "drm.h"

#include "../dmabuf/dmabuf.h"
#include "../io/eventfd.h"
#include "../lib/kerrno.h"
#include "../lib/kstring.h"
#include "../mm/kheap.h"
#include "../mm/usercopy.h"
#include "../rtc.h"
#include "../timer.h"

/* How long a lookup waits for a point to be submitted, Linux's
 * DRM_SYNCOBJ_WAIT_FOR_SUBMIT_TIMEOUT, in nanoseconds. */
#define SUBMIT_TIMEOUT_NS 5000000000ll

/* No deadline: a CLOCK_MONOTONIC time that never comes. */
#define NO_DEADLINE 0x7fffffffffffffffll

/* The queue of a thread that waits on syncobjs, and the count of the
 * changes that end its sleeps: a fence signalled or a point submitted. */
struct waiter {
    wait_queue_t wq;
    uint32_t events, seen;
    int64_t deadline; /* CLOCK_MONOTONIC in nanoseconds, or NO_DEADLINE */
};

struct syncobj_wait;
struct wait_cb {
    struct dma_fence_cb cb;
    struct syncobj_wait *wait;
};

/*
 * A wait for a point of one syncobj. While `listed`, it is on the
 * syncobj's list of waits for the point to be submitted; `fence` is the
 * point's fence once it is, with a reference, and `cb` its callback, whose
 * `func` is set once armed.
 */
struct syncobj_wait {
    struct syncobj_wait *next;
    int listed;
    uint64_t point;
    struct dma_fence *fence;
    struct wait_cb cb;
    struct waiter *waiter;
};

struct syncobj_eventfd;
struct eventfd_cb {
    struct dma_fence_cb cb;
    struct syncobj_eventfd *entry;
};

/*
 * An eventfd waiting for a point of a syncobj, Linux's
 * syncobj_eventfd_entry: on the syncobj's list until the point is
 * submitted, then on the point's fence through `cb`, unless it waits only
 * for the submission (DRM_SYNCOBJ_WAIT_FLAGS_WAIT_AVAILABLE). It holds the
 * eventfd and, once found, the fence; it never holds the syncobj, whose
 * release frees the records still on its list.
 */
struct syncobj_eventfd {
    struct syncobj_eventfd *next;
    struct eventfd *ev;
    uint64_t point;
    uint32_t flags;
    struct dma_fence *fence;
    struct eventfd_cb cb;
};

/* The object behind a syncobj's descriptor, which holds the syncobj. */
struct drm_syncobj_file {
    struct anon_obj anon;
    struct drm_syncobj *syncobj;
};

/* ------------------------------------------------------------------------
 * Objects and their fences
 */

void drm_syncobj_get(struct drm_syncobj *s)
{
    s->refs++;
}

/** Free an eventfd's record and what it holds. */
static void eventfd_free(struct syncobj_eventfd *e)
{
    eventfd_put(e->ev);
    dma_fence_put(e->fence);
    kfree(e);
}

void drm_syncobj_put(struct drm_syncobj *s)
{
    struct syncobj_eventfd *e;

    if (!s || --s->refs) {
        return;
    }
    drm_syncobj_replace_fence(s, NULL);
    while ((e = s->eventfds) != NULL) {
        s->eventfds = e->next;
        eventfd_free(e);
    }
    kfree(s);
}

struct drm_syncobj *drm_syncobj_find(struct drm_file *file, uint32_t handle)
{
    struct drm_syncobj *s = (struct drm_syncobj *)drm_handle_find(&file->syncobjs, handle);

    if (s) {
        drm_syncobj_get(s);
    }
    return s;
}

/**
 * The fence of `point` from `f`, a syncobj's fence, which this takes, as
 * Linux's callers of dma_fence_chain_find_seqno use it: 0 with the point's
 * fence in `*out`, the stub when the point has signalled and gone, or
 * -EINVAL when `f` is NULL or has not reached `point`, with nothing held.
 */
static int32_t point_fence(struct dma_fence *f, uint64_t point, struct dma_fence **out)
{
    *out = NULL;
    if (!f) {
        return -EINVAL;
    }
    if (dma_fence_chain_find_seqno(&f, point)) {
        dma_fence_put(f);
        return -EINVAL;
    }
    *out = f ? f : dma_fence_get_stub();
    return 0;
}

/** Count a change for a waiting thread and wake it. */
static void waiter_wake(struct waiter *w)
{
    w->events++;
    wq_wake_all(&w->wq);
}

/** Give a listed wait the fence of its point if the point has been
 * submitted, take it off the list and wake its thread. */
static void wait_check(struct drm_syncobj *s, struct syncobj_wait *w)
{
    struct syncobj_wait **link;

    if (point_fence(dma_fence_get(s->fence), w->point, &w->fence)) {
        return;
    }
    for (link = &s->waits; *link && *link != w; link = &(*link)->next) {
    }
    if (*link) {
        *link = w->next;
    }
    w->next = NULL;
    w->listed = 0;
    waiter_wake(w->waiter);
}

/** Signal the eventfd and free its record once the point's fence has
 * signalled. */
static void eventfd_done(struct dma_fence *f, struct dma_fence_cb *cb)
{
    struct syncobj_eventfd *e = ((struct eventfd_cb *)cb)->entry;

    (void)f;
    eventfd_signal(e->ev);
    eventfd_free(e);
}

/**
 * Act for an eventfd on the syncobj's list once its point is submitted, as
 * Linux's syncobj_eventfd_entry_func: take it off the list, then signal
 * the eventfd now if it waits only for the submission or the fence has
 * signalled, else when the fence signals.
 */
static void eventfd_check(struct drm_syncobj *s, struct syncobj_eventfd *e)
{
    struct syncobj_eventfd **link;

    if (point_fence(dma_fence_get(s->fence), e->point, &e->fence)) {
        return;
    }
    for (link = &s->eventfds; *link && *link != e; link = &(*link)->next) {
    }
    if (*link) {
        *link = e->next;
    }
    e->next = NULL;
    if ((e->flags & DRM_SYNCOBJ_WAIT_FLAGS_WAIT_AVAILABLE) ||
        dma_fence_add_callback(e->fence, &e->cb.cb, eventfd_done)) {
        eventfd_signal(e->ev);
        eventfd_free(e);
    }
}

/** Tell every wait and eventfd on the syncobj's lists that its fence
 * changed. */
static void syncobj_changed(struct drm_syncobj *s)
{
    struct syncobj_wait *w, *wnext;
    struct syncobj_eventfd *e, *enext;

    for (w = s->waits; w; w = wnext) {
        wnext = w->next;
        wait_check(s, w);
    }
    for (e = s->eventfds; e; e = enext) {
        enext = e->next;
        eventfd_check(s, e);
    }
}

void drm_syncobj_replace_fence(struct drm_syncobj *s, struct dma_fence *fence)
{
    struct dma_fence *old = s->fence;

    s->fence = dma_fence_get(fence);
    if (fence != old) {
        syncobj_changed(s);
    }
    dma_fence_put(old);
}

void drm_syncobj_add_point(struct drm_syncobj *s, struct dma_fence_chain *chain,
                           struct dma_fence *fence, uint64_t point)
{
    dma_fence_chain_init(chain, s->fence, dma_fence_get(fence), point);
    s->fence = &chain->base;
    syncobj_changed(s);
    dma_fence_put(dma_fence_chain_walk(dma_fence_get(s->fence)));
}

struct drm_syncobj *drm_syncobj_create(uint32_t flags)
{
    struct drm_syncobj *s = (struct drm_syncobj *)kzalloc(sizeof(*s));

    if (!s) {
        return NULL;
    }
    s->refs = 1;
    if (flags & DRM_SYNCOBJ_CREATE_SIGNALED) {
        struct dma_fence *stub = dma_fence_get_stub();

        drm_syncobj_replace_fence(s, stub);
        dma_fence_put(stub);
    }
    return s;
}

/* ------------------------------------------------------------------------
 * Sleeping
 */

/** Whether a waiting thread has something to look at again: a change since
 * it last looked, or its deadline. */
static int waiter_ready(void *arg)
{
    struct waiter *w = (struct waiter *)arg;

    return w->events != w->seen || dma_fence_now() >= w->deadline;
}

/**
 * Sleep until a change since the waiter last looked or its deadline,
 * restoring the thread's timer. 0, or -EINTR for a signal. May sleep.
 */
static int32_t waiter_sleep(struct waiter *w)
{
    uint64_t saved, delta;
    int64_t left;
    int rc;

    if (!current_proc) {
        return -EINVAL;
    }
    saved = current_proc->wakeup_tick;
    current_proc->wakeup_tick = 0;
    if (w->deadline != NO_DEADLINE) {
        left = w->deadline - dma_fence_now();
        delta = tick_div((uint64_t)(left > 0 ? left : 0), 1000, NULL);
        delta = tick_div(delta + 999, 1000, NULL);
        current_proc->wakeup_tick = rtc_ticks_now() + delta + 1;
    }
    rc = wq_wait_event(&w->wq, waiter_ready, w);
    current_proc->wakeup_tick = saved;
    return rc < 0 ? -EINTR : 0;
}

/** Put a wait on the syncobj's list unless its point is there already, as
 * Linux's drm_syncobj_fence_add_wait. */
static void wait_add(struct drm_syncobj *s, struct syncobj_wait *w)
{
    if (w->fence) {
        return;
    }
    if (!point_fence(dma_fence_get(s->fence), w->point, &w->fence)) {
        return;
    }
    w->next = s->waits;
    s->waits = w;
    w->listed = 1;
}

/** Take a wait off the syncobj's list if it is still there. */
static void wait_remove(struct drm_syncobj *s, struct syncobj_wait *w)
{
    struct syncobj_wait **link;

    if (!w->listed) {
        return;
    }
    for (link = &s->waits; *link && *link != w; link = &(*link)->next) {
    }
    if (*link) {
        *link = w->next;
    }
    w->listed = 0;
}

/** Count a signalled fence for the thread that waits on it. */
static void wait_fence_done(struct dma_fence *f, struct dma_fence_cb *cb)
{
    (void)f;
    waiter_wake(((struct wait_cb *)cb)->wait->waiter);
}

int32_t drm_syncobj_find_fence(struct drm_file *file, uint32_t handle, uint64_t point,
                               uint32_t flags, struct dma_fence **fence)
{
    struct drm_syncobj *s = drm_syncobj_find(file, handle);
    struct syncobj_wait w;
    struct waiter t;
    int32_t rc;

    *fence = NULL;
    if (flags & ~(uint32_t)DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT) {
        drm_syncobj_put(s);
        return -EINVAL;
    }
    if (!s) {
        return -ENOENT;
    }
    rc = point_fence(dma_fence_get(s->fence), point, fence);
    if (rc == 0 || !(flags & DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT)) {
        drm_syncobj_put(s);
        return rc;
    }
    k_memset(&w, 0, sizeof(w));
    k_memset(&t, 0, sizeof(t));
    wait_queue_init(&t.wq);
    t.deadline = dma_fence_now() + SUBMIT_TIMEOUT_NS;
    w.point = point;
    w.waiter = &t;
    wait_add(s, &w);
    for (;;) {
        t.seen = t.events;
        if (w.fence) {
            rc = 0;
            break;
        }
        if (dma_fence_now() >= t.deadline) {
            rc = -ETIME;
            break;
        }
        rc = waiter_sleep(&t);
        if (rc) {
            break;
        }
    }
    wait_remove(s, &w);
    *fence = w.fence;
    if (rc) {
        dma_fence_put(w.fence);
        *fence = NULL;
    }
    drm_syncobj_put(s);
    return rc;
}

/*
 * A wait on an array of syncobjs, as Linux's drm_syncobj_array_wait_timeout
 * takes it: the syncobjs and their points (0 for a binary one), the
 * DRM_SYNCOBJ_WAIT_FLAGS_* flags, the CLOCK_MONOTONIC time it ends, where 0
 * or a time past asks once, and the deadline hint to pass to each fence, or
 * NULL. `first` is the index of the first syncobj found ready, or ~0.
 */
struct array_wait {
    struct drm_syncobj **objs;
    const uint64_t *points;
    uint32_t count, flags;
    int64_t timeout;
    const int64_t *hint;
    uint32_t first;
    struct syncobj_wait *w; /* one wait per syncobj */
    struct waiter t;
};

#define WAIT_SUBMIT (DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT | DRM_SYNCOBJ_WAIT_FLAGS_WAIT_AVAILABLE)

/**
 * Find each syncobj's point, as the first pass of Linux's wait does: 1 when
 * the wait is over already, 0 when it must go on, -EINVAL for a point not
 * submitted without WAIT_FOR_SUBMIT or WAIT_AVAILABLE, or -ENOMEM.
 */
static int32_t array_wait_start(struct array_wait *a)
{
    struct syncobj_wait *w;
    uint32_t i, ready = 0;

    if ((size_t)a->count * sizeof(*w) / sizeof(*w) != a->count) {
        return -ENOMEM;
    }
    a->w = w = (struct syncobj_wait *)kzalloc((size_t)a->count * sizeof(*w));
    if (!w) {
        return -ENOMEM;
    }
    for (i = 0; i < a->count; i++) {
        w[i].point = a->points ? a->points[i] : 0;
        w[i].waiter = &a->t;
        w[i].cb.wait = &w[i];
        if (point_fence(dma_fence_get(a->objs[i]->fence), w[i].point, &w[i].fence)) {
            if (a->flags & WAIT_SUBMIT) {
                continue;
            }
            return -EINVAL;
        }
        if ((a->flags & DRM_SYNCOBJ_WAIT_FLAGS_WAIT_AVAILABLE) ||
            dma_fence_is_signaled(w[i].fence)) {
            if (ready++ == 0) {
                a->first = i;
            }
        }
    }
    return ready == a->count || (ready && !(a->flags & DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL));
}

/** Look at every point once, arming a callback on each fence not yet
 * signalled: 1 when the wait is over, else 0. */
static int array_wait_scan(struct array_wait *a)
{
    struct syncobj_wait *w;
    uint32_t i, ready = 0;

    a->t.seen = a->t.events;
    for (i = 0; i < a->count; i++) {
        w = &a->w[i];
        if (!w->fence) {
            continue;
        }
        if ((a->flags & DRM_SYNCOBJ_WAIT_FLAGS_WAIT_AVAILABLE) || dma_fence_is_signaled(w->fence) ||
            (!w->cb.cb.func && dma_fence_add_callback(w->fence, &w->cb.cb, wait_fence_done))) {
            if (!(a->flags & DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL)) {
                a->first = i;
                return 1;
            }
            ready++;
        }
    }
    return ready == a->count;
}

/** Take every wait off its syncobj and its fence, and free them. */
static void array_wait_finish(struct array_wait *a)
{
    struct syncobj_wait *w;
    uint32_t i;

    for (i = 0; a->w && i < a->count; i++) {
        w = &a->w[i];
        wait_remove(a->objs[i], w);
        if (w->fence && w->cb.cb.func) {
            dma_fence_remove_callback(w->fence, &w->cb.cb);
        }
        dma_fence_put(w->fence);
    }
    kfree(a->w);
    a->w = NULL;
}

/**
 * Wait as `a` asks: for any syncobj, or all with WAIT_ALL; for the points to
 * be submitted first with WAIT_FOR_SUBMIT, or only submitted with
 * WAIT_AVAILABLE. A fence's error does not fail the wait. 0 with
 * `a->first` set, -ETIME, -EINTR, -EINVAL or -ENOMEM. May sleep.
 */
static int32_t array_wait(struct array_wait *a)
{
    uint32_t i;
    int32_t rc;

    a->first = ~0u;
    a->w = NULL;
    k_memset(&a->t, 0, sizeof(a->t));
    wait_queue_init(&a->t.wq);
    a->t.deadline = a->timeout == 0 || a->timeout <= dma_fence_now() ? 0 : a->timeout;
    rc = array_wait_start(a);
    if (rc) {
        array_wait_finish(a);
        return rc < 0 ? rc : 0;
    }
    for (i = 0; (a->flags & WAIT_SUBMIT) && i < a->count; i++) {
        wait_add(a->objs[i], &a->w[i]);
    }
    for (i = 0; a->hint && i < a->count; i++) {
        if (a->w[i].fence) {
            dma_fence_set_deadline(a->w[i].fence, *a->hint);
        }
    }
    while (!array_wait_scan(a)) {
        if (dma_fence_now() >= a->t.deadline) {
            rc = -ETIME;
            break;
        }
        rc = waiter_sleep(&a->t);
        if (rc) {
            break;
        }
    }
    array_wait_finish(a);
    return rc;
}

/* ------------------------------------------------------------------------
 * Files
 */

/** Drop the file's reference to its syncobj. */
static void syncobj_file_close(fd_entry_t *e)
{
    struct drm_syncobj_file *f = (struct drm_syncobj_file *)e->priv;

    if (f) {
        e->priv = NULL;
        drm_syncobj_put(f->syncobj);
        kfree(f);
    }
}

static const struct file_ops syncobj_file_ops = {NULL, NULL, NULL, NULL, syncobj_file_close,
                                                 NULL, NULL, 0};

/** A close-on-exec descriptor of the caller's on `s`, holding a reference
 * of its own, shown as anon_inode:syncobj_file as on Linux: the
 * descriptor, or -ENOMEM or the table's error. */
static int32_t syncobj_file_install(struct drm_syncobj *s)
{
    struct drm_syncobj_file *f;
    fd_entry_t *e;
    int32_t fd;

    if (!current_proc) {
        return -EINVAL;
    }
    f = (struct drm_syncobj_file *)kzalloc(sizeof(*f));
    if (!f) {
        return -ENOMEM;
    }
    fd = fd_alloc(current_proc->fds);
    if (fd < 0) {
        kfree(f);
        return fd;
    }
    f->anon.name = "syncobj_file";
    f->syncobj = s;
    drm_syncobj_get(s);
    e = fd_slot(current_proc->fds, fd);
    e->type = FD_TYPE_ANON;
    e->ops = &syncobj_file_ops;
    e->priv = f;
    e->flags = FD_FLAG_READ | FD_FLAG_CLOEXEC;
    fd_install(current_proc->fds, fd);
    return fd;
}

/** The syncobj of the caller's descriptor `fd`, with a reference, or NULL
 * when it is none. */
static struct drm_syncobj *syncobj_file_get(int32_t fd)
{
    fd_entry_t *e = current_proc ? fd_get(current_proc->fds, fd) : NULL;
    struct drm_syncobj *s;

    if (!e || (e->flags & FD_FLAG_PATH) || e->ops != &syncobj_file_ops || !e->priv) {
        return NULL;
    }
    s = ((struct drm_syncobj_file *)e->priv)->syncobj;
    drm_syncobj_get(s);
    return s;
}

void drm_syncobj_release(struct drm_file *file)
{
    uint32_t i;

    for (i = drm_handle_next(&file->syncobjs, 1); i; i = drm_handle_next(&file->syncobjs, i + 1)) {
        drm_syncobj_put((struct drm_syncobj *)drm_handle_remove(&file->syncobjs, i));
    }
    drm_handles_fini(&file->syncobjs);
}

/* ------------------------------------------------------------------------
 * Ioctls, Linux 6.12's, each refused with EOPNOTSUPP on a device whose
 * driver lacks DRIVER_SYNCOBJ, or DRIVER_SYNCOBJ_TIMELINE for the timeline
 * calls, as GET_CAP reports them.
 */

/**
 * The syncobjs of the `count` handles at the user address `handles`, each
 * with a reference, into a new array in `*out`, as Linux's
 * drm_syncobj_array_find: 0, -EFAULT, -ENOENT for a handle of no syncobj,
 * or -ENOMEM. May sleep.
 */
static int32_t array_find(struct drm_file *file, uint64_t handles, uint32_t count,
                          struct drm_syncobj ***out)
{
    struct drm_syncobj **objs;
    uint32_t *ids, i;
    int32_t rc = 0;

    *out = NULL;
    if ((size_t)count * sizeof(*objs) / sizeof(*objs) != count) {
        return -ENOMEM;
    }
    ids = (uint32_t *)kmalloc((size_t)count * sizeof(*ids));
    if (!ids) {
        return -ENOMEM;
    }
    if (handles > (uintptr_t)-1 || copy_from_user(ids, (const void __user *)(uintptr_t)handles,
                                                  (size_t)count * sizeof(*ids))) {
        kfree(ids);
        return -EFAULT;
    }
    objs = (struct drm_syncobj **)kzalloc((size_t)count * sizeof(*objs));
    if (!objs) {
        kfree(ids);
        return -ENOMEM;
    }
    for (i = 0; i < count; i++) {
        objs[i] = drm_syncobj_find(file, ids[i]);
        if (!objs[i]) {
            rc = -ENOENT;
            break;
        }
    }
    kfree(ids);
    if (rc) {
        while (i--) {
            drm_syncobj_put(objs[i]);
        }
        kfree(objs);
        return rc;
    }
    *out = objs;
    return 0;
}

/** Drop the syncobjs of `array_find` and the array. */
static void array_free(struct drm_syncobj **objs, uint32_t count)
{
    uint32_t i;

    for (i = 0; i < count; i++) {
        drm_syncobj_put(objs[i]);
    }
    kfree(objs);
}

/** `count` points from the user address `addr` into a new array, or zeros
 * for a NULL address: the array, or NULL with `*err` -ENOMEM or -EFAULT. */
static uint64_t *points_copy(uint64_t addr, uint32_t count, int32_t *err)
{
    uint64_t *points;

    if ((size_t)count * sizeof(*points) / sizeof(*points) != count) {
        *err = -ENOMEM;
        return NULL;
    }
    points = (uint64_t *)kzalloc((size_t)count * sizeof(*points));
    if (!points) {
        *err = -ENOMEM;
        return NULL;
    }
    if (addr &&
        (addr > (uintptr_t)-1 || copy_from_user(points, (const void __user *)(uintptr_t)addr,
                                                (size_t)count * sizeof(*points)))) {
        kfree(points);
        *err = -EFAULT;
        return NULL;
    }
    return points;
}

int32_t drm_syncobj_create_ioctl(struct drm_device *dev, void *data, struct drm_file *file)
{
    struct drm_syncobj_create *args = (struct drm_syncobj_create *)data;
    struct drm_syncobj *s;
    int32_t rc;

    if (!drm_core_check_feature(dev, DRIVER_SYNCOBJ)) {
        return -EOPNOTSUPP;
    }
    if (args->flags & ~(uint32_t)DRM_SYNCOBJ_CREATE_SIGNALED) {
        return -EINVAL;
    }
    s = drm_syncobj_create(args->flags);
    if (!s) {
        return -ENOMEM;
    }
    rc = drm_handle_insert(&file->syncobjs, s, &args->handle);
    if (rc) {
        drm_syncobj_put(s);
    }
    return rc;
}

int32_t drm_syncobj_destroy_ioctl(struct drm_device *dev, void *data, struct drm_file *file)
{
    struct drm_syncobj_destroy *args = (struct drm_syncobj_destroy *)data;
    struct drm_syncobj *s;

    if (!drm_core_check_feature(dev, DRIVER_SYNCOBJ)) {
        return -EOPNOTSUPP;
    }
    if (args->pad) {
        return -EINVAL;
    }
    s = (struct drm_syncobj *)drm_handle_remove(&file->syncobjs, args->handle);
    if (!s) {
        return -EINVAL;
    }
    drm_syncobj_put(s);
    return 0;
}

int32_t drm_syncobj_handle_to_fd_ioctl(struct drm_device *dev, void *data, struct drm_file *file)
{
    struct drm_syncobj_handle *args = (struct drm_syncobj_handle *)data;
    struct drm_syncobj *s;
    struct dma_fence *fence;
    int32_t fd;

    if (!drm_core_check_feature(dev, DRIVER_SYNCOBJ)) {
        return -EOPNOTSUPP;
    }
    if (args->pad) {
        return -EINVAL;
    }
    if (args->flags != 0 && args->flags != DRM_SYNCOBJ_HANDLE_TO_FD_FLAGS_EXPORT_SYNC_FILE) {
        return -EINVAL;
    }
    if (args->flags) {
        fd = drm_syncobj_find_fence(file, args->handle, 0, 0, &fence);
        if (fd) {
            return fd;
        }
        fd = sync_file_create(fence, NULL);
        dma_fence_put(fence);
    } else {
        s = drm_syncobj_find(file, args->handle);
        if (!s) {
            return -EINVAL;
        }
        fd = syncobj_file_install(s);
        drm_syncobj_put(s);
    }
    if (fd < 0) {
        return fd;
    }
    args->fd = fd;
    return 0;
}

int32_t drm_syncobj_fd_to_handle_ioctl(struct drm_device *dev, void *data, struct drm_file *file)
{
    struct drm_syncobj_handle *args = (struct drm_syncobj_handle *)data;
    struct drm_syncobj *s;
    struct dma_fence *fence;
    int32_t rc;

    if (!drm_core_check_feature(dev, DRIVER_SYNCOBJ)) {
        return -EOPNOTSUPP;
    }
    if (args->pad) {
        return -EINVAL;
    }
    if (args->flags != 0 && args->flags != DRM_SYNCOBJ_FD_TO_HANDLE_FLAGS_IMPORT_SYNC_FILE) {
        return -EINVAL;
    }
    if (args->flags) {
        fence = sync_file_get_fence(args->fd);
        if (!fence) {
            return -EINVAL;
        }
        s = drm_syncobj_find(file, args->handle);
        if (!s) {
            dma_fence_put(fence);
            return -ENOENT;
        }
        drm_syncobj_replace_fence(s, fence);
        dma_fence_put(fence);
        drm_syncobj_put(s);
        return 0;
    }
    s = syncobj_file_get(args->fd);
    if (!s) {
        return -EINVAL;
    }
    rc = drm_handle_insert(&file->syncobjs, s, &args->handle);
    if (rc) {
        drm_syncobj_put(s);
    }
    return rc;
}

int32_t drm_syncobj_transfer_ioctl(struct drm_device *dev, void *data, struct drm_file *file)
{
    struct drm_syncobj_transfer *args = (struct drm_syncobj_transfer *)data;
    struct drm_syncobj *dst;
    struct dma_fence *fence, *leaves;
    struct dma_fence_chain *chain;
    int32_t rc;

    if (!drm_core_check_feature(dev, DRIVER_SYNCOBJ_TIMELINE)) {
        return -EOPNOTSUPP;
    }
    if (args->pad) {
        return -EINVAL;
    }
    dst = drm_syncobj_find(file, args->dst_handle);
    if (!dst) {
        return -ENOENT;
    }
    rc = drm_syncobj_find_fence(file, args->src_handle, args->src_point, args->flags, &fence);
    if (rc) {
        drm_syncobj_put(dst);
        return rc;
    }
    if (!args->dst_point) {
        drm_syncobj_replace_fence(dst, fence);
        dma_fence_put(fence);
        drm_syncobj_put(dst);
        return 0;
    }
    /* A timeline point holds the source's leaves, never a chain of
     * chains, as Linux's dma_fence_unwrap_merge gives them. */
    leaves = dma_fence_unwrap_merge(&fence, 1);
    dma_fence_put(fence);
    chain = leaves ? dma_fence_chain_alloc() : NULL;
    if (!chain) {
        dma_fence_put(leaves);
        drm_syncobj_put(dst);
        return -ENOMEM;
    }
    drm_syncobj_add_point(dst, chain, leaves, args->dst_point);
    dma_fence_put(leaves);
    drm_syncobj_put(dst);
    return 0;
}

/** The wait of SYNCOBJ_WAIT and SYNCOBJ_TIMELINE_WAIT, whose flags,
 * timeout and hint `a` holds: the syncobjs of `count` handles at the user
 * address `handles`, at the points at `points`, or 0 for none. No handles
 * is 0 at once, as Linux. */
static int32_t wait_ioctl(struct drm_file *file, struct array_wait *a, uint64_t handles,
                          uint64_t points)
{
    uint64_t *copy;
    int32_t rc;

    if (a->count == 0) {
        return 0;
    }
    rc = array_find(file, handles, a->count, &a->objs);
    if (rc) {
        return rc;
    }
    copy = points_copy(points, a->count, &rc);
    if (copy) {
        a->points = copy;
        rc = array_wait(a);
        kfree(copy);
    }
    array_free(a->objs, a->count);
    return rc;
}

int32_t drm_syncobj_wait_ioctl(struct drm_device *dev, void *data, struct drm_file *file)
{
    struct drm_syncobj_wait *args = (struct drm_syncobj_wait *)data;
    struct array_wait a;
    int64_t hint = (int64_t)args->deadline_nsec;
    int32_t rc;

    if (!drm_core_check_feature(dev, DRIVER_SYNCOBJ)) {
        return -EOPNOTSUPP;
    }
    if (args->flags &
        ~(uint32_t)(DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL | DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT |
                    DRM_SYNCOBJ_WAIT_FLAGS_WAIT_DEADLINE)) {
        return -EINVAL;
    }
    k_memset(&a, 0, sizeof(a));
    a.count = args->count_handles;
    a.flags = args->flags;
    a.timeout = args->timeout_nsec;
    a.hint = (args->flags & DRM_SYNCOBJ_WAIT_FLAGS_WAIT_DEADLINE) ? &hint : NULL;
    rc = wait_ioctl(file, &a, args->handles, 0);
    if (rc == 0) {
        args->first_signaled = a.first;
    }
    return rc;
}

int32_t drm_syncobj_timeline_wait_ioctl(struct drm_device *dev, void *data, struct drm_file *file)
{
    struct drm_syncobj_timeline_wait *args = (struct drm_syncobj_timeline_wait *)data;
    struct array_wait a;
    int64_t hint = (int64_t)args->deadline_nsec;
    int32_t rc;

    if (!drm_core_check_feature(dev, DRIVER_SYNCOBJ_TIMELINE)) {
        return -EOPNOTSUPP;
    }
    if (args->flags &
        ~(uint32_t)(DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL | DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT |
                    DRM_SYNCOBJ_WAIT_FLAGS_WAIT_AVAILABLE | DRM_SYNCOBJ_WAIT_FLAGS_WAIT_DEADLINE)) {
        return -EINVAL;
    }
    k_memset(&a, 0, sizeof(a));
    a.count = args->count_handles;
    a.flags = args->flags;
    a.timeout = args->timeout_nsec;
    a.hint = (args->flags & DRM_SYNCOBJ_WAIT_FLAGS_WAIT_DEADLINE) ? &hint : NULL;
    rc = wait_ioctl(file, &a, args->handles, args->points);
    if (rc == 0) {
        args->first_signaled = a.first;
    }
    return rc;
}

int32_t drm_syncobj_eventfd_ioctl(struct drm_device *dev, void *data, struct drm_file *file)
{
    struct drm_syncobj_eventfd *args = (struct drm_syncobj_eventfd *)data;
    struct drm_syncobj *s;
    struct syncobj_eventfd *e;
    struct eventfd *ev;
    int32_t rc;

    if (!drm_core_check_feature(dev, DRIVER_SYNCOBJ_TIMELINE)) {
        return -EOPNOTSUPP;
    }
    if (args->flags & ~(uint32_t)DRM_SYNCOBJ_WAIT_FLAGS_WAIT_AVAILABLE) {
        return -EINVAL;
    }
    if (args->pad) {
        return -EINVAL;
    }
    s = drm_syncobj_find(file, args->handle);
    if (!s) {
        return -ENOENT;
    }
    ev = eventfd_get(args->fd, &rc);
    if (!ev) {
        drm_syncobj_put(s);
        return rc;
    }
    e = (struct syncobj_eventfd *)kzalloc(sizeof(*e));
    if (!e) {
        eventfd_put(ev);
        drm_syncobj_put(s);
        return -ENOMEM;
    }
    e->ev = ev;
    e->point = args->point;
    e->flags = args->flags;
    e->cb.entry = e;
    e->next = s->eventfds;
    s->eventfds = e;
    eventfd_check(s, e);
    drm_syncobj_put(s);
    return 0;
}

/** Act on each syncobj of a RESET or SIGNAL: the array's checks, as
 * Linux's, then `fence` given to each, or taken away when NULL. */
static int32_t array_set(struct drm_device *dev, struct drm_file *file,
                         struct drm_syncobj_array *args, struct dma_fence *fence)
{
    struct drm_syncobj **objs;
    uint32_t i;
    int32_t rc;

    if (!drm_core_check_feature(dev, DRIVER_SYNCOBJ)) {
        return -EOPNOTSUPP;
    }
    if (args->pad != 0 || args->count_handles == 0) {
        return -EINVAL;
    }
    rc = array_find(file, args->handles, args->count_handles, &objs);
    if (rc) {
        return rc;
    }
    for (i = 0; i < args->count_handles; i++) {
        drm_syncobj_replace_fence(objs[i], fence);
    }
    array_free(objs, args->count_handles);
    return 0;
}

int32_t drm_syncobj_reset_ioctl(struct drm_device *dev, void *data, struct drm_file *file)
{
    return array_set(dev, file, (struct drm_syncobj_array *)data, NULL);
}

int32_t drm_syncobj_signal_ioctl(struct drm_device *dev, void *data, struct drm_file *file)
{
    struct dma_fence *stub = dma_fence_get_stub();
    int32_t rc = array_set(dev, file, (struct drm_syncobj_array *)data, stub);

    dma_fence_put(stub);
    return rc;
}

int32_t drm_syncobj_timeline_signal_ioctl(struct drm_device *dev, void *data, struct drm_file *file)
{
    struct drm_syncobj_timeline_array *args = (struct drm_syncobj_timeline_array *)data;
    struct drm_syncobj **objs;
    struct dma_fence_chain **chains;
    struct dma_fence *stub;
    uint64_t *points;
    uint32_t i, n = args->count_handles;
    int32_t rc;

    if (!drm_core_check_feature(dev, DRIVER_SYNCOBJ_TIMELINE)) {
        return -EOPNOTSUPP;
    }
    if (args->flags != 0 || n == 0) {
        return -EINVAL;
    }
    rc = array_find(file, args->handles, n, &objs);
    if (rc) {
        return rc;
    }
    points = points_copy(args->points, n, &rc);
    if (!points) {
        array_free(objs, n);
        return rc;
    }
    chains = (struct dma_fence_chain **)kzalloc((size_t)n * sizeof(*chains));
    for (i = 0; chains && i < n; i++) {
        chains[i] = dma_fence_chain_alloc();
        if (!chains[i]) {
            break;
        }
    }
    if (!chains || i < n) {
        while (chains && i--) {
            kfree(chains[i]);
        }
        kfree(chains);
        kfree(points);
        array_free(objs, n);
        return -ENOMEM;
    }
    stub = dma_fence_get_stub();
    /* Linux chains every point, 0 too, which keeps the object a timeline. */
    for (i = 0; i < n; i++) {
        drm_syncobj_add_point(objs[i], chains[i], stub, points[i]);
        chains[i] = NULL;
    }
    dma_fence_put(stub);
    for (i = 0; i < n; i++) {
        kfree(chains[i]);
    }
    kfree(chains);
    kfree(points);
    array_free(objs, n);
    return 0;
}

/**
 * The point a timeline syncobj has reached, as Linux's QUERY: the newest
 * point with LAST_SUBMITTED; else the newest point whose fence and those
 * before it have signalled, read from the oldest point of the newest's
 * context not yet signalled, so that a point out of order hides those
 * before it. 0 for a binary syncobj. Does not sleep.
 */
static uint64_t query_point(struct drm_syncobj *s, uint32_t flags)
{
    struct dma_fence *head = dma_fence_get(s->fence), *iter, *last;
    uint64_t point = 0;

    if (to_dma_fence_chain(head)) {
        if (flags & DRM_SYNCOBJ_QUERY_FLAGS_LAST_SUBMITTED) {
            point = head->seqno;
        } else {
            last = dma_fence_get(head);
            for (iter = dma_fence_get(head); iter; iter = dma_fence_chain_walk(iter)) {
                if (iter->context != head->context) {
                    dma_fence_put(iter);
                    break;
                }
                dma_fence_put(last);
                last = dma_fence_get(iter);
            }
            point =
                dma_fence_is_signaled(last) ? last->seqno : to_dma_fence_chain(last)->prev_seqno;
            dma_fence_put(last);
        }
    }
    dma_fence_put(head);
    return point;
}

int32_t drm_syncobj_query_ioctl(struct drm_device *dev, void *data, struct drm_file *file)
{
    struct drm_syncobj_timeline_array *args = (struct drm_syncobj_timeline_array *)data;
    struct drm_syncobj **objs;
    uint64_t point;
    uint32_t i;
    int32_t rc;

    if (!drm_core_check_feature(dev, DRIVER_SYNCOBJ_TIMELINE)) {
        return -EOPNOTSUPP;
    }
    if (args->flags & ~(uint32_t)DRM_SYNCOBJ_QUERY_FLAGS_LAST_SUBMITTED) {
        return -EINVAL;
    }
    if (args->count_handles == 0) {
        return -EINVAL;
    }
    rc = array_find(file, args->handles, args->count_handles, &objs);
    if (rc) {
        return rc;
    }
    for (i = 0; i < args->count_handles; i++) {
        point = query_point(objs[i], args->flags);
        if (args->points + (uint64_t)i * sizeof(point) > (uintptr_t)-1 ||
            copy_to_user((void __user *)(uintptr_t)(args->points + (uint64_t)i * sizeof(point)),
                         &point, sizeof(point))) {
            rc = -EFAULT;
            break;
        }
    }
    array_free(objs, args->count_handles);
    return rc;
}
