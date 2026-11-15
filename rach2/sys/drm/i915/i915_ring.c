/*
 * i915_ring.c: Haswell's render, blitter, video and video enhancement
 * rings: their status pages and ring buffers in the global GTT, ring start
 * and stop, commands, and the breadcrumbs the chip writes and the user
 * interrupt that wakes their waiters, after Linux 6.12's
 * gt/intel_ring_submission.c, gt/intel_ring.c and gt/intel_breadcrumbs.c.
 */

#include "i915.h"
#include "i915_drm.h"
#include "i915_reg.h"

#include "../../arch/arch.h"
#include "../../klog.h"
#include "../../lib/kerrno.h"
#include "../../lib/kstring.h"
#include "../../mm/kheap.h"
#include "../../mm/paging.h"
#include "../../proc/percpu.h"
#include "../../rtc.h"

/* What tells one engine from another (intel_engine_cs.c's intel_engines,
 * intel_ring_submission.c's setup_rcs, setup_bcs, setup_vcs and setup_vecs,
 * and intel_reset.c's engine reset domains), by index. */
static const struct {
    const char *name;
    uint32_t mmio_base;
    uint32_t hws_pga;
    uint32_t irq_bit;
    int irq_bank;
    uint16_t uabi_class;
    uint8_t linux_id;
    uint32_t reset_domain;
    uint32_t fault_reg;
} i915_engine_info[I915_NUM_ENGINES] = {
    {"rcs0", RENDER_RING_BASE, RENDER_HWS_PGA_GEN7, GT_RENDER_USER_INTERRUPT, I915_IRQ_GT,
     I915_ENGINE_CLASS_RENDER, 0, GEN6_GRDOM_RENDER, RING_FAULT_REG_RCS},
    {"bcs0", BLT_RING_BASE, BLT_HWS_PGA_GEN7, GT_BLT_USER_INTERRUPT, I915_IRQ_GT,
     I915_ENGINE_CLASS_COPY, 1, GEN6_GRDOM_BLT, RING_FAULT_REG_BCS},
    {"vcs0", GEN6_BSD_RING_BASE, BSD_HWS_PGA_GEN7, GT_BSD_USER_INTERRUPT, I915_IRQ_GT,
     I915_ENGINE_CLASS_VIDEO, 10, GEN6_GRDOM_MEDIA, RING_FAULT_REG_VCS},
    {"vecs0", VEBOX_RING_BASE, VEBOX_HWS_PGA_GEN7, PM_VEBOX_USER_INTERRUPT, I915_IRQ_PM,
     I915_ENGINE_CLASS_VIDEO_ENHANCE, 18, GEN6_GRDOM_VECS, RING_FAULT_REG_VECS}};

/* Linux's bounds for the stop (1 ms fast and 100 ms slow, taken as one),
 * the TLB flush and the start (gt/intel_engine_cs.c, intel_ring_submission.c),
 * and the driver's own for the room in a ring. */
#define I915_STOP_US       100000u
#define I915_TLB_FLUSH_US  2000u
#define I915_START_US      5000u
#define I915_RING_SPACE_US 10000u

int32_t i915_gt_ggtt_take(struct drm_i915_private *i915, uint32_t bytes, uint32_t *offset)
{
    struct i915_gt *gt = &i915->gt;
    const struct i915_ggtt *ggtt = &i915->ggtt;
    uint64_t size =
        ((uint64_t)bytes + I915_GTT_PAGE_SIZE - 1) & ~(uint64_t)(I915_GTT_PAGE_SIZE - 1);
    const struct i915_vma *vma;
    uint64_t start;
    int i;

    if (size == 0 || size >= gt->ggtt_bottom) {
        return -ENOSPC;
    }
    start = gt->ggtt_bottom - size;
    for (i = 0; i < ggtt->nkept; i++) {
        if (start < ggtt->kept[i].end && gt->ggtt_bottom > ggtt->kept[i].start) {
            return -ENOSPC;
        }
    }
    /* Nor the space a buffer's binding holds. */
    for (vma = i915->gem.bound; vma; vma = vma->ggtt_next) {
        if (start < vma->start + vma->node_size && gt->ggtt_bottom > vma->start) {
            return -ENOSPC;
        }
    }
    gt->ggtt_bottom = start;
    *offset = (uint32_t)start;
    return 0;
}

