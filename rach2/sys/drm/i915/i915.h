/*
 * i915.h: the Intel graphics driver's device, the Haswell chips it binds,
 * and the functions its files share.
 */
#ifndef _I915_H_
#define _I915_H_

#include "../drm.h"
#include "../../lib/spinlock.h"
#include "../../proc/wait_queue.h"
#include "i915_hw.h"

/*
 * rach2/sys/drm/i915/Readme.md tells the design. The rules every file of
 * sys/drm/i915/ keeps:
 *
 * The boundary. Every access to the device, a register, the GTT or a
 * wait, goes through the device's `hw` (i915_hw.h); no file maps BAR 0 or
 * reads it by address but i915_hw.c.
 *
 * Locking. Everything runs under the big kernel lock, as sys/drm/drm.h
 * says, and the order of the locks the driver takes beside it, outermost
 * first, is: a DRM device's `commit_lock`; the driver's locks of objects
 * and contexts, which may sleep; then the locks of the hardware, which a
 * holder takes with interrupts off and never sleeps under: the register
 * lock of forcewake and of any read and write the interrupt handler also
 * makes, then the interrupt mask's. The interrupt handler takes only the
 * hardware locks.
 *
 * Interrupts. Every source is masked and its enable cleared before the
 * device's message is enabled, and a source is unmasked only once the
 * handler of its owner is registered (i915_irq_register).
 */

/* What Linux 6.12's device table (i915_pci.c, hsw_gt2_info) says of the
 * parts it binds here: Haswell, graphics 7.5 and display 7, GT2, with an
 * LLC, the L3's parity interrupts, RC6 without RC6p, an engine reset, an
 * aliasing PPGTT of 2^31 bytes, 40 bits of DMA, and the render, video,
 * blitter and video enhancement engines. */
#define I915_ENGINE_RCS0  (1u << 0)
#define I915_ENGINE_BCS0  (1u << 1)
#define I915_ENGINE_VCS0  (1u << 2)
#define I915_ENGINE_VECS0 (1u << 3)

struct i915_platform {
    const char *name;     /* Linux's platform name, "HASWELL" */
    uint8_t graphics_ver; /* 7 */
    uint8_t graphics_rel; /* 50: Haswell is 7.5 */
    uint8_t display_ver;  /* 7 */
    uint8_t gt;           /* 2 */
    uint32_t engines;     /* I915_ENGINE_* */
    uint8_t has_llc;
    uint8_t has_l3_dpf;
    uint8_t has_rc6;
    uint8_t has_rc6p;
    uint8_t has_reset_engine;
    uint8_t ppgtt_size; /* bits of the aliasing PPGTT's address space */
    uint8_t dma_mask_size;
};

/* A device id Linux gives `platform`, with its subplatform: ULT, or ULX,
 * which Linux counts as ULT too. */
struct i915_id {
    uint16_t device_id;
    uint8_t ult;
    uint8_t ulx;
    const struct i915_platform *platform;
};

/* Linux's ids for Haswell GT2, and how many there are. */
extern const struct i915_id i915_ids[];
extern const int i915_nids;

/*
 * Forcewake and the GT FIFO (i915_uncore.c). `lock` is the register lock
 * of i915.h's order, over the rest. `wake_count` counts the references to
 * forcewake held, which is held on the chip while it is not 0.
 * `fifo_count` is how many free entries the driver believes the GT FIFO
 * has, read again from GTFIFOCTL once it reaches the reserve.
 * `fifo_timeouts` and `thread_timeouts` count the waits for the FIFO and
 * for the GT's threads to wake that reached their bound, which Linux too
 * logs and goes on past.
 */
struct i915_uncore {
    struct spinlock lock;
    uint32_t wake_count;
    uint32_t fifo_count;
    uint32_t fifo_timeouts;
    uint32_t thread_timeouts;
};

/*
 * The display power well, Haswell's one, which Linux calls "display"
 * (i915_power.c). `count` counts the references to it. `on` is whether the
 * driver's request is set and the well reported on. `kept` is set while
 * the count is 0 and the well stays on because a pipe that needs it runs.
 */
struct i915_power {
    uint32_t count;
    uint8_t on;
    uint8_t kept;
};

/*
 * Stolen memory, which the firmware takes from the top of memory for the
 * GPU (i915_stolen.c). It is [base, base + size), from BDSM and the GMCH
 * control word; `size` is 0 when there is none. The firmware keeps
 * [reserved_base, reserved_base + reserved_size) of it, at its top, and
 * the driver the `usable_size` bytes below. `in_rmrr` is set when the
 * memory lies inside a reserved region the firmware's DMAR table names
 * for the device, and `for_objects` when buffers may be made in it, as
 * Linux decides: not with VT-d on, on a chip before graphics version 8.
 */
