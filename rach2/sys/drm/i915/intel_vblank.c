/*
 * intel_vblank.c: the vblanks of Haswell's pipes and the events of the
 * commits on them, after Linux's display/intel_vblank.c and the display
 * engine's interrupts of i915_irq.c: the frame counter, the vblank and
 * flip done interrupts, and a commit's event sent at the vblank that shows
 * it.
 */

#include "i915.h"
#include "i915_reg.h"

#include "../../klog.h"
#include "../../lib/kerrno.h"
#include "../../lib/kirq.h"
#include "../../mm/kheap.h"

/*
 * The interrupt handler takes the pipe's events under the interrupt mask's
 * lock and leaves them in `display.de_iir`; the deferred work then runs
 * the DRM core's vblank code, which may mask the vblank again through
 * `intel_disable_vblank`, a call that takes that lock. Process context
 * reaches the same state, the events and the vblank counts, with
 * interrupts off.
 */

/** The display engine's events of each pipe that the driver takes, and
 * the error interrupt that carries the FIFO underruns. */
static uint32_t de_bits(void)
{
    uint32_t bits = DE_ERR_INT_IVB;
    int pipe;

    for (pipe = 0; pipe < I915_NUM_PIPES; pipe++) {
        bits |= DE_PIPE_VBLANK_IVB(pipe) | DE_PLANE_FLIP_DONE_IVB(pipe);
    }
    return bits;
}

uint32_t intel_get_vblank_counter(struct drm_crtc *crtc)
{
    struct intel_crtc *c = to_intel_crtc(crtc);

    return i915_read(&c->i915->hw, PIPE_FRMCOUNT_G4X(c->pipe));
}

/**
 * The line `c` scans, Linux's __intel_get_crtc_scanline: PIPEDSL, read
 * again for up to 100 us while it gives 0, which Haswell's may just before
 * the vertical blank, then moved by Linux's intel_crtc_scanline_offset,
 * since the counter runs a line behind, two on HDMI; 0 for a pipe that is
 * off. `vtotal` is the frame's lines.
 */
static int32_t crtc_scanline(struct intel_crtc *c, int32_t vtotal)
{
    struct i915_hw *hw = &c->i915->hw;
    int32_t position, temp;
    int i;

    if (!c->cfg.active) {
        return 0;
    }
    position = (int32_t)(i915_read(hw, PIPEDSL(c->pipe)) & PIPEDSL_LINE_MASK);
    for (i = 0; position == 0 && i < 100; i++) {
        i915_delay_us(hw, 1);
        temp = (int32_t)(i915_read(hw, PIPEDSL(c->pipe)) & PIPEDSL_LINE_MASK);
        if (temp != position) {
            position = temp;
        }
    }
    return (position + vtotal + (c->cfg.output == INTEL_OUTPUT_HDMI ? 2 : 1)) % vtotal;
}

/**
 * Where `crtc` scans, Linux's i915_get_crtc_scanoutpos with the scanline
 * counter: the line from the end of the vertical blank of `mode`,
 * negative inside it, read with interrupts off between `*stime` and
 * `*etime`; Linux's intel_mode_vtotal and its kin halve an interlaced
 * mode's lines to a field's. 1, or 0 for a mode without a clock.
 */
static int crtc_scanoutpos(struct drm_crtc *crtc, int in_vblank_irq, int32_t *vpos, int32_t *hpos,
                           int64_t *stime, int64_t *etime, const struct drm_mode_modeinfo *mode)
{
    struct intel_crtc *c = to_intel_crtc(crtc);
    int32_t vtotal = mode->vtotal, position;
    int32_t vbl_start = mode->vsync_start < mode->vdisplay ? mode->vsync_start : mode->vdisplay;
    int32_t vbl_end = mode->vsync_end > mode->vtotal ? mode->vsync_end : mode->vtotal;
    uint32_t flags;

    (void)in_vblank_irq;
    if (mode->clock == 0 || vtotal == 0) {
        return 0;
    }
    if (mode->flags & DRM_MODE_FLAG_INTERLACE) {
        vtotal /= 2;
        vbl_start = (vbl_start + 1) / 2;
        vbl_end /= 2;
    }
    flags = kirq_save();
    *stime = drm_vblank_ktime();
    position = crtc_scanline(c, vtotal);
    *etime = drm_vblank_ktime();
    kirq_restore(flags);
    *vpos = position >= vbl_start ? position - vbl_end : position + vtotal - vbl_end;
    *hpos = 0;
    return 1;
}

