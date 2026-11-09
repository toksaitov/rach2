/*
 * i915_hw.h: the Intel graphics driver's hardware boundary, through which
 * every register access and every wait on the device goes.
 */
#ifndef _I915_HW_H_
#define _I915_HW_H_

#include "../../lib/kint.h"
#include "../../pci.h"

/*
 * The driver reaches its device only through a `struct i915_hw`, so that
 * what stands behind it may change: the registers of BAR 0 mapped
 * uncached (`i915_hw_mmio_init`), the host model of the HD 4400
 * (tests/host/i915_model.c), or the fake chip that completes work at once
 * for tests in QEMU (i915_fake.c). A register is its offset in BAR 0,
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
 *
 * The rest a backend that stands in for the chip gives, and the MMIO
 * backend and the host model leave NULL:
 *
 * * `config_read32` answers for the chip's identity in configuration
 *   space, the dword at `offset`, a multiple of 4: the ids, the class and
 *   revision, the board's ids, the GMCH control word, BDSM and MSAC. The
 *   driver then reads those from it and the PCI function's own command
 *   word and capabilities from the function (i915_hw_config32).
 * * `fault` answers at a fault point (I915_FAULT_*, i915_hw_fault).
 * * `attach` is told of the device once it is registered, `detach` at its
 *   unplug, and `release` when its record goes, after which `priv` is
 *   not used again.
 * * `aperture_page` and `aperture_sync` stand in for the aperture, BAR 2,
 *   where nothing backs it: `aperture_page` gives in `*phys` the frame
 *   that holds the aperture's page at `offset`, which a mapping into a
 *   program or the kernel then reaches in place of the BAR, and returns 0
 *   or -1; `aperture_sync` copies [offset, offset + size) of the aperture
 *   between those frames and the memory the GTT's entries map, through
 *   the fence that covers it, into memory when `to_memory` is set and out
 *   of it otherwise. The chip's aperture needs neither: the driver calls
 *   the sync only where the chip would be coherent by itself, before it
 *   reads the aperture, after it writes it, when an object moves into the
 *   GTT's domain and when its writes through the GTT are flushed.
 */
struct drm_i915_private;

struct i915_hw_ops {
    const char *name;
    uint32_t (*read32)(struct i915_hw *hw, uint32_t reg);
    void (*write32)(struct i915_hw *hw, uint32_t reg, uint32_t value);
    void (*delay_us)(struct i915_hw *hw, uint32_t us);
    int32_t (*gtt_map)(struct i915_hw *hw, uint64_t phys, uint32_t size);
    uint32_t (*config_read32)(struct i915_hw *hw, uint32_t offset);
    int32_t (*fault)(struct i915_hw *hw, int point);
    void (*attach)(struct i915_hw *hw, struct drm_i915_private *i915);
    void (*detach)(struct i915_hw *hw);
    void (*release)(struct i915_hw *hw);
    int (*aperture_page)(struct i915_hw *hw, uint64_t offset, uintptr_t *phys);
    void (*aperture_sync)(struct i915_hw *hw, uint64_t offset, uint64_t size, int to_memory);
};

/*
 * A device as the driver reaches it. `priv` is the backend's own: the
 * kernel address BAR 0's registers are mapped at, or the model. `size` is
 * how many bytes of BAR 0 the backend answers for; an access at or past it
 * is refused by the calls below. `timeouts` counts the waits that reached
 * their bound and `last_timeout` names the last of them, or is NULL.
 * `gtt_entries` is how many entries of the global GTT `i915_hw_gtt_init`
 * made reachable, 0 before it, and `gsm` the kernel address of the first
 * where the backend's `gtt_map` mapped them, or NULL. `faults` counts the
 * errors the backend injected at fault points.
 */
struct i915_hw {
    const struct i915_hw_ops *ops;
    void *priv;
    uint32_t size;
    uint32_t timeouts;
    const char *last_timeout;
    uint32_t gtt_entries;
    void *gsm;
    uint32_t faults;
};

/*
 * Fault points: the operations of the driver that a backend standing in
 * for the chip may fail, so that a test reaches the driver's error paths.
 * The chip itself never fails them; a hang and a lost interrupt, which it
 * can give, the backend gives through its registers.
 */