struct i915_stolen {
    uint64_t base;
    uint64_t size;
    uint64_t reserved_base;
    uint64_t reserved_size;
    uint64_t usable_size;
    uint8_t in_rmrr;
    uint8_t for_objects;
};

/* The cache levels of a page in the GTT, Linux's enum i915_cache_level. */
#define I915_CACHE_NONE   0
#define I915_CACHE_LLC    1
#define I915_CACHE_L3_LLC 2
#define I915_CACHE_WT     3

#define I915_GTT_PAGE_SIZE 4096u
/* The firmware's surfaces kept in the global GTT: one a pipe at most. */
#define I915_GGTT_KEPT_MAX 3

/* A range of GTT offsets, [start, end). */
struct i915_gtt_range {
    uint64_t start;
    uint64_t end;
};

/*
 * The global GTT (i915_gtt.c): `total` bytes of the GPU's address space,
 * an entry for each page, of which the first `mappable_end` bytes are
 * reached through the aperture, BAR 2. Every entry that maps nothing maps
 * the scratch page, a zeroed frame at `scratch` whose entry is
 * `scratch_pte`. `kept` holds the `nkept` ranges of the firmware's
 * surfaces, which the bind left as it found them and which the GTT's calls
 * refuse to change, since the console draws there through the aperture.
 */
struct i915_ggtt {
    uint64_t total;
    uint64_t mappable_end;
    uintptr_t scratch;
    uint32_t scratch_pte;
    int nkept;
    struct i915_gtt_range kept[I915_GGTT_KEPT_MAX];
};

/*
 * The interrupt banks of Haswell (i915_irq.c): the GT's, power
 * management's, the display engine's and the south display's, each an
 * identity, mask and enable register, with a handler for each bit.
 */
#define I915_IRQ_GT    0
#define I915_IRQ_PM    1
#define I915_IRQ_DE    2
#define I915_IRQ_SDE   3
#define I915_IRQ_BANKS 4

struct drm_i915_private;

/* A source's handler: `iir` holds the bits of its bank that were set and
 * that it registered, already acknowledged. It runs in the interrupt, with
 * the interrupt mask's lock held, and may only read and write registers,
 * signal, count and wake. */
typedef void (*i915_irq_fn)(struct drm_i915_private *i915, uint32_t iir, void *data);

/*
 * The interrupt state. `lock` is the interrupt mask's lock of i915.h's
 * order, over the rest. `imr` and `ier` are what the driver last wrote to
 * each bank's mask and enable; a bit is enabled only while a handler is
 * registered for it, and unmasked only while it is enabled. `fn` and
 * `data` are the handlers by bank and bit. `messages` counts the messages
 * taken, `spurious` those that found no identity bit set, and `unclaimed`
 * the identity bits that had no handler, each of which is masked and
 * disabled again when it is seen. `installed` is set once the master
 * enable is on.
 */
struct i915_irq {
    struct spinlock lock;
    uint32_t imr[I915_IRQ_BANKS];
    uint32_t ier[I915_IRQ_BANKS];
    i915_irq_fn fn[I915_IRQ_BANKS][32];
    void *data[I915_IRQ_BANKS][32];
    uint32_t messages;
    uint32_t spurious;
    uint32_t unclaimed;
    uint8_t installed;
};

/* The engines this part of the driver starts, the render and blitter
 * rings, by index into `engine`. */
#define I915_RCS0        0
#define I915_BCS0        1
#define I915_NUM_ENGINES 2

/* The bytes of a ring, Linux's legacy ring (SZ_16K). */
#define I915_RING_SIZE 16384u

/*
 * An engine and its ring (i915_ring.c). `mmio_base` is the base of its
 * registers, `hws_pga` the register of its status page's address, and
 * `irq_bit` its user interrupt, one bit in the GT bank and in its own
 * RING_IMR.
 *
 * The status page is one page at `hws`, the kernel's address, mapped at
 * `hws_ggtt` in the global GTT; the chip writes breadcrumbs into it. The
 * ring is `ring_size` bytes at `ring`, virtually contiguous, mapped at
 * `ring_ggtt`. `tail` is the offset the next command goes to, and `head`
 * the head as the driver last read it. Both are cached in the LLC, which
 * the chip snoops.
 *
 * Breadcrumbs. `seqno` is the last sequence number emitted, and the
 * status page's dword I915_GEM_HWS_SEQNO the last the chip has written.
 * `wq` holds the processes that wait for one, woken from the user
 * interrupt, which is unmasked while `irq_refs` is above 0. `irq_seen`
 * counts the user interrupts taken. `running` is set once the ring has
 * started.
 */
