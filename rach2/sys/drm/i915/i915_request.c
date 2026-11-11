/*
 * i915_request.c: the Intel graphics driver's requests on the GPU's
 * rings: their commands, the context switch, the breadcrumb that
 * closes each, their fences signalled from the user interrupt, waits and
 * retirement, and the default context image, after Linux 6.12's
 * i915_request.c, gt/intel_ring_submission.c, gt/gen6_engine_cs.c,
 * gt/intel_gt_requests.c and gt/intel_gt.c on Haswell.
 */

#include "i915.h"
#include "i915_reg.h"

#include "../../dmabuf/dmabuf.h"
#include "../../klog.h"
#include "../../lib/kerrno.h"
#include "../../lib/kirq.h"
#include "../../lib/kstring.h"
#include "../../mm/kheap.h"
#include "../../proc/percpu.h"
#include "../../timer.h"
#include "../../rtc.h"
#ifndef RACH2_HOSTTEST
#include "../../arch/arch.h"
#include "../../proc/process.h"
#include "../../proc/sched.h"
#endif

/* The dwords of the commands a request takes: the render ring's flush
 * with its invalidation, Linux's gen7_emit_flush_rcs with gen7_stall_cs,
 * and without it; the other rings', mi_flush_dw; the PPGTT's directory
 * loaded, load_pd_dir; the context switch, mi_set_context on Haswell with
 * the I915_NUM_ENGINES - 1 other engines; and the breadcrumbs,
 * gen7_emit_breadcrumb_rcs and _xcs, the others' with GEN7_XCS_WA
 * stores. */
#define I915_FLUSH_RCS_DW      8u
#define I915_WRITE_FLUSH_DW    4u
#define I915_FLUSH_XCS_DW      4u
#define I915_LOAD_PD_DW        12u
#define I915_SIGNALLERS        (I915_NUM_ENGINES - 1u)
#define I915_SET_CONTEXT_DW    (4u + 2u + 4u * I915_SIGNALLERS + 6u)
#define I915_BREADCRUMB_RCS_DW 6u
#define I915_XCS_WA            32u
#define I915_BREADCRUMB_XCS_DW (3u + 3u * I915_XCS_WA + 3u + 2u)

/* Where in the GT's scratch page the context switch's delay writes and the
 * render flush's post-sync write goes, Linux's
 * INTEL_GT_SCRATCH_FIELD_DEFAULT and _RENDER_FLUSH. */
#define I915_SCRATCH_DEFAULT      0u
#define I915_SCRATCH_RENDER_FLUSH 128u

/* How long the default state's recording may take, Linux's
 * I915_GEM_IDLE_TIMEOUT (HZ / 5), and how often a waiter reads the
 * breadcrumb itself, as Linux's retire worker runs, once a second. */
#define I915_RECORD_TIMEOUT_NS 200000000ll
#define I915_WAIT_SLICE_NS     1000000000ll

/* The room a request is given in its ring before it begins, which holds
 * the most the driver emits for one with its breadcrumb and a wrap, as
 * Linux's intel_ring_begin waits for the requests ahead to free it. */
#define I915_REQUEST_ROOM 2048u

/** Whether `rq`'s breadcrumb has been written or its fence signalled. */
int i915_request_completed(const struct i915_request *rq)
{
    return rq->fence.base.signaled || i915_engine_passed(rq->engine, rq->seqno);
}

int i915_request_started(const struct i915_request *rq)
{
    return rq->fence.base.signaled || i915_engine_passed(rq->engine, rq->seqno - 1u);
}

/* ------------------------------------------------------------------------
 * Submission
 */

/* The lock of every request's `awaits` and `added` and every engine's
 * `kick`, innermost of all, since a fence may signal in any interrupt and
 * with an engine's lock held. */
static struct spinlock submit_lock = SPINLOCK_INIT;

/* The devices whose requests may wait for fences, by their GT's
 * `submit_next`; and whether an engine has a request to give the chip,
 * which the worker waits for. */
static struct drm_i915_private *submit_devices;
static volatile int submit_kicked;
static wait_queue_t submit_wait;

/** Whether a fence's error keeps a request that waits for it from running,
 * as Linux's fatal_error: any but -EAGAIN, an innocent request's in a
 * reset, and -ETIMEDOUT. */
static int fatal_error(int32_t err)
{
    return err != 0 && err != -EAGAIN && err != -ETIMEDOUT;
}

/**
 * One thing `rq` waited for is done, with the error `err` it brings: the
 * last one done once the request is on its engine's list marks the engine
 * for the worker. Any context, an interrupt among them.
 */
static void await_done(struct i915_request *rq, int32_t err)
{
    uint32_t flags = spin_lock_irqsave(&submit_lock);
    int kick = 0;

    if (fatal_error(err) && !rq->fence.base.error) {
        dma_fence_set_error(&rq->fence.base, err);
    }
    if (--rq->awaits == 0 && rq->added) {
        rq->engine->kick = 1;
        submit_kicked = 1;
        kick = 1;
    }
    spin_unlock_irqrestore(&submit_lock, flags);
    if (kick) {
        wq_wake_all(&submit_wait);
    }
}

/* How long a request waits for another driver's fence before it runs
 * without it, its fence's error -ETIMEDOUT, as Linux's
 * CONFIG_DRM_I915_FENCE_TIMEOUT. */
#define I915_FENCE_TIMEOUT_MS 10000u

/* The earliest tick at which a request's wait for another driver's fence
 * ends, or 0 for none; the worker wakes for it. */
static uint64_t await_next;

/** Fold the deadlines of `rq`'s waits for other drivers' fences into
 * `await_next`. */
static void await_deadlines(const struct i915_request *rq)
{
    const struct i915_await *w;

    for (w = rq->await_list; w; w = w->next) {
        if (w->deadline && (await_next == 0 || w->deadline < await_next)) {
            await_next = w->deadline;
        }
    }
}

/**
 * End each of `rq`'s waits for another driver's fence whose deadline `now`
 * has passed, as Linux's timer_i915_sw_fence_wake: the callback taken off,
 * the request's error -ETIMEDOUT, which still lets it run, and the wait
 * counted done.
 */
static void request_expire(struct i915_request *rq, uint64_t now)
{
    struct i915_await *w;
    uint32_t flags;
    int removed;

    for (w = rq->await_list; w; w = w->next) {
        if (!w->deadline || w->deadline > now) {
            continue;
        }
        w->deadline = 0;
        flags = kirq_save();
        removed = dma_fence_remove_callback(w->fence, &w->cb);
        kirq_restore(flags);
        if (removed) {
            i915_request_set_error_once(rq, -ETIMEDOUT);
            await_done(rq, -ETIMEDOUT);
        }
    }
}