void *i915_gt_pages_map(struct drm_i915_private *i915, uint32_t npages, int cache, uint32_t *ggtt)
{
    uint64_t *pages;
    uint8_t *v;
    uint32_t i, offset;

    v = (uint8_t *)kalloc_pages(npages, 0);
    if (!v) {
        return 0;
    }
    pages = (uint64_t *)kmalloc(npages * sizeof(*pages));
    if (!pages) {
        kfree_pages(v, npages);
        return 0;
    }
    k_memset(v, 0, (size_t)npages * I915_GTT_PAGE_SIZE);
    for (i = 0; i < npages; i++) {
        pages[i] = paging_virt_to_phys((uintptr_t)(v + (size_t)i * I915_GTT_PAGE_SIZE));
        if (pages[i] == 0) {
            break;
        }
    }
    if (i < npages || i915_gt_ggtt_take(i915, npages * I915_GTT_PAGE_SIZE, &offset) < 0) {
        kfree(pages);
        kfree_pages(v, npages);
        return 0;
    }
    if (i915_ggtt_insert(i915, offset, pages, npages, cache) < 0) {
        /* The space was the last taken, so it is given back. */
        i915->gt.ggtt_bottom += (uint64_t)npages * I915_GTT_PAGE_SIZE;
        kfree(pages);
        kfree_pages(v, npages);
        return 0;
    }
    kfree(pages);
    *ggtt = offset;
    return v;
}

void i915_gt_pages_unmap(struct drm_i915_private *i915, void *vaddr, uint32_t npages, uint32_t ggtt)
{
    if (!vaddr) {
        return;
    }
    (void)i915_ggtt_clear_range(i915, ggtt, (uint64_t)npages * I915_GTT_PAGE_SIZE, 0);
    if (ggtt == i915->gt.ggtt_bottom) {
        i915->gt.ggtt_bottom += (uint64_t)npages * I915_GTT_PAGE_SIZE;
    }
    kfree_pages(vaddr, npages);
}

/* ------------------------------------------------------------------------
 * Workarounds
 */

/*
 * One register of a workaround list, as Linux's struct i915_wa: the bits
 * cleared and set, and in `read` the bits a read back must show as `set`
 * has them, 0 for a register that is not read back. A masked register has
 * `clr` 0 and is written whole; any other is read first and written only
 * when the write would change it. A list is in the order of its
 * registers, as Linux keeps one.
 */
struct i915_wa {
    uint32_t reg;
    uint32_t clr;
    uint32_t set;
    uint32_t read;
};

/* Haswell's GT workarounds, Linux's hsw_gt_workarounds_init: the vertex
 * shader's reference count left to the hardware, and L3's atomics off in
 * the data port and in the rows, which a program turns on through the
 * command parser's two registers. Linux does not read the row's back. */
static const struct i915_wa hsw_gt_was[] = {
    {GEN7_FF_THREAD_MODE, GEN7_FF_VS_REF_CNT_FFME, 0, GEN7_FF_VS_REF_CNT_FFME},
    {HSW_SCRATCH1, 0xffffffffu, HSW_SCRATCH1_L3_DATA_ATOMICS_DISABLE, 0xffffffffu},
    {HSW_ROW_CHICKEN3, 0, I915_MASKED_ENABLE(HSW_ROW_CHICKEN3_L3_GLOBAL_ATOMICS_DISABLE), 0}};

/* The render engine's workarounds on Haswell, Linux's rcs_engine_wa_init:
 * WaDisableAsyncFlipPerfMode, WaBCSVCSTlbInvalidationMode, the HiZ raw
 * stall optimization on and WaDisable_RenderCache_OperationalFlush,
 * WaDisable4x2SubspanOptimization, 16x4 WIZ hashing and
 * WaSampleCChickenBitEnable. The other engines have none. */
