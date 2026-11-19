/*
 * intel_backlight.c: the eDP panel's backlight once i915 binds, after
 * Linux's display/intel_backlight.c for a Lynx Point PCH: the PWM that
 * intelbl drove at boot, now through the device's registers, as
 * /sys/class/backlight/intel_backlight, turned off and on with the panel's
 * pipe, and its enable line off while the pipe is off.
 */

#include "i915.h"
#include "i915_reg.h"

#include "../../display/backlight.h"
#include "../../klog.h"

/* The PWM's period and granularity (`bl_alt`) the driver last ran the panel
 * with, which intel_backlight_fini keeps when the driver lets the device go
 * and the next setup takes when it finds no period, as after the function's
 * reset of a handoff, so that the levels and the frequency stay the
 * firmware's. The VBT gives only the frequency, which in the PCH's step of
 * the moment may give another range. 0 for none. */
static int kept_max;
static int kept_alt;

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

/** `v` clamped to [`smin`, `smax`] and taken to [`tmin`, `tmax`], rounded
 * to the closest, as Linux's scale(); a PWM's levels fit 16 bits, so the
 * product fits 32. */
static int bl_scale(int v, int smin, int smax, int tmin, int tmax)
{
    uint32_t span = (uint32_t)(smax - smin);

    v = v < smin ? smin : v > smax ? smax : v;
    if (span == 0) {
        return tmin;
    }
    return tmin + (int)(((uint32_t)(v - smin) * (uint32_t)(tmax - tmin) + span / 2u) / span);
}

/** The backlight class's `get`, of the device at `ctx`: the PWM's level
 * while it is on, 0 while it is off, taken from the PWM's [`bl_min`, max]
 * to the class's [0, max], as Linux's intel_backlight_device_get_brightness
 * through scale_hw_to_user. */
static int bl_get(void *ctx)
{
    struct intel_display *d = &((struct drm_i915_private *)ctx)->display;
    uint32_t flags = spin_lock_irqsave(&d->bl_lock);
    int level =
        bl_scale(d->bl_enabled ? intelbl_get(&d->bl) : 0, d->bl_min, d->bl.max, 0, d->bl.max);

    spin_unlock_irqrestore(&d->bl_lock, flags);
    return level;
}

/** The backlight class's `set`: the class's level taken to the PWM's
 * [`bl_min`, max], kept, and shown while the PWM is on, as Linux's
 * intel_panel_set_backlight through scale_user_to_hw. Level 0 is dark: the
 * switch turns the backlight off, as Linux's update_status does, and with
 * no switch the PWM's duty is 0. */
static void bl_set(void *ctx, int level)
{
    struct intel_display *d = &((struct drm_i915_private *)ctx)->display;
    uint32_t flags = spin_lock_irqsave(&d->bl_lock);

    d->bl_dark = level <= 0;
    d->bl_level = bl_scale(level, 0, d->bl.max, d->bl_min, d->bl.max);
    if (d->bl_enabled) {
        intelbl_set(&d->bl, d->bl_dark && !d->bl.switched ? 0 : d->bl_level);
    }
    spin_unlock_irqrestore(&d->bl_lock, flags);
}

/** The backlight class's `power`: what it asks is kept, and holds while
 * the panel's pipe is on and its level is not 0, as Linux's
 * intel_backlight_enable runs from the encoder's enable. */
