/*
 * i915_reset.c: the Intel graphics driver's recovery from a hang: the
 * reset of a hung engine through its domain of GEN6_GDRST, the whole GPU's
 * when that fails, the guilty request skipped and its context perhaps
 * banned, the requests behind it run again, and the GPU wedged when
 * nothing resets it, after Linux 6.12's gt/intel_reset.c and
 * gt/intel_ring_submission.c on Haswell.
 */

#include "i915.h"
#include "i915_reg.h"

#include "../../klog.h"
#include "../../lib/kerrno.h"
#include "../../rtc.h"

/* Linux's bounds (gen6_hw_domain_reset, do_reset): 2 ms for GDRST's
 * acknowledgement, which is asked for twice, a settling delay of 50 us
 * after it, and three more tries of the whole GPU's reset, 10 ms more
 * apart each time (RESET_MAX_RETRIES). */
#define I915_GDRST_US        2000u
#define I915_GDRST_SETTLE_US 50u
#define I915_RESET_RETRIES   3

/**
 * Reset the domains `mask` of GEN6_GDRST, as Linux's gen6_hw_domain_reset:
 * the bits written and waited for until they read clear, twice, since a
 * repeated reset waits for the first to settle, then 50 us more. Under
 * forcewake.
 *
 * @return 0, or -ETIMEDOUT when a bit stayed set for 2 ms.
 */
static int32_t gdrst(struct drm_i915_private *i915, uint32_t mask)
{
    struct i915_hw *hw = &i915->hw;
    int loops = 2;
    int32_t err;

    do {
        i915_write(hw, GEN6_GDRST, mask);
        err = i915_wait_for(hw, "gdrst", GEN6_GDRST, mask, 0, I915_GDRST_US);
    } while (err == 0 && --loops > 0);
    i915_delay_us(hw, I915_GDRST_SETTLE_US);
    return err;
}

/**
 * Clear the error registers after a hang's state is captured, as Linux's
 * intel_gt_clear_error_registers on graphics 7: the GTT's errors, the
 * render parser's and EIR, a stuck EIR bit masked in EMR, and the fault
 * register of each engine of `engine_mask`, under forcewake.
 */
static void clear_error_registers(struct drm_i915_private *i915, uint32_t engine_mask)
{
    uint32_t eir;
    int fw, i;

    /* EMR and the fault registers are the GT's, which read stale while it
     * sleeps. */
    fw = i915_forcewake_get(i915) == 0;
    i915_uncore_write(i915, PGTBL_ER, 0);
    i915_uncore_write(i915, IPEIR_I965, 0);
    i915_uncore_write(i915, EIR, 0);
    eir = i915_uncore_read(i915, EIR);
    if (eir) {
        klog(KLOG_DEBUG, "i915", "reset: EIR stuck: 0x%08x, masking", (unsigned)eir);
        (void)i915_rmw(&i915->hw, EMR, 0, eir);
        i915_uncore_write(i915, GEN2_IIR, I915_MASTER_ERROR_INTERRUPT);
    }
    for (i = 0; i < I915_NUM_ENGINES; i++) {
        if (engine_mask & (1u << i)) {
            (void)i915_rmw(&i915->hw, i915->gt.engine[i].fault_reg, RING_FAULT_VALID, 0);
            i915_posting_read(&i915->hw, i915->gt.engine[i].fault_reg);
        }
    }
    if (fw) {
        i915_forcewake_put(i915);
    }
}

/**
 * Ban the context of `rq`, a guilty request, as Linux's intel_context_ban
 * with ring_context_revoke: EXECBUFFER2 refuses it from now on, and its
 * requests behind `rq` on the engine fail with -EIO, their payloads
 * skipped.
 */
static void context_ban(struct i915_request *rq)
{
    struct intel_context *ce = rq->ce;
    struct i915_request *later;

    ce->banned = 1;
    for (later = rq->next; later; later = later->next) {
        if (later->ce == ce) {
            i915_request_set_error_once(later, -EIO);
            i915_request_skip(later);
        }
    }
}

/**
 * Settle `rq`, the first request of its engine the reset found not
 * completed, as Linux's __i915_request_reset: a guilty one fails with
 * -EIO, its payload skipped, its context's hang counted and the context
 * perhaps banned; an innocent one takes -EAGAIN, which does not stop its
 * commands from running again, and its context counts it.
 */
