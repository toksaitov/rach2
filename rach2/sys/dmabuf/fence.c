/* Fence lifetimes, completion callbacks, waits and flattened all-of fences. */
#include "dmabuf.h"
#include "../mm/kheap.h"
#include "../lib/kerrno.h"
#include "../lib/kstring.h"
#include "../rtc.h"
#include "../timer.h"

/** Read monotonic time in the fence ABI's nanoseconds. */
int64_t dma_fence_now(void)
{
    int64_t sec, ns;
    rtc_monotonic(&sec, &ns);
    return sec * 1000000000ll + ns;
}

/** Allocate distinct contexts under the big kernel lock. */
uint64_t dma_fence_context_alloc(uint32_t num)
{
    static uint64_t next = 1;
    uint64_t first = next;
    next += num;
    return first;
}

/** Initialize a caller-owned fence with one reference. */
void dma_fence_init(struct dma_fence *f, const struct dma_fence_ops *ops, uint64_t context,
                    uint64_t seqno)
{
    k_memset(f, 0, sizeof(*f));
    f->ops = ops;
    f->context = context;
    f->seqno = seqno;
    f->refs = 1;
    f->deadline_ns = 0x7fffffffffffffffll;
    wait_queue_init(&f->wq);
}

/** Take a fence reference, allowing NULL. */
struct dma_fence *dma_fence_get(struct dma_fence *f)
{
    if (f)
        f->refs++;
    return f;
}

/** Drop a fence reference and release its record at zero. */
void dma_fence_put(struct dma_fence *f)
{
    if (!f || --f->refs)
        return;
    if (f->ops && f->ops->release)
        f->ops->release(f);
    else
        kfree(f);
}

/** Set the producer's negative error before completion. */
void dma_fence_set_error(struct dma_fence *f, int32_t error)
{
    if (f && !f->signaled && error < 0)
        f->error = error;
}

/** Stamp completion, detach each callback before calling it, and wake all. */
int32_t dma_fence_signal(struct dma_fence *f)
{
    struct dma_fence_cb *cb;
    if (!f || f->signaled)
        return -EINVAL;
    dma_fence_get(f);
    f->signaled = 1;
    f->timestamp_ns = dma_fence_now();
    while ((cb = f->cbs) != NULL) {
        f->cbs = cb->next;
        cb->next = NULL;
        cb->func(f, cb);
    }
    wq_wake_all(&f->wq);
    dma_fence_put(f);
    return 0;
}

/** Enable a driver's completion interrupt once, allowing synchronous signal. */
int dma_fence_is_signaled(struct dma_fence *f)
{
    if (!f)
        return 1;
    if (!f->signaled && !f->signaling_enabled) {
        f->signaling_enabled = 1;
        if (f->ops && f->ops->enable_signaling && !f->ops->enable_signaling(f) && !f->signaled)
            dma_fence_signal(f);
    }
    return f->signaled;
}

/** Report completion or the producer's error. */
int32_t dma_fence_status(struct dma_fence *f)
{
    return dma_fence_is_signaled(f) ? (f && f->error ? f->error : 1) : 0;
}

/** Add a callback unless completion has already happened. */
int32_t dma_fence_add_callback(struct dma_fence *f, struct dma_fence_cb *cb,
                               void (*func)(struct dma_fence *, struct dma_fence_cb *))
{
    if (dma_fence_is_signaled(f))
        return -ENOENT;
    cb->func = func;
    cb->next = f->cbs;
    f->cbs = cb;
    return 0;
}

/** Detach a still-pending callback, returning whether it was found. */
int dma_fence_remove_callback(struct dma_fence *f, struct dma_fence_cb *cb)
{
    struct dma_fence_cb **p;
    if (!f)
        return 0;
    for (p = &f->cbs; *p; p = &(*p)->next) {
        if (*p == cb) {
            *p = cb->next;
            cb->next = NULL;
            return 1;
        }
    }
    return 0;
}

struct fence_wait {
    struct dma_fence *fence;
    int64_t deadline;
};

/** End a wait on completion or at the caller's deadline. */
static int fence_ready(void *arg)
{
    struct fence_wait *w = arg;
    return dma_fence_is_signaled(w->fence) || (w->deadline >= 0 && dma_fence_now() >= w->deadline);
}

