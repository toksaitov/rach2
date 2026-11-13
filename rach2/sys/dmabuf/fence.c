/* Fence lifetimes, completion callbacks, waits and flattened all-of fences. */
#include "dmabuf.h"
#include "../mm/kheap.h"
#include "../lib/kerrno.h"
#include "../lib/kirq.h"
#include "../lib/kstring.h"
#include "../proc/percpu.h"
#include "../rtc.h"
#include "../timer.h"

/** Read monotonic time in the fence ABI's nanoseconds. */
int64_t dma_fence_now(void)
{
    int64_t sec, ns;
    rtc_monotonic(&sec, &ns);
    return sec * 1000000000ll + ns;
}

/* The timelines given out so far; the lock makes each allocation one
 * step on any processor. */
static struct spinlock context_lock = SPINLOCK_INIT_NAMED("dma_fence_context");
static uint64_t context_next = 1;

/** Allocate `num` distinct contexts. Any context. */
uint64_t dma_fence_context_alloc(uint32_t num)
{
    uint32_t flags = spin_lock_irqsave(&context_lock);
    uint64_t first = context_next;

    context_next += num;
    spin_unlock_irqrestore(&context_lock, flags);
    return first;
}

/** Add 1 to `*refs`, atomically. */
static void fence_ref_get(volatile uint32_t *refs)
{
    uint32_t old;

    do {
        old = *refs;
    } while (arch_atomic_cmpxchg((volatile unsigned int *)refs, old, old + 1u) != old);
}

/** Take 1 from `*refs`, atomically: 1 when that was the last. */
static int fence_ref_put(volatile uint32_t *refs)
{
    uint32_t old;

    do {
        old = *refs;
    } while (arch_atomic_cmpxchg((volatile unsigned int *)refs, old, old - 1u) != old);
    return old == 1u;
}

/** Initialize a caller-owned fence with one reference and its lock, of
 * one class for every fence, since no fence's lock is taken under
 * another's. */
void dma_fence_init(struct dma_fence *f, const struct dma_fence_ops *ops, uint64_t context,
                    uint64_t seqno)
{
    k_memset(f, 0, sizeof(*f));
    spin_lock_init(&f->lock, "dma_fence");
    f->ops = ops;
    f->context = context;
    f->seqno = seqno;
    f->refs = 1;
    f->deadline_ns = 0x7fffffffffffffffll;
    wait_queue_init(&f->wq);
}

/** Order two seqnos of one timeline by its producer's width, as Linux's
 * __dma_fence_is_later (include/linux/dma-fence.h). */
int dma_fence_seqno_is_later(uint64_t f1, uint64_t f2, const struct dma_fence_ops *ops)
{
    if (ops && ops->use_64bit_seqno)
        return f1 > f2;
    return (int32_t)((uint32_t)f1 - (uint32_t)f2) > 0;
}

/** Order two fences of one context, as Linux's dma_fence_is_later. */
int dma_fence_is_later(struct dma_fence *f1, struct dma_fence *f2)
{
    if (f1->context != f2->context)
        return 0;
    return dma_fence_seqno_is_later(f1->seqno, f2->seqno, f1->ops);
}

/** Whether `f1` is `f2` or later, as Linux's dma_fence_is_later_or_same. */
int dma_fence_is_later_or_same(struct dma_fence *f1, struct dma_fence *f2)
{
    return f1 == f2 || dma_fence_is_later(f1, f2);
}

/** Take a fence reference, allowing NULL. Any context. */
struct dma_fence *dma_fence_get(struct dma_fence *f)
{
    if (f)
        fence_ref_get(&f->refs);
    return f;
}

