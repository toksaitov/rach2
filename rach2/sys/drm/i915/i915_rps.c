/*
 * i915_rps.c: the Haswell GPU's frequency, RPS, its up and down
 * interrupts, the pcode mailbox and the ring's frequency table, and the
 * /sys files of frequency and RC6, after Linux 6.12's gt/intel_rps.c,
 * intel_pcode.c, gt/intel_llc.c and gt/intel_gt_sysfs_pm.c.
 */

#include "i915.h"
#include "i915_reg.h"

#include "../../fs/sysfs.h"
#include "../../klog.h"
#include "../../lib/kerrno.h"
#include "../../lib/kstring.h"

/* MHz in a frequency unit of the chip, Linux's GT_FREQUENCY_MULTIPLIER. */
#define RPS_MHZ 50

/* The pcode's bound: Linux's 500 us fast wait and 20 ms slow wait. */
#define PCODE_TIMEOUT_US 20500u

/* ---- the pcode mailbox ---- */

/** The pcode's status in a finished command, as Linux's
 * gen7_check_mailbox_status: 0 or -errno. */
static int32_t pcode_status(uint32_t mbox)
{
    switch (mbox & GEN6_PCODE_ERROR_MASK) {
    case GEN6_PCODE_SUCCESS:
        return 0;
    case GEN6_PCODE_ILLEGAL_CMD:
        return -ENXIO;
    case GEN7_PCODE_TIMEOUT:
        return -ETIMEDOUT;
    case GEN7_PCODE_ILLEGAL_DATA:
        return -EINVAL;
    case GEN11_PCODE_ILLEGAL_SUBCOMMAND:
        return -ENXIO;
    case GEN11_PCODE_LOCKED:
        return -EBUSY;
    case GEN11_PCODE_REJECTED:
        return -EACCES;
    case GEN7_PCODE_MIN_FREQ_TABLE_GT_RATIO_OUT_OF_RANGE:
        return -EOVERFLOW;
    default:
        /* Linux warns of a status it does not know and takes it as done. */
        return 0;
    }
}

/**
 * One command through the mailbox, as Linux's __snb_pcode_rw. The mailbox
 * is outside the GT, so it needs no forcewake. 0 or -errno as
 * `i915_pcode_read` says.
 */
static int32_t pcode_rw(struct drm_i915_private *i915, uint32_t mbox, uint32_t *val, uint32_t *val1,
                        int is_read)
{
    struct i915_hw *hw = &i915->hw;
    int32_t err;

    if (i915_read(hw, GEN6_PCODE_MAILBOX) & GEN6_PCODE_READY) {
        err = -EAGAIN;
    } else {
        i915_write(hw, GEN6_PCODE_DATA, *val);
        i915_write(hw, GEN6_PCODE_DATA1, val1 ? *val1 : 0);
        i915_write(hw, GEN6_PCODE_MAILBOX, GEN6_PCODE_READY | mbox);
        if (i915_wait_for(hw, "pcode mailbox", GEN6_PCODE_MAILBOX, GEN6_PCODE_READY, 0,
                          PCODE_TIMEOUT_US) < 0) {
            err = -ETIMEDOUT;
        } else {
            if (is_read) {
                *val = i915_read(hw, GEN6_PCODE_DATA);
                if (val1) {
                    *val1 = i915_read(hw, GEN6_PCODE_DATA1);
                }
            }
            err = pcode_status(i915_read(hw, GEN6_PCODE_MAILBOX));
        }
    }
    if (err < 0) {
        klog(KLOG_DEBUG, "i915", "pcode: %s of command 0x%x: error %d", is_read ? "read" : "write",
             (unsigned)mbox, (int)err);
    }
    return err;
}

int32_t i915_pcode_read(struct drm_i915_private *i915, uint32_t mbox, uint32_t *val, uint32_t *val1)
{
    return pcode_rw(i915, mbox, val, val1, 1);
}

int32_t i915_pcode_write(struct drm_i915_private *i915, uint32_t mbox, uint32_t val)
{
    return pcode_rw(i915, mbox, &val, 0, 0);
}

/* ---- frequency ---- */

/** `v` held within [lo, hi]. */
static int rps_clamp(int v, int lo, int hi)
{
    return v < lo ? lo : v > hi ? hi : v;
}

/** The chip's unit nearest `mhz`, as Linux's intel_freq_opcode. */
static int rps_opcode(uint32_t mhz)
{
    return (int)(mhz / RPS_MHZ + (mhz % RPS_MHZ >= RPS_MHZ / 2));
}

/** Nanoseconds as the PM unit's count, a sixteenth of the 12.5 MHz
 * timestamp's, rounded up, as Linux's intel_gt_ns_to_pm_interval. */