static void bl_power(void *ctx, int on)
{
    struct intel_display *d = &((struct drm_i915_private *)ctx)->display;
    uint32_t flags = spin_lock_irqsave(&d->bl_lock);

    d->bl_want_on = on != 0;
    /* The switch is off with the pipe, and the pipe's enable takes what
     * was asked; the device may sleep meanwhile. */
    if (d->bl_pipe_on) {
        intelbl_power(&d->bl, on && !d->bl_dark);
    }
    spin_unlock_irqrestore(&d->bl_lock, flags);
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

/** The PWM's period in its clock's steps for `hz`, Linux's
 * lpt_hz_to_pwm: the PCH's clock is 24 MHz on Lynx Point LP, the ULT's,
 * and 135 MHz on Lynx Point H. */
static uint32_t lpt_hz_to_pwm(struct drm_i915_private *i915, uint32_t hz)
{
    uint32_t mul = LPT_PWM_STEP(i915->display.bl_alt ? LPT_PWM_GRANULARITY : 0);
    uint32_t clock = LPT_PWM_CLOCK_HZ(i915->id && i915->id->ult);

    return (clock + hz * mul / 2u) / (hz * mul);
}

/** Set the PWM up when the firmware left it off, as Linux's
 * lpt_setup_backlight does: the period the firmware wrote; else the period
 * and granularity an earlier bind ran with (`kept_max`), which Linux does
 * not keep; else the VBT's frequency's, else 200 Hz's. 0, or -1 with the
 * reason in `*why`. */
static int pwm_off_setup(struct drm_i915_private *i915, const char **why)
{
    struct intel_display *d = &i915->display;
    uint32_t ctl1 = bl_rd(i915, BLC_PWM_PCH_CTL1), max = bl_rd(i915, BLC_PWM_PCH_CTL2) >> 16;

    if (max == 0 && kept_max > 0) {
        max = (uint32_t)kept_max;
        d->bl_alt = kept_alt;
    } else if (max == 0) {
        max = lpt_hz_to_pwm(i915, d->dp.vbt.bl_pwm_freq_hz ? d->dp.vbt.bl_pwm_freq_hz : 200u);
    }
    if (max == 0 || max > BACKLIGHT_DUTY_CYCLE_MASK) {
        *why = "the PWM has no period";
        return -1;
    }
    d->bl.max = (int)max;
    d->bl.inverted = (ctl1 & BLM_PCH_POLARITY) != 0;
    d->bl.switched = 0;
    return 0;
}

void intel_backlight_setup(struct drm_i915_private *i915)
{
    struct intel_display *d = &i915->display;
    const struct backlight_ops *ops;
    const char *why;
    uint32_t min;

    d->bl.rd = bl_rd;
    d->bl.wr = bl_wr;
    d->bl.io = i915;
    d->bl_alt = (bl_rd(i915, SOUTH_CHICKEN2) & LPT_PWM_GRANULARITY) != 0;
    d->bl_enabled = (bl_rd(i915, BLC_PWM_PCH_CTL1) & BLM_PCH_PWM_ENABLE) != 0;
    if (d->bl_enabled ? intelbl_attach(&d->bl, &why) != 0 : pwm_off_setup(i915, &why) != 0) {
        klog(KLOG_INFO, "i915", "backlight: none: %s", why);
        d->bl_enabled = 0;
        return;
    }
    /* Linux's get_backlight_min_vbt: the VBT's least of 255, at most 64
     * of it, lest a table of 255 leave no range. */
    min = d->dp.vbt.bl_min_brightness > 64 ? 64u : d->dp.vbt.bl_min_brightness;
    d->bl_min = (int)((min * (uint32_t)d->bl.max + 127u) / 255u);
    d->bl_level = intelbl_get(&d->bl);
    if (d->bl_enabled) {
        intelbl_attach_switch(&d->bl);
    }
    d->bl_pipe_on = d->pps.panel_on;
    d->bl_want_on = d->pps.backlight_on;
    ops = d->bl.switched ? &bl_ops : &bl_ops_noswitch;
    d->bl_index = backlight_rebind("intel_backlight", d->bl.max, ops, i915);
    if (d->bl_index < 0) {
        d->bl_index = backlight_register("intel_backlight", d->bl.max, ops, i915);
    }
    if (d->bl_index < 0) {
        return;
    }
    klog(KLOG_INFO, "i915", "backlight: intel_backlight: levels 0 to %d, at %d%s%s", d->bl.max,
         bl_get(i915), d->bl_enabled ? "" : ", off", d->bl.switched ? ", off with the pipe" : "");
}

void intel_backlight_enable(struct drm_i915_private *i915)
{
    struct intel_display *d = &i915->display;
    uint32_t ctl1, flags;
    int raised = 0;

    if (d->bl_index < 0) {
        return;
    }
    flags = spin_lock_irqsave(&d->bl_lock);
    /* Linux's __intel_backlight_enable: a level at or below the least
     * comes on at the most, and lit. Any other level is kept. */
    if (d->bl_level <= d->bl_min) {
        d->bl_level = d->bl.max;
        d->bl_dark = 0;
        raised = 1;
    }
    /* Linux's lpt_enable_backlight: a PWM left on is turned off first. */
    ctl1 = bl_rd(i915, BLC_PWM_PCH_CTL1);
    if (ctl1 & BLM_PCH_PWM_ENABLE) {
        bl_wr(i915, BLC_PWM_PCH_CTL1, ctl1 & ~BLM_PCH_PWM_ENABLE);
    }
    i915_rmw(&i915->hw, SOUTH_CHICKEN2, LPT_PWM_GRANULARITY, d->bl_alt ? LPT_PWM_GRANULARITY : 0);
    bl_wr(i915, BLC_PWM_PCH_CTL2, (uint32_t)d->bl.max << 16);
    /* After Lynx Point the override is the default. */
    ctl1 = BLM_PCH_OVERRIDE_ENABLE | (d->bl.inverted ? BLM_PCH_POLARITY : 0);
    bl_wr(i915, BLC_PWM_PCH_CTL1, ctl1);
    i915_posting_read(&i915->hw, BLC_PWM_PCH_CTL1);
    bl_wr(i915, BLC_PWM_PCH_CTL1, ctl1 | BLM_PCH_PWM_ENABLE);
    /* The level sticks only once the PWM is on. */
    intelbl_set(&d->bl, d->bl_level);
    d->bl_enabled = 1;
    spin_unlock_irqrestore(&d->bl_lock, flags);
    /* As Linux sets props.brightness, so that the class shows the level
     * lit and a later write or unblank does not darken it again. */
    if (raised) {
        backlight_driver_level(
            d->bl_index, d->bl.max,
            "raised to the most at the panel's enable from at or below its least");
    }
}

void intel_backlight_disable(struct drm_i915_private *i915)
{
    struct intel_display *d = &i915->display;
    uint32_t cpu, flags;

    if (d->bl_index < 0) {
        return;
    }
    flags = spin_lock_irqsave(&d->bl_lock);
    d->bl_enabled = 0;
    /* Linux's lpt_disable_backlight: duty cycle 0, the processor's PWM off
     * should the firmware have left it on, then the PCH's. */
    i915_rmw(&i915->hw, BLC_PWM_PCH_CTL2, BACKLIGHT_DUTY_CYCLE_MASK, 0);
    cpu = bl_rd(i915, BLC_PWM_CPU_CTL2);
    if (cpu & BLM_PWM_ENABLE) {
        bl_wr(i915, BLC_PWM_CPU_CTL2, cpu & ~BLM_PWM_ENABLE);
    }
    i915_rmw(&i915->hw, BLC_PWM_PCH_CTL1, BLM_PCH_PWM_ENABLE, 0);
    spin_unlock_irqrestore(&d->bl_lock, flags);
}

void intel_backlight_pipe(struct drm_i915_private *i915, int on)
{
    struct intel_display *d = &i915->display;
    uint32_t flags = spin_lock_irqsave(&d->bl_lock);

    d->bl_pipe_on = on != 0;
    if (d->bl_index >= 0 && d->bl.switched) {
        intelbl_power(&d->bl, on && d->bl_want_on && !d->bl_dark);
    } else {
        /* No class switch to ask: the enable line follows the pipe, as
         * Linux's intel_pps_backlight_on and _off set it. */
        i915_rmw(&i915->hw, PP_CONTROL, PANEL_UNLOCK_MASK | EDP_BLC_ENABLE,
                 PANEL_UNLOCK_REGS | (on ? EDP_BLC_ENABLE : 0));
    }
    i915_posting_read(&i915->hw, PP_CONTROL);
    spin_unlock_irqrestore(&d->bl_lock, flags);
}

void intel_backlight_fini(struct drm_i915_private *i915)
{
    struct intel_display *d = &i915->display;

    if (d->bl_index < 0) {
        return;
    }
    kept_max = d->bl.max;
    kept_alt = d->bl_alt;
    if (intelbl_take_back() != 0) {
        (void)backlight_rebind("intel_backlight", d->bl.max, &gone_ops, NULL);
    }
    d->bl_index = -1;
}
