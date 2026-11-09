/*
 * i915_hw.h: the Intel graphics driver's hardware boundary, through which
 * every register access and every wait on the device goes.
 */
#ifndef _I915_HW_H_
#define _I915_HW_H_

#include "../../lib/kint.h"

/*
 * The driver reaches its device only through a `struct i915_hw`, so that
 * what stands behind it may change: the registers of BAR 0 mapped
 * uncached (`i915_hw_mmio_init`), the host model of the HD 4400
 * (tests/host/i915_model.c), or a backend that completes work at once for
 * tests in QEMU. A register is its offset in BAR 0,
 * as i915_reg.h gives it, and every access is 32 bits wide. A backend
 * answers for the first `size` bytes: the MMIO backend for the registers,
 * the first half of BAR 0, since the GTT's half is the global GTT's, which
 * maps it write-combining as Linux does.
 *
 * Waits. Every wait on the device has a bound in microseconds and a name.
 * The time is counted by the backend's `delay_us`, the processor's spin
 * on its reference counter on the machine and the model's clock in a
 * host test, never by a loop count. A wait that reaches its bound returns
 * -ETIMEDOUT, logs `i915: NAME: timeout after N us` with what the register
 * last read, and leaves its name in `last_timeout`, so that a caller and a
 * test see which wait failed. A wait never sleeps: it spins with whatever
 * the caller holds, so its bound is kept short, and a longer wait is a
 * sleep of the caller's between bounded waits.
 *
 * Locking. The calls take no lock of their own. Under the big kernel lock
 * (sys/drm/drm.h) a register access is atomic with respect to the rest of
 * the driver, and an interrupt handler may read and write registers too.
 * A sequence that must not interleave with the handler, a read and a
 * write of one register that the handler also writes, masks the source
 * first or runs with interrupts off.
 */

struct i915_hw;

/*
 * A backend. `read32` and `write32` reach the register at `reg`, a byte
 * offset into BAR 0 that is a multiple of 4. `delay_us` lets at least `us`
 * microseconds pass on the device: a spin on the machine, the model's clock
 * moved forward in a test. `name` is the backend's, for the log.
 *
 * `gtt_map`, which a backend may leave NULL, makes the global GTT's
 * entries reachable: `size` bytes of BAR 0's second half at the physical
 * address `phys`, which the MMIO backend maps write-combining and reaches
 * through `gsm`. A backend without it answers for the entries as it does
 * for registers, at I915_GSM_OFFSET and up, and its `size` must cover them.
 */
struct i915_hw_ops {
    const char *name;
    uint32_t (*read32)(struct i915_hw *hw, uint32_t reg);
    void (*write32)(struct i915_hw *hw, uint32_t reg, uint32_t value);
    void (*delay_us)(struct i915_hw *hw, uint32_t us);
    int32_t (*gtt_map)(struct i915_hw *hw, uint64_t phys, uint32_t size);
};

/*
 * A device as the driver reaches it. `priv` is the backend's own: the
 * kernel address BAR 0's registers are mapped at, or the model. `size` is
 * how many bytes of BAR 0 the backend answers for; an access at or past it
 * is refused by the calls below. `timeouts` counts the waits that reached
 * their bound and `last_timeout` names the last of them, or is NULL.
 * `gtt_entries` is how many entries of the global GTT `i915_hw_gtt_init`
 * made reachable, 0 before it, and `gsm` the kernel address of the first
 * where the backend's `gtt_map` mapped them, or NULL.
 */
struct i915_hw {
    const struct i915_hw_ops *ops;
    void *priv;
    uint32_t size;
    uint32_t timeouts;
    const char *last_timeout;
    uint32_t gtt_entries;
    void *gsm;
};

/** The register at `reg` of `hw`; all ones for an offset past the
 * backend's size, which a device that has gone also reads as. */
uint32_t i915_read(struct i915_hw *hw, uint32_t reg);