static uint32_t rps_pm_interval(uint32_t ns)
{
    uint32_t clocks = (ns + 79u) / 80u;

    return (clocks + 15u) / 16u;
}

/**
 * Write the evaluation intervals and busy thresholds of the power mode
 * `mode`, an I915_RPS_ mode, and enable RPS's hardware in GEN6_RP_CONTROL,
 * as Linux's rps_set_power. The intervals are Linux's in units of 1.28
 * us. Under the frequency lock.
 */
static void rps_set_power(struct drm_i915_private *i915, int mode)
{
    struct i915_rps *rps = &i915->rps;
    struct i915_hw *hw = &i915->hw;
    uint32_t ei_up, ei_down = 32000;

    if (mode == rps->power_mode) {
        return;
    }
    ei_up = mode == I915_RPS_LOW_POWER ? 16000u : mode == I915_RPS_BETWEEN ? 13000u : 10000u;
    i915_write(hw, GEN6_RP_UP_EI, rps_pm_interval(ei_up * 1000u));
    i915_write(hw, GEN6_RP_UP_THRESHOLD,
               rps_pm_interval(ei_up * (uint32_t)rps->up_threshold * 10u));
    i915_write(hw, GEN6_RP_DOWN_EI, rps_pm_interval(ei_down * 1000u));
    i915_write(hw, GEN6_RP_DOWN_THRESHOLD,
               rps_pm_interval(ei_down * (uint32_t)rps->down_threshold * 10u));
    i915_write(hw, GEN6_RP_CONTROL,
               GEN6_RP_MEDIA_TURBO | GEN6_RP_MEDIA_HW_NORMAL_MODE | GEN6_RP_MEDIA_IS_GFX |
                   GEN6_RP_ENABLE | GEN6_RP_UP_BUSY_AVG | GEN6_RP_DOWN_IDLE_AVG);
    rps->power_mode = mode;
}

/** Choose the power mode for the frequency `val`, as Linux's
 * gen6_rps_set_thresholds: high power while a flip is pending. Under the
 * frequency lock. */
static void rps_set_thresholds(struct drm_i915_private *i915, int val)
{
    struct i915_rps *rps = &i915->rps;
    int mode = rps->power_mode;

    switch (rps->power_mode) {
    case I915_RPS_LOW_POWER:
        if (val > rps->efficient_freq + 1 && val > rps->cur_freq) {
            mode = I915_RPS_BETWEEN;
        }
        break;
    case I915_RPS_BETWEEN:
        if (val <= rps->efficient_freq && val < rps->cur_freq) {
            mode = I915_RPS_LOW_POWER;
        } else if (val >= rps->rp0_freq && val > rps->cur_freq) {
            mode = I915_RPS_HIGH_POWER;
        }
        break;
    case I915_RPS_HIGH_POWER:
        if (val < (rps->rp1_freq + rps->rp0_freq) >> 1 && val < rps->cur_freq) {
            mode = I915_RPS_BETWEEN;
        }
        break;
    default: /* none written yet: only the limits choose */
        break;
    }
    if (val <= rps->min_freq_softlimit) {
        mode = I915_RPS_LOW_POWER;
    }
    if (val >= rps->max_freq_softlimit) {
        mode = I915_RPS_HIGH_POWER;
    }
    if (rps->interactive) {
        mode = I915_RPS_HIGH_POWER;
    }
    rps_set_power(i915, mode);
}

/** Ask the chip for the frequency `val` through GEN6_RPNSWREQ, and with
 * `update` the thresholds of its power mode, as Linux's rps_set. Under the
 * frequency lock. */
static void rps_set(struct drm_i915_private *i915, int val, int update)
{
    struct i915_rps *rps = &i915->rps;

    if (val == rps->last_freq) {
        return;
    }
    i915_write(&i915->hw, GEN6_RPNSWREQ, HSW_FREQUENCY(val));
    if (update) {
        rps_set_thresholds(i915, val);
    }
    rps->last_freq = val;
}

/** GEN6_RP_INTERRUPT_LIMITS for the frequency `val`: the upper softlimit,
 * and the lower only once it is reached, as Linux's rps_limits. */
static uint32_t rps_limits(const struct i915_rps *rps, int val)
{
    uint32_t limits = (uint32_t)rps->max_freq_softlimit << 24;

    if (val <= rps->min_freq_softlimit) {
        limits |= (uint32_t)rps->min_freq_softlimit << 16;
    }
    return limits;
}

/** GEN6_PMINTRMSK for the frequency `val`: the down events unmasked above
 * the lower softlimit and the up events below the upper, as Linux's
 * rps_pm_mask; `pm_intrmsk_mbz` is never masked. */
