/*
 * i915_runtime_pm.c: the device's runtime power management on Haswell,
 * after Linux 6.12's intel_runtime_suspend and intel_runtime_resume
 * (i915_driver.c), hsw_enable_pc8 and hsw_disable_pc8
 * (display/intel_display_power.c), lpt_disable_clkout_dp
 * (display/intel_pch_refclk.c) and the PCI core's D3hot.
 */

#include "i915.h"
#include "i915_reg.h"

#include "../../fs/debugfs.h"
#include "../../fs/synthfs.h"
#include "../../klog.h"
#include "../../lib/kerrno.h"

/* Linux's bounds: the switch of the CD clock's source, 1 us; LCPLL's lock
 * and unlock, 5 ms and 1 ms; D_COMP's compensation, 1 ms; the sideband's
 * idle and its answer, 100 ms each (intel_sbi_rw); and the wait after
 * PATHALT before SSC goes off, 32 us. */
#define FCLK_US    1u
#define LOCK_US    5000u
#define UNLOCK_US  1000u
#define RCOMP_US   1000u
#define SBI_US     100000u
#define PATHALT_US 32u
#define PCI_D3_US  (PCI_PM_D3HOT_WAIT * 1000u)

/** The device of the record's `core`. */
static struct drm_i915_private *rpm_i915(void *owner)
{
    return (struct drm_i915_private *)owner;
}

/* ------------------------------------------------------------------------
 * The sideband to the PCH's clocks and CLKOUT_DP
 */

/**
 * One transfer of the sideband to the clock unit's register `reg`, as
 * Linux's intel_sbi_rw: `*val` written, or read into it. 0; -EBUSY when a
 * transfer before it has not ended, -ETIMEDOUT when this one does not, or
 * -ENXIO when the PCH refuses it.
 */
static int32_t sbi_rw(struct drm_i915_private *i915, uint32_t reg, uint32_t *val, int write)
{
    struct i915_hw *hw = &i915->hw;
    uint32_t cmd = SBI_CTL_DEST_ICLK | SBI_CTL_OP_CRRD | (write ? SBI_CTL_OP_WRITE : 0u);

    if (i915_wait_for(hw, "sbi ready", SBI_CTL_STAT, SBI_BUSY, 0, SBI_US) < 0) {
        return -EBUSY;
    }
    i915_write(hw, SBI_ADDR, reg << 16);
    i915_write(hw, SBI_DATA, write ? *val : 0u);
    i915_write(hw, SBI_CTL_STAT, cmd | SBI_BUSY);
    if (i915_wait_for(hw, "sbi transfer", SBI_CTL_STAT, SBI_BUSY, 0, SBI_US) < 0) {
        return -ETIMEDOUT;
    }
    if (i915_read(hw, SBI_CTL_STAT) & SBI_RESPONSE_FAIL) {
        klog(KLOG_ERR, "i915", "sbi: transfer of 0x%x refused", (unsigned)reg);
        return -ENXIO;
    }
    if (!write) {
        *val = i915_read(hw, SBI_DATA);
    }
    return 0;
}

/** Whether the PCH is Lynx Point LP, which ULT and ULX parts carry. */
static int pch_lp(const struct drm_i915_private *i915)
{
    return i915->id && (i915->id->ult || i915->id->ulx);
}

/** Turn the CLKOUT_DP reference off, as Linux's lpt_disable_clkout_dp: its
 * buffer disabled, then spread spectrum's path switched and its clock off. */
static void clkout_dp_off(struct drm_i915_private *i915)
{
    uint32_t reg = pch_lp(i915) ? SBI_GEN0 : SBI_DBUFF0, v = 0;

    if (sbi_rw(i915, reg, &v, 0) == 0) {
        v &= ~SBI_GEN0_CFG_BUFFENABLE_DISABLE;
        (void)sbi_rw(i915, reg, &v, 1);
    }
    if (sbi_rw(i915, SBI_SSCCTL, &v, 0) != 0 || (v & SBI_SSCCTL_DISABLE)) {
        return;
    }
    if (!(v & SBI_SSCCTL_PATHALT)) {
        v |= SBI_SSCCTL_PATHALT;
        (void)sbi_rw(i915, SBI_SSCCTL, &v, 1);
        i915_delay_us(&i915->hw, PATHALT_US);
    }
    v |= SBI_SSCCTL_DISABLE;
    (void)sbi_rw(i915, SBI_SSCCTL, &v, 1);
}