int intel_crtc_get_vblank_timestamp(struct drm_crtc *crtc, int64_t *time_ns, int in_vblank_irq)
{
    return drm_crtc_vblank_helper_get_vblank_timestamp_internal(
        crtc, time_ns, in_vblank_irq, crtc_scanoutpos, &to_intel_crtc(crtc)->cfg.mode);
}

int32_t intel_enable_vblank(struct drm_crtc *crtc)
{
    struct intel_crtc *c = to_intel_crtc(crtc);

    i915_irq_unmask(c->i915, I915_IRQ_DE, DE_PIPE_VBLANK_IVB(c->pipe));
    c->vbl_on = 1;
    return 0;
}

void intel_disable_vblank(struct drm_crtc *crtc)
{
    struct intel_crtc *c = to_intel_crtc(crtc);

    i915_irq_mask(c->i915, I915_IRQ_DE, DE_PIPE_VBLANK_IVB(c->pipe));
    c->vbl_on = 0;
}

void intel_crtc_interactive_end(struct intel_crtc *c)
{
    uint8_t *mark;

    if (c->rps_mark && !c->base.flip_pending) {
        mark = &c->i915->display.rps_marks[c->rps_mark - 1];
        *mark &= (uint8_t)~(1u << c->pipe);
        c->rps_mark = 0;
        if (*mark == 0) {
            i915_rps_mark_interactive(c->i915, 0);
        }
    }
}

void intel_crtc_send_event(struct intel_crtc *c)
{
    struct drm_pending_event *e = c->event;

    if (!e) {
        intel_crtc_interactive_end(c);
        return;
    }
    c->event = NULL;
    if (c->event_async) {
        i915_irq_mask(c->i915, I915_IRQ_DE, DE_PLANE_FLIP_DONE_IVB(c->pipe));
        c->event_async = 0;
    }
    drm_crtc_send_vblank_event(&c->base, e);
    drm_crtc_vblank_put(&c->base);
    intel_crtc_interactive_end(c);
}

void intel_crtc_boost_after_vblank(struct intel_crtc *c, struct dma_fence *fence)
{
    struct intel_boost *b;
    uint32_t flags;

    if (!fence || fence->ops != &i915_fence_ops || drm_crtc_vblank_get(&c->base) != 0) {
        return;
    }
    b = (struct intel_boost *)kmalloc(sizeof(*b));
    if (!b) {
        drm_crtc_vblank_put(&c->base);
        return;
    }
    b->rq = (struct i915_request *)dma_fence_get(fence);
    flags = kirq_save();
    b->next = c->boosts;
    c->boosts = b;
    kirq_restore(flags);
}

/** Take `c`'s boosts after a vblank, each with its references let go and
 * its entry freed; at the vblank (`boost`) the requests the chip has not
 * begun are boosted, as Linux's do_rps_boost, since one that runs may be
 * taken to end before the next. Interrupts masked. */
static void vblank_boost(struct intel_crtc *c, int boost)
{
    struct intel_boost *b;

    while ((b = c->boosts) != NULL) {
        c->boosts = b->next;
        if (boost && !i915_request_started(b->rq)) {
            i915_rps_boost(b->rq);
        }
        dma_fence_put(&b->rq->fence.base);
        drm_crtc_vblank_put(&c->base);
        kfree(b);
    }
}

void intel_crtc_vblank_off(struct intel_crtc *c)
{
    uint32_t flags;

    drm_crtc_vblank_off(&c->base);
    flags = kirq_save();
    vblank_boost(c, 1);
    kirq_restore(flags);
}