/** End the waits for other drivers' fences of every queued request whose
 * deadline `now` has passed, then set `await_next` from the waits left.
 * Under the big kernel lock. */
static void awaits_expire(uint64_t now)
{
    struct drm_i915_private *i915;
    struct i915_request *rq;
    int i;

    await_next = 0;
    for (i915 = submit_devices; i915; i915 = i915->gt.submit_next) {
        for (i = 0; i < I915_NUM_ENGINES; i++) {
            for (rq = i915->gt.engine[i].queued; rq; rq = rq->next) {
                request_expire(rq, now);
                await_deadlines(rq);
            }
        }
    }
}

/** A fence a request waited for signalled. In the signaller's context. */
static void await_fence_done(struct dma_fence *fence, struct dma_fence_cb *cb)
{
    await_done(((struct i915_await *)cb)->rq, fence->error);
}

/** Fill [from, to) of `e`'s ring with MI_NOOP, across its end if need be. */
static void ring_noop(struct i915_engine *e, uint32_t from, uint32_t to)
{
    while (from != to) {
        e->ring[from / 4u] = MI_NOOP;
        from = (from + 4u) & (e->ring_size - 1u);
    }
}

void i915_request_skip(struct i915_request *rq)
{
    ring_noop(rq->engine, rq->infix, rq->postfix);
    rq->infix = rq->postfix;
}

void i915_request_set_error_once(struct i915_request *rq, int32_t err)
{
    if (!rq->fence.base.signaled && !fatal_error(rq->fence.base.error)) {
        rq->fence.base.error = err;
    }
}

/**
 * Give the chip the requests of `e` that wait for nothing more, oldest
 * first, up to the first that still waits, as Linux's ring submission
 * moves RING_TAIL: a request of a banned context fails with -EIO, as
 * Linux's __i915_request_submit fails one whose context may not run, a
 * request whose fence holds an error keeps only its breadcrumb, and the
 * requests that waited to see one given to the chip are told. Under the
 * big kernel lock.
 */
static void engine_submit(struct drm_i915_private *i915, struct i915_engine *e)
{
    struct i915_request *first, *last = 0, *rq;
    struct i915_await *w, *next;
    uint32_t flags = spin_lock_irqsave(&submit_lock);

    first = e->queued;
    while ((rq = e->queued) != 0 && rq->awaits == 0) {
        rq->submitted = 1;
        last = rq;
        e->queued = rq->next;
    }
    spin_unlock_irqrestore(&submit_lock, flags);
    if (!last) {
        return;
    }
    for (rq = first;; rq = rq->next) {
        if (rq->ce && rq->ce->banned) {
            i915_request_set_error_once(rq, -EIO);
        }
        if (fatal_error(rq->fence.base.error)) {
            i915_request_skip(rq);
        }
        for (w = rq->exec_waiters; w; w = next) {
            next = w->exec_next;
            w->exec_next = 0;
            w->src = 0;
            await_done(w->rq, 0);
        }
        rq->exec_waiters = 0;
        if (rq == last) {
            break;
        }
    }
    /* The commands reach memory before the chip reads the tail. */
    arch_dma_fence();
    i915_uncore_write(i915, RING_TAIL(e->mmio_base), last->tail);
    e->hw_tail = last->tail;
}

int i915_submit_work(void)
{
    struct drm_i915_private *i915;
    uint32_t flags;
    int i, kicked, n = 0;

    submit_kicked = 0;
    if (await_next != 0 && rtc_ticks_now() >= await_next) {
        awaits_expire(rtc_ticks_now());
    }
    for (i915 = submit_devices; i915; i915 = i915->gt.submit_next) {
        for (i = 0; i < I915_NUM_ENGINES; i++) {
            struct i915_engine *e = &i915->gt.engine[i];

            flags = spin_lock_irqsave(&submit_lock);
            kicked = e->kick;
            e->kick = 0;
            spin_unlock_irqrestore(&submit_lock, flags);
            if (kicked) {
                engine_submit(i915, e);
                n++;
            }
        }
    }
    return n;
}

/* Whether a GT unparked, or a wait for another driver's fence began, since
 * the worker last looked, so that it sleeps no longer than its first
 * heartbeat or the wait's deadline. */
static volatile int hangcheck_kicked;

#ifndef RACH2_HOSTTEST
/** The earliest heartbeat of the devices on the worker's list whose GT is
 * awake, as a tick, or 0 for none. */
static uint64_t hangcheck_deadline(void)
{
    struct drm_i915_private *i915;
    uint64_t at = 0;

    for (i915 = submit_devices; i915; i915 = i915->gt.submit_next) {
        if (i915->gt.awake && !i915->gt.wedged && (at == 0 || i915->gt.hc_next < at)) {
            at = i915->gt.hc_next;
        }
    }
    if (await_next != 0 && (at == 0 || await_next < at)) {
        at = await_next;
    }
    return at;
}

static process_t *submit_thread;

/* The tick the worker's sleep ends at for the next heartbeat, 0 for none. */
static uint64_t submit_deadline;

/** Whether an engine has a request whose last fence signalled, a GT has
 * unparked, or a heartbeat is due. */
static int submit_ready(void *arg)
{
    (void)arg;
    return submit_kicked || hangcheck_kicked ||
           (submit_deadline != 0 && rtc_ticks_now() >= submit_deadline);
}

/**
 * The submission worker, ki915: sleep in an idle wait until a fence a
 * request waited for signals or the next heartbeat of an unparked GT is
 * due, then give the chip what is ready and beat, holding the big lock as
 * kernel code does. The heartbeat is the driver's hang detection, as
 * Linux's heartbeat worker is (i915_hangcheck.c).
 */
static void submit_thread_main(void)
{
    struct drm_i915_private *i915;

    arch_irq_enable();
    for (;;) {
        submit_deadline = hangcheck_deadline();
        current_proc->wakeup_tick = submit_deadline;
        (void)wq_wait_event_idle(&submit_wait, submit_ready, 0);
        current_proc->wakeup_tick = 0;
        hangcheck_kicked = 0;
        (void)i915_submit_work();
        for (i915 = submit_devices; i915; i915 = i915->gt.submit_next) {
            (void)i915_hangcheck(i915);
        }
    }
}

/** Start the worker the first time a request waits for a fence: 0, or
 * -ENOMEM. */
