/*
 * i915_uncore.c: forcewake and the GT FIFO of Haswell, through which the
 * driver reads and writes the registers of the GT, after Linux 6.12's
 * intel_uncore.c.
 */

#include "i915.h"
#include "i915_reg.h"

#include "../../idle_timer.h"
#include "../../rtc.h"
#include "../../klog.h"
#include "../../lib/kerrno.h"

/* The bounds of Linux's waits: an acknowledgement of forcewake
 * (FORCEWAKE_ACK_TIMEOUT_MS), the GT's threads leaving C0's sleep, and
 * room in the GT FIFO (GT_FIFO_TIMEOUT_MS). */
#define I915_FW_ACK_US    50000u
#define I915_FW_THREAD_US 5000u
#define I915_GT_FIFO_US   10000u

/* Linux's one-millisecond forcewake timer allows a millisecond of slack.
 * Its two-millisecond deadline is served by the boot processor's separate
 * millisecond service event, independently of the scheduler tick. */
#define I915_FW_TIMER_NS 2000000u

/* The GT's registers lie below 0x40000, the one range of Linux's
 * __gen6_fw_ranges, and need forcewake to read. A write waits for room in
 * the GT FIFO below 0x40000 and from 0x116000, Linux's NEEDS_FORCE_WAKE,
 * which gen6_write tests. */
#define I915_GT_REG_END   0x40000u
#define I915_GT_FIFO_HIGH 0x116000u

/** Whether `reg` is a register of the GT, which a read wakes. */
static int i915_gt_reg(uint32_t reg)
{
    return reg < I915_GT_REG_END;
}

/** Whether a write to `reg` goes through the GT FIFO. */
static int i915_gt_fifo_reg(uint32_t reg)
{
    return reg < I915_GT_REG_END || reg >= I915_GT_FIFO_HIGH;
}

void i915_uncore_init(struct drm_i915_private *i915)
{
    struct i915_hw *hw = &i915->hw;
    uint32_t dbg;

    if (i915_read(hw, FPGA_DBG) & FPGA_DBG_RM_NOCLAIM) {
        klog(KLOG_INFO, "i915", "uncore: an unclaimed access before the bind, cleared");
        i915_write(hw, FPGA_DBG, FPGA_DBG_RM_NOCLAIM);
    }
    dbg = i915_read(hw, GTFIFODBG);
    if (dbg != 0) {
        klog(KLOG_INFO, "i915", "uncore: GT FIFO errors 0x%x before the bind, cleared",
             (unsigned)dbg);
        i915_write(hw, GTFIFODBG, dbg);
    }
    /* WaRsClearFWBitsAtReset: every request bit, the firmware's too. */
    i915_write(hw, FORCEWAKE_MT, I915_MASKED_DISABLE(0xffffu));
    i915->uncore.wake_count = 0;
    i915->uncore.timer_armed = 0;
    i915->uncore.active = 0;
    i915->uncore.gone = 0;
    i915->uncore.fifo_count = i915_read(hw, GTFIFOCTL) & GT_FIFO_FREE_ENTRIES_MASK;
}

/**
 * Wake the GT: wait for the last release's acknowledgement to clear, set
 * the kernel's request, wait for its acknowledgement, then for the GT's
 * threads. Under the register lock. 0, or -ETIMEDOUT with no request left
 * set.
 */
static int32_t i915_fw_wake(struct drm_i915_private *i915)
{
    struct i915_hw *hw = &i915->hw;
    int32_t err;

    err = i915_wait_for(hw, "forcewake ack clear", FORCEWAKE_ACK_HSW, FORCEWAKE_KERNEL, 0,
                        I915_FW_ACK_US);
    if (err < 0) {
        return err;
    }
    i915_write(hw, FORCEWAKE_MT, I915_MASKED_ENABLE(FORCEWAKE_KERNEL));
    err = i915_wait_for(hw, "forcewake ack", FORCEWAKE_ACK_HSW, FORCEWAKE_KERNEL, FORCEWAKE_KERNEL,
                        I915_FW_ACK_US);
    if (err < 0) {
        i915_write(hw, FORCEWAKE_MT, I915_MASKED_DISABLE(FORCEWAKE_KERNEL));
        return err;
    }
    /* WaRsForcewakeWaitTC0: Linux warns and goes on. */
    if (i915_wait_for(hw, "gt thread c0", GEN6_GT_THREAD_STATUS_REG,
                      GEN6_GT_THREAD_STATUS_CORE_MASK, 0, I915_FW_THREAD_US) < 0) {
        i915->uncore.thread_timeouts++;
    }
    return 0;
}

/** Whether forcewake is held on the chip: a reference is held or the
 * release timer is armed. Under the register lock. */
static int i915_fw_held(const struct i915_uncore *u)
{
    return u->wake_count != 0 || u->timer_armed;
}

