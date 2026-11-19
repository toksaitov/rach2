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
#include "../../proc/mutex.h"
#include "../../proc/sched.h"

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
            ggtt->fw_tiled = 1;
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

/*
 * The pages a binding left mapped, which the next bind of its function
 * keeps: their function, bus << 8 | devfn, and range, `ncarried` of them.
 * Linux has none, since its unbind frees a status page only after masking
 * its writes; here a release that could not mask them keeps the page, and
 * a bind with no reset of the function between could otherwise point the
 * range elsewhere while the GPU still writes there.
 */
static struct {
    int fn;
    struct i915_gtt_range range;
} carried[I915_NUM_ENGINES];
static int ncarried;

/** The function `pdev` as `carried` names it. */
static int carry_fn(const pci_device_t *pdev)
{
    return (int)pdev->bus << 8 | pdev->device << 3 | pdev->function;
}

void i915_ggtt_carry(struct drm_i915_private *i915, uint64_t offset)
{
    if (ncarried == I915_NUM_ENGINES) {
        klog(KLOG_WARN, "i915", "gtt: no room to keep 0x%lx for the next bind",
             (unsigned long)offset);
        return;
    }
    carried[ncarried].fn = carry_fn(&i915->pci);
    carried[ncarried].range.start = offset;
    carried[ncarried].range.end = offset + I915_GTT_PAGE_SIZE;
    ncarried++;
}

void i915_ggtt_carry_drop(const pci_device_t *pdev)
{
    int i, n = 0, fn = carry_fn(pdev);

    for (i = 0; i < ncarried; i++) {
        if (carried[i].fn != fn) {
            carried[n++] = carried[i];
        }
    }
    ncarried = n;
}

/** Keep the ranges an earlier binding of `i915`'s function carried. */
static void i915_ggtt_keep_carried(struct drm_i915_private *i915)
{
    int i, fn = carry_fn(&i915->pci);

    for (i = 0; i < ncarried; i++) {
        if (carried[i].fn == fn) {
            i915_ggtt_keep(&i915->ggtt, carried[i].range.start, carried[i].range.end);
            klog(KLOG_WARN, "i915",
                 "gtt: 0x%lx kept: a status page the function's last "
                 "binding kept, with no reset since",
                 (unsigned long)carried[i].range.start);
        }
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
    err = i915_hw_fault(&i915->hw, I915_FAULT_BIND);
    if (err < 0) {
        return err;
    }
    /* The entries are the device's, as Linux's GGTT updates hold a wakeref. */
    i915_rpm_get(i915);
    for (i = 0; i < n; i++) {
        i915_gtt_write(&i915->hw, first + i, i915_ggtt_pte_encode(pages[i], cache));
    }
    i915_ggtt_invalidate(i915);
    i915_rpm_put(i915);
    return 0;
}

/*
 * The entries a clear writes between two points where it gives way: 2 MiB
 * of the GTT, as Linux gives way after each PMD entry's 512 pages
 * (mm/memory.c:1740).
 */
#define I915_GGTT_CLEAR_BATCH 512u

/**
 * Linux's gen6_ggtt_clear_range, which Haswell's aliasing PPGTT selects
 * (gt/intel_ggtt.c:718-737, 1422-1425), and gen8_ggtt_clear_range (548-567)
 * write the entries in one loop and leave the GTT's flush to the next
 * insert (gen6_ggtt_insert_entries, 637-641); this clear flushes once,
 * after the last entry, so that no translation of a page it unmaps
 * outlives it in the GPU's TLB. The entries go through a write-combining
 * mapping, as Linux's ggtt_probe_common maps them (1167-1170), and the
 * flush's posting read waits for them all. With `may_yield` set, a holder
 * of the GEM lock with interrupts on gives way after each
 * I915_GGTT_CLEAR_BATCH entries (`sched_give_way`): the lock keeps the GTT
 * and its bindings whole for every other thread (i915.h), and the range
 * stays reserved to its caller, which releases it only after the clear, so
 * no other thread reaches the entries between two batches. Only a caller
 * that may sleep sets it, as only such a caller of Linux's unbind reaches a
 * cond_resched (i915_gem_evict.c:270, gem/i915_gem_object.c:423): the
 * ioctls, eviction and an object's free by the holder that took the GEM
 * lock, for the free or before it (`gem.freed`). A
 * give-way lets the big lock go, and an MMU notifier's callback, the
 * shrinker and whatever runs inside them hold it across a change that must
 * look whole to every other thread.
 */
int32_t i915_ggtt_clear_range(struct drm_i915_private *i915, uint64_t offset, uint64_t size,
                              int may_yield)
{
    uint32_t i, n, first = (uint32_t)(offset / I915_GTT_PAGE_SIZE);
    int32_t err = i915_ggtt_check(&i915->ggtt, offset, size);
    int yield;

    if (err < 0) {
        return err;
    }
    n = (uint32_t)(size / I915_GTT_PAGE_SIZE);
    yield = may_yield && mutex_held(&i915->gem.lock);
    i915_rpm_get(i915);
    for (i = 0; i < n; i++) {
        i915_gtt_write(&i915->hw, first + i, i915->ggtt.scratch_pte);
        if (yield && (i + 1u) % I915_GGTT_CLEAR_BATCH == 0 && i + 1u < n) {
            sched_give_way();
        }
    }
    i915_ggtt_invalidate(i915);
    i915_rpm_put(i915);
    return 0;
}

uint32_t i915_ggtt_pte(struct drm_i915_private *i915, uint64_t offset)
{
    uint32_t pte;

    if (offset >= i915->ggtt.total) {
        return 0xffffffffu;
    }
    i915_rpm_get(i915);
    pte = i915_gtt_read(&i915->hw, (uint32_t)(offset / I915_GTT_PAGE_SIZE));
    i915_rpm_put(i915);
    return pte;
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

    msac = i915_hw_config16(&i915->hw, pdev, HSW_MSAC);
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
    i915_ggtt_keep_carried(i915);
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