struct i915_engine {
    const char *name;
    uint32_t mmio_base;
    uint32_t hws_pga;
    uint32_t irq_bit;
    uint32_t *hws;
    uint32_t hws_ggtt;
    uint32_t *ring;
    uint32_t ring_size;
    uint32_t ring_ggtt;
    uint32_t head;
    uint32_t tail;
    uint32_t seqno;
    uint32_t irq_refs;
    uint32_t irq_seen;
    wait_queue_t wq;
    uint8_t running;
};

/*
 * The GT's own objects (i915_ring.c): the status pages and rings, and
 * first light's page, which take the global GTT from its top down, from
 * `ggtt_top` to `ggtt_bottom`, the lowest offset they use. An allocator
 * of the GTT leaves [ggtt_bottom, total) alone. Space comes back only when
 * the last taken is given back, and all of it when the rings stop.
 */
struct i915_gt {
    uint64_t ggtt_top;
    uint64_t ggtt_bottom;
    struct i915_engine engine[I915_NUM_ENGINES];
};

/*
 * A bound device. `drm` is first, and its `driver_priv` points back here.
 * `pci` is the function as the probe found it. `hw` reaches its registers.
 * `mmio_phys` is BAR 0's physical address and `gmadr_phys` BAR 2's, the
 * aperture. `gmch_ctrl` and `bsm` are the configuration words of the
 * stolen memory and the GTT's size (SNB_GMCH_CTRL, INTEL_BSM) as the
 * firmware left them, and `subsystem_vendor` and `subsystem_device` the
 * board's ids. `msi_vector` is the vector of the device's message, or -1
 * while it has none; `irqs` counts the interrupts taken on it.
 */
struct drm_i915_private {
    struct drm_device drm;
    pci_device_t pci;
    const struct i915_id *id;
    struct i915_hw hw;
    uint64_t mmio_phys;
    uint64_t gmadr_phys;
    uint16_t gmch_ctrl;
    uint32_t bsm;
    uint16_t subsystem_vendor;
    uint16_t subsystem_device;
    int msi_vector;
    uint32_t irqs;
    struct i915_uncore uncore;
    struct i915_power power;
    struct i915_stolen stolen;
    struct i915_ggtt ggtt;
    struct i915_irq irq;
    struct i915_gt gt;
};

/* The driver's device of a DRM device it bound. */
#define to_i915(dev) ((struct drm_i915_private *)(dev)->driver_priv)

/** The id entry of a device id, or NULL when the driver does not bind it. */
const struct i915_id *i915_id_lookup(uint16_t device_id);

/**
 * Bind the PCI function `pdev`, whose registers `hw` reaches, and register
 * its DRM device: the probe's work once BAR 0 is mapped, and the way in of
 * any other backend, the host model's among them. Turns on memory decoding
 * and bus mastering, masks and clears every interrupt source, brings up
 * forcewake, the display power well, stolen memory and the global GTT,
 * takes a vector and enables the device's message on it where the machine
 * has messages, and registers the device as `i915`. A copy of `hw` is
 * kept.
 *
 * @return 0, -ENODEV for a device the driver does not bind, -ENOMEM, the
 *         GTT's error or the DRM core's; on an error the command word is
 *         restored and the message is off, and every source stays masked.
 */
int32_t i915_driver_bind(const pci_device_t *pdev, const struct i915_hw *hw);

/* ---- i915_uncore.c ---- */

/** Clear what the firmware left in the unclaimed-access and GT FIFO error
 * registers and release every forcewake request, as Linux's
 * intel_uncore_forcewake_reset does at load; then read the FIFO's free
 * entries. The first of the bind's parts. */
void i915_uncore_init(struct drm_i915_private *i915);

/**
 * Take a reference to forcewake, which keeps the GT awake so that its
 * registers read true: the first reference waits for the chip to drop its
 * acknowledgement of the last request, sets the kernel's request in
 * FORCEWAKE_MT and waits for the acknowledgement in FORCEWAKE_ACK_HSW, 50
 * ms at most each, then for the GT's threads to leave C0's sleep, as
 * Linux's fw_domains_get_with_thread_status. Takes the register lock,
 * with interrupts off.
 *
 * @return 0, or -ETIMEDOUT when an acknowledgement did not come, with the
 *         wait's name, "forcewake ack clear" or "forcewake ack", in
 *         `hw.last_timeout`; then no reference is held and the request is
 *         withdrawn.
 */