/** Whether a PLL runs from the PCH's spread-spectrum reference, as Linux's
 * spll_uses_pch_ssc and wrpll_uses_pch_ssc on Haswell. */
static int plls_use_pch_ssc(struct drm_i915_private *i915)
{
    struct i915_hw *hw = &i915->hw;
    uint32_t cpu_ssc = i915_read(hw, FUSE_STRAP) & HSW_CPU_SSC_ENABLE;
    uint32_t spll = i915_read(hw, SPLL_CTL), wrpll;
    int i;

    if ((spll & SPLL_PLL_ENABLE) && (spll & SPLL_REF_MASK) == SPLL_REF_MUXED_SSC && !cpu_ssc) {
        return 1;
    }
    for (i = 0; i < 2; i++) {
        wrpll = i915_read(hw, WRPLL_CTL(i));
        if (!(wrpll & WRPLL_PLL_ENABLE)) {
            continue;
        }
        if ((wrpll & WRPLL_REF_MASK) == WRPLL_REF_PCH_SSC) {
            return 1;
        }
        /* The muxed reference of a ULT part, Linux's
         * WRPLL_REF_MUXED_SSC_BDW, has the value of WRPLL_REF_SPECIAL_HSW. */
        if (pch_lp(i915) && (wrpll & WRPLL_REF_MASK) == WRPLL_REF_SPECIAL_HSW && !cpu_ssc) {
            return 1;
        }
    }
    return 0;
}

/* ------------------------------------------------------------------------
 * LCPLL and package C8
 */

/** Write D_COMP through pcode, as Linux's hsw_write_dcomp on Haswell. */
static void dcomp_write(struct drm_i915_private *i915, uint32_t v)
{
    if (i915_pcode_write(i915, GEN6_PCODE_WRITE_D_COMP, v) != 0) {
        klog(KLOG_WARN, "i915", "runtime pm: D_COMP not written");
    }
}

/**
 * The first of Linux's checks before LCPLL goes off
 * (assert_can_disable_lcpll) that the hardware fails, or NULL: no pipe
 * active, the display well off, no PLL running, no panel power, no
 * backlight PWM, the utility pin not a PWM, and the PCH's time code off.
 */
static const char *lcpll_blocker(struct drm_i915_private *i915)
{
    struct i915_hw *hw = &i915->hw;
    uint32_t util = i915_read(hw, UTIL_PIN_CTL);
    int pipe;

    if (!i915->display.ready) {
        return "the display is the firmware's";
    }
    for (pipe = 0; pipe < I915_NUM_PIPES; pipe++) {
        if (i915->display.crtc[pipe].cfg.active) {
            return "a pipe is active";
        }
    }
    if (i915_read(hw, HSW_PWR_WELL_CTL2) != 0) {
        return "the display well is on";
    }
    if ((i915_read(hw, SPLL_CTL) & SPLL_PLL_ENABLE) ||
        (i915_read(hw, WRPLL_CTL(0)) & WRPLL_PLL_ENABLE) ||
        (i915_read(hw, WRPLL_CTL(1)) & WRPLL_PLL_ENABLE)) {
        return "a PLL runs";
    }
    if (i915_read(hw, PP_STATUS) & PP_ON) {
        return "the panel has power";
    }
    if ((i915_read(hw, BLC_PWM_CPU_CTL2) & BLM_PWM_ENABLE) ||
        (i915_read(hw, HSW_BLC_PWM2_CTL) & BLM_PWM_ENABLE) ||
        (i915_read(hw, BLC_PWM_PCH_CTL1) & BLM_PCH_PWM_ENABLE)) {
        return "a backlight PWM is on";
    }
    if ((util & (UTIL_PIN_ENABLE | UTIL_PIN_MODE_MASK)) == (UTIL_PIN_ENABLE | UTIL_PIN_MODE_PWM)) {
        return "the utility pin is a PWM";
    }
    if (i915_read(hw, PCH_GTC_CTL) & PCH_GTC_ENABLE) {
        return "the PCH's time code is on";
    }
    return NULL;
}

