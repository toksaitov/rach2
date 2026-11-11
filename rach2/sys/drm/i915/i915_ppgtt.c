/*
 * i915_ppgtt.c: Haswell's aliasing PPGTT, as Linux 6.12 runs it on a
 * chip of graphics version 7 (gt/gen6_ppgtt.c): one PPGTT that every
 * context shares, mapping the buffers' bindings at their global GTT
 * offsets.
 */

#include "i915.h"
#include "i915_reg.h"

#include "../../klog.h"
#include "../../lib/kerrno.h"
#include "../../lib/kstring.h"
#include "../../mm/kheap.h"
#include "../../mm/paging.h"

/** Fill the table `pt` with the scratch page's entry. */
static void ppgtt_fill_scratch(struct drm_i915_private *i915, uint32_t *pt)
{
    uint32_t i;

    for (i = 0; i < I915_PPGTT_PTES; i++) {
        pt[i] = i915->ggtt.scratch_pte;
    }
}

/** A zeroed page for a table and its address in `*phys`, or NULL. */
static uint32_t *ppgtt_alloc_table(uint64_t *phys)
{
    uint32_t *pt = (uint32_t *)kalloc_pages(1, 0);

    if (!pt) {
        return 0;
    }
    *phys = paging_virt_to_phys((uintptr_t)pt);
    if (*phys == 0) {
        kfree_pages(pt, 1);
        return 0;
    }
    return pt;
}

/**
 * Write the directory entries [first, last) into the global GTT's entries,
 * each naming its table or the scratch table, then read the last back and
 * flush the GTT's cache, as gen6_flush_pd.
 */
static void ppgtt_flush_pd(struct drm_i915_private *i915, uint32_t first, uint32_t last)
{
    struct i915_ppgtt *pp = &i915->gt.ppgtt;
    uint32_t base = pp->pd_ggtt / I915_GTT_PAGE_SIZE, pde;

    for (pde = first; pde < last; pde++) {
        uint64_t phys = pp->pt[pde] ? pp->pt_phys[pde] : pp->scratch_pt_phys;

        i915_gtt_write(&i915->hw, base + pde, GEN6_PDE_ADDR_ENCODE(phys) | GEN6_PDE_VALID);
    }
    (void)i915_gtt_read(&i915->hw, base + last - 1u);
    i915_ggtt_invalidate(i915);
}

int32_t i915_ppgtt_init(struct drm_i915_private *i915)
{
    struct i915_ppgtt *pp = &i915->gt.ppgtt;
    uint64_t bottom = i915->gt.ggtt_bottom;
    uint32_t pad, offset;
    int32_t err;

    k_memset(pp, 0, sizeof(*pp));
    pp->scratch_pt = ppgtt_alloc_table(&pp->scratch_pt_phys);
    if (!pp->scratch_pt) {
        klog(KLOG_ERR, "i915", "ppgtt: no page for the scratch table");
        return -ENOMEM;
    }
    ppgtt_fill_scratch(i915, pp->scratch_pt);

    /* PP_DIR_BASE holds the directory's offset among the entries in units
     * of 64 bytes, so it starts on a 64 KiB boundary of the GTT. */
    if (bottom < I915_PPGTT_PD_BYTES) {
        i915_ppgtt_fini(i915);
        return -ENOSPC;
    }
    pad = (uint32_t)((bottom - I915_PPGTT_PD_BYTES) & (I915_PPGTT_PD_ALIGN - 1u));
    err = i915_gt_ggtt_take(i915, I915_PPGTT_PD_BYTES + pad, &offset);
    if (err < 0) {
        klog(KLOG_ERR, "i915", "ppgtt: no room in the GTT for the directory");
        i915_ppgtt_fini(i915);
        return err;
    }
    pp->pd_ggtt = offset;
    pp->pd_take = I915_PPGTT_PD_BYTES + pad;
    pp->pp_dir = (offset / I915_GTT_PAGE_SIZE * 4u) << 10;
    ppgtt_flush_pd(i915, 0, I915_PPGTT_PDES);
    pp->ready = 1;
    klog(KLOG_INFO, "i915", "ppgtt: aliasing, %lu MiB shared, directory at 0x%x",
         (unsigned long)(I915_PPGTT_SIZE >> 20), (unsigned)offset);
    return 0;
}