static int32_t submit_worker_start(void)
{
    process_t *p;
    uintptr_t base;
    uint32_t flags;

    if (submit_thread) {
        return 0;
    }
    flags = kirq_save();
    base = kstack_alloc();
    p = base ? alloc_process() : 0;
    if (!p) {
        if (base) {
            kstack_free(base + KERNEL_STACK_SIZE);
        }
        kirq_restore(flags);
        klog(KLOG_ERR, "i915", "worker: refused: no record or stack for it");
        return -ENOMEM;
    }
    p->kernel_stack = base + KERNEL_STACK_SIZE;
    p->space_root = 0;
    p->page_dir = 0;
    p->parent_pid = 0;
    proc_set_comm(p, "ki915");
    setup_process_stack(p, submit_thread_main);
    submit_thread = p;
    sched_make_ready(p);
    kirq_restore(flags);
    klog(KLOG_DEBUG, "i915", "worker: ki915 is pid %d", (int)p->pid);
    return 0;
}

/** Whether the worker runs. */
static int submit_worker_started(void)
{
    return submit_thread != 0;
}
#else
/** The host tests run the worker's work themselves (`i915_submit_work`). */
static int32_t submit_worker_start(void)
{
    return 0;
}

/** The host tests' worker is always there. */
static int submit_worker_started(void)
{
    return 1;
}
#endif

/** Put the device on the worker's list, once. */
static void submit_device_add(struct drm_i915_private *i915)
{
    if (!i915->gt.submit_listed) {
        i915->gt.submit_listed = 1;
        i915->gt.submit_next = submit_devices;
        submit_devices = i915;
    }
}

/** Take the device off the worker's list. */
static void submit_device_remove(struct drm_i915_private *i915)
{
    struct drm_i915_private **link;

    for (link = &submit_devices; *link; link = &(*link)->gt.submit_next) {
        if (*link == i915) {
            *link = i915->gt.submit_next;
            break;
        }
    }
    i915->gt.submit_listed = 0;
    i915->gt.submit_next = 0;
}

/** A new record of what `rq` waits for, on its list and counted; NULL
 * without memory. */
static struct i915_await *await_new(struct i915_request *rq)
{
    struct i915_await *w = (struct i915_await *)kzalloc(sizeof(*w));
    uint32_t flags;

    if (!w) {
        return 0;
    }
    w->rq = rq;
    w->next = rq->await_list;
    rq->await_list = w;
    flags = spin_lock_irqsave(&submit_lock);
    rq->awaits++;
    spin_unlock_irqrestore(&submit_lock, flags);
    return w;
}

/** Wait for `src`, a request of the same device, to be given to the chip,
 * as Linux's __i915_request_await_execution. 0 or -ENOMEM. */
static int32_t await_submit(struct i915_request *rq, struct i915_request *src)
{
    struct i915_await *w;

    if (src->engine == rq->engine || src->submitted) {
        return 0;
    }
    w = await_new(rq);
    if (!w) {
        return -ENOMEM;
    }
    w->src = src;
    w->exec_next = src->exec_waiters;
    src->exec_waiters = w;
    return 0;
}

int32_t i915_request_await_fence(struct i915_request *rq, struct dma_fence *fence, int exec)
{
    struct i915_await *w;
    struct dma_fence *leaf;
    uint32_t i, n, flags;
    int32_t err;

    if (dma_fence_is_signaled(fence)) {
        return 0;
    }
    /* `i915_request_create` started the worker, whose start may sleep; the
     * leaves stay put only while nothing does. */
    if (!submit_worker_started()) {
        return -ENOMEM;
    }
    submit_device_add(rq->i915);
    n = dma_fence_count(fence);
    for (i = 0; i < n; i++) {
        leaf = dma_fence_at(fence, i);
        if (!leaf || dma_fence_is_signaled(leaf) || leaf->context == rq->engine->fence_context) {
            continue;
        }
        if (exec && leaf->ops == &i915_fence_ops &&
            ((struct i915_request *)leaf)->i915 == rq->i915) {
            err = await_submit(rq, (struct i915_request *)leaf);
            if (err < 0) {
                return err;
            }
            continue;
        }
        w = await_new(rq);
        if (!w) {
            return -ENOMEM;
        }
        w->fence = dma_fence_get(leaf);
        flags = kirq_save();
        err = dma_fence_add_callback(leaf, &w->cb, await_fence_done);
        kirq_restore(flags);
        if (err != 0) {
            /* It signalled meanwhile, and is not waited for, as Linux
             * passes over a fence already signalled. */
            flags = spin_lock_irqsave(&submit_lock);
            rq->awaits--;
            spin_unlock_irqrestore(&submit_lock, flags);
        } else if (leaf->ops != &i915_fence_ops) {
            /* Another driver's fence is waited for 10 s at most, as
             * Linux's i915_request_await_external; the worker wakes for
             * it. */
            w->deadline = rtc_ticks_now() + MS_TO_TICKS(I915_FENCE_TIMEOUT_MS);
            await_deadlines(rq);
            hangcheck_kicked = 1;
            wq_wake_all(&submit_wait);
        }
    }
    return 0;
}

/**
 * Take back what `rq` still waits for, which then never comes: the fences'
 * callbacks removed and the requests it waited to see submitted told to
 * forget it; then free every record. For a request that leaves without
 * running, and for one that retired, whose records are all spent.
 */
static void awaits_release(struct i915_request *rq)
{
    struct i915_await *w, **link;
    uint32_t flags;

    while ((w = rq->await_list) != 0) {
        rq->await_list = w->next;
        if (w->fence) {
            flags = kirq_save();
            (void)dma_fence_remove_callback(w->fence, &w->cb);
            kirq_restore(flags);
            dma_fence_put(w->fence);
        } else if (w->src) {
            for (link = &w->src->exec_waiters; *link; link = &(*link)->exec_next) {
                if (*link == w) {
                    *link = w->exec_next;
                    break;
                }
            }
        }
        kfree(w);
    }
}

/** Tell the requests that wait to see `rq` submitted that it never will
 * be: they forget it, and may then run. */
static void exec_waiters_release(struct i915_request *rq)
{
    struct i915_await *w;

    while ((w = rq->exec_waiters) != 0) {
        rq->exec_waiters = w->exec_next;
        w->exec_next = 0;
        w->src = 0;
        await_done(w->rq, 0);
    }
}

/* ------------------------------------------------------------------------
 * Signalling and retirement
 */

