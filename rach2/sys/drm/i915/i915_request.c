/*
 * i915_request.c: the Intel graphics driver's requests on the GPU's
 * rings: their commands, the context switch, the breadcrumb that
 * closes each, their fences, which the user interrupt signals, and the
 * fences' ops, waits and retirement, and the default context image,
 * after Linux 6.12's i915_request.c, gt/intel_ring_submission.c,
 * gt/gen6_engine_cs.c, gt/intel_gt_requests.c and gt/intel_gt.c on
 * Haswell.
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
#include "../../proc/smp.h"
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

/* How long a wait polls a request the chip runs before it asks for the
 * signal, Linux's CONFIG_DRM_I915_MAX_REQUEST_BUSYWAIT, 8000 ns. */
#define I915_BUSYWAIT_US 8u

/* The second the retire worker's pass is rounded to while the GT is
 * awake, as Linux's retire_work_handler queues itself
 * round_jiffies_up_relative(HZ) on (`retire_deadline`). */
#define I915_RETIRE_INTERVAL_MS 1000u

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
static struct spinlock submit_lock = SPINLOCK_INIT_NAMED("submit_lock");

/* The devices whose requests may wait for fences, by their GT's
 * `submit_next`; and whether an engine has a request to give the chip,
 * which the worker waits for. */
static struct drm_i915_private *submit_devices;
static volatile int submit_kicked;
static wait_queue_t submit_wait;

/* Whether an engine's last request signalled to a listener since the
 * worker last looked, so that it retires the engine's requests at once, as
 * Linux's signal_irq_work queues the engine's retire_work. It is set and
 * taken under `submit_lock`, so that the worker's read and clear are one
 * step, as Linux's engine_retire takes its queue with xchg. */
static volatile int retire_kicked;

static void retire_work(void);

/**
 * The tick of the retire worker's next pass from the tick `now`: the first
 * whole second after a second from now, 1 to 2 s on, as Linux's
 * round_jiffies_up_relative(HZ) rounds it up on the first processor
 * (gt/intel_gt_requests.c:210-211 and 227-228; kernel/time/timer.c:
 * 349-377, whose rounding up adds a second even to a whole one).
 */
static uint64_t retire_deadline(uint64_t now)
{
    uint32_t sec = MS_TO_TICKS(I915_RETIRE_INTERVAL_MS), rem;
    uint64_t at = now + sec;

    (void)tick_div(at, sec, &rem);
    return at - rem + sec;
}

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

/* The earliest tick at which a request's wait for another driver's fence
 * ends, or 0 for none; the worker wakes for it. Under `submit_lock`. */
static uint64_t await_next;

/** Publish the next timeout or retry tick, `at`, if it is earlier. */
static void await_schedule(uint64_t at)
{
    uint32_t flags = spin_lock_irqsave(&submit_lock);

    if (at && (!await_next || at < await_next)) {
        await_next = at;
    }
    spin_unlock_irqrestore(&submit_lock, flags);
}

/** Read the timeout tick under its lock, also on a 32-bit processor. */
static uint64_t await_deadline(void)
{
    uint32_t flags = spin_lock_irqsave(&submit_lock);
    uint64_t at = await_next;

    spin_unlock_irqrestore(&submit_lock, flags);
    return at;
}

/** Fold the deadlines of `rq`'s waits for other drivers' fences into
 * `await_next`. The caller holds its GEM lock. */
static void await_deadlines(const struct i915_request *rq)
{
    const struct i915_await *w;

    for (w = rq->await_list; w; w = w->next) {
        await_schedule(w->deadline);
    }
}

/**
 * End each of `rq`'s waits for another driver's fence whose deadline `now`
 * has passed, as Linux's timer_i915_sw_fence_wake: the callback taken off,
 * the request's error -ETIMEDOUT, which still lets it run, and the wait
 * counted done. The caller holds the GEM lock until the walk ends, since
 * making a request runnable lets a submitter run and retire it.
 */