static uint32_t rps_pm_mask(const struct i915_rps *rps, int val)
{
    uint32_t mask = 0;

    if (val > rps->min_freq_softlimit) {
        mask |= GEN6_PM_RP_UP_EI_EXPIRED | GEN6_PM_RP_DOWN_THRESHOLD | GEN6_PM_RP_DOWN_TIMEOUT;
    }
    if (val < rps->max_freq_softlimit) {
        mask |= GEN6_PM_RP_UP_EI_EXPIRED | GEN6_PM_RP_UP_THRESHOLD;
    }
    mask &= rps->pm_events;
    return ~mask & ~rps->pm_intrmsk_mbz;
}

int32_t i915_rps_set_locked(struct drm_i915_private *i915, int val)
{
    struct i915_rps *rps = &i915->rps;

    if (val < rps->min_freq || val > rps->max_freq) {
        return -EINVAL;
    }
    if (rps->active) {
        rps_set(i915, val, 1);
        if (rps->interrupts) {
            i915_write(&i915->hw, GEN6_RP_INTERRUPT_LIMITS, rps_limits(rps, val));
            i915_write(&i915->hw, GEN6_PMINTRMSK, rps_pm_mask(rps, val));
        }
    }
    rps->cur_freq = val;
    return 0;
}

/** A PM bank interrupt: the RPS events masked until the work has taken
 * them, as Linux's gen6_rps_irq_handler. Under the interrupt mask's lock. */
static void rps_irq(struct drm_i915_private *i915, uint32_t iir, void *data)
{
    uint32_t events = iir & i915->rps.pm_events;

    (void)data;
    if (events == 0) {
        return;
    }
    i915_irq_mask_locked(i915, I915_IRQ_PM, events);
    i915->rps.pm_iir |= events;
}

/** Turn the up and down interrupts on, as Linux's rps_enable_interrupts:
 * their handler registered and unmasked, then GEN6_PMINTRMSK, written
 * through the GT FIFO as Linux's intel_uncore_write, where the frequency's
 * own registers are written directly, as Linux's set() writes them. */
static void rps_enable_interrupts(struct drm_i915_private *i915)
{
    struct i915_rps *rps = &i915->rps;
    uint32_t flags;

    if (i915_irq_register(i915, I915_IRQ_PM, rps->pm_events, rps_irq, 0) < 0) {
        klog(KLOG_ERR, "i915", "rps: the up and down interrupts are taken");
        return;
    }
    i915_irq_unmask(i915, I915_IRQ_PM, rps->pm_events);
    flags = spin_lock_irqsave(&rps->lock);
    i915_uncore_write(i915, GEN6_PMINTRMSK, rps_pm_mask(rps, rps->last_freq));
    spin_unlock_irqrestore(&rps->lock, flags);
}

/**
 * Turn the up and down interrupts off, as Linux's rps_disable_interrupts:
 * every RPS event masked in GEN6_PMINTRMSK, through the GT FIFO, and in
 * the PM bank and its handler unregistered, then what latched cleared,
 * twice, as Linux's gen6_gt_pm_reset_iir, and the work's events dropped.
 */
static void rps_disable_interrupts(struct drm_i915_private *i915)
{
    struct i915_rps *rps = &i915->rps;
    uint32_t flags;

    i915_uncore_write(i915, GEN6_PMINTRMSK, ~0u & ~rps->pm_intrmsk_mbz);
    i915_irq_unregister(i915, I915_IRQ_PM, GEN6_PM_RPS_EVENTS);
    flags = spin_lock_irqsave(&i915->irq.lock);
    i915_write(&i915->hw, GEN6_PMIIR, GEN6_PM_RPS_EVENTS);
    i915_posting_read(&i915->hw, GEN6_PMIIR);
    i915_write(&i915->hw, GEN6_PMIIR, GEN6_PM_RPS_EVENTS);
    i915_posting_read(&i915->hw, GEN6_PMIIR);
    rps->pm_iir = 0;
    spin_unlock_irqrestore(&i915->irq.lock, flags);
}

