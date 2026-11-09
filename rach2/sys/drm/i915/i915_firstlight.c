/*
 * i915_firstlight.c: first light, the driver's first work for the GPU: one
 * MI_STORE_DWORD_IMM from the render ring to memory, read back by the
 * processor, before any display code runs.
 */

#include "i915.h"
#include "i915_reg.h"

#include "../../cmdline.h"
#include "../../klog.h"
#include "../../lib/kerrno.h"

/* The value the render ring writes: any that a zeroed page cannot hold. */
#define FIRSTLIGHT_VALUE 0x1915f157u

/* How long the breadcrumb and then the interrupt may take: far more than
 * the microseconds an idle ring needs, and short enough to spin through
 * at boot. */
#define FIRSTLIGHT_BREADCRUMB_US 50000u
#define FIRSTLIGHT_IRQ_US        10000u

/* What first light's waits read. */
struct firstlight {
    struct drm_i915_private *i915;
    struct i915_engine *e;
    uint32_t seqno;
    uint32_t irq_seen;
};

/** Whether the render ring has written the breadcrumb. */
static int firstlight_breadcrumb(struct i915_hw *hw, void *arg)
{
    struct firstlight *f = (struct firstlight *)arg;

    (void)hw;
    return i915_engine_passed(f->e, f->seqno);
}

/** Whether the user interrupt was taken, or is latched in the GT's
 * identity register for a processor that has its interrupts off. */
static int firstlight_irq(struct i915_hw *hw, void *arg)
{
    struct firstlight *f = (struct firstlight *)arg;

    return f->e->irq_seen != f->irq_seen || (i915_read(hw, GTIIR) & f->e->irq_bit) != 0;
}

/** Log first light's failure `why` and return `err`. */
static int32_t firstlight_fail(struct drm_i915_private *i915, struct i915_engine *e,
                               const char *why, int32_t err)
{
    klog(KLOG_ERR, "i915", "first light: %s: head %08x tail %08x acthd %08x ipehr %08x", why,
         (unsigned)i915_uncore_read(i915, RING_HEAD(e->mmio_base)),
         (unsigned)i915_uncore_read(i915, RING_TAIL(e->mmio_base)),
         (unsigned)i915_uncore_read(i915, RING_ACTHD(e->mmio_base)),
         (unsigned)i915_uncore_read(i915, RING_IPEHR(e->mmio_base)));
    return err;
}

int32_t i915_firstlight(struct drm_i915_private *i915)
{
    struct i915_engine *e = &i915->gt.engine[I915_RCS0];
    struct firstlight f;
    uint32_t *page;
    uint32_t ggtt, *cs, got;
    int32_t err = 0;

    if (!kcmdline_has("rach2.i915.firstlight")) {
        return -ENODEV;
    }
    if (!e->running) {
        klog(KLOG_ERR, "i915", "first light: the render ring is not running");
        return -EIO;
    }
    page = (uint32_t *)i915_gt_pages_map(i915, 1, I915_CACHE_LLC, &ggtt);
    if (!page) {
        klog(KLOG_ERR, "i915", "first light: no page for the value");
        return -ENOMEM;
    }
    f.i915 = i915;
    f.e = e;
    f.irq_seen = e->irq_seen;
    i915_engine_irq_get(i915, e);

    cs = i915_ring_begin(e, i915, 4);
    if (!cs) {
        err = firstlight_fail(i915, e, "no room in the ring", -EIO);
        goto out;
    }
    cs[0] = MI_STORE_DWORD_IMM_GEN4 | MI_USE_GGTT;
    cs[1] = 0;
    cs[2] = ggtt;
    cs[3] = FIRSTLIGHT_VALUE;
    i915_ring_advance(e, i915);
    f.seqno = i915_engine_emit_breadcrumb(i915, e);
    if (f.seqno == 0) {
        err = firstlight_fail(i915, e, "no room for the breadcrumb", -EIO);
        goto out;
    }

    if (i915_wait_until(&i915->hw, "first light breadcrumb", firstlight_breadcrumb, &f,
                        FIRSTLIGHT_BREADCRUMB_US) < 0) {
        err = firstlight_fail(i915, e, "no breadcrumb", -ETIME);
        goto out;
    }
    got = *(volatile uint32_t *)page;
    if (got != FIRSTLIGHT_VALUE) {
        klog(KLOG_ERR, "i915", "first light: read back 0x%08x at 0x%x, wrote 0x%08x", (unsigned)got,
             (unsigned)ggtt, (unsigned)FIRSTLIGHT_VALUE);
        err = -EIO;
        goto out;
    }
    if (i915_wait_until(&i915->hw, "first light interrupt", firstlight_irq, &f, FIRSTLIGHT_IRQ_US) <
        0) {
        err = firstlight_fail(i915, e, "no user interrupt", -ETIME);
        goto out;
    }
    klog(KLOG_INFO, "i915",
         "first light: ok: %s wrote 0x%08x at 0x%x, breadcrumb %u, user "
         "interrupt %s",
         e->name, (unsigned)got, (unsigned)ggtt, (unsigned)f.seqno,
         e->irq_seen != f.irq_seen ? "taken" : "latched");
out:
    i915_engine_irq_put(i915, e);
    i915_gt_pages_unmap(i915, page, 1, ggtt);
    return err;
}