int32_t i915_forcewake_get(struct drm_i915_private *i915);

/** Drop a reference `i915_forcewake_get` gave; the last withdraws the
 * request. A put with no reference held is logged and changes nothing. */
void i915_forcewake_put(struct drm_i915_private *i915);

/**
 * Read `reg` as Linux's accessors do on Haswell: a register of the GT,
 * below 0x40000, under forcewake taken for the read when no reference is
 * held, and any other directly. A GT register whose forcewake cannot be
 * had reads all ones, as a register of a chip that has gone.
 */
uint32_t i915_uncore_read(struct drm_i915_private *i915, uint32_t reg);

/**
 * Write `value` to `reg` as Linux's gen6 accessors do: a write below
 * 0x40000 or from 0x116000 waits, 10 ms at most, until the GT FIFO has
 * more free entries than the 20 it keeps in reserve, since a write that
 * finds it full is lost; a wait that reaches its bound is logged and
 * counted, and the write goes ahead, as on Linux.
 */
void i915_uncore_write(struct drm_i915_private *i915, uint32_t reg, uint32_t value);

/* ---- i915_power.c ---- */

/**
 * Take the display power well over from the firmware and hold it on for
 * the driver, as Linux's intel_power_domains_init_hw: the BIOS's request
 * becomes the driver's, then the driver takes the reference Linux's
 * POWER_DOMAIN_INIT holds until the display is read out.
 *
 * @return 0, or -ETIMEDOUT when the well did not report on within 1 ms.
 */
int32_t i915_power_init(struct drm_i915_private *i915);

/** Take a reference to the display well, turning it on with the first:
 * 0, or -ETIMEDOUT, holding none, when it did not come on in 1 ms. */
int32_t i915_power_get(struct drm_i915_private *i915);

/** Drop a reference to the display well. With the last the well is turned
 * off, unless a pipe that needs it runs, as the firmware's may: then it
 * stays on, `kept`, and is logged. */
void i915_power_put(struct drm_i915_private *i915);

/** Whether the display well reports on. */
int i915_power_well_on(struct drm_i915_private *i915);

/** Whether a pipe, transcoder, plane or panel fitter that the display well
 * powers is enabled, as the firmware's pipe may be. */
int i915_power_well_in_use(struct drm_i915_private *i915);

/* ---- i915_stolen.c ---- */

struct frame_map;

/**
 * Take the stolen memory of a Haswell GT2 chip at 00:02.0 out of the boot's
 * memory map `map`, before the frame allocator is made from it, as Linux's
 * early quirk does, so that no frame of it is ever handed out, whatever
 * the firmware's map says. Reads configuration space by mechanism #1,
 * since nothing else is up yet.
 *
 * @return the bytes reserved, 0 with no such chip or no stolen memory.
 */
uint64_t i915_stolen_reserve_early(struct frame_map *map);

/**
 * Find the stolen memory from the configuration words the bind read and
 * check it: page frames none of which the allocator owns, inside a
 * reserved region of the device where the firmware's DMAR table names
 * one, with the part the firmware keeps at its top from
 * GEN6_STOLEN_RESERVED. Memory that fails a check is logged and left
 * unused: `size` 0.
 */
void i915_stolen_init(struct drm_i915_private *i915);

/* ---- i915_gtt.c ---- */

/** Haswell's GTT entry for the page at `addr`, valid, with `cache` one of
 * the I915_CACHE_ levels, as Linux's hsw_pte_encode. */
uint32_t i915_ggtt_pte_encode(uint64_t addr, int cache);

/**
 * Bring the global GTT up: its size from the GMCH control word, the
 * aperture's from MSAC, its entries made reachable through the hardware
 * boundary, a scratch page, the firmware's surfaces found from the enabled
 * primary planes and kept, and every other entry pointed at the scratch
 * page, as Linux's init_ggtt clears the holes.
 *
 * @return 0, -ENODEV when the chip has no GTT, -ENOMEM with no frame for
 *         the scratch page, or the boundary's error.
 */
int32_t i915_ggtt_init(struct drm_i915_private *i915);

/** Give the scratch page back; the GTT's entries are left as they are. */
void i915_ggtt_fini(struct drm_i915_private *i915);