void i915_rps_init(struct drm_i915_private *i915)
{
    struct i915_rps *rps = &i915->rps;
    uint32_t cap, ddcc = 0, params = 0;

    k_memset(rps, 0, sizeof(*rps));
    cap = i915_uncore_read(i915, GEN6_RP_STATE_CAP);
    rps->rp0_freq = (int)(cap & 0xffu);
    rps->rp1_freq = (int)((cap >> 8) & 0xffu);
    rps->min_freq = (int)((cap >> 16) & 0xffu);
    rps->max_freq = rps->rp0_freq;
    rps->efficient_freq = rps->rp1_freq;
    if (i915_pcode_read(i915, HSW_PCODE_DYNAMIC_DUTY_CYCLE_CONTROL, &ddcc, 0) == 0) {
        rps->efficient_freq = rps_clamp((int)((ddcc >> 8) & 0xffu), rps->min_freq, rps->max_freq);
    }
    rps->max_freq_softlimit = rps->max_freq;
    rps->min_freq_softlimit = rps->min_freq;
    (void)i915_pcode_read(i915, GEN6_READ_OC_PARAMS, &params, 0);
    if (params & (1u << 31)) {
        klog(KLOG_DEBUG, "i915", "rps: overclocking supported: max %d MHz, overclock %d MHz",
             rps->max_freq * RPS_MHZ, (int)(params & 0xffu) * RPS_MHZ);
        rps->max_freq = (int)(params & 0xffu);
    }
    rps->up_threshold = 95;
    rps->down_threshold = 85;
    rps->boost_freq = rps->max_freq;
    rps->idle_freq = rps->min_freq;
    rps->cur_freq = rps->efficient_freq;
    rps->last_freq = -1;
    rps->power_mode = -1;
    /* Haswell can hang on a looping batch with the up interval's expiry
     * masked (Linux's intel_rps_init). */
    rps->pm_intrmsk_mbz = GEN6_PM_RP_UP_EI_EXPIRED;
    i915->irq.deferred = i915_rps_work;
    rps_disable_interrupts(i915);
    klog(KLOG_INFO, "i915", "rps: RPn %d MHz, RP1 %d MHz, RP0 %d MHz, RPe %d MHz, max %d MHz",
         rps->min_freq * RPS_MHZ, rps->rp1_freq * RPS_MHZ, rps->rp0_freq * RPS_MHZ,
         rps->efficient_freq * RPS_MHZ, rps->max_freq * RPS_MHZ);
}

/**
 * Load the ring's frequency for each GPU frequency into the pcode, from
 * the highest down, as Linux's gen6_update_ring_freq on Haswell: the
 * ring at 5/4 of the GPU's frequency, never below the memory's from DCLK,
 * and the IA's left to cpufreq. A write the pcode refuses is logged by
 * the mailbox and the next goes on, as on Linux.
 */
static void rps_ring_freq(struct drm_i915_private *i915)
{
    struct i915_rps *rps = &i915->rps;
    int gpu, ring, min_ring;

    if (!i915->id || !i915->id->platform->has_llc || rps->max_freq <= rps->min_freq) {
        return;
    }
    /* DDR's frequency in units of 266.6 MHz, as bandwidth. */
    min_ring = (int)(i915_uncore_read(i915, DCLK) & 0xfu) * 8 / 3;
    for (gpu = rps->max_freq; gpu >= rps->min_freq; gpu--) {
        ring = gpu * 5 / 4;
        if (ring < min_ring) {
            ring = min_ring;
        }
        (void)i915_pcode_write(i915, GEN6_PCODE_WRITE_MIN_FREQ_TABLE,
                               (uint32_t)ring << GEN6_PCODE_FREQ_RING_RATIO_SHIFT | (uint32_t)gpu);
    }
}

void i915_rps_enable(struct drm_i915_private *i915)
{
    struct i915_rps *rps = &i915->rps;
    uint32_t flags;
    int32_t fw;

    if (rps->enabled) {
        return;
    }
    fw = i915_forcewake_get(i915);
    if (rps->max_freq > rps->min_freq) {
        /* Down to RPe or RPn after 50 ms of idle. */
        i915_write(&i915->hw, GEN6_RP_DOWN_TIMEOUT, 50000);
        i915_write(&i915->hw, GEN6_RP_IDLE_HYSTERSIS, 10);
        flags = spin_lock_irqsave(&rps->lock);
        rps->pm_events =
            GEN6_PM_RP_UP_THRESHOLD | GEN6_PM_RP_DOWN_THRESHOLD | GEN6_PM_RP_DOWN_TIMEOUT;
        rps->power_mode = -1;
        rps->last_freq = -1;
        rps_set(i915, rps->min_freq, 1);
        rps->cur_freq = rps->min_freq;
        rps->interrupts = 1;
        rps->enabled = 1;
        spin_unlock_irqrestore(&rps->lock, flags);
    }
    if (fw == 0) {
        i915_forcewake_put(i915);
    }
    rps_ring_freq(i915);
    if (!rps->enabled) {
        klog(KLOG_INFO, "i915", "rps: off: no range from %d MHz to %d MHz", rps->min_freq * RPS_MHZ,
             rps->max_freq * RPS_MHZ);
    }
}

void i915_rps_disable(struct drm_i915_private *i915)
{
    struct i915_rps *rps = &i915->rps;

    if (!rps->enabled) {
        return;
    }
    if (rps->active) {
        rps->active = 0;
        rps_disable_interrupts(i915);
    }
    rps->enabled = 0;
    rps->interrupts = 0;
    i915_write(&i915->hw, GEN6_RP_CONTROL, 0);
}