static const struct i915_wa hsw_rcs_was[] = {
    {RING_MI_MODE(RENDER_RING_BASE), 0, I915_MASKED_ENABLE(ASYNC_FLIP_PERF_DISABLE),
     ASYNC_FLIP_PERF_DISABLE},
    {RING_MODE_GEN7(RENDER_RING_BASE), 0,
     I915_MASKED_ENABLE(GFX_TLB_INVALIDATE_EXPLICIT | GFX_REPLAY_MODE),
     GFX_TLB_INVALIDATE_EXPLICIT | GFX_REPLAY_MODE},
    {CACHE_MODE_0_GEN7, 0, I915_MASKED_DISABLE(HIZ_RAW_STALL_OPT_DISABLE | RC_OP_FLUSH_ENABLE),
     HIZ_RAW_STALL_OPT_DISABLE | RC_OP_FLUSH_ENABLE},
    {CACHE_MODE_1, 0, I915_MASKED_ENABLE(PIXEL_SUBSPAN_COLLECT_OPT_DISABLE),
     PIXEL_SUBSPAN_COLLECT_OPT_DISABLE},
    {GEN7_GT_MODE, 0, GEN6_WIZ_HASHING_MASK << 16 | GEN6_WIZ_HASHING_16x4, GEN6_WIZ_HASHING_MASK},
    {HSW_HALF_SLICE_CHICKEN3, 0, I915_MASKED_ENABLE(HSW_SAMPLE_C_PERFORMANCE),
     HSW_SAMPLE_C_PERFORMANCE}};

/**
 * Write the `n` workarounds of `wa`, as Linux's wa_list_apply, and read
 * each back as its wa_verify does: one the chip did not keep is logged
 * under the list's `name` and counted in `gt.wa_lost`. Under forcewake.
 */
static void i915_wa_list_apply(struct drm_i915_private *i915, const struct i915_wa *wa, unsigned n,
                               const char *name)
{
    struct i915_hw *hw = &i915->hw;
    uint32_t old, val, cur;
    unsigned i;

    for (i = 0; i < n; i++, wa++) {
        old = wa->clr ? i915_read(hw, wa->reg) : 0;
        val = (old & ~wa->clr) | wa->set;
        if (val != old || !wa->clr) {
            i915_write(hw, wa->reg, val);
        }
        if (!wa->read) {
            continue;
        }
        cur = i915_read(hw, wa->reg);
        if ((cur ^ wa->set) & wa->read) {
            klog(KLOG_ERR, "i915",
                 "workarounds: %s: lost: register 0x%x reads 0x%08x, expected 0x%08x of 0x%08x",
                 name, (unsigned)wa->reg, (unsigned)cur, (unsigned)(wa->set & wa->read),
                 (unsigned)wa->read);
            i915->gt.wa_lost++;
        }
    }
}

void i915_gt_init_hw(struct drm_i915_private *i915)
{
    struct i915_hw *hw = &i915->hw;
    int gt3 = i915->id && i915->id->platform->gt == 3;

    if (i915_read(hw, HSW_EDRAM_CAP) & EDRAM_ENABLED) {
        (void)i915_rmw(hw, HSW_IDICR, 0, IDIHASHMSK(0xf));
    }
    i915_write(hw, HSW_MI_PREDICATE_RESULT_2, gt3 ? LOWER_SLICE_ENABLED : LOWER_SLICE_DISABLED);
    i915_wa_list_apply(i915, hsw_gt_was, sizeof(hsw_gt_was) / sizeof(hsw_gt_was[0]), "gt");
}

/**
 * Write the GT's registers of Linux's hsw_init_clock_gating, which it
 * writes once, at load: the message channel's interrupt mode that
 * WaCatErrorRejectionIssue needs, and the arbitration priority of stream
 * output over the vertex fetch, WaSwitchSolVfFArbitrationPriority. The
 * display's registers of that function are the display's to write
 * (intel_display.c). Under forcewake. A runtime resume writes them again,
 * as Linux's hsw_disable_pc8 does through intel_clock_gating_init.
 */