/**
 * Allow package C8, Linux's hsw_enable_pc8 with hsw_disable_lcpll(true,
 * true): the PCH's partition level gating back on a low-power PCH,
 * CLKOUT_DP off, the CD clock moved to FCLK, LCPLL off, D_COMP's
 * compensation off, and LCPLL allowed to power down. Linux logs a wait
 * that does not end and goes on; so does this.
 */
static void pc8_enable(struct drm_i915_private *i915)
{
    struct i915_hw *hw = &i915->hw;
    uint32_t v;

    if (pch_lp(i915)) {
        (void)i915_rmw(hw, SOUTH_DSPCLK_GATE_D, PCH_LP_PARTITION_LEVEL_DISABLE, 0);
    }
    clkout_dp_off(i915);
    v = i915_read(hw, LCPLL_CTL) | LCPLL_CD_SOURCE_FCLK;
    i915_write(hw, LCPLL_CTL, v);
    (void)i915_wait_for(hw, "lcpll fclk", LCPLL_CTL, LCPLL_CD_SOURCE_FCLK_DONE,
                        LCPLL_CD_SOURCE_FCLK_DONE, FCLK_US);
    v = i915_read(hw, LCPLL_CTL) | LCPLL_PLL_DISABLE;
    i915_write(hw, LCPLL_CTL, v);
    i915_posting_read(hw, LCPLL_CTL);
    (void)i915_wait_for(hw, "lcpll unlock", LCPLL_CTL, LCPLL_PLL_LOCK, 0, UNLOCK_US);
    dcomp_write(i915, i915_read(hw, D_COMP_HSW) | D_COMP_COMP_DISABLE);
    i915_delay_us(hw, 1);
    (void)i915_wait_for(hw, "d_comp rcomp", D_COMP_HSW, D_COMP_RCOMP_IN_PROGRESS, 0, RCOMP_US);
    (void)i915_rmw(hw, LCPLL_CTL, 0, LCPLL_POWER_DOWN_ALLOW);
    i915_posting_read(hw, LCPLL_CTL);
    i915->rpm.pc8 = 1;
}

/**
 * Leave package C8, Linux's hsw_disable_pc8: LCPLL restored under
 * forcewake, which keeps the package out of C8 meanwhile
 * (hsw_restore_lcpll), the PCH's reference clocks as no PLL then needs
 * them (lpt_init_pch_refclk, whose CLKOUT_DP stays off on a part without
 * FDI, as every ULT part is and Rach2 drives none), and the clock gating
 * that package C8 loses.
 */
static void pc8_disable(struct drm_i915_private *i915)
{
    struct i915_hw *hw = &i915->hw;
    uint32_t v = i915_read(hw, LCPLL_CTL);
    int fw = i915_forcewake_get(i915) == 0;

    if ((v & (LCPLL_PLL_LOCK | LCPLL_PLL_DISABLE | LCPLL_CD_SOURCE_FCLK |
              LCPLL_POWER_DOWN_ALLOW)) != LCPLL_PLL_LOCK) {
        if (v & LCPLL_POWER_DOWN_ALLOW) {
            v &= ~LCPLL_POWER_DOWN_ALLOW;
            i915_write(hw, LCPLL_CTL, v);
            i915_posting_read(hw, LCPLL_CTL);
        }
        dcomp_write(i915, (i915_read(hw, D_COMP_HSW) | D_COMP_COMP_FORCE) & ~D_COMP_COMP_DISABLE);
        v = i915_read(hw, LCPLL_CTL) & ~LCPLL_PLL_DISABLE;
        i915_write(hw, LCPLL_CTL, v);
        (void)i915_wait_for(hw, "lcpll lock", LCPLL_CTL, LCPLL_PLL_LOCK, LCPLL_PLL_LOCK, LOCK_US);
        if (v & LCPLL_CD_SOURCE_FCLK) {
            (void)i915_rmw(hw, LCPLL_CTL, LCPLL_CD_SOURCE_FCLK, 0);
            (void)i915_wait_for(hw, "lcpll back from fclk", LCPLL_CTL, LCPLL_CD_SOURCE_FCLK_DONE, 0,
                                FCLK_US);
        }
    }
    if (!plls_use_pch_ssc(i915)) {
        clkout_dp_off(i915);
    }
    hsw_display_clock_gating(i915);
    i915_gt_init_clock_gating(i915);
    if (fw) {
        i915_forcewake_put(i915);
    }
    i915->rpm.pc8 = 0;
}