void i915_rps_unpark(struct drm_i915_private *i915)
{
    struct i915_rps *rps = &i915->rps;
    uint32_t flags;

    if (!rps->enabled) {
        return;
    }
    flags = spin_lock_irqsave(&rps->lock);
    rps->active = 1;
    rps->unparks++;
    (void)i915_rps_set_locked(
        i915, rps_clamp(rps->cur_freq, rps->min_freq_softlimit, rps->max_freq_softlimit));
    spin_unlock_irqrestore(&rps->lock, flags);
    flags = spin_lock_irqsave(&i915->irq.lock);
    rps->pm_iir = 0;
    spin_unlock_irqrestore(&i915->irq.lock, flags);
    if (rps->interrupts) {
        rps_enable_interrupts(i915);
    }
}

void i915_rps_park(struct drm_i915_private *i915)
{
    struct i915_rps *rps = &i915->rps;
    uint32_t flags;
    int32_t fw;
    int adj;

    if (!rps->enabled || !rps->active) {
        return;
    }
    flags = spin_lock_irqsave(&rps->lock);
    rps->active = 0;
    rps->parks++;
    spin_unlock_irqrestore(&rps->lock, flags);
    if (rps->interrupts) {
        rps_disable_interrupts(i915);
    }
    flags = spin_lock_irqsave(&rps->lock);
    if (rps->last_freq <= rps->idle_freq) {
        spin_unlock_irqrestore(&rps->lock, flags);
        return;
    }
    /* The punit delays a change while the GT sleeps; forcewake makes it
     * take the idle frequency and voltage now. */
    fw = i915_forcewake_get(i915);
    rps_set(i915, rps->idle_freq, 0);
    if (fw == 0) {
        i915_forcewake_put(i915);
    }
    /* Start lower at the next unpark, as after a down interrupt. */
    adj = rps->last_adj;
    adj = adj < 0 ? adj * 2 : -2;
    rps->last_adj = adj;
    rps->cur_freq = rps->cur_freq + adj > rps->min_freq ? rps->cur_freq + adj : rps->min_freq;
    if (rps->cur_freq < rps->efficient_freq) {
        rps->cur_freq = rps->efficient_freq;
        rps->last_adj = 0;
    }
    spin_unlock_irqrestore(&rps->lock, flags);
}

void i915_rps_work(struct drm_i915_private *i915)
{
    struct i915_rps *rps = &i915->rps;
    uint32_t flags, pm_iir;
    int adj, new_freq, max, client_boost, queued;

    flags = spin_lock_irqsave(&i915->irq.lock);
    pm_iir = rps->pm_iir & rps->pm_events;
    rps->pm_iir = 0;
    queued = rps->boost_queued;
    rps->boost_queued = 0;
    spin_unlock_irqrestore(&i915->irq.lock, flags);
    if (pm_iir == 0 && !queued) {
        return;
    }
    flags = spin_lock_irqsave(&rps->lock);
    client_boost = rps->num_waiters > 0;
    if (!rps->active || (pm_iir == 0 && !client_boost)) {
        spin_unlock_irqrestore(&rps->lock, flags);
        return;
    }
    rps->up_events += (pm_iir & GEN6_PM_RP_UP_THRESHOLD) != 0;
    rps->down_events += (pm_iir & GEN6_PM_RP_DOWN_THRESHOLD) != 0;
    rps->timeout_events += (pm_iir & GEN6_PM_RP_DOWN_TIMEOUT) != 0;
    adj = rps->last_adj;
    new_freq = rps->cur_freq;
    max = client_boost ? rps->max_freq : rps->max_freq_softlimit;
    if (client_boost && new_freq < rps->boost_freq) {
        new_freq = rps->boost_freq;
        adj = 0;
    } else if (pm_iir & GEN6_PM_RP_UP_THRESHOLD) {
        adj = adj > 0 ? adj * 2 : 1;
        if (new_freq >= rps->max_freq_softlimit) {
            adj = 0;
        }
    } else if (client_boost) {
        adj = 0;
    } else if (pm_iir & GEN6_PM_RP_DOWN_TIMEOUT) {
        if (rps->cur_freq > rps->efficient_freq) {
            new_freq = rps->efficient_freq;
        } else if (rps->cur_freq > rps->min_freq_softlimit) {
            new_freq = rps->min_freq_softlimit;
        }
        adj = 0;
    } else if (pm_iir & GEN6_PM_RP_DOWN_THRESHOLD) {
        adj = adj < 0 ? adj * 2 : -1;
        if (new_freq <= rps->min_freq_softlimit) {
            adj = 0;
        }
    } else {
        adj = 0;
    }
    new_freq = rps_clamp(new_freq + adj, rps->min_freq_softlimit, max);
    if (i915_rps_set_locked(i915, new_freq) < 0) {
        adj = 0;
    }
    rps->last_adj = adj;
    rps->work_runs++;
    spin_unlock_irqrestore(&rps->lock, flags);
    i915_irq_unmask(i915, I915_IRQ_PM, rps->pm_events);
}

