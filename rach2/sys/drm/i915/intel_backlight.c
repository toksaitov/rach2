/*
 * intel_backlight.c: the eDP panel's backlight once i915 binds, after
 * Linux's display/intel_backlight.c for a Lynx Point PCH: the PWM that
 * intelbl drove at boot, now through the device's registers, as
 * /sys/class/backlight/intel_backlight, and its enable line off while the
 * panel's pipe is off.
 */

#include "i915.h"

#include "../../display/backlight.h"
#include "../../klog.h"

/** A register read for the PWM code, through the device at `io`. */
static uint32_t bl_rd(void *io, uint32_t reg)
{
    return i915_read(&((struct drm_i915_private *)io)->hw, reg);
}

/** A register write for the PWM code. */
static void bl_wr(void *io, uint32_t reg, uint32_t value)
{
    i915_write(&((struct drm_i915_private *)io)->hw, reg, value);
}

/** The backlight class's `get`, of the device at `ctx`. */
static int bl_get(void *ctx)
{
    return intelbl_get(&((struct drm_i915_private *)ctx)->display.bl);
}

/** The backlight class's `set`. */
static void bl_set(void *ctx, int level)
{
    intelbl_set(&((struct drm_i915_private *)ctx)->display.bl, level);
}

/** The backlight class's `power`: what it asks is kept, and holds while
 * the panel's pipe is on, as Linux's intel_backlight_enable runs from the
 * encoder's enable. */
static void bl_power(void *ctx, int on)
{
    struct intel_display *d = &((struct drm_i915_private *)ctx)->display;

    d->bl_want_on = on != 0;
    intelbl_power(&d->bl, on && d->bl_pipe_on);
}

static const struct backlight_ops bl_ops = {bl_get, bl_set, bl_power};
static const struct backlight_ops bl_ops_noswitch = {bl_get, bl_set, NULL};

/** A backlight whose driver is gone: level 0, and nothing written. */
static int gone_get(void *ctx)
{
    (void)ctx;
    return 0;
}

/** Nothing to set on a backlight whose driver is gone. */
static void gone_set(void *ctx, int level)
{
    (void)ctx;
    (void)level;
}

static const struct backlight_ops gone_ops = {gone_get, gone_set, NULL};

void intel_backlight_setup(struct drm_i915_private *i915)
{
    struct intel_display *d = &i915->display;
    const struct backlight_ops *ops;
    const char *why;

    d->bl.rd = bl_rd;
    d->bl.wr = bl_wr;
    d->bl.io = i915;
    if (intelbl_attach(&d->bl, &why) != 0) {
        klog(KLOG_INFO, "i915", "backlight: none: %s", why);
        return;
    }
    intelbl_attach_switch(&d->bl);
    d->bl_pipe_on = 1;
    d->bl_want_on = d->pps.backlight_on;
    ops = d->bl.switched ? &bl_ops : &bl_ops_noswitch;
    d->bl_index = backlight_rebind("intel_backlight", d->bl.max, ops, i915);
    if (d->bl_index < 0) {
        d->bl_index = backlight_register("intel_backlight", d->bl.max, ops, i915);
    }
    if (d->bl_index < 0) {
        return;
    }
    klog(KLOG_INFO, "i915", "backlight: intel_backlight: levels 0 to %d, at %d%s", d->bl.max,
         intelbl_get(&d->bl), d->bl.switched ? ", off with the pipe" : "");
}

void intel_backlight_pipe(struct drm_i915_private *i915, int on)
{
    struct intel_display *d = &i915->display;

    d->bl_pipe_on = on != 0;
    if (d->bl_index >= 0 && d->bl.switched) {
        intelbl_power(&d->bl, on && d->bl_want_on);
    }
}

void intel_backlight_fini(struct drm_i915_private *i915)
{
    struct intel_display *d = &i915->display;

    if (d->bl_index < 0) {
        return;
    }
    if (intelbl_take_back() != 0) {
        (void)backlight_rebind("intel_backlight", d->bl.max, &gone_ops, NULL);
    }
    d->bl_index = -1;
}