/**
 * Signal the fence of each request of `e` whose breadcrumb is written,
 * oldest first, the ring running them in order. The engine's
 * `irq_notify`, from the user interrupt, and from any context besides.
 */
static void i915_request_signal(struct drm_i915_private *i915, struct i915_engine *e)
{
    struct i915_request *rq;
    uint32_t flags;

    (void)i915;
    flags = spin_lock_irqsave(&e->lock);
    for (rq = e->requests; rq && i915_engine_passed(e, rq->seqno); rq = rq->next) {
        if (!rq->fence.base.signaled) {
            (void)dma_fence_signal(&rq->fence.base);
        }
    }
    spin_unlock_irqrestore(&e->lock, flags);
}

/** Let go of what the request holds for its batch, which may free its
 * objects; in process context. */
static void request_put_hold(struct i915_request *rq)
{
    struct i915_request_hold *hold = rq->hold;

    rq->hold = 0;
    if (hold) {
        hold->release(hold);
    }
}

/** Give up what a request held once the chip is past it: what it waited
 * for, its batch's objects, the state it switched from, its own, and the
 * engine list's reference. */
static void request_release(struct i915_request *rq)
{
    request_put_hold(rq);
    awaits_release(rq);
    exec_waiters_release(rq);
    intel_context_put(rq->prev_ce);
    rq->prev_ce = 0;
    intel_context_put(rq->ce);
    rq->ce = 0;
    dma_fence_put(&rq->fence.base);
}

/** Park the GT, which is awake, and count the ticks it was awake, as
 * Linux's __gt_park adds to its awake time. */
static void gt_park(struct drm_i915_private *i915)
{
    struct i915_gt *gt = &i915->gt;

    gt->awake = 0;
    gt->awake_ticks += rtc_ticks_now() - gt->unpark_ticks;
    i915_gt_park(i915);
}

/**
 * Take from `e`'s list each request that `done` says is finished, oldest
 * first, its fence signalled, its waitboost let go, and release it; the
 * user interrupt is let go once the list is empty.  Each finished request
 * moves the ring's head to its end, as Linux's i915_request_retire, so
 * that the room it took is free again; a cancel (`all`) leaves the head to
 * the ring's restart.
 */
static void engine_retire(struct drm_i915_private *i915, struct i915_engine *e, int all)
{
    struct i915_request *done = 0, **tail = &done, *rq;
    uint32_t flags;

    flags = spin_lock_irqsave(&e->lock);
    if (all) {
        e->queued = 0;
    }
    while ((rq = e->requests) != 0 && (all || i915_engine_passed(e, rq->seqno))) {
        e->requests = rq->next;
        rq->next = 0;
        if (!all) {
            e->head = rq->postfix;
        }
        *tail = rq;
        tail = &rq->next;
        if (!rq->fence.base.signaled) {
            if (all) {
                dma_fence_set_error(&rq->fence.base, -EIO);
            }
            (void)dma_fence_signal(&rq->fence.base);
        }
    }
    if (!e->requests) {
        e->requests_tail = 0;
    }
    spin_unlock_irqrestore(&e->lock, flags);
    if (done && !e->requests) {
        i915_engine_irq_put(i915, e);
    }
    while ((rq = done) != 0) {
        done = rq->next;
        rq->next = 0;
        i915_rps_retire(rq);
        request_release(rq);
    }
}

void i915_retire_requests(struct drm_i915_private *i915)
{
    struct i915_gt *gt = &i915->gt;
    int i, busy = 0;

    for (i = 0; i < I915_NUM_ENGINES; i++) {
        engine_retire(i915, &gt->engine[i], 0);
        busy |= gt->engine[i].requests != 0;
    }
    if (gt->awake && !busy) {
        gt_park(i915);
    }
}

/* ------------------------------------------------------------------------
 * Commands
 */

uint32_t *i915_request_begin(struct i915_request *rq, uint32_t ndw)
{
    return i915_ring_begin(rq->engine, rq->i915, ndw);
}

/** Invalidate the caches and TLBs before a request's commands: on the
 * render ring a stall, then a PIPE_CONTROL with a post-sync write to the
 * scratch page; on the others MI_FLUSH_DW with the TLBs, and on the video
 * ring its own caches too, as Linux's gen6_emit_flush_vcs. 0 or -EIO. */
static int32_t emit_invalidate(struct i915_request *rq)
{
    struct i915_gt *gt = &rq->i915->gt;
    uint32_t *cs;

    if (rq->engine != &gt->engine[I915_RCS0]) {
        cs = i915_request_begin(rq, I915_FLUSH_XCS_DW);
        if (!cs) {
            return -EIO;
        }
        cs[0] = MI_FLUSH_DW | MI_FLUSH_DW_STORE_INDEX | MI_FLUSH_DW_OP_STOREDW | MI_INVALIDATE_TLB;
        if (rq->engine == &gt->engine[I915_VCS0]) {
            cs[0] |= MI_INVALIDATE_BSD;
        }
        cs[1] = HWS_SCRATCH_ADDR | MI_FLUSH_DW_USE_GTT;
        cs[2] = 0;
        cs[3] = MI_NOOP;
        return 0;
    }
    cs = i915_request_begin(rq, I915_FLUSH_RCS_DW);
    if (!cs) {
        return -EIO;
    }
    /* A CS stall before the PIPE_CONTROL that invalidates the state
     * cache, as the workaround asks. */
    cs[0] = GFX_OP_PIPE_CONTROL(4);
    cs[1] = PIPE_CONTROL_CS_STALL | PIPE_CONTROL_STALL_AT_SCOREBOARD;
    cs[2] = 0;
    cs[3] = 0;
    cs[4] = GFX_OP_PIPE_CONTROL(4);
    cs[5] = PIPE_CONTROL_CS_STALL | PIPE_CONTROL_QW_WRITE | PIPE_CONTROL_GLOBAL_GTT_IVB |
            PIPE_CONTROL_TLB_INVALIDATE | PIPE_CONTROL_INSTRUCTION_CACHE_INVALIDATE |
            PIPE_CONTROL_TEXTURE_CACHE_INVALIDATE | PIPE_CONTROL_VF_CACHE_INVALIDATE |
            PIPE_CONTROL_CONST_CACHE_INVALIDATE | PIPE_CONTROL_STATE_CACHE_INVALIDATE |
            PIPE_CONTROL_MEDIA_STATE_CLEAR;
    cs[6] = gt->scratch_ggtt + I915_SCRATCH_RENDER_FLUSH;
    cs[7] = 0;
    return 0;
}