/** Flush the chip's GTT cache, so that it sees the entries written, as
 * Linux's gen6_ggtt_invalidate. */
void i915_ggtt_invalidate(struct drm_i915_private *i915);

/**
 * Map the `n` pages whose physical addresses are `pages` at the GTT offset
 * `offset`, at the cache level `cache`, and flush the GTT's cache.
 *
 * @return 0; -EINVAL for an offset or an address not of whole pages, an
 *         address past the 39 bits an entry holds or a range past the
 *         GTT; -EBUSY for a range that touches a kept surface. Nothing is
 *         written on an error.
 */
int32_t i915_ggtt_insert(struct drm_i915_private *i915, uint64_t offset, const uint64_t *pages,
                         uint32_t n, int cache);

/** Point the entries of [offset, offset + size) at the scratch page and
 * flush the GTT's cache: 0, or `i915_ggtt_insert`'s errors. */
int32_t i915_ggtt_clear_range(struct drm_i915_private *i915, uint64_t offset, uint64_t size);

/** The entry that maps the GTT offset `offset`; all ones past the GTT. */
uint32_t i915_ggtt_pte(struct drm_i915_private *i915, uint64_t offset);

/* ---- i915_irq.c ---- */

/**
 * Mask and clear every interrupt source of the device and clear every
 * enable, the display engine's master enable among them, as Linux's
 * ilk_irq_reset does on Haswell, and forget every handler: the state the
 * bind starts from and the unplug leaves. Takes the interrupt mask's lock.
 */
void i915_irq_reset(struct drm_i915_private *i915);

/**
 * Turn the display engine's master enable on, every source still masked
 * and disabled, once the device's message is enabled, as Linux's
 * ilk_irq_postinstall does before its owners unmask their sources.
 */
void i915_irq_install(struct drm_i915_private *i915);

/**
 * Register `fn` for the bits `bits` of the bank `bank` and enable them, so
 * that each latches in the identity register once unmasked; they stay
 * masked until `i915_irq_unmask`. A south display source also enables the
 * display engine's PCH event, which carries it.
 *
 * @return 0; -EINVAL for an unknown bank, no bits or the display engine's
 *         master bit; -EBUSY when a bit has a handler already.
 */
int32_t i915_irq_register(struct drm_i915_private *i915, int bank, uint32_t bits, i915_irq_fn fn,
                          void *data);

/** Mask and disable the bits `bits` of `bank` and forget their handler. */
void i915_irq_unregister(struct drm_i915_private *i915, int bank, uint32_t bits);

/** Unmask the bits `bits` of `bank` that are registered, with a posting
 * read, as Linux's gen5_gt_enable_irq; a bit without a handler stays
 * masked. Takes the interrupt mask's lock, with interrupts off. */
void i915_irq_unmask(struct drm_i915_private *i915, int bank, uint32_t bits);

/** Mask the bits `bits` of `bank` again, with a posting read. */
void i915_irq_mask(struct drm_i915_private *i915, int bank, uint32_t bits);

/**
 * The device's interrupt, as Linux's ilk_irq_handler: the master enable
 * and the south display's off, then each bank's identity read, written
 * back and handed to its handlers, the GT's first, then the display
 * engine's with the south display's behind its PCH event, then power
 * management's; then the enables restored. Counts the message, a message
 * with no bit set as spurious, and a bit with no handler as unclaimed,
 * which it masks and disables. Any context the handler runs in.
 */
void i915_irq_handle(struct drm_i915_private *i915);

/* ---- i915_ring.c ---- */

/**
 * Set up and start the render and blitter rings, as Linux's
 * xcs_resume: for each a status page and a ring in the global GTT, the
 * ring stopped and emptied, the status page's address written and the
 * TLB flushed, the PPGTT's registers (`i915_ppgtt_enable_ring`), and the
 * ring started with RING_VALID, each wait bounded; then its user
 * interrupt registered, masked until a waiter needs it. Holds forcewake
 * throughout.
 *
 * @return 0; -ENOMEM; -ENOSPC when the GTT has no room at its top; -EIO
 *         when a ring would not stop or start, its wait named in
 *         `hw.last_timeout`. On an error every ring is torn down.
 */
int32_t i915_gt_init(struct drm_i915_private *i915);

/** Stop every ring, unregister its interrupt, clear its GTT entries and
 * free its pages; for the unplug and a failed bind. */
void i915_gt_fini(struct drm_i915_private *i915);

