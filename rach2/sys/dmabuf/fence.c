/* Fence lifetimes, completion callbacks, waits and flattened all-of fences. */
#include "dmabuf.h"
#include "../mm/kheap.h"
#include "../lib/kerrno.h"
#include "../lib/kirq.h"
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

/** Stamp completion, run each callback and wake all.
 * The mark is made with interrupts masked, since an interrupt may signal
 * the fence while a thread adds or removes a callback; once it is made,
 * dma_fence_add_callback refuses. Each callback stays on the list, where
 * dma_fence_remove_callback can find it, until it is taken off to be
 * called, so that a callback may free the owner of another one still
 * pending. The callbacks run in the signaller's context. */
int32_t dma_fence_signal(struct dma_fence *f)
{
    struct dma_fence_cb *cb;
    uint32_t flags;
    if (!f)
        return -EINVAL;
    flags = kirq_save();
    if (f->signaled) {
        kirq_restore(flags);
        return -EINVAL;
    }
    dma_fence_get(f);
    f->signaled = 1;
    f->timestamp_ns = dma_fence_now();
    kirq_restore(flags);
    for (;;) {
        flags = kirq_save();
        cb = f->cbs;
        if (cb) {
            f->cbs = cb->next;
            cb->next = NULL;
        }
        kirq_restore(flags);
        if (!cb)
            break;
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

/** Add a callback unless completion has already happened: 0, or -ENOENT
 * when it has. The test and the link are one step with interrupts masked,
 * so that a signal from an interrupt either finds the callback or comes
 * first. */
int32_t dma_fence_add_callback(struct dma_fence *f, struct dma_fence_cb *cb,
                               void (*func)(struct dma_fence *, struct dma_fence_cb *))
{
    uint32_t flags;
    if (dma_fence_is_signaled(f))
        return -ENOENT;
    flags = kirq_save();
    if (f->signaled) {
        kirq_restore(flags);
        return -ENOENT;
    }
    cb->func = func;
    cb->next = f->cbs;
    f->cbs = cb;
    kirq_restore(flags);
    return 0;
}

/** Detach a still-pending callback, returning whether it was found. */
int dma_fence_remove_callback(struct dma_fence *f, struct dma_fence_cb *cb)
{
    struct dma_fence_cb **p;
    uint32_t flags;
    if (!f)
        return 0;
    flags = kirq_save();
    for (p = &f->cbs; *p; p = &(*p)->next) {
        if (*p == cb) {
            *p = cb->next;
            cb->next = NULL;
            kirq_restore(flags);
            return 1;
        }
    }
    kirq_restore(flags);
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

static const struct dma_fence_ops chain_ops;

/** Return a fence's chain record, or NULL for any other fence. */
struct dma_fence_chain *to_dma_fence_chain(struct dma_fence *f)
{
    return f && f->ops == &chain_ops ? (struct dma_fence_chain *)f : NULL;
}

/**
 * Walk the leaves of `f` in the order of Linux's dma_fence_unwrap: a
 * chain's own fence, then the fence of each earlier point not yet
 * signalled, or `f` alone, each array among them by its fences. Count them
 * in `*count` and return the borrowed leaf `index`, or NULL past the end.
 * Changes nothing but what a signalled test enables; does not sleep.
 */
static struct dma_fence *unwrap(struct dma_fence *f, uint32_t index, uint32_t *count)
{
    struct dma_fence_chain *c;
    struct dma_fence *node, *leaf, *found = NULL;
    uint32_t n = 0, k;
    for (node = f; node; node = c ? c->prev : NULL) {
        c = to_dma_fence_chain(node);
        leaf = c ? c->fence : node;
        if (node != f && dma_fence_is_signaled(leaf))
            continue;
        k = leaf->ops == &array_ops ? ((struct fence_array *)leaf)->count : 1;
        if (!found && index >= n && index - n < k)
            found =
                leaf->ops == &array_ops ? ((struct fence_array *)leaf)->fences[index - n] : leaf;
        n += k;
    }
    *count = n;
    return found;
}

/** Count the leaves that Linux's dma_fence_unwrap visits. */
uint32_t dma_fence_count(struct dma_fence *f)
{
    uint32_t n;
    unwrap(f, 0, &n);
    return n;
}

/** Return a borrowed leaf in unwrap order. */
struct dma_fence *dma_fence_at(struct dma_fence *f, uint32_t index)
{
    uint32_t n;
    return unwrap(f, index, &n);
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
    if (a->count < 2) {
        f = a->count ? a->fences[0] : kzalloc(sizeof(*f));
        if (!a->count && f) {
            dma_fence_init(f, NULL, dma_fence_context_alloc(1), 1);
            dma_fence_signal(f);
        }
        a->count = 0;
        array_release(&a->base);
        return f;
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

/** Merge the leaves still active of each fence as Linux's
 * dma_fence_unwrap_merge does, omitting work already finished and keeping
 * the newest completion time when nothing is left. */
struct dma_fence *dma_fence_unwrap_merge(struct dma_fence **fences, uint32_t count)
{
    struct dma_fence **leaves, *f, *out;
    uint32_t i, j, n = 0, total = 0;
    int64_t timestamp = 0;
    for (i = 0; i < count; i++) {
        j = dma_fence_count(fences[i]);
        if (j > 0xffffffffu - total)
            return NULL;
        total += j;
    }
    if ((size_t)total * sizeof(*leaves) / sizeof(*leaves) != total)
        return NULL;
    leaves = kmalloc(total ? (size_t)total * sizeof(*leaves) : sizeof(*leaves));
    if (!leaves)
        return NULL;
    for (i = 0; i < count; i++)
        for (j = 0; j < dma_fence_count(fences[i]); j++) {
            f = dma_fence_at(fences[i], j);
            if (!dma_fence_is_signaled(f))
                leaves[n++] = f;
            else if (f->timestamp_ns > timestamp)
                timestamp = f->timestamp_ns;
        }
    out = dma_fence_merge(leaves, n);
    if (!n && out)
        out->timestamp_ns = timestamp;
    kfree(leaves);
    return out;
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
    if (f->ops == &array_ops || f->ops == &chain_ops)
        for (i = 0; i < dma_fence_count(f); i++)
            dma_fence_set_deadline(dma_fence_at(f, i), deadline);
}

/* ------------------------------------------------------------------------
 * Chains: the points of a timeline, Linux's dma_fence_chain
 */

/** The chain's driver name, Linux's. */
static const char *chain_driver_name(struct dma_fence *f)
{
    (void)f;
    return "dma_fence_chain";
}

/** The chain's timeline name, Linux's. */
static const char *chain_timeline_name(struct dma_fence *f)
{
    (void)f;
    return "unbound";
}

static void chain_done(struct dma_fence *f, struct dma_fence_cb *cb);

/**
 * Wait on the first fence of the chain from `c` back that has not
 * signalled: its own, then each earlier point's. Return 1 with the
 * callback armed, or 0 when every one has signalled.
 */
static int chain_arm(struct dma_fence_chain *c)
{
    struct dma_fence_chain *p;
    struct dma_fence *node = &c->base, *f;
    while (node) {
        p = to_dma_fence_chain(node);
        f = p ? p->fence : node;
        if (!dma_fence_add_callback(f, &c->cb.cb, chain_done)) {
            c->armed = f;
            return 1;
        }
        node = p ? p->prev : NULL;
    }
    return 0;
}

/** Move to the next fence the chain waits on, or signal the chain. Frees
 * nothing, since a fence may signal in an interrupt. */
static void chain_done(struct dma_fence *f, struct dma_fence_cb *cb)
{
    struct dma_fence_chain *c = ((struct dma_fence_chain_cb *)cb)->chain;
    (void)f;
    c->armed = NULL;
    if (!chain_arm(c))
        dma_fence_signal(&c->base);
}

/** Start waiting on the first fence of the chain not yet signalled. */
static int chain_enable_signaling(struct dma_fence *f)
{
    return chain_arm((struct dma_fence_chain *)f);
}

/** Release a point and, without recursion, each earlier point it held last. */
static void chain_release(struct dma_fence *f)
{
    struct dma_fence_chain *c = (struct dma_fence_chain *)f, *p;
    struct dma_fence *prev = c->prev, *next;
    if (c->armed)
        dma_fence_remove_callback(c->armed, &c->cb.cb);
    c->prev = NULL;
    while (prev && prev->refs == 1 && (p = to_dma_fence_chain(prev)) != NULL) {
        next = p->prev;
        p->prev = NULL;
        dma_fence_put(prev);
        prev = next;
    }
    dma_fence_put(prev);
    dma_fence_put(c->fence);
    kfree(c);
}

static const struct dma_fence_ops chain_ops = {chain_driver_name, chain_timeline_name,
                                               chain_enable_signaling, chain_release};

/** Allocate a chain record for dma_fence_chain_init. */
struct dma_fence_chain *dma_fence_chain_alloc(void)
{
    return kzalloc(sizeof(struct dma_fence_chain));
}

/** Make `c` point `seqno` after `prev`, continuing prev's context when it
 * is an earlier point, as Linux's dma_fence_chain_init does. */
void dma_fence_chain_init(struct dma_fence_chain *c, struct dma_fence *prev,
                          struct dma_fence *fence, uint64_t seqno)
{
    struct dma_fence_chain *p = to_dma_fence_chain(prev);
    uint64_t context, prev_seqno = 0;
    if (p && seqno > prev->seqno) {
        context = prev->context;
        prev_seqno = prev->seqno;
    } else {
        context = dma_fence_context_alloc(1);
        if (p && prev->seqno > seqno)
            seqno = prev->seqno;
    }
    dma_fence_init(&c->base, &chain_ops, context, seqno);
    c->prev = prev;
    c->prev_seqno = prev_seqno;
    c->fence = fence;
    c->armed = NULL;
    c->cb.chain = c;
}

/** Step back from `f`, dropping the earlier points whose fences have
 * signalled, as Linux's dma_fence_chain_walk does. */
struct dma_fence *dma_fence_chain_walk(struct dma_fence *f)
{
    struct dma_fence_chain *c = to_dma_fence_chain(f), *p;
    struct dma_fence *prev;
    if (!c) {
        dma_fence_put(f);
        return NULL;
    }
    while ((prev = c->prev) != NULL) {
        p = to_dma_fence_chain(prev);
        if (!dma_fence_is_signaled(p ? p->fence : prev))
            break;
        c->prev = p ? dma_fence_get(p->prev) : NULL;
        dma_fence_put(prev);
    }
    dma_fence_get(prev);
    dma_fence_put(f);
    return prev;
}

/** Find the point that covers `seqno`, as Linux's
 * dma_fence_chain_find_seqno does. */
int32_t dma_fence_chain_find_seqno(struct dma_fence **pfence, uint64_t seqno)
{
    struct dma_fence_chain *c;
    struct dma_fence *f;
    if (!seqno)
        return 0;
    c = to_dma_fence_chain(*pfence);
    if (!c || c->base.seqno < seqno)
        return -EINVAL;
    for (f = dma_fence_get(&c->base); f; f = dma_fence_chain_walk(f))
        if (f->context != c->base.context || to_dma_fence_chain(f)->prev_seqno < seqno)
            break;
    dma_fence_put(&c->base);
    *pfence = f;
    return 0;
}

/** Name the stub fence as Linux's dma_fence_stub_ops does. */
static const char *stub_name(struct dma_fence *f)
{
    (void)f;
    return "stub";
}

static const struct dma_fence_ops stub_ops = {stub_name, stub_name, NULL, NULL};

/** Return a reference to the shared signalled stub, made at first use. */
struct dma_fence *dma_fence_get_stub(void)
{
    static struct dma_fence stub;
    if (!stub.ops) {
        dma_fence_init(&stub, &stub_ops, dma_fence_context_alloc(1), 0);
        dma_fence_signal(&stub);
    }
    return dma_fence_get(&stub);
}
