/*
 * i915_pc8.c: package C8, the state a runtime suspend lets the package
 * reach, after Linux 6.12's hsw_enable_pc8 and hsw_disable_pc8
 * (display/intel_display_power.c), the PCH's reference clocks
 * (display/intel_pch_refclk.c) and its sideband (intel_sbi.c).
 */

#include "i915.h"
#include "i915_reg.h"

#include "../../klog.h"
#include "../../lib/kerrno.h"
#include "../../udelay.h"

/* Linux's bounds: the sideband's idle and its answer, 100 ms each
 * (intel_sbi.c); FCLK's switch, 1 us; LCPLL's lock lost, 1 ms, and won
 * back, 5 ms; D_COMP's compensation, 1 ms. */
#define I915_SBI_US       100000u
#define I915_FCLK_US      1u
#define I915_LCPLL_OFF_US 1000u
#define I915_LCPLL_ON_US  5000u
#define I915_RCOMP_US     1000u

/* The tries of a pcode write that found the mailbox taken. */
#define I915_DCOMP_TRIES 10

/**
 * Read (`write` 0) or write the PCH clock unit's register `reg` through the
 * sideband, Linux's intel_sbi_rw with SBI_ICLK: wait for the interface to
 * be idle, post the address, the data and the command, and wait for the
 * answer. 0; -EBUSY when the interface stayed busy, -ETIMEDOUT when the
 * command was not answered, -ENXIO when it failed. The callers hold the
 * runtime PM transition, which keeps one command at a time, as Linux's
 * sb_lock.
 */
static int32_t i915_sbi_rw(struct drm_i915_private *i915, uint32_t reg, uint32_t *val, int write)
{
    struct i915_hw *hw = &i915->hw;
    uint32_t cmd = SBI_CTL_DEST_ICLK | SBI_CTL_OP_CRRD;

    if (i915_wait_for(hw, "sbi idle", SBI_CTL_STAT, SBI_BUSY, 0, I915_SBI_US) < 0) {
        klog(KLOG_ERR, "i915", "pc8: sideband busy before register 0x%x", (unsigned)reg);
        return -EBUSY;
    }
    i915_write(hw, SBI_ADDR, reg << 16);
    i915_write(hw, SBI_DATA, write ? *val : 0);
    if (write) {
        cmd |= SBI_CTL_OP_WRITE;
    }
    i915_write(hw, SBI_CTL_STAT, cmd | SBI_BUSY);
    if (i915_wait_for(hw, "sbi answer", SBI_CTL_STAT, SBI_BUSY, 0, I915_SBI_US) < 0) {
        klog(KLOG_ERR, "i915", "pc8: sideband did not answer for register 0x%x", (unsigned)reg);
        return -ETIMEDOUT;
    }
    if (i915_read(hw, SBI_CTL_STAT) & SBI_RESPONSE_FAIL) {
        klog(KLOG_ERR, "i915", "pc8: sideband %s of register 0x%x failed", write ? "write" : "read",
             (unsigned)reg);
        return -ENXIO;
    }
    if (!write) {
        *val = i915_read(hw, SBI_DATA);
    }
    return 0;
}

/**
 * Turn CLKOUT_DP, the PCH's reference for FDI, off, Linux's
 * lpt_disable_clkout_dp: its buffer disabled, then its spread spectrum
 * path moved to the alternate and the path disabled. GEN0 on a low-power
 * PCH, DBUFF0 on the others.
 */
static void i915_lpt_disable_clkout_dp(struct drm_i915_private *i915)
{
    uint32_t reg = i915->id && (i915->id->ult || i915->id->ulx) ? SBI_GEN0 : SBI_DBUFF0;
    uint32_t v;

    if (i915_sbi_rw(i915, reg, &v, 0) < 0) {
        return;
    }
    v &= ~SBI_GEN0_CFG_BUFFENABLE_DISABLE;
    if (i915_sbi_rw(i915, reg, &v, 1) < 0 || i915_sbi_rw(i915, SBI_SSCCTL, &v, 0) < 0) {
        return;
    }
    if (v & SBI_SSCCTL_DISABLE) {
        return;
    }
    if (!(v & SBI_SSCCTL_PATHALT)) {
        v |= SBI_SSCCTL_PATHALT;
        if (i915_sbi_rw(i915, SBI_SSCCTL, &v, 1) < 0) {
            return;
        }
        k_udelay(32);
    }
    v |= SBI_SSCCTL_DISABLE;
    (void)i915_sbi_rw(i915, SBI_SSCCTL, &v, 1);
}

/** Whether the SPLL runs from the PCH's SSC reference, Linux's
 * spll_uses_pch_ssc on Haswell. */
static int i915_spll_uses_pch_ssc(struct i915_hw *hw)
{
    uint32_t ctl = i915_read(hw, SPLL_CTL);

    return (ctl & SPLL_PLL_ENABLE) && (ctl & SPLL_REF_MASK) == SPLL_REF_MUXED_SSC &&
           !(i915_read(hw, FUSE_STRAP) & HSW_CPU_SSC_ENABLE);
}