void i915_gt_init_clock_gating(struct drm_i915_private *i915)
{
    (void)i915_rmw(&i915->hw, GEN7_SQ_CHICKEN_MBCUNIT_CONFIG, 0, GEN7_SQ_CHICKEN_MBCUNIT_SQINTMOB);
    (void)i915_rmw(&i915->hw, GAM_ECOCHK, 0, HSW_ECOCHK_ARB_PRIO_SOL);
}

/**
 * Stop the command streamer of `e` and empty its ring, as Linux's
 * intel_engine_stop_cs and stop_ring: STOP_RING set and MODE_IDLE waited
 * for, a timeout forgiven when the head has reached the tail, as Linux
 * does, or always when `force` is set, as before a reset, where a hung
 * ring never idles; then the head moved to the tail, the ring disabled,
 * and the head and tail cleared. Under forcewake.
 *
 * @return 0, or -EIO when the streamer did not stop or the head would not
 *         clear.
 */
static int32_t i915_ring_stop(struct drm_i915_private *i915, struct i915_engine *e, int force)
{
    struct i915_hw *hw = &i915->hw;
    uint32_t base = e->mmio_base;
    int tries;

    i915_write(hw, RING_MI_MODE(base), I915_MASKED_ENABLE(STOP_RING));
    if (i915_wait_for(hw, "ring stop", RING_MI_MODE(base), MODE_IDLE, MODE_IDLE, I915_STOP_US) <
            0 &&
        (i915_read(hw, RING_HEAD(base)) & HEAD_ADDR) !=
            (i915_read(hw, RING_TAIL(base)) & TAIL_ADDR) &&
        !force) {
        klog(KLOG_ERR, "i915", "%s: did not stop: head %08x tail %08x", e->name,
             (unsigned)i915_read(hw, RING_HEAD(base)), (unsigned)i915_read(hw, RING_TAIL(base)));
        return -EIO;
    }
    i915_posting_read(hw, RING_HEAD(base));

    /* Twice, as Linux tries again for chips whose head did not clear. */
    for (tries = 0; tries < 2; tries++) {
        i915_write(hw, RING_HEAD(base), i915_read(hw, RING_TAIL(base)));
        i915_posting_read(hw, RING_HEAD(base));
        i915_write(hw, RING_CTL(base), 0);
        i915_posting_read(hw, RING_CTL(base));
        i915_write(hw, RING_HEAD(base), 0);
        i915_write(hw, RING_TAIL(base), 0);
        if ((i915_read(hw, RING_HEAD(base)) & HEAD_ADDR) == 0) {
            return 0;
        }
    }
    klog(KLOG_ERR, "i915", "%s: the head would not clear: head %08x", e->name,
         (unsigned)i915_read(hw, RING_HEAD(base)));
    return -EIO;
}