#define I915_FAULT_SUBMIT 0 /* work handed to a ring: -EIO, as a wedged GPU */
#define I915_FAULT_BIND   1 /* pages bound into the global GTT: -ENOSPC */

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
 * The word at `offset`, a multiple of 2, of the configuration space of
 * the device on `pdev`: the backend's where it answers for one
 * (`config_read32`), and the function's otherwise. `i915_hw_config32` is
 * the dword at `offset`, a multiple of 4.
 */
uint16_t i915_hw_config16(struct i915_hw *hw, const pci_device_t *pdev, uint32_t offset);
uint32_t i915_hw_config32(struct i915_hw *hw, const pci_device_t *pdev, uint32_t offset);

/**
 * Ask the backend whether the operation at the fault point `point`
 * (I915_FAULT_*) fails this time. Counted in `faults` when it does.
 *
 * @return 0 to go on, or the -errno the backend injects.
 */
int32_t i915_hw_fault(struct i915_hw *hw, int point);

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

/*
 * The aperture, BAR 2 (i915_aperture.c): the first `size` bytes of the
 * global GTT as the processor sees them, through the fences, at the
 * physical address `phys`. A program's mapping reaches it by physical
 * address. The kernel reaches it a page at a time through windows, as
 * Linux's io_mapping does, since the aperture, 256 MiB on the Lenovo, is
 * larger than the kernel's device region, 128 MiB (cpu.h's KVA_MMIO):
 * I915_APERTURE_WINDOWS windows of I915_APERTURE_WINDOW bytes each, mapped
 * write-combining where they are needed and moved, the least recently used
 * first, when another part is. A window's `users` counts the pages of it
 * handed out and not yet given back, and while it is not 0 the window
 * stays. `used` orders the windows by their last use. `maps` counts the
 * windows mapped since the bind, for a test. A backend that stands in for
 * the aperture (`aperture_page`) gives its pages through temporary
 * mappings instead, and no window is mapped.
 */
#define I915_APERTURE_WINDOW  0x200000u
#define I915_APERTURE_WINDOWS 4

struct i915_aperture_window {
    uintptr_t va; /* 0 while it maps nothing */
    uint64_t offset;
    uint32_t size; /* I915_APERTURE_WINDOW, less at the aperture's end */
    uint32_t users;
    uint32_t used;
};

struct i915_aperture {
    uint64_t phys;
    uint64_t size;
    uint32_t clock;
    uint32_t maps;
    struct i915_aperture_window win[I915_APERTURE_WINDOWS];
};

/** Make `ap` the aperture of `size` bytes at `phys`, with no window mapped. */
void i915_aperture_init(struct i915_aperture *ap, uint64_t phys, uint64_t size);

/** Take every window away; none may be in use. */
void i915_aperture_fini(struct i915_aperture *ap);

/**
 * The physical address a program's mapping of the aperture's page at
 * `offset`, a whole page below `ap->size`, maps, in `*phys`: the BAR's, or
 * the backend's frame (`aperture_page`).
 *
 * @return 0, or -EINVAL past the aperture or where the backend has none.
 */
int32_t i915_aperture_phys(struct i915_hw *hw, const struct i915_aperture *ap, uint64_t offset,
                           uintptr_t *phys);

/**
 * The kernel's address of the aperture's page at `offset`, a whole page
 * below `ap->size`, valid until `i915_aperture_kunmap`; the caller may
 * sleep while it holds it. A window that holds the page is used, or the
 * least recently used window that no one holds is moved over it.
 *
 * @return the address, or NULL past the aperture, when every window is
 *         held, or when the kernel's device region or the temporary slots
 *         have no room.
 */
void *i915_aperture_kmap(struct i915_hw *hw, struct i915_aperture *ap, uint64_t offset);

/** Give back a page `i915_aperture_kmap` returned at `va`. */
void i915_aperture_kunmap(struct i915_hw *hw, struct i915_aperture *ap, void *va);

/** The backend's `aperture_sync`, where it has one; nothing on the chip. */
void i915_aperture_sync(struct i915_hw *hw, uint64_t offset, uint64_t size, int to_memory);

#endif /* _I915_HW_H_ */
