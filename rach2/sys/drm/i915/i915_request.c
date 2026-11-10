/*
 * i915_request.c: the Intel graphics driver's requests on the render and
 * blitter rings: their commands, the context switch, the breadcrumb that
 * closes each, their fences signalled from the user interrupt, waits and
 * retirement, and the default context image, after Linux 6.18's
 * i915_request.c, gt/intel_ring_submission.c, gt/gen6_engine_cs.c,
 * gt/intel_gt_requests.c and gt/intel_gt.c on Haswell.
 */

#include "i915.h"
#include "i915_reg.h"

#include "../../dmabuf/dmabuf.h"
#include "../../klog.h"
#include "../../lib/kerrno.h"
#include "../../lib/kstring.h"
#include "../../mm/kheap.h"
#include "../../proc/percpu.h"
#include "../../timer.h"

/* The dwords of the commands a request takes: the render ring's flush
 * with its invalidation, Linux's gen7_emit_flush_rcs with gen7_stall_cs;
 * the blitter's, mi_flush_dw; the context switch, mi_set_context on
 * Haswell with one other engine; and the breadcrumbs, gen7_emit_breadcrumb
 * _rcs and _xcs, the blitter's with GEN7_XCS_WA stores. */
#define I915_FLUSH_RCS_DW      8u
#define I915_FLUSH_XCS_DW      4u
#define I915_SET_CONTEXT_DW    16u
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

/** Whether `rq`'s breadcrumb has been written or its fence signalled. */
int i915_request_completed(const struct i915_request *rq)
{
    return rq->fence.base.signaled || i915_engine_passed(rq->engine, rq->seqno);
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

/** Give up what a request held once the chip is past it: the state it
 * switched from, its own, and the engine list's reference. */
static void request_release(struct i915_request *rq)
{
    intel_context_put(rq->prev_ce);
    rq->prev_ce = 0;
    intel_context_put(rq->ce);
    rq->ce = 0;
    dma_fence_put(&rq->fence.base);
}

/**
 * Take from `e`'s list each request that `done` says is finished, oldest
 * first, its fence signalled, and release it; the user interrupt is let go
 * once the list is empty.
 */
static void engine_retire(struct drm_i915_private *i915, struct i915_engine *e, int all)
{
    struct i915_request *done = 0, **tail = &done, *rq;
    uint32_t flags;

    flags = spin_lock_irqsave(&e->lock);
    while ((rq = e->requests) != 0 && (all || i915_engine_passed(e, rq->seqno))) {
        e->requests = rq->next;
        rq->next = 0;
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
        gt->awake = 0;
        i915_gt_park(i915);
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
 * scratch page; on the blitter MI_FLUSH_DW with the TLBs. 0 or -EIO. */
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

/**
 * Load the render ring's state `ce` with MI_SET_CONTEXT, as Linux's
 * mi_set_context on Haswell: arbitration off and the blitter's sleep
 * messages held around it, the image saved and restored with its extended
 * state, or its restore inhibited while it holds nothing, then a register
 * read written to the scratch page as a delay before the next switch.
 * 0 or -EIO.
 */
static int32_t emit_set_context(struct i915_request *rq, struct intel_context *ce, uint32_t flags)
{
    uint32_t psmi = RING_PSMI_CTL(rq->i915->gt.engine[I915_BCS0].mmio_base);
    uint32_t *cs = i915_request_begin(rq, I915_SET_CONTEXT_DW);

    if (!cs) {
        return -EIO;
    }
    cs[0] = MI_ARB_ON_OFF | MI_ARB_DISABLE;
    cs[1] = MI_LOAD_REGISTER_IMM(1);
    cs[2] = psmi;
    cs[3] = I915_MASKED_ENABLE(GEN6_PSMI_SLEEP_MSG_DISABLE);
    cs[4] = MI_NOOP;
    cs[5] = MI_SET_CONTEXT;
    cs[6] = ce->image_ggtt | flags;
    cs[7] = MI_NOOP; /* MI_SET_CONTEXT is followed by MI_NOOP, as Linux */
    cs[8] = MI_LOAD_REGISTER_IMM(1);
    cs[9] = psmi;
    cs[10] = I915_MASKED_DISABLE(GEN6_PSMI_SLEEP_MSG_DISABLE);
    cs[11] = MI_STORE_REGISTER_MEM | MI_SRM_LRM_GLOBAL_GTT;
    cs[12] = psmi;
    cs[13] = rq->i915->gt.scratch_ggtt + I915_SCRATCH_DEFAULT;
    cs[14] = MI_NOOP;
    cs[15] = MI_ARB_ON_OFF | MI_ARB_ENABLE;
    return 0;
}

/**
 * Switch the chip to the request's state, as Linux's switch_context: on
 * the render ring its image loaded, valid from then on, and the engine's
 * `last_ce` made it, the request keeping the one it replaces until it
 * retires. The blitter has no state to switch. 0 or -EIO.
 */
static int32_t request_switch(struct i915_request *rq)
{
    struct i915_engine *e = rq->engine;
    struct intel_context *ce = rq->ce;
    uint32_t flags = MI_SAVE_EXT_STATE_EN | MI_MM_SPACE_GTT;
    int32_t err;

    if (!ce->image) {
        return 0;
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
        i915_gt_unpark(i915);
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
    rq->head = e->tail;
    e->reserved = breadcrumb_bytes(i915, e);
    err = emit_invalidate(rq);
    if (err == 0) {
        err = request_switch(rq);
    }
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
    n = breadcrumb_bytes(i915, e) / 4u;
    cs = i915_ring_begin(e, i915, n);
    if (!cs) {
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
    i915_ring_advance(e, i915);
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
        /* An interrupt may have been lost: the page is read again. */
        i915_request_signal(i915, e);
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
 * inhibited takes Linux's context workarounds for graphics version 7
 * (intel_workarounds.c's gen7_ctx_workarounds: INSTPM's forced ordering,
 * CACHE_MODE_0_GEN7's RC op flush off, CACHE_MODE_1's pixel subspan
 * collect optimization off), the switch to the GT's own context saves its
 * image, and the image is kept as the state every new context starts from.
 * Linux also runs the null render state batch, which needs the batches of
 * 78-5b. 0, -ENOMEM, or -EIO when the chip did not finish in time.
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
        klog(KLOG_ERR, "i915", "contexts: the default state was not recorded: error %d; wedged",
             (int)err);
        return -EIO;
    }
    return 0;
}

int32_t i915_request_create(struct intel_context *ce, struct i915_request **out)
{
    struct drm_i915_private *i915 = ce->i915;
    int32_t err;

    if (i915->gt.wedged || !ce->engine->running) {
        return -EIO;
    }
    i915_retire_requests(i915);
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
        gt->awake = 0;
        i915_gt_park(i915);
    }
    i915_gt_pages_unmap(i915, gt->scratch, 1, gt->scratch_ggtt);
    gt->scratch = 0;
    kfree(gt->default_state);
    gt->default_state = 0;
    gt->ctx_ready = 0;
}