int32_t i915_ring_resume(struct drm_i915_private *i915, struct i915_engine *e, uint32_t head,
                         uint32_t tail)
{
    struct i915_hw *hw = &i915->hw;
    uint32_t base = e->mmio_base;
    int32_t err;

    /* intel_engine_resume: the engine's workarounds first, which a reset of
     * its domain loses. */
    if (base == RENDER_RING_BASE) {
        i915_wa_list_apply(i915, hsw_rcs_was, sizeof(hsw_rcs_was) / sizeof(hsw_rcs_was[0]),
                           e->name);
    }
    err = i915_ring_stop(i915, e, 0);
    if (err < 0) {
        return err;
    }
    /* set_hwstam: the render ring keeps bit 0 unmasked. */
    i915_write(hw, RING_HWSTAM(base), e->mmio_base == RENDER_RING_BASE ? ~1u : 0xffffffffu);
    i915_write(hw, RING_IMR(base), 0xffffffffu);

    /* ring_setup_status_page: set_hwsp, then flush_cs_tlb. */
    i915_write(hw, e->hws_pga, e->hws_ggtt);
    i915_posting_read(hw, e->hws_pga);
    if (!(i915_read(hw, RING_MI_MODE(base)) & MODE_IDLE)) {
        klog(KLOG_WARN, "i915", "%s: not idle before the TLB flush", e->name);
    }
    i915_write(hw, RING_INSTPM(base),
               I915_MASKED_ENABLE(INSTPM_TLB_INVALIDATE | INSTPM_SYNC_FLUSH));
    (void)i915_wait_for(hw, "ring tlb flush", RING_INSTPM(base), INSTPM_SYNC_FLUSH, 0,
                        I915_TLB_FLUSH_US);
    i915_posting_read(hw, RING_HEAD(base));

    i915_write(hw, RING_START(base), e->ring_ggtt);
    (void)i915_ppgtt_enable_ring(i915, e);
    i915_write(hw, RING_HEAD(base), head);
    i915_write(hw, RING_TAIL(base), head);
    i915_posting_read(hw, RING_TAIL(base));
    i915_write(hw, RING_CTL(base), (RING_CTL_SIZE(e->ring_size) & RING_NR_PAGES) | RING_VALID);
    if (i915_wait_for(hw, "ring start", RING_CTL(base), RING_VALID, RING_VALID, I915_START_US) <
        0) {
        klog(KLOG_ERR, "i915",
             "%s: initialization failed: ctl %08x head %08x tail %08x start %08x, expected %08x",
             e->name, (unsigned)i915_read(hw, RING_CTL(base)),
             (unsigned)i915_read(hw, RING_HEAD(base)), (unsigned)i915_read(hw, RING_TAIL(base)),
             (unsigned)i915_read(hw, RING_START(base)), (unsigned)e->ring_ggtt);
        return -EIO;
    }
    i915_write(hw, RING_MI_MODE(base), I915_MASKED_DISABLE(STOP_RING));
    e->head = head;
    e->hw_tail = head;
    e->running = 1;
    if (tail != head) {
        /* The commands reach memory before the chip reads the tail. */
        arch_dma_fence();
        i915_write(hw, RING_TAIL(base), tail);
        i915_posting_read(hw, RING_TAIL(base));
        e->hw_tail = tail;
    }
    return 0;
}

void i915_ring_reset_prepare(struct drm_i915_private *i915, struct i915_engine *e)
{
    (void)i915_ring_stop(i915, e, 1);
}

/**
 * Start the ring of `e` empty, as Linux's xcs_resume at load: stopped and
 * emptied, every write to the status page's interrupt dwords masked, the
 * status page's address written and the TLB flushed, the ring's start, the
 * PPGTT's registers, an empty ring enabled with RING_VALID and waited for,
 * then STOP_RING cleared. Under forcewake.
 *
 * @return 0, or -EIO with the wait that failed named.
 */
static int32_t i915_ring_start(struct drm_i915_private *i915, struct i915_engine *e)
{
    e->tail = 0;
    return i915_ring_resume(i915, e, 0, 0);
}

/** The user interrupt of the engine `data`: counted, its requests'
 * fences signalled through `irq_notify`, and its waiters woken, each of
 * which reads the status page itself. */
static void i915_ring_irq(struct drm_i915_private *i915, uint32_t iir, void *data)
{
    struct i915_engine *e = (struct i915_engine *)data;

    (void)iir;
    e->irq_seen++;
    if (e->irq_notify) {
        e->irq_notify(i915, e);
    }
    wq_wake_all(&e->wq);
}

/** Give `e` its status page and ring, mapped in the global GTT at the
 * LLC's cache level, as Linux's internal objects on an LLC part: 0 or
 * -ENOMEM. */