/** Whether WRPLL `pll`, 0 or 1, runs from the PCH's SSC reference, Linux's
 * wrpll_uses_pch_ssc. */
static int i915_wrpll_uses_pch_ssc(struct drm_i915_private *i915, int pll)
{
    uint32_t ctl = i915_read(&i915->hw, WRPLL_CTL(pll));

    if (!(ctl & WRPLL_PLL_ENABLE)) {
        return 0;
    }
    if ((ctl & WRPLL_REF_MASK) == WRPLL_REF_PCH_SSC) {
        return 1;
    }
    return i915->id && i915->id->ult && (ctl & WRPLL_REF_MASK) == WRPLL_REF_MUXED_SSC_BDW &&
           !(i915_read(&i915->hw, FUSE_STRAP) & HSW_CPU_SSC_ENABLE);
}

/**
 * Set the PCH's reference clocks, Linux's lpt_init_pch_refclk: left alone
 * while a PLL runs from the PCH's SSC reference, else CLKOUT_DP turned off,
 * since no output here uses FDI, which only the analog port has.
 */
static void i915_lpt_init_pch_refclk(struct drm_i915_private *i915)
{
    if (i915_spll_uses_pch_ssc(&i915->hw) || i915_wrpll_uses_pch_ssc(i915, 0) ||
        i915_wrpll_uses_pch_ssc(i915, 1)) {
        return;
    }
    i915_lpt_disable_clkout_dp(i915);
}

/**
 * Write `val` to D_COMP through the pcode, Linux's hsw_write_dcomp, trying
 * again a mailbox that another command holds, since a resume may come from
 * a thread without the big lock that keeps the mailbox to one command.
 */
static void i915_dcomp_write(struct drm_i915_private *i915, uint32_t val)
{
    int32_t err;
    int i;

    for (i = 0; i < I915_DCOMP_TRIES; i++) {
        err = i915_pcode_write(i915, GEN6_PCODE_WRITE_D_COMP, val);
        if (err != -EAGAIN) {
            break;
        }
        k_udelay(100);
    }
    if (err != 0) {
        klog(KLOG_ERR, "i915", "pc8: D_COMP not written: error %d", (int)err);
    }
}

/** Log each part that should be off before LCPLL is, Linux's
 * assert_can_disable_lcpll. */
static void i915_lcpll_check_idle(struct drm_i915_private *i915)
{
    struct i915_hw *hw = &i915->hw;
    int pipe;

    for (pipe = 0; pipe < I915_NUM_PIPES; pipe++) {
        if (i915->display.crtc[pipe].cfg.active) {
            klog(KLOG_WARN, "i915", "pc8: pipe %c enabled", 'A' + pipe);
        }
    }
    if (i915_read(hw, HSW_PWR_WELL_CTL2) != 0) {
        klog(KLOG_WARN, "i915", "pc8: display power well on");
    }
    if (i915_read(hw, SPLL_CTL) & SPLL_PLL_ENABLE) {
        klog(KLOG_WARN, "i915", "pc8: SPLL enabled");
    }
    if (i915_read(hw, WRPLL_CTL(0)) & WRPLL_PLL_ENABLE) {
        klog(KLOG_WARN, "i915", "pc8: WRPLL1 enabled");
    }
    if (i915_read(hw, WRPLL_CTL(1)) & WRPLL_PLL_ENABLE) {
        klog(KLOG_WARN, "i915", "pc8: WRPLL2 enabled");
    }
    if (i915_read(hw, PP_STATUS) & PP_ON) {
        klog(KLOG_WARN, "i915", "pc8: panel power on");
    }
    if (i915_read(hw, BLC_PWM_CPU_CTL2) & BLM_PWM_ENABLE) {
        klog(KLOG_WARN, "i915", "pc8: CPU PWM1 enabled");
    }
    if (i915_read(hw, HSW_BLC_PWM2_CTL) & BLM_PWM_ENABLE) {
        klog(KLOG_WARN, "i915", "pc8: CPU PWM2 enabled");
    }
    if (i915_read(hw, BLC_PWM_PCH_CTL1) & BLM_PCH_PWM_ENABLE) {
        klog(KLOG_WARN, "i915", "pc8: PCH PWM1 enabled");
    }
    if ((i915_read(hw, UTIL_PIN_CTL) & (UTIL_PIN_ENABLE | UTIL_PIN_MODE_MASK)) ==
        (UTIL_PIN_ENABLE | UTIL_PIN_MODE_PWM)) {
        klog(KLOG_WARN, "i915", "pc8: utility pin enabled in PWM mode");
    }
    if (i915_read(hw, PCH_GTC_CTL) & PCH_GTC_ENABLE) {
        klog(KLOG_WARN, "i915", "pc8: PCH GTC enabled");
    }
    if (!i915->irq.suspended) {
        klog(KLOG_WARN, "i915", "pc8: interrupts enabled");
    }
}