/* ------------------------------------------------------------------------
 * PCI D3hot
 */

/** Whether the sleep puts the function in D3hot: a chip reached through
 * its BAR, with a power management capability. */
static int uses_d3(struct drm_i915_private *i915)
{
    return i915->rpm.pm_cap && i915_hw_is_mmio(&i915->hw);
}

/** Save the function's configuration and put it in D3hot, as the PCI
 * core's pci_pm_runtime_suspend, waiting out Linux's d3hot_delay. */
static void d3_enter(struct drm_i915_private *i915)
{
    if (!uses_d3(i915)) {
        return;
    }
    pci_save_state(&i915->pci, &i915->rpm.pci);
    (void)pci_set_power_state(&i915->pci, PCI_D3HOT);
    i915_delay_us(&i915->hw, PCI_D3_US);
    i915->rpm.d3 = 1;
}

/** Bring the function back to D0, wait out the delay, and write its
 * configuration back, the message's enable among it, as the PCI core's
 * pci_pm_runtime_resume with pci_restore_state. */
static void d3_leave(struct drm_i915_private *i915)
{
    const pci_device_t *p = &i915->pci;
    uint8_t msi;

    if (!i915->rpm.d3) {
        return;
    }
    (void)pci_set_power_state(p, PCI_D0);
    i915_delay_us(&i915->hw, PCI_D3_US);
    pci_restore_state(p, &i915->rpm.pci);
    i915->rpm.d3 = 0;
    msi = pci_find_capability(p->bus, p->device, p->function, PCI_CAP_ID_MSI);
    if (i915->msi_vector >= 0 && msi != 0 &&
        !(pci_read_config16(p->bus, p->device, p->function, (uint8_t)(msi + 2)) & PCI_MSI_ENABLE) &&
        pci_msi_enable(p, (uint8_t)i915->msi_vector) != 0) {
        klog(KLOG_ERR, "i915", "runtime pm: the message would not enable again");
    }
}

/* ------------------------------------------------------------------------
 * The core's callbacks
 */

/** Log `why` the sleep was refused when it differs from the last reason;
 * -EBUSY, to have it tried again. */
static int32_t refuse(struct drm_i915_private *i915, const char *why)
{
    if (i915->rpm.refused != why) {
        i915->rpm.refused = why;
        klog(KLOG_INFO, "i915", "runtime pm: suspend waits: %s", why);
    }
    return -EBUSY;
}

/** The state a sleep needs, checked before and again in it: no forcewake
 * held, the display well off, and LCPLL free to go off. 0, or `refuse`'s
 * -EBUSY. */
static int32_t sleep_ready(struct drm_i915_private *i915)
{
    const char *why;

    if (i915->uncore.wake_count != 0) {
        return refuse(i915, "forcewake is held");
    }
    if (i915_power_suspend(i915) != 0) {
        return refuse(i915, "the display well is in use");
    }
    why = lcpll_blocker(i915);
    return why ? refuse(i915, why) : 0;
}

/**
 * Before a sleep, the checks of `sleep_ready`, then Linux's
 * i915_gem_runtime_suspend: every program's GTT map revoked, so that its
 * next access faults and wakes the device, since the aperture is gone in
 * D3hot. Under the big lock; the GEM lock only tried, as the worker runs
 * beside its holders. 0, -EBUSY when the hardware is not ready or another
 * thread holds the GEM lock.
 */
static int32_t rpm_prepare(void *owner)
{
    struct drm_i915_private *i915 = rpm_i915(owner);
    struct drm_gem_object *base;
    int32_t err = sleep_ready(i915);
    int taken;

    if (err != 0) {
        return err;
    }
    if (!i915_gem_trylock(i915, &taken)) {
        return -EBUSY;
    }
    i915->rpm.faults_seen = i915->rpm.faults;
    for (base = drm_gem_object_next(&i915->drm, 0); base;
         base = drm_gem_object_next(&i915->drm, base)) {
        struct drm_i915_gem_object *obj = (struct drm_i915_gem_object *)base;

        if (base->funcs == &i915_gem_object_funcs && obj->userfault_count) {
            i915_gem_object_release_mmap_gtt(obj);
        }
    }
    i915_gem_unlock(i915, taken);
    return 0;
}