static int32_t i915_engine_alloc(struct drm_i915_private *i915, struct i915_engine *e)
{
    uint32_t ggtt;

    e->hws = (uint32_t *)i915_gt_pages_map(i915, 1, I915_CACHE_LLC, &ggtt);
    if (!e->hws) {
        return -ENOMEM;
    }
    e->hws_ggtt = ggtt;
    e->ring = (uint32_t *)i915_gt_pages_map(i915, I915_RING_SIZE / I915_GTT_PAGE_SIZE,
                                            I915_CACHE_LLC, &ggtt);
    if (!e->ring) {
        return -ENOMEM;
    }
    e->ring_ggtt = ggtt;
    e->ring_size = I915_RING_SIZE;
    return 0;
}

int32_t i915_gt_init(struct drm_i915_private *i915)
{
    struct i915_gt *gt = &i915->gt;
    int32_t err = 0;
    int i;

    k_memset(gt, 0, sizeof(*gt));
    gt->init_ticks = rtc_ticks_now();
    gt->ggtt_top = gt->ggtt_bottom = i915->ggtt.total;
    for (i = 0; i < I915_NUM_ENGINES; i++) {
        struct i915_engine *e = &gt->engine[i];
        e->name = i915_engine_info[i].name;
        e->mmio_base = i915_engine_info[i].mmio_base;
        e->hws_pga = i915_engine_info[i].hws_pga;
        e->irq_bit = i915_engine_info[i].irq_bit;
        e->irq_bank = i915_engine_info[i].irq_bank;
        e->uabi_class = i915_engine_info[i].uabi_class;
        e->linux_id = i915_engine_info[i].linux_id;
        e->reset_domain = i915_engine_info[i].reset_domain;
        e->fault_reg = i915_engine_info[i].fault_reg;
        wait_queue_init(&e->wq);
    }
    for (i = 0; i < I915_NUM_ENGINES && err == 0; i++) {
        err = i915_engine_alloc(i915, &gt->engine[i]);
    }
    if (err < 0) {
        klog(KLOG_ERR, "i915", "rings: no memory for the status pages and rings");
        i915_gt_fini(i915);
        return err;
    }
    err = i915_ppgtt_init(i915);
    if (err < 0) {
        i915_gt_fini(i915);
        return err;
    }
    if (i915_forcewake_get(i915) < 0) {
        i915_gt_fini(i915);
        return -EIO;
    }
    i915_gt_init_clock_gating(i915);
    i915_gt_init_hw(i915);
    for (i = 0; i < I915_NUM_ENGINES && err == 0; i++) {
        err = i915_ring_start(i915, &gt->engine[i]);
    }
    i915_forcewake_put(i915);
    if (err < 0) {
        i915_gt_fini(i915);
        return err;
    }
    for (i = 0; i < I915_NUM_ENGINES; i++) {
        struct i915_engine *e = &gt->engine[i];
        (void)i915_irq_register(i915, e->irq_bank, e->irq_bit, i915_ring_irq, e);
    }
    klog(KLOG_INFO, "i915",
         "rings: rcs0, bcs0, vcs0 and vecs0 started: %u KiB each at 0x%x, 0x%x, 0x%x and 0x%x, "
         "status pages at 0x%x, 0x%x, 0x%x and 0x%x",
         (unsigned)(I915_RING_SIZE / 1024), (unsigned)gt->engine[I915_RCS0].ring_ggtt,
         (unsigned)gt->engine[I915_BCS0].ring_ggtt, (unsigned)gt->engine[I915_VCS0].ring_ggtt,
         (unsigned)gt->engine[I915_VECS0].ring_ggtt, (unsigned)gt->engine[I915_RCS0].hws_ggtt,
         (unsigned)gt->engine[I915_BCS0].hws_ggtt, (unsigned)gt->engine[I915_VCS0].hws_ggtt,
         (unsigned)gt->engine[I915_VECS0].hws_ggtt);
    klog(KLOG_INFO, "i915", "workarounds: applied: %u of the GT and %u of rcs0, %u lost",
         (unsigned)(sizeof(hsw_gt_was) / sizeof(hsw_gt_was[0])),
         (unsigned)(sizeof(hsw_rcs_was) / sizeof(hsw_rcs_was[0])), (unsigned)gt->wa_lost);
    return 0;
}

