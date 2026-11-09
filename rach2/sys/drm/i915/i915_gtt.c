/*
 * i915_gtt.c: Haswell's global GTT, the table that maps the GPU's global
 * address space, and through the aperture the processor's view of it, to
 * pages, after Linux 6.12's gt/intel_ggtt.c and gt/intel_ggtt_gmch.c.
 */

#include "i915.h"
#include "i915_reg.h"

#include "../../klog.h"
#include "../../lib/kerrno.h"
#include "../../lib/kstring.h"
#include "../../mm/paging.h"

/* The aperture's sizes by MSAC's code; 0 for the code no chip uses. */
static const uint32_t i915_aperture_sizes[4] = {0x8000000u, 0x10000000u, 0, 0x20000000u};

/* The widest address an entry holds: bits 39:32 go in bits 10:4, of
 * which Haswell has bits 38:32. */
#define I915_GTT_ADDR_LIMIT (1ULL << 39)

uint32_t i915_ggtt_pte_encode(uint64_t addr, int cache)
{
    uint32_t pte = GEN6_PTE_VALID | HSW_GTT_ADDR_ENCODE(addr);

    if (cache != I915_CACHE_NONE) {
        pte |= HSW_WB_LLC_AGE3;
    }
    return pte;
}

void i915_ggtt_invalidate(struct drm_i915_private *i915)
{
    uint32_t flags = spin_lock_irqsave(&i915->uncore.lock);

    i915_write(&i915->hw, GFX_FLSH_CNTL_GEN6, GFX_FLSH_CNTL_EN);
    i915_posting_read(&i915->hw, GFX_FLSH_CNTL_GEN6);
    spin_unlock_irqrestore(&i915->uncore.lock, flags);
}

/** Add [start, end) to the kept ranges, when there is room. */
static void i915_ggtt_keep(struct i915_ggtt *ggtt, uint64_t start, uint64_t end)
{
    if (ggtt->nkept < I915_GGTT_KEPT_MAX) {
        ggtt->kept[ggtt->nkept].start = start;
        ggtt->kept[ggtt->nkept].end = end;
        ggtt->nkept++;
    }
}

/**
 * Find the surfaces the firmware's enabled primary planes scan out, which
 * the console also draws on through the aperture, and keep their ranges:
 * the surface's address and its stride times its rows, the plane's offset
 * among them, and whole tile rows of 8 for an X-tiled one. Pipes B and C
 * are read only while the display well, which powers them, is on.
 */
static void i915_ggtt_find_firmware(struct drm_i915_private *i915)
{
    struct i915_hw *hw = &i915->hw;
    struct i915_ggtt *ggtt = &i915->ggtt;
    uint64_t surf, bytes;
    uint32_t stride, rows;
    int pipe, well = i915_power_well_on(i915);

    for (pipe = I915_PIPE_A; pipe <= I915_PIPE_C; pipe++) {
        uint32_t ctl;

        if (pipe != I915_PIPE_A && !well) {
            continue;
        }
        ctl = i915_read(hw, DSPCNTR(pipe));
        if (!(ctl & DISP_ENABLE)) {
            continue;
        }
        surf = i915_read(hw, DSPSURF(pipe)) & DISP_ADDR_MASK;
        stride = i915_read(hw, DSPSTRIDE(pipe));
        rows = (i915_read(hw, PIPESRC(pipe)) & 0xfffu) + 1u +
               ((i915_read(hw, DSPOFFSET(pipe)) >> 16) & 0xfffu);
        if (ctl & DISP_TILED) {
            rows = (rows + 7u) & ~7u;
        }
        bytes = ((uint64_t)stride * rows + I915_GTT_PAGE_SIZE - 1) &
                ~(uint64_t)(I915_GTT_PAGE_SIZE - 1);
        if (stride == 0 || surf >= ggtt->total) {
            continue;
        }
        if (bytes > ggtt->total - surf) {
            bytes = ggtt->total - surf;
        }
        i915_ggtt_keep(ggtt, surf, surf + bytes);
        klog(KLOG_INFO, "i915", "gtt: pipe %c's firmware surface at 0x%lx, %lu KiB, kept",
             'A' + pipe, (unsigned long)surf, (unsigned long)(bytes / 1024));
    }
}

/** Whether the GTT offset `offset` lies in a kept range. */
static int i915_ggtt_is_kept(const struct i915_ggtt *ggtt, uint64_t offset)
{
    int i;

    for (i = 0; i < ggtt->nkept; i++) {
        if (offset >= ggtt->kept[i].start && offset < ggtt->kept[i].end) {
            return 1;
        }
    }
    return 0;
}

/**
 * Check that [offset, offset + size) is whole pages inside the GTT and
 * touches no kept range: 0, -EINVAL or -EBUSY.
 */
static int32_t i915_ggtt_check(const struct i915_ggtt *ggtt, uint64_t offset, uint64_t size)
{
    int i;

    if (size == 0 || ((offset | size) & (I915_GTT_PAGE_SIZE - 1)) != 0 || offset > ggtt->total ||
        size > ggtt->total - offset) {
        return -EINVAL;
    }
    for (i = 0; i < ggtt->nkept; i++) {
        if (offset < ggtt->kept[i].end && offset + size > ggtt->kept[i].start) {
            return -EBUSY;
        }
    }
    return 0;
}