/** Flush the caches' writes, as Linux's emit_flush with EMIT_FLUSH: on the
 * render ring a PIPE_CONTROL with its post-sync write to the scratch page,
 * on the others MI_FLUSH_DW without the TLBs. 0 or -EIO. */
static int32_t emit_write_flush(struct i915_request *rq)
{
    struct i915_gt *gt = &rq->i915->gt;
    uint32_t *cs = i915_request_begin(rq, I915_WRITE_FLUSH_DW);

    if (!cs) {
        return -EIO;
    }
    if (rq->engine != &gt->engine[I915_RCS0]) {
        cs[0] = MI_FLUSH_DW | MI_FLUSH_DW_STORE_INDEX | MI_FLUSH_DW_OP_STOREDW;
        cs[1] = HWS_SCRATCH_ADDR | MI_FLUSH_DW_USE_GTT;
        cs[2] = 0;
        cs[3] = MI_NOOP;
        return 0;
    }
    cs[0] = GFX_OP_PIPE_CONTROL(4);
    cs[1] = PIPE_CONTROL_CS_STALL | PIPE_CONTROL_QW_WRITE | PIPE_CONTROL_GLOBAL_GTT_IVB |
            PIPE_CONTROL_RENDER_TARGET_CACHE_FLUSH | PIPE_CONTROL_DEPTH_CACHE_FLUSH |
            PIPE_CONTROL_DC_FLUSH_ENABLE | PIPE_CONTROL_FLUSH_ENABLE;
    cs[2] = gt->scratch_ggtt + I915_SCRATCH_RENDER_FLUSH;
    cs[3] = 0;
    return 0;
}

/**
 * Load the aliasing PPGTT's directory into the request's ring, as Linux's
 * switch_mm does before every request on a ring of graphics version 7:
 * the writes flushed; PP_DIR_DCLV and PP_DIR_BASE loaded, PP_DIR_BASE
 * stored to the scratch page as a stall, and the TLBs invalidated, then
 * flushed again (load_pd_dir); and the caches invalidated. So a table made
 * since the ring last loaded the directory is seen. Nothing before the
 * PPGTT is made. 0 or -EIO.
 */
static int32_t request_switch_mm(struct i915_request *rq)
{
    struct i915_gt *gt = &rq->i915->gt;
    uint32_t base = rq->engine->mmio_base, *cs;
    int32_t err;

    if (!gt->ppgtt.ready) {
        return 0;
    }
    err = emit_write_flush(rq);
    if (err < 0) {
        return err;
    }
    cs = i915_request_begin(rq, I915_LOAD_PD_DW);
    if (!cs) {
        return -EIO;
    }
    cs[0] = MI_LOAD_REGISTER_IMM(1);
    cs[1] = RING_PP_DIR_DCLV(base);
    cs[2] = PP_DIR_DCLV_2G;
    cs[3] = MI_LOAD_REGISTER_IMM(1);
    cs[4] = RING_PP_DIR_BASE(base);
    cs[5] = gt->ppgtt.pp_dir;
    cs[6] = MI_STORE_REGISTER_MEM | MI_SRM_LRM_GLOBAL_GTT;
    cs[7] = RING_PP_DIR_BASE(base);
    cs[8] = gt->scratch_ggtt + I915_SCRATCH_DEFAULT;
    cs[9] = MI_LOAD_REGISTER_IMM(1);
    cs[10] = RING_INSTPM(base);
    cs[11] = I915_MASKED_ENABLE(INSTPM_TLB_INVALIDATE);
    err = emit_write_flush(rq);
    if (err < 0) {
        return err;
    }
    return emit_invalidate(rq);
}

/**
 * Load the render ring's state `ce` with MI_SET_CONTEXT, as Linux's
 * mi_set_context on Haswell: arbitration off and every other engine's
 * sleep messages held around it, the image saved and restored with its
 * extended state, or its restore inhibited while it holds nothing, then a
 * read of the last engine's register written to the scratch page as a
 * delay before the next switch. 0 or -EIO.
 */
static int32_t emit_set_context(struct i915_request *rq, struct intel_context *ce, uint32_t flags)
{
    struct i915_gt *gt = &rq->i915->gt;
    uint32_t *cs = i915_request_begin(rq, I915_SET_CONTEXT_DW), psmi = 0, n = 0;
    int i;

    if (!cs) {
        return -EIO;
    }
    cs[n++] = MI_ARB_ON_OFF | MI_ARB_DISABLE;
    cs[n++] = MI_LOAD_REGISTER_IMM(I915_SIGNALLERS);
    for (i = 0; i < I915_NUM_ENGINES; i++) {
        if (&gt->engine[i] != rq->engine) {
            cs[n++] = RING_PSMI_CTL(gt->engine[i].mmio_base);
            cs[n++] = I915_MASKED_ENABLE(GEN6_PSMI_SLEEP_MSG_DISABLE);
        }
    }
    cs[n++] = MI_NOOP;
    cs[n++] = MI_SET_CONTEXT;
    cs[n++] = ce->image_ggtt | flags;
    cs[n++] = MI_NOOP; /* MI_SET_CONTEXT is followed by MI_NOOP, as Linux */
    cs[n++] = MI_LOAD_REGISTER_IMM(I915_SIGNALLERS);
    for (i = 0; i < I915_NUM_ENGINES; i++) {
        if (&gt->engine[i] != rq->engine) {
            psmi = RING_PSMI_CTL(gt->engine[i].mmio_base);
            cs[n++] = psmi;
            cs[n++] = I915_MASKED_DISABLE(GEN6_PSMI_SLEEP_MSG_DISABLE);
        }
    }
    cs[n++] = MI_STORE_REGISTER_MEM | MI_SRM_LRM_GLOBAL_GTT;
    cs[n++] = psmi;
    cs[n++] = gt->scratch_ggtt + I915_SCRATCH_DEFAULT;
    cs[n++] = MI_NOOP;
    cs[n] = MI_ARB_ON_OFF | MI_ARB_ENABLE;
    return 0;
}

/**
 * Switch the chip to the request's state, as Linux's switch_context: the
 * PPGTT's directory loaded on either ring; on the render ring the image
 * loaded, valid from then on, and the engine's `last_ce` made it, the
 * request keeping the one it replaces until it retires. The other rings
 * have no state to switch. 0 or -EIO.
 */
