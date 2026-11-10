/*
 * i915.h: the Intel graphics driver's device, the Haswell chips it binds,
 * and the functions its files share.
 */
#ifndef _I915_H_
#define _I915_H_

#include "../drm.h"
#include "../../mm/notify.h"
#include "../../mm/pressure.h"
#include "../../mm/shmem.h"
#include "../../fs/sysfs.h"
#include "../../lib/spinlock.h"
#include "../../proc/wait_queue.h"
#include "../../display/intelbl.h"
#include "i915_hw.h"
#include "intel_bios.h"

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
 * holder takes with interrupts off and never sleeps under: the frequency
 * lock of RPS, then the register lock of forcewake and of any read and
 * write the interrupt handler also makes, then the interrupt mask's. The
 * interrupt handler takes only the hardware locks.
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
 * `fw_tiled` is set when one of them is X-tiled, so that the swizzle the
 * firmware chose stands (Linux's preserve_bios_swizzle).
 */
struct i915_ggtt {
    uint64_t total;
    uint64_t mappable_end;
    uintptr_t scratch;
    uint32_t scratch_pte;
    int nkept;
    struct i915_gtt_range kept[I915_GGTT_KEPT_MAX];
    uint8_t fw_tiled;
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
 * enable is on. `deferred`, when set, runs at the end of each message once
 * the handler has let go of the lock, for the work a source's handler
 * leaves to it, as RPS's does (i915_rps.c), and `deferred_display` after
 * it, for the display's vblanks, events and CRCs (intel_vblank.c), which
 * call into the DRM core and back into the mask.
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
    void (*deferred)(struct drm_i915_private *i915);
    void (*deferred_display)(struct drm_i915_private *i915);
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
 *
 * Requests (i915_request.c). `requests` lists those emitted and not yet
 * retired, oldest first, to `requests_tail`; each holds a reference to
 * its fence there. `lock` guards the list against the user interrupt,
 * which signals their fences through `irq_notify`; it is the innermost of
 * the hardware locks, taken with interrupts off. `fence_context` is the
 * engine's timeline. `last_ce` is the context whose state the chip holds,
 * with a reference, and `kernel_ce` the GT's own, made with the first
 * request. `reserved` is the bytes `i915_ring_begin` leaves free for the
 * breadcrumb of the request being built.
 */
struct i915_request;
struct intel_context;

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
    struct spinlock lock;
    struct i915_request *requests;
    struct i915_request *requests_tail;
    uint64_t fence_context;
    struct intel_context *last_ce;
    struct intel_context *kernel_ce;
    uint32_t reserved;
    void (*irq_notify)(struct drm_i915_private *i915, struct i915_engine *engine);
};

/*
 * The GT's own objects (i915_ring.c): the status pages and rings, and
 * first light's page, which take the global GTT from its top down, from
 * `ggtt_top` to `ggtt_bottom`, the lowest offset they use. An allocator
 * of the GTT leaves [ggtt_bottom, total) alone. Space comes back only when
 * the last taken is given back, and all of it when the rings stop.
 *
 * What the requests need, made with the first (i915_request.c): `scratch`,
 * the GT's scratch page at `scratch_ggtt`, which flushes and the context
 * switch write; and `default_state`, the render ring's context image as a
 * fresh context left it with Linux's context workarounds, which every new
 * context starts from (Linux's engine->default_state). `ctx_ready` is set
 * once they are made. `awake` is set while any engine has requests, the
 * GT unparked. `wedged` is set when the GPU cannot run work, and new
 * requests and contexts fail with -EIO, as Linux's terminally wedged GT.
 * `reset_count` counts the GPU's resets, for GET_RESET_STATS.
 */
struct i915_gt {
    uint64_t ggtt_top;
    uint64_t ggtt_bottom;
    struct i915_engine engine[I915_NUM_ENGINES];
    uint32_t *scratch;
    uint32_t scratch_ggtt;
    uint32_t *default_state;
    uint8_t ctx_ready;
    uint8_t awake;
    uint8_t wedged;
    uint32_t reset_count;
};

/*
 * The slices, subslices and execution units of the GT, as Linux's
 * hsw_sseu_info_init finds them on GT2: one slice of two subslices, and
 * 10, 8 or 6 EUs in each by HSW_PAVP_FUSE1. `subslice_mask` is slice 0's.
 * `edram` is whether the package has eDRAM (HSW_EDRAM_CAP), which Linux's
 * HAS_WT reports. Read once, at the first query (`i915_sseu_init`).
 */
struct i915_sseu {
    uint8_t ready;
    uint8_t slice_mask;
    uint8_t subslice_mask;
    uint8_t max_slices;
    uint8_t max_subslices;
    uint8_t max_eus_per_subslice;
    uint8_t eu_per_subslice;
    uint8_t edram;
    uint16_t eu_total;
};

/*
 * RC6, the GT's sleep while it is idle (i915_rc6.c), as Linux's
 * intel_rc6.c runs it on Haswell. `ctl_enable` is GEN6_RC_CONTROL while
 * the GT is awake: RC6 under the hardware's timers. `manual` is set when
 * a parked GT is sent into RC6 at once, by software, as Linux does
 * wherever RC6 is enabled. The residency counter, GEN6_GT_GFX_RC6, counts
 * 1.28 us units in 32 bits; `prev_hw` is its value at the last read and
 * `cur` the units counted since the bind, so that a wrap between two reads
 * is not lost. Both are under the register lock. `supported` is set once
 * RC6 is sanitized and `enabled` once its thresholds are written.
 */
struct i915_rc6 {
    uint32_t ctl_enable;
    uint32_t prev_hw;
    uint64_t cur;
    uint8_t supported;
    uint8_t enabled;
    uint8_t manual;
};

/* Linux's power modes of RPS, which choose its evaluation intervals. */
#define I915_RPS_LOW_POWER  0
#define I915_RPS_BETWEEN    1
#define I915_RPS_HIGH_POWER 2

/*
 * RPS, the GPU's frequency (i915_rps.c), as Linux's intel_rps.c runs it on
 * Haswell. Every frequency is in the chip's units of 50 MHz. `rp0_freq`,
 * `rp1_freq` and `min_freq` are the chip's limits, RP0, RP1 and RPn, from
 * GEN6_RP_STATE_CAP; `max_freq` is RP0, or the overclocking limit the
 * pcode reports; `efficient_freq` is RPe, from the pcode's duty cycle
 * control. The softlimits are what gt_min_freq_mhz and gt_max_freq_mhz
 * set, inside [min_freq, max_freq]. `cur_freq` is the frequency asked for,
 * which gt_cur_freq_mhz shows, and `last_freq` the last written to
 * GEN6_RPNSWREQ, -1 for none. `last_adj` is the last step the interrupts
 * took. `power_mode` is an I915_RPS_ mode, -1 for none written, and
 * `up_threshold` and `down_threshold` its busy percentages.
 *
 * `pm_events` are the up and down interrupts RPS takes, and
 * `pm_intrmsk_mbz` the bits GEN6_PMINTRMSK must never mask; `pm_iir`
 * holds the events the interrupt latched and the deferred work has yet to
 * take, under the interrupt mask's lock. `enabled` is set once RPS runs,
 * `active` while the GT is unparked, `interrupts` once the up and down
 * interrupts drive it. `lock` is the frequency lock of i915.h's order,
 * over the rest. `work_runs` counts the events the work acted on.
 */
struct i915_rps {
    struct spinlock lock;
    int rp0_freq;
    int rp1_freq;
    int min_freq;
    int max_freq;
    int efficient_freq;
    int min_freq_softlimit;
    int max_freq_softlimit;
    int boost_freq;
    int idle_freq;
    int cur_freq;
    int last_freq;
    int last_adj;
    int power_mode;
    int up_threshold;
    int down_threshold;
    uint32_t pm_events;
    uint32_t pm_intrmsk_mbz;
    uint32_t pm_iir;
    uint32_t work_runs;
    uint8_t enabled;
    uint8_t active;
    uint8_t interrupts;
};

/*
 * Buffers: Linux's GEM objects for i915 (i915_gem.c and the files beside
 * it, which the Readme's buffer section tells).
 */

struct drm_i915_gem_object;
struct i915_vma;
struct pin;

/* The kinds of mapping MMAP_OFFSET gives, by its I915_MMAP_OFFSET_ number:
 * through the aperture, and of the pages write-combining, cached and
 * uncached. */
#define I915_MMAP_TYPES 4

/*
 * A place in the device's mmap space for one kind of mapping of an object,
 * Linux's i915_mmap_offset. `node.offset` is 0 until MMAP_OFFSET first
 * asks for it. A mapping of it is a device region whose object is this
 * record, so that the mappings of one kind are revoked by themselves
 * (paging_zap_device); the region holds a reference to the object.
 */
struct i915_mmap_offset {
    struct drm_mmap_node node;
    int type; /* I915_MMAP_OFFSET_* */
};

/*
 * A fence register, which detiles the aperture's accesses to a range of
 * the global GTT, Linux's i915_fence_reg. `vma` is the binding it serves,
 * NULL while free; `pin_count` counts its users that may not lose it;
 * `dirty` says the binding's tiling changed since it was written; `used`
 * orders the fences by their last use, the least recent stolen first.
 */
struct i915_fence_reg {
    int id;
    struct i915_vma *vma;
    uint32_t pin_count;
    uint32_t used;
    uint8_t dirty;
};

#define I915_NUM_FENCES 32

/* What `i915_vma_pin` is asked: the binding inside the aperture, from the
 * top of the space down, and no eviction to make room. */
#define I915_PIN_MAPPABLE 0x1u
#define I915_PIN_HIGH     0x2u
#define I915_PIN_NOEVICT  0x4u

/*
 * A binding of an object, or of a part of it, into the global GTT, Linux's
 * i915_vma. The view is the whole object when `view_pages` is 0, or the
 * `view_pages` pages from `view_offset`, a partial view, which a GTT map
 * of an object too large for the aperture binds a piece at a time. It lies
 * at [start, start + node_size) of the GTT, whose first `size` bytes map
 * the view's pages; `node_size` is the fence's size for a tiled object
 * inside the aperture, so that a fence covers it. `mappable` is set when
 * the node lies inside the aperture, and `cache` is the level its entries
 * were written with. `pin_count` counts the users that keep it bound.
 * `fence` is the fence register it holds, or NULL, and `fence_size` and
 * `fence_alignment` what a fence needs of it for the object's tiling.
 * `userfault` is set while programs map it through the aperture, and
 * `ggtt_write` while writes through the aperture may not have reached
 * memory. `evict` is set while an eviction's scan counts its space as free
 * (i915_gem_evict.c). A binding of an object is on the device's list by
 * last use, `lru_prev` and `lru_next`, the least recent first; a range the
 * kernel reserved is on none.
 */
struct i915_vma {
    struct drm_i915_gem_object *obj;
    uint64_t start;
    uint64_t node_size;
    uint64_t size;
    uint32_t view_offset;
    uint32_t view_pages;
    uint32_t pin_count;
    uint32_t fence_size;
    uint32_t fence_alignment;
    struct i915_fence_reg *fence;
    uint8_t mappable;
    uint8_t cache;
    uint8_t userfault;
    uint8_t ggtt_write;
    uint8_t evict;
    struct i915_vma *obj_next;  /* the object's bindings */
    struct i915_vma *ggtt_next; /* the GTT's, by offset */
    struct i915_vma *lru_prev;
    struct i915_vma *lru_next;
};

/* What the object's caching makes coherent between the processor and the
 * GPU without a flush, Linux's I915_BO_CACHE_COHERENT_FOR_ bits. */
#define I915_BO_CACHE_COHERENT_FOR_READ  0x1u
#define I915_BO_CACHE_COHERENT_FOR_WRITE 0x2u

/*
 * The part of an object of GEM_USERPTR (i915_gem_userptr.c): `ptr` is the
 * first byte of the program's memory it stands for, never 0, and `range`
 * is registered on that address space from the object's making, so that a
 * change to the pages there revokes the object's bindings and drops the
 * frames it pinned. `stale` is set when that happened while a binding was
 * pinned, whose frames go with its last pin, which undoes the binding.
 */
struct i915_userptr {
    uintptr_t ptr;
    struct mmn_range range;
    uint8_t stale;
};

/*
 * An object, Linux's drm_i915_gem_object. Its pages are a shmem table,
 * given as they are first touched, or, for an import, the pages of the
 * dma-buf `base.import`, or, for a userptr object, a program's. While
 * anything needs them where the GPU reads, `pages_pin_count` is above 0 and
 * `frames` holds each page's physical address, the object's own pinned
 * through `pin` (sys/mm/pin.h); the array stays until the object goes, is
 * purged, or, for a userptr object, until the program's pages change.
 *
 * `cache_level` is the I915_CACHE_ level its GTT entries take, and
 * `cache_coherent` what that leaves coherent; `cache_dirty` is set when
 * the processor's cache may hold lines the GPU must not miss.
 * `read_domains` and `write_domain` are the I915_GEM_DOMAIN_ bits it was
 * last moved to (SET_DOMAIN). `tiling` and `stride` are its layout
 * (SET_TILING), `vmas` its bindings, `mmo` its mmap offsets by kind, and
 * `userfault_count` how many of its bindings programs map through the
 * aperture. `madv` is the I915_MADV_ state of its pages: WILLNEED, or
 * DONTNEED when reclaim may purge them, or __I915_MADV_PURGED once it did.
 * `dumb` says it was made as a dumb buffer, whose processor maps are
 * write-combined. The device's list of objects holds it (`base.dev_next`).
 */
struct drm_i915_gem_object {
    struct drm_gem_object base;
    struct shmem pages;
    uintptr_t *frames;
    struct pin *pin;
    uint32_t pages_pin_count;
    uint8_t cache_level;
    uint8_t cache_coherent;
    uint8_t cache_dirty;
    uint32_t read_domains;
    uint32_t write_domain;
    uint32_t tiling;
    uint32_t stride;
    struct i915_vma *vmas;
    struct i915_mmap_offset mmo[I915_MMAP_TYPES];
    uint32_t userfault_count;
    uint8_t madv;
    uint8_t dumb;
    struct i915_userptr userptr;
};

/* Whether `obj` was made by GEM_USERPTR. */
#define i915_gem_object_is_userptr(obj) ((obj)->userptr.ptr != 0)

/*
 * A fence of the driver's own, which work on one of its engines signals:
 * `uabi_class` is the engine's class as programs name it (Linux's
 * I915_ENGINE_CLASS_), which BUSY reports. The submission makes them.
 */
struct i915_fence {
    struct dma_fence base;
    uint16_t uabi_class;
};

extern const struct dma_fence_ops i915_fence_ops;

/* The device's shrinker: reclaim's record first, so that reclaim's
 * pointer is this one's, and the device it purges. */
struct i915_shrinker {
    struct mem_shrinker base;
    struct drm_i915_private *i915;
};

/*
 * The device's buffer state (i915_gem.c, i915_vma.c, i915_gem_tiling.c).
 * `bound` lists the bindings by GTT offset. `fences` are the fence
 * registers and `fence_clock` their use's clock. `swizzle_x` and
 * `swizzle_y` are the swizzles of bit 6 the memory controller gives X and
 * Y tiles (I915_BIT_6_SWIZZLE_): none, 9 or 9/10 on Haswell, whose
 * controller never swizzles by bit 17. `aperture` reaches the aperture for
 * the kernel's own accesses. `lru_head` and `lru_tail` are the ends of
 * the list of the objects' bindings by last use. `shrinker` is what
 * reclaim calls to purge objects (i915_gem_shrinker.c); `evictions`
 * counts the bindings eviction undid and `purged` the objects reclaim
 * purged.
 */
struct i915_gem {
    struct i915_vma *bound;
    struct i915_vma *lru_head;
    struct i915_vma *lru_tail;
    struct i915_shrinker shrinker;
    uint32_t evictions;
    uint32_t purged;
    struct i915_fence_reg fences[I915_NUM_FENCES];
    uint32_t fence_clock;
    uint32_t swizzle_x;
    uint32_t swizzle_y;
    struct i915_aperture aperture;
};

/*
 * The display (intel_display.c, intel_plane.c, intel_cursor.c,
 * intel_vblank.c, intel_backlight.c and i915_display_debugfs.c), Linux's
 * display/ for Haswell as far as the Readme's display section says: the
 * firmware's picture read out and adopted, three pipes each with a
 * primary, a sprite and a cursor plane, the eDP panel's connector and its
 * backlight, vblanks, page flips and the pipe CRC.
 */

#define I915_NUM_PIPES 3

/* A pipe's planes, by index into `intel_crtc`'s scanouts. */
#define INTEL_PLANE_PRIMARY 0
#define INTEL_PLANE_SPRITE  1
#define INTEL_PLANE_CURSOR  2
#define INTEL_NUM_PLANES    3

/*
 * What the readout found of a pipe and what feeds it, as Linux's
 * hsw_get_pipe_config reads it into a crtc state. `active` is set when a
 * transcoder routed to the pipe is enabled; `trans` is that transcoder,
 * I915_TRANS_EDP or the pipe's own, and `port` the DDI port its function
 * control selects. `mode` is the timings of the transcoder with the pixel
 * clock in kHz the link's M/N give of `port_clock`, the link's symbol
 * clock in kHz as Linux reports it; `bpp` is the pipe's bits a pixel,
 * three times its bits a colour; `lanes` the link's lanes. `src_w` and
 * `src_h` are PIPESRC's size. The panel fitter's three registers are kept
 * as read, `pf_ctl` 0 when it is off. The primary plane: `plane_on`,
 * `format` (DRM_FORMAT_, 0 for one Linux does not read), `modifier`,
 * `surf` its GGTT address, `stride` in bytes, and `x`, `y` its offset.
 */
struct intel_pipe_readout {
    uint8_t active;
    uint8_t dither;
    int trans;
    int port;
    struct drm_mode_modeinfo mode;
    uint32_t port_clock;
    uint32_t bpp;
    uint32_t lanes;
    uint32_t link_m, link_n, data_m, data_n, tu;
    uint32_t src_w, src_h;
    uint32_t pf_ctl, pf_win_pos, pf_win_sz;
    uint8_t plane_on;
    uint32_t format;
    uint64_t modifier;
    uint32_t surf, stride, x, y;
};

/* The panel power sequencer as read at the bind: its four registers, and
 * whether the panel's power and its backlight enable were on. */
struct intel_pps_readout {
    uint32_t status, control, on_delays, off_delays, divisor;
    uint8_t panel_on, backlight_on;
};

/*
 * A plane's framebuffer as the hardware scans it out: held, with the
 * binding `vma` pinned for scanout, NULL for the firmware's, which lies
 * where the firmware put it. A plane keeps two, what it shows and what it
 * showed before, which the hardware may read until the flip that replaced
 * it latches; that one is let go at the plane's next update, which comes
 * only after the flip (drm_atomic_helper_commit's stall).
 */
struct intel_scanout {
    struct drm_framebuffer *fb;
    struct i915_vma *vma;
};

/*
 * A pipe as a CRTC, with its three planes. `hw` is the readout of the
 * bind. `event` is the commit's event the pipe holds until the frame
 * counter passes `event_frame`, the count read after the commit's writes,
 * or until the flip done of an async flip (`event_async`); NULL for none.
 * `vbl_on` says its vblank interrupt is unmasked. The CRC: `crc_ctl` is
 * PIPE_CRC_CTL's value while a source runs, 0 when none does, and
 * `crc_skip` the frames still to drop after it starts. `shown` and
 * `parked` are each plane's scanouts (`struct intel_scanout`). `wm` is the
 * level 0 watermark word last written. `planes_on` has bit
 * INTEL_PLANE_ for each plane the hardware shows.
 */
struct intel_crtc {
    struct drm_crtc base;
    struct drm_plane primary, sprite, cursor;
    struct drm_i915_private *i915;
    int pipe;
    struct intel_pipe_readout hw;
    struct drm_pending_event *event;
    uint32_t event_frame;
    uint8_t event_async;
    uint8_t vbl_on;
    uint32_t crc_ctl;
    uint32_t crc_skip;
    uint32_t planes_on;
    uint32_t wm;
    struct intel_scanout shown[INTEL_NUM_PLANES];
    struct intel_scanout parked[INTEL_NUM_PLANES];
};

/*
 * The device's display. `crtc` are the pipes A to C, `encoder` DDI A and
 * `connector` its eDP panel. `pps` is the sequencer's readout.
 * `fw_taken` says the firmware's framebuffer was made a framebuffer of the
 * device, whose object lies at `fw_ggtt` in the GGTT. `de_iir` holds the
 * display engine's events the interrupt latched and the deferred work has
 * yet to take, under the interrupt mask's lock.
 * `bl` is the panel's PWM, `bl_index` its backlight's index or -1, and
 * `bl_want_on` what the backlight class last asked of its switch, which
 * holds only while pipe A is on (`bl_pipe_on`). `vga_off` says the VGA
 * plane has been turned off. `lp_wm_off` says the low-power watermarks
 * are off, as they are once a sprite or a cursor is shown. `ready` is set
 * once the device's mode configuration is made, which its release frees.
 */
struct intel_display {
    struct intel_crtc crtc[I915_NUM_PIPES];
    struct drm_encoder encoder;
    struct drm_connector connector;
    struct intel_pps_readout pps;
    uint32_t fw_ggtt;
    uint8_t fw_taken;
    uint32_t de_iir;
    struct intelbl bl;
    int bl_index;
    uint8_t bl_want_on;
    uint8_t bl_pipe_on;
    uint8_t vga_off;
    uint8_t lp_wm_off;
    uint8_t ready;
};

#define to_intel_crtc(c) ((struct intel_crtc *)(c))

/*
 * A bound device. `drm` is first, and its `driver_priv` points back here.
 * `pci` is the function as the probe found it. `hw` reaches its registers.
 * `mmio_phys` is BAR 0's physical address and `gmadr_phys` BAR 2's, the
 * aperture. `gmch_ctrl` and `bsm` are the configuration words of the
 * stolen memory and the GTT's size (SNB_GMCH_CTRL, INTEL_BSM) as the
 * firmware left them, and `subsystem_vendor` and `subsystem_device` the
 * board's ids, and `revision` the chip's revision; all of these come from
 * the backend's configuration space where it answers for one
 * (`config_read32`), and `ident` then holds the identity /sys reports for
 * the function. `msi_vector` is the vector of the device's message, or -1
 * while it has none; `irqs` counts the interrupts taken on it. `opregion`
 * is the firmware's OpRegion and `vbt` the video BIOS table decoded
 * (intel_bios.h), Linux's defaults when the firmware gave none. `display`
 * is the display's state (intel_display.c). `attached`
 * is set once the device is registered and owns its backend, whose
 * `attach`, `detach` and `release` it then calls.
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
    uint8_t revision;
    struct sysfs_pci_ident ident;
    uint8_t attached;
    int msi_vector;
    uint32_t irqs;
    struct i915_uncore uncore;
    struct i915_power power;
    struct i915_stolen stolen;
    struct i915_ggtt ggtt;
    struct i915_irq irq;
    struct i915_gt gt;
    struct i915_rc6 rc6;
    struct i915_rps rps;
    struct intel_opregion opregion;
    struct intel_vbt vbt;
    struct i915_gem gem;
    struct i915_sseu sseu;
    struct intel_display display;
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
 * kept. A backend that answers for configuration space gives the ids and
 * the rest of the chip's identity, which /sys then reports for `pdev`;
 * otherwise they are `pdev`'s. Once registered, the device owns the
 * backend and calls its `attach`; on an error the backend is the
 * caller's again.
 *
 * @return 0, -ENODEV for a device the driver does not bind, -ENOMEM, the
 *         GTT's error or the DRM core's; on an error the command word is
 *         restored and the message is off, and every source stays masked.
 */
int32_t i915_driver_bind(const pci_device_t *pdev, const struct i915_hw *hw);

/**
 * Find the OpRegion through ASLS and decode the VBT it holds, or the option
 * ROM's when it holds none, as Linux's intel_opregion_setup and
 * intel_bios_init do; Linux's defaults stand in when neither has one.
 * `command` is the PCI command word the firmware left, which says whether
 * the device decoded the legacy ROM's shadow at boot. Failures are logged
 * and the bind goes on, as on Linux.
 */
void i915_bios_setup(struct drm_i915_private *i915, uint16_t command);

/* ---- i915_fake.c ---- */

/**
 * Bind the fake chip, a test backend that answers as an HD 4400 and
 * completes every command at once, to each PCI function of QEMU's `edu`
 * device, 1234:11e8, under `rach2.i915fake`; i915_fake.c tells the rest.
 *
 * @return how many were bound, or -ENODEV for none.
 */
int32_t i915_fake_probe(void);

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
 *         GTT; -EBUSY for a range that touches a kept surface; or the
 *         backend's error at I915_FAULT_BIND, -ENOSPC. Nothing is written
 *         on an error.
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
 * ilk_irq_reset does on Haswell, and forget every handler and the
 * deferred work: the state the bind starts from and the unplug leaves.
 * Takes the interrupt mask's lock.
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

/** Mask the bits `bits` of `bank`, with a posting read, from a source's
 * handler, which runs with the interrupt mask's lock held. */
void i915_irq_mask_locked(struct drm_i915_private *i915, int bank, uint32_t bits);

/**
 * The device's interrupt, as Linux's ilk_irq_handler: the master enable
 * and the south display's off, then each bank's identity read, written
 * back and handed to its handlers, the GT's first, then the display
 * engine's with the south display's behind its PCH event, then power
 * management's; then the enables restored, the lock let go, and the
 * `deferred` work run. Counts the message, a message with no bit set as
 * spurious, and a bit with no handler as unclaimed, which it masks and
 * disables. Any context the handler runs in.
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
 * @return 0, or -ENOSPC when it would reach a kept range, a buffer's
 *         binding or offset 0.
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
 * The engine's `reserved` bytes stay free beyond them.
 *
 * @return the address, or NULL when the ring is not running, `ndw` is odd
 *         or too large, the wait reached its bound ("ring space"), or the
 *         backend refused the submission (I915_FAULT_SUBMIT), which
 *         `hw.faults` then counts.
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
 * @return 0, -ENODEV without the option, or `i915_firstlight_run`'s.
 */
int32_t i915_firstlight(struct drm_i915_private *i915);

/* ---- i915_rc6.c ---- */

/**
 * Make RC6 known and turn it off until the driver is ready, as Linux's
 * intel_rc6_init: GEN6_RC_CONTROL and GEN6_RC_STATE cleared under
 * forcewake, whatever the firmware left. Before the rings start.
 */
void i915_rc6_init(struct drm_i915_private *i915);

/**
 * Enable RC6 as Linux's gen6_rc6_enable on Haswell: the wake rate limits,
 * the evaluation interval and idle hysteresis, each engine's idle limit,
 * and the RC1e, RC6, RC6p and RC6pp thresholds, under forcewake; the
 * pcode's RC6 voltages read, as Linux reads them; and `ctl_enable` made
 * RC6 under the hardware's timers, which the GT takes when it unparks.
 * Nothing is written to GEN6_RC_CONTROL here.
 */
void i915_rc6_enable(struct drm_i915_private *i915);

/** Turn RC6 off, GEN6_RC_CONTROL and GEN6_RC_STATE cleared, as Linux's
 * intel_rc6_disable: for the unplug. */
void i915_rc6_disable(struct drm_i915_private *i915);

/** The GT goes idle: RC6's timers off and the GT sent into RC6 at once,
 * as Linux's intel_rc6_park on a part with RC6 alone. */
void i915_rc6_park(struct drm_i915_private *i915);

/** The GT takes work again: RC6 back under the hardware's timers. */
void i915_rc6_unpark(struct drm_i915_private *i915);

/**
 * The milliseconds the GT has spent in RC6 since the bind, as Linux's
 * rc6_residency_ms: the 32-bit counter of 1.28 us units read and extended
 * across its wraps, which a read at least every 91 minutes keeps whole.
 * Takes the register lock. 0 when RC6 is not supported.
 */
uint64_t i915_rc6_residency_ms(struct drm_i915_private *i915);

/**
 * Bring the GT's power up at the bind, as Linux's intel_gt_resume and the
 * park that follows it: RPS enabled, the ring's frequency table written,
 * RC6 enabled, then the GT parked, since no work runs yet, so that it
 * enters RC6 at once.
 */
void i915_gt_pm_enable(struct drm_i915_private *i915);

/** Turn RPS and RC6 off, for the unplug and a failed bind. */
void i915_gt_pm_disable(struct drm_i915_private *i915);

/**
 * The GT takes work: RC6 under its timers and RPS active from the last
 * frequency asked for, with its up and down interrupts, as Linux's
 * __gt_unpark. The code that submits work calls it with the first request
 * and `i915_gt_park` when the last retires. Process context.
 */
void i915_gt_unpark(struct drm_i915_private *i915);

/** The GT is idle: RPS's interrupts off and its frequency at the idle
 * frequency, then RC6 at once, as Linux's __gt_park. */
void i915_gt_park(struct drm_i915_private *i915);

/* ---- i915_rps.c ---- */

/**
 * Read the pcode mailbox's command `mbox`, as Linux's snb_pcode_read: the
 * data written, the command posted with GEN6_PCODE_READY, the ready bit
 * waited for, 20.5 ms at most, then the data read back into `*val` and,
 * when `val1` is not NULL, `*val1`. The callers hold the big kernel lock,
 * which keeps one command in the mailbox at a time, as Linux's sb_lock;
 * never from an interrupt.
 *
 * @return 0; -EAGAIN when a command is in the mailbox already; -ETIMEDOUT
 *         when it was not taken, named "pcode mailbox"; or the pcode's
 *         error as Linux's gen7_check_mailbox_status maps it: -ENXIO,
 *         -EINVAL, -EBUSY, -EACCES or -EOVERFLOW.
 */
int32_t i915_pcode_read(struct drm_i915_private *i915, uint32_t mbox, uint32_t *val,
                        uint32_t *val1);

/** Write `val` to the pcode mailbox's command `mbox`, as Linux's
 * snb_pcode_write: 0, or `i915_pcode_read`'s errors. */
int32_t i915_pcode_write(struct drm_i915_private *i915, uint32_t mbox, uint32_t val);

/**
 * Read the frequency limits, as Linux's gen6_rps_init and intel_rps_init on
 * Haswell: RP0, RP1 and RPn from GEN6_RP_STATE_CAP, RPe from the pcode's
 * duty cycle control, the overclocking limit from its parameters, the
 * softlimits the whole range, 95 and 85 percent as the up and down
 * thresholds, and the start at RPe; then RPS's interrupts off. Registers
 * the up and down interrupts' handler, masked.
 */
void i915_rps_init(struct drm_i915_private *i915);

/**
 * Enable RPS as Linux's gen6_rps_enable, under forcewake: the down timeout
 * of 50 ms and the idle hysteresis written, the up and down interrupts
 * chosen, and the frequency reset to RPn with its thresholds; then the
 * ring's frequency table loaded into the pcode for each GPU frequency, as
 * Linux's gen6_update_ring_freq. RPS stays inactive until the GT unparks.
 * Leaves RPS disabled when the chip has no range to move in.
 */
void i915_rps_enable(struct drm_i915_private *i915);

/** Turn RPS off, its interrupts off and GEN6_RP_CONTROL cleared, as
 * Linux's intel_rps_disable. */
void i915_rps_disable(struct drm_i915_private *i915);

/** The GT unparks: RPS active at the frequency asked for, within the
 * softlimits, and its interrupts on, as Linux's intel_rps_unpark. */
void i915_rps_unpark(struct drm_i915_private *i915);

/** The GT parks: RPS's interrupts off, the idle frequency written under
 * forcewake, and the frequency to start from next stepped down, as
 * Linux's intel_rps_park. */
void i915_rps_park(struct drm_i915_private *i915);

/**
 * Act on the up and down interrupts the handler latched, as Linux's
 * rps_work: step the frequency up or down, or to RPe or RPn on the down
 * timeout, within the softlimits, then unmask the events again. Runs as
 * the interrupt's `deferred` work, with no lock of the driver held, and
 * may be called from anywhere else; it takes the frequency lock.
 */
void i915_rps_work(struct drm_i915_private *i915);

/**
 * Set the frequency `val`, in the chip's units, as Linux's intel_rps_set:
 * while RPS is active, GEN6_RPNSWREQ, the thresholds of its power mode,
 * the interrupt limits and GEN6_PMINTRMSK; `cur_freq` either way. Under
 * the frequency lock. 0, or -EINVAL for a frequency outside
 * [min_freq, max_freq].
 */
int32_t i915_rps_set_locked(struct drm_i915_private *i915, int val);

/** The frequency the GPU runs at, in MHz, from GEN6_RPSTAT1's CAGF, as
 * Linux's intel_rps_read_actual_frequency. */
uint32_t i915_rps_read_actual_mhz(struct drm_i915_private *i915);

/**
 * Set the softlimit gt_max_freq_mhz or gt_min_freq_mhz shows, `val` in
 * MHz, rounded to the nearest 50, as Linux's set_max_freq and
 * set_min_freq, and ask for the current frequency again within the new
 * limits.
 *
 * @return 0, or -EINVAL for a value outside [min_freq, max_freq] or past
 *         the other softlimit.
 */
int32_t i915_rps_set_max_mhz(struct drm_i915_private *i915, uint32_t val);
int32_t i915_rps_set_min_mhz(struct drm_i915_private *i915, uint32_t val);

/** Set the boost frequency, gt_boost_freq_mhz, `val` in MHz: 0, or
 * -EINVAL outside [min_freq, max_freq]. */
int32_t i915_rps_set_boost_mhz(struct drm_i915_private *i915, uint32_t val);

/**
 * Add the device's /sys files below its card's directory, as Linux's
 * intel_gt_sysfs_pm_init on Haswell: gt_act_freq_mhz, gt_boost_freq_mhz,
 * gt_cur_freq_mhz, gt_max_freq_mhz, gt_min_freq_mhz, gt_RP0_freq_mhz,
 * gt_RP1_freq_mhz and gt_RPn_freq_mhz, and power/rc6_enable and
 * power/rc6_residency_ms. Once the DRM device is registered; they go with
 * the card's directory. A file that cannot be added is logged.
 */
void i915_sysfs_register(struct drm_i915_private *i915);

/**
 * First light whatever the command line says, as the fake chip's debugfs
 * file runs it on demand. The breadcrumb is polled; with the processor's
 * interrupts on, the user interrupt is taken by the handler.
 *
 * @return 0, -EIO when the ring is not running, its submission is refused
 *         or the value read back is wrong, -ENOMEM, or -ETIME when the
 *         breadcrumb or the interrupt did not come.
 */
int32_t i915_firstlight_run(struct drm_i915_private *i915);

/*
 * Buffers (i915_gem.c, i915_vma.c, i915_gem_tiling.c, i915_gem_mman.c,
 * i915_gem_prime.c, i915_gem_evict.c, i915_gem_shrinker.c,
 * i915_gem_userptr.c and i915_fence.c). Every one is
 * called under the big kernel lock; those that wait may sleep, and the lock
 * is let go while they do.
 */

extern const struct drm_gem_object_funcs i915_gem_object_funcs;

/* The driver's own ioctls, for the DRM core. */
#define I915_NUM_IOCTLS 22
extern const struct drm_ioctl_desc i915_ioctls[I915_NUM_IOCTLS];

/**
 * Hold the object's pages where the GPU reads them, as Linux's
 * i915_gem_object_pin_pages: a shmem object's pages are all given and
 * pinned at the first pin, memory short of them reclaimed first and once
 * more after a failure; an import's are its dma-buf's; a userptr object's
 * are the program's (`i915_gem_userptr_get_pages`). The physical addresses
 * stay in `obj->frames` until the object goes or is purged. An object
 * without pages whose `madv` is not WILLNEED has none to give.
 *
 * @return 0; -ENOMEM; -EFAULT when a page cannot be had or the object is
 *         purgeable or purged; -ENODEV once the device is gone; or the
 *         userptr's errors.
 */
int32_t i915_gem_object_pin_pages(struct drm_i915_gem_object *obj);

/** Drop a pin `i915_gem_object_pin_pages` took; a userptr object's stale
 * frames go with the last. */
void i915_gem_object_unpin_pages(struct drm_i915_gem_object *obj);

/** Whether the object has pages: frames held, or a shmem page given. */
int i915_gem_object_has_pages(const struct drm_i915_gem_object *obj);

/** Whether a fence of the object's reservation has not signalled, so that
 * the GPU may still read or write its pages. */
int i915_gem_object_is_active(const struct drm_i915_gem_object *obj);

/** The physical address of the object's page `pgoff`, a shmem page given
 * on the first touch; 0 past its end or when none can be had. */
uintptr_t i915_gem_object_frame(struct drm_i915_gem_object *obj, uint32_t pgoff);

/** Set the cache level of the object's GTT entries and what it leaves
 * coherent, as Linux's i915_gem_object_set_cache_coherency; nothing is
 * rebound. */
void i915_gem_object_set_cache_coherency(struct drm_i915_gem_object *obj, int level);

/** A new object of `size` bytes, a whole number of pages, with one
 * reference, its pages in the CPU's domains and cached in the LLC; NULL
 * without memory. */
struct drm_i915_gem_object *i915_gem_object_alloc(struct drm_i915_private *i915, uint64_t size);

/** A new object of `size` bytes rounded up to pages, in `*out`: 0, -EINVAL
 * for 0, -E2BIG past what an object holds, or -ENOMEM. */
int32_t i915_gem_object_create(struct drm_i915_private *i915, uint64_t size,
                               struct drm_i915_gem_object **out);

/** The driver's object of `handle` in `file` with a reference, or NULL
 * for no handle or another driver's object. */
struct drm_i915_gem_object *i915_gem_object_lookup(struct drm_file *file, uint32_t handle);

/** Drop a reference; NULL is ignored. */
void i915_gem_object_put(struct drm_i915_gem_object *obj);

/**
 * Wait for the object's writers, or with `all` for its readers too, for
 * `timeout_ns`, for ever when it is negative, as Linux's
 * i915_gem_object_wait.
 *
 * @return 0, -ETIME when one is busy at the timeout (at once for 0),
 *         -EINTR for a signal, or -ENOMEM.
 */
int32_t i915_gem_object_wait(struct drm_i915_gem_object *obj, int all, int64_t timeout_ns);

/** Whether a write by the processor must be flushed from its cache for
 * the GPU to see it, as Linux's i915_gem_cpu_write_needs_clflush. */
int i915_gem_cpu_write_needs_clflush(const struct drm_i915_gem_object *obj);

/** Make the writes of the object's write domain, if it is one of
 * `flush_domains`, reach memory, and leave it with none, as Linux's
 * flush_write_domain. */
void i915_gem_object_flush_write_domain(struct drm_i915_gem_object *obj, uint32_t flush_domains);

/**
 * Move the object into the write-combining, aperture or processor's
 * domain for reading, or with `write` for writing, as Linux's
 * i915_gem_object_set_to_wc_domain, _gtt_domain and _cpu_domain: its work
 * waited for and the old domain's writes flushed.
 *
 * @return 0, or the errors of `i915_gem_object_wait` and
 *         `i915_gem_object_pin_pages`.
 */
int32_t i915_gem_object_set_to_wc_domain(struct drm_i915_gem_object *obj, int write);
int32_t i915_gem_object_set_to_gtt_domain(struct drm_i915_gem_object *obj, int write);
int32_t i915_gem_object_set_to_cpu_domain(struct drm_i915_gem_object *obj, int write);

/** Give the object the cache level `level`, its work waited for and its
 * bindings undone, as Linux's i915_gem_object_set_cache_level: 0, the
 * wait's errors, or -EAGAIN when a binding is held. */
int32_t i915_gem_object_set_cache_level(struct drm_i915_gem_object *obj, int level);

/* The ioctls, as Linux's of the same names; each one's comment is at its
 * definition. */
int32_t i915_gem_create_ioctl(struct drm_device *dev, void *data, struct drm_file *file);
int32_t i915_gem_pread_ioctl(struct drm_device *dev, void *data, struct drm_file *file);
int32_t i915_gem_pwrite_ioctl(struct drm_device *dev, void *data, struct drm_file *file);
int32_t i915_gem_set_domain_ioctl(struct drm_device *dev, void *data, struct drm_file *file);
int32_t i915_gem_sw_finish_ioctl(struct drm_device *dev, void *data, struct drm_file *file);
int32_t i915_gem_set_caching_ioctl(struct drm_device *dev, void *data, struct drm_file *file);
int32_t i915_gem_get_caching_ioctl(struct drm_device *dev, void *data, struct drm_file *file);
int32_t i915_gem_busy_ioctl(struct drm_device *dev, void *data, struct drm_file *file);
int32_t i915_gem_wait_ioctl(struct drm_device *dev, void *data, struct drm_file *file);
int32_t i915_gem_set_tiling_ioctl(struct drm_device *dev, void *data, struct drm_file *file);
int32_t i915_gem_get_tiling_ioctl(struct drm_device *dev, void *data, struct drm_file *file);
int32_t i915_gem_mmap_offset_ioctl(struct drm_device *dev, void *data, struct drm_file *file);
int32_t i915_gem_get_aperture_ioctl(struct drm_device *dev, void *data, struct drm_file *file);
int32_t i915_gem_madvise_ioctl(struct drm_device *dev, void *data, struct drm_file *file);
int32_t i915_gem_userptr_ioctl(struct drm_device *dev, void *data, struct drm_file *file);

/** Revoke every program's mapping of every object, once the device is
 * unplugged, so that the next access faults to a device that refuses it. */
void i915_gem_unplug(struct drm_i915_private *i915);

/**
 * Reserve `size` bytes of the global GTT, inside the aperture with
 * I915_PIN_MAPPABLE, for the kernel's own use, as Linux's
 * i915_gem_gtt_reserve, into `*offset`. The entries are the caller's to
 * write and clear.
 *
 * @return 0, -ENOSPC, or -ENOMEM.
 */
int32_t i915_ggtt_reserve(struct drm_i915_private *i915, uint64_t size, uint32_t flags,
                          uint64_t *offset);

/** Give back the range `i915_ggtt_reserve` gave at `offset`. */
void i915_ggtt_release(struct drm_i915_private *i915, uint64_t offset);

/**
 * Bind the object's view of `view_pages` pages from `view_offset`, or the
 * whole object for 0, into the global GTT and pin the binding, as Linux's
 * i915_gem_object_ggtt_pin: a binding that exists is reused unless it is
 * outside the aperture with I915_PIN_MAPPABLE or not aligned to `align`,
 * when it is undone first. With I915_PIN_MAPPABLE the binding is sized
 * and aligned for a fence of the object's tiling. In `*out`.
 *
 * @return 0; -EINVAL for a view past the object; -ENOSPC without room or
 *         when a binding to move is held; or the errors of
 *         `i915_gem_object_pin_pages` and `i915_ggtt_insert`.
 */
int32_t i915_vma_pin(struct drm_i915_gem_object *obj, uint32_t view_offset, uint32_t view_pages,
                     uint32_t flags, uint32_t align, struct i915_vma **out);

/** Drop a pin `i915_vma_pin` took; the binding stays, but for a userptr
 * object's whose program's pages changed while it was pinned, which the
 * last pin undoes. */
void i915_vma_unpin(struct i915_vma *vma);

/** Unmap the binding's pages from every program that maps them through
 * the aperture, as Linux's i915_vma_revoke_mmap. */
void i915_vma_revoke_mmap(struct i915_vma *vma);

/** Make the writes through the binding's aperture reach memory, as Linux's
 * i915_vma_flush_writes. */
void i915_vma_flush_writes(struct i915_vma *vma);

/**
 * Undo a binding, as Linux's __i915_vma_unbind: programs' maps revoked,
 * writes flushed, its fence let go, its entries cleared and its record
 * freed.
 *
 * @return 0, or -EAGAIN while it is pinned.
 */
int32_t i915_vma_unbind(struct i915_vma *vma);

/** Undo every binding of the object: 0, or -EAGAIN when one is pinned. */
int32_t i915_gem_object_unbind(struct drm_i915_gem_object *obj);

/** The object's binding inside the aperture that maps page `pgoff`, or
 * NULL. */
struct i915_vma *i915_vma_for_page(struct drm_i915_gem_object *obj, uint32_t pgoff);

/** Recompute what a fence needs of the binding after its object's tiling
 * changed, and whether it still serves a GTT map and a fence. */
void i915_vma_update_fenceable(struct i915_vma *vma);

/**
 * The range of `size` bytes aligned to `align` below `hi` of the global
 * GTT that no binding, kept surface or object of the GT's touches, the
 * lowest, or with I915_PIN_HIGH the highest, in `*at`; a binding marked
 * `evict` counts as free. 0, or -ENOSPC.
 */
int32_t i915_ggtt_find(struct drm_i915_private *i915, uint64_t size, uint64_t align, uint64_t hi,
                       uint32_t flags, uint64_t *at);

/**
 * Make room in [start, end) of the global GTT for `size` bytes aligned to
 * `align`, at the end `flags` names, as Linux's i915_gem_evict_something:
 * the bindings of objects are scanned least recently used first, each
 * unpinned one whose object is idle counted as free, until a range fits;
 * then those of the scanned that lie in the range are undone. A pinned
 * binding, one whose object the GPU may still use, and a range the kernel
 * reserved are never undone. Does not sleep.
 *
 * @return 0, or -ENOSPC when even the idle bindings make no room.
 */
int32_t i915_gem_evict_something(struct drm_i915_private *i915, uint64_t size, uint64_t align,
                                 uint64_t start, uint64_t end, uint32_t flags);

/* ---- i915_gem_shrinker.c ---- */

/**
 * Purge the object, as Linux's i915_gem_object_truncate on shmem: every
 * program's map of it revoked, its frames unpinned and its pages freed,
 * and `madv` made __I915_MADV_PURGED. The caller has undone its bindings
 * and holds no pin of its pages. Returns the frames freed.
 */
uint32_t i915_gem_object_truncate(struct drm_i915_gem_object *obj);

/**
 * Purge the idle objects of the device whose pages programs said they can
 * lose (MADVISE's DONTNEED) until `want` frames came back, as Linux's
 * i915_gem_shrink with its purgeable objects: an object is passed over
 * while the GPU may use it or a pin holds its pages or a binding of it.
 * Does not sleep. Returns the frames freed.
 */
uint32_t i915_gem_shrink(struct drm_i915_private *i915, uint32_t want);

/** Register the device's shrinker with reclaim, once it is bound. */
void i915_gem_shrinker_register(struct drm_i915_private *i915);

/** Take the device's shrinker off; nothing when it is not registered. */
void i915_gem_shrinker_unregister(struct drm_i915_private *i915);

/* ---- i915_gem_userptr.c ---- */

/**
 * Pin a userptr object's pages, the program's, for the GPU, as Linux's
 * i915_gem_object_userptr_submit_init and get_pages: each page brought in
 * for a write and its frame pinned, which no limit of RLIMIT_MEMLOCK
 * counts, as on Linux. Only the address space that made the object may.
 * May sleep. Called by `i915_gem_object_pin_pages`.
 *
 * @return 0; -EFAULT for another address space, one that has ended, or a
 *         page outside the program's memory or without write access;
 *         -EAGAIN while stale frames are still pinned; or -ENOMEM.
 */
int32_t i915_gem_userptr_get_pages(struct drm_i915_gem_object *obj);

/** Drop a userptr object's frames, which no pin holds any more. */
void i915_gem_userptr_put_pages(struct drm_i915_gem_object *obj);

/** Take a userptr object's range off its address space and drop its
 * frames, for its `free`. */
void i915_gem_userptr_release(struct drm_i915_gem_object *obj);

/** The bytes of a tile row, `stride` times the tile's height, for
 * `tiling`; 0 untiled. */
uint32_t i915_gem_tile_row_size(uint32_t tiling, uint32_t stride);

/** The size a fence of `tiling` and `stride` covers for `size` bytes, as
 * Linux's i915_gem_fence_size: whole tile rows, or `size` untiled. */
uint32_t i915_gem_fence_size(struct drm_i915_private *i915, uint32_t size, uint32_t tiling,
                             uint32_t stride);

/** The alignment a binding needs for a fence of `tiling`, as Linux's
 * i915_gem_fence_alignment: a page on Haswell. */
uint32_t i915_gem_fence_alignment(struct drm_i915_private *i915, uint32_t tiling);

/**
 * Give the binding the fence its object's tiling needs and pin it, as
 * Linux's i915_vma_pin_fence: its own when it has one and the tiling has
 * not changed, otherwise a free one or the least recently used, whose old
 * binding's GTT maps are revoked; an untiled object's fence is let go.
 *
 * @return 0, -ENOBUFS when every fence is pinned, or -EINVAL for a binding
 *         that cannot take a fence.
 */
int32_t i915_vma_pin_fence(struct i915_vma *vma);

/** Drop a pin `i915_vma_pin_fence` took. */
void i915_vma_unpin_fence(struct i915_vma *vma);

/** Take the binding's fence away and clear the register, as Linux's
 * i915_vma_revoke_fence; the fence must not be pinned. */
void i915_vma_revoke_fence(struct i915_vma *vma);

/** Revoke every program's GTT map of the object, as Linux's
 * i915_gem_object_release_mmap_gtt, its writes through them flushed. */
void i915_gem_object_release_mmap_gtt(struct drm_i915_gem_object *obj);

/** The `mmap` of an object's mmap offset (`struct drm_mmap_node`): a device
 * region over its pages or through the aperture, by the offset's kind. */
int32_t i915_gem_mmo_mmap(struct drm_mmap_node *node, vma_t *vma);

/** The `mmap` of an object for its dma-buf: its pages write-back, or
 * -ENODEV for an import. */
int32_t i915_gem_object_mmap_cpu(struct drm_gem_object *base, vma_t *vma);

/** The object of another device's dma-buf `buf`, for the core's PRIME, as
 * Linux's i915_gem_prime_import: its pages are the buffer's, in a domain
 * coherent with memory. NULL with `*err` -E2BIG, -EINVAL for a buffer
 * without pages, or -ENOMEM. */
struct drm_gem_object *i915_gem_prime_import(struct drm_device *dev, struct dma_buf *buf,
                                             int32_t *err);

/** The `cpu_access` of an exported object, for DMA_BUF_IOCTL_SYNC: into
 * the processor's domain at the start of an access, back to the aperture's
 * at its end, as Linux's i915_gem_begin_cpu_access and _end_cpu_access. */
int32_t i915_gem_object_cpu_access(struct drm_gem_object *base, uint64_t flags);

/** Write fence register `id` (i915_fence.c), whose comment says more. */
void i915_fence_write(struct drm_i915_private *i915, int id, uint64_t start, uint64_t size,
                      uint32_t stride, uint32_t tiling);

/*
 * The display. Each runs under the big kernel lock in process context
 * unless it says otherwise; the commit's hooks run under the DRM device's
 * commit lock.
 */

/**
 * Read out each pipe's state into `display.crtc[N].hw`, and the panel
 * power sequencer's into `display.pps`, as Linux's
 * intel_modeset_readout_hw_state does at load: nothing is written.
 */
void intel_display_readout(struct drm_i915_private *i915);

/**
 * Read out pipe `pipe` into `*out`, as Linux's hsw_get_pipe_config and
 * i9xx_get_initial_plane_config: zeroed when no enabled transcoder feeds
 * it. Nothing is written.
 */
void intel_pipe_readout(struct drm_i915_private *i915, int pipe, struct intel_pipe_readout *out);

/**
 * Make the display and adopt the firmware's picture: read it out, make
 * the planes, CRTCs, the encoder and the eDP connector, take the
 * firmware's framebuffer as a framebuffer of the device on the pipe that
 * scans it out with the mode the readout found, take the panel's
 * backlight over, and hook the display's interrupts. The device then has
 * DRIVER_MODESET and DRIVER_ATOMIC. Before the DRM device registers.
 *
 * @return 0, or -ENOMEM or the DRM core's error, with nothing made that
 *         `intel_display_fini` would not undo.
 */
int32_t intel_display_init(struct drm_i915_private *i915);

/**
 * Undo what `intel_display_init` hooked into the hardware's paths: the
 * interrupts, the backlight, given back to the boot's driver, and every
 * scanout's pin. The DRM core frees the objects with the device's mode
 * configuration. From the unplug.
 */
void intel_display_fini(struct drm_i915_private *i915);

/**
 * The commit, with Linux's prepare_fb and cleanup_fb around the core's:
 * each framebuffer of a plane state the commit carries is pinned for
 * scanout, and what each plane showed before the last update is let go
 * once the commit is made, for `mode_config.atomic_commit`. 0, or the
 * pin's or the core's error with nothing changed.
 */
int32_t intel_atomic_commit(struct drm_device *dev, struct drm_atomic_state *state, int nonblock);

/** The GGTT address of the framebuffer `fb`'s object, which its plane's
 * pin bound: the scanout's base, without the framebuffer's offset. */
uint32_t intel_fb_ggtt(const struct drm_framebuffer *fb);

/** The modifier `fb` is scanned out with: its own, or X tiling for a
 * framebuffer made without modifiers of an X-tiled object, as Linux's
 * intel_framebuffer_init takes it. */
uint64_t intel_fb_modifier(const struct drm_framebuffer *fb);

/**
 * Make the three planes of `crtc`: the primary, the sprite and the cursor,
 * with Haswell's formats and modifiers, rotation by 0 and 180 degrees and
 * the zpos Linux gives them. 0 or the DRM core's error.
 */
int32_t intel_planes_init(struct intel_crtc *crtc);

/** Make the cursor plane of `crtc`, as `intel_planes_init` does. */
int32_t intel_cursor_init(struct intel_crtc *crtc);

/**
 * Write pipe `crtc`'s level 0 watermarks for the planes it shows, Linux's
 * ilk_compute_wm level 0 from the latencies Haswell's pcode gives, and
 * turn the low-power watermarks off the first time a sprite or a cursor
 * shows. Inside a commit's planes.
 */
void intel_update_wm(struct intel_crtc *crtc);

/** Unmask the flip done of `crtc` when its new state in `state` is an
 * async flip with an event. From the CRTC's `atomic_begin`, before the
 * planes are written. */
void intel_crtc_arm_flip_done(struct intel_crtc *crtc, struct drm_atomic_state *state);

/**
 * Hand the event of `crtc`'s new state in `state` to the vblank path: the
 * commit's flip armed and its event held until the frame counter passes
 * the count read now, or sent now on a pipe that is off or without its
 * vblanks. From the CRTC's `atomic_flush`, after the planes are written.
 */
void intel_crtc_arm_event(struct intel_crtc *crtc, struct drm_atomic_state *state);

/** The CRTC's `enable_vblank`: unmask the pipe's vblank. 0. Any context. */
int32_t intel_enable_vblank(struct drm_crtc *crtc);
/** The CRTC's `disable_vblank`: mask it. Any context. */
void intel_disable_vblank(struct drm_crtc *crtc);
/** The CRTC's `get_vblank_counter`: PIPE_FRMCOUNT. Any context. */
uint32_t intel_get_vblank_counter(struct drm_crtc *crtc);

/**
 * Hook the display's interrupts: each pipe's vblank and primary flip done,
 * masked until a holder wants them, and the deferred work that takes them.
 * 0 or i915_irq_register's error.
 */
int32_t intel_vblank_init(struct drm_i915_private *i915);

/** Unhook them, with every held event sent. */
void intel_vblank_fini(struct drm_i915_private *i915);

/** Send `crtc`'s held event now, as a pipe that goes off does, with
 * interrupts off. */
void intel_crtc_send_event(struct intel_crtc *crtc);

/**
 * The display's deferred interrupt work, `irq.deferred_display`: each
 * pipe's vblank counted by the DRM core, the events whose frame has come
 * sent, and the CRC of each frame entered. In the interrupt, after the
 * mask's lock is let go.
 */
void intel_display_irq_work(struct drm_i915_private *i915);

/** The CRC hooks of the CRTCs (i915_display_debugfs.c). */
int32_t intel_crtc_set_crc_source(struct drm_crtc *crtc, const char *name);
int32_t intel_crtc_verify_crc_source(struct drm_crtc *crtc, const char *name, uint32_t *values_cnt);
const char *const *intel_crtc_get_crc_sources(struct drm_crtc *crtc, uint32_t *count);

/** Enter the CRC of the frame that just ended on `crtc` while a source
 * runs, past the first, as Linux's display_pipe_crc_irq_handler; from the
 * deferred interrupt work. */
void intel_crtc_crc_frame(struct intel_crtc *crtc);

/**
 * Take the eDP panel's backlight over from the boot's driver: attach the
 * PCH's PWM through the device's registers and drive "intel_backlight"
 * through it, registering it when the boot did not. Nothing when the
 * firmware left the PWM off. From the connector's init.
 */
void intel_backlight_setup(struct drm_i915_private *i915);

/** Turn the backlight's enable line with pipe A: off while the pipe is
 * off, and on again at the class's wish when it comes back. */
void intel_backlight_pipe(struct drm_i915_private *i915, int on);

/** Give the backlight back to the boot's driver, from the unplug. */
void intel_backlight_fini(struct drm_i915_private *i915);

/**
 * Make a dumb buffer, Linux's i915_gem_dumb_create: a linear object with
 * its pitch aligned to 64 bytes, whose processor maps are write-combined,
 * as Linux's MAP_DUMB maps one. The driver's `dumb_create`.
 */
int32_t i915_gem_dumb_create(struct drm_file *file, struct drm_device *dev,
                             struct drm_mode_create_dumb *args);

/**
 * Get the object ready to scan out, as Linux's
 * i915_gem_object_pin_to_display_plane: its caching off, bound inside the
 * aperture at a multiple of `align`, and its processor cache flushed. The
 * binding in `*vma`, pinned, which `i915_vma_unpin` lets go. 0 or the
 * binding's error.
 */
int32_t i915_gem_object_pin_to_display(struct drm_i915_gem_object *obj, uint32_t align,
                                       struct i915_vma **vma);

/** Flush the processor's cache of bytes [start, end) of the object, for
 * rows drawn by the processor that the display reads. */
void i915_gem_object_flush_range(struct drm_i915_gem_object *obj, uint64_t start, uint64_t end);

/** Read the swizzle, clear every fence and set the GPU's swizzling, once
 * at the bind. */
void i915_gem_init_hw(struct drm_i915_private *i915);

/** Where byte `v` of a linear surface lies once tiled and swizzled, the
 * arithmetic a fence does (i915_fence.c). */
uint32_t i915_tiled_offset(uint32_t tiling, uint32_t stride, uint32_t swizzle, uint32_t v);

/*
 * Contexts, requests and parameters (i915_gem_context.c, i915_request.c,
 * i915_getparam.c), after Linux 6.18's gem/i915_gem_context.c,
 * gt/intel_context.c, i915_request.c, gt/intel_ring_submission.c,
 * i915_getparam.c and i915_query.c. Every call is made under the big
 * kernel lock; those that wait may sleep, and the lock is let go while
 * they do.
 */

struct i915_gem_context;

/* The pages of the render ring's context image on Haswell, Linux's
 * HSW_CXT_TOTAL_SIZE. Linux reads no CXT_SIZE register on Haswell, whose
 * value it calls not valid there. */
#define I915_HSW_CXT_PAGES 17

/*
 * A context's state on one engine, Linux's intel_context. `ctx` is the
 * user's context it belongs to, with a reference, or NULL for the GT's
 * own. `refs` counts its references: its context's, the requests made on
 * it, and the engine's `last_ce` while the chip holds its state. On the
 * render ring it has an image of I915_HSW_CXT_PAGES pages at `image`,
 * mapped at `image_ggtt`, made at its first request; `image_valid` is set
 * once the image holds state to restore (Linux's CONTEXT_VALID_BIT): the
 * default state, or what the chip will have saved before it next loads
 * it. `sseu` is the slices and EUs it asks for, the GT's on the render
 * ring and none elsewhere.
 */
struct intel_context {
    struct drm_i915_private *i915;
    struct i915_gem_context *ctx;
    struct i915_engine *engine;
    uint32_t refs;
    uint32_t *image;
    uint32_t image_ggtt;
    uint8_t image_valid;
    uint8_t slice_mask;
    uint8_t subslice_mask;
    uint8_t min_eus;
    uint8_t max_eus;
};

/* A context's flags, Linux's UCONTEXT_ bits. */
#define I915_CTX_NO_ERROR_CAPTURE 0x1u
#define I915_CTX_BANNABLE         0x2u
#define I915_CTX_RECOVERABLE      0x4u
#define I915_CTX_PERSISTENCE      0x8u

/*
 * A user's context, Linux's i915_gem_context and, while `proto` is set,
 * its i915_gem_proto_context. `id` names it in its file, whose list holds
 * one of its `refs`; lookups and each of its engines' states hold the
 * rest. A proto context takes its parameters until its first use as a
 * context, its finalizing, which makes its states: one for each engine,
 * by the engine's index, or, when the user gave an engine map, one for
 * each slot of `map`, each an engine's index or -1 for an empty slot.
 * `nmap` is -1 without a map. `ce` holds the `nce` states, NULL for an
 * empty slot. `guilty_count` and `active_count` are what GET_RESET_STATS
 * reports: the batches of this context that hung the GPU, and those lost
 * in a reset it did not cause, which the reset (78-6a) counts. `closed`
 * is set once DESTROY or the file's close took it from the file.
 */
struct i915_gem_context {
    struct drm_i915_private *i915;
    uint32_t id;
    uint32_t refs;
    uint32_t flags;
    uint8_t proto;
    uint8_t closed;
    int nmap;
    int8_t *map;
    uint32_t nce;
    struct intel_context **ce;
    uint32_t guilty_count;
    uint32_t active_count;
    struct i915_gem_context *next; /* the file's, by id */
};

/*
 * A file's state, Linux's drm_i915_file_private: its contexts by id, the
 * default context 0 first, and `ban_score`, the score of its contexts'
 * hangs and bans that 78-6a keeps, at which it may make no more
 * (I915_CLIENT_SCORE_BANNED).
 */
struct i915_file {
    struct drm_i915_private *i915;
    struct i915_gem_context *contexts;
    uint32_t ban_score;
};

#define I915_CLIENT_SCORE_BANNED 9

/*
 * A request: commands on one engine for one context, which a breadcrumb
 * closes, Linux's i915_request. Its fence is its first member, so that the
 * fence's last reference frees it (i915_fence_ops). The fence's seqno is
 * `seqno`, which the breadcrumb writes. `ce` is its context's state, with
 * a reference. When it switches the chip's state from another context's,
 * `prev_ce` is that one, whose reference it holds until it retires, since
 * the chip saves into its image until then; `switched` says so, since
 * `prev_ce` is NULL at the first switch. `set_context` is set when it
 * loads `ce`'s image, and `was_valid` is whether the image was valid
 * before. `head`, `postfix` and `tail` are its ring offsets: its first
 * command, its breadcrumb and past its end.
 */
struct i915_request {
    struct i915_fence fence;
    struct drm_i915_private *i915;
    struct i915_engine *engine;
    struct intel_context *ce;
    struct intel_context *prev_ce;
    uint32_t seqno;
    uint32_t head;
    uint32_t postfix;
    uint32_t tail;
    uint8_t switched;
    uint8_t set_context;
    uint8_t was_valid;
    struct i915_request *next; /* the engine's, oldest first */
};

/** The driver's open hook: the file's state and its default context, 0.
 * 0 or -ENOMEM. */
int32_t i915_driver_open(struct drm_device *dev, struct drm_file *file);

/** The driver's postclose hook: every context of the file closed and its
 * requests retired as they complete. */
void i915_driver_postclose(struct drm_device *dev, struct drm_file *file);

/**
 * The context `id` of `file` with a reference, finalized if it was a
 * proto context, as Linux's i915_gem_context_lookup, in `*out`.
 *
 * @return 0, -ENOENT for no such context, or -ENOMEM.
 */
int32_t i915_gem_context_lookup(struct drm_file *file, uint32_t id, struct i915_gem_context **out);

/** Drop a reference to a context; the last frees it. */
void i915_gem_context_put(struct i915_gem_context *ctx);

/** The state of slot `idx` of a finalized context's engines with a
 * reference: by engine index, or by the slot of its map. NULL for a slot
 * past the end or empty. */
struct intel_context *i915_gem_context_engine(struct i915_gem_context *ctx, uint32_t idx);

/** A new state of `engine` for the context `ctx`, or for the GT with NULL,
 * with one reference; NULL without memory. */
struct intel_context *intel_context_create(struct drm_i915_private *i915,
                                           struct i915_engine *engine,
                                           struct i915_gem_context *ctx);

/** Take a reference to a context's state. */
struct intel_context *intel_context_get(struct intel_context *ce);

/** Drop a reference to a context's state; the last frees its image. */
void intel_context_put(struct intel_context *ce);

/**
 * Give a render ring's state its image if it has none: the pages mapped at
 * the LLC's level, from the default state when the GT has recorded it, and
 * then valid. Nothing on another engine.
 *
 * @return 0, -ENOMEM, or -ENOSPC without room in the global GTT.
 */
int32_t intel_context_pin_image(struct intel_context *ce);

/* The ioctls, as Linux's of the same names. */
int32_t i915_gem_context_create_ioctl(struct drm_device *dev, void *data, struct drm_file *file);
int32_t i915_gem_context_destroy_ioctl(struct drm_device *dev, void *data, struct drm_file *file);
int32_t i915_gem_context_getparam_ioctl(struct drm_device *dev, void *data, struct drm_file *file);
int32_t i915_gem_context_setparam_ioctl(struct drm_device *dev, void *data, struct drm_file *file);
int32_t i915_gem_context_reset_stats_ioctl(struct drm_device *dev, void *data,
                                           struct drm_file *file);
int32_t i915_getparam_ioctl(struct drm_device *dev, void *data, struct drm_file *file);
int32_t i915_query_ioctl(struct drm_device *dev, void *data, struct drm_file *file);

/** Read the GT's slices, subslices, EUs and eDRAM once, as Linux's
 * hsw_sseu_info_init and its eDRAM probe. */
void i915_sseu_init(struct drm_i915_private *i915);

/** The engine of the class and instance programs name (I915_ENGINE_CLASS_),
 * or NULL, as Linux's intel_engine_lookup_user over the engines that run:
 * rcs0 and bcs0. */
struct i915_engine *i915_engine_lookup_user(struct drm_i915_private *i915, uint16_t cls,
                                            uint16_t instance);

/** The class programs give `engine`, I915_ENGINE_CLASS_RENDER or _COPY. */
uint16_t i915_engine_uabi_class(const struct i915_engine *engine);

/**
 * Begin a request on the state `ce`, as Linux's i915_request_create on a
 * legacy ring: requests that completed retired, the GT unparked, the
 * GT's own objects and the default state made with the first, then the
 * caches and TLBs invalidated and the chip switched to `ce`'s state. Room
 * for the breadcrumb is kept until `i915_request_add`, which must follow
 * before the caller sleeps. In `*out`, with one reference, the caller's.
 *
 * @return 0; -EIO when the GPU is wedged or the ring refuses the commands;
 *         -ENOMEM; -ENOSPC without room in the GTT for an image.
 */
int32_t i915_request_create(struct intel_context *ce, struct i915_request **out);

/** Room for `ndw` dwords of the request's commands in its ring, as
 * Linux's intel_ring_begin, or NULL when the ring cannot give it. */
uint32_t *i915_request_begin(struct i915_request *rq, uint32_t ndw);

/**
 * Close the request with its breadcrumb and hand it to the chip, as
 * Linux's i915_request_add: the engine's list takes a reference.
 *
 * @return 0, or -EIO when the ring refused the breadcrumb, when the
 *         request's commands are taken back and its fence signals with
 *         -EIO.
 */
int32_t i915_request_add(struct i915_request *rq);

/** Whether the chip has written the request's breadcrumb. */
int i915_request_completed(const struct i915_request *rq);

/**
 * Wait for the request for `timeout_ns`, for ever when it is negative, as
 * Linux's i915_request_wait; the breadcrumb is read again at each second
 * and at the timeout, in case an interrupt was lost.
 *
 * @return 0 once it completed, -ETIME at the timeout (at once for 0), or
 *         -EINTR for a signal when `intr` is set.
 */
int32_t i915_request_wait(struct i915_request *rq, int intr, int64_t timeout_ns);

/** Drop a reference to a request. */
void i915_request_put(struct i915_request *rq);

/** Retire every request whose breadcrumb the chip has written, oldest
 * first on each engine, and park the GT once every engine is idle, as
 * Linux's intel_gt_retire_requests. */
void i915_retire_requests(struct drm_i915_private *i915);

/** The device is unplugged: every request not completed fails with -EIO,
 * all retire, and the GT's own states, scratch page and default state go.
 * Before the rings stop. */
void i915_requests_unplug(struct drm_i915_private *i915);

#endif /* _I915_H_ */