void intel_crtc_arm_flip_done(struct intel_crtc *c, struct drm_atomic_state *state)
{
    struct drm_crtc_state *cs = state->crtcs_new[c->base.index];

    if (cs && cs->async_flip && cs->event) {
        i915_irq_unmask(c->i915, I915_IRQ_DE, DE_PLANE_FLIP_DONE_IVB(c->pipe));
    }
}

void intel_crtc_arm_event(struct intel_crtc *c, struct drm_atomic_state *state)
{
    struct drm_crtc_state *cs = state->crtcs_new[c->base.index];
    struct drm_pending_event *e;
    uint32_t flags;

    if (!cs || !(e = cs->event)) {
        return;
    }
    cs->event = NULL;
    flags = kirq_save();
    if (e->flip_crtc && c->base.flip_pending == DRM_FLIP_QUEUED) {
        c->base.flip_pending = DRM_FLIP_ARMED;
        wq_wake_all(&c->base.vblank.wait);
    }
    if (c->i915->drm.unplugged) {
        dma_fence_set_error(e->fence, -ENODEV);
    }
    /* A pipe has one commit in flight, which the core's stall keeps so;
     * one left over is sent before the new is held. */
    intel_crtc_send_event(c);
    if (cs->active && drm_crtc_vblank_get(&c->base) == 0) {
        c->event = e;
        c->event_frame = intel_get_vblank_counter(&c->base);
        c->event_async = cs->async_flip != 0;
        if (c->event_async) {
            /* Unmasked before the planes were written, and again in case
             * the event left over masked it; IIR keeps a flip done that
             * came between. */
            i915_irq_unmask(c->i915, I915_IRQ_DE, DE_PLANE_FLIP_DONE_IVB(c->pipe));
        }
    } else {
        if (cs->async_flip) {
            i915_irq_mask(c->i915, I915_IRQ_DE, DE_PLANE_FLIP_DONE_IVB(c->pipe));
        }
        drm_crtc_send_vblank_event(&c->base, e);
        intel_crtc_interactive_end(c);
    }
    kirq_restore(flags);
}

/** The error interrupt, Linux's ivb_err_int_handler and
 * intel_cpu_fifo_underrun_irq_handler: each pipe's underrun, while it is
 * reported, turns its reporting off and is left for the deferred work to
 * log; each pipe's CRC done, while a source runs, reads the frame's CRC
 * for the deferred work to enter, as hsw_pipe_crc_irq_handler, before the
 * next frame's replaces it; GEN7_ERR_INT is cleared of what it held.
 * Under the interrupt mask's lock. */
static void err_int(struct drm_i915_private *i915)
{
    struct intel_display *d = &i915->display;
    uint32_t e = i915_read(&i915->hw, GEN7_ERR_INT);
    int pipe;

    for (pipe = 0; pipe < I915_NUM_PIPES; pipe++) {
        if ((e & ERR_INT_FIFO_UNDERRUN(pipe)) && !d->crtc[pipe].underrun_off) {
            d->crtc[pipe].underrun_off = 1;
            i915_irq_mask_locked(i915, I915_IRQ_DE, DE_ERR_INT_IVB);
            d->underruns |= 1u << pipe;
        }
        if ((e & ERR_INT_PIPE_CRC_DONE(pipe)) && d->crtc[pipe].crc_ctl != 0) {
            d->crtc[pipe].crc_res = i915_read(&i915->hw, PIPE_CRC_RES_HSW(pipe));
            d->crc_done |= 1u << pipe;
        }
    }
    i915_write(&i915->hw, GEN7_ERR_INT, e);
}

/** The interrupt handler of the display engine's pipe events: kept for the
 * deferred work, and the error interrupt taken at once, under the
 * interrupt mask's lock. */
static void intel_de_irq(struct drm_i915_private *i915, uint32_t iir, void *data)
{
    (void)data;
    if (iir & DE_ERR_INT_IVB) {
        err_int(i915);
        iir &= ~DE_ERR_INT_IVB;
    }
    i915->display.de_iir |= iir;
}

