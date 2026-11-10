/*
 * intel_vblank.c: the vblanks of Haswell's pipes and the events of the
 * commits on them, after Linux's display/intel_vblank.c and the display
 * engine's interrupts of i915_irq.c: the frame counter, the vblank and
 * flip done interrupts, and a commit's event sent at the vblank that shows
 * it.
 */

#include "i915.h"
#include "i915_reg.h"

#include "../../lib/kerrno.h"
#include "../../lib/kirq.h"

/*
 * The interrupt handler takes the pipe's events under the interrupt mask's
 * lock and leaves them in `display.de_iir`; the deferred work then runs
 * the DRM core's vblank code, which may mask the vblank again through
 * `intel_disable_vblank`, a call that takes that lock. Process context
 * reaches the same state, the events and the vblank counts, with
 * interrupts off.
 */

/** The display engine's events of each pipe that the driver takes. */
static uint32_t de_bits(void)
{
    uint32_t bits = 0;
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

void intel_crtc_send_event(struct intel_crtc *c)
{
    struct drm_pending_event *e = c->event;

    if (!e) {
        return;
    }
    c->event = NULL;
    if (c->event_async) {
        i915_irq_mask(c->i915, I915_IRQ_DE, DE_PLANE_FLIP_DONE_IVB(c->pipe));
        c->event_async = 0;
    }
    drm_crtc_send_vblank_event(&c->base, e);
    drm_crtc_vblank_put(&c->base);
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
    }
    kirq_restore(flags);
}

/** The interrupt handler of the display engine's pipe events: kept for the
 * deferred work, under the interrupt mask's lock. */
static void intel_de_irq(struct drm_i915_private *i915, uint32_t iir, void *data)
{
    (void)data;
    i915->display.de_iir |= iir;
}

void intel_display_irq_work(struct drm_i915_private *i915)
{
    struct intel_display *d = &i915->display;
    uint32_t flags, iir;
    int pipe;

    flags = spin_lock_irqsave(&i915->irq.lock);
    iir = d->de_iir;
    d->de_iir = 0;
    spin_unlock_irqrestore(&i915->irq.lock, flags);
    if (iir == 0) {
        return;
    }
    flags = kirq_save();
    for (pipe = 0; pipe < I915_NUM_PIPES; pipe++) {
        struct intel_crtc *c = &d->crtc[pipe];

        if (iir & DE_PIPE_VBLANK_IVB(pipe)) {
            (void)drm_crtc_handle_vblank(&c->base);
            intel_crtc_crc_frame(c);
            intel_wm_vblank(c);
            /* The writes latch at the vblank that ends the frame they were
             * made in, so the event goes once the counter has moved past
             * the frame read after them. */
            if (c->event && !c->event_async &&
                intel_get_vblank_counter(&c->base) != c->event_frame) {
                intel_crtc_send_event(c);
            }
        }
        if ((iir & DE_PLANE_FLIP_DONE_IVB(pipe)) && c->event && c->event_async) {
            intel_crtc_send_event(c);
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
    }
    kirq_restore(flags);
}
