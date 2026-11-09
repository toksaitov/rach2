/*
 * i915_ring.c: Haswell's render and blitter rings: their status pages and
 * ring buffers in the global GTT, ring start and stop, commands, and the
 * breadcrumbs the chip writes and the user interrupt that wakes their
 * waiters, after Linux 6.12's gt/intel_ring_submission.c, gt/intel_ring.c
 * and gt/intel_breadcrumbs.c.
 */

#include "i915.h"
#include "i915_reg.h"

#include "../../arch/arch.h"
#include "../../klog.h"
#include "../../lib/kerrno.h"
#include "../../lib/kstring.h"
#include "../../mm/kheap.h"
#include "../../mm/paging.h"
#include "../../proc/percpu.h"
#include "../../rtc.h"

/* What tells one engine from another (intel_engine_cs.c's intel_engines
 * and intel_ring_submission.c's setup_rcs and setup_bcs). */
static const struct {
    const char *name;
    uint32_t mmio_base;
    uint32_t hws_pga;
    uint32_t irq_bit;
} i915_engine_info[I915_NUM_ENGINES] = {
    {"rcs0", RENDER_RING_BASE, RENDER_HWS_PGA_GEN7, GT_RENDER_USER_INTERRUPT},
    {"bcs0", BLT_RING_BASE, BLT_HWS_PGA_GEN7, GT_BLT_USER_INTERRUPT}};

/* The bytes between the tail and the head that stay free, so that a full
 * ring's tail never reaches its head (intel_ring.h, CACHELINE_BYTES). */
#define I915_RING_GAP 64u

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
    (void)i915_ggtt_clear_range(i915, ggtt, (uint64_t)npages * I915_GTT_PAGE_SIZE);
    if (ggtt == i915->gt.ggtt_bottom) {
        i915->gt.ggtt_bottom += (uint64_t)npages * I915_GTT_PAGE_SIZE;
    }
    kfree_pages(vaddr, npages);
}

/**
 * Stop the command streamer of `e` and empty its ring, as Linux's
 * intel_engine_stop_cs and stop_ring: STOP_RING set and MODE_IDLE waited
 * for, a timeout forgiven when the head has reached the tail, as Linux
 * does; then the head moved to the tail, the ring disabled, and the head
 * and tail cleared. Under forcewake.
 *
 * @return 0, or -EIO when the streamer did not stop or the head would not
 *         clear.
 */
static int32_t i915_ring_stop(struct drm_i915_private *i915, struct i915_engine *e)
{
    struct i915_hw *hw = &i915->hw;
    uint32_t base = e->mmio_base;
    int tries;

    i915_write(hw, RING_MI_MODE(base), I915_MASKED_ENABLE(STOP_RING));
    if (i915_wait_for(hw, "ring stop", RING_MI_MODE(base), MODE_IDLE, MODE_IDLE, I915_STOP_US) <
            0 &&
        (i915_read(hw, RING_HEAD(base)) & HEAD_ADDR) !=
            (i915_read(hw, RING_TAIL(base)) & TAIL_ADDR)) {
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

/**
 * Start the ring of `e`, as Linux's xcs_resume: stopped and emptied, every
 * write to the status page's interrupt dwords masked, the status page's
 * address written and the TLB flushed, the ring's start, the PPGTT's
 * registers, an empty ring enabled with RING_VALID and waited for, then
 * STOP_RING cleared. Under forcewake.
 *
 * @return 0, or -EIO with the wait that failed named.
 */
static int32_t i915_ring_start(struct drm_i915_private *i915, struct i915_engine *e)
{
    struct i915_hw *hw = &i915->hw;
    uint32_t base = e->mmio_base;
    int32_t err;

    err = i915_ring_stop(i915, e);
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
    i915_write(hw, RING_HEAD(base), 0);
    i915_write(hw, RING_TAIL(base), 0);
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
    e->head = e->tail = 0;
    e->running = 1;
    return 0;
}

/** The user interrupt of the engine `data`: counted, and its waiters
 * woken, each of which reads the status page itself. */
static void i915_ring_irq(struct drm_i915_private *i915, uint32_t iir, void *data)
{
    struct i915_engine *e = (struct i915_engine *)data;

    (void)i915;
    (void)iir;
    e->irq_seen++;
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
    gt->ggtt_top = gt->ggtt_bottom = i915->ggtt.total;
    for (i = 0; i < I915_NUM_ENGINES; i++) {
        struct i915_engine *e = &gt->engine[i];
        e->name = i915_engine_info[i].name;
        e->mmio_base = i915_engine_info[i].mmio_base;
        e->hws_pga = i915_engine_info[i].hws_pga;
        e->irq_bit = i915_engine_info[i].irq_bit;
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
    if (i915_forcewake_get(i915) < 0) {
        i915_gt_fini(i915);
        return -EIO;
    }
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
        (void)i915_irq_register(i915, I915_IRQ_GT, e->irq_bit, i915_ring_irq, e);
    }
    klog(KLOG_INFO, "i915",
         "rings: rcs0 and bcs0 started: %u KiB each at 0x%x and 0x%x, status pages at 0x%x "
         "and 0x%x",
         (unsigned)(I915_RING_SIZE / 1024), (unsigned)gt->engine[I915_RCS0].ring_ggtt,
         (unsigned)gt->engine[I915_BCS0].ring_ggtt, (unsigned)gt->engine[I915_RCS0].hws_ggtt,
         (unsigned)gt->engine[I915_BCS0].hws_ggtt);
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
            i915_irq_unregister(i915, I915_IRQ_GT, e->irq_bit);
            i915_write(&i915->hw, RING_IMR(e->mmio_base), 0xffffffffu);
            (void)i915_ring_stop(i915, e);
            e->running = 0;
            e->irq_refs = 0;
        }
        i915_gt_pages_unmap(i915, e->ring, e->ring_size / I915_GTT_PAGE_SIZE, e->ring_ggtt);
        e->ring = 0;
        i915_gt_pages_unmap(i915, e->hws, 1, e->hws_ggtt);
        e->hws = 0;
    }
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
    remain = e->ring_size - e->tail;
    need = bytes > remain ? bytes + remain : bytes;
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
}

uint32_t i915_engine_emit_breadcrumb(struct drm_i915_private *i915, struct i915_engine *e)
{
    uint32_t *cs = i915_ring_begin(e, i915, 4), seqno;

    if (!cs) {
        return 0;
    }
    seqno = e->seqno + 1u;
    if (seqno == 0) {
        seqno = 1;
    }
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
        /* The ring's own mask before the GT's, as gen6_irq_enable. */
        i915_uncore_write(i915, RING_IMR(e->mmio_base), ~e->irq_bit);
        (void)i915_uncore_read(i915, RING_IMR(e->mmio_base));
        i915_irq_unmask(i915, I915_IRQ_GT, e->irq_bit);
    }
}

void i915_engine_irq_put(struct drm_i915_private *i915, struct i915_engine *e)
{
    if (e->irq_refs == 0) {
        return;
    }
    if (--e->irq_refs == 0) {
        i915_uncore_write(i915, RING_IMR(e->mmio_base), 0xffffffffu);
        i915_irq_mask(i915, I915_IRQ_GT, e->irq_bit);
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
        w.deadline = rtc_ticks_now() + (timeout_us + 999u) / 1000u + 1u;
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
