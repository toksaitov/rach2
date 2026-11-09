/*
 * i915_hw.c: the Intel graphics driver's hardware boundary: the accesses
 * and bounded waits of i915_hw.h, and its backend over BAR 0's registers.
 */

#include "i915_hw.h"

#include "../../klog.h"
#include "../../lib/kerrno.h"
#include "../../mm/paging.h"
#include "../../udelay.h"

/* The longest single wait between two reads of a register in a bounded
 * wait, in microseconds. */
#define I915_WAIT_STEP_MAX 1000u

uint32_t i915_read(struct i915_hw *hw, uint32_t reg)
{
    if (reg > hw->size - 4u || (reg & 3u) != 0) {
        return 0xffffffffu;
    }
    return hw->ops->read32(hw, reg);
}

void i915_write(struct i915_hw *hw, uint32_t reg, uint32_t value)
{
    if (reg > hw->size - 4u || (reg & 3u) != 0) {
        return;
    }
    hw->ops->write32(hw, reg, value);
}

void i915_posting_read(struct i915_hw *hw, uint32_t reg)
{
    (void)i915_read(hw, reg);
}

uint32_t i915_rmw(struct i915_hw *hw, uint32_t reg, uint32_t clear, uint32_t set)
{
    uint32_t old = i915_read(hw, reg);

    i915_write(hw, reg, (old & ~clear) | set);
    return old;
}

uint64_t i915_read64_2x32(struct i915_hw *hw, uint32_t lo, uint32_t hi)
{
    uint32_t upper, lower, again;
    int tries = 3;

    upper = i915_read(hw, hi);
    do {
        again = upper;
        lower = i915_read(hw, lo);
        upper = i915_read(hw, hi);
    } while (upper != again && --tries > 0);
    return ((uint64_t)upper << 32) | lower;
}

void i915_delay_us(struct i915_hw *hw, uint32_t us)
{
    hw->ops->delay_us(hw, us);
}

/**
 * Record that the wait `what` reached its bound of `bound_us` with `reg`
 * last reading `last`, in the log and on `hw`; -ETIMEDOUT for the caller to
 * return.
 */
static int32_t i915_wait_timed_out(struct i915_hw *hw, const char *what, uint32_t reg,
                                   uint32_t last, uint32_t bound_us)
{
    hw->timeouts++;
    hw->last_timeout = what;
    klog(KLOG_ERR, "i915", "%s: timeout after %u us: register 0x%x reads 0x%x", what,
         (unsigned)bound_us, (unsigned)reg, (unsigned)last);
    return -ETIMEDOUT;
}

int32_t i915_wait_for(struct i915_hw *hw, const char *what, uint32_t reg, uint32_t mask,
                      uint32_t value, uint32_t bound_us)
{
    uint32_t waited = 0, step = 1, got;

    for (;;) {
        got = i915_read(hw, reg);
        if ((got & mask) == value) {
            return 0;
        }
        if (waited >= bound_us) {
            return i915_wait_timed_out(hw, what, reg, got, bound_us);
        }
        if (step > bound_us - waited) {
            step = bound_us - waited;
        }
        i915_delay_us(hw, step);
        waited += step;
        if (step < I915_WAIT_STEP_MAX) {
            step = step * 2u > I915_WAIT_STEP_MAX ? I915_WAIT_STEP_MAX : step * 2u;
        }
    }
}

int32_t i915_wait_until(struct i915_hw *hw, const char *what, int (*done)(struct i915_hw *, void *),
                        void *arg, uint32_t bound_us)
{
    uint32_t waited = 0, step = 1;

    for (;;) {
        if (done(hw, arg)) {
            return 0;
        }
        if (waited >= bound_us) {
            hw->timeouts++;
            hw->last_timeout = what;
            klog(KLOG_ERR, "i915", "%s: timeout after %u us", what, (unsigned)bound_us);
            return -ETIMEDOUT;
        }
        if (step > bound_us - waited) {
            step = bound_us - waited;
        }
        i915_delay_us(hw, step);
        waited += step;
        if (step < I915_WAIT_STEP_MAX) {
            step = step * 2u > I915_WAIT_STEP_MAX ? I915_WAIT_STEP_MAX : step * 2u;
        }
    }
}

/* ---- the backend over BAR 0 ---- */

/** A register of the mapped half of BAR 0, by one uncached load. */
static uint32_t i915_mmio_read(struct i915_hw *hw, uint32_t reg)
{
    return mmio_read32((uintptr_t)hw->priv + reg);
}

/** A register of the mapped half of BAR 0, by one uncached store. */
static void i915_mmio_write(struct i915_hw *hw, uint32_t reg, uint32_t value)
{
    mmio_write32((uintptr_t)hw->priv + reg, value);
}

/** A spin on the processor's reference counter (sys/udelay.h). */
static void i915_mmio_delay(struct i915_hw *hw, uint32_t us)
{
    (void)hw;
    k_udelay(us);
}

static const struct i915_hw_ops i915_mmio_ops = {"mmio", i915_mmio_read, i915_mmio_write,
                                                 i915_mmio_delay};

int32_t i915_hw_mmio_init(struct i915_hw *hw, uint64_t phys, uint32_t size)
{
    uintptr_t regs;

    if (size < 4u || phys + size > (uint64_t)(uintptr_t)-1) {
        return -ENOMEM;
    }
    regs = map_mmio_typed((uintptr_t)phys, size, PTE_NOCACHE);
    if (regs == 0) {
        return -ENOMEM;
    }
    hw->ops = &i915_mmio_ops;
    hw->priv = (void *)regs;
    hw->size = size;
    hw->timeouts = 0;
    hw->last_timeout = 0;
    return 0;
}