int32_t i915_ggtt_insert(struct drm_i915_private *i915, uint64_t offset, const uint64_t *pages,
                         uint32_t n, int cache)
{
    uint32_t i, first = (uint32_t)(offset / I915_GTT_PAGE_SIZE);
    int32_t err = i915_ggtt_check(&i915->ggtt, offset, (uint64_t)n * I915_GTT_PAGE_SIZE);

    if (err < 0) {
        return err;
    }
    for (i = 0; i < n; i++) {
        if ((pages[i] & (I915_GTT_PAGE_SIZE - 1)) || pages[i] >= I915_GTT_ADDR_LIMIT) {
            return -EINVAL;
        }
    }
    for (i = 0; i < n; i++) {
        i915_gtt_write(&i915->hw, first + i, i915_ggtt_pte_encode(pages[i], cache));
    }
    i915_ggtt_invalidate(i915);
    return 0;
}

int32_t i915_ggtt_clear_range(struct drm_i915_private *i915, uint64_t offset, uint64_t size)
{
    uint32_t i, first = (uint32_t)(offset / I915_GTT_PAGE_SIZE);
    int32_t err = i915_ggtt_check(&i915->ggtt, offset, size);

    if (err < 0) {
        return err;
    }
    for (i = 0; i < size / I915_GTT_PAGE_SIZE; i++) {
        i915_gtt_write(&i915->hw, first + i, i915->ggtt.scratch_pte);
    }
    i915_ggtt_invalidate(i915);
    return 0;
}

uint32_t i915_ggtt_pte(struct drm_i915_private *i915, uint64_t offset)
{
    if (offset >= i915->ggtt.total) {
        return 0xffffffffu;
    }
    return i915_gtt_read(&i915->hw, (uint32_t)(offset / I915_GTT_PAGE_SIZE));
}

int32_t i915_ggtt_init(struct drm_i915_private *i915)
{
    struct i915_ggtt *ggtt = &i915->ggtt;
    const pci_device_t *pdev = &i915->pci;
    uint32_t gsm_size, entries, i, cleared = 0;
    uint16_t msac;
    int32_t err;

    k_memset(ggtt, 0, sizeof(*ggtt));
    /* gen6_get_total_gtt_size: GGMS counts MiB of entries. */
    gsm_size = (uint32_t)((i915->gmch_ctrl >> SNB_GMCH_GGMS_SHIFT) & SNB_GMCH_GGMS_MASK) << 20;
    if (gsm_size == 0) {
        klog(KLOG_ERR, "i915", "gtt: none: the GMCH control word gives no GTT");
        return -ENODEV;
    }
    err = i915_hw_gtt_init(&i915->hw, i915->mmio_phys + I915_GSM_OFFSET, gsm_size);
    if (err < 0) {
        klog(KLOG_ERR, "i915", "gtt: entries not reachable: error %d", (int)err);
        return err;
    }
    entries = gsm_size / 4u;
    ggtt->total = (uint64_t)entries * I915_GTT_PAGE_SIZE;

    msac = pci_read_config16(pdev->bus, pdev->device, pdev->function, HSW_MSAC);
    ggtt->mappable_end = i915_aperture_sizes[(msac >> HSW_MSAC_SIZE_SHIFT) & HSW_MSAC_SIZE_MASK];
    if (ggtt->mappable_end == 0) {
        klog(KLOG_WARN, "i915", "gtt: aperture size code %u unknown, taken as 128 MiB",
             (unsigned)((msac >> HSW_MSAC_SIZE_SHIFT) & HSW_MSAC_SIZE_MASK));
        ggtt->mappable_end = i915_aperture_sizes[0];
    }
    if (ggtt->mappable_end > ggtt->total) {
        ggtt->mappable_end = ggtt->total;
    }

    ggtt->scratch = alloc_frame();
    if (ggtt->scratch == 0) {
        klog(KLOG_ERR, "i915", "gtt: no frame for the scratch page");
        return -ENOMEM;
    }
    if (paging_zero_frame(ggtt->scratch) < 0) {
        free_frame(ggtt->scratch);
        ggtt->scratch = 0;
        klog(KLOG_ERR, "i915", "gtt: the scratch page would not clear");
        return -ENOMEM;
    }
    ggtt->scratch_pte = i915_ggtt_pte_encode(ggtt->scratch, I915_CACHE_NONE);

    i915_ggtt_find_firmware(i915);
    for (i = 0; i < entries; i++) {
        if (!i915_ggtt_is_kept(ggtt, (uint64_t)i * I915_GTT_PAGE_SIZE)) {
            i915_gtt_write(&i915->hw, i, ggtt->scratch_pte);
            cleared++;
        }
    }
    i915_ggtt_invalidate(i915);
    klog(KLOG_INFO, "i915",
         "gtt: %lu MiB, %lu MiB of it through the aperture; %lu entries to the scratch page, %lu "
         "kept",
         (unsigned long)(ggtt->total >> 20), (unsigned long)(ggtt->mappable_end >> 20),
         (unsigned long)cleared, (unsigned long)(entries - cleared));
    return 0;
}

void i915_ggtt_fini(struct drm_i915_private *i915)
{
    if (i915->ggtt.scratch) {
        free_frame(i915->ggtt.scratch);
        i915->ggtt.scratch = 0;
    }
}