/** Arm the release timer `ns` nanoseconds from now. Under the register lock. */
static void i915_fw_timer_at(struct i915_uncore *u, uint32_t ns)
{
    u->timer_due = (uint64_t)rtc_monotonic_ns() + ns;
    u->timer_armed = 1;
}

/** Arm the release timer, which withdraws the request two milliseconds
 * later unless a reference is held then (`i915_uncore_tick`), as Linux's
 * fw_domain_arm_timer. Under the register lock, with forcewake held. */
static void i915_fw_arm_timer(struct i915_uncore *u)
{
    i915_fw_timer_at(u, I915_FW_TIMER_NS);
    u->active = 0;
}

/** Withdraw the kernel's request. Under the register lock, once nothing
 * holds forcewake. */
static void i915_fw_release(struct drm_i915_private *i915)
{
    i915_write(&i915->hw, FORCEWAKE_MT, I915_MASKED_DISABLE(FORCEWAKE_KERNEL));
}

int32_t i915_forcewake_get(struct drm_i915_private *i915)
{
    struct i915_uncore *u = &i915->uncore;
    uint32_t flags = spin_lock_irqsave(&u->lock);
    int32_t err = 0;

    if (u->gone) {
        err = -ENODEV;
    } else if (i915->hw.asleep) {
        err = -EIO;
    } else if (!i915_fw_held(u)) {
        err = i915_fw_wake(i915);
    } else if (u->timer_armed) {
        u->active = 1;
    }
    if (err == 0) {
        u->wake_count++;
    }
    spin_unlock_irqrestore(&u->lock, flags);
    return err;
}

void i915_forcewake_put(struct drm_i915_private *i915)
{
    int64_t release;
    struct i915_uncore *u = &i915->uncore;
    uint32_t flags = spin_lock_irqsave(&u->lock);

    if (u->gone) {
        spin_unlock_irqrestore(&u->lock, flags);
        return;
    }
    if (u->wake_count == 0) {
        spin_unlock_irqrestore(&u->lock, flags);
        klog(KLOG_ERR, "i915", "forcewake: put with no reference held");
        return;
    }
    if (--u->wake_count == 0) {
        if (u->timer_armed) {
            u->active = 1;
        } else {
            i915_fw_arm_timer(u);
        }
    }
    release = u->timer_armed ? (int64_t)u->timer_due : 0;
    spin_unlock_irqrestore(&u->lock, flags);
    timer_service_wake_at(release);
}

/**
 * Have forcewake held for an access that follows under the same hold of
 * the register lock, as Linux's __force_wake_auto: when it is not held,
 * wake the GT and arm the release timer, which lets it go. Under the
 * register lock. 0; -ENODEV once `i915_uncore_fini` has run; the error of
 * `i915_fw_wake`.
 */
static int32_t i915_fw_auto(struct drm_i915_private *i915)
{
    struct i915_uncore *u = &i915->uncore;
    int32_t err;

    if (u->gone) {
        return -ENODEV;
    }
    if (i915->hw.asleep) {
        return -EIO;
    }
    if (i915_fw_held(u)) {
        return 0;
    }
    err = i915_fw_wake(i915);
    if (err == 0) {
        i915_fw_arm_timer(u);
    }
    return err;
}

uint32_t i915_uncore_read(struct drm_i915_private *i915, uint32_t reg)
{
    int64_t release;
    struct i915_uncore *u = &i915->uncore;
    uint32_t flags, value = 0xffffffffu;

    if (!i915_gt_reg(reg)) {
        return i915_read(&i915->hw, reg);
    }
    flags = spin_lock_irqsave(&u->lock);
    if (i915_fw_auto(i915) == 0) {
        value = i915_read(&i915->hw, reg);
    }
    release = u->timer_armed ? (int64_t)u->timer_due : 0;
    spin_unlock_irqrestore(&u->lock, flags);
    timer_service_wake_at(release);
    return value;
}

int32_t i915_uncore_read64(struct drm_i915_private *i915, uint32_t lo, uint32_t hi, int whole,
                           uint64_t *val)
{
    int64_t release;
    struct i915_uncore *u = &i915->uncore;
    uint32_t flags = spin_lock_irqsave(&u->lock);
    int32_t err = i915_fw_auto(i915);

    if (err == 0 && whole) {
        *val = i915_read64_2x32(&i915->hw, lo, hi);
    } else if (err == 0) {
        *val = i915_read(&i915->hw, lo);
        *val |= (uint64_t)i915_read(&i915->hw, hi) << 32;
    }
    release = u->timer_armed ? (int64_t)u->timer_due : 0;
    spin_unlock_irqrestore(&u->lock, flags);
    timer_service_wake_at(release);
    return err;
}