static void request_reset(struct i915_request *rq, int guilty)
{
    if (guilty) {
        i915_request_set_error_once(rq, -EIO);
        i915_request_skip(rq);
        if (i915_gem_context_mark_guilty(rq->ce, rtc_ticks_now())) {
            context_ban(rq);
        }
    } else {
        i915_request_set_error_once(rq, -EAGAIN);
        i915_gem_context_mark_innocent(rq->ce);
    }
}

/**
 * Find where `e`'s ring starts again after a reset, as Linux's
 * reset_rewind: at the head of its first request on the chip not
 * completed, which is settled as guilty when the engine `stalled` and
 * innocent otherwise, so that its switch, breadcrumb and the requests
 * behind it run; or at the end of what the chip was given when none is.
 */
static uint32_t engine_rewind(struct i915_engine *e, int stalled)
{
    struct i915_request *rq;

    for (rq = e->requests; rq && i915_engine_passed(e, rq->seqno); rq = rq->next) {
    }
    if (!rq || !rq->submitted) {
        return e->hw_tail;
    }
    request_reset(rq, stalled);
    return rq->head;
}

/**
 * Start `e`'s ring again from `head` to the end of what the chip was
 * given, its user interrupt unmasked again when requests wait for it, as
 * the reset cleared RING_IMR, and the breadcrumbs read in case the reset
 * lost an interrupt, as Linux's xcs_resume does. Under forcewake.
 *
 * @return 0, or -EIO when the ring would not start.
 */
static int32_t engine_resume(struct drm_i915_private *i915, struct i915_engine *e, uint32_t head)
{
    int32_t err = i915_ring_resume(i915, e, head, e->hw_tail);

    if (err == 0 && e->irq_refs) {
        i915_uncore_write(i915, RING_IMR(e->mmio_base), ~e->irq_bit);
        (void)i915_uncore_read(i915, RING_IMR(e->mmio_base));
    }
    e->hc_armed = 0;
    if (e->irq_notify) {
        e->irq_notify(i915, e);
    }
    return err;
}

int32_t i915_reset_engine(struct drm_i915_private *i915, struct i915_engine *e, const char *msg)
{
    int32_t err;

    if (i915->gt.wedged || !e->running) {
        return -EIO;
    }
    if (!e->requests) {
        return 0;
    }
    if (i915_forcewake_get(i915) < 0) {
        return -EIO;
    }
    i915_ring_reset_prepare(i915, e);
    klog(KLOG_INFO, "i915", "reset: %s for %s", e->name, msg);
    i915->gt.reset_engine_count[i915_engine_uabi_class(e)]++;
    err = gdrst(i915, e->reset_domain);
    if (err == 0) {
        err = engine_resume(i915, e, engine_rewind(e, 1));
    } else {
        klog(KLOG_ERR, "i915", "reset: %s: its domain did not reset: GDRST reads 0x%08x", e->name,
             (unsigned)i915_read(&i915->hw, GEN6_GDRST));
        /* The whole GPU's reset follows; the ring is let go meanwhile, as
         * Linux's intel_engine_cancel_stop_cs. */
        i915_write(&i915->hw, RING_MI_MODE(e->mmio_base), I915_MASKED_DISABLE(STOP_RING));
    }
    i915_forcewake_put(i915);
    return err;
}

void i915_gt_set_wedged(struct drm_i915_private *i915)
{
    struct i915_gt *gt = &i915->gt;
    int fw, i;

    klog(KLOG_ERR, "i915", "reset: wedged: the GPU takes no work until a reset succeeds");
    gt->wedged = 1;
    fw = i915_forcewake_get(i915) == 0;
    for (i = 0; i < I915_NUM_ENGINES; i++) {
        if (gt->engine[i].running) {
            i915_ring_reset_prepare(i915, &gt->engine[i]);
        }
    }
    if (fw) {
        i915_forcewake_put(i915);
    }
    i915_requests_cancel(i915);
}

/**
 * Revoke the programs' aperture maps of each fenced binding before a reset
 * of the whole GPU clears the fences, as Linux's revoke_mmaps, so that an
 * access through one faults in again once the fences are written back
 * rather than reaching memory untiled meanwhile.
 */
static void revoke_mmaps(struct drm_i915_private *i915)
{
    int i;

    for (i = 0; i < I915_NUM_FENCES; i++) {
        if (i915->gem.fences[i].vma) {
            i915_vma_revoke_mmap(i915->gem.fences[i].vma);
        }
    }
}