static void request_expire(struct i915_request *rq, uint64_t now)
{
    struct i915_await *w;
    uint32_t flags;
    int removed;

    lockdep_assert_held(&rq->i915->gem.lock);
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
 * Under the big kernel lock, which keeps the device list. Each device's
 * GEM lock protects its queue and wait records against caller retirement;
 * a held lock leaves that device for a retry on the next tick. */
static void awaits_expire(uint64_t now)
{
    struct drm_i915_private *i915;
    struct i915_request *rq;
    uint32_t flags;
    int i, taken;

    flags = spin_lock_irqsave(&submit_lock);
    await_next = 0;
    spin_unlock_irqrestore(&submit_lock, flags);
    for (i915 = submit_devices; i915; i915 = i915->gt.submit_next) {
        if (!i915_gem_trylock(i915, &taken)) {
            await_schedule(now + 1u);
            continue;
        }
        for (i = 0; i < I915_NUM_ENGINES; i++) {
            for (rq = i915->gt.engine[i].queued; rq; rq = rq->next) {
                request_expire(rq, now);
                await_deadlines(rq);
            }
        }
        i915_gem_unlock(i915, taken);
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
 * engine's lock, held throughout, so that the worker and an add on
 * another processor give the chip their requests one after the other and
 * RING_TAIL never moves back, as Linux's i915_request_submit holds the
 * engine's sched_engine lock (i915_request.c:604-612, 697-707).
 */
static void engine_submit(struct drm_i915_private *i915, struct i915_engine *e)
{
    struct i915_request *first, *last = 0, *rq;
    struct i915_await *w, *next;
    uint32_t elock = spin_lock_irqsave(&e->lock);
    uint32_t flags = spin_lock_irqsave(&submit_lock);

    first = e->queued;
    while ((rq = e->queued) != 0 && rq->awaits == 0) {
        rq->submitted = 1;
        last = rq;
        e->queued = rq->next;
    }
    spin_unlock_irqrestore(&submit_lock, flags);
    if (!last) {
        spin_unlock_irqrestore(&e->lock, elock);
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
    spin_unlock_irqrestore(&e->lock, elock);
}

int i915_submit_work(void)
{
    struct drm_i915_private *i915;
    uint64_t at = await_deadline();
    uint32_t flags;
    int i, kicked, n = 0;

    submit_kicked = 0;
    if (at != 0 && rtc_ticks_now() >= at) {
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
    retire_work();
    return n;
}

/* Whether a GT unparked, or a wait for another driver's fence began, since
 * the worker last looked, so that it sleeps no longer than its first
 * heartbeat or the wait's deadline. */
static volatile int hangcheck_kicked;

#ifndef RACH2_HOSTTEST
/** The earliest heartbeat or retire pass of the devices on the worker's
 * list whose GT is awake, as a tick, or 0 for none. A wedged GT has no
 * heartbeat, since i915_hangcheck returns before it moves `hc_next`, and a
 * deadline already past would keep the worker from sleeping. A GT whose
 * hangcheck is off keeps its next beat, which stops the heartbeats and
 * sets `hc_next` to 0 until the GT or an engine next unparks, as Linux's
 * heartbeat with enable_hangcheck off queues no other
 * (drivers/gpu/drm/i915/gt/intel_engine_heartbeat.c:234 in 6.12.115). */
static uint64_t hangcheck_deadline(void)
{
    struct drm_i915_private *i915;
    uint64_t at = 0, await = await_deadline();

    for (i915 = submit_devices; i915; i915 = i915->gt.submit_next) {
        if (i915->gt.awake && !i915->gt.wedged && i915->gt.hc_next != 0 &&
            (at == 0 || i915->gt.hc_next < at)) {
            at = i915->gt.hc_next;
        }
        if (i915->gt.awake && (at == 0 || i915->gt.retire_next < at)) {
            at = i915->gt.retire_next;
        }
    }
    if (await != 0 && (at == 0 || await < at)) {
        at = await;
    }
    return at;
}

static process_t *submit_thread;

/* The tick the worker's sleep ends at for the next heartbeat, 0 for none. */
static uint64_t submit_deadline;

/** Whether an engine has a request whose last fence signalled, an
 * engine's last request signalled to a listener, a GT has unparked, or a
 * heartbeat or retire pass is due. */
static int submit_ready(void *arg)
{
    (void)arg;
    return submit_kicked || retire_kicked || hangcheck_kicked ||
           (submit_deadline != 0 && rtc_ticks_now() >= submit_deadline);
}

/**
 * The submission worker, ki915: sleep in an idle wait until a fence a request
 * waited for signals or the next heartbeat of an unparked GT is due, then give
 * the chip what is ready and beat, holding the big lock as kernel code does,
 * but for its retire passes (`retire_work`). The heartbeat is the driver's
 * hang detection, as Linux's heartbeat worker is (i915_hangcheck.c). Like
 * every kernel thread, it is not switched by the tick (`no_tick_switch`,
 * which `setup_process_stack` sets), so it gives way only where it sleeps and
 * between passes, as a kworker of Linux without full preemption gives way
 * only between works (process_one_work's cond_resched,
 * kernel/workqueue.c:3298-3299 in 6.12.115).
 */
static void submit_thread_main(void)
{
    struct drm_i915_private *i915;

    arch_irq_enable();
    for (;;) {
        sched_service_point();
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
static int32_t submit_worker_spawn(void);

/* The process list is the big lock's, which EXECBUFFER2, under the GEM lock
 * alone, takes for the spawn. */
static int32_t submit_worker_start(void)
{
    int32_t rc;
    int big;

    if (submit_thread) {
        return 0;
    }
    big = !bkl_held();
    if (big) {
        bkl_enter();
    }
    rc = submit_worker_spawn();
    if (big) {
        bkl_exit();
    }
    return rc;
}

/** Make and start the worker unless it runs: 0, or -ENOMEM. */
static int32_t submit_worker_spawn(void)
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
    struct dma_fence_unwrap u;
    struct i915_await *w;
    struct dma_fence *leaf;
    uint32_t flags;
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
    /* One walk, as Linux's dma_fence_unwrap_for_each: a lookup by index
     * would pass over a leaf when an earlier one signalled between two. */
    for (leaf = dma_fence_unwrap_first(fence, &u); leaf; leaf = dma_fence_unwrap_next(&u)) {
        if (dma_fence_is_signaled(leaf) || leaf->context == rq->engine->fence_context) {
            continue;
        }
        if (exec && leaf->ops == &i915_fence_ops &&
            ((struct i915_request *)leaf)->i915 == rq->i915) {
            err = await_submit(rq, (struct i915_request *)leaf);
            if (err < 0) {
                dma_fence_unwrap_end(&u);
                return err;
            }
            continue;
        }
        w = await_new(rq);
        if (!w) {
            dma_fence_unwrap_end(&u);
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
 * running, and for one that retired, whose records are all spent. The
 * caller holds the GEM lock, also held by the timeout walk.
 */
static void awaits_release(struct i915_request *rq)
{
    struct i915_await *w, **link;
    uint32_t flags;

    lockdep_assert_held(&rq->i915->gem.lock);
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

/** The driver's name, for a sync file's information. */
static const char *i915_fence_driver(struct dma_fence *fence)
{
    (void)fence;
    return "i915";
}

/** The timeline's name: the engine's class, as programs number it. */
static const char *i915_fence_timeline(struct dma_fence *fence)
{
    static const char *const names[] = {"rcs", "bcs", "vcs", "vecs"};
    uint16_t c = ((struct i915_fence *)fence)->uabi_class;

    return c < 4 ? names[c] : "unknown";
}

/** Whether the request's breadcrumb is written, as Linux's
 * i915_fence_signaled (i915_request.c:88-91): a query that finds it so
 * signals the fence, outside the rule of `i915_request_signal`, whose
 * next pass still hands the engine to retirement for a request whose
 * signal was asked for before. Any context. */
static int i915_fence_signaled(struct dma_fence *fence)
{
    return i915_request_completed((struct i915_request *)fence);
}

/**
 * Ask for the request's signal, as Linux's i915_fence_enable_signaling
 * and insert_breadcrumb (i915_request.c:93-96, gt/intel_breadcrumbs.c:
 * 362-364): 0, which signals the fence at once, for a request whose
 * breadcrumb is written, which then does not count as listened for and
 * leaves the engine held, as Linux's irq_signal_request leaves
 * I915_FENCE_FLAG_SIGNAL clear; otherwise 1, the request marked
 * `signaling` for the interrupt. Called with interrupts masked, as every
 * `enable_signaling` is, and takes no lock, since a fence's callback may
 * ask for another request's signal with the engine's lock held; every
 * kernel path runs under the big lock.
 */
static int i915_fence_enable_signaling(struct dma_fence *fence)
{
    struct i915_request *rq = (struct i915_request *)fence;

    if (i915_request_completed(rq)) {
        return 0;
    }
    rq->signaling = 1;
    return 1;
}

/**
 * Wait for a request's fence until the CLOCK_MONOTONIC `deadline_ns`, or
 * for ever when it is negative, through `i915_request_wait` with
 * I915_WAIT_PRIORITY, as Linux's i915_fence_wait (i915_request.c:98-105),
 * so that every wait on the driver's fence spins first and boosts a
 * request the chip has not begun. 0, -ETIME or -EINTR; sleeps.
 */
static int32_t i915_fence_wait(struct dma_fence *fence, int intr, int64_t deadline_ns)
{
    int64_t timeout = -1, now;

    if (deadline_ns >= 0) {
        now = dma_fence_now();
        timeout = deadline_ns > now ? deadline_ns - now : 0;
    }
    return i915_request_wait((struct i915_request *)fence,
                             (intr ? I915_WAIT_INTERRUPTIBLE : 0u) | I915_WAIT_PRIORITY, timeout);
}

/* Linux's i915_fence_ops (i915_request.c:173-180); the fence is the
 * request's first member, and its last reference frees the request. */
const struct dma_fence_ops i915_fence_ops = {i915_fence_driver,
                                             i915_fence_timeline,
                                             i915_fence_enable_signaling,
                                             0,
                                             i915_fence_signaled,
                                             i915_fence_wait,
                                             0};

/**
 * Signal `rq`, which is on `e`'s list, complete, and not yet past the
 * signal's rule, with `e`'s lock held: when its signal was asked for
 * before it completed (`signaling`) and it is the engine's last, with
 * none being built (`building`), Linux would retire every request of the
 * engine now, so `unretired` is cleared. Returns 1 then, for the worker
 * to be told. As Linux's signal_irq_work, which for a request with
 * I915_FENCE_FLAG_SIGNAL hands the timeline whose last request it is to
 * the engine's retire_work, whether or not a query signalled its fence
 * first (gt/intel_breadcrumbs.c:217-238; intel_timeline_is_last, which a
 * request being built, already on the timeline, makes false).
 */
static int request_signal_locked(struct i915_engine *e, struct i915_request *rq)
{
    int kick = 0;

    if (rq->signaling) {
        rq->signaling = 0;
        if (rq == e->requests_tail && !e->building) {
            e->unretired = 0;
            kick = 1;
        }
    }
    if (!rq->fence.base.signaled) {
        (void)dma_fence_signal(&rq->fence.base);
    }
    return kick;
}

/** Tell the retire worker that an engine's last request signalled to a
 * listener, so that it retires at once. */
static void retire_kick(void)
{
    uint32_t flags = spin_lock_irqsave(&submit_lock);

    retire_kicked = 1;
    spin_unlock_irqrestore(&submit_lock, flags);
    wq_wake_all(&submit_wait);
}

/**
 * Signal the fence of each request of `e` whose breadcrumb is written,
 * oldest first, the ring running them in order, under the rule of
 * `request_signal_locked`. The engine's `irq_notify`, from the user
 * interrupt, and from any context besides.
 */
static void i915_request_signal(struct drm_i915_private *i915, struct i915_engine *e)
{
    struct i915_request *rq;
    uint32_t flags;
    int kick = 0;

    (void)i915;
    flags = spin_lock_irqsave(&e->lock);
    for (rq = e->requests; rq && i915_engine_passed(e, rq->seqno); rq = rq->next) {
        kick |= request_signal_locked(e, rq);
    }
    spin_unlock_irqrestore(&e->lock, flags);
    if (kick) {
        retire_kick();
    }
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
 * Linux's __gt_park adds to its awake time. Each engine's user interrupt,
 * armed since its first request of the GT's wake, is let go, as Linux's
 * engine park disarms its breadcrumbs (gt/intel_engine_pm.c:280,
 * gt/intel_breadcrumbs.c:317-324, 266-272). The pool's idle shadows go,
 * since no retire pass runs to age them while the GT sleeps, and the
 * wakeref the unpark took is put. */
static void gt_park(struct drm_i915_private *i915)
{
    struct i915_gt *gt = &i915->gt;
    int i;

    gt->awake = 0;
    gt->awake_ticks += rtc_ticks_now() - gt->unpark_ticks;
    for (i = 0; i < I915_NUM_ENGINES; i++) {
        if (gt->engine[i].irq_armed) {
            gt->engine[i].irq_armed = 0;
            i915_engine_irq_put(i915, &gt->engine[i]);
        }
    }
    i915_gt_park(i915);
    i915_gem_pool_trim(i915, I915_POOL_IDLE);
    i915_rpm_put(i915);
}

/**
 * Take from `e`'s list each request whose breadcrumb is written, or every
 * one for a cancel (`all`), oldest first, its fence signalled, its
 * waitboost let go, and release it; with `end` set, only those before it,
 * and the fences of the rest are left to the interrupt. The user interrupt
 * stays armed until the GT parks. Each finished request moves the ring's
 * head to its end, as Linux's i915_request_retire, so that the room it
 * took is free again;
 * a cancel leaves the head to the ring's restart. Every request taken off
 * is signalled under `request_signal_locked`'s rule, a breadcrumb the chip
 * wrote after `i915_request_signal`'s pass included, so that a listened
 * last request lets the engine go however it is retired, as Linux's
 * i915_request_retire drops the request's hold on its context, and with
 * it the engine's on the GT, in every path (i915_request.c:409,
 * gt/intel_context.h:225-233).
 */
static void engine_retire(struct drm_i915_private *i915, struct i915_engine *e, int all,
                          const struct i915_request *end)
{
    struct i915_request *done = 0, **tail = &done, *rq;
    uint32_t flags;

    if (!all && !end) {
        i915_request_signal(i915, e);
    }
    flags = spin_lock_irqsave(&e->lock);
    if (all) {
        e->queued = 0;
        e->unretired = 0;
    }
    while ((rq = e->requests) != 0 && rq != end && (all || i915_engine_passed(e, rq->seqno))) {
        e->requests = rq->next;
        rq->next = 0;
        if (!all) {
            e->head = rq->postfix;
        }
        *tail = rq;
        tail = &rq->next;
        if (all) {
            rq->signaling = 0;
            if (!rq->fence.base.signaled) {
                dma_fence_set_error(&rq->fence.base, -EIO);
                (void)dma_fence_signal(&rq->fence.base);
            }
        } else {
            /* The caller's pass parks the GT itself: no kick. */
            (void)request_signal_locked(e, rq);
        }
    }
    if (!e->requests) {
        e->requests_tail = 0;
    }
    spin_unlock_irqrestore(&e->lock, flags);
    while ((rq = done) != 0) {
        done = rq->next;
        rq->next = 0;
        i915_rps_retire(rq);
        request_release(rq);
    }
}

#ifndef RACH2_HOSTTEST
/** Let go of the big lock, which the worker holds as kernel code does, for
 * its retire pass: the depth for `retire_big_take`. */
static int retire_big_drop(void)
{
    uint32_t flags = kirq_save();
    int depth = bkl_drop();

    kirq_restore(flags);
    return depth;
}

/** Take the big lock back at `depth` after the retire pass. */
static void retire_big_take(int depth)
{
    uint32_t flags = kirq_save();

    bkl_take(depth);
    kirq_restore(flags);
}
#else
/** The host tests have no big lock. */
static int retire_big_drop(void)
{
    return 0;
}

/** The host tests have no big lock. */
static void retire_big_take(int depth)
{
    (void)depth;
}
#endif

/**
 * Retire each engine's completed requests and, when `park` is set, the
 * pass of Linux's intel_gt_retire_requests: first each engine whose last
 * request is complete counts as one Linux would have retired
 * (`unretired` cleared), then, when no engine has a request on its list,
 * one being built or one Linux would not yet have retired, the GT parks,
 * and the pool's stale shadows go. Under the GEM lock, since a request's
 * release lets go of its objects and their bindings. For the retire
 * worker (`worker`) the lock is only tried, and the pass skipped,
 * returning 0, when another thread holds it, as Linux's retire worker
 * does with each timeline's mutex (gt/intel_gt_requests.c:77-80,
 * 144-145); and the worker's big lock goes while the GEM lock is held,
 * which keeps the device on the worker's list meanwhile, since its unplug
 * takes the GEM lock. Returns 1 when the pass ran.
 */
static int retire_pass(struct drm_i915_private *i915, int park, int worker)
{
    struct i915_gt *gt = &i915->gt;
    uint32_t flags;
    int i, held = 0, taken, depth = 0;

    if (!worker) {
        taken = i915_gem_lock(i915);
    } else if (!i915_gem_trylock(i915, &taken)) {
        return 0;
    } else {
        depth = retire_big_drop();
    }
    for (i = 0; park && i < I915_NUM_ENGINES; i++) {
        struct i915_engine *e = &gt->engine[i];

        /* One being built is busy, as Linux's trylock of a held timeline
         * mutex counts it (gt/intel_gt_requests.c:144-145). */
        flags = spin_lock_irqsave(&e->lock);
        if (e->unretired && !e->building &&
            (!e->requests_tail || i915_engine_passed(e, e->requests_tail->seqno))) {
            e->unretired = 0;
        }
        spin_unlock_irqrestore(&e->lock, flags);
    }
    for (i = 0; i < I915_NUM_ENGINES; i++) {
        engine_retire(i915, &gt->engine[i], 0, 0);
        held |= gt->engine[i].requests != 0 || gt->engine[i].unretired || gt->engine[i].building;
    }
    if (park && gt->awake && !held) {
        gt_park(i915);
    }
    if (park) {
        /* Under the lock, so that a shadow's last put joins `gem.freed`
         * and the worker never waits for the lock in its free. */
        i915_gem_pool_trim(i915, I915_POOL_STALE);
    }
    if (worker) {
        retire_big_take(depth);
    }
    i915_gem_unlock(i915, taken);
    return 1;
}

void i915_retire_requests(struct drm_i915_private *i915)
{
    (void)retire_pass(i915, 0, 0);
}

void i915_gt_retire_requests(struct drm_i915_private *i915)
{
    (void)retire_pass(i915, 1, 0);
}

void i915_request_retire_engine(struct i915_request *rq)
{
    lockdep_assert_held(&rq->i915->gem.lock);
    engine_retire(rq->i915, rq->engine, 0, rq);
}

/**
 * The retire worker, Linux's retire_work_handler and engine_retire: on
 * each device whose GT is awake, the pass of every engine at
 * `retire_deadline`, which parks an idle GT, and the requests of an engine
 * whose last request signalled to a listener at once, which does not.
 * Linux parks an engine only after a switch to its kernel context has run
 * and retired (gt/intel_engine_pm.c:151-288), and on the Lenovo
 * its GT stayed awake from one call of WineD3D's round trip to the next:
 * 1,013 of the GPU's interrupts for 1,003 calls, where a park after each
 * would add the switch's own (dev-log/84-locks/84-157.md). So the GT
 * stays awake between a client's requests, and RC6 and RPS's down
 * interrupts lower its power meanwhile.
 *
 * Each pass only tries the GEM lock and is skipped while another thread
 * holds it, as Linux's worker only tries each timeline's mutex: a holder
 * that builds a request retires the engine itself, at the request's
 * creation and after its add, and what no holder retires waits for the
 * next pass, within two seconds. A pass runs without the big lock, which
 * nothing in a retire needs, as EXECBUFFER2's retires run without it: what
 * it changes is the GEM lock's and the engines' locks', and the one free
 * that reaches state of the big lock's, an import's exporter on a device
 * without DRIVER_KMS_UNLOCKED, takes it there (drm_prime.c). In the
 * worker, or a host test's call of `i915_submit_work`.
 */
static void retire_work(void)
{
    struct drm_i915_private *i915;
    uint64_t now = rtc_ticks_now();
    uint32_t flags;
    int kicked, due;

    /* Taken and cleared in one step, Linux's xchg(&engine->retire, NULL)
     * (gt/intel_gt_requests.c:64): a signal after it kicks the next pass. */
    flags = spin_lock_irqsave(&submit_lock);
    kicked = retire_kicked;
    retire_kicked = 0;
    spin_unlock_irqrestore(&submit_lock, flags);
    for (i915 = submit_devices; i915; i915 = i915->gt.submit_next) {
        if (!i915->gt.awake) {
            continue;
        }
        due = now >= i915->gt.retire_next;
        if (!due && !kicked) {
            continue;
        }
        if (due) {
            i915->gt.retire_next = retire_deadline(now);
        }
        (void)retire_pass(i915, due, 1);
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

/** The request being built on `e` is gone without being published: the
 * timeline is let go, as Linux's error paths unlock its mutex. The hold
 * on the GT it took stays until the retire worker's pass. */
static void request_unbuild(struct i915_engine *e)
{
    uint32_t flags = spin_lock_irqsave(&e->lock);

    e->building = 0;
    spin_unlock_irqrestore(&e->lock, flags);
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
 * and room kept for the breadcrumb. `room` zeroed bytes follow the request
 * in its block, freed with it.
 */
static int32_t request_new(struct intel_context *ce, size_t room, struct i915_request **out)
{
    struct drm_i915_private *i915 = ce->i915;
    struct i915_engine *e = ce->engine;
    struct i915_request *rq;
    uint64_t now;
    uint32_t flags;
    int32_t err;
    int idle;

    lockdep_assert_held(&i915->gem.lock);
    err = intel_context_pin_image(ce);
    if (err < 0) {
        return err;
    }
    rq = (struct i915_request *)kzalloc(sizeof(*rq) + room);
    if (!rq) {
        return -ENOMEM;
    }
    /* One tick for the GT's unpark and the engine's, so that the engine's
     * first beat is the GT's first. */
    now = rtc_ticks_now();
    if (!i915->gt.awake) {
        /* Linux's __gt_unpark takes POWER_DOMAIN_GT_IRQ's wakeref, which
         * the park puts. */
        i915_rpm_get(i915);
        i915->gt.awake = 1;
        i915->gt.unpark_ticks = now;
        i915->gt.retire_next = retire_deadline(now);
        i915_gt_unpark(i915);
        /* The worker beats while the GT is awake. */
        i915_hangcheck_unpark(i915, now);
        submit_device_add(i915);
        hangcheck_kicked = 1;
        wq_wake_all(&submit_wait);
    }
    if (!e->fence_context) {
        e->fence_context = dma_fence_context_alloc(1);
    }
    /* The request is on the timeline and holds it until it is published,
     * as Linux's i915_request_create takes the timeline's mutex
     * (i915_request.c:1033) and __i915_request_create puts the request on
     * the timeline's list under it (i915_request.c:1009). */
    flags = spin_lock_irqsave(&e->lock);
    /* The engine is awake, as Linux's engine wakeref is held, while it
     * holds the GT awake by the parking rule. */
    idle = !e->requests && !e->unretired && !e->building;
    e->seqno = e->seqno == 0xffffffffu ? 1u : e->seqno + 1u; /* 0 is never a seqno */
    e->unretired = 1;
    e->building = 1;
    spin_unlock_irqrestore(&e->lock, flags);
    if (idle && i915_hangcheck_unpark_engine(i915, e, now)) {
        hangcheck_kicked = 1;
        wq_wake_all(&submit_wait);
    }
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
        request_unbuild(e);
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

    lockdep_assert_held(&i915->gem.lock);
    e->reserved = 0;
    rq->postfix = e->tail;
    rq->emitted = rtc_ticks_now();
    n = breadcrumb_bytes(i915, e) / 4u;
    cs = i915_ring_begin(e, i915, n);
    if (!cs) {
        request_put_hold(rq);
        awaits_release(rq);
        request_rewind(rq);
        request_unbuild(e);
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
     * and stays so until the GT parks, as Linux arms the breadcrumbs' and
     * disarms them only when the engine parks or an interrupt finds no
     * listener (gt/intel_breadcrumbs.c:182-207, 266-272); here every
     * breadcrumb interrupts. */
    if (!e->irq_armed) {
        e->irq_armed = 1;
        i915_engine_irq_get(i915, e);
    }
    (void)dma_fence_get(&rq->fence.base);
    /* Published as the engine's last and the timeline let go in one step,
     * as Linux's i915_request_add unlocks the mutex (i915_request.c:1865). */
    flags = spin_lock_irqsave(&e->lock);
    if (e->requests_tail) {
        e->requests_tail->next = rq;
    } else {
        e->requests = rq;
    }
    e->requests_tail = rq;
    e->building = 0;
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

int32_t i915_request_wait(struct i915_request *rq, uint32_t flags, int64_t timeout_ns)
{
    struct drm_i915_private *i915 = rq->i915;
    struct i915_engine *e = rq->engine;
    int intr = (flags & I915_WAIT_INTERRUPTIBLE) != 0, dropped;
    int64_t deadline, slice, now;
    int32_t err = 0;
    uint32_t irq;

    if (!i915_request_completed(rq) && timeout_ns == 0) {
        return -ETIME;
    }
    /* The wait never holds the GEM lock, which the heartbeat's reset and
     * the retirement it waits on take (i915.h's Locking rule). */
    dropped = i915_gem_lock_drop(i915);
    deadline = timeout_ns < 0 ? -1 : dma_fence_now() + timeout_ns;
    /* Linux's optimistic spin (i915_request.c:2030-2032): a request the
     * chip has begun is polled before the wait asks for its signal, and
     * one done by then is not listened for. Otherwise the wait adds a
     * callback, which enables signalling for good (i915_request.c:2050). */
    if (!i915_request_completed(rq) && rq->submitted && i915_request_started(rq)) {
        request_poll(rq, I915_BUSYWAIT_US);
    }
    if (!i915_request_completed(rq)) {
        /* Linux's waitboost before the sleep (i915_request.c:2046-2047). */
        if ((flags & I915_WAIT_PRIORITY) && !i915_request_started(rq)) {
            i915_rps_boost(rq);
        }
        dma_fence_enable_sw_signaling(&rq->fence.base);
    }
    irq = spin_lock_irqsave(&e->lock);
    rq->waiters++;
    spin_unlock_irqrestore(&e->lock, irq);
    while (!i915_request_completed(rq)) {
        now = dma_fence_now();
        if (deadline >= 0 && now >= deadline) {
            i915_request_signal(i915, e);
            if (!i915_request_completed(rq)) {
                err = -ETIME;
            }
            break;
        }
        slice = now + I915_WAIT_SLICE_NS;
        if (deadline >= 0 && deadline < slice) {
            slice = deadline;
        }
        if (current_proc && i915->msi_vector >= 0) {
            /* The default sleep: the fence's own wait is this one. */
            err = dma_fence_default_wait(&rq->fence.base, intr, slice);
            if (err == -EINTR) {
                break;
            }
            err = 0;
        } else {
            /* Without the interrupt the breadcrumb is polled. */
            request_poll(rq, tick_div((uint64_t)(slice - now), 1000u, (uint32_t *)0) + 1u);
        }
        /* An interrupt may have been lost: the page is read again; and a
         * heartbeat may be due, which resets a hung engine. */
        i915_request_signal(i915, e);
        (void)i915_hangcheck(i915);
    }
    irq = spin_lock_irqsave(&e->lock);
    rq->waiters--;
    spin_unlock_irqrestore(&e->lock, irq);
    i915_gem_lock_retake(i915, dropped);
    if (err == 0) {
        i915_retire_requests(i915);
    }
    return err;
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
    err = request_new(ce, 0, &rq);
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
        err = request_new(e->kernel_ce, 0, &flush);
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
    /* Linux waits for the GT's idle after the recording, which parks it. */
    i915_gt_retire_requests(i915);
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
        err = i915_request_wait(rq, I915_WAIT_INTERRUPTIBLE, -1);
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
        i915_gt_retire_requests(i915);
        rq = 0;
        for (i = 0; i < I915_NUM_ENGINES && !rq; i++) {
            rq = i915->gt.engine[i].requests_tail;
        }
        if (!rq) {
            return 0;
        }
        (void)dma_fence_get(&rq->fence.base);
        /* Linux's waits on each timeline's last request through the
         * fence, which boosts (gt/intel_gt_requests.c:161). */
        err = i915_request_wait(rq, (intr ? I915_WAIT_INTERRUPTIBLE : 0u) | I915_WAIT_PRIORITY, -1);
        i915_request_put(rq);
        if (err < 0) {
            return err;
        }
    }
}

int32_t i915_request_create_room(struct intel_context *ce, size_t room, struct i915_request **out)
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
    return request_new(ce, room, out);
}

int32_t i915_request_create(struct intel_context *ce, struct i915_request **out)
{
    return i915_request_create_room(ce, 0, out);
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

        engine_retire(i915, e, 1, 0);
        e->irq_notify = 0;
        intel_context_put(e->last_ce);
        e->last_ce = 0;
        intel_context_put(e->kernel_ce);
        e->kernel_ce = 0;
    }
    if (gt->awake) {
        gt_park(i915);
    }
    i915_gem_pool_trim(i915, I915_POOL_ALL);
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
        engine_retire(i915, &gt->engine[i], 1, 0);
    }
    if (gt->awake) {
        gt_park(i915);
    }
}