/** Drop a fence reference and release its record at zero. */
void dma_fence_put(struct dma_fence *f)
{
    if (!f || !fence_ref_put(&f->refs))
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

/**
 * Stamp completion, run each callback and wake all, as Linux's
 * dma_fence_signal. The mark is made under the fence's lock, after which
 * dma_fence_add_callback refuses. The callbacks run one at a time with
 * interrupts masked, each taken off the list under the lock just before it
 * runs and named in `running` while it does, so that one still waiting its
 * turn can be removed, and a removal on another processor of the one that
 * runs waits for its end: the remover may free its owner once the removal
 * returns, as once Linux's under the lock does. Unlike Linux's, a callback
 * runs without the fence's lock, since one may signal another fence or add
 * a callback to one, which on Linux an irq_work does after the lock is let
 * go. The callbacks run in the signaller's context and must not sleep.
 */
int32_t dma_fence_signal(struct dma_fence *f)
{
    struct dma_fence_cb *cb;
    uint32_t flags;
    if (!f)
        return -EINVAL;
    flags = spin_lock_irqsave(&f->lock);
    if (f->signaled) {
        spin_unlock_irqrestore(&f->lock, flags);
        return -EINVAL;
    }
    dma_fence_get(f);
    f->timestamp_ns = dma_fence_now();
    f->signaled = 1;
    while ((cb = f->cbs) != NULL) {
        f->cbs = cb->next;
        cb->next = NULL;
        f->running = cb;
        f->running_cpu = this_cpu();
        spin_unlock(&f->lock);
        cb->func(f, cb);
        spin_lock(&f->lock);
        f->running = NULL;
        f->running_cpu = NULL;
    }
    spin_unlock_irqrestore(&f->lock, flags);
    wq_wake_all(&f->wq);
    dma_fence_put(f);
    return 0;
}

/** Answer from the mark, or from the fence's `signaled`, signalling it when
 * that says it may be, as Linux's dma_fence_is_signaled; enables nothing. */
int dma_fence_is_signaled(struct dma_fence *f)
{
    if (!f || f->signaled)
        return 1;
    if (f->ops && f->ops->signaled && f->ops->signaled(f)) {
        (void)dma_fence_signal(f);
        return 1;
    }
    return 0;
}

/**
 * Mark the fence listened under its lock and, the first time, run its
 * `enable_signaling`, signalling the fence when that returns 0, as Linux's
 * __dma_fence_enable_signaling (dma-fence.c:579-601). The mark is the
 * test and set of one step under the lock, so the hook runs once however
 * many processors and interrupts ask; it runs after the lock is let go,
 * since an array's and a chain's add callbacks to other fences, and a
 * signal may come before it or while it runs, which every hook allows.
 * Interrupts are masked. Returns 1 while the fence is unsignalled.
 */
static int enable_masked(struct dma_fence *f)
{
    int was_set;
    spin_lock(&f->lock);
    was_set = f->listened;
    f->listened = 1;
    spin_unlock(&f->lock);
    if (f->signaled)
        return 0;
    if (!was_set && f->ops && f->ops->enable_signaling && !f->ops->enable_signaling(f)) {
        (void)dma_fence_signal(f);
        return 0;
    }
    return !f->signaled;
}

/** Enable signalling with interrupts masked, as Linux's
 * dma_fence_enable_sw_signaling (dma-fence.c:611-618). */
void dma_fence_enable_sw_signaling(struct dma_fence *f)
{
    uint32_t flags;
    if (!f)
        return;
    flags = kirq_save();
    (void)enable_masked(f);
    kirq_restore(flags);
}

/** Report completion or the producer's error. */
int32_t dma_fence_status(struct dma_fence *f)
{
    return dma_fence_is_signaled(f) ? (f && f->error ? f->error : 1) : 0;
}

/** Add a callback unless completion has already happened: 0, or -ENOENT
 * when it has. Signalling is enabled first, and the callback linked under
 * the fence's lock unless the mark is made by then, so that a signal on any
 * processor either finds it or came first and the callback never runs
 * inside this call, as Linux's dma_fence_add_callback (dma-fence.c:
 * 657-667). */
int32_t dma_fence_add_callback(struct dma_fence *f, struct dma_fence_cb *cb,
                               void (*func)(struct dma_fence *, struct dma_fence_cb *))
{
    uint32_t flags;
    if (dma_fence_is_signaled(f))
        return -ENOENT;
    flags = kirq_save();
    if (!enable_masked(f)) {
        kirq_restore(flags);
        return -ENOENT;
    }
    spin_lock(&f->lock);
    if (f->signaled) {
        spin_unlock(&f->lock);
        kirq_restore(flags);
        return -ENOENT;
    }
    cb->func = func;
    cb->next = f->cbs;
    f->cbs = cb;
    spin_unlock(&f->lock);
    kirq_restore(flags);
    return 0;
}

/** Detach a still-pending callback, returning whether it was found. One
 * that another processor runs is waited for, so that a 0 means it has run
 * to its end, as Linux's removal under the lock means; one this processor
 * runs, a callback that removes itself or another of its fence, is not. */
int dma_fence_remove_callback(struct dma_fence *f, struct dma_fence_cb *cb)
{
    struct dma_fence_cb **p;
    uint32_t flags;
    if (!f)
        return 0;
    flags = spin_lock_irqsave(&f->lock);
    for (p = &f->cbs; *p; p = &(*p)->next) {
        if (*p == cb) {
            *p = cb->next;
            cb->next = NULL;
            spin_unlock_irqrestore(&f->lock, flags);
            return 1;
        }
    }
    while (f->running == cb && f->running_cpu != this_cpu()) {
        spin_unlock(&f->lock);
        spin_lock(&f->lock);
    }
    spin_unlock_irqrestore(&f->lock, flags);
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

/** Enable the fence and hand it to its own wait or the default one, as
 * Linux's dma_fence_wait_timeout (dma-fence.c:512-518), with a reference
 * held across the wait. */
int32_t dma_fence_wait_until(struct dma_fence *f, int intr, int64_t deadline_ns)
{
    int32_t rc;
    if (dma_fence_is_signaled(f))
        return 0;
    if (deadline_ns >= 0 && dma_fence_now() >= deadline_ns)
        return -ETIME;
    dma_fence_get(f);
    dma_fence_enable_sw_signaling(f);
    if (f->signaled)
        rc = 0;
    else if (f->ops && f->ops->wait)
        rc = f->ops->wait(f, intr, deadline_ns);
    else
        rc = dma_fence_default_wait(f, intr, deadline_ns);
    dma_fence_put(f);
    return rc;
}

/** Sleep on the fence's queue with a reference held, restoring the
 * thread's previous timer. */
int32_t dma_fence_default_wait(struct dma_fence *f, int intr, int64_t deadline_ns)
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
    dma_fence_enable_sw_signaling(f);
    saved = current_proc->wakeup_tick;
    if (deadline_ns >= 0) {
        left = deadline_ns - dma_fence_now();
        delta = tick_div64((uint64_t)(left > 0 ? left : 0) + TICK_NS - 1u, TICK_NS, NULL);
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
    uint32_t count;
    volatile uint32_t pending; /* fences not yet signalled, counted once armed */
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

/** An array's driver name, Linux's dma-fence-array's. */
static const char *array_driver_name(struct dma_fence *f)
{
    (void)f;
    return "dma_fence_array";
}

/** An array's timeline name, Linux's dma-fence-array's. */
static const char *array_timeline_name(struct dma_fence *f)
{
    (void)f;
    return "unbound";
}

/** Carry a child's error and complete the array after its last child,
 * whose callbacks may run on several processors at once. */
static void array_done(struct dma_fence *f, struct dma_fence_cb *cb)
{
    struct fence_array *a = ((struct array_cb *)cb)->array;
    if (f->error && !a->base.error)
        dma_fence_set_error(&a->base, f->error);
    if (fence_ref_put(&a->pending))
        dma_fence_signal(&a->base);
}

/**
 * Add a callback to each fence of the array, the first time its signal is
 * asked for, as Linux's dma_fence_array_enable_signaling
 * (dma-fence-array.c:70-87): an array made and only queried, or dropped,
 * enables none of its fences. Returns 1; the last fence found signalled
 * signals the array.
 */
static int array_enable_signaling(struct dma_fence *f)
{
    struct fence_array *a = (struct fence_array *)f;
    uint32_t i;
    a->pending = a->count;
    for (i = 0; i < a->count; i++) {
        a->callbacks[i].array = a;
        if (dma_fence_add_callback(a->fences[i], &a->callbacks[i].cb, array_done))
            array_done(a->fences[i], &a->callbacks[i].cb);
    }
    return 1;
}

/** Whether every fence of an array not yet enabled has signalled, carrying
 * the first error, as Linux's dma_fence_array_signaled; an enabled array
 * is signalled by its callbacks. */
static int array_signaled(struct dma_fence *f)
{
    struct fence_array *a = (struct fence_array *)f;
    uint32_t i;
    if (f->listened)
        return 0;
    for (i = 0; i < a->count; i++)
        if (!dma_fence_is_signaled(a->fences[i]))
            return 0;
    for (i = 0; i < a->count && !f->error; i++)
        dma_fence_set_error(f, a->fences[i]->error);
    return 1;
}

static const struct dma_fence_ops array_ops = {array_driver_name,
                                               array_timeline_name,
                                               array_enable_signaling,
                                               array_release,
                                               array_signaled,
                                               NULL,
                                               0};

static const struct dma_fence_ops chain_ops;

/* Over every chain's `prev`, which a walk unlinks while others read it, as
 * Linux's cmpxchg and RCU keep it (dma-fence-chain.c:24-72). A reader takes
 * a reference to the point under it, a writer swaps the link under it and
 * puts what it unlinked after; no fence's lock is taken under it. */
static struct spinlock chain_lock = SPINLOCK_INIT_NAMED("dma_fence_chain");

/** `c`'s earlier point with a reference, or NULL. */
static struct dma_fence *chain_prev_get(struct dma_fence_chain *c)
{
    struct dma_fence *prev;
    uint32_t flags = spin_lock_irqsave(&chain_lock);
    prev = dma_fence_get(c->prev);
    spin_unlock_irqrestore(&chain_lock, flags);
    return prev;
}

/** Make `next` `c`'s earlier point if `old` still is: 1 when it did, and
 * the link's reference to `old` is the caller's to put. */
static int chain_prev_swap(struct dma_fence_chain *c, struct dma_fence *old, struct dma_fence *next)
{
    int swapped;
    uint32_t flags = spin_lock_irqsave(&chain_lock);
    swapped = c->prev == old;
    if (swapped)
        c->prev = next;
    spin_unlock_irqrestore(&chain_lock, flags);
    return swapped;
}

/** Return a fence's chain record, or NULL for any other fence. */
struct dma_fence_chain *to_dma_fence_chain(struct dma_fence *f)
{
    return f && f->ops == &chain_ops ? (struct dma_fence_chain *)f : NULL;
}

/** The fence a point of a walk stands for: a chain's own fence, or the
 * point itself when it is no chain. */
static struct dma_fence *unwrap_contained(struct dma_fence *node)
{
    struct dma_fence_chain *c = to_dma_fence_chain(node);
    return c ? c->fence : node;
}

/** Move the walk to the nearest earlier point whose fence has not
 * signalled, or to the end, giving back the reference to the point it
 * leaves. The signalled points passed stay in the chain: Linux's
 * dma_fence_unwrap_next steps with dma_fence_chain_walk, which unlinks
 * each by cmpxchg on `prev` and puts it (dma-fence-chain.c:50-72), as
 * Rach2's dma_fence_chain_walk does for the syncobjs. */
static void unwrap_step(struct dma_fence_unwrap *cursor)
{
    struct dma_fence_chain *c = to_dma_fence_chain(cursor->chain);
    struct dma_fence *prev = c ? chain_prev_get(c) : NULL, *next;
    while (prev && dma_fence_is_signaled(unwrap_contained(prev))) {
        c = to_dma_fence_chain(prev);
        next = c ? chain_prev_get(c) : NULL;
        dma_fence_put(prev);
        prev = next;
    }
    dma_fence_put(cursor->chain);
    cursor->chain = prev;
    cursor->array = NULL;
    cursor->index = 0;
}

/** The first leaf of the walk's point, an array's first fence or the
 * point's own, or NULL at the end. */
static struct dma_fence *unwrap_point(struct dma_fence_unwrap *cursor)
{
    struct dma_fence *f;
    while (cursor->chain) {
        f = unwrap_contained(cursor->chain);
        cursor->index = 0;
        if (f->ops != &array_ops) {
            cursor->array = NULL;
            return f;
        }
        cursor->array = f;
        if (((struct fence_array *)f)->count)
            return ((struct fence_array *)f)->fences[0];
        unwrap_step(cursor);
    }
    return NULL;
}

/** Begin a walk at `head`, as Linux's dma_fence_unwrap_first. */
struct dma_fence *dma_fence_unwrap_first(struct dma_fence *head, struct dma_fence_unwrap *cursor)
{
    cursor->chain = dma_fence_get(head);
    cursor->array = NULL;
    cursor->index = 0;
    return unwrap_point(cursor);
}

/** Go on with a walk, as Linux's dma_fence_unwrap_next. */
struct dma_fence *dma_fence_unwrap_next(struct dma_fence_unwrap *cursor)
{
    struct fence_array *a = (struct fence_array *)cursor->array;
    if (a && ++cursor->index < a->count)
        return a->fences[cursor->index];
    if (!cursor->chain)
        return NULL;
    unwrap_step(cursor);
    return unwrap_point(cursor);
}

/** Give back the point a walk left before its end holds. */
void dma_fence_unwrap_end(struct dma_fence_unwrap *cursor)
{
    dma_fence_put(cursor->chain);
    cursor->chain = NULL;
    cursor->array = NULL;
}

/** Whether `f` holds other fences: an array or a chain's point. */
int dma_fence_is_container(const struct dma_fence *f)
{
    return f && (f->ops == &array_ops || f->ops == &chain_ops);
}

/** Count the leaves that Linux's dma_fence_unwrap visits. */
uint32_t dma_fence_count(struct dma_fence *f)
{
    struct dma_fence_unwrap u;
    struct dma_fence *leaf;
    uint32_t n = 0;
    for (leaf = dma_fence_unwrap_first(f, &u); leaf; leaf = dma_fence_unwrap_next(&u))
        n++;
    return n;
}

/** Return a borrowed leaf in unwrap order. */
struct dma_fence *dma_fence_at(struct dma_fence *f, uint32_t index)
{
    struct dma_fence_unwrap u;
    struct dma_fence *leaf;
    for (leaf = dma_fence_unwrap_first(f, &u); leaf && index; leaf = dma_fence_unwrap_next(&u))
        index--;
    dma_fence_unwrap_end(&u);
    return leaf;
}

/** Build an all-of fence, flattening nested merges and keeping of each
 * context the leaf that `dma_fence_is_later` puts last. */
struct dma_fence *dma_fence_merge(struct dma_fence **fences, uint32_t count)
{
    struct dma_fence_unwrap u;
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
    /* One walk of each fence, since a leaf that signals between two
     * lookups by index would move the later ones; a walk finds no more
     * leaves than the count before it. */
    for (i = 0; i < count; i++)
        for (f = dma_fence_unwrap_first(fences[i], &u); f; f = dma_fence_unwrap_next(&u)) {
            for (k = 0; k < a->count; k++)
                if (a->fences[k]->context == f->context)
                    break;
            if (k < a->count) {
                if (!dma_fence_is_later(f, a->fences[k]))
                    continue;
                dma_fence_put(a->fences[k]);
            } else if (a->count < total)
                a->count++;
            else
                continue;
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
    return &a->base;
}

/** Merge the leaves still active of each fence as Linux's
 * dma_fence_unwrap_merge does, omitting work already finished and keeping
 * the newest completion time when nothing is left. */
struct dma_fence *dma_fence_unwrap_merge(struct dma_fence **fences, uint32_t count)
{
    struct dma_fence_unwrap u;
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
        for (f = dma_fence_unwrap_first(fences[i], &u); f; f = dma_fence_unwrap_next(&u)) {
            if (!dma_fence_is_signaled(f)) {
                if (n < total)
                    leaves[n++] = f;
            } else if (f->timestamp_ns > timestamp)
                timestamp = f->timestamp_ns;
        }
    out = dma_fence_merge(leaves, n);
    if (!n && out)
        out->timestamp_ns = timestamp;
    kfree(leaves);
    return out;
}

/** Read the earliest requested deadline of a fence under its lock, as
 * Linux's sw_sync_ioctl_get_deadline (drivers/dma-buf/sw_sync.c:452-458 of
 * 6.12.115), since the 64-bit value is two words on i686. */
int64_t dma_fence_deadline(struct dma_fence *f)
{
    uint32_t flags;
    int64_t deadline;

    if (!f)
        return 0x7fffffffffffffffll;
    flags = spin_lock_irqsave(&f->lock);
    deadline = f->deadline_ns;
    spin_unlock_irqrestore(&f->lock, flags);
    return deadline;
}

/**
 * Pass a deadline hint to all children without changing completion. The
 * fence keeps the earliest hint, compared and stored under its lock as
 * Linux's timeline_fence_set_deadline (drivers/dma-buf/sw_sync.c:195-209 of
 * 6.12.115), so that two waiters on different processors cannot lose the
 * earlier one. The lock is given back before the children are visited, as
 * Linux's array and chain hooks (dma-fence-array.c:152-160,
 * dma-fence-chain.c:210-218) visit them under none.
 */
void dma_fence_set_deadline(struct dma_fence *f, int64_t deadline)
{
    struct dma_fence_unwrap u;
    struct dma_fence *leaf;
    uint32_t flags;
    if (!f)
        return;
    flags = spin_lock_irqsave(&f->lock);
    if (deadline < f->deadline_ns)
        f->deadline_ns = deadline;
    spin_unlock_irqrestore(&f->lock, flags);
    if (f->ops == &array_ops || f->ops == &chain_ops)
        for (leaf = dma_fence_unwrap_first(f, &u); leaf; leaf = dma_fence_unwrap_next(&u))
            dma_fence_set_deadline(leaf, deadline);
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
 * callback armed, or 0 when every one has signalled. `armed` names the
 * fence, with a reference, before its callback is added, and nothing here
 * writes it after, so that the callback, which may run on another
 * processor at once, is its only writer from then on.
 */
static int chain_arm(struct dma_fence_chain *c)
{
    struct dma_fence_chain *p;
    struct dma_fence *node = dma_fence_get(&c->base), *f, *next;
    while (node) {
        p = to_dma_fence_chain(node);
        f = dma_fence_get(p ? p->fence : node);
        c->armed = f;
        if (!dma_fence_add_callback(f, &c->cb.cb, chain_done)) {
            dma_fence_put(node);
            return 1;
        }
        dma_fence_put(f);
        next = p ? chain_prev_get(p) : NULL;
        dma_fence_put(node);
        node = next;
    }
    c->armed = NULL;
    return 0;
}

/** Move to the next fence the chain waits on, or signal the chain and let
 * go of the reference its enabling took, as Linux's dma_fence_chain_cb and
 * dma_fence_chain_irq_work do (dma-fence-chain.c:122-143). */
static void chain_done(struct dma_fence *f, struct dma_fence_cb *cb)
{
    struct dma_fence_chain *c = ((struct dma_fence_chain_cb *)cb)->chain;
    /* The signal holds `f` while its callbacks run. */
    dma_fence_put(f);
    if (chain_arm(c))
        return;
    dma_fence_signal(&c->base);
    dma_fence_put(&c->base);
}

/** Start waiting on the first fence of the chain not yet signalled, with a
 * reference to the chain held while a callback is armed, as Linux's
 * dma_fence_chain_enable_signaling takes one (dma-fence-chain.c:
 * 145-161): so the chain is never released while its callback may run. */
static int chain_enable_signaling(struct dma_fence *f)
{
    dma_fence_get(f);
    if (chain_arm((struct dma_fence_chain *)f))
        return 1;
    dma_fence_put(f);
    return 0;
}

/** Whether the fence of the point and of each earlier one has signalled,
 * as Linux's dma_fence_chain_signaled, without arming the chain. */
static int chain_signaled(struct dma_fence *f)
{
    struct dma_fence_chain *p;
    struct dma_fence *node = dma_fence_get(f), *next;
    while (node) {
        p = to_dma_fence_chain(node);
        if (!dma_fence_is_signaled(p ? p->fence : node)) {
            dma_fence_put(node);
            return 0;
        }
        next = p ? chain_prev_get(p) : NULL;
        dma_fence_put(node);
        node = next;
    }
    return 1;
}

/** Release a point and, without recursion, each earlier point it held last. */
static void chain_release(struct dma_fence *f)
{
    struct dma_fence_chain *c = (struct dma_fence_chain *)f, *p;
    struct dma_fence *prev = c->prev, *next;
    /* No callback is armed: an armed one holds a reference. */
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

static const struct dma_fence_ops chain_ops = {chain_driver_name,
                                               chain_timeline_name,
                                               chain_enable_signaling,
                                               chain_release,
                                               chain_signaled,
                                               NULL,
                                               1};

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
    if (p && dma_fence_seqno_is_later(seqno, prev->seqno, prev->ops)) {
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
 * signalled, as Linux's dma_fence_chain_walk does (dma-fence-chain.c:
 * 39-72): each unlink is a swap of the link that another walk may have
 * made first. Returns the earlier point with a reference, or NULL, and
 * puts `f`. */
struct dma_fence *dma_fence_chain_walk(struct dma_fence *f)
{
    struct dma_fence_chain *c = to_dma_fence_chain(f), *p;
    struct dma_fence *prev, *next;
    if (!c) {
        dma_fence_put(f);
        return NULL;
    }
    while ((prev = chain_prev_get(c)) != NULL) {
        p = to_dma_fence_chain(prev);
        if (!dma_fence_is_signaled(p ? p->fence : prev))
            break;
        next = p ? chain_prev_get(p) : NULL;
        if (chain_prev_swap(c, prev, next))
            dma_fence_put(prev);
        else
            dma_fence_put(next);
        dma_fence_put(prev);
    }
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

static const struct dma_fence_ops stub_ops = {stub_name, stub_name, NULL, NULL, NULL, NULL, 0};

/* The shared stub, and the lock over its making, as Linux's
 * dma_fence_stub and dma_fence_stub_lock (drivers/dma-buf/dma-fence.c:27-28
 * of 6.12.115). */
static struct spinlock stub_lock = SPINLOCK_INIT_NAMED("dma_fence_stub");
static struct dma_fence stub;

/**
 * Return a reference to the shared signalled stub, made at first use, as
 * Linux's dma_fence_get_stub (drivers/dma-buf/dma-fence.c:131-148). The
 * stub is made and signalled under `stub_lock`, so that two first callers
 * on different processors make it once and neither takes a reference
 * before it is whole: a second dma_fence_init would clear the references
 * the first caller took, and a later put could then free the static
 * record. Any context that may take a spinlock; the stub's own lock is
 * taken under `stub_lock` once, at the signal.
 */
struct dma_fence *dma_fence_get_stub(void)
{
    uint32_t flags = spin_lock_irqsave(&stub_lock);

    if (!stub.ops) {
        dma_fence_init(&stub, &stub_ops, dma_fence_context_alloc(1), 0);
        dma_fence_signal(&stub);
    }
    spin_unlock_irqrestore(&stub_lock, flags);
    return dma_fence_get(&stub);
}