/**
 * Put the device to sleep, Linux's intel_runtime_suspend and the PCI
 * core's after it: a GTT fault since `rpm_prepare` refuses it, and so do
 * the checks of `sleep_ready` again; then forcewake let go, the
 * interrupts off and a handler on another processor finished, as Linux's
 * intel_runtime_pm_disable_interrupts, package C8 allowed, D3hot, every
 * access refused (`hw.asleep`), and the connectors polled.
 */
static int32_t rpm_suspend(void *owner)
{
    struct drm_i915_private *i915 = rpm_i915(owner);
    int32_t err;

    if (i915->rpm.faults != i915->rpm.faults_seen) {
        return -EAGAIN;
    }
    err = sleep_ready(i915);
    if (err != 0) {
        return err;
    }
    if (i915_uncore_suspend(i915) != 0) {
        return refuse(i915, "forcewake is held");
    }
    i915->rpm.refused = NULL;
    i915_irq_suspend(i915);
    i915_irq_sync(i915);
    pc8_enable(i915);
    d3_enter(i915);
    i915->hw.asleep_logged = 0;
    i915->hw.asleep = 1;
    intel_hotplug_suspend(i915);
    return 0;
}

/**
 * Wake the device, Linux's intel_runtime_resume after the PCI core's: D0
 * and the configuration again, package C8 left, the interrupts back, the
 * fence registers and swizzle written again, and the hotplug detection
 * back with every connector probed. Spins and never sleeps. 0.
 */
static int32_t rpm_resume(void *owner)
{
    struct drm_i915_private *i915 = rpm_i915(owner);

    i915->hw.asleep = 0;
    if (i915->hw.asleep_accesses != 0 && i915->hw.asleep_logged) {
        klog(KLOG_ERR, "i915", "runtime pm: %u accesses while suspended, the last of 0x%x",
             (unsigned)i915->hw.asleep_accesses, (unsigned)i915->hw.asleep_reg);
    }
    d3_leave(i915);
    pc8_disable(i915);
    i915_irq_resume(i915);
    i915_gem_restore_fences(i915);
    intel_hotplug_resume(i915);
    return 0;
}

/** Have the worker run `pm_runtime_idle` `ms` milliseconds from now. */
static void rpm_schedule(void *owner, uint32_t ms)
{
    struct drm_i915_private *i915 = rpm_i915(owner);

    drm_dev_work_later_ms(&i915->drm, &i915->rpm.work, ms);
}

static const struct pm_runtime_ops i915_rpm_ops = {rpm_prepare, rpm_suspend, rpm_resume,
                                                   rpm_schedule};

/** The worker's run of the sleep, under the big lock, with the device
 * held by the worker. */
static void rpm_work(struct drm_work *work)
{
    pm_runtime_idle(&to_i915(work->dev)->rpm.core);
}

/* ------------------------------------------------------------------------
 * The interface
 */

void i915_rpm_init(struct drm_i915_private *i915)
{
    const pci_device_t *p = &i915->pci;

    pm_runtime_init(&i915->rpm.core, &i915_rpm_ops, i915);
    i915->rpm.work.fn = rpm_work;
    i915->rpm.pm_cap = pci_find_capability(p->bus, p->device, p->function, PCI_CAP_ID_PM) != 0;
}

void i915_rpm_register(struct drm_i915_private *i915)
{
    const pci_device_t *p = &i915->pci;
    int key = p->bus << 8 | p->device << 3 | p->function;
    int dir = sysfs_anchor(SYSFS_PCI_DEVICE, key);

    pm_runtime_set_delay(&i915->rpm.core, (int32_t)I915_RPM_DELAY_MS);
    if (dir <= 0 || (dir = pm_runtime_sysfs_add(&i915->rpm.core, dir)) < 0) {
        klog(KLOG_WARN, "i915", "sysfs: power not added: error %d", dir);
    }
    pm_runtime_enable(&i915->rpm.core);
}