/** Write `value` to the register at `reg`; nothing for an offset past the
 * backend's size. */
void i915_write(struct i915_hw *hw, uint32_t reg, uint32_t value);

/** Read `reg` and throw the value away, so that the writes before it have
 * reached the device: Linux's posting read. */
void i915_posting_read(struct i915_hw *hw, uint32_t reg);

/** Clear the bits `clear` of `reg` and set the bits `set`, by a read and a
 * write; the value read. */
uint32_t i915_rmw(struct i915_hw *hw, uint32_t reg, uint32_t clear, uint32_t set);

/**
 * A 64-bit counter kept in two registers, `lo` and `hi`, read so that a
 * carry between the halves is never seen half done: the high half, the low
 * half and the high half again, until the two highs agree, three times at
 * most, as Linux's intel_uncore_read64_2x32 does.
 */
uint64_t i915_read64_2x32(struct i915_hw *hw, uint32_t lo, uint32_t hi);

/** Let `us` microseconds pass on the device: the backend's `delay_us`. */
void i915_delay_us(struct i915_hw *hw, uint32_t us);

/**
 * Wait until the bits `mask` of `reg` read `value`, for at most `bound_us`
 * microseconds. The register is read at once, then after waits that start
 * at 1 us and double up to 1000 us, so that a fast answer costs little and
 * a slow one few reads. `what` names the wait for the log and for
 * `last_timeout`, and must outlive the device, as a literal does.
 *
 * @return 0 once the bits match, or -ETIMEDOUT at the bound.
 */
int32_t i915_wait_for(struct i915_hw *hw, const char *what, uint32_t reg, uint32_t mask,
                      uint32_t value, uint32_t bound_us);

/**
 * Wait until `done(hw, arg)` returns nonzero, for at most `bound_us`
 * microseconds, on the same clock and with the same doubling waits as
 * `i915_wait_for`, for a condition no one register's bits give, such as
 * the GT FIFO's free entries. `done` reaches the device only through `hw`.
 *
 * @return 0 once `done` holds, or -ETIMEDOUT at the bound.
 */
int32_t i915_wait_until(struct i915_hw *hw, const char *what, int (*done)(struct i915_hw *, void *),
                        void *arg, uint32_t bound_us);

/**
 * Make the first `size` bytes of the global GTT's entries reachable, at
 * the physical address `phys` of BAR 0's second half: through the
 * backend's `gtt_map`, or as registers from I915_GSM_OFFSET where it has
 * none.
 *
 * @return 0, -ENXIO when a backend without `gtt_map` does not answer for
 *         them, or the error of `gtt_map`.
 */
int32_t i915_hw_gtt_init(struct i915_hw *hw, uint64_t phys, uint32_t size);

/** Entry `index` of the global GTT; all ones past `gtt_entries`. */
uint32_t i915_gtt_read(struct i915_hw *hw, uint32_t index);

/** Write `pte` to entry `index` of the global GTT; nothing past
 * `gtt_entries`. The chip sees it once the caller has flushed the GTT's
 * cache (GFX_FLSH_CNTL_GEN6), a write to a register, which also drains
 * the processor's write-combining buffers. */
void i915_gtt_write(struct i915_hw *hw, uint32_t index, uint32_t pte);

/**
 * Make `hw` the backend over BAR 0's registers, `size` bytes at the
 * physical address `phys`, mapped uncached (map_mmio_typed with
 * PTE_NOCACHE), as Linux maps them, so that a read is never stale and a
 * write is never merged or held back. The GTT's half of BAR 0 is not
 * mapped here: `i915_hw_gtt_init` maps it write-combining, as Linux does,
 * where the processor has a page attribute table, and uncached otherwise.
 *
 * @return 0, or -ENOMEM when the kernel's device window has no room left.
 */
int32_t i915_hw_mmio_init(struct i915_hw *hw, uint64_t phys, uint32_t size);

#endif /* _I915_HW_H_ */
