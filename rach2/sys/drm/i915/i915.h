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
#include "../../timer.h"
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

/* The engines the driver starts, by index into `engine`: the render,
 * blitter, video and video enhancement rings, in the order of Linux's
 * engine ids. */
#define I915_RCS0        0
#define I915_BCS0        1
#define I915_VCS0        2
#define I915_VECS0       3
#define I915_NUM_ENGINES 4

/* The bytes of a ring, Linux's legacy ring (SZ_16K). */
#define I915_RING_SIZE 16384u
/* The bytes between the tail and the head that stay free, so that a full
 * ring's tail never reaches its head (intel_ring.h, CACHELINE_BYTES). */
#define I915_RING_GAP  64u

/*
 * An engine and its ring (i915_ring.c). `mmio_base` is the base of its
 * registers, `hws_pga` the register of its status page's address, and
 * `irq_bit` its user interrupt, one bit in the bank `irq_bank` and in its
 * own RING_IMR: the GT bank's, but power management's for the video
 * enhancement ring, as Linux's hsw_irq_enable_vecs. `uabi_class` is its
 * class as programs name it (I915_ENGINE_CLASS_), `linux_id` its index in
 * Linux's engine ids (RCS0 0, BCS0 1, VCS0 10, VECS0 18), `reset_domain`
 * its bit of GEN6_GDRST and `fault_reg` its RING_FAULT_REG.
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
 *
 * Parking. `unretired` is set by each new request and cleared once Linux
 * would have retired every request of the engine, which on Haswell's ring
 * submission, one timeline an engine, is when the last request signals to
 * a listener or when the retire worker, once a second, finds it complete;
 * the GT parks only when no engine has it set (i915_request.c).
 * `building` is set from a request's creation to its publication on
 * `requests`, as Linux holds the timeline's mutex from i915_request_create
 * to i915_request_add: while it is set the engine's last request is not
 * the last of its timeline, and neither its signal nor the retire worker
 * clears `unretired`. Both change under `lock`.
 *
 * Hang detection and reset (i915_hangcheck.c, i915_reset.c). `hw_tail` is
 * what RING_TAIL was last given, the end of what the chip may run, which a
 * reset gives it again. `hc_armed` is set by a heartbeat that found the
 * engine busy, and `hc_seqno` is then the last request given to the chip
 * at that beat, which must complete before the next beat, as Linux's
 * heartbeat request must; `hc_acthd` and `hc_hws` are ACTHD and the
 * breadcrumb the last beat read.
 *
 * Submission. RING_TAIL moves past a request only once every fence it
 * waits for has signalled and every request before it has gone, as the
 * one timeline of a legacy ring in Linux orders them: `queued` is the
 * oldest request on the list not yet given to the chip, NULL when all
 * are. `kick` is set, under `lock`, when a request's last fence signals
 * outside the big kernel lock, and the submission worker then moves the
 * tail (i915_request.c).
 */
struct i915_request;
struct intel_context;
struct i915_gpu_error;

struct i915_engine {
    const char *name;
    uint32_t mmio_base;
    uint32_t hws_pga;
    uint32_t irq_bit;
    int irq_bank;
    uint16_t uabi_class;
    uint8_t linux_id;
    uint32_t reset_domain;
    uint32_t fault_reg;
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
    struct i915_request *queued;
    uint8_t kick;
    uint32_t hw_tail;
    uint8_t unretired;
    uint8_t building;
    uint8_t hc_armed;
    uint32_t hc_seqno;
    uint32_t hc_acthd;
    uint32_t hc_hws;
};

/* The aliasing PPGTT's shape (gt/intel_gtt.h): 512 directory entries, each
 * naming a table of 1024 entries of 4 KiB pages, 2 GiB in all; the
 * directory's entries lie in the global GTT's own, 2 MiB of its space
 * aligned to 64 KiB, as GEN6_PD_ALIGN. */
#define I915_PPGTT_PDES     512u
#define I915_PPGTT_PTES     1024u
#define I915_PPGTT_SIZE     ((uint64_t)I915_PPGTT_PDES * I915_PPGTT_PTES * I915_GTT_PAGE_SIZE)
#define I915_PPGTT_PD_BYTES (I915_PPGTT_PDES * I915_GTT_PAGE_SIZE)
#define I915_PPGTT_PD_ALIGN 0x10000u

/*
 * The aliasing PPGTT (i915_ppgtt.c), Linux's gen6_ppgtt as Haswell runs
 * it: one table of the GPU's addresses that every context shares, which
 * maps each buffer's binding at the binding's offset in the global GTT,
 * and nothing of the GT's own objects. Its directory is the global GTT's
 * entries at `pd_ggtt`, which the GT's space holds, `pd_take` bytes of it
 * with the alignment's pad. `pt[i]` is the table of directory entry i and
 * `pt_phys[i]` its address, made when a binding first reaches its 4 MiB
 * and kept until the PPGTT goes, as Linux keeps them while the directory
 * is bound, since the chip caches directory entries with no way to
 * invalidate them; `tables` counts them. An entry with no table names
 * `scratch_pt`, each of whose entries maps the global GTT's scratch page.
 * `pp_dir` is what each ring's PP_DIR_BASE takes. `ready` is set once the
 * directory is written. Under the lock of the bindings, as the GTT's.
 */