static int32_t request_switch(struct i915_request *rq)
{
    struct i915_engine *e = rq->engine;
    struct intel_context *ce = rq->ce;
    uint32_t flags = MI_SAVE_EXT_STATE_EN | MI_MM_SPACE_GTT;
    int32_t err;

    err = request_switch_mm(rq);
    if (err < 0 || !ce->image) {
        return err;
    }
    flags |= ce->image_valid ? MI_RESTORE_EXT_STATE_EN : MI_RESTORE_INHIBIT;
    err = emit_set_context(rq, ce, flags);
    if (err < 0) {
        return err;
    }
    rq->set_context = 1;
    rq->was_valid = ce->image_valid;
    ce->image_valid = 1;
    if (e->last_ce != ce) {
        rq->switched = 1;
        rq->prev_ce = e->last_ce;
        e->last_ce = intel_context_get(ce);
    }
    return 0;
}

/** Take back a request's commands, which never reached the chip, and the
 * switch they made, as if it had not begun. */
static void request_rewind(struct i915_request *rq)
{
    struct i915_engine *e = rq->engine;

    e->tail = rq->head;
    e->reserved = 0;
    if (rq->switched) {
        intel_context_put(e->last_ce);
        e->last_ce = rq->prev_ce;
        rq->prev_ce = 0;
        rq->switched = 0;
    }
    if (rq->set_context) {
        rq->ce->image_valid = rq->was_valid;
        rq->set_context = 0;
    }
}

/** The bytes of `e`'s breadcrumb. */
static uint32_t breadcrumb_bytes(struct drm_i915_private *i915, struct i915_engine *e)
{
    return 4u *
           (e == &i915->gt.engine[I915_RCS0] ? I915_BREADCRUMB_RCS_DW : I915_BREADCRUMB_XCS_DW);
}

/* ------------------------------------------------------------------------
 * Requests
 */

/**
 * A request on `ce` without the GT's checks, for `i915_request_create`
 * and the default state's recording: its image made, the GT unparked, the
 * fence on the engine's timeline, the invalidation and the switch emitted
 * and room kept for the breadcrumb.
 */
static int32_t request_new(struct intel_context *ce, struct i915_request **out)
{
    struct drm_i915_private *i915 = ce->i915;
    struct i915_engine *e = ce->engine;
    struct i915_request *rq;
    int32_t err;

    err = intel_context_pin_image(ce);
    if (err < 0) {
        return err;
    }
    rq = (struct i915_request *)kzalloc(sizeof(*rq));
    if (!rq) {
        return -ENOMEM;
    }
    if (!i915->gt.awake) {
        i915->gt.awake = 1;
        i915->gt.unpark_ticks = rtc_ticks_now();
        i915_gt_unpark(i915);
        /* The worker beats while the GT is awake. */
        i915_hangcheck_unpark(i915);
        submit_device_add(i915);
        hangcheck_kicked = 1;
        wq_wake_all(&submit_wait);
    }
    if (!e->fence_context) {
        e->fence_context = dma_fence_context_alloc(1);
    }
    e->seqno = e->seqno == 0xffffffffu ? 1u : e->seqno + 1u; /* 0 is never a seqno */
    rq->seqno = e->seqno;
    dma_fence_init(&rq->fence.base, &i915_fence_ops, e->fence_context, rq->seqno);
    rq->fence.uabi_class = i915_engine_uabi_class(e);
    rq->i915 = i915;
    rq->engine = e;
    rq->ce = intel_context_get(ce);
    rq->awaits = 1; /* its own, until it is added */
    rq->head = e->tail;
    e->reserved = breadcrumb_bytes(i915, e);
    err = emit_invalidate(rq);
    if (err == 0) {
        err = request_switch(rq);
    }
    rq->infix = e->tail;
    if (err < 0) {
        request_rewind(rq);
        dma_fence_set_error(&rq->fence.base, err);
        (void)dma_fence_signal(&rq->fence.base);
        intel_context_put(rq->ce);
        rq->ce = 0;
        dma_fence_put(&rq->fence.base);
        return err;
    }
    *out = rq;
    return 0;
}

int32_t i915_request_add(struct i915_request *rq)
{
    struct drm_i915_private *i915 = rq->i915;
    struct i915_engine *e = rq->engine;
    uint32_t *cs, flags, i, n;

    e->reserved = 0;
    rq->postfix = e->tail;
    rq->emitted = rtc_ticks_now();
    n = breadcrumb_bytes(i915, e) / 4u;
    cs = i915_ring_begin(e, i915, n);
    if (!cs) {
        request_put_hold(rq);
        awaits_release(rq);
        request_rewind(rq);
        dma_fence_set_error(&rq->fence.base, -EIO);
        (void)dma_fence_signal(&rq->fence.base);
        intel_context_put(rq->prev_ce);
        rq->prev_ce = 0;
        intel_context_put(rq->ce);
        rq->ce = 0;
        return -EIO;
    }
    if (e == &i915->gt.engine[I915_RCS0]) {
        cs[0] = GFX_OP_PIPE_CONTROL(4);
        cs[1] = PIPE_CONTROL_RENDER_TARGET_CACHE_FLUSH | PIPE_CONTROL_DEPTH_CACHE_FLUSH |
                PIPE_CONTROL_DC_FLUSH_ENABLE | PIPE_CONTROL_FLUSH_ENABLE | PIPE_CONTROL_QW_WRITE |
                PIPE_CONTROL_GLOBAL_GTT_IVB | PIPE_CONTROL_CS_STALL;
        cs[2] = e->hws_ggtt + I915_GEM_HWS_SEQNO_ADDR;
        cs[3] = rq->seqno;
        cs[4] = MI_USER_INTERRUPT;
        cs[5] = MI_NOOP;
    } else {
        cs[0] = MI_FLUSH_DW | MI_INVALIDATE_TLB | MI_FLUSH_DW_OP_STOREDW | MI_FLUSH_DW_STORE_INDEX;
        cs[1] = I915_GEM_HWS_SEQNO_ADDR | MI_FLUSH_DW_USE_GTT;
        cs[2] = rq->seqno;
        /* The stores again, so that the write has landed before the
         * interrupt, as Linux's GEN7_XCS_WA. */
        for (i = 0; i < I915_XCS_WA; i++) {
            cs[3 + 3 * i] = MI_STORE_DWORD_INDEX;
            cs[4 + 3 * i] = I915_GEM_HWS_SEQNO_ADDR;
            cs[5 + 3 * i] = rq->seqno;
        }
        cs[n - 5] = MI_FLUSH_DW;
        cs[n - 4] = 0;
        cs[n - 3] = 0;
        cs[n - 2] = MI_USER_INTERRUPT;
        cs[n - 1] = MI_NOOP;
    }
    rq->tail = e->tail;
    rq->added_ns = dma_fence_now();
    /* The interrupt is unmasked before the chip can reach the breadcrumb,
     * and stays so while the engine has requests. */
    if (!e->requests) {
        i915_engine_irq_get(i915, e);
    }
    (void)dma_fence_get(&rq->fence.base);
    flags = spin_lock_irqsave(&e->lock);
    if (e->requests_tail) {
        e->requests_tail->next = rq;
    } else {
        e->requests = rq;
    }
    e->requests_tail = rq;
    spin_unlock_irqrestore(&e->lock, flags);
    /* On the list, it waits only for its fences; the chip takes it once
     * they and every request before it are done. */
    flags = spin_lock_irqsave(&submit_lock);
    if (!e->queued) {
        e->queued = rq;
    }
    rq->added = 1;
    rq->awaits--;
    spin_unlock_irqrestore(&submit_lock, flags);
    await_deadlines(rq);
    engine_submit(i915, e);
    return 0;
}