void i915_gt_fini(struct drm_i915_private *i915)
{
    struct i915_gt *gt = &i915->gt;
    int fw = i915_forcewake_get(i915) == 0;
    int i;

    for (i = 0; i < I915_NUM_ENGINES; i++) {
        struct i915_engine *e = &gt->engine[i];

        if (e->running) {
            i915_irq_unregister(i915, e->irq_bank, e->irq_bit);
            i915_write(&i915->hw, RING_IMR(e->mmio_base), 0xffffffffu);
            (void)i915_ring_stop(i915, e, 0);
            e->running = 0;
            e->irq_refs = 0;
            e->irq_armed = 0;
        }
        i915_gt_pages_unmap(i915, e->ring, e->ring_size / I915_GTT_PAGE_SIZE, e->ring_ggtt);
        e->ring = 0;
        i915_gt_pages_unmap(i915, e->hws, 1, e->hws_ggtt);
        e->hws = 0;
    }
    i915_ppgtt_fini(i915);
    if (fw) {
        i915_forcewake_put(i915);
    }
    gt->ggtt_bottom = gt->ggtt_top;
}

/** The free bytes of a ring of `size` bytes between `tail` and `head`,
 * the gap kept out, as Linux's __intel_ring_space. */
static uint32_t i915_ring_space(uint32_t head, uint32_t tail, uint32_t size)
{
    return (head - tail - I915_RING_GAP) & (size - 1u);
}

/* What a wait for room in a ring needs. */
struct i915_ring_room {
    struct drm_i915_private *i915;
    struct i915_engine *e;
    uint32_t bytes;
};

/** Whether the chip's head has moved far enough to give the room. */
static int i915_ring_has_room(struct i915_hw *hw, void *arg)
{
    struct i915_ring_room *r = (struct i915_ring_room *)arg;

    (void)hw;
    r->e->head = i915_uncore_read(r->i915, RING_HEAD(r->e->mmio_base)) & HEAD_ADDR;
    return i915_ring_space(r->e->head, r->e->tail, r->e->ring_size) >= r->bytes;
}

uint32_t *i915_ring_begin(struct i915_engine *e, struct drm_i915_private *i915, uint32_t ndw)
{
    struct i915_ring_room room;
    uint32_t bytes = ndw * 4u, remain, need, *cs, i;

    if (!e->running || (ndw & 1u) || ndw == 0 || bytes > e->ring_size / 2u ||
        i915_hw_fault(&i915->hw, I915_FAULT_SUBMIT) < 0) {
        return 0;
    }
    /* The room asked for and the bytes kept for the open request's
     * breadcrumb, which, when they do not fit before the end, need the
     * wrap as well, as Linux's intel_ring_begin. */
    remain = e->ring_size - e->tail;
    need = bytes + e->reserved;
    if (need > remain) {
        need = bytes > remain ? need + remain : e->reserved + remain;
    }
    if (i915_ring_space(e->head, e->tail, e->ring_size) < need) {
        room.i915 = i915;
        room.e = e;
        room.bytes = need;
        if (i915_wait_until(&i915->hw, "ring space", i915_ring_has_room, &room,
                            I915_RING_SPACE_US) < 0) {
            return 0;
        }
    }
    if (bytes > remain) {
        cs = e->ring + e->tail / 4u;
        for (i = 0; i < remain / 4u; i++) {
            cs[i] = MI_NOOP;
        }
        e->tail = 0;
    }
    cs = e->ring + e->tail / 4u;
    e->tail = (e->tail + bytes) & (e->ring_size - 1u);
    return cs;
}

void i915_ring_advance(struct i915_engine *e, struct drm_i915_private *i915)
{
    /* The commands reach memory before the chip reads the tail. */
    arch_dma_fence();
    i915_uncore_write(i915, RING_TAIL(e->mmio_base), e->tail);
    e->hw_tail = e->tail;
}