/**
 * Take `bytes` from the top of the global GTT's free space for one of the
 * GT's own objects, page-aligned, and store its offset in `*offset`.
 *
 * @return 0, or -ENOSPC when it would reach a kept range or offset 0.
 */
int32_t i915_gt_ggtt_take(struct drm_i915_private *i915, uint32_t bytes, uint32_t *offset);

/**
 * Allocate `npages` pages for the chip, zeroed, virtually contiguous in
 * the kernel, and map them in the global GTT at the cache level `cache`
 * at an offset `i915_gt_ggtt_take` gives.
 *
 * @return the kernel's address and the GTT offset in `*ggtt`, or NULL
 *         with nothing held.
 */
void *i915_gt_pages_map(struct drm_i915_private *i915, uint32_t npages, int cache, uint32_t *ggtt);

/** Point the pages' entries at the scratch page again and free them; the
 * GTT's space comes back when it was the last taken. */
void i915_gt_pages_unmap(struct drm_i915_private *i915, void *vaddr, uint32_t npages,
                         uint32_t ggtt);

/**
 * Make room for `ndw` dwords, an even number, in `engine`'s ring and
 * return where they go: past the end of the ring the rest is filled with
 * MI_NOOP and the commands start again at 0, as Linux's intel_ring_begin;
 * a ring without room waits, 10 ms at most, for the chip's head to move.
 *
 * @return the address, or NULL when the ring is not running, `ndw` is odd
 *         or too large, or the wait reached its bound ("ring space").
 */
uint32_t *i915_ring_begin(struct i915_engine *engine, struct drm_i915_private *i915, uint32_t ndw);

/** Hand the chip what was written since `i915_ring_begin`: the writes
 * ordered before the tail's, then RING_TAIL written. */
void i915_ring_advance(struct i915_engine *engine, struct drm_i915_private *i915);

/**
 * Emit a breadcrumb on `engine`: MI_STORE_DWORD_INDEX of the next sequence
 * number to the status page's I915_GEM_HWS_SEQNO and MI_USER_INTERRUPT,
 * and hand it to the chip.
 *
 * @return the sequence number, or 0 when the ring had no room, which is
 *         never a sequence number.
 */
uint32_t i915_engine_emit_breadcrumb(struct drm_i915_private *i915, struct i915_engine *engine);

/** Whether `engine` has written the breadcrumb `seqno`, in the order of
 * sequence numbers that wrap. */
int i915_engine_passed(const struct i915_engine *engine, uint32_t seqno);

/** Take a reference to `engine`'s user interrupt, unmasking it with the
 * first, as Linux's irq_enable: RING_IMR, then the GT's mask. */
void i915_engine_irq_get(struct drm_i915_private *i915, struct i915_engine *engine);

/** Drop a reference to `engine`'s user interrupt, masking it with the
 * last. */
void i915_engine_irq_put(struct drm_i915_private *i915, struct i915_engine *engine);

/**
 * Wait until `engine` has written the breadcrumb `seqno`, for at most
 * `timeout_us`: asleep on the engine's queue, woken by the user
 * interrupt, when a process waits, and polling the status page in bounded
 * waits when none does, as at boot. The interrupt is unmasked for the
 * wait.
 *
 * @return 0, -ETIME at the timeout, or -EIO when the ring is not running
 *         and the breadcrumb not written.
 */
int32_t i915_engine_wait(struct drm_i915_private *i915, struct i915_engine *engine, uint32_t seqno,
                         uint32_t timeout_us);

/* ---- i915_ppgtt.c ---- */

/**
 * Point `engine`'s ring at the aliasing PPGTT, at ring start and after a
 * reset, as Linux's set_pp_dir: until the PPGTT exists it writes nothing
 * and the ring runs on the global GTT.
 *
 * @return 0.
 */
int32_t i915_ppgtt_enable_ring(struct drm_i915_private *i915, struct i915_engine *engine);

/* ---- i915_firstlight.c ---- */

/**
 * First light, under `rach2.i915.firstlight`: the render ring writes a
 * value to a page of the global GTT with MI_STORE_DWORD_IMM and a
 * breadcrumb behind it, and the processor reads the value back. Logs
 * `i915: first light: ok` or the failure by name; every wait is bounded.
 *
 * @return 0, -ENODEV without the option, -EIO when the ring is not
 *         running, -ENOMEM, -ETIME when the breadcrumb or the interrupt
 *         did not come, or -EIO when the value read back is wrong.
 */
int32_t i915_firstlight(struct drm_i915_private *i915);

#endif /* _I915_H_ */