void i915_request_put(struct i915_request *rq)
{
    if (rq) {
        dma_fence_put(&rq->fence.base);
    }
}

/** Poll the request's breadcrumb for `us` microseconds at most, in steps
 * that double up to a millisecond. */
static void request_poll(struct i915_request *rq, uint64_t us)
{
    uint64_t waited = 0;
    uint32_t step = 1;

    while (!i915_request_completed(rq) && waited < us) {
        if (step > us - waited) {
            step = (uint32_t)(us - waited);
        }
        i915_delay_us(&rq->i915->hw, step);
        waited += step;
        step = step >= 500u ? 1000u : step * 2u;
    }
}

int32_t i915_request_wait(struct i915_request *rq, int intr, int64_t timeout_ns)
{
    struct drm_i915_private *i915 = rq->i915;
    struct i915_engine *e = rq->engine;
    int64_t deadline, slice, now;
    int32_t err;

    if (!i915_request_completed(rq) && timeout_ns == 0) {
        return -ETIME;
    }
    deadline = timeout_ns < 0 ? -1 : dma_fence_now() + timeout_ns;
    while (!i915_request_completed(rq)) {
        now = dma_fence_now();
        if (deadline >= 0 && now >= deadline) {
            i915_request_signal(i915, e);
            if (i915_request_completed(rq)) {
                break;
            }
            return -ETIME;
        }
        slice = now + I915_WAIT_SLICE_NS;
        if (deadline >= 0 && deadline < slice) {
            slice = deadline;
        }
        if (current_proc && i915->msi_vector >= 0) {
            err = dma_fence_wait_until(&rq->fence.base, intr, slice);
            if (err == -EINTR) {
                return err;
            }
        } else {
            /* Without the interrupt the breadcrumb is polled. */
            request_poll(rq, tick_div((uint64_t)(slice - now), 1000u, (uint32_t *)0) + 1u);
        }
        /* An interrupt may have been lost: the page is read again; and a
         * heartbeat may be due, which resets a hung engine. */
        i915_request_signal(i915, e);
        (void)i915_hangcheck(i915);
    }
    i915_retire_requests(i915);
    return 0;
}

/* ------------------------------------------------------------------------
 * The GT's own objects and the default state
 */

/**
 * Record the render ring's default state, as Linux's
 * __engines_record_defaults: a fresh context loaded with its restore
 * inhibited takes Linux's context workaround for graphics version 7
 * (intel_workarounds.c's gen7_ctx_workarounds_init: INSTPM's forced
 * ordering) and, loaded again, two of the render engine's that the ring's
 * start wrote to the registers (CACHE_MODE_0_GEN7's RC op flush off,
 * CACHE_MODE_1's pixel subspan collect optimization off), the switch to
 * the GT's own context saves its image, and the image is kept as the
 * state every new context starts from.
 * Linux also runs the null render state batch before the switch, which
 * this driver does not make. 0, -ENOMEM, or -EIO when the chip did not
 * finish in time.
 */
static int32_t record_defaults(struct drm_i915_private *i915)
{
    struct i915_engine *e = &i915->gt.engine[I915_RCS0];
    struct i915_request *rq = 0, *flush = 0;
    struct intel_context *ce = intel_context_create(i915, e, 0);
    uint32_t *cs;
    int32_t err;

    if (!ce) {
        return -ENOMEM;
    }
    err = request_new(ce, &rq);
    if (err == 0) {
        cs = i915_request_begin(rq, 8);
        if (cs) {
            cs[0] = MI_LOAD_REGISTER_IMM(3);
            cs[1] = INSTPM;
            cs[2] = I915_MASKED_ENABLE(INSTPM_FORCE_ORDERING);
            cs[3] = CACHE_MODE_0_GEN7;
            cs[4] = I915_MASKED_DISABLE(RC_OP_FLUSH_ENABLE);
            cs[5] = CACHE_MODE_1;
            cs[6] = I915_MASKED_ENABLE(PIXEL_SUBSPAN_COLLECT_OPT_DISABLE);
            cs[7] = MI_NOOP;
        }
        err = i915_request_add(rq);
        if (err == 0 && !cs) {
            err = -EIO;
        }
    }
    if (err == 0) {
        err = request_new(e->kernel_ce, &flush);
    }
    if (err == 0) {
        err = i915_request_add(flush);
    }
    if (err == 0) {
        err = i915_request_wait(flush, 0, I915_RECORD_TIMEOUT_NS) < 0 ? -EIO : 0;
    }
    if (err == 0) {
        i915->gt.default_state =
            (uint32_t *)kmalloc((size_t)I915_HSW_CXT_PAGES * I915_GTT_PAGE_SIZE);
        if (!i915->gt.default_state) {
            err = -ENOMEM;
        } else {
            k_memcpy(i915->gt.default_state, ce->image,
                     (size_t)I915_HSW_CXT_PAGES * I915_GTT_PAGE_SIZE);
        }
    }
    i915_request_put(flush);
    i915_request_put(rq);
    intel_context_put(ce);
    i915_retire_requests(i915);
    return err;
}

/**
 * Make what the requests need with the first, as Linux makes at load:
 * the scratch page, each engine's own state, the user interrupt's hook and
 * the default state. A recording that fails wedges the GPU, as Linux's
 * intel_gt_set_wedged_on_init. 0, -ENOMEM, or -EIO.
 */