void i915_uncore_tick(struct drm_i915_private *i915)
{
    int64_t release;
    struct i915_uncore *u = &i915->uncore;
    uint32_t flags = spin_lock_irqsave(&u->lock);

    if (u->timer_armed && (uint64_t)rtc_monotonic_ns() >= u->timer_due) {
        u->timer_armed = 0;
        if (u->active) {
            u->active = 0;
            i915_fw_timer_at(u, 1000000u);
        } else if (u->wake_count == 0) {
            i915_fw_release(i915);
        }
    }
    release = u->timer_armed ? (int64_t)u->timer_due : 0;
    spin_unlock_irqrestore(&u->lock, flags);
    timer_service_wake_at(release);
}

uint32_t i915_uncore_idle_ticks(struct drm_i915_private *i915)
{
    struct i915_uncore *u = &i915->uncore;
    uint32_t wait = TICK_HZ, flags = spin_lock_irqsave(&u->lock);

    if (u->timer_armed) {
        wait = 1;
    }
    spin_unlock_irqrestore(&u->lock, flags);
    return wait;
}

void i915_uncore_fini(struct drm_i915_private *i915)
{
    struct i915_uncore *u = &i915->uncore;
    uint32_t flags = spin_lock_irqsave(&u->lock);

    if (!u->gone && i915_fw_held(u)) {
        i915_fw_release(i915);
    }
    u->wake_count = 0;
    u->timer_armed = 0;
    u->active = 0;
    u->gone = 1;
    spin_unlock_irqrestore(&u->lock, flags);
}

int32_t i915_uncore_suspend(struct drm_i915_private *i915)
{
    struct i915_uncore *u = &i915->uncore;
    uint32_t flags = spin_lock_irqsave(&u->lock);

    if (u->wake_count != 0) {
        spin_unlock_irqrestore(&u->lock, flags);
        return -EBUSY;
    }
    if (!u->gone && u->timer_armed) {
        i915_fw_release(i915);
    }
    u->timer_armed = 0;
    u->active = 0;
    spin_unlock_irqrestore(&u->lock, flags);
    if (i915_read(&i915->hw, FPGA_DBG) & FPGA_DBG_RM_NOCLAIM) {
        klog(KLOG_WARN, "i915", "uncore: an unclaimed access before the suspend");
        i915_write(&i915->hw, FPGA_DBG, FPGA_DBG_RM_NOCLAIM);
    }
    return 0;
}

void i915_uncore_resume(struct drm_i915_private *i915)
{
    struct i915_uncore *u = &i915->uncore;
    uint32_t flags;

    if (i915_read(&i915->hw, FPGA_DBG) & FPGA_DBG_RM_NOCLAIM) {
        klog(KLOG_INFO, "i915", "uncore: an unclaimed access while suspended, cleared");
        i915_write(&i915->hw, FPGA_DBG, FPGA_DBG_RM_NOCLAIM);
    }
    flags = spin_lock_irqsave(&u->lock);
    u->fifo_count = i915_read(&i915->hw, GTFIFOCTL) & GT_FIFO_FREE_ENTRIES_MASK;
    spin_unlock_irqrestore(&u->lock, flags);
}

/** The wait's condition: more free entries in the GT FIFO than its
 * reserve, the count left in `*arg`. */
static int i915_fifo_has_room(struct i915_hw *hw, void *arg)
{
    uint32_t *n = (uint32_t *)arg;

    *n = i915_read(hw, GTFIFOCTL) & GT_FIFO_FREE_ENTRIES_MASK;
    return *n > GT_FIFO_NUM_RESERVED_ENTRIES;
}

/**
 * Count one entry of the GT FIFO for a write about to be made, reading the
 * free entries again, and waiting for them, once the count is down to the
 * reserve, as Linux's __gen6_gt_wait_for_fifo. Under the register lock.
 */
static void i915_gt_fifo_wait(struct drm_i915_private *i915)
{
    struct i915_uncore *u = &i915->uncore;
    uint32_t n = u->fifo_count;

    if (n <= GT_FIFO_NUM_RESERVED_ENTRIES &&
        i915_wait_until(&i915->hw, "gt fifo", i915_fifo_has_room, &n, I915_GT_FIFO_US) < 0) {
        u->fifo_timeouts++;
        return;
    }
    u->fifo_count = n - 1;
}

void i915_uncore_write(struct drm_i915_private *i915, uint32_t reg, uint32_t value)
{
    uint32_t flags;

    if (!i915_gt_fifo_reg(reg)) {
        i915_write(&i915->hw, reg, value);
        return;
    }
    flags = spin_lock_irqsave(&i915->uncore.lock);
    i915_gt_fifo_wait(i915);
    i915_write(&i915->hw, reg, value);
    spin_unlock_irqrestore(&i915->uncore.lock, flags);
}
