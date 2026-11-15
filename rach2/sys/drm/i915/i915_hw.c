/*
 * i915_hw.c: the Intel graphics driver's hardware boundary: the accesses
 * and bounded waits of i915_hw.h, and its backend over BAR 0's registers.
 */

#include "i915_hw.h"
#include "i915_reg.h"

#include "../../arch/x86/pat.h"
#include "../../arch/x86/portio.h"
#include "../../klog.h"
#include "../../lib/kerrno.h"
#include "../../mm/paging.h"
#include "../../udelay.h"

/* The longest single wait between two reads of a register in a bounded
 * wait, in microseconds. */
#define I915_WAIT_STEP_MAX 1000u

/* The accesses to a device asleep in D3hot that are logged one by one;
 * the rest are only counted. */
#define I915_ASLEEP_LOGGED 8u

/**
 * Count an access to `hw` while it is asleep (`asleep`), at register
 * offset `reg`, a GTT entry's counted from I915_GSM_OFFSET, and log the
 * first few, as Linux's "RPM wakelock ref not held during HW access".
 */
static void i915_asleep_access(struct i915_hw *hw, uint32_t reg, int write)
{
    if (++hw->asleep_accesses <= I915_ASLEEP_LOGGED) {
        klog(KLOG_WARN, "i915", "rpm: %s of 0x%x while the device is suspended; nothing reached",
             write ? "write" : "read", (unsigned)reg);
    }
}

uint32_t i915_read(struct i915_hw *hw, uint32_t reg)
{
    if (hw->size < 4u || reg > hw->size - 4u || (reg & 3u) != 0) {
        return 0xffffffffu;
    }
    if (hw->asleep) {
        i915_asleep_access(hw, reg, 0);
        return 0xffffffffu;
    }
    return hw->ops->read32(hw, reg);
}

void i915_write(struct i915_hw *hw, uint32_t reg, uint32_t value)
{
    if (hw->size < 4u || reg > hw->size - 4u || (reg & 3u) != 0) {
        return;
    }
    if (hw->asleep) {
        i915_asleep_access(hw, reg, 1);
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

int32_t i915_hw_gtt_init(struct i915_hw *hw, uint64_t phys, uint32_t size)
{
    int32_t err;

    if (hw->ops->gtt_map) {
        err = hw->ops->gtt_map(hw, phys, size);
        if (err < 0) {
            return err;
        }
    } else if (hw->size < I915_GSM_OFFSET || size > hw->size - I915_GSM_OFFSET) {
        return -ENXIO;
    }
    hw->gtt_entries = size / 4u;
    return 0;
}

uint32_t i915_gtt_read(struct i915_hw *hw, uint32_t index)
{
    if (index >= hw->gtt_entries) {
        return 0xffffffffu;
    }
    if (hw->asleep) {
        i915_asleep_access(hw, I915_GSM_OFFSET + index * 4u, 0);
        return 0xffffffffu;
    }
    if (hw->gsm) {
        return mmio_read32((uintptr_t)hw->gsm + index * 4u);
    }
    return i915_read(hw, I915_GSM_OFFSET + index * 4u);
}

void i915_gtt_write(struct i915_hw *hw, uint32_t index, uint32_t pte)
{
    if (index >= hw->gtt_entries) {
        return;
    }
    if (hw->asleep) {
        i915_asleep_access(hw, I915_GSM_OFFSET + index * 4u, 1);
        return;
    }
    if (hw->gsm) {
        mmio_write32((uintptr_t)hw->gsm + index * 4u, pte);
        return;
    }
    i915_write(hw, I915_GSM_OFFSET + index * 4u, pte);
}

uint16_t i915_hw_config16(struct i915_hw *hw, const pci_device_t *pdev, uint32_t offset)
{
    if (hw->ops->config_read32) {
        return (uint16_t)(hw->ops->config_read32(hw, offset & ~3u) >> ((offset & 2u) * 8u));
    }
    return pci_read_config16(pdev->bus, pdev->device, pdev->function, (uint8_t)offset);
}

uint32_t i915_hw_config32(struct i915_hw *hw, const pci_device_t *pdev, uint32_t offset)
{
    if (hw->ops->config_read32) {
        return hw->ops->config_read32(hw, offset);
    }
    /* Two words, so that a host test's configuration space needs only
     * pci_read_config16. */
    return (uint32_t)i915_hw_config16(hw, pdev, offset + 2u) << 16 |
           i915_hw_config16(hw, pdev, offset);
}

int32_t i915_hw_fault(struct i915_hw *hw, int point)
{
    int32_t err;

    if (!hw->ops->fault) {
        return 0;
    }
    err = hw->ops->fault(hw, point);
    if (err < 0) {
        hw->faults++;
    }
    return err;
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

/** Map the GTT's entries write-combining where the processor has a page
 * attribute table, as Linux's ioremap_wc, and uncached otherwise. */
static int32_t i915_mmio_gtt_map(struct i915_hw *hw, uint64_t phys, uint32_t size)
{
    uintptr_t gsm;

    if (size < 4u || phys + size > (uint64_t)(uintptr_t)-1) {
        return -ENOMEM;
    }
    gsm = map_mmio_typed((uintptr_t)phys, size, pat_write_combining() ? PTE_WC : PTE_NOCACHE);
    if (gsm == 0) {
        return -ENOMEM;
    }
    hw->gsm = (void *)gsm;
    return 0;
}

static const struct i915_hw_ops i915_mmio_ops = {
    "mmio", i915_mmio_read, i915_mmio_write, i915_mmio_delay, i915_mmio_gtt_map, 0, 0, 0, 0, 0, 0,
    0};

void i915_hw_gone(struct i915_hw *hw)
{
    hw->size = 0;
    hw->gtt_entries = 0;
}

int i915_hw_is_mmio(const struct i915_hw *hw)
{
    return hw->ops == &i915_mmio_ops;
}

void i915_hw_vga_reset_io_mem(struct i915_hw *hw)
{
    if (i915_hw_is_mmio(hw)) {
        outb(0x3c2, inb(0x3cc));
    }
}

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
    hw->gtt_entries = 0;
    hw->gsm = 0;
    hw->faults = 0;
    return 0;
}
