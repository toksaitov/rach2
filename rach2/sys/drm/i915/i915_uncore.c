/*
 * i915_uncore.c: forcewake and the GT FIFO of Haswell, through which the
 * driver reads and writes the registers of the GT, after Linux 6.12's
 * intel_uncore.c.
 */

#include "i915.h"
#include "i915_reg.h"

#include "../../klog.h"
#include "../../lib/kerrno.h"

/* The bounds of Linux's waits: an acknowledgement of forcewake
 * (FORCEWAKE_ACK_TIMEOUT_MS), the GT's threads leaving C0's sleep, and
 * room in the GT FIFO (GT_FIFO_TIMEOUT_MS). */
#define I915_FW_ACK_US    50000u
#define I915_FW_THREAD_US 5000u
#define I915_GT_FIFO_US   10000u

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

int32_t i915_forcewake_get(struct drm_i915_private *i915)
{
    struct i915_uncore *u = &i915->uncore;
    uint32_t flags = spin_lock_irqsave(&u->lock);
    int32_t err = 0;

    if (u->wake_count == 0) {
        err = i915_fw_wake(i915);
    }
    if (err == 0) {
        u->wake_count++;
    }
    spin_unlock_irqrestore(&u->lock, flags);
    return err;
}

void i915_forcewake_put(struct drm_i915_private *i915)
{
    struct i915_uncore *u = &i915->uncore;
    uint32_t flags = spin_lock_irqsave(&u->lock);

    if (u->wake_count == 0) {
        spin_unlock_irqrestore(&u->lock, flags);
        klog(KLOG_ERR, "i915", "forcewake: put with no reference held");
        return;
    }
    if (--u->wake_count == 0) {
        i915_write(&i915->hw, FORCEWAKE_MT, I915_MASKED_DISABLE(FORCEWAKE_KERNEL));
    }
    spin_unlock_irqrestore(&u->lock, flags);
}

uint32_t i915_uncore_read(struct drm_i915_private *i915, uint32_t reg)
{
    uint32_t value;

    if (!i915_gt_reg(reg)) {
        return i915_read(&i915->hw, reg);
    }
    if (i915_forcewake_get(i915) < 0) {
        return 0xffffffffu;
    }
    value = i915_read(&i915->hw, reg);
    i915_forcewake_put(i915);
    return value;
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