void i915_ppgtt_fini(struct drm_i915_private *i915)
{
    struct i915_ppgtt *pp = &i915->gt.ppgtt;
    uint32_t pde;

    if (pp->ready) {
        (void)i915_ggtt_clear_range(i915, pp->pd_ggtt, I915_PPGTT_PD_BYTES, 0);
        if (pp->pd_ggtt == i915->gt.ggtt_bottom) {
            i915->gt.ggtt_bottom += pp->pd_take;
        }
    }
    for (pde = 0; pde < I915_PPGTT_PDES; pde++) {
        if (pp->pt[pde]) {
            kfree_pages(pp->pt[pde], 1);
        }
    }
    if (pp->scratch_pt) {
        kfree_pages(pp->scratch_pt, 1);
    }
    k_memset(pp, 0, sizeof(*pp));
}

/** Whether [offset, offset + size) is whole pages inside the PPGTT. */
static int ppgtt_range_ok(uint64_t offset, uint64_t size)
{
    return ((offset | size) & (I915_GTT_PAGE_SIZE - 1u)) == 0 && offset <= I915_PPGTT_SIZE &&
           size <= I915_PPGTT_SIZE - offset;
}

int32_t i915_ppgtt_insert(struct drm_i915_private *i915, uint64_t offset, const uint64_t *pages,
                          uint32_t n, int cache)
{
    struct i915_ppgtt *pp = &i915->gt.ppgtt;
    uint32_t first = (uint32_t)(offset / I915_GTT_PAGE_SIZE), lo, hi, made_lo, made_hi, pde, i;
    int32_t err = 0;

    if (!pp->ready || n == 0) {
        return 0;
    }
    if (!ppgtt_range_ok(offset, (uint64_t)n * I915_GTT_PAGE_SIZE)) {
        return -EINVAL;
    }
    lo = first / I915_PPGTT_PTES;
    hi = (first + n - 1u) / I915_PPGTT_PTES + 1u;
    made_lo = hi;
    made_hi = lo;
    for (pde = lo; pde < hi; pde++) {
        if (pp->pt[pde]) {
            continue;
        }
        pp->pt[pde] = ppgtt_alloc_table(&pp->pt_phys[pde]);
        if (!pp->pt[pde]) {
            klog(KLOG_ERR, "i915", "ppgtt: no page for the table of 0x%lx",
                 (unsigned long)pde * I915_PPGTT_PTES * I915_GTT_PAGE_SIZE);
            err = -ENOMEM;
            break;
        }
        ppgtt_fill_scratch(i915, pp->pt[pde]);
        pp->tables++;
        if (pde < made_lo) {
            made_lo = pde;
        }
        made_hi = pde + 1u;
    }
    /* The tables made stay, named in the directory, as Linux's stash. */
    if (made_lo < made_hi) {
        ppgtt_flush_pd(i915, made_lo, made_hi);
    }
    if (err < 0) {
        return err;
    }
    for (i = 0; i < n; i++) {
        uint32_t e = first + i;

        pp->pt[e / I915_PPGTT_PTES][e % I915_PPGTT_PTES] = i915_ggtt_pte_encode(pages[i], cache);
    }
    return 0;
}

void i915_ppgtt_clear_range(struct drm_i915_private *i915, uint64_t offset, uint64_t size)
{
    struct i915_ppgtt *pp = &i915->gt.ppgtt;
    uint32_t first = (uint32_t)(offset / I915_GTT_PAGE_SIZE), i;

    if (!pp->ready || !ppgtt_range_ok(offset, size)) {
        return;
    }
    for (i = 0; i < size / I915_GTT_PAGE_SIZE; i++) {
        uint32_t e = first + i;
        uint32_t *pt = pp->pt[e / I915_PPGTT_PTES];

        if (pt) {
            pt[e % I915_PPGTT_PTES] = i915->ggtt.scratch_pte;
        }
    }
}

int32_t i915_ppgtt_enable_ring(struct drm_i915_private *i915, struct i915_engine *engine)
{
    struct i915_hw *hw = &i915->hw;
    uint32_t base = engine->mmio_base;

    if (!i915->gt.ppgtt.ready) {
        return 0;
    }
    if (engine == &i915->gt.engine[I915_RCS0]) {
        (void)i915_rmw(hw, GAC_ECO_BITS, 0, ECOBITS_PPGTT_CACHE64B);
        i915_write(hw, GAM_ECOCHK, i915_read(hw, GAM_ECOCHK) | ECOCHK_PPGTT_WB_HSW);
    }
    i915_write(hw, RING_PP_DIR_DCLV(base), PP_DIR_DCLV_2G);
    i915_write(hw, RING_PP_DIR_BASE(base), i915->gt.ppgtt.pp_dir);
    i915_write(hw, RING_MODE_GEN7(base), I915_MASKED_ENABLE(GFX_PPGTT_ENABLE));
    return 0;
}