uint32_t i915_engine_emit_breadcrumb(struct drm_i915_private *i915, struct i915_engine *e)
{
    uint32_t *cs = i915_ring_begin(e, i915, 4), seqno;

    if (!cs) {
        return 0;
    }
    seqno = e->seqno == 0xffffffffu ? 1u : e->seqno + 1u; /* 0 is never a seqno */
    cs[0] = MI_STORE_DWORD_INDEX;
    cs[1] = I915_GEM_HWS_SEQNO_ADDR;
    cs[2] = seqno;
    cs[3] = MI_USER_INTERRUPT;
    e->seqno = seqno;
    i915_ring_advance(e, i915);
    return seqno;
}

int i915_engine_passed(const struct i915_engine *e, uint32_t seqno)
{
    /* The chip writes the page behind the compiler's back. */
    return e->hws && (int32_t)(((volatile uint32_t *)e->hws)[I915_GEM_HWS_SEQNO] - seqno) >= 0;
}

void i915_engine_irq_get(struct drm_i915_private *i915, struct i915_engine *e)
{
    if (e->irq_refs++ == 0) {
        /* The ring's own mask before its bank's, as gen6_irq_enable and
         * hsw_irq_enable_vecs. */
        i915_uncore_write(i915, RING_IMR(e->mmio_base), ~e->irq_bit);
        (void)i915_uncore_read(i915, RING_IMR(e->mmio_base));
        i915_irq_unmask(i915, e->irq_bank, e->irq_bit);
    }
}

void i915_engine_irq_put(struct drm_i915_private *i915, struct i915_engine *e)
{
    if (e->irq_refs == 0) {
        return;
    }
    if (--e->irq_refs == 0) {
        i915_uncore_write(i915, RING_IMR(e->mmio_base), 0xffffffffu);
        i915_irq_mask(i915, e->irq_bank, e->irq_bit);
    }
}

/* What a waiter for a breadcrumb needs. */
struct i915_waiter {
    struct i915_engine *e;
    uint32_t seqno;
    uint64_t deadline; /* rtc_ticks_now() at which the sleep ends */
};

/** A sleeping waiter's condition: the breadcrumb, or the deadline. */
static int i915_waiter_done(void *arg)
{
    struct i915_waiter *w = (struct i915_waiter *)arg;

    return i915_engine_passed(w->e, w->seqno) || rtc_ticks_now() >= w->deadline;
}

/** A polling waiter's condition: the breadcrumb. */
static int i915_waiter_polled(struct i915_hw *hw, void *arg)
{
    (void)hw;
    return i915_engine_passed(((struct i915_waiter *)arg)->e, ((struct i915_waiter *)arg)->seqno);
}

int32_t i915_engine_wait(struct drm_i915_private *i915, struct i915_engine *e, uint32_t seqno,
                         uint32_t timeout_us)
{
    struct i915_waiter w;
    uint64_t saved;

    if (i915_engine_passed(e, seqno)) {
        return 0;
    }
    if (!e->running) {
        return -EIO;
    }
    w.e = e;
    w.seqno = seqno;
    i915_engine_irq_get(i915, e);
    if (current_proc && i915->msi_vector >= 0) {
        /* A whole tick more than the timeout, so that a wait begun late
         * in a tick is never short. */
        w.deadline =
            rtc_ticks_now() + tick_div64((uint64_t)timeout_us + TICK_US - 1u, TICK_US, NULL) + 1u;
        saved = current_proc->wakeup_tick;
        current_proc->wakeup_tick = w.deadline;
        (void)wq_wait_event_nointr(&e->wq, i915_waiter_done, &w);
        current_proc->wakeup_tick = saved;
    } else {
        (void)i915_wait_until(&i915->hw, "breadcrumb", i915_waiter_polled, &w, timeout_us);
    }
    i915_engine_irq_put(i915, e);
    return i915_engine_passed(e, seqno) ? 0 : -ETIME;
}