uint32_t i915_rps_read_actual_mhz(struct drm_i915_private *i915)
{
    uint32_t rpstat = i915_uncore_read(i915, GEN6_RPSTAT1);

    return ((rpstat & HSW_CAGF_MASK) >> HSW_CAGF_SHIFT) * RPS_MHZ;
}

int32_t i915_rps_set_max_mhz(struct drm_i915_private *i915, uint32_t mhz)
{
    struct i915_rps *rps = &i915->rps;
    int32_t err = 0;
    uint32_t flags = spin_lock_irqsave(&rps->lock);
    int val = rps_opcode(mhz);

    if (val < rps->min_freq || val > rps->max_freq || val < rps->min_freq_softlimit) {
        err = -EINVAL;
    } else {
        if (val > rps->rp0_freq) {
            klog(KLOG_DEBUG, "i915", "rps: overclocking to %d MHz asked for", val * RPS_MHZ);
        }
        rps->max_freq_softlimit = val;
        /* Asked again even when unchanged, for the new interrupt limits
         * and mask. */
        (void)i915_rps_set_locked(
            i915, rps_clamp(rps->cur_freq, rps->min_freq_softlimit, rps->max_freq_softlimit));
    }
    spin_unlock_irqrestore(&rps->lock, flags);
    return err;
}

int32_t i915_rps_set_min_mhz(struct drm_i915_private *i915, uint32_t mhz)
{
    struct i915_rps *rps = &i915->rps;
    int32_t err = 0;
    uint32_t flags = spin_lock_irqsave(&rps->lock);
    int val = rps_opcode(mhz);

    if (val < rps->min_freq || val > rps->max_freq || val > rps->max_freq_softlimit) {
        err = -EINVAL;
    } else {
        rps->min_freq_softlimit = val;
        (void)i915_rps_set_locked(
            i915, rps_clamp(rps->cur_freq, rps->min_freq_softlimit, rps->max_freq_softlimit));
    }
    spin_unlock_irqrestore(&rps->lock, flags);
    return err;
}

/** Ask for the work as Linux's queue_work of rps_work, and run it, since
 * nothing here defers it. No lock of the driver held. */
static void rps_queue_work(struct drm_i915_private *i915)
{
    uint32_t flags = spin_lock_irqsave(&i915->irq.lock);

    i915->rps.boost_queued = 1;
    spin_unlock_irqrestore(&i915->irq.lock, flags);
    i915_rps_work(i915);
}

int32_t i915_rps_set_boost_mhz(struct drm_i915_private *i915, uint32_t mhz)
{
    struct i915_rps *rps = &i915->rps;
    uint32_t flags;
    int val = rps_opcode(mhz), boost = 0;

    if (val < rps->min_freq || val > rps->max_freq) {
        return -EINVAL;
    }
    flags = spin_lock_irqsave(&rps->lock);
    if (val != rps->boost_freq) {
        rps->boost_freq = val;
        boost = rps->num_waiters > 0;
    }
    spin_unlock_irqrestore(&rps->lock, flags);
    if (boost) {
        rps_queue_work(i915);
    }
    return 0;
}

void i915_rps_boost(struct i915_request *rq)
{
    struct drm_i915_private *i915 = rq->i915;
    struct i915_rps *rps = &i915->rps;
    uint32_t flags;
    int queue = 0;

    if (rq->fence.base.signaled || rq->boosted) {
        return;
    }
    /* The mark is tested and set under the lock the retire takes. */
    flags = spin_lock_irqsave(&rps->lock);
    if (!rq->boosted) {
        rq->boosted = 1;
        if (rps->num_waiters++ == 0 && rps->active) {
            queue = rps->cur_freq < rps->boost_freq;
            rps->boosts++;
        }
    }
    spin_unlock_irqrestore(&rps->lock, flags);
    if (queue) {
        rps_queue_work(i915);
    }
}

void i915_rps_retire(struct i915_request *rq)
{
    struct i915_rps *rps = &rq->i915->rps;
    uint32_t flags = spin_lock_irqsave(&rps->lock);

    if (rq->boosted) {
        rps->num_waiters--;
    }
    rq->boosted = 1;
    spin_unlock_irqrestore(&rps->lock, flags);
}

void i915_rps_mark_interactive(struct drm_i915_private *i915, int on)
{
    struct i915_rps *rps = &i915->rps;
    uint32_t flags = spin_lock_irqsave(&rps->lock);

    if (on) {
        if (rps->interactive++ == 0 && rps->active) {
            rps_set_power(i915, I915_RPS_HIGH_POWER);
        }
    } else if (rps->interactive > 0) {
        rps->interactive--;
    }
    spin_unlock_irqrestore(&rps->lock, flags);
}

