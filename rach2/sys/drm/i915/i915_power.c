/*
 * i915_power.c: Haswell's display power well, which powers pipes B and C,
 * transcoders A to C, the panel fitters and the DDI lanes of ports B to
 * E, after Linux 6.12's hsw_power_well_ops
 * (display/intel_display_power_well.c).
 */

#include "i915.h"
#include "i915_reg.h"

#include "../../klog.h"
#include "../../lib/kerrno.h"
#include "../../udelay.h"

/* The well's request and state bits in each of its four registers. */
#define I915_PW_REQ   HSW_PWR_WELL_CTL_REQ(HSW_PW_CTL_IDX_GLOBAL)
#define I915_PW_STATE HSW_PWR_WELL_CTL_STATE(HSW_PW_CTL_IDX_GLOBAL)

/* Linux's bound on the well's change of state: 1 ms. */
#define I915_PW_US 1000u

int i915_power_well_on(struct drm_i915_private *i915)
{
    return (i915_read(&i915->hw, HSW_PWR_WELL_CTL2) & I915_PW_STATE) != 0;
}

int i915_power_well_in_use(struct drm_i915_private *i915)
{
    struct i915_hw *hw = &i915->hw;
    uint32_t edp;
    int i;

    /* Nothing in the well runs while it is off, and its registers then read
     * as unclaimed. */
    if (!i915_power_well_on(i915)) {
        return 0;
    }
    for (i = I915_PIPE_A; i <= I915_PIPE_C; i++) {
        if ((i915_read(hw, TRANSCONF(i)) & TRANSCONF_ENABLE) ||
            (i915_read(hw, PF_CTL(i)) & PF_ENABLE)) {
            return 1;
        }
        if (i != I915_PIPE_A && (i915_read(hw, DSPCNTR(i)) & DISP_ENABLE)) {
            return 1;
        }
    }
    /* The eDP transcoder is outside the well, but an input other than
     * pipe A always on goes through pipe A's panel fitter or pipe B or C,
     * which are in it (Linux's force_thru). */
    edp = i915_read(hw, TRANS_DDI_FUNC_CTL(I915_TRANS_EDP));
    return (edp & TRANS_DDI_FUNC_ENABLE) &&
           (edp & TRANS_DDI_EDP_INPUT_MASK) != TRANS_DDI_EDP_INPUT_A_ON;
}

/** Set the driver's request and wait for the well's state: 0, or
 * -ETIMEDOUT. */
static int32_t i915_power_enable(struct drm_i915_private *i915)
{
    int was_on = i915_power_well_on(i915);
    int32_t err;

    i915_rmw(&i915->hw, HSW_PWR_WELL_CTL2, 0, I915_PW_REQ);
    err = i915_wait_for(&i915->hw, "power well on", HSW_PWR_WELL_CTL2, I915_PW_STATE, I915_PW_STATE,
                        I915_PW_US);
    if (err == 0 && !was_on) {
        i915_hw_vga_reset_io_mem(&i915->hw);
    }
    return err;
}

/** The requests other than the driver's: bit 0 the BIOS's, 2 the KVMR's,
 * 3 the debug register's, as Linux's hsw_power_well_requesters. */
static uint32_t i915_power_requesters(struct i915_hw *hw)
{
    return ((i915_read(hw, HSW_PWR_WELL_CTL1) & I915_PW_REQ) ? 1u : 0u) |
           ((i915_read(hw, HSW_PWR_WELL_CTL3) & I915_PW_REQ) ? 4u : 0u) |
           ((i915_read(hw, HSW_PWR_WELL_CTL4) & I915_PW_REQ) ? 8u : 0u);
}

/** The wait's condition: the well off, or held on by another request. */
static int i915_power_off_or_held(struct i915_hw *hw, void *arg)
{
    (void)arg;
    return !(i915_read(hw, HSW_PWR_WELL_CTL2) & I915_PW_STATE) || i915_power_requesters(hw) != 0;
}

/** Clear the driver's request and wait for the well to go off, unless
 * another request holds it on, which is logged, as Linux does. */
static void i915_power_disable(struct drm_i915_private *i915)
{
    struct i915_hw *hw = &i915->hw;
    uint32_t reqs;

    i915_rmw(hw, HSW_PWR_WELL_CTL2, I915_PW_REQ, 0);
    (void)i915_wait_until(hw, "power well off", i915_power_off_or_held, 0, I915_PW_US);
    if (!i915_power_well_on(i915)) {
        return;
    }
    reqs = i915_power_requesters(hw);
    klog(KLOG_INFO, "i915", "power: display well forced on: bios %d, kvmr %d, debug %d",
         (int)(reqs & 1u), (int)((reqs >> 2) & 1u), (int)((reqs >> 3) & 1u));
}

int32_t i915_power_get(struct drm_i915_private *i915)
{
    struct i915_power *p = &i915->power;
    int32_t err;

    if (p->count == 0) {
        err = i915_power_enable(i915);
        if (err < 0) {
            return err;
        }
        p->on = 1;
        p->kept = 0;
    }
    p->count++;
    return 0;
}

void i915_power_put(struct drm_i915_private *i915)
{
    struct i915_power *p = &i915->power;

    if (p->count == 0) {
        klog(KLOG_ERR, "i915", "power: put with no reference held");
        return;
    }
    if (--p->count > 0) {
        return;
    }
    if (i915_power_well_in_use(i915)) {
        p->kept = 1;
        klog(KLOG_INFO, "i915", "power: display well kept on: a pipe it powers runs");
        return;
    }
    i915_power_disable(i915);
    p->on = 0;
}

int32_t i915_power_init(struct drm_i915_private *i915)
{
    struct i915_hw *hw = &i915->hw;
    uint32_t bios = i915_read(hw, HSW_PWR_WELL_CTL1), drv;
    int32_t err;

    /* hsw_power_well_sync_hw: the BIOS's request becomes the driver's. */
    if (bios & I915_PW_REQ) {
        drv = i915_read(hw, HSW_PWR_WELL_CTL2);
        if (!(drv & I915_PW_REQ)) {
            i915_write(hw, HSW_PWR_WELL_CTL2, drv | I915_PW_REQ);
        }
        i915_write(hw, HSW_PWR_WELL_CTL1, bios & ~I915_PW_REQ);
    }
    i915->power.count = 0;
    i915->power.on = (uint8_t)i915_power_well_on(i915);
    i915->power.kept = 0;
    err = i915_power_get(i915);
    i915->power.init_ref = err == 0;
    return err;
}

int32_t i915_power_sync_idle(struct drm_i915_private *i915)
{
    struct i915_power *p = &i915->power;

    if (p->count != 0) {
        return -EBUSY;
    }
    if (!i915_power_well_on(i915)) {
        p->on = 0;
        p->kept = 0;
        return 0;
    }
    if (i915_power_well_in_use(i915)) {
        return -EBUSY;
    }
    i915_power_disable(i915);
    p->on = 0;
    p->kept = 0;
    return i915_power_well_on(i915) ? -EBUSY : 0;
}