/** Wait with a reference held, restoring the thread's previous timer. */
int32_t dma_fence_wait_until(struct dma_fence *f, int intr, int64_t deadline_ns)
{
    struct fence_wait w;
    uint64_t saved, delta;
    int64_t left;
    int rc;
    if (dma_fence_is_signaled(f))
        return 0;
    if (deadline_ns >= 0 && dma_fence_now() >= deadline_ns)
        return -ETIME;
    if (!current_proc)
        return -EINVAL;
    w.fence = dma_fence_get(f);
    w.deadline = deadline_ns;
    saved = current_proc->wakeup_tick;
    if (deadline_ns >= 0) {
        left = deadline_ns - dma_fence_now();
        delta = tick_div((uint64_t)(left > 0 ? left : 0), 1000, NULL);
        delta = tick_div(delta + 999, 1000, NULL);
        current_proc->wakeup_tick = rtc_ticks_now() + delta + 1;
    } else
        current_proc->wakeup_tick = 0;
    rc = intr ? wq_wait_event(&f->wq, fence_ready, &w)
              : wq_wait_event_nointr(&f->wq, fence_ready, &w);
    current_proc->wakeup_tick = saved;
    if (rc < 0)
        rc = -EINTR;
    else
        rc = dma_fence_is_signaled(f) ? 0 : -ETIME;
    dma_fence_put(f);
    return rc;
}

struct fence_array;
struct array_cb {
    struct dma_fence_cb cb;
    struct fence_array *array;
};
struct fence_array {
    struct dma_fence base;
    uint32_t count, pending;
    struct dma_fence **fences;
    struct array_cb *callbacks;
};

/** Release an array, withdrawing callbacks before freeing their storage. */
static void array_release(struct dma_fence *f)
{
    struct fence_array *a = (struct fence_array *)f;
    uint32_t i;
    for (i = 0; i < a->count; i++) {
        dma_fence_remove_callback(a->fences[i], &a->callbacks[i].cb);
        dma_fence_put(a->fences[i]);
    }
    kfree(a->callbacks);
    kfree(a->fences);
    kfree(a);
}

static const struct dma_fence_ops array_ops = {NULL, NULL, NULL, array_release};

/** Carry a child's error and complete the array after its last child. */
static void array_done(struct dma_fence *f, struct dma_fence_cb *cb)
{
    struct fence_array *a = ((struct array_cb *)cb)->array;
    if (f->error && !a->base.error)
        dma_fence_set_error(&a->base, f->error);
    if (--a->pending == 0)
        dma_fence_signal(&a->base);
}

/** Count an array's leaves, or one ordinary fence. */
uint32_t dma_fence_count(struct dma_fence *f)
{
    return !f ? 0 : f->ops == &array_ops ? ((struct fence_array *)f)->count : 1;
}

/** Return a borrowed flattened leaf. */
struct dma_fence *dma_fence_at(struct dma_fence *f, uint32_t index)
{
    if (index >= dma_fence_count(f))
        return NULL;
    return f->ops == &array_ops ? ((struct fence_array *)f)->fences[index] : f;
}

/** Build an all-of fence, flattening nested merges and duplicate contexts. */
struct dma_fence *dma_fence_merge(struct dma_fence **fences, uint32_t count)
{
    struct fence_array *a;
    struct dma_fence *f;
    uint32_t i, j, k, total = 0;
    for (i = 0; i < count; i++) {
        j = dma_fence_count(fences[i]);
        if (j > 0xffffffffu - total)
            return NULL;
        total += j;
    }
    if ((size_t)total * sizeof(struct array_cb) / sizeof(struct array_cb) != total)
        return NULL;
    a = kzalloc(sizeof(*a));
    if (!a)
        return NULL;
    dma_fence_init(&a->base, &array_ops, dma_fence_context_alloc(1), 1);
    if (total) {
        a->fences = kzalloc((size_t)total * sizeof(f));
        a->callbacks = kzalloc((size_t)total * sizeof(*a->callbacks));
        if (!a->fences || !a->callbacks) {
            array_release(&a->base);
            return NULL;
        }
    }
    for (i = 0; i < count; i++)
        for (j = 0; j < dma_fence_count(fences[i]); j++) {
            f = dma_fence_at(fences[i], j);
            for (k = 0; k < a->count; k++)
                if (a->fences[k]->context == f->context)
                    break;
            if (k < a->count) {
                if ((int64_t)(f->seqno - a->fences[k]->seqno) <= 0)
                    continue;
                dma_fence_put(a->fences[k]);
            } else
                a->count++;
            a->fences[k] = dma_fence_get(f);
        }
    a->pending = a->count;
    for (i = 0; i < a->count; i++) {
        a->callbacks[i].array = a;
        if (dma_fence_add_callback(a->fences[i], &a->callbacks[i].cb, array_done))
            array_done(a->fences[i], &a->callbacks[i].cb);
    }
    if (!a->count)
        dma_fence_signal(&a->base);
    return &a->base;
}

/** Read the earliest requested deadline of a fence. */
int64_t dma_fence_deadline(struct dma_fence *f)
{
    return f ? f->deadline_ns : 0x7fffffffffffffffll;
}

/** Pass a deadline hint to all children without changing completion. */
void dma_fence_set_deadline(struct dma_fence *f, int64_t deadline)
{
    uint32_t i;
    if (!f)
        return;
    if (deadline < f->deadline_ns)
        f->deadline_ns = deadline;
    if (f->ops == &array_ops)
        for (i = 0; i < dma_fence_count(f); i++)
            dma_fence_set_deadline(dma_fence_at(f, i), deadline);
}