/* ---- the /sys files ---- */

/* The `arg` of a frequency file. */
#define SYSFS_ACT           0
#define SYSFS_CUR           1
#define SYSFS_BOOST         2
#define SYSFS_MAX           3
#define SYSFS_MIN           4
#define SYSFS_RP0           5
#define SYSFS_RP1           6
#define SYSFS_RPN           7
#define SYSFS_RC6_ENABLE    0
#define SYSFS_RC6_RESIDENCY 1

/** Write `v` into `buf`, `cap` bytes, in `radix` 10 or 16 with a newline,
 * as sysfs_emit's "%u\n" and "%x\n": the bytes it takes, which may pass
 * `cap`. */
static int32_t sysfs_emit_u32(char *buf, uint32_t cap, uint32_t v, uint32_t radix)
{
    char digits[11];
    int32_t n = 0, len, i;

    do {
        digits[n++] = "0123456789abcdef"[v % radix];
        v /= radix;
    } while (v);
    len = n + 1;
    for (i = 0; i < n && (uint32_t)i < cap; i++) {
        buf[i] = digits[n - 1 - i];
    }
    if ((uint32_t)n < cap) {
        buf[n] = '\n';
    }
    return len;
}

/** The value of the digit `c` in any radix up to 16, or 16 for none. */
static uint32_t sysfs_digit(char c)
{
    if (c >= '0' && c <= '9') {
        return (uint32_t)(c - '0');
    }
    if (c >= 'a' && c <= 'f') {
        return (uint32_t)(c - 'a') + 10u;
    }
    if (c >= 'A' && c <= 'F') {
        return (uint32_t)(c - 'A') + 10u;
    }
    return 16;
}

/** The radix of the number at `buf[*i]`, as Linux's
 * _parse_integer_fixup_radix with base 0: 16 after `0x` and a hex digit,
 * with `*i` moved past the `0x`; 8 after `0`; 10 otherwise. */
static uint32_t sysfs_radix(const char *buf, uint32_t len, uint32_t *i)
{
    if (*i + 2 < len && buf[*i] == '0' && (buf[*i + 1] == 'x' || buf[*i + 1] == 'X') &&
        sysfs_digit(buf[*i + 2]) < 16) {
        *i += 2;
        return 16;
    }
    return *i < len && buf[*i] == '0' ? 8 : 10;
}

/**
 * Parse the `len` bytes at `buf` as an unsigned 32-bit number, as Linux's
 * kstrtou32 with base 0: an optional `+`, then hexadecimal after `0x`,
 * octal after `0`, decimal otherwise, and at most one newline after.
 *
 * @return 0 with the number in `*out`; -EINVAL for no digits or anything
 *         else; -ERANGE past 2^32 - 1.
 */
static int32_t sysfs_parse_u32(const char *buf, uint32_t len, uint32_t *out)
{
    uint32_t i = len > 0 && buf[0] == '+' ? 1 : 0, radix, v = 0, digit, ndigits = 0;

    radix = sysfs_radix(buf, len, &i);
    for (; i < len && (digit = sysfs_digit(buf[i])) < radix; i++) {
        if (v > (0xffffffffu - digit) / radix) {
            return -ERANGE;
        }
        v = v * radix + digit;
        ndigits++;
    }
    if (i < len && buf[i] == '\n') {
        i++;
    }
    if (ndigits == 0 || i != len) {
        return -EINVAL;
    }
    *out = v;
    return 0;
}

/** A frequency file of the card: its value in MHz. */
static int32_t show_freq(void *obj, int arg, char *buf, uint32_t cap)
{
    struct drm_i915_private *i915 = (struct drm_i915_private *)obj;
    struct i915_rps *rps = &i915->rps;
    uint32_t mhz;

    switch (arg) {
    case SYSFS_ACT:
        mhz = i915_rps_read_actual_mhz(i915);
        break;
    case SYSFS_CUR:
        mhz = (uint32_t)rps->cur_freq * RPS_MHZ;
        break;
    case SYSFS_BOOST:
        mhz = (uint32_t)rps->boost_freq * RPS_MHZ;
        break;
    case SYSFS_MAX:
        mhz = (uint32_t)rps->max_freq_softlimit * RPS_MHZ;
        break;
    case SYSFS_MIN:
        mhz = (uint32_t)rps->min_freq_softlimit * RPS_MHZ;
        break;
    case SYSFS_RP0:
        mhz = (uint32_t)rps->rp0_freq * RPS_MHZ;
        break;
    case SYSFS_RP1:
        mhz = (uint32_t)rps->rp1_freq * RPS_MHZ;
        break;
    default:
        mhz = (uint32_t)rps->min_freq * RPS_MHZ;
        break;
    }
    return sysfs_emit_u32(buf, cap, mhz, 10);
}