void i915_rpm_unplug(struct drm_i915_private *i915)
{
    (void)drm_dev_work_cancel(&i915->rpm.work);
    if (pm_runtime_disable(&i915->rpm.core) != 0) {
        klog(KLOG_ERR, "i915", "runtime pm: the device did not wake for its unplug");
    }
    pm_runtime_sysfs_remove(&i915->rpm.core);
}

void i915_rpm_get(struct drm_i915_private *i915)
{
    (void)pm_runtime_get_sync(&i915->rpm.core);
}

int i915_rpm_get_if_awake(struct drm_i915_private *i915)
{
    return pm_runtime_get_if_active(&i915->rpm.core);
}

void i915_rpm_get_noresume(struct drm_i915_private *i915)
{
    pm_runtime_get_noresume(&i915->rpm.core);
}

void i915_rpm_put(struct drm_i915_private *i915)
{
    pm_runtime_put(&i915->rpm.core);
}

void i915_rpm_gtt_fault(struct drm_i915_private *i915)
{
    i915->rpm.faults++;
    pm_runtime_mark_busy(&i915->rpm.core, I915_RPM_GTT_FAULT_MS);
}

void i915_drm_pm_get(struct drm_device *dev)
{
    i915_rpm_get(to_i915(dev));
}

void i915_drm_pm_put(struct drm_device *dev)
{
    i915_rpm_put(to_i915(dev));
}

void intel_crtc_rpm_sync(struct intel_crtc *c)
{
    if (c->cfg.active && !c->rpm_held) {
        c->rpm_held = 1;
        i915_rpm_get_noresume(c->i915);
    } else if (!c->cfg.active && c->rpm_held) {
        c->rpm_held = 0;
        i915_rpm_put(c->i915);
    }
}

/* ------------------------------------------------------------------------
 * debugfs
 */

/** Append the NUL-terminated `s` to the `cap` bytes at `buf`, as far as it
 * goes, counting its whole length in `*len`. */
static void put_s(char *buf, uint32_t cap, uint32_t *len, const char *s)
{
    for (; *s; s++, (*len)++) {
        if (*len + 1u < cap) {
            buf[*len] = *s;
            buf[*len + 1u] = '\0';
        }
    }
}

/** Append the decimal `v`, which may be negative, as `put_s` does. */
static void put_d(char *buf, uint32_t cap, uint32_t *len, int32_t v)
{
    char digits[16];
    int i = (int)sizeof(digits) - 1;
    uint32_t u = v < 0 ? (uint32_t)0 - (uint32_t)v : (uint32_t)v;

    digits[i] = '\0';
    do {
        digits[--i] = (char)('0' + (int)(u % 10u));
        u /= 10u;
    } while (u != 0);
    if (v < 0) {
        digits[--i] = '-';
    }
    put_s(buf, cap, len, &digits[i]);
}

/** i915_runtime_pm_status, Linux 6.18's i915_runtime_pm_status: five
 * lines, each from the driver's records. */
static int32_t runtime_pm_status_show(void *data, char *buf, uint32_t cap)
{
    struct drm_i915_private *i915 = (struct drm_i915_private *)data;
    uint32_t len = 0;

    put_s(buf, cap, &len, "Runtime power status: ");
    put_s(buf, cap, &len, i915->rpm.core.enabled ? "enabled" : "disabled");
    put_s(buf, cap, &len, "\nGPU idle: ");
    put_s(buf, cap, &len, i915->gt.awake ? "no" : "yes");
    put_s(buf, cap, &len, "\nIRQs disabled: ");
    put_s(buf, cap, &len, i915->irq.suspended ? "yes" : "no");
    put_s(buf, cap, &len, "\nUsage count: ");
    put_d(buf, cap, &len, pm_runtime_usage(&i915->rpm.core));
    put_s(buf, cap, &len, "\nPCI device power state: ");
    put_s(buf, cap, &len, i915->rpm.d3 ? "D3hot [3]\n" : "D0 [0]\n");
    return (int32_t)len;
}

const struct debugfs_text_ops i915_runtime_pm_status_ops = {runtime_pm_status_show, NULL};
