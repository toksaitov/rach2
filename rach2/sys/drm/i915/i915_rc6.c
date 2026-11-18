/*
 * i915_rc6.c: RC6, the Haswell GT's sleep while it is idle, and the GT's
 * park and unpark, which move RC6 and RPS between work and idle, after
 * Linux 6.12's gt/intel_rc6.c and gt/intel_gt_pm.c.
 */

#include "i915.h"
#include "i915_reg.h"

#include "../../timer.h"

/* The engines' register bases in Linux's order of engine ids, with the
 * platform bit of each. */
static const struct {
    uint32_t engine;
    uint32_t base;
} rc6_engines[] = {{I915_ENGINE_RCS0, RENDER_RING_BASE},
                   {I915_ENGINE_BCS0, BLT_RING_BASE},
                   {I915_ENGINE_VCS0, GEN6_BSD_RING_BASE},
                   {I915_ENGINE_VECS0, VEBOX_RING_BASE}};

/* GEN6_RC_STATE's software targets: RC6, and the deeper RC6p. */
#define RC6_TARGET_RC6  0x4u
#define RC6_TARGET_RC6P 0x5u

/** Whether the bound part has RC6p, as Linux's HAS_RC6p. */
static int rc6_has_rc6p(const struct drm_i915_private *i915)
{
    return i915->id && i915->id->platform->has_rc6p;
}

/**
 * Clear GEN6_RC_CONTROL and GEN6_RC_STATE under forcewake, as Linux's
 * __intel_rc6_disable. A forcewake that does not come is logged by
 * forcewake, and the writes go ahead, as on Linux.
 */
static void rc6_disable_hw(struct drm_i915_private *i915)
{
    int32_t fw = i915_forcewake_get(i915);

    i915_write(&i915->hw, GEN6_RC_CONTROL, 0);
    i915_write(&i915->hw, GEN6_RC_STATE, 0);
    if (fw == 0) {
        i915_forcewake_put(i915);
    }
}

void i915_rc6_init(struct drm_i915_private *i915)
{
    struct i915_rc6 *rc6 = &i915->rc6;

    rc6->supported = rc6->enabled = rc6->manual = 0;
    rc6->ctl_enable = 0;
    rc6->prev_hw = 0;
    rc6->cur = 0;
    if (!i915->id || !i915->id->platform->has_rc6) {
        return;
    }
    rc6_disable_hw(i915);
    rc6->supported = 1;
}

void i915_rc6_enable(struct drm_i915_private *i915)
{
    struct i915_rc6 *rc6 = &i915->rc6;
    struct i915_hw *hw = &i915->hw;
    uint32_t mask, rc6vids = 0;
    int32_t fw;
    unsigned i;

    if (!rc6->supported || rc6->enabled) {
        return;
    }
    fw = i915_forcewake_get(i915);
    i915_write(hw, GEN6_RC1_WAKE_RATE_LIMIT, 1000u << 16);
    i915_write(hw, GEN6_RC6_WAKE_RATE_LIMIT, 40u << 16 | 30u);
    i915_write(hw, GEN6_RC6pp_WAKE_RATE_LIMIT, 30);
    i915_write(hw, GEN6_RC_EVALUATION_INTERVAL, 125000);
    i915_write(hw, GEN6_RC_IDLE_HYSTERSIS, 25);
    for (i = 0; i < sizeof(rc6_engines) / sizeof(rc6_engines[0]); i++) {
        if (i915->id->platform->engines & rc6_engines[i].engine) {
            i915_write(hw, RING_MAX_IDLE(rc6_engines[i].base), 10);
        }
    }
    i915_write(hw, GEN6_RC_SLEEP, 0);
    i915_write(hw, GEN6_RC1e_THRESHOLD, 1000);
    i915_write(hw, GEN6_RC6_THRESHOLD, 50000);
    i915_write(hw, GEN6_RC6p_THRESHOLD, 150000);
    i915_write(hw, GEN6_RC6pp_THRESHOLD, 64000); /* unused, as on Linux */

    mask = GEN6_RC_CTL_RC6_ENABLE;
    if (rc6_has_rc6p(i915)) {
        mask |= GEN6_RC_CTL_RC6p_ENABLE;
    }
    rc6->ctl_enable = mask | GEN6_RC_CTL_EI_MODE(1) | GEN6_RC_CTL_HW_ENABLE;
    /* Linux reads the voltages on every part and corrects them only on
     * graphics version 6. */
    (void)i915_pcode_read(i915, GEN6_PCODE_READ_RC6VIDS, &rc6vids, 0);
    rc6->manual = (rc6->ctl_enable & GEN6_RC_CTL_RC6_ENABLE) != 0;
    if (fw == 0) {
        i915_forcewake_put(i915);
    }
    rc6->enabled = 1;
}

void i915_rc6_disable(struct drm_i915_private *i915)
{
    if (!i915->rc6.enabled) {
        return;
    }
    i915->rc6.enabled = 0;
    rc6_disable_hw(i915);
}

void i915_rc6_park(struct drm_i915_private *i915)
{
    if (!i915->rc6.enabled || !i915->rc6.manual) {
        return;
    }
    i915_write(&i915->hw, GEN6_RC_CONTROL, GEN6_RC_CTL_RC6_ENABLE);
    i915_write(&i915->hw, GEN6_RC_STATE,
               (rc6_has_rc6p(i915) ? RC6_TARGET_RC6P : RC6_TARGET_RC6) << RC_SW_TARGET_STATE_SHIFT);
}

void i915_rc6_unpark(struct drm_i915_private *i915)
{
    if (!i915->rc6.enabled) {
        return;
    }
    i915_write(&i915->hw, GEN6_RC_CONTROL, i915->rc6.ctl_enable);
}

uint64_t i915_rc6_residency_ms(struct drm_i915_private *i915)
{
    struct i915_rc6 *rc6 = &i915->rc6;
    uint64_t units, us;
    uint32_t flags, hw;

    if (!rc6->supported) {
        return 0;
    }
    flags = spin_lock_irqsave(&i915->uncore.lock);
    hw = i915_read(&i915->hw, GEN6_GT_GFX_RC6);
    /* The difference modulo 2^32 is the units since the last read, a wrap
     * between them included. */
    rc6->cur += (uint32_t)(hw - rc6->prev_hw);
    rc6->prev_hw = hw;
    units = rc6->cur;
    spin_unlock_irqrestore(&i915->uncore.lock, flags);

    /* 1.28 us a unit: microseconds rounded up, then milliseconds to the
     * nearest, as Linux's intel_rc6_residency_us and get_residency. */
    us = tick_div(units * 32u + 24u, 25u, (uint32_t *)0);
    return tick_div(us + 500u, 1000u, (uint32_t *)0);
}

void i915_gt_pm_enable(struct drm_i915_private *i915)
{
    i915_rps_enable(i915);
    i915_rc6_enable(i915);
    i915_gt_park(i915);
}

void i915_gt_pm_disable(struct drm_i915_private *i915)
{
    i915_rps_disable(i915);
    i915_rc6_disable(i915);
}

void i915_gt_pm_restore(struct drm_i915_private *i915)
{
    i915_rps_enable(i915);
    i915_rc6_enable(i915);
    if (i915->gt.awake) {
        i915_gt_unpark(i915);
    } else {
        i915_gt_park(i915);
    }
}

void i915_gt_unpark(struct drm_i915_private *i915)
{
    i915_rc6_unpark(i915);
    i915_rps_unpark(i915);
}

void i915_gt_park(struct drm_i915_private *i915)
{
    i915_rps_park(i915);
    i915_rc6_park(i915);
}