void intel_fifo_underrun_reporting(struct intel_crtc *c, int on)
{
    struct drm_i915_private *i915 = c->i915;
    uint32_t flags = spin_lock_irqsave(&i915->irq.lock);
    int old = !c->underrun_off, all = 1, pipe;

    c->underrun_off = on ? 0 : 1;
    for (pipe = 0; pipe < I915_NUM_PIPES; pipe++) {
        all = all && !i915->display.crtc[pipe].underrun_off;
    }
    spin_unlock_irqrestore(&i915->irq.lock, flags);
    if (on) {
        i915_write(&i915->hw, GEN7_ERR_INT, ERR_INT_FIFO_UNDERRUN(c->pipe));
        if (all) {
            i915_irq_unmask(i915, I915_IRQ_DE, DE_ERR_INT_IVB);
        }
        return;
    }
    i915_irq_mask(i915, I915_IRQ_DE, DE_ERR_INT_IVB);
    if (old && (i915_read(&i915->hw, GEN7_ERR_INT) & ERR_INT_FIFO_UNDERRUN(c->pipe))) {
        klog(KLOG_ERR, "i915", "display: uncleared FIFO underrun on pipe %c", 'A' + c->pipe);
    }
}

void intel_display_irq_work(struct drm_i915_private *i915)
{
    struct intel_display *d = &i915->display;
    uint32_t flags, iir, underruns, crc_done, crc[I915_NUM_PIPES];
    int pipe;

    flags = spin_lock_irqsave(&i915->irq.lock);
    iir = d->de_iir;
    d->de_iir = 0;
    underruns = d->underruns;
    d->underruns = 0;
    crc_done = d->crc_done;
    d->crc_done = 0;
    for (pipe = 0; pipe < I915_NUM_PIPES; pipe++) {
        crc[pipe] = d->crtc[pipe].crc_res;
    }
    spin_unlock_irqrestore(&i915->irq.lock, flags);
    for (pipe = 0; pipe < I915_NUM_PIPES; pipe++) {
        if (underruns & (1u << pipe)) {
            klog(KLOG_ERR, "i915", "display: CPU pipe %c FIFO underrun", 'A' + pipe);
        }
    }
    if (iir == 0 && crc_done == 0) {
        return;
    }
    flags = kirq_save();
    for (pipe = 0; pipe < I915_NUM_PIPES; pipe++) {
        struct intel_crtc *c = &d->crtc[pipe];

        if (iir & DE_PIPE_VBLANK_IVB(pipe)) {
            (void)drm_crtc_handle_vblank(&c->base);
            intel_wm_vblank(c);
            vblank_boost(c, 1);
            /* The writes latch at the vblank that ends the frame they were
             * made in, so the event goes once the counter has moved past
             * the frame read after them. */
            if (c->event && !c->event_async &&
                intel_get_vblank_counter(&c->base) != c->event_frame) {
                intel_crtc_send_event(c);
            }
            /* A flip whose event the core sent ends here too. */
            intel_crtc_interactive_end(c);
        }
        if ((iir & DE_PLANE_FLIP_DONE_IVB(pipe)) && c->event && c->event_async) {
            intel_crtc_send_event(c);
        }
        if (crc_done & (1u << pipe)) {
            intel_crtc_crc_frame(c, crc[pipe]);
        }
    }
    kirq_restore(flags);
}

int32_t intel_vblank_init(struct drm_i915_private *i915)
{
    int32_t err = i915_irq_register(i915, I915_IRQ_DE, de_bits(), intel_de_irq, NULL);

    if (err == 0) {
        i915->irq.deferred_display = intel_display_irq_work;
    }
    return err;
}

void intel_vblank_fini(struct drm_i915_private *i915)
{
    uint32_t flags;
    int pipe;

    i915_irq_unregister(i915, I915_IRQ_DE, de_bits());
    i915->irq.deferred_display = 0;
    flags = kirq_save();
    for (pipe = 0; pipe < I915_NUM_PIPES; pipe++) {
        intel_crtc_send_event(&i915->display.crtc[pipe]);
        vblank_boost(&i915->display.crtc[pipe], 0);
    }
    kirq_restore(flags);
}