/**
 * Stop every running ring by force and reset the whole GPU, as Linux's
 * reset.prepare of each engine and __intel_gt_reset of all of them: the
 * full domain's reset, and three more tries 10 ms more apart each time.
 * The rings stay stopped. Under forcewake.
 *
 * @return 0, or -ETIMEDOUT when no try reset the GPU, which is logged.
 */
static int32_t gt_stop_and_reset(struct drm_i915_private *i915)
{
    struct i915_gt *gt = &i915->gt;
    int32_t err;
    int i;

    for (i = 0; i < I915_NUM_ENGINES; i++) {
        if (gt->engine[i].running) {
            i915_ring_reset_prepare(i915, &gt->engine[i]);
        }
    }
    err = gdrst(i915, GEN6_GRDOM_FULL);
    for (i = 0; err < 0 && i < I915_RESET_RETRIES; i++) {
        i915_delay_us(&i915->hw, 10000u * (uint32_t)(i + 1));
        err = gdrst(i915, GEN6_GRDOM_FULL);
    }
    if (err < 0) {
        klog(KLOG_ERR, "i915", "reset: the chip did not reset: GDRST reads 0x%08x",
             (unsigned)i915_read(&i915->hw, GEN6_GDRST));
    }
    return err;
}

int32_t i915_gt_quiesce(struct drm_i915_private *i915)
{
    int32_t err;

    if (i915_forcewake_get(i915) < 0) {
        return -EIO;
    }
    err = gt_stop_and_reset(i915);
    i915_forcewake_put(i915);
    if (err < 0) {
        i915_gt_set_wedged(i915);
    }
    return err;
}

void i915_reset_gt(struct drm_i915_private *i915, uint32_t stalled_mask, const char *msg)
{
    struct i915_gt *gt = &i915->gt;
    uint32_t head[I915_NUM_ENGINES];
    int32_t err;
    int fw, i;

    revoke_mmaps(i915);
    if (gt->wedged_terminal) {
        return;
    }
    /* A wedge a failed reset left is tried again, as Linux's
     * __intel_gt_unset_wedged; its requests are gone already. */
    gt->wedged = 0;
    klog(KLOG_INFO, "i915", "reset: the chip for %s", msg);
    gt->reset_count++;
    if (I915_RESET_PARAM(i915) == 0) {
        /* i915.reset 0: no reset, so a hang wedges the GPU (intel_gt_reset). */
        klog(KLOG_INFO, "i915", "reset: disabled");
        i915_gt_set_wedged(i915);
        return;
    }
    fw = i915_forcewake_get(i915) == 0;
    err = gt_stop_and_reset(i915);
    if (err == 0) {
        for (i = 0; i < I915_NUM_ENGINES; i++) {
            head[i] = 0;
            if (gt->engine[i].running) {
                head[i] = engine_rewind(&gt->engine[i], (int)((stalled_mask >> i) & 1u));
            }
        }
        i915_gem_restore_fences(i915);
        i915_gt_init_hw(i915);
        for (i = 0; i < I915_NUM_ENGINES && err == 0; i++) {
            if (gt->engine[i].running) {
                err = engine_resume(i915, &gt->engine[i], head[i]);
            }
        }
        if (err < 0) {
            klog(KLOG_ERR, "i915", "reset: the rings did not start again: error %d", (int)err);
        }
    }
    if (fw) {
        i915_forcewake_put(i915);
    }
    if (err < 0) {
        i915_gt_set_wedged(i915);
    }
}

void i915_handle_error(struct drm_i915_private *i915, uint32_t engine_mask, const char *msg)
{
    int i;

    engine_mask &= (1u << I915_NUM_ENGINES) - 1u;
    i915_gpu_error_capture(i915, engine_mask, msg);
    clear_error_registers(i915, engine_mask);
    if (i915->id->platform->has_reset_engine && I915_RESET_PARAM(i915) >= 2 && !i915->gt.wedged) {
        for (i = 0; i < I915_NUM_ENGINES; i++) {
            if ((engine_mask & (1u << i)) &&
                i915_reset_engine(i915, &i915->gt.engine[i], msg) == 0) {
                engine_mask &= ~(1u << i);
            }
        }
    }
    if (engine_mask) {
        i915_reset_gt(i915, engine_mask, msg);
    }
}