/** A write of gt_boost_freq_mhz, gt_max_freq_mhz or gt_min_freq_mhz. */
static int32_t store_freq(void *obj, int arg, const char *buf, uint32_t len)
{
    struct drm_i915_private *i915 = (struct drm_i915_private *)obj;
    uint32_t mhz;
    int32_t err = sysfs_parse_u32(buf, len, &mhz);

    if (err == 0) {
        err = arg == SYSFS_BOOST ? i915_rps_set_boost_mhz(i915, mhz)
              : arg == SYSFS_MAX ? i915_rps_set_max_mhz(i915, mhz)
                                 : i915_rps_set_min_mhz(i915, mhz);
    }
    return err < 0 ? err : (int32_t)len;
}

/** power/rc6_enable, in hexadecimal as Linux's, and
 * power/rc6_residency_ms, cut to 32 bits as Linux's. */
static int32_t show_rc6(void *obj, int arg, char *buf, uint32_t cap)
{
    struct drm_i915_private *i915 = (struct drm_i915_private *)obj;

    if (arg == SYSFS_RC6_ENABLE) {
        return sysfs_emit_u32(buf, cap, i915->rc6.enabled, 16);
    }
    return sysfs_emit_u32(buf, cap, (uint32_t)i915_rc6_residency_ms(i915), 10);
}

/** show_freq with the device held awake, as Linux's frequency reads take
 * a wakeref. */
static int32_t show_freq_awake(void *obj, int arg, char *buf, uint32_t cap)
{
    int32_t rc;

    i915_rpm_get((struct drm_i915_private *)obj);
    rc = show_freq(obj, arg, buf, cap);
    i915_rpm_put((struct drm_i915_private *)obj);
    return rc;
}

/** store_freq with the device held awake. */
static int32_t store_freq_awake(void *obj, int arg, const char *buf, uint32_t len)
{
    int32_t rc;

    i915_rpm_get((struct drm_i915_private *)obj);
    rc = store_freq(obj, arg, buf, len);
    i915_rpm_put((struct drm_i915_private *)obj);
    return rc;
}

/** show_rc6 with the device held awake, as Linux's residency read. */
static int32_t show_rc6_awake(void *obj, int arg, char *buf, uint32_t cap)
{
    int32_t rc;

    i915_rpm_get((struct drm_i915_private *)obj);
    rc = show_rc6(obj, arg, buf, cap);
    i915_rpm_put((struct drm_i915_private *)obj);
    return rc;
}

/* Linux's gen6_gt_attrs, in its order. */
static const struct sysfs_attr freq_attrs[] = {
    {"gt_act_freq_mhz", 0444u, show_freq_awake, 0},
    {"gt_cur_freq_mhz", 0444u, show_freq_awake, 0},
    {"gt_boost_freq_mhz", 0644u, show_freq_awake, store_freq_awake},
    {"gt_max_freq_mhz", 0644u, show_freq_awake, store_freq_awake},
    {"gt_min_freq_mhz", 0644u, show_freq_awake, store_freq_awake},
    {"gt_RP0_freq_mhz", 0444u, show_freq_awake, 0},
    {"gt_RP1_freq_mhz", 0444u, show_freq_awake, 0},
    {"gt_RPn_freq_mhz", 0444u, show_freq_awake, 0}};

static const struct sysfs_attr rc6_attrs[] = {{"rc6_enable", 0444u, show_rc6_awake, 0},
                                              {"rc6_residency_ms", 0444u, show_rc6_awake, 0}};

void i915_sysfs_register(struct drm_i915_private *i915)
{
    int card = drm_sysfs_card_dir(&i915->drm), power, h;
    unsigned i;

    if (card <= 0) {
        return;
    }
    for (i = 0; i < sizeof(freq_attrs) / sizeof(freq_attrs[0]); i++) {
        h = sysfs_add_attr(card, &freq_attrs[i], i915, (int)i);
        if (h < 0) {
            klog(KLOG_WARN, "i915", "sysfs: %s not added: error %d", freq_attrs[i].name, h);
        }
    }
    if (!i915->rc6.supported) {
        return;
    }
    power = sysfs_add_dir(card, "power");
    if (power == -EEXIST) {
        power = sysfs_node_child(card, "power", 5);
    }
    if (power <= 0) {
        klog(KLOG_WARN, "i915", "sysfs: power not added: error %d", power);
        return;
    }
    for (i = 0; i < sizeof(rc6_attrs) / sizeof(rc6_attrs[0]); i++) {
        h = sysfs_add_attr(power, &rc6_attrs[i], i915, (int)i);
        if (h < 0) {
            klog(KLOG_WARN, "i915", "sysfs: power/%s not added: error %d", rc6_attrs[i].name, h);
        }
    }
}