struct i915_ppgtt {
    uint32_t *pt[I915_PPGTT_PDES];
    uint64_t pt_phys[I915_PPGTT_PDES];
    uint32_t tables;
    uint32_t *scratch_pt;
    uint64_t scratch_pt_phys;
    uint32_t pd_ggtt;
    uint32_t pd_take;
    uint32_t pp_dir;
    uint8_t ready;
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
 * once they are made. `awake` is set from a request made with the GT
 * parked until the GT parks again, once no engine has a request or one
 * Linux would not yet have retired; `retire_next` is the tick of the next
 * pass of the retire worker while it is awake, as Linux's retire_work
 * runs once a second (i915_request.c). `wedged` is set when the GPU
 * cannot run work, and new requests and contexts fail with -EIO, as
 * Linux's wedged GT;
 * `wedged_terminal` says no reset may undo it, as when the default state
 * could not be recorded or the device is gone. `reset_count` counts the
 * whole GPU's resets, which GET_RESET_STATS gives, and
 * `reset_engine_count` the engine resets by the engine's class, as Linux's
 * gpu_error counts them. `hc_next` is the tick of the next heartbeat while
 * the GT is unparked (i915_hangcheck.c). `error` is the state captured at
 * the first hang, kept until it is cleared (i915_gpu_error.c).
 * `submit_next` links the devices whose requests may wait for fences, for
 * the submission worker, and `submit_listed` says the device is on that
 * list. `ppgtt` is the aliasing PPGTT, made with the rings.
 * `init_ticks` is the tick at which the GT was made, from which the
 * error state counts its uptime; `awake_ticks` counts the ticks the GT
 * was awake before its last park, and `unpark_ticks` is the tick of its
 * last unpark, as Linux's intel_gt_get_awake_time adds them.
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
    uint8_t wedged_terminal;
    uint32_t reset_count;
    uint32_t reset_engine_count[I915_NUM_ENGINES];
    uint32_t wa_lost; /* workarounds the chip did not keep, since the bind */
    uint64_t hc_next;
    uint64_t retire_next;
    struct i915_gpu_error *error;
    uint8_t submit_listed;
    struct drm_i915_private *submit_next;
    struct i915_ppgtt ppgtt;
    uint64_t init_ticks;
    uint64_t awake_ticks;
    uint64_t unpark_ticks;
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
 * interrupts drive it. `boost_queued` says a waitboost or a new boost
 * frequency asked for the work, Linux's queue_work of rps_work, under the
 * interrupt mask's lock with `pm_iir`. `lock` is the frequency lock of
 * i915.h's order, over the rest. `work_runs` counts the events the work
 * acted on.
 *
 * The waitboost, Linux's: `num_waiters` counts the requests boosted that
 * have yet to retire, each marked by its `boosted`, and while any is
 * there the work asks for `boost_freq` at least, up to `max_freq`;
 * `boosts` counts the boosts that found RPS active, as Linux's debug
 * count. `interactive` counts the page flips pending, Linux's
 * power.interactive, which hold the power mode at high power.
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
    int num_waiters;
    int interactive;
    uint32_t boosts;
    uint8_t enabled;
    uint8_t active;
    uint8_t interrupts;
    uint8_t boost_queued;
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
 * top of the space down, no eviction to make room, and, with eviction, no
 * wait for the GPU (Linux's PIN_NONBLOCK); a wait takes a signal.
 * `i915_vma_pin_args` adds a binding at the offset it names
 * (PIN_OFFSET_FIXED) or above it (PIN_OFFSET_BIAS). */
#define I915_PIN_MAPPABLE     0x1u
#define I915_PIN_HIGH         0x2u
#define I915_PIN_NOEVICT      0x4u
#define I915_PIN_NONBLOCK     0x8u
#define I915_PIN_OFFSET_FIXED 0x10u
#define I915_PIN_OFFSET_BIAS  0x20u

/*
 * A remapped view of an object, Linux's intel_remapped_plane_info for a
 * single colour plane: `height` rows of `width` pages each, row r the
 * `width` pages from page `offset` + r * `src_stride` of the object, laid
 * one after another, so that the view's stride is `width` pages, as
 * Linux's dst_stride on Haswell. A plane whose framebuffer's stride passes
 * its limit scans out only its source rectangle through one. A `width` of
 * 0 names no remapped view.
 */
struct i915_remap_info {
    uint32_t offset;
    uint32_t width;
    uint32_t height;
    uint32_t src_stride;
};

/* The page of the object that page `k` of the remapped view `r` maps. */
#define I915_REMAP_PAGE(r, k) \
    ((r)->offset + ((k) / (r)->width) * (r)->src_stride + (k) % (r)->width)

/*
 * A binding of an object, or of a part of it, into the global GTT, Linux's
 * i915_vma. The view is the whole object when `view_pages` is 0, or the
 * `view_pages` pages from `view_offset`, a partial view, which a GTT map
 * of an object too large for the aperture binds a piece at a time, or,
 * when `remap.width` is not 0, the remapped view `remap` of `view_pages`
 * pages, which a plane scans out and nothing maps or fences. It lies
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
    struct i915_remap_info remap;
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
 * first byte of the program's memory it stands for, which may be 0, `set`
 * marks the object as one of GEM_USERPTR, and `range`
 * is registered on that address space from the object's making, so that a
 * change to the pages there revokes the object's bindings and drops the
 * frames it pinned. `stale` is set when that happened while a binding was
 * pinned, whose frames go with its last pin, which undoes the binding.
 */
struct i915_userptr {
    uintptr_t ptr;
    struct mmn_range range;
    uint8_t set;
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
 * (SET_TILING), fixed while `fb_refs`, the framebuffers made of it, is
 * above 0, as Linux's frontbuffer fixes it. `vmas` are its bindings,
 * `mmo` its mmap offsets by kind, and `userfault_count` how many of its
 * bindings programs map through the aperture. `madv` is the I915_MADV_
 * state of its pages: WILLNEED, or DONTNEED when reclaim may purge them,
 * or __I915_MADV_PURGED once it did. `dumb` says it was made as a dumb
 * buffer, whose processor maps are write-combined. The device's list of
 * objects holds it (`base.dev_next`).
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
    uint32_t fb_refs;
    struct i915_vma *vmas;
    struct i915_mmap_offset mmo[I915_MMAP_TYPES];
    uint32_t userfault_count;
    uint8_t madv;
    uint8_t dumb;
    struct i915_userptr userptr;
};

/* Whether `obj` was made by GEM_USERPTR. */
#define i915_gem_object_is_userptr(obj) ((obj)->userptr.set != 0)

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
 * `src_h` are PIPESRC's size. `output` is the INTEL_OUTPUT_ the function
 * control's mode names, `has_hdmi_sink` says its mode is HDMI rather than
 * DVI, and `pixel_multiplier` is PIPE_MULT's, 1 for the eDP transcoder.
 * The panel fitter's three registers are kept
 * as read, `pf_ctl` 0 when it is off. The primary plane: `plane_on`,
 * `format` (DRM_FORMAT_, 0 for one Linux does not read), `modifier`,
 * `surf` its GGTT address, `stride` in bytes, and `x`, `y` its offset.
 */
struct intel_pipe_readout {
    uint8_t active;
    uint8_t dither;
    int trans;
    int port;
    int output;
    uint8_t has_hdmi_sink;
    uint32_t pixel_multiplier;
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
 * The panel power sequencer of the eDP panel (intel_pps.c), Linux's
 * intel_pps. The delays the sequencer is programmed with, the larger of
 * the firmware's registers and the VBT's, in 100 us units as written
 * (`power_up` to `power_cycle`), and the same in milliseconds for the
 * driver's own waits. The times, in milliseconds of rtc_ticks_now, of the
 * panel's last power off, its last power on and the backlight's last
 * off. `want_vdd` says a caller holds the panel's VDD forced on;
 * `vdd_off_due` is when VDD that no caller wants goes off, 0 when it is
 * not pending. `sleep` is the queue the driver sleeps on in its waits.
 */
struct intel_pps {
    uint32_t power_up, backlight_on, backlight_off, power_down, power_cycle;
    uint32_t power_up_ms, backlight_on_ms, backlight_off_ms, power_down_ms, power_cycle_ms;
    uint64_t power_off_time, last_power_on, last_backlight_off;
    uint64_t vdd_off_due;
    uint8_t want_vdd;
    uint8_t ready;
    wait_queue_t sleep;
};

/* The most fixed modes a panel has, its preferred one first. */
#define INTEL_PANEL_FIXED_MODES 4
/* The EDID blocks the eDP connector keeps. */
#define INTEL_EDID_BLOCKS       4

/*
 * A DisplayPort sink on a DDI port (intel_dp.c), Linux's intel_dp: the
 * eDP panel on DDI A, with its connector's panel, or a monitor on DDI B,
 * C or D. `i915` is the device, `name` the log's prefix ("edp", "dp B"),
 * `port` the DDI port, `aux_port` the AUX channel's port, which the VBT
 * may set apart from it, and `edp` says the sink is the panel, whose AUX
 * messages hold its VDD. `dpcd` is the sink's receiver capabilities and
 * `edp_dpcd` its eDP display control capabilities, 0 before eDP 1.x;
 * `sink_rates` the link rates in kHz of symbol clock it takes, highest
 * last, and `use_rate_select` says they came from eDP 1.4's table, set
 * by index through DP_LINK_RATE_SET. `max_lanes` the lanes it has. The
 * link now: `link_rate` (kHz), `lane_count`, `train_set` per lane as
 * DP_TRAINING_LANE0_SET has it, and `link_trained`. `rates` are the
 * `nrates` rates both the port and the sink take, lowest first. After
 * failed trainings (intel_dp_link_training.c), `use_max_params` says the
 * next mode set takes the sink's most instead of the least the mode
 * needs, `link_max_rate` and `link_max_lanes` bound the link, 0 for no
 * bound, and `train_failures` counts the failures in a row. `buf_ctl` is
 * the value DDI_BUF_CTL is built in, and `saved_port_bits` the bits of the
 * firmware's kept across mode sets. `edid` holds `edid_len` bytes, 0 when
 * the sink gave none, and `edid_bpc` is the bits a colour it names, 0 for
 * none. Of the panel:
 * `fixed` its modes, preferred first, `nfixed` of them, `vbt` what the
 * VBT says of it, and `pps` its power sequencer. `present` says the
 * panel answered at init, which makes its connector. Of a monitor:
 * `connector` its connector; `downstream_ports` the DPCD's ports of a
 * branch device and `sink_count` its sinks; `info` what its EDID says of
 * it; the `dfp_` limits of a branch's first port, Linux's intel_dp->dfp,
 * 0 for none: bits a colour, dot clock and TMDS clocks in kHz;
 * `reset_link` says a long pulse asks the next probe to forget the
 * fallback, and `retrain` that a short pulse found the link lost.
 */
struct drm_i915_private;
struct intel_dp {
    struct drm_i915_private *i915;
    const char *name;
    int port;
    int aux_port;
    uint8_t edp;
    uint8_t present;
    uint8_t reset_link;
    uint8_t retrain;
    uint8_t dpcd[16];
    uint8_t edp_dpcd[3];
    uint32_t sink_rates[8];
    int nsink_rates;
    uint8_t use_rate_select;
    uint32_t max_lanes;
    uint32_t link_rate;
    uint32_t lane_count;
    uint8_t train_set[4];
    uint8_t link_trained;
    uint32_t rates[8];
    int nrates;
    uint8_t use_max_params;
    uint32_t link_max_rate, link_max_lanes;
    uint32_t train_failures;
    uint32_t buf_ctl;
    uint32_t saved_port_bits;
    uint8_t edid[128 * INTEL_EDID_BLOCKS];
    uint32_t edid_len;
    struct drm_mode_modeinfo fixed[INTEL_PANEL_FIXED_MODES];
    int nfixed;
    struct intel_vbt_panel vbt;
    uint32_t edid_bpc;
    struct intel_pps pps;
    struct drm_connector *connector;
    uint8_t downstream_ports[16];
    uint8_t sink_count;
    struct drm_edid_hdmi_info info;
    uint32_t dfp_max_bpc, dfp_max_dotclock, dfp_min_tmds, dfp_max_tmds;
};

/* ---- The external outputs, DDI B to D ---- */

/* What a pipe drives, Linux's INTEL_OUTPUT_ types of a DDI port. */
#define INTEL_OUTPUT_NONE 0
#define INTEL_OUTPUT_EDP  1
#define INTEL_OUTPUT_DP   2
#define INTEL_OUTPUT_HDMI 3

/*
 * An I2C message, Linux's struct i2c_msg without its flags but one:
 * `len` bytes of `buf` to or from the device at the 7-bit `addr`.
 */
struct intel_i2c_msg {
    uint8_t addr;
    uint8_t read;
    uint32_t len;
    uint8_t *buf;
};

/*
 * An I2C bus, Linux's i2c_adapter as the EDID's read uses it: `xfer` runs
 * the `n` messages as one transaction, a repeated start between them and
 * a stop at the end, and returns `n` or a negative error: -ENXIO when no
 * device answered its address, -EIO for a NAK of data, -ETIMEDOUT and
 * others for the bus. `ctx` is its own; `name` names it in the log.
 */
struct intel_i2c_bus {
    int32_t (*xfer)(void *ctx, struct intel_i2c_msg *msgs, int n);
    void *ctx;
    const char *name;
};

/* The GMBUS pins of a Lynx Point PCH, Linux's GMBUS_PIN_ (the VBT's DDC
 * pins on Haswell), and the most a pin's number may be. */
#define GMBUS_PIN_DISABLED 0
#define GMBUS_PIN_SSC      1
#define GMBUS_PIN_VGADDC   2
#define GMBUS_PIN_PANEL    3
#define GMBUS_PIN_DPC      4
#define GMBUS_PIN_DPB      5
#define GMBUS_PIN_DPD      6
#define GMBUS_NUM_PINS     7

/*
 * The I2C bus of one GMBUS pin (intel_gmbus.c), Linux's intel_gmbus.
 * `reg0` is GMBUS0's value for it, the pin and the rate; `gpio_reg` the
 * GPIO register its lines are bit-banged through. `force_bit` counts the
 * callers that want bit-banging, as Linux's intel_gmbus_force_bit, and
 * holds GMBUS_FORCE_BIT_RETRY besides when a GMBUS transfer timed out and
 * the bus fell back to bit-banging, which it keeps while the bit-banged
 * transfers succeed; the bus bit-bangs while it is not 0. The counts:
 * transfers by GMBUS and by bit-banging, NAKs and timeouts. `bus` is its
 * face as an I2C bus.
 */
struct intel_gmbus {
    struct drm_i915_private *i915;
    const char *name;
    uint8_t pin;
    uint8_t valid;
    uint32_t force_bit;
    uint32_t reg0;
    uint32_t gpio_reg;
    uint32_t xfers, bit_xfers, naks, timeouts;
    struct intel_i2c_bus bus;
};

/* Linux's GMBUS_FORCE_BIT_RETRY. */
#define GMBUS_FORCE_BIT_RETRY (1u << 31)

/* The display PLLs of Haswell, Linux's enum intel_dpll_id. */
#define DPLL_ID_NONE       (-1)
#define DPLL_ID_WRPLL1     0
#define DPLL_ID_WRPLL2     1
#define DPLL_ID_SPLL       2
#define DPLL_ID_LCPLL_810  3
#define DPLL_ID_LCPLL_1350 4
#define DPLL_ID_LCPLL_2700 5
#define INTEL_NUM_DPLLS    6

/*
 * A display PLL (intel_dpll.c), Linux's intel_shared_dpll for Haswell:
 * `hw` is the WRPLL_CTL or SPLL_CTL value it runs or last ran, `on`
 * whether it runs, `pipes` the pipes whose configuration names it, a bit
 * each, and `active` the pipes that have it enabled; a WRPLL or the SPLL
 * runs while `active` is not 0. The LCPLL outputs are always on and only
 * counted.
 */
struct intel_dpll {
    const char *name;
    int id;
    uint32_t hw;
    uint8_t on;
    uint32_t pipes;
    uint32_t active;
};

/*
 * What the PLLs will run once a commit's pipes are set, Linux's shared
 * DPLL state of an atomic state: the pipes that will name each PLL, a bit
 * each, and the value each will run.
 */
struct intel_dpll_plan {
    uint32_t pipes[INTEL_NUM_DPLLS];
    uint32_t hw[INTEL_NUM_DPLLS];
};

/* The longest infoframe a DIP's data registers hold, Linux's
 * VIDEO_DIP_DATA_SIZE, with the hole after the header Linux packs. */
#define INTEL_DIP_DATA_SIZE 32

/* The DP++ adaptors, Linux's enum drm_dp_dual_mode_type, in its order. */
#define DP_DUAL_MODE_NONE       0
#define DP_DUAL_MODE_UNKNOWN    1
#define DP_DUAL_MODE_TYPE1_DVI  2
#define DP_DUAL_MODE_TYPE1_HDMI 3
#define DP_DUAL_MODE_TYPE2_DVI  4
#define DP_DUAL_MODE_TYPE2_HDMI 5
#define DP_DUAL_MODE_LSPCON     6

/*
 * An HDMI monitor on a DDI port (intel_hdmi.c), Linux's intel_hdmi with
 * its connector's EDID. `ddc_pin` is the GMBUS pin of its DDC, `level`
 * the buffer translation the VBT gives, `max_tmds` the most TMDS clock in
 * kHz the port drives, the VBT's or Haswell's 300 MHz, and `dual_mode`
 * the VBT's word that the port is DP++. From the last probe: `edid` and
 * `edid_len`, `info` what the EDID, or the connector's override, says of
 * the sink, and the DP++ adaptor found, `adaptor` (DP_DUAL_MODE_*) and
 * its most TMDS clock in kHz, `adaptor_max_tmds`, 0 for no limit.
 */
struct intel_hdmi {
    uint8_t ddc_pin;
    int level;
    uint32_t max_tmds;
    uint8_t dual_mode;
    uint8_t edid[128 * INTEL_EDID_BLOCKS];
    uint32_t edid_len;
    struct drm_edid_hdmi_info info;
    int adaptor;
    uint32_t adaptor_max_tmds;
};

/* The hotplug pins of the PCH's ports, Linux's HPD_PORT_B to D. */
#define HPD_PORT_B   0
#define HPD_PORT_C   1
#define HPD_PORT_D   2
#define HPD_NUM_PINS 3

/*
 * DDI B, C or D with its outputs (intel_display.c), Linux's
 * intel_digital_port: the one encoder, a DisplayPort connector and an HDMI
 * connector as the VBT says the port carries them, `has_dp` and
 * `has_hdmi`, and their sinks. `hpd_pin` is its HPD_PORT_ pin. `present`
 * says the port was made.
 */
struct intel_ddi_port {
    int port;
    uint8_t present;
    uint8_t has_dp, has_hdmi;
    int hpd_pin;
    struct drm_encoder encoder;
    struct drm_connector dp_connector, hdmi_connector;
    struct intel_dp dp;
    struct intel_hdmi hdmi;
};

/* An HPD pin's state, Linux's enum hpd_pin_state. */
#define HPD_ENABLED       0
#define HPD_DISABLED      1
#define HPD_MARK_DISABLED 2

/* Linux's bounds of a storm: more than 50 counts within 1000 ms, a long
 * pulse counting 10 and a short one none on a part with DP MST, turns the
 * pin off for two minutes, while its connectors are polled every 10 s; an
 * output that did not change is probed once more 1 s later. */
#define HPD_STORM_THRESHOLD   50
#define HPD_STORM_DETECT_MS   1000u
#define HPD_STORM_REENABLE_MS (2u * 60u * 1000u)
#define HPD_POLL_MS           10000u
#define HPD_RETRY_MS          1000u

/*
 * Hotplug (intel_hotplug.c), Linux's intel_hotplug for a Lynx Point PCH,
 * the pins a bit each by HPD_PORT_. Under the interrupt mask's lock: the
 * pulses the handler took on a port with DisplayPort, `long_mask` and
 * `short_mask`, for its pulse handler, and the pins whose outputs are to be
 * probed, `event_bits`; and each pin's state, `state` HPD_*, with its storm
 * window's start `storm_start` (ms of rtc_ticks_now) and count
 * `storm_count`. The work's own: `retry_bits`, the pins probed again at
 * `retry_at`, and `retries`, how many times in a row each was; when the
 * storm's pins come back, `reenable_at`, and when they are next polled,
 * `poll_at`, all ms, 0 for none. `work` is the DRM core's deferred work
 * that runs it in process context. `pulses` counts the interrupts taken
 * and `storms` the storms; `ready` says the interrupts are hooked.
 */
struct intel_hotplug {
    uint32_t long_mask, short_mask, event_bits;
    uint8_t state[HPD_NUM_PINS];
    uint64_t storm_start[HPD_NUM_PINS];
    uint32_t storm_count[HPD_NUM_PINS];
    uint32_t retry_bits;
    uint8_t retries[HPD_NUM_PINS];
    uint64_t retry_at, reenable_at, poll_at;
    uint32_t pulses, storms;
    struct drm_work work;
    uint8_t ready;
};

/*
 * What a pipe is set to, Linux's part of intel_crtc_state that the
 * outputs' compute_config and the fitter set:
 * `active`; the output, `output` INTEL_OUTPUT_, on DDI `port` through
 * transcoder `trans`, the eDP transcoder for eDP and the pipe's own for
 * the rest; the timings in `mode` with the pixel clock in kHz, the pipe's
 * source size `src_w` by `src_h`, the user's mode's, `bpp` and `dither`,
 * the link's `port_clock` (kHz of symbol clock, for HDMI the TMDS clock
 * the PLL gives) and `lanes`, the transcoder's M/N and transfer unit, the
 * fitter (`pfit`, its window `pf_pos` and `pf_size` as PF_WIN_POS and
 * PF_WIN_SZ take them, and `force_thru`, the eDP input of pipe A that goes
 * through the fitter's power well), the pixel rate after the fitter, and
 * the line times for WM_LINETIME. The PLL: `pll`, DPLL_ID_, and the value
 * `pll_hw` it runs with. HDMI: `has_hdmi_sink`, `limited_range` (RGB
 * 16-235, which the pipe's colour matrix makes), the infoframes enabled in
 * `infoframes` (VIDEO_DIP_ENABLE_*_HSW) and their packed bytes, `avi`,
 * `spd` and `vs` with `vs_len`, and GCP's value, `gcp`; and
 * `pixel_multiplier`, 2 for a DBLCLK mode, 1 otherwise. `well` says the
 * pipe holds a reference to the display power well.
 */
struct intel_crtc_config {
    uint8_t active;
    uint8_t dither;
    uint8_t pfit;
    uint8_t force_thru;
    int output;
    int port;
    int trans;
    struct drm_mode_modeinfo mode;
    uint32_t src_w, src_h;
    uint32_t bpp;
    uint32_t port_clock;
    uint32_t lanes;
    uint32_t data_m, data_n, link_m, link_n, tu;
    uint32_t pf_pos, pf_size;
    uint32_t pixel_rate;
    uint32_t linetime, ips_linetime;
    int pll;
    uint32_t pll_hw;
    uint8_t has_hdmi_sink;
    uint8_t limited_range;
    uint32_t infoframes;
    uint8_t avi[INTEL_DIP_DATA_SIZE];
    uint8_t spd[INTEL_DIP_DATA_SIZE];
    uint8_t vs[INTEL_DIP_DATA_SIZE];
    uint32_t avi_len, spd_len, vs_len;
    uint32_t gcp;
    uint32_t pixel_multiplier;
    uint8_t well;
};

/*
 * A level of a pipe's watermarks (intel_wm.c), Linux's intel_wm_level:
 * the FIFO cachelines of each plane and the frame buffer compressor's
 * lines, and whether the level may be used.
 */
struct intel_wm_level {
    uint8_t enable;
    uint32_t pri, spr, cur, fbc;
};

/* A pipe's watermarks at the five levels, and what the pipe shows that
 * they depend on (Linux's intel_pipe_wm). */
#define INTEL_WM_LEVELS 5
struct intel_pipe_wm {
    struct intel_wm_level wm[INTEL_WM_LEVELS];
    uint8_t pipe_enabled, sprites_enabled, sprites_scaled, fbc_wm_enabled;
};

/* The watermark registers' values as last written (Linux's
 * ilk_wm_values). */
struct intel_wm_values {
    uint32_t wm_pipe[3];
    uint32_t wm_lp[3];
    uint32_t wm_lp_spr[3];
    uint8_t enable_fbc_wm;
    uint8_t partition_5_6;
};

/*
 * A plane's framebuffer as the hardware scans it out: held, with the
 * binding `vma` pinned for scanout, NULL for the firmware's, which lies
 * where the firmware put it. A plane keeps two, what it shows and what it
 * showed before, which the hardware may read until the flip that replaced
 * it latches; that one is let go at the plane's next update, which comes
 * only after the flip (drm_atomic_helper_commit's stall), or, for a legacy
 * cursor update, which has no stall, once a vblank has passed.
 */
struct intel_scanout {
    struct drm_framebuffer *fb;
    struct i915_vma *vma;
};

/** A request a pipe boosts at its next vblank, Linux's wait_rps_boost:
 * `rq` holds a fence reference, and the entry a vblank reference. */
struct intel_boost {
    struct intel_boost *next;
    struct i915_request *rq;
};

/*
 * A pipe as a CRTC, with its three planes. `hw` is the readout of the
 * bind. `event` is the commit's event the pipe holds until the frame
 * counter passes `event_frame`, the count read after the commit's writes,
 * or until the flip done of an async flip (`event_async`); NULL for none.
 * `cfg` is what the pipe is set to, from the readout until its first mode
 * set. `crc_wa` says a CRC source runs that needs Linux's force-through
 * of the fitter. The watermarks (intel_wm.c): `wm_active` those the
 * registers hold for the pipe, `wm_optimal` those of the last commit's
 * planes, made active once the frame counter passes `wm_frame` while
 * `wm_pending`, which holds a vblank reference.
 * `vbl_on` says its vblank interrupt is unmasked. The CRC: `crc_ctl` is
 * PIPE_CRC_CTL's value while a source runs, 0 when none does,
 * `crc_skip` the frames still to drop after it starts, and `crc_res`
 * PIPE_CRC_RES_HSW as the error interrupt read it at the pipe's CRC done,
 * which latches a frame's CRC after its vblank, under the interrupt
 * mask's lock. `shown` and
 * `parked` are each plane's scanouts (`struct intel_scanout`), and
 * `shown_from` and `shown_to` the frame counts read before and after the
 * commit that wrote `shown`, by which a commit without a stall, a legacy
 * cursor update, tells whether the chip may still read `parked`.
 * `planes_on` has bit INTEL_PLANE_ for each plane the hardware shows.
 * `underrun_off` says its FIFO underruns are not reported, Linux's
 * cpu_fifo_underrun_disabled, and `inherited` that it shows what the
 * firmware lit, with no commit of its own yet. `boosts` lists, under the
 * interrupts' masking, the requests drawing flips that the next vblank
 * boosts if the chip has not begun them, one entry for each call of
 * `intel_crtc_boost_after_vblank`, Linux's wait_rps_boost;
 * `rps_interactive` says the pipe holds a commit's interactive mark of
 * RPS (`i915_rps_mark_interactive`) until its pending flip ends.
 */
struct intel_crtc {
    struct drm_crtc base;
    struct drm_plane primary, sprite, cursor;
    struct drm_i915_private *i915;
    int pipe;
    struct intel_pipe_readout hw;
    struct intel_crtc_config cfg;
    uint8_t crc_wa;
    struct intel_pipe_wm wm_active, wm_optimal;
    uint8_t wm_pending;
    uint32_t wm_frame;
    struct drm_pending_event *event;
    uint32_t event_frame;
    uint8_t event_async;
    uint8_t vbl_on;
    uint32_t crc_ctl;
    uint32_t crc_skip;
    uint32_t crc_res;
    uint32_t planes_on;
    uint8_t underrun_off;
    uint8_t inherited;
    struct intel_scanout shown[INTEL_NUM_PLANES];
    struct intel_scanout parked[INTEL_NUM_PLANES];
    uint32_t shown_from[INTEL_NUM_PLANES], shown_to[INTEL_NUM_PLANES];
    struct intel_boost *boosts;
    uint8_t rps_interactive;
};

/*
 * The device's display. `crtc` are the pipes A to C, `encoder` DDI A and
 * `connector` its eDP panel, `dp` the panel, and `ddi` DDI B to D, by port less
 * one. `gmbus` are the PCH's GMBUS pins, by pin, and `dpll` the PLLs, by
 * DPLL_ID_, with `dpll_nssc` the non-SSC reference in kHz of a non-ULT part, 24
 * or 135 MHz as FUSE_STRAP3 says. `hotplug` is the ports' hotplug. `pps` is the
 * sequencer's readout. `cdclk` is the CD clock in kHz and `max_cdclk` the most
 * it may be, both read at init (intel_cdclk.c). `wm_latency` are the memory
 * latencies of the watermark levels, level 0 in 0.1 us and the rest in 0.5 us,
 * and `wm_hw` the watermark registers as written. `fw_taken` says the
 * firmware's framebuffer was made a framebuffer of the device, whose object
 * lies at `fw_ggtt` in the GGTT. `de_iir` holds the display engine's events the
 * interrupt latched and the deferred work has yet to take, `underruns` a bit
 * for each pipe whose FIFO underrun it has yet to log, and `crc_done` a bit for
 * each pipe whose CRC (its `crc_res`) it has yet to enter, all under the
 * interrupt mask's lock. `bl` is the panel's PWM, `bl_index` its backlight's
 * index or -1, and `bl_want_on` what the backlight class last asked of its
 * switch, which holds only while the sequencer's backlight enable is on with
 * the panel's pipe (`bl_pipe_on`). `bl_enabled` says the PWM is on, as the
 * pipe's enable leaves it, `bl_level` is the PWM's level it shows then,
 * `bl_min` the least the VBT allows, `bl_dark` says the class asked for its
 * level 0, which Linux shows by turning the backlight off, and `bl_alt` says
 * the PCH counts the PWM in its coarse steps (LPT_PWM_GRANULARITY). `vga_off`
 * says the VGA plane has been turned off. `ready` is set once the device's mode
 * configuration is made, which its release frees.
 */
struct intel_display {
    struct intel_crtc crtc[I915_NUM_PIPES];
    struct drm_encoder encoder;
    struct drm_connector connector;
    struct intel_dp dp;
    struct intel_ddi_port ddi[3];
    struct intel_gmbus gmbus[GMBUS_NUM_PINS];
    struct intel_dpll dpll[INTEL_NUM_DPLLS];
    uint32_t dpll_nssc;
    struct intel_hotplug hotplug;
    struct intel_pps_readout pps;
    uint32_t cdclk, max_cdclk;
    uint16_t wm_latency[INTEL_WM_LEVELS];
    struct intel_wm_values wm_hw;
    uint32_t fw_ggtt;
    uint8_t fw_taken;
    uint32_t de_iir;
    uint32_t underruns;
    uint32_t crc_done;
    struct intelbl bl;
    int bl_index;
    uint8_t bl_want_on;
    uint8_t bl_pipe_on;
    uint8_t bl_enabled;
    uint8_t bl_alt;
    uint8_t bl_dark;
    int bl_level, bl_min;
    uint8_t vga_off;
    uint8_t ready;
};

#define to_intel_crtc(c) ((struct intel_crtc *)(c))

/*
 * The module parameters that debugfs's i915_params lets root change and
 * the driver obeys, kept so that a structure of zeros holds Linux's
 * defaults. `reset` is Linux's i915.reset, which counts once `reset_set`
 * is set and is 3 until then: 0 for no reset, so that a hang wedges the
 * GPU; 1 for the whole GPU's reset alone; 2 and above for an engine's
 * reset first. `hangcheck_off` is i915.enable_hangcheck written false: no
 * heartbeat finds a hang, and a new context is not persistent.
 */
struct i915_params {
    uint32_t reset;
    uint8_t reset_set;
    uint8_t hangcheck_off;
};

/* The i915.reset parameter in force. */
#define I915_RESET_PARAM(i915) ((i915)->params.reset_set ? (i915)->params.reset : 3u)

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
 * is the display's state (intel_display.c), and `params` the parameters
 * root may change (`struct i915_params`). `attached`
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
    struct i915_params params;
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
 * Bind one function the id table names, through the MMIO backend: function
 * 0 only, as Linux, since early chips had a second function with the same
 * id; BAR 0 must be set. Its registers are mapped here, and given back on
 * an error.
 *
 * @return 0, -ENODEV for another function, -ENXIO without BAR 0, -ENOMEM
 *         when the registers cannot be mapped, or the bind's error.
 */
int32_t i915_pci_probe(const pci_device_t *pdev);

/**
 * Give back what the MMIO backend mapped for `hw`, the registers and the
 * GTT's entries, and cut `hw` off from the device (`i915_hw_gone`): for a
 * bind that failed, a device released to another owner, and a record's
 * end. Another backend is only cut off. Calling it again does nothing.
 */
void i915_pci_unmap(struct i915_hw *hw);

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

/**
 * Bind the fake chip to `pdev`, an `edu` function, as the probe does for
 * each: for the bring-up of one the driver released (i915_handoff.c).
 *
 * @return 0, -ENODEV for a function that is no `edu` device, -ENOMEM, or
 *         the bind's error.
 */
int32_t i915_fake_bind(const pci_device_t *pdev);

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
 * Set up and start the render, blitter, video and video enhancement
 * rings, as Linux's xcs_resume: for each a status page and a ring in the
 * global GTT; the aliasing PPGTT made (`i915_ppgtt_init`); then each ring
 * stopped and emptied, the status page's address written and the TLB
 * flushed, the PPGTT's registers (`i915_ppgtt_enable_ring`), and the ring
 * started with RING_VALID, each wait bounded; then its user interrupt
 * registered in its bank, masked until a waiter needs it. Holds forcewake
 * throughout.
 *
 * @return 0; -ENOMEM; -ENOSPC when the GTT has no room at its top; -EIO
 *         when a ring would not stop or start, its wait named in
 *         `hw.last_timeout`. On an error every ring is torn down.
 */
int32_t i915_gt_init(struct drm_i915_private *i915);

/**
 * Write the GT's state that a reset of the whole GPU loses, as Linux's
 * intel_gt_init_hw does on Haswell before the rings start, at the bind and
 * after such a reset: the hash mask of a part with eDRAM, the predicate's
 * second result for the slices the part has, and the GT's workarounds
 * (hsw_gt_workarounds_init), each read back as Linux verifies it. The
 * swizzle and the PPGTT's caching are `i915_gem_restore_fences`'s and
 * `i915_ppgtt_enable_ring`'s. Under forcewake.
 */
void i915_gt_init_hw(struct drm_i915_private *i915);

/** Stop every ring, unregister its interrupt, clear its GTT entries and
 * free its pages, then the PPGTT; for the unplug and a failed bind. */
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

/**
 * Start `engine`'s ring again with the chip to run from `head` to `tail`,
 * as Linux's intel_engine_resume and xcs_resume after a reset: the render
 * engine's workarounds written (rcs_engine_wa_init) and read back, the
 * ring stopped and emptied, the status page's address written and the TLB
 * flushed, the ring's start and the PPGTT's registers, enabled empty at
 * `head` with RING_VALID waited for, STOP_RING cleared, and then `tail`
 * given. Every interrupt of the ring is masked in its RING_IMR. Under
 * forcewake.
 *
 * @return 0, or -EIO when the ring would not stop or start, its registers
 *         logged.
 */
int32_t i915_ring_resume(struct drm_i915_private *i915, struct i915_engine *engine, uint32_t head,
                         uint32_t tail);

/**
 * Stop `engine`'s command streamer and empty its ring before a reset, as
 * Linux's reset_prepare for ring submission: STOP_RING and a wait of
 * 100 ms for MODE_IDLE, which a hung ring never reaches and which is not
 * waited for again, then the head moved to the tail, the ring disabled
 * and the head and tail cleared, twice if the head will not clear. Under
 * forcewake.
 */
void i915_ring_reset_prepare(struct drm_i915_private *i915, struct i915_engine *engine);

/* ---- i915_ppgtt.c ---- */

/**
 * Make the aliasing PPGTT, as Linux's gen6_ppgtt_create and pd_vma_bind:
 * the scratch table, the directory's space taken from the GT's at a 64 KiB
 * boundary, and every directory entry written to name the scratch table.
 * `i915_gt_init` calls it before the rings start, which need it.
 *
 * @return 0; -ENOMEM; or -ENOSPC when the GT's space has no room.
 */
int32_t i915_ppgtt_init(struct drm_i915_private *i915);

/** Undo `i915_ppgtt_init`, any part of it done: the directory's entries
 * on the scratch page again and every table freed. For `i915_gt_fini`,
 * once the rings have stopped. */
void i915_ppgtt_fini(struct drm_i915_private *i915);

/**
 * Map the `n` pages `pages` at the GPU address `offset` at the cache level
 * `cache`, as Linux's gen6_alloc_va_range and gen6_ppgtt_insert_entries:
 * the tables the range lacks made first, each filled with the scratch
 * page's entry and named in the directory, whose entries are then read
 * back and the GTT's cache flushed. The entries are the global GTT's
 * encoding; the request's TLB invalidation makes the chip see them. The
 * binding code calls it beside `i915_ggtt_insert`, at the same offset,
 * for the buffers alone. Nothing to do before the PPGTT is made.
 *
 * @return 0; -EINVAL for a range that is not whole pages inside 2 GiB; or
 *         -ENOMEM when a table cannot be made, no entry written.
 */
int32_t i915_ppgtt_insert(struct drm_i915_private *i915, uint64_t offset, const uint64_t *pages,
                          uint32_t n, int cache);

/** Point the PPGTT's entries of [offset, offset + size) at the scratch
 * page again, as gen6_ppgtt_clear_range; the tables stay. Beside
 * `i915_ggtt_clear_range` for a buffer's binding. */
void i915_ppgtt_clear_range(struct drm_i915_private *i915, uint64_t offset, uint64_t size);

/**
 * Point `engine`'s ring at the aliasing PPGTT, at ring start and after a
 * reset, as Linux's set_pp_dir: PP_DIR_DCLV with the whole 2 GiB valid,
 * PP_DIR_BASE with the directory, and RING_MODE_GEN7's PPGTT bit, the
 * render ring's being GFX_MODE_GEN7. For the render ring it first sets
 * what Linux's gen7_ppgtt_enable sets at each hardware init, the PPGTT's
 * caching in GAC_ECO_BITS and GAM_ECOCHK, so that every start, a reset's
 * among them, leaves them set. Under forcewake. Writes nothing before the
 * PPGTT is made.
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
 * Act on the up and down interrupts the handler latched and on a waitboost
 * queued, as Linux's rps_work: with waiters, to `boost_freq` at least and
 * held there, up to `max_freq`; otherwise step the frequency up or down,
 * or to RPe or RPn on the down timeout, within the softlimits; then
 * unmask the events again. Runs as the interrupt's `deferred` work, with
 * no lock of the driver held, and may be called from anywhere else; it
 * takes the frequency lock.
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

/** Set the boost frequency, gt_boost_freq_mhz, `val` in MHz, and run the
 * work again for the waiters while it changes, as Linux's
 * rps_set_boost_freq: 0, or -EINVAL outside [min_freq, max_freq]. */
int32_t i915_rps_set_boost_mhz(struct drm_i915_private *i915, uint32_t val);

/**
 * Boost the GT's clock for a client waiting on `rq`, as Linux's
 * intel_rps_boost: nothing for a request signalled or boosted already;
 * otherwise `rq` is marked and counted a waiter until it retires, and the
 * first waiter, while RPS is active and asks for less than `boost_freq`,
 * runs the work, which asks for `boost_freq`. The callers boost only a
 * request the chip has not begun (`i915_request_started`). Any context
 * where the work may run: no lock of the driver held.
 */
void i915_rps_boost(struct i915_request *rq);

/** `rq` retires: its waitboost, if any, is no longer counted, and none
 * asked after is, as Linux's i915_request_retire. Any context. */
void i915_rps_retire(struct i915_request *rq);

/**
 * Count a page flip pending, with `on`, or one done, as Linux's
 * intel_rps_mark_interactive: while any is pending the power mode is high
 * power, with its shorter evaluation interval, and the first switches to
 * it at once while RPS is active. Any context; takes the frequency lock.
 */
void i915_rps_mark_interactive(struct drm_i915_private *i915, int on);

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
#define I915_NUM_IOCTLS 28
extern const struct drm_ioctl_desc i915_ioctls[I915_NUM_IOCTLS];

/**
 * Hold the object's pages where the GPU reads them, as Linux's
 * i915_gem_object_pin_pages: a shmem object's pages are all given and
 * pinned at the first pin, memory short of them reclaimed first and once
 * more after a failure; an import's are its dma-buf's; a userptr object's
 * are the program's (`i915_gem_userptr_get_pages`). The physical addresses
 * stay in `obj->frames` until the object goes or is purged. An object
 * without pages whose `madv` is not WILLNEED has none to give. Reclaim and
 * the userptr's pin may sleep, so both look again after it: a caller that
 * finds the pages given meanwhile counts one more pin on them.
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

/**
 * Wait until no fence of the object's reservation is busy, so that the GPU
 * no longer reads or writes its pages, for ever, as Linux's i915_vma_sync;
 * with `intr` a signal ends the wait. Allocates nothing.
 *
 * @return 0, or -EINTR with `intr`.
 */
int32_t i915_gem_object_wait_idle(struct drm_i915_gem_object *obj, int intr);

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
 * i915_gem_object_wait, after a waitboost for each of their requests the
 * chip has not begun, timeout 0 too.
 *
 * @return 0, -ETIME when one is busy at the timeout (at once for 0),
 *         -EINTR for a signal, or -ENOMEM.
 */
int32_t i915_gem_object_wait(struct drm_i915_gem_object *obj, int all, int64_t timeout_ns);

/** Whether a write by the processor must be flushed from its cache for
 * the GPU to see it, as Linux's i915_gem_cpu_write_needs_clflush. */
int i915_gem_cpu_write_needs_clflush(const struct drm_i915_gem_object *obj);

/* Whether the kernel flushes a range before or after it touches the pages
 * through the processor's cache (Linux's CLFLUSH_BEFORE and _AFTER). */
#define I915_CLFLUSH_BEFORE 0x1u
#define I915_CLFLUSH_AFTER  0x2u

/**
 * Get the object's pages ready for the processor to read through its
 * cache, as Linux's i915_gem_object_prepare_read: its writers waited for,
 * its pages pinned, which the caller drops, its write domain flushed, and
 * in `*needs_clflush` I915_CLFLUSH_BEFORE when the lines must be flushed
 * before they are read.
 *
 * @return 0, -ENODEV for an import, or the wait's and the pin's errors.
 */
int32_t i915_gem_object_prepare_read(struct drm_i915_gem_object *obj, uint32_t *needs_clflush);

/**
 * Get the object's pages ready for the processor to write through its
 * cache, as Linux's i915_gem_object_prepare_write: its work waited for, its
 * pages pinned, which the caller drops, its write domain flushed, and in
 * `*needs_clflush` the I915_CLFLUSH_ bits the write needs.
 *
 * @return 0, -ENODEV for an import, or the wait's and the pin's errors.
 */
int32_t i915_gem_object_prepare_write(struct drm_i915_gem_object *obj, uint32_t *needs_clflush);

/** Make what the processor wrote to the object reach memory before the GPU
 * reads it, as Linux's execbuffer does for each object: the write domain
 * of the CPU, the aperture or write-combining flushed, and a cache the
 * GPU does not snoop flushed. */
void i915_gem_object_flush_for_gpu(struct drm_i915_gem_object *obj);

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
int32_t i915_gem_create_ext_ioctl(struct drm_device *dev, void *data, struct drm_file *file);
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
int32_t i915_gem_mmap_ioctl(struct drm_device *dev, void *data, struct drm_file *file);
int32_t i915_gem_get_aperture_ioctl(struct drm_device *dev, void *data, struct drm_file *file);
int32_t i915_gem_madvise_ioctl(struct drm_device *dev, void *data, struct drm_file *file);
int32_t i915_gem_userptr_ioctl(struct drm_device *dev, void *data, struct drm_file *file);

/** Revoke every program's mapping of every object, once the device is
 * unplugged, so that the next access faults to a device that refuses it. */
void i915_gem_unplug(struct drm_i915_private *i915);

/**
 * GEM_EXECBUFFER2 and its _WR form, as Linux 6.12's
 * i915_gem_execbuffer2_ioctl on a legacy ring (i915_gem_execbuffer.c): the
 * objects bound and relocated, the batch started on the engine the flags
 * or the context's map name, its fences awaited and signalled.
 */
int32_t i915_gem_execbuffer2_ioctl(struct drm_device *dev, void *data, struct drm_file *file);

/* The command parser's version, which GETPARAM gives, Linux 6.12's; and
 * the bytes past a batch's copy that its shadow keeps for the trampoline,
 * Linux's I915_CMD_PARSER_TRAMPOLINE_SIZE. */
#define I915_CMD_PARSER_VERSION         10
#define I915_CMD_PARSER_TRAMPOLINE_SIZE 8u

/*
 * A batch to parse and the shadow it is copied to. `batch` and `shadow`
 * are the frames of each object's pages; `offset` and `len` the bytes of
 * the batch in its object, each a multiple of 4, `len` above 0, and `addr`
 * the batch's GTT address at `offset`. `clflush` is I915_CLFLUSH_BEFORE
 * when the batch's lines must be flushed before they are read. The shadow
 * holds `shadow_size` bytes, `len` and the trampoline at least.
 */
struct i915_cmd_parse {
    const uintptr_t *batch;
    uint32_t offset;
    uint32_t len;
    uint32_t addr;
    uint32_t clflush;
    const uintptr_t *shadow;
    uint32_t shadow_size;
};

/** Whether `engine` parses the batches whose length a call gives, as
 * Linux's intel_engine_using_cmd_parser: on Haswell, every ring. */
int i915_engine_using_cmd_parser(const struct i915_engine *engine);

/**
 * Copy a batch to its shadow and check it before `engine` runs it, as
 * Linux 6.12's intel_engine_cmd_parser on graphics 7 with its trampoline
 * (i915_cmd_parser.c). The shadow runs privileged from its start and then
 * unprivileged from `len`, where the trampoline is: a batch that passes runs
 * privileged from the copy, and the trampoline ends; one with a command or
 * register the tables refuse, or any MI_BATCH_BUFFER_START, has its copy
 * ended at once and the trampoline start the original, unprivileged, so
 * that the chip refuses what it must as it would without the parser.
 *
 * @return 0 to run the shadow; -EINVAL for a command the engine does not
 *         know, one that runs past the batch, or a batch without
 *         MI_BATCH_BUFFER_END, which Linux's call refuses; or -ENOMEM
 *         without a temporary mapping.
 */
int32_t i915_cmd_parser(const struct i915_engine *engine, const struct i915_cmd_parse *p);

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
 * i915_gem_object_ggtt_pin. A partial view may cover the whole object, a
 * binding apart from the normal view's, as Linux's I915_GTT_VIEW_PARTIAL.
 * A binding that exists is reused unless it is
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

/**
 * Bind the remapped view `remap` of the object into the global GTT and pin
 * the binding, as Linux's i915_gem_object_ggtt_pin with an
 * I915_GTT_VIEW_REMAPPED view: page k of the binding maps the object's page
 * I915_REMAP_PAGE(remap, k). A binding of the same view is reused, as
 * `i915_vma_pin` reuses one. In `*out`.
 *
 * @return 0; -EINVAL for an empty view, a row wider than `src_stride` or
 *         a view past the object; or `i915_vma_pin`'s errors.
 */
int32_t i915_vma_pin_remapped(struct drm_i915_gem_object *obj, const struct i915_remap_info *remap,
                              uint32_t flags, uint32_t align, struct i915_vma **out);

/*
 * Where `i915_vma_pin_args` binds a whole object: `flags` are I915_PIN_*,
 * `align` a power of two or 0, `size` the least bytes its node takes
 * (EXEC_OBJECT_PAD_TO_SIZE), 0 for the object's, and `offset` the offset
 * of I915_PIN_OFFSET_FIXED or the lowest of I915_PIN_OFFSET_BIAS.
 */
struct i915_pin_args {
    uint32_t flags;
    uint64_t align;
    uint64_t size;
    uint64_t offset;
};

/**
 * Bind the whole object where `a` says and pin the binding, as Linux's
 * i915_vma_pin_ww for a program's batch: a binding that exists is reused
 * unless it is misplaced (Linux's i915_vma_misplaced), when it is undone
 * first. A fixed offset takes its range from the bindings there, each
 * waited for while the GPU uses it unless I915_PIN_NONBLOCK, as Linux's
 * i915_gem_evict_for_node. In `*out`.
 *
 * @return 0; -EINVAL for a fixed offset not aligned or past the GTT;
 *         -ENOSPC without room, or when a binding in the way is pinned or
 *         the firmware's or the GT's; -EINTR; or `i915_vma_pin`'s errors.
 */
int32_t i915_vma_pin_args(struct drm_i915_gem_object *obj, const struct i915_pin_args *a,
                          struct i915_vma **out);

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
 * freed. A binding whose object the GPU may still use is kept, since its
 * entries and pages must stay until the work is done: a caller that may
 * sleep waits with `i915_gem_object_wait_idle` first, where Linux's
 * unbind waits itself. Never sleeps.
 *
 * @return 0, -EAGAIN while it is pinned, or -EBUSY while its object is
 *         active.
 */
int32_t i915_vma_unbind(struct i915_vma *vma);

/** Undo every binding of the object: 0, or -EAGAIN when one is pinned or
 * the object is active. */
int32_t i915_gem_object_unbind(struct drm_i915_gem_object *obj);

/** The object's binding inside the aperture that maps page `pgoff`, or
 * NULL; a remapped view is never one. */
struct i915_vma *i915_vma_for_page(struct drm_i915_gem_object *obj, uint32_t pgoff);

/** Recompute what a fence needs of the binding after its object's tiling
 * changed, and whether it still serves a GTT map and a fence. */
void i915_vma_update_fenceable(struct i915_vma *vma);

/**
 * The range of `size` bytes aligned to `align` in [lo, hi) of the global
 * GTT that no binding, kept surface or object of the GT's touches, the
 * lowest, or with I915_PIN_HIGH the highest, in `*at`; a binding marked
 * `evict` counts as free. 0, or -ENOSPC.
 */
int32_t i915_ggtt_find(struct drm_i915_private *i915, uint64_t size, uint64_t align, uint64_t lo,
                       uint64_t hi, uint32_t flags, uint64_t *at);

/**
 * Make room in [start, end) of the global GTT for `size` bytes aligned to
 * `align`, at the end `flags` names, as Linux's i915_gem_evict_something:
 * the bindings of objects are scanned least recently used first, each
 * unpinned one whose object is idle counted as free, until a range fits;
 * then those of the scanned that lie in the range are undone. When the
 * idle bindings make no room, it waits for the GPU to finish its work and
 * scans once more, as Linux's ggtt_flush, unless `flags` has
 * I915_PIN_NONBLOCK. A pinned binding, one whose object the GPU may still
 * use, and a range the kernel reserved are never undone. Sleeps only in
 * that wait, which a signal ends.
 *
 * @return 0, -ENOSPC when even the idle bindings make no room, or -EINTR.
 */
int32_t i915_gem_evict_something(struct drm_i915_private *i915, uint64_t size, uint64_t align,
                                 uint64_t start, uint64_t end, uint32_t flags);

/**
 * Empty [start, start + size) of the global GTT for a binding at a fixed
 * offset, as Linux's i915_gem_evict_for_node: each binding there undone,
 * once the GPU has finished with its object, which is waited for unless
 * `flags` has I915_PIN_NONBLOCK. With I915_PIN_NOEVICT nothing is undone.
 *
 * @return 0; -ENOSPC when a binding there is pinned or, with
 *         I915_PIN_NOEVICT, is there at all, or when the range meets the
 *         firmware's surfaces or the GT's own objects, or a binding is busy
 *         under I915_PIN_NONBLOCK; or -EINTR.
 */
int32_t i915_gem_evict_for_node(struct drm_i915_private *i915, uint64_t start, uint64_t size,
                                uint32_t flags);

/**
 * Undo every binding of an object that nothing pins, once the GPU is idle,
 * as Linux's i915_gem_evict_vm, for a submission that found no room for
 * its objects otherwise. May sleep.
 *
 * @return 0 or -EINTR.
 */
int32_t i915_gem_evict_vm(struct drm_i915_private *i915);

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

/* Which objects `i915_gem_shrink_flags` purges, Linux's I915_SHRINK_
 * bits: those with no binding, and those with bindings, undone first. */
#define I915_SHRINK_UNBOUND 0x1u
#define I915_SHRINK_BOUND   0x2u

/**
 * Purge as `i915_gem_shrink` does, the objects `flags` names alone, as
 * Linux's i915_gem_shrink does with no swap, where only purgeable objects
 * give their pages back. Does not sleep. Returns the frames freed.
 */
uint32_t i915_gem_shrink_flags(struct drm_i915_private *i915, uint32_t want, uint32_t flags);

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
 *         -EAGAIN while stale frames are still pinned; -EINTR for a signal
 *         while the GPU finishes with the stale frames; or -ENOMEM.
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

/** The GGTT address of the remapped view `remap` of `fb`'s object, which
 * its plane's pin bound, or 0 when no pinned binding of it is there. */
uint32_t intel_fb_ggtt_remapped(const struct drm_framebuffer *fb,
                                const struct i915_remap_info *remap);

/**
 * Whether the primary or sprite plane state `ps` scans out through a
 * remapped view, as Linux's intel_plane_needs_remap asks: it is visible,
 * its framebuffer's object is the driver's, its stride passes the plane's
 * limit and its layout can be remapped (X tiles, or linear at a stride of
 * whole pages). 1 with the view in `*remap`, or 0 with `remap->width` 0.
 * The plane's check has computed `ps`'s source rectangle.
 */
int intel_plane_remap(const struct drm_plane_state *ps, struct i915_remap_info *remap);

/**
 * Check a new framebuffer of an object of the driver against the object's
 * layout, Linux's intel_fb_bo_framebuffer_init and intel_fill_fb_info: a
 * framebuffer made without modifiers of an X-tiled object takes X tiles
 * and of a Y-tiled one is refused; a modifier must match the object's
 * tiling and the pitch its stride; X tiles must fit the object in whole
 * rows of tiles. Then the object counts it (`fb_refs`). 0 or -EINVAL; the
 * mode configuration's `fb_create`.
 */
int32_t intel_fb_create(struct drm_device *dev, struct drm_framebuffer *fb);

/** Let go the count `intel_fb_create` took; `fb_destroy`. */
void intel_fb_destroy(struct drm_framebuffer *fb);

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

/** The bits of the primary plane's DSPCNTR that set the pipe and not the
 * plane, Linux's i9xx_plane_ctl_crtc: the pipe's colour space conversion
 * when it makes limited range RGB. A disabled plane keeps them, since
 * they also act on the pipe's background. */
uint32_t intel_plane_ctl_crtc(const struct intel_crtc *crtc);

/*
 * The watermarks (intel_wm.c), Linux's i9xx_wm.c for Haswell.
 */

/** Read the memory latencies from MCH_SSKPD and the watermark registers,
 * the low-power levels turned off, each pipe the readout found on keeping
 * the firmware's level 0, Linux's ilk_wm_get_hw_state. After the readout. */
void intel_wm_init(struct drm_i915_private *i915);

/**
 * Compute `crtc`'s watermarks at the five levels into `wm` for `cfg` and
 * the planes of `state` (their states now where `state` has none, or for
 * NULL), Linux's ilk_compute_pipe_wm. 0, or -EINVAL when level 0 does not
 * fit the pipe's share of the FIFO.
 */
int32_t intel_wm_compute(struct intel_crtc *crtc, struct drm_atomic_state *state,
                         const struct intel_crtc_config *cfg, struct intel_pipe_wm *wm);

/** Make `wm` safe for the planes of `old` as well, the larger of each
 * value, Linux's ilk_compute_intermediate_wm. 0, or -EINVAL when level 0
 * then does not fit. */
int32_t intel_wm_intermediate(const struct intel_pipe_wm *old, struct intel_pipe_wm *wm);

/**
 * Program `crtc`'s watermarks for the commit `state`, Linux's
 * ilk_initial_watermarks: for a mode set, those of its new configuration;
 * else the intermediate of the old planes' and the new, made the new
 * planes' alone once the vblank latched them (`intel_wm_vblank`). Inside
 * the commit, before the planes are written; may take a vblank reference.
 */
void intel_wm_commit(struct intel_crtc *crtc, struct drm_atomic_state *state, int modeset);

/** Program `crtc`'s new planes' watermarks once the frame counter passed
 * the commit's frame, Linux's ilk_optimize_watermarks. From the deferred
 * vblank work. */
void intel_wm_vblank(struct intel_crtc *crtc);

/**
 * Compute each pipe's watermarks for the state the readout and adoption
 * built, with no intermediate, and program them, Linux's ilk_wm_sanitize;
 * the firmware's stay when any pipe's do not fit. Once, after adoption.
 */
void intel_wm_sanitize(struct drm_i915_private *i915);

/** Let a pending `intel_wm_vblank` go, with its vblank reference. */
void intel_wm_cancel(struct intel_crtc *crtc);

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
/** The CRTC's `get_vblank_timestamp`, Linux's
 * intel_crtc_get_vblank_timestamp: the end of the latest vertical blank
 * from PIPEDSL. 1, or 0 for a pipe without a clock. Any context. */
int intel_crtc_get_vblank_timestamp(struct drm_crtc *crtc, int64_t *time_ns, int in_vblank_irq);

/**
 * Hook the display's interrupts: each pipe's vblank and primary flip done,
 * masked until a holder wants them, and the deferred work that takes them.
 * 0 or i915_irq_register's error.
 */
int32_t intel_vblank_init(struct drm_i915_private *i915);

/** Unhook them, with every held event sent and every boost after a vblank
 * dropped. */
void intel_vblank_fini(struct drm_i915_private *i915);

/** Send `crtc`'s held event now, as a pipe that goes off does, with
 * interrupts off; the pipe's interactive mark goes once no flip is
 * pending. */
void intel_crtc_send_event(struct intel_crtc *crtc);

/** Let `c`'s interactive mark go once its flip is no longer pending, as
 * Linux's intel_cleanup_plane_fb after the flip. Interrupts masked. */
void intel_crtc_interactive_end(struct intel_crtc *c);

/**
 * Boost the GT's clock at the next vblank of `c` for `fence`, the request
 * drawing a flip, if the chip has not begun it by then, as Linux's
 * intel_display_rps_boost_after_vblank: nothing for a fence of another
 * driver or a merge, a pipe whose vblanks cannot be had, or when no memory
 * is left. Each call adds its own entry, so a later commit on the plane
 * leaves a pending flip's boost in place. Process context; may be called
 * with interrupts on.
 */
void intel_crtc_boost_after_vblank(struct intel_crtc *c, struct dma_fence *fence);

/**
 * The display's deferred interrupt work, `irq.deferred_display`: each
 * pipe's vblank counted by the DRM core, its flips' requests boosted, the
 * events whose frame has come sent, and the CRC of each frame entered. In
 * the interrupt, after the mask's lock is let go.
 */
void intel_display_irq_work(struct drm_i915_private *i915);

/**
 * Report pipe `c`'s FIFO underruns or not, Linux's
 * intel_set_cpu_fifo_underrun_reporting on Haswell: on clears the pipe's
 * underrun in GEN7_ERR_INT and unmasks the error interrupt once every pipe
 * reports; off masks it and logs an underrun latched while it reported.
 * The first underrun turns its pipe's reporting off. Takes the interrupt
 * mask's lock; not from a handler.
 */
void intel_fifo_underrun_reporting(struct intel_crtc *c, int on);

/*
 * The eDP panel's AUX channel (intel_dp_aux.c), under the big lock in
 * process context; a transfer may sleep a millisecond while the channel
 * is busy, and holds the panel's VDD while it runs.
 */

/* A message on the AUX channel, Linux's drm_dp_aux_msg: `request`
 * DP_AUX_*, `address` in the DPCD or on the I2C bus, `size` bytes of
 * `buffer`, and the sink's `reply`. */
struct intel_dp_aux_msg {
    uint32_t address;
    uint8_t request;
    uint8_t reply;
    uint8_t *buffer;
    uint32_t size;
};

/**
 * Send `msg` once, Linux's intel_dp_aux_transfer: the bytes sent or
 * received, with the sink's reply in `msg->reply`, -E2BIG for more than
 * 16 bytes, -EINVAL for another request, or the channel's -EBUSY, -EIO or
 * -ETIMEDOUT.
 */
int32_t intel_dp_aux_transfer(struct intel_dp *dp, struct intel_dp_aux_msg *msg);

/** Read `size` bytes at DPCD `addr` into `buf`, as Linux's
 * drm_dp_dpcd_read: `size`, or the first error of 32 tries. */
int32_t intel_dp_dpcd_read(struct intel_dp *dp, uint32_t addr, uint8_t *buf, uint32_t size);

/** Write `size` bytes, 16 at most, of `buf` at DPCD `addr`: `size`, or the
 * first error of 32 tries. */
int32_t intel_dp_dpcd_write(struct intel_dp *dp, uint32_t addr, const uint8_t *buf, uint32_t size);

/** Write one byte at DPCD `addr`. */
int32_t intel_dp_dpcd_writeb(struct intel_dp *dp, uint32_t addr, uint8_t value);

/**
 * Read the sink's EDID over I2C over AUX into `buf`, `cap` bytes at most,
 * as Linux's drm_edid_read_ddc: the base block four times at most until
 * its header and sum are good, then the extensions it counts that fit;
 * the bytes read in `*len`. 0, -EIO when the sink did not answer, or
 * -EINVAL for a base block never good.
 */
int32_t intel_dp_read_edid(struct intel_dp *dp, uint8_t *buf, uint32_t cap, uint32_t *len);

/** Whether a sink answers on `dp`'s I2C over AUX, Linux's drm_probe_ddc
 * of the AUX channel's DDC. */
int intel_dp_probe_ddc(struct intel_dp *dp);

/** `dp`'s I2C over AUX bus, for the DP++ adaptor's registers. */
void intel_dp_i2c_bus(struct intel_dp *dp, struct intel_i2c_bus *bus);

/*
 * The panel power sequencer (intel_pps.c), Linux's intel_pps.c for one
 * panel on a Lynx Point PCH, under the big lock in process context. The
 * waits sleep: on the panel's timings, at most the 500 ms of a power cycle
 * and the 5 s the sequencer is given to finish.
 */

/** Sleep `ms` milliseconds, as Linux's msleep: on the scheduler's clock in
 * a process, spun on the device's clock before there is one. */
void intel_display_msleep(struct drm_i915_private *i915, uint32_t ms);

/**
 * Set the sequencer up, Linux's intel_pps_init: the delays chosen from its
 * registers, the VBT and the eDP specification's limits and written back,
 * and a VDD the firmware left on taken over.
 */
void intel_pps_init(struct drm_i915_private *i915);

/**
 * Force the panel's VDD on for a caller that needs the panel's logic, as
 * Linux's intel_pps_vdd_on_unlocked: after the power cycle's wait and with
 * the power up delay when the panel's power is off. 1 when this call
 * turned it on, so that the caller turns it off again, 0 when it was.
 */
int intel_pps_vdd_on_unlocked(struct drm_i915_private *i915);

/** Let VDD go: at once with `sync`, else once five power cycles pass
 * without another caller, as Linux's delayed work does it. */
void intel_pps_vdd_off_unlocked(struct drm_i915_private *i915, int sync);

/** Turn off a VDD let go whose time has come; from each commit and probe,
 * Linux's edp_panel_vdd_work. */
void intel_pps_vdd_work(struct drm_i915_private *i915);

/** Choose the delays again once the VBT's panel is known and write them,
 * with VDD let go if nothing wants it, Linux's intel_pps_init_late. */
void intel_pps_init_late(struct drm_i915_private *i915);

/** Power the panel on, Linux's intel_pps_on: after the power cycle's wait,
 * then wait for the sequencer to be idle on. */
void intel_pps_on(struct drm_i915_private *i915);

/** Power it off, with VDD and the backlight's enable, Linux's
 * intel_pps_off, and wait for the sequencer to be idle off. */
void intel_pps_off(struct drm_i915_private *i915);

/** Set the backlight's enable once the backlight on delay has passed since
 * the panel came on, Linux's intel_pps_backlight_on. */
void intel_pps_backlight_on(struct drm_i915_private *i915);

/** Clear it and wait the backlight off delay, Linux's
 * intel_pps_backlight_off. */
void intel_pps_backlight_off(struct drm_i915_private *i915);

/** Whether the panel's power or its VDD is on. */
int intel_pps_have_power_or_vdd(struct drm_i915_private *i915);

/*
 * The CD clock (intel_cdclk.c), which Haswell's driver reads and never
 * changes.
 */

/** Read the CD clock into `display.cdclk` and `display.max_cdclk`, Linux's
 * hsw_get_cdclk; from the display's init, before the AUX channel, whose
 * clock divides it. */
void intel_cdclk_init(struct drm_i915_private *i915);

/** The fastest pixel clock in kHz a pipe may take, Linux's max_dotclk_freq. */
uint32_t intel_max_dotclock(struct drm_i915_private *i915);

/** 0 when a pipe's pixel rate of `pixel_rate` kHz fits the CD clock, Linux's
 * fixed_modeset_calc_cdclk, -EINVAL otherwise. */
int32_t intel_cdclk_check(struct drm_i915_private *i915, uint32_t pixel_rate);

/* The translations of DDI_BUF_TRANS a DisplayPort link uses on Haswell. */
#define INTEL_DDI_DP_LEVELS 9

/** Write `dp`'s port's buffer translations for DisplayPort, Linux's
 * hsw_prepare_dp_ddi_buffers. (intel_ddi.c) */
void intel_ddi_prepare_buf_trans(struct intel_dp *dp);

/** The most swing the port drives, as DP_TRAINING_LANEx_SET codes it. */
uint8_t intel_ddi_dp_voltage_max(void);

/** Build `dp`'s DDI_BUF_CTL for `lanes` at the lowest levels, not yet
 * enabled, in its `buf_ctl`, Linux's intel_ddi_init_dp_buf_reg. */
void intel_ddi_init_dp_buf_reg(struct intel_dp *dp, uint32_t lanes);

/** Drive the levels of lane 0's `train_set` on `dp`'s port, Linux's
 * hsw_set_signal_levels. */
void intel_ddi_set_signal_levels(struct intel_dp *dp);

/** Wait, 8 us at most, for `dp`'s port's buffer to go idle; a timeout is
 * logged. */
void intel_ddi_wait_buf_idle(struct intel_dp *dp);

/** Turn the transport off and on again on pattern 1, `enhanced` framing
 * or not, and enable the buffer, Linux's intel_ddi_prepare_link_retrain. */
void intel_ddi_prepare_link_retrain(struct intel_dp *dp, int enhanced);

/** Drive the training pattern of DPCD code `dp_train_pat` on the
 * transport, the normal pattern for DP_TRAINING_PATTERN_DISABLE. */
void intel_ddi_set_link_train(struct intel_dp *dp, uint8_t dp_train_pat);

/** Put the transport on its idle pattern, Linux's
 * intel_ddi_set_idle_link_train. */
void intel_ddi_set_idle_link_train(struct intel_dp *dp);

/**
 * Train the eDP link at `display.dp`'s `link_rate` and `lane_count`, the
 * buffer built by `intel_ddi_init_dp_buf_reg`, and leave the port on its
 * idle pattern, Linux's intel_dp_start_link_train. A failure is tried once
 * more at once; a second chooses the link the next mode set trains, for
 * a mode of `mode_clock` kHz. 0; -EAGAIN when the link did not train and a
 * new mode set should try the link chosen, Linux's case for its modeset
 * retry; or -EIO when no link is left to try. Process context; sleeps.
 * (intel_dp_link_training.c)
 */
int32_t intel_dp_start_link_train(struct intel_dp *dp, uint32_t mode_clock);

/** Whether `dp`'s trained link still has channel equalization on every
 * lane, Linux's intel_dp_link_ok for 8b/10b: 1, 0, or -EIO when the
 * status does not read. */
int intel_dp_link_ok(struct intel_dp *dp);

/** End the training: the sink off its pattern and the port on the normal
 * one, Linux's intel_dp_stop_link_train. */
void intel_dp_stop_link_train(struct intel_dp *dp);

/*
 * A mode set of a pipe on a DDI port, in Linux's order (intel_ddi.c):
 * the port's clock and link up before the pipe, the transcoder's function
 * and enable after it, and the reverse to turn it off; `cfg->output`
 * says which of eDP, DisplayPort or HDMI. From the CRTC's hooks; they
 * sleep.
 */

/**
 * Bring the port up for `cfg`, Linux's intel_ddi_pre_enable: for eDP and
 * DisplayPort hsw_ddi_pre_enable_dp with intel_ddi_set_dp_msa (the panel's
 * power, the port's clock and buffer, the sink in D0, the link trained,
 * and the stream's attributes for the transcoder); for HDMI
 * intel_ddi_pre_enable_hdmi (the port's clock, the sink's TMDS mode and
 * the infoframes). 0, or `intel_dp_start_link_train`'s error.
 */
int32_t intel_ddi_pre_enable(struct drm_i915_private *i915, const struct intel_crtc_config *cfg);

/** Start the stream on pipe `pipe`, Linux's intel_enable_ddi: the
 * transcoder's function and enable, then for DisplayPort the link's normal
 * pattern and for eDP the backlight, or for HDMI the port's buffer. */
void intel_ddi_enable(struct drm_i915_private *i915, int pipe, const struct intel_crtc_config *cfg);

/** The output's first step off, Linux's intel_disable_ddi: the backlight
 * of eDP, nothing for the rest. */
void intel_ddi_disable(struct drm_i915_private *i915, const struct intel_crtc_config *cfg);

/** Stop the stream and the port, Linux's intel_ddi_post_disable: vblanks
 * off, the transcoder and its function off, the fitter off, then for
 * DisplayPort the sink in D3, the buffer, the panel's power of eDP and the
 * clock off, or for HDMI the infoframes, the buffer and the clock off. */
void intel_ddi_post_disable(struct drm_i915_private *i915, struct intel_crtc *crtc,
                            const struct intel_crtc_config *cfg);

/** Write port `port`'s buffer translations for HDMI, the VBT's `level` as
 * entry 9, Linux's hsw_prepare_hdmi_ddi_buffers. */
void intel_ddi_prepare_hdmi_buf_trans(struct drm_i915_private *i915, int port, int level);

/** The buffer translation an HDMI port drives by default on Haswell,
 * Linux's hsw_ddi_hdmi_level default. */
int intel_ddi_hdmi_default_level(void);

/*
 * The display PLLs (intel_dpll.c), Linux's intel_dpll_mgr.c for Haswell:
 * WRPLL 1 and 2 for HDMI, SPLL at 1350 MHz, and the LCPLL's three outputs
 * for DisplayPort, shared between pipes whose clocks agree.
 */

/** Set the PLLs up at the display's init and read out what the firmware
 * left running, Linux's intel_shared_dpll_init and readout. */
void intel_dpll_init(struct drm_i915_private *i915);

/** The WRPLL's dividers for a clock of `clock` Hz, Linux's
 * hsw_ddi_calculate_wrpll: `*r2`, `*n2` and `*p`. */
void intel_dpll_calc_wrpll(uint32_t clock, uint32_t *r2, uint32_t *n2, uint32_t *p);

/** The clock in kHz of symbol clock a WRPLL_CTL value `wrpll` gives,
 * Linux's hsw_ddi_wrpll_get_freq: the LCPLL's 2.7 GHz, the PCH's 135 MHz
 * SSC, or for WRPLL_REF_SPECIAL_HSW the SSC on a ULT part and the
 * non-SSC reference intel_dpll_init read (`dpll_nssc`) on another; 0 for
 * another reference. */
uint32_t intel_dpll_wrpll_freq(const struct drm_i915_private *i915, uint32_t wrpll);

/** The clock in kHz an SPLL_CTL value `spll` gives, Linux's
 * hsw_ddi_spll_get_freq; 0 for a frequency it does not name. */
uint32_t intel_dpll_spll_freq(uint32_t spll);

/** Start `plan` from the PLLs as they run, without the pipes `leaving`, a
 * bit each, which the commit sets again. */
void intel_dpll_plan_init(struct drm_i915_private *i915, uint32_t leaving,
                          struct intel_dpll_plan *plan);

/**
 * Choose the PLL of `cfg` for pipe `pipe` and reserve it in `plan`, Linux's
 * hsw_compute_dpll and hsw_get_dpll: for HDMI the WRPLL whose value is the
 * one the clock takes, else a free one, WRPLL 1 first, `cfg->port_clock`
 * then the clock the PLL gives; for DisplayPort the LCPLL output of the
 * link rate. 0, -EINVAL for a clock no PLL gives, or -EBUSY when both
 * WRPLLs run other clocks.
 */
int32_t intel_dpll_compute(struct drm_i915_private *i915, struct intel_dpll_plan *plan, int pipe,
                           struct intel_crtc_config *cfg);

/** Read out pipe `pipe`'s PLL from the clock select `sel` of its port into
 * `cfg`: the PLL, its value and the link's clock, and count the pipe on
 * it. 0, or -EINVAL for a PLL that does not run. */
int32_t intel_dpll_readout(struct drm_i915_private *i915, int pipe, uint32_t sel,
                           struct intel_crtc_config *cfg);

/** Turn off a WRPLL or the SPLL that runs for no pipe, Linux's
 * intel_sanitize_dplls. */
void intel_dpll_sanitize(struct drm_i915_private *i915);

/** Take `cfg`'s PLL for pipe `pipe` and turn it on if it is off, Linux's
 * intel_reference_shared_dpll and intel_enable_shared_dpll; the WRPLL
 * runs 20 us after its enable. */
void intel_dpll_enable(struct drm_i915_private *i915, int pipe,
                       const struct intel_crtc_config *cfg);

/** Let pipe `pipe`'s PLL go, turning it off when no pipe has it, Linux's
 * intel_disable_shared_dpll and intel_unreference_shared_dpll. */
void intel_dpll_disable(struct drm_i915_private *i915, int pipe,
                        const struct intel_crtc_config *cfg);

/** The PORT_CLK_SEL value of a PLL id. */
uint32_t intel_dpll_port_clk_sel(int id);

/*
 * GMBUS and the EDID (intel_gmbus.c), Linux's intel_gmbus.c with its
 * bit-banging fallback, i2c-algo-bit's, and drm_edid.c's read of the
 * EDID over an I2C bus. Process context; they sleep.
 */

/** Set the GMBUS pins up, Linux's intel_gmbus_setup: each valid pin's bus
 * at 100 kHz, and the controller reset. */
void intel_gmbus_setup(struct drm_i915_private *i915);

/** Whether `pin` is a GMBUS pin of the PCH, Linux's
 * intel_gmbus_is_valid_pin. */
int intel_gmbus_is_valid_pin(int pin);

/** The I2C bus of GMBUS pin `pin`, or NULL. */
struct intel_i2c_bus *intel_gmbus_bus(struct drm_i915_private *i915, int pin);

/** Make GMBUS pin `pin` bit-bang while `force` callers want it, Linux's
 * intel_gmbus_force_bit: a call with `force` set adds one, without takes
 * one away. */
void intel_gmbus_force_bit(struct drm_i915_private *i915, int pin, int force);

/** Whether GMBUS pin `pin` bit-bangs, Linux's intel_gmbus_is_forced_bit. */
int intel_gmbus_is_forced_bit(struct drm_i915_private *i915, int pin);

/**
 * Read the EDID of the sink on `bus` into `buf`, `cap` bytes at most, as
 * Linux's drm_edid_read_ddc: the base block four times at most until its
 * header and sum are good, then each extension it counts that fits, four
 * tries each, a bad one ending the read; the bytes read in `*len`. 0, -EIO
 * when the sink did not answer, or -EINVAL for a base block never good.
 */
int32_t intel_edid_read(const struct intel_i2c_bus *bus, uint8_t *buf, uint32_t cap, uint32_t *len);

/** Whether a sink answers a read of one byte of the EDID on `bus`, Linux's
 * drm_probe_ddc. */
int intel_i2c_probe_ddc(const struct intel_i2c_bus *bus);

/*
 * The HDMI outputs (intel_hdmi.c), Linux's intel_hdmi.c for Haswell's
 * DDI ports B to D.
 */

/** Make DDI port `p`'s HDMI connector, Linux's intel_hdmi_init_connector:
 * its DDC pin and level from the VBT, "max bpc" from 8 to 12. 0 or the
 * DRM core's error. */
int32_t intel_hdmi_connector_init(struct drm_i915_private *i915, struct intel_ddi_port *p);

/**
 * Compute what an HDMI output takes for `cfg`'s mode, Linux's
 * intel_hdmi_compute_config for Haswell on pipe `pipe`'s transcoder: the
 * bits a colour, 12 when the sink, "max bpc" and the clock allow, else 8;
 * the TMDS clock, twice the pixel's for DBLCLK; an HDMI sink's limited
 * range for a CEA mode but the first; and GCP and the AVI, SPD and HDMI
 * vendor infoframes. 0, or -EINVAL for a mode the port cannot drive.
 */
int32_t intel_hdmi_compute_config(struct intel_ddi_port *p, int pipe,
                                  const struct drm_connector_state *conn,
                                  struct intel_crtc_config *cfg);

/** Turn a DP++ adaptor's TMDS output on or off, Linux's
 * intel_dp_dual_mode_set_tmds_output: for a type 2 adaptor alone. */
void intel_hdmi_dual_mode_tmds(struct intel_ddi_port *p, int enable);

/** Write and enable `cfg`'s infoframes on its transcoder, or turn them
 * all off when `enable` is 0, Linux's hsw_set_infoframes. */
void intel_hdmi_set_infoframes(struct drm_i915_private *i915, int enable,
                               const struct intel_crtc_config *cfg);

/*
 * Hotplug (intel_hotplug.c), Linux's intel_hotplug.c and the PCH's part
 * of intel_hotplug_irq.c for Lynx Point.
 */

/** Hook the ports' hotplug interrupts and enable their detection, Linux's
 * intel_hpd_init and ibx_hpd_irq_setup. 0 or the interrupt's error. */
int32_t intel_hotplug_init(struct drm_i915_private *i915);

/** Unhook them and let the deferred work go. */
void intel_hotplug_fini(struct drm_i915_private *i915);

/** The deferred work: re-enable pins whose storm is over, then probe the
 * outputs of the pins that pulsed, Linux's i915_digport_work_func and
 * i915_hotplug_work_func, sending a hotplug event for each change. */
void intel_hotplug_work(struct drm_work *work);

/** Whether the sink of a PCH port's pin `pin` is there, Linux's
 * lpt_digital_port_connected: SDEISR's hotplug bit. */
int intel_hotplug_live(struct drm_i915_private *i915, int pin);

/*
 * The external DisplayPort outputs (intel_dp.c).
 */

/** Make DDI port `p`'s DisplayPort connector, Linux's
 * intel_dp_init_connector for an external port: its AUX channel from the
 * VBT, "max bpc" from 6 to 12. 0 or the DRM core's error. */
int32_t intel_dp_ext_connector_init(struct drm_i915_private *i915, struct intel_ddi_port *p);

/** Whether `dp`'s sink is there now: the panel always, a monitor by its
 * hotplug pin's live status. */
int intel_dp_connected(struct intel_dp *dp);

/**
 * Handle a pulse on `dp`'s pin, Linux's intel_dp_hpd_pulse: a long pulse
 * forgets the link's fallback and asks for a probe (1); a short one reads
 * the sink's DPCD and its service requests and, when the link lost its
 * training, retrains it by a mode set of its pipe, Linux's
 * intel_dp_retrain_link; 0 when handled, 1 when a probe should follow.
 */
int intel_dp_hpd_pulse(struct intel_dp *dp, int long_hpd);

/** The pipe whose configuration drives `dp`'s port with a link, or NULL. */
struct intel_crtc *intel_dp_active_crtc(struct intel_dp *dp);

/** Set pipe `c` again as it is, a full mode set, Linux's
 * intel_modeset_commit_pipes for one pipe. 0 or the commit's error. */
int32_t intel_display_commit_pipe(struct intel_crtc *c);

/** The device's power-well-backed parts a configuration needs, Linux's
 * get_crtc_power_domains on Haswell: whether `cfg` uses the display well
 * (pipe B or C, transcoder A to C, a fitter, a forced-through eDP input,
 * or a DDI port B to D). */
int intel_crtc_needs_well(int pipe, const struct intel_crtc_config *cfg);

/*
 * The panel (intel_dp.c, intel_panel.c) and the fitter (intel_pfit.c).
 */

/**
 * Set the eDP panel up, Linux's intel_edp_init_connector: the VBT's panel,
 * the sequencer, the DPCD, the EDID and the fixed modes, the link rates
 * both ends take, and the source's OUI. 0 with `display.dp.present` set,
 * or -ENODEV when DDI A has no display, the panel does not answer or has
 * no fixed mode, with its VDD let go. Sleeps.
 */
int32_t intel_dp_init(struct drm_i915_private *i915);

/** Make the eDP connector and its encoder, which reach every pipe, with
 * Linux's "max bpc" and "scaling mode" (Full aspect at first). 0 or the
 * DRM core's error. */
int32_t intel_dp_connector_init(struct drm_i915_private *i915);

/**
 * Configure `cfg` for the eDP panel on `pipe`, Linux's
 * intel_dp_compute_config: `cfg->mode`, the user's mode on entry, becomes
 * the fixed mode that drives it, and the link, the bits a pixel, the M/N
 * values, the fitter and the dither are chosen as `conn`'s "scaling mode"
 * and "max bpc" ask. `crc_wa` says a CRC of the fitter runs. 0, -EINVAL
 * for a mode the panel or the link cannot take, or -ERANGE for a fitter's
 * scale past its limit.
 */
int32_t intel_dp_compute_config(struct intel_dp *dp, int pipe,
                                const struct drm_connector_state *conn, int crc_wa,
                                struct intel_crtc_config *cfg);

/** Set the sink's power state to DP_SET_POWER_D0 or _D3, Linux's
 * intel_dp_set_power: D0 tried three times a millisecond apart, after the
 * source's OUI. */
void intel_dp_set_power(struct intel_dp *dp, uint8_t mode);

/** Choose the panel's fixed modes into `display.dp.fixed`, Linux's
 * intel_panel_add_edid_fixed_modes and intel_panel_add_vbt_lfp_fixed_mode:
 * the EDID's preferred mode, or its first, and its other modes of that
 * size and flags; else the VBT's mode. */
void intel_panel_init_fixed_modes(struct drm_i915_private *i915);

/** The fixed mode nearest `mode` in refresh rate, Linux's
 * intel_panel_fixed_mode, or NULL for a panel without one. */
const struct drm_mode_modeinfo *intel_panel_fixed_mode(struct drm_i915_private *i915,
                                                       const struct drm_mode_modeinfo *mode);

/** 0 when `mode` has a fixed mode's size and rate, Linux's
 * intel_panel_mode_valid, else -EINVAL. */
int32_t intel_panel_mode_valid(struct drm_i915_private *i915, const struct drm_mode_modeinfo *mode);

/** Linux's intel_cpu_transcoder_mode_valid for display version 5 and up: 0
 * for a mode of at least 64 pixels across with 32 of horizontal blanking,
 * 5 lines of vertical blanking and an hsync after the active area, else
 * -EINVAL. */
int32_t intel_cpu_transcoder_mode_valid(const struct drm_mode_modeinfo *m);

/** Replace the user's `mode` with the fixed mode that drives it, Linux's
 * intel_panel_compute_config: 0, or -EINVAL for a rate more than a hertz
 * from the panel's. */
int32_t intel_panel_compute_config(struct drm_i915_private *i915, struct drm_mode_modeinfo *mode);

/** Add the fixed modes to `connector`'s probed modes, Linux's
 * intel_panel_get_modes; how many. */
int intel_panel_get_modes(struct drm_i915_private *i915, struct drm_connector *connector);

/**
 * Set `cfg`'s fitter for a pipe source of `src_w` by `src_h` shown in its
 * `mode`, Linux 6.12's pch_panel_fitting: none for a source of the mode's
 * size, else a window by `scaling_mode`, DRM_MODE_SCALE_*. 0, -EINVAL for
 * a window or source the fitter of `pipe` cannot take, or -ERANGE for a
 * scale down past 1.125, by 6.18's checks, which 6.12 lacks.
 */
int32_t intel_pfit_compute_config(int pipe, uint32_t scaling_mode, struct intel_crtc_config *cfg);

/** The rate in kHz the pipe reads its source at, Linux's
 * ilk_pipe_pixel_rate: the mode's clock, more when the fitter scales down. */
uint32_t intel_pfit_pixel_rate(const struct intel_crtc_config *cfg);

/** Turn pipe `pipe`'s fitter on as `cfg` sets it, Linux's ilk_pfit_enable. */
void ilk_pfit_enable(struct drm_i915_private *i915, int pipe, const struct intel_crtc_config *cfg);

/** Turn it off when `cfg` had it on, Linux's ilk_pfit_disable. */
void ilk_pfit_disable(struct drm_i915_private *i915, int pipe, const struct intel_crtc_config *cfg);

/**
 * Start the CRC source `name` on `crtc`, or stop it for NULL or "none",
 * Linux's intel_crtc_set_crc_source (i915_display_debugfs.c). Sleeps: it
 * takes the commit lock for the eDP pipe's fitter workaround. 0, -EINVAL
 * for a source Haswell does not have, or -EIO for a pipe that is off.
 */
int32_t intel_crtc_set_crc_source(struct drm_crtc *crtc, const char *name);

/** Whether `name` is a source of `crtc`: 0 with `*values_cnt` the values
 * a frame gives, 5, or -EINVAL. Does not sleep. */
int32_t intel_crtc_verify_crc_source(struct drm_crtc *crtc, const char *name, uint32_t *values_cnt);

/** Linux's names of the CRC sources, `*count` of them, in its order; a
 * static table. */
const char *const *intel_crtc_get_crc_sources(struct drm_crtc *crtc, uint32_t *count);

/** Enter `crc`, the CRC the error interrupt read at `crtc`'s CRC done,
 * while a source runs, past the first, as Linux's
 * display_pipe_crc_irq_handler; from the deferred interrupt work. */
void intel_crtc_crc_frame(struct intel_crtc *crtc, uint32_t crc);

/**
 * Take the eDP panel's backlight over from the boot's driver, Linux's
 * lpt_setup_backlight: attach the PCH's PWM through the device's
 * registers, its period from the VBT's frequency when the firmware left it
 * none, and drive "intel_backlight" through it, registering it when the
 * boot did not. From the display's init.
 */
void intel_backlight_setup(struct drm_i915_private *i915);

/** Turn the PWM on at the level kept, at least the VBT's least, Linux's
 * intel_backlight_enable with lpt_enable_backlight. */
void intel_backlight_enable(struct drm_i915_private *i915);

/** Turn the PWM off, its duty cycle 0, Linux's intel_backlight_disable
 * with lpt_disable_backlight; the level is kept for the next enable. */
void intel_backlight_disable(struct drm_i915_private *i915);

/** Turn the backlight's enable line with the panel's pipe: off while the
 * pipe is off, and on again at the class's wish when it comes back, or
 * with the pipe when the class has no switch. */
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
 * i915_gem_object_pin_to_display_plane: its caching off, its view bound
 * at a multiple of `align`, and its processor cache flushed. The view is
 * the whole object for a NULL `remap`, bound inside the aperture when
 * room there needs no wait and anywhere in the GTT otherwise, as Linux
 * binds it on a chip without a GMCH, or the remapped view `remap`,
 * anywhere. The binding in `*vma`, pinned, which
 * `i915_vma_unpin` lets go. 0 or the binding's error.
 */
int32_t i915_gem_object_pin_to_display(struct drm_i915_gem_object *obj,
                                       const struct i915_remap_info *remap, uint32_t align,
                                       struct i915_vma **vma);

/** Flush the processor's cache of bytes [start, end) of the object, for
 * rows drawn by the processor that the display reads. */
void i915_gem_object_flush_range(struct drm_i915_gem_object *obj, uint64_t start, uint64_t end);

/** Read the swizzle, clear every fence and set the GPU's swizzling, once
 * at the bind. */
void i915_gem_init_hw(struct drm_i915_private *i915);

/**
 * Give the hardware its GEM state again after a reset of the whole GPU, as
 * Linux's intel_ggtt_restore_fences: every fence written from the binding
 * it serves, and the GPU's swizzling set. The reset has revoked the
 * programs' aperture maps through the fences first (i915_reset.c).
 */
void i915_gem_restore_fences(struct drm_i915_private *i915);

/** Where byte `v` of a linear surface lies once tiled and swizzled, the
 * arithmetic a fence does (i915_fence.c). */
uint32_t i915_tiled_offset(uint32_t tiling, uint32_t stride, uint32_t swizzle, uint32_t v);

/*
 * Contexts, requests and parameters (i915_gem_context.c, i915_request.c,
 * i915_getparam.c), after Linux 6.12's gem/i915_gem_context.c,
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
 * ring and none elsewhere. `banned` is set when a hang it caused banned it
 * (i915_reset.c): its requests fail with -EIO and EXECBUFFER2 refuses it.
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
    uint8_t banned;
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
 * in a reset it did not cause, which the reset counts
 * (`i915_gem_context_mark_guilty` and `_innocent`). `hang_ticks` are the
 * ticks of its last two hangs, 0 for none, by which a third within 120 s
 * bans it. `file` is the file whose ban score its hangs raise, NULL once
 * the context is closed, as Linux's file_priv. `pid` and `comm` name the
 * process that made it, for the log. `closed` is set once DESTROY or the
 * file's close took it from the file.
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
    uint64_t hang_ticks[2];
    struct i915_file *file;
    uint32_t pid;
    char comm[16];
    struct i915_gem_context *next; /* the file's, by id */
};

/*
 * A file's state, Linux's drm_i915_file_private: its contexts by id, the
 * default context 0 first, and `ban_score`, the score of its contexts'
 * hangs and bans, at which it may make no more contexts
 * (I915_CLIENT_SCORE_BANNED). `hang_ticks` is the tick of its last hang,
 * 0 for none.
 */
struct i915_file {
    struct drm_i915_private *i915;
    struct i915_gem_context *contexts;
    uint32_t ban_score;
    uint64_t hang_ticks;
};

/* Linux's scores of a file's hangs (i915_file_private.h): a hang within a
 * minute of the last scores 1, a context banned 3, and a file at 9 may
 * make no more contexts; and a context's third hang within 120 s bans it
 * (i915_gem_context_types.h). In ticks of the kernel's clock. */
#define I915_CLIENT_SCORE_HANG_FAST   1
#define I915_CLIENT_FAST_HANG_TICKS   MS_TO_TICKS(60000)
#define I915_CLIENT_SCORE_CONTEXT_BAN 3
#define I915_CLIENT_SCORE_BANNED      9
#define I915_CONTEXT_FAST_HANG_TICKS  MS_TO_TICKS(120000)

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
 * before. `head`, `infix`, `postfix` and `tail` are its ring offsets: its
 * first command, its payload after the invalidation and the switch, its
 * breadcrumb, and past its end. `batch` is the GTT address of the batch
 * its payload starts, 0 for none, for the error state. `emitted` is the
 * tick at which it was added, by which debugfs gives its age.
 *
 * Submission. `awaits` counts what must happen before the chip may run it:
 * the fences it waits for that have not signalled, the requests on other
 * engines it waits to see given to the chip, and one count held while it
 * is built; each has a record on `await_list`. `exec_waiters` holds the
 * records of the requests that wait for this one to be given to the chip
 * (I915_EXEC_FENCE_SUBMIT). `added` is set once it is on its engine's
 * list, `submitted` once RING_TAIL is past it. A request whose fence holds
 * an error when it is given to the chip has its payload filled with
 * MI_NOOP, so that only its breadcrumb runs, as Linux's
 * __i915_request_skip. `awaits` and `added` change under the submission's
 * lock in i915_request.c, since a fence may signal in an interrupt.
 *
 * `hold`, when set, is what its batch needs until it retires: the objects
 * of its call and the parser's shadow, which `hold->release` lets go once
 * the chip is past it, as Linux's request keeps its vmas active.
 */
struct i915_await;

/* What a request holds for its batch until it retires; `release` frees it
 * with what it holds, in process context. */
struct i915_request_hold {
    void (*release)(struct i915_request_hold *hold);
};

struct i915_request {
    struct i915_fence fence;
    struct drm_i915_private *i915;
    struct i915_engine *engine;
    struct intel_context *ce;
    struct intel_context *prev_ce;
    uint32_t seqno;
    uint32_t head;
    uint32_t infix;
    uint32_t postfix;
    uint32_t tail;
    uint32_t batch;
    uint64_t emitted;
    uint8_t switched;
    uint8_t set_context;
    uint8_t was_valid;
    uint8_t added;
    uint8_t submitted;
    uint8_t boosted;  /* Linux's I915_FENCE_FLAG_BOOST, under the frequency lock */
    uint16_t waiters; /* i915_request_wait's callers waiting for it */
    uint32_t awaits;
    struct i915_await *await_list;
    struct i915_await *exec_waiters;
    struct i915_request *next; /* the engine's, oldest first */
    struct i915_request_hold *hold;
    int64_t added_ns; /* dma_fence_now() when it was added, for THROTTLE */
};

/*
 * What a request waits for: a fence, with a reference, whose callback `cb`
 * counts it done, or, with `fence` NULL, the request `src` on another
 * engine, done when that one is given to the chip. `next` links the
 * waiting request's records and `exec_next` those `src` holds.
 */
struct i915_await {
    struct dma_fence_cb cb; /* first, so that the callback finds its record */
    struct i915_request *rq;
    struct dma_fence *fence;
    uint64_t deadline; /* the tick a wait for another driver's fence ends, or 0 */
    struct i915_request *src;
    struct i915_await *next;
    struct i915_await *exec_next;
};

/** Whether the caller may do what Linux asks CAP_SYS_ADMIN for. */
int i915_capable(void);

/* A handler of one extension of a chain, given the extension's user
 * address: 0 or -errno. */
typedef int32_t (*i915_ext_fn)(uintptr_t ext, void *data);

/**
 * Walk the chain of extensions from the user address `ext`, as Linux's
 * i915_user_extensions: each one's flags and reserved words must be 0,
 * and its name must have a handler in `tbl` of `count`.
 *
 * @return 0; -E2BIG past 512 extensions; -EINVAL for a word that must be 0
 *         or an unknown name; -EFAULT; or a handler's error.
 */
int32_t i915_user_extensions(uint64_t ext, const i915_ext_fn *tbl, uint32_t count, void *data);

/** 0 when the `size` bytes, at most 8, at the user address `at` are zero;
 * -EINVAL when not; -EFAULT when they cannot be read. Linux's
 * check_user_mbz. */
int32_t i915_check_user_mbz(uintptr_t at, size_t size);

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
int32_t i915_gem_throttle_ioctl(struct drm_device *dev, void *data, struct drm_file *file);
int32_t i915_getparam_ioctl(struct drm_device *dev, void *data, struct drm_file *file);
int32_t i915_query_ioctl(struct drm_device *dev, void *data, struct drm_file *file);
int32_t i915_reg_read_ioctl(struct drm_device *dev, void *data, struct drm_file *file);

/** GET_PIPE_FROM_CRTC_ID, Linux's intel_get_pipe_from_crtc_id_ioctl
 * (intel_display.c): the CRTC's pipe, or -ENOENT for no such CRTC. */
int32_t intel_get_pipe_from_crtc_id_ioctl(struct drm_device *dev, void *data,
                                          struct drm_file *file);

/** Read the GT's slices, subslices, EUs and eDRAM once, as Linux's
 * hsw_sseu_info_init and its eDRAM probe. */
void i915_sseu_init(struct drm_i915_private *i915);

/** The engine of the class and instance programs name (I915_ENGINE_CLASS_),
 * or NULL, as Linux's intel_engine_lookup_user over the engines that run:
 * rcs0, bcs0, vcs0 and vecs0, each instance 0. */
struct i915_engine *i915_engine_lookup_user(struct drm_i915_private *i915, uint16_t cls,
                                            uint16_t instance);

/** The class programs give `engine`, I915_ENGINE_CLASS_RENDER, _COPY,
 * _VIDEO or _VIDEO_ENHANCE. */
uint16_t i915_engine_uabi_class(const struct i915_engine *engine);

/**
 * Begin a request on the state `ce`, as Linux's i915_request_create on a
 * legacy ring: the submission worker started, requests that completed
 * retired, room in the ring waited for (Linux's wait_for_space), the GT
 * unparked, the GT's own objects and the default state made with the
 * first, then the caches and TLBs invalidated and the chip switched to
 * `ce`'s state. Room for the breadcrumb is kept until `i915_request_add`,
 * which must follow before the caller sleeps. In `*out`, with one
 * reference, the caller's.
 *
 * @return 0; -EIO when the GPU is wedged or the ring refuses the commands;
 *         -EINTR in the wait for room; -ENOMEM; -ENOSPC without room in
 *         the GTT for an image.
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

/** Whether the chip has begun the request: it has signalled, or its engine
 * has passed the request before it, as Linux's i915_request_started. */
int i915_request_started(const struct i915_request *rq);

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

/**
 * Hold the request, which is being built, until `fence` has signalled, as
 * Linux's i915_request_await_dma_fence: each leaf of an array or a chain
 * that has not signalled and is not of the request's own engine, whose
 * ring runs in order, is awaited. With `exec`, a leaf that is a request of
 * this device on another engine is awaited only until it is given to the
 * chip, as Linux's i915_request_await_execution; any other leaf until it
 * signals. A leaf that signals with an error other than -EAGAIN and
 * -ETIMEDOUT gives the request that error, and its payload is not run.
 *
 * @return 0, or -ENOMEM.
 */
int32_t i915_request_await_fence(struct i915_request *rq, struct dma_fence *fence, int exec);

/**
 * Wait for every request of the device to complete and retire them, as
 * Linux's intel_gt_wait_for_idle with no timeout: interruptible by a
 * signal when `intr` is set. A request held by a fence waits with it.
 *
 * @return 0, or -EINTR.
 */
int32_t i915_gt_wait_for_idle(struct drm_i915_private *i915, int intr);

/**
 * The newest request of the state `ce` that has not completed and was
 * added at or before `before`, a dma_fence_now() time, with a reference,
 * or NULL: what THROTTLE waits for on that state, as Linux's walk of the
 * context's timeline. Does not sleep.
 */
struct i915_request *i915_request_throttle_target(struct intel_context *ce, int64_t before);

/** Give the chip each request whose last fence signalled outside the big
 * kernel lock, on every device, then run the retire worker's pass where it
 * is due or a last request signalled to a listener: the submission
 * worker's work, which a host test calls itself. Returns the engines it
 * moved. */
int i915_submit_work(void);

/** Retire every request whose breadcrumb the chip has written, oldest
 * first on each engine, and park the GT once no engine has a request on
 * its list, one being built, or one Linux would not yet have retired
 * (`unretired`). */
void i915_retire_requests(struct drm_i915_private *i915);

/** Retire every completed request as Linux's intel_gt_retire_requests
 * does, all of an engine's once its last request is complete, and park the
 * GT when that leaves every engine idle: the retire worker's pass, and
 * what a wait for the GT's idle and debugfs's DROP_RETIRE do. */
void i915_gt_retire_requests(struct drm_i915_private *i915);

/** The device is unplugged: every request not completed fails with -EIO,
 * all retire, and the GT's own states, scratch page and default state go.
 * Before the rings stop. */
void i915_requests_unplug(struct drm_i915_private *i915);

/** Set the error `err` on the request's fence unless it has signalled or
 * holds a fatal error already, as Linux's i915_request_set_error_once. */
void i915_request_set_error_once(struct i915_request *rq, int32_t err);

/** Fill the request's payload in the ring with MI_NOOP, so that only its
 * switch and breadcrumb run, as Linux's __i915_request_skip. */
void i915_request_skip(struct i915_request *rq);

/** Every request of every engine fails with -EIO, its fence signalled,
 * and retires, as Linux's reset_cancel when the GPU is wedged; the
 * commands and switches of those the chip was never given are taken back
 * first, so that each ring's tail and state are again what the chip has. */
void i915_requests_cancel(struct drm_i915_private *i915);

/*
 * Hang detection, reset and the error state (i915_hangcheck.c,
 * i915_reset.c, i915_gpu_error.c), after Linux 6.12's
 * gt/intel_engine_heartbeat.c, gt/intel_reset.c, gt/intel_ring_submission.c
 * and i915_gpu_error.c on Haswell. Every call is made under the big kernel
 * lock, in process context.
 */

/* Linux's heartbeat interval (CONFIG_DRM_I915_HEARTBEAT_INTERVAL), in ms. */
#define I915_HEARTBEAT_INTERVAL_MS 2500u

/** Start the heartbeat as the GT unparks: the first beat an interval
 * away, every engine unarmed. */
void i915_hangcheck_unpark(struct drm_i915_private *i915);

/**
 * Beat once if the heartbeat is due, as Linux's heartbeat worker on a
 * legacy ring: an armed engine whose request of the last beat has not
 * completed is hung, and each such engine is reset (`i915_handle_error`);
 * an engine that has requests on the chip is armed with its last. The
 * next beat is an interval away.
 *
 * @return the engines found hung, as a mask of 1 << index.
 */
uint32_t i915_hangcheck(struct drm_i915_private *i915);

/**
 * Recover the engines of `engine_mask` (1 << index) from a hang, as
 * Linux's intel_gt_handle_error with I915_ERROR_CAPTURE: the error state
 * captured and the error registers cleared, each engine reset alone, and
 * the whole GPU reset for those whose reset failed. `msg` says why, for
 * the log.
 */
void i915_handle_error(struct drm_i915_private *i915, uint32_t engine_mask, const char *msg);

/**
 * Reset `engine` alone through its domain of GEN6_GDRST, as Linux's
 * __intel_engine_reset_bh: the ring stopped, the domain reset with a
 * bounded wait, the first request not completed marked guilty, its payload
 * skipped and its context perhaps banned, and the ring started again from
 * that request's head, so that the requests behind it run.
 *
 * @return 0, also for an engine with nothing to run; -EIO for a ring that
 *         is not running or would not start again; -ETIMEDOUT when the
 *         domain's reset did not finish in 2 ms.
 */
int32_t i915_reset_engine(struct drm_i915_private *i915, struct i915_engine *engine,
                          const char *msg);

/**
 * Reset the whole GPU through GEN6_GDRST's full domain, as Linux's
 * intel_gt_reset: every ring stopped, the reset tried four times, the
 * fences and the swizzle written again, each engine's first request not
 * completed marked guilty when its bit is in `stalled_mask` and innocent
 * otherwise, and every ring started again. A reset that fails wedges the
 * GPU (`i915_gt_set_wedged`).
 */
void i915_reset_gt(struct drm_i915_private *i915, uint32_t stalled_mask, const char *msg);

/** Wedge the GPU, as Linux's __intel_gt_set_wedged: every ring stopped,
 * every request failed with -EIO, and new requests and contexts refused
 * with -EIO until a reset succeeds. */
void i915_gt_set_wedged(struct drm_i915_private *i915);

/**
 * Stop every ring by force and reset the whole GPU, leaving the rings
 * stopped, as Linux's intel_engines_release does before a driver gives the
 * rings' pages back, so that the chip no longer reads them: the forced
 * stop of each running ring, then the full domain of GEN6_GDRST, tried
 * four times. The requests the chip had not completed are lost; the caller
 * fails them (`i915_requests_unplug`). Does not sleep.
 *
 * @return 0; -EIO without forcewake; -ETIMEDOUT when the GPU did not
 *         reset, which wedges it, as a failed reset does.
 */
int32_t i915_gt_quiesce(struct drm_i915_private *i915);

/* The most incomplete requests of an engine the error state keeps. */
#define I915_ERROR_REQUESTS 8

/* The subslices of Haswell GT2's one slice, whose sampler and row
 * INSTDONE the error state keeps. */
#define I915_ERROR_SUBSLICES 2

/*
 * One engine in the error state, Linux's intel_engine_coredump: whether
 * it hung, its registers as engine_record_registers reads them on graphics
 * 7, with the render engine's slice, sampler and row INSTDONE, the
 * breadcrumb and the last sequence number emitted, its resets so far, and
 * its requests on the chip not yet completed, oldest first, each with its
 * context's id and process, its ring offsets and the batch it starts, the
 * batch's head. The first of those requests is the active one, whose
 * head, breadcrumb and tail are `rq_head`, `rq_post` and `rq_tail`, and
 * whose context is named by `comm` and `pid`, with its `guilty` and
 * `active` counts. `hws` and `ring` are copies of the status page and of
 * the whole ring, at `hws_ggtt` and `ring_ggtt`, as Linux's coredump keeps
 * their "HW Status" and "ring" objects.
 */
struct i915_error_request {
    uint32_t seqno;
    uint32_t ctx_id;
    uint32_t pid;
    uint32_t head;
    uint32_t tail;
    uint32_t batch;
};

struct i915_error_engine {
    uint8_t hung;
    uint32_t start, head, tail, ctl, mode;
    uint32_t acthd, ipehr, ipeir, instdone, instps, instpm;
    uint32_t sc_instdone;
    uint32_t sampler_instdone[I915_ERROR_SUBSLICES];
    uint32_t row_instdone[I915_ERROR_SUBSLICES];
    uint32_t esr, faddr, bbaddr, bbstate, ccid, rc_psmi, fault_reg, hws_pga;
    uint32_t gfx_mode, pp_dir_base;
    uint32_t hws_seqno;
    uint32_t seqno;
    uint32_t reset_count;
    uint32_t rq_head, rq_post, rq_tail;
    char comm[16];
    uint32_t pid;
    uint32_t guilty, active;
    uint32_t hws_ggtt, ring_ggtt;
    uint32_t hws[I915_GTT_PAGE_SIZE / 4u];
    uint32_t ring[I915_RING_SIZE / 4u];
    uint32_t nrequests;
    struct i915_error_request rq[I915_ERROR_REQUESTS];
};

/*
 * The GPU's state at a hang, Linux's i915_gpu_coredump: when, in ticks,
 * which engines hung, why, the message Linux logs ("GPU HANG: ecode
 * 7:M:E", with the first hung request's process), empty for a state not
 * kept, the whole GPU's resets so far, whether the GT was awake, the
 * parameters in force, the GT's error and interrupt registers, the fences,
 * and each engine's state.
 */
struct i915_gpu_error {
    uint64_t ticks;
    uint32_t engine_mask;
    char reason[80];
    char msg[96];
    uint32_t reset_count;
    uint8_t awake;
    struct i915_params params;
    uint32_t eir, pgtbl_er, error_gen6, done_reg;
    uint32_t gtier, forcewake, err_int, derrmr;
    uint64_t fence[I915_NUM_FENCES];
    struct i915_error_engine engine[I915_NUM_ENGINES];
};

/**
 * Capture the GPU's state now, as Linux's i915_gpu_coredump, with the
 * engines of `engine_mask` that have an incomplete request on the chip
 * marked hung and `reason` kept, but no message
 * made and nothing kept by the device. The caller owns the state and
 * frees it with `kfree`.
 *
 * @return the state, or NULL when memory is short.
 */
struct i915_gpu_error *i915_gpu_coredump(struct drm_i915_private *i915, uint32_t engine_mask,
                                         const char *reason);

/**
 * Capture the GPU's state for a hang of the engines `engine_mask`, as
 * Linux's i915_capture_error_state, and log its message. The device keeps
 * the first state until `i915_gpu_error_clear`; a later one is logged and
 * dropped.
 */
void i915_gpu_error_capture(struct drm_i915_private *i915, uint32_t engine_mask,
                            const char *reason);

/** Free the kept error state, as Linux's i915_reset_error_state. */
void i915_gpu_error_clear(struct drm_i915_private *i915);

/**
 * Count a hang that `ce`'s request caused, at the tick `now`, as Linux's
 * mark_guilty: the context's guilty count raised, and, when it is
 * bannable, its hang recorded, the context banned when it is not
 * recoverable or this is its third hang within 120 s, and its file's ban
 * score raised. A closed context is banned at once and counts nothing.
 *
 * @return whether `ce` is to be banned.
 */
int i915_gem_context_mark_guilty(struct intel_context *ce, uint64_t now);

/** Count a request of `ce` lost in a reset it did not cause, as Linux's
 * mark_innocent. */
void i915_gem_context_mark_innocent(struct intel_context *ce);

/**
 * Make the driver's files in the device's debugfs directory, Linux's
 * i915_debugfs_register with i915_gpu_error_debugfs_register and
 * intel_cdclk_debugfs_register as far as IGT's tests use them (the
 * Readme's debugfs section), and the error state's `error` in its card's
 * directory in /sys, as Linux's i915_setup_error_capture. After the
 * device is registered; the files go with its directories at the unplug.
 */
void i915_debugfs_register(struct drm_i915_private *i915);

/* ---- i915_handoff.c ---- */

/*
 * The release of a bound device to another driver and its bring-up again
 * from a reset, through /sys/bus/pci/drivers/i915/unbind and bind, which
 * i915_handoff.c tells whole. The bind and the unplug tell it of each
 * device that comes and goes.
 */

/** A device is bound and registered: make the driver's directory of /sys
 * if it is not there, and show the driver on the function as Linux's
 * driver core does (`sysfs_pci_bind`): its `driver` link, the directory's
 * link to it and DRIVER= in its uevent, which libudev, IGT's device scan
 * and Mesa's loader read. From the bind, under the big lock. */
void i915_handoff_bound(struct drm_i915_private *i915);

/** A device is unplugged: take that binding away. Nothing for a device
 * `i915_handoff_bound` was not told of. */
void i915_handoff_unbound(struct drm_i915_private *i915);

#endif /* _I915_H_ */