void i915_pc8_enable(struct drm_i915_private *i915)
{
    struct i915_hw *hw = &i915->hw;
    uint32_t v;

    if (i915->id && (i915->id->ult || i915->id->ulx)) {
        (void)i915_rmw(hw, SOUTH_DSPCLK_GATE_D, PCH_LP_PARTITION_LEVEL_DISABLE, 0);
    }
    i915_lpt_disable_clkout_dp(i915);

    /* hsw_disable_lcpll(switch_to_fclk, allow_power_down): the CD clock
     * moved to FCLK, the PLL off, its compensation off, power-down let. */
    i915_lcpll_check_idle(i915);
    v = i915_read(hw, LCPLL_CTL) | LCPLL_CD_SOURCE_FCLK;
    i915_write(hw, LCPLL_CTL, v);
    if (i915_wait_for(hw, "fclk switch", LCPLL_CTL, LCPLL_CD_SOURCE_FCLK_DONE,
                      LCPLL_CD_SOURCE_FCLK_DONE, I915_FCLK_US) < 0) {
        klog(KLOG_ERR, "i915", "pc8: switching to FCLK failed");
    }
    v = i915_read(hw, LCPLL_CTL) | LCPLL_PLL_DISABLE;
    i915_write(hw, LCPLL_CTL, v);
    i915_posting_read(hw, LCPLL_CTL);
    if (i915_wait_for(hw, "lcpll unlock", LCPLL_CTL, LCPLL_PLL_LOCK, 0, I915_LCPLL_OFF_US) < 0) {
        klog(KLOG_ERR, "i915", "pc8: LCPLL still locked");
    }
    i915_dcomp_write(i915, i915_read(hw, D_COMP_HSW) | D_COMP_COMP_DISABLE);
    k_udelay(1);
    if (i915_wait_for(hw, "d_comp rcomp", D_COMP_HSW, D_COMP_RCOMP_IN_PROGRESS, 0, I915_RCOMP_US) <
        0) {
        klog(KLOG_ERR, "i915", "pc8: D_COMP RCOMP still in progress");
    }
    (void)i915_rmw(hw, LCPLL_CTL, 0, LCPLL_POWER_DOWN_ALLOW);
    i915_posting_read(hw, LCPLL_CTL);
}

void i915_pc8_disable(struct drm_i915_private *i915)
{
    struct i915_hw *hw = &i915->hw;
    uint32_t v = i915_read(hw, LCPLL_CTL), d;
    int fw;

    /* hsw_restore_lcpll, with forcewake held so that the package does not
     * enter PC8 meanwhile, which would hang the machine. */
    if ((v & (LCPLL_PLL_LOCK | LCPLL_PLL_DISABLE | LCPLL_CD_SOURCE_FCLK |
              LCPLL_POWER_DOWN_ALLOW)) != LCPLL_PLL_LOCK) {
        fw = i915_forcewake_get(i915) == 0;
        if (v & LCPLL_POWER_DOWN_ALLOW) {
            v &= ~LCPLL_POWER_DOWN_ALLOW;
            i915_write(hw, LCPLL_CTL, v);
            i915_posting_read(hw, LCPLL_CTL);
        }
        d = (i915_read(hw, D_COMP_HSW) | D_COMP_COMP_FORCE) & ~D_COMP_COMP_DISABLE;
        i915_dcomp_write(i915, d);
        v = i915_read(hw, LCPLL_CTL) & ~LCPLL_PLL_DISABLE;
        i915_write(hw, LCPLL_CTL, v);
        if (i915_wait_for(hw, "lcpll lock", LCPLL_CTL, LCPLL_PLL_LOCK, LCPLL_PLL_LOCK,
                          I915_LCPLL_ON_US) < 0) {
            klog(KLOG_ERR, "i915", "pc8: LCPLL not locked yet");
        }
        if (v & LCPLL_CD_SOURCE_FCLK) {
            (void)i915_rmw(hw, LCPLL_CTL, LCPLL_CD_SOURCE_FCLK, 0);
            if (i915_wait_for(hw, "lcpll switch", LCPLL_CTL, LCPLL_CD_SOURCE_FCLK_DONE, 0,
                              I915_FCLK_US) < 0) {
                klog(KLOG_ERR, "i915", "pc8: switching back to LCPLL failed");
            }
        }
        if (fw) {
            i915_forcewake_put(i915);
        }
    }
    i915_lpt_init_pch_refclk(i915);
    /* Many display registers do not survive PC8: intel_clock_gating_init. */
    if (i915_forcewake_get(i915) == 0) {
        i915_gt_init_clock_gating(i915);
        i915_forcewake_put(i915);
    }
    if (i915->display.ready) {
        intel_display_clock_gating(i915);
    }
}