static int32_t gt_contexts_init(struct drm_i915_private *i915)
{
    struct i915_gt *gt = &i915->gt;
    int32_t err;
    int i;

    if (!gt->scratch) {
        gt->scratch = (uint32_t *)i915_gt_pages_map(i915, 1, I915_CACHE_LLC, &gt->scratch_ggtt);
        if (!gt->scratch) {
            return -ENOMEM;
        }
    }
    for (i = 0; i < I915_NUM_ENGINES; i++) {
        struct i915_engine *e = &gt->engine[i];

        if (!e->kernel_ce) {
            e->kernel_ce = intel_context_create(i915, e, 0);
            if (!e->kernel_ce) {
                return -ENOMEM;
            }
        }
        e->irq_notify = i915_request_signal;
    }
    gt->ctx_ready = 1;
    err = record_defaults(i915);
    if (err < 0) {
        gt->wedged = 1;
        gt->wedged_terminal = 1;
        klog(KLOG_ERR, "i915", "contexts: the default state was not recorded: error %d; wedged",
             (int)err);
        return -EIO;
    }
    return 0;
}

/** The free bytes of a ring of `size` between `tail` and `head`, the gap
 * kept out, as Linux's __intel_ring_space. */
static uint32_t ring_space(uint32_t head, uint32_t tail, uint32_t size)
{
    return (head - tail - I915_RING_GAP) & (size - 1u);
}

/**
 * Wait until `e`'s ring has I915_REQUEST_ROOM bytes free, as Linux's
 * wait_for_space: the oldest request whose end leaves the room once the
 * chip is past it is waited for, interruptibly, and the requests retired,
 * which moves the head to the end of each, as Linux's
 * i915_request_retire_upto. A request held back by its fences is waited
 * for with them. 0 or -EINTR.
 */
static int32_t ring_wait_for_space(struct i915_engine *e)
{
    struct i915_request *rq;
    int32_t err;

    /* The wait sleeps, and other callers may take the room meanwhile, so
     * the room is looked at again after each, as Linux's wait_for_space
     * loops. */
    while (ring_space(e->head, e->tail, e->ring_size) < I915_REQUEST_ROOM) {
        for (rq = e->requests; rq; rq = rq->next) {
            if (ring_space(rq->postfix, e->tail, e->ring_size) >= I915_REQUEST_ROOM) {
                break;
            }
        }
        if (!rq) {
            return 0;
        }
        (void)dma_fence_get(&rq->fence.base);
        err = i915_request_wait(rq, 1, -1);
        i915_request_put(rq);
        if (err != 0) {
            return err;
        }
    }
    return 0;
}

struct i915_request *i915_request_throttle_target(struct intel_context *ce, int64_t before)
{
    struct i915_engine *e = ce->engine;
    struct i915_request *rq, *target = 0;
    uint32_t flags;

    flags = spin_lock_irqsave(&e->lock);
    for (rq = e->requests; rq; rq = rq->next) {
        if (rq->ce == ce && rq->added_ns <= before && !i915_request_completed(rq)) {
            target = rq;
        }
    }
    if (target) {
        (void)dma_fence_get(&target->fence.base);
    }
    spin_unlock_irqrestore(&e->lock, flags);
    return target;
}

int32_t i915_gt_wait_for_idle(struct drm_i915_private *i915, int intr)
{
    struct i915_request *rq;
    int32_t err;
    int i;

    for (;;) {
        i915_retire_requests(i915);
        rq = 0;
        for (i = 0; i < I915_NUM_ENGINES && !rq; i++) {
            rq = i915->gt.engine[i].requests_tail;
        }
        if (!rq) {
            return 0;
        }
        (void)dma_fence_get(&rq->fence.base);
        err = i915_request_wait(rq, intr, -1);
        i915_request_put(rq);
        if (err < 0) {
            return err;
        }
    }
}

int32_t i915_request_create(struct intel_context *ce, struct i915_request **out)
{
    struct drm_i915_private *i915 = ce->i915;
    int32_t err;

    if (i915->gt.wedged || !ce->engine->running) {
        return -EIO;
    }
    /* What may sleep comes first: from the request's start to its add the
     * ring is its alone. */
    err = submit_worker_start();
    if (err < 0) {
        return err;
    }
    i915_retire_requests(i915);
    err = ring_wait_for_space(ce->engine);
    if (err < 0) {
        return err;
    }
    if (i915->gt.wedged) {
        return -EIO;
    }
    if (!i915->gt.ctx_ready) {
        err = gt_contexts_init(i915);
        if (err < 0) {
            return err;
        }
    }
    return request_new(ce, out);
}

void i915_requests_unplug(struct drm_i915_private *i915)
{
    struct i915_gt *gt = &i915->gt;
    int i;

    gt->wedged = 1;
    gt->wedged_terminal = 1;
    submit_device_remove(i915);
    for (i = 0; i < I915_NUM_ENGINES; i++) {
        struct i915_engine *e = &gt->engine[i];

        engine_retire(i915, e, 1);
        e->irq_notify = 0;
        intel_context_put(e->last_ce);
        e->last_ce = 0;
        intel_context_put(e->kernel_ce);
        e->kernel_ce = 0;
    }
    if (gt->awake) {
        gt_park(i915);
    }
    i915_gt_pages_unmap(i915, gt->scratch, 1, gt->scratch_ggtt);
    gt->scratch = 0;
    kfree(gt->default_state);
    gt->default_state = 0;
    gt->ctx_ready = 0;
    i915_gpu_error_clear(i915);
}

/**
 * Take back the commands and switches of `e`'s requests that the chip was
 * never given, newest first, so that each puts back what the one before it
 * left, and the ring's tail to the end of what the chip was given: the
 * reset that ends a wedge starts the ring with nothing stale after it, and
 * `last_ce` is again the state the chip holds. Under the big kernel lock,
 * with the GPU wedged, so that nothing is submitted meanwhile.
 */
static void engine_cancel_queued(struct i915_engine *e)
{
    struct i915_request *rq, *last;

    do {
        last = 0;
        for (rq = e->queued; rq; rq = rq->next) {
            if (rq->switched || rq->set_context) {
                last = rq;
            }
        }
        if (last) {
            request_rewind(last);
        }
    } while (last);
    e->tail = e->hw_tail;
    e->reserved = 0;
}

void i915_requests_cancel(struct drm_i915_private *i915)
{
    struct i915_gt *gt = &i915->gt;
    int i;

    for (i = 0; i < I915_NUM_ENGINES; i++) {
        gt->engine[i].hc_armed = 0;
        engine_cancel_queued(&gt->engine[i]);
        engine_retire(i915, &gt->engine[i], 1);
    }
    if (gt->awake) {
        gt_park(i915);
    }
}
