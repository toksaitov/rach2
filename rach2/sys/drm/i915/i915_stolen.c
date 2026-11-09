/*
 * i915_stolen.c: the GPU's stolen memory, which the firmware takes from the
 * top of the machine's memory: reserved from the frame allocator at boot,
 * as Linux's early quirk does, and found and checked at the bind, as
 * Linux 6.12's i915_gem_stolen.c does.
 */

#include "i915.h"
#include "i915_reg.h"

#include "../../iommu.h"
#include "../../klog.h"
#include "../../lib/kstring.h"
#include "../../mm/frame.h"
#include "../../mm/paging.h"

#define PCI_VENDOR_INTEL 0x8086

/* Where Intel's integrated GPU is, on every chipset that has one. */
#define I915_GPU_BUS      0
#define I915_GPU_DEVICE   2
#define I915_GPU_FUNCTION 0

#define I915_STOLEN_UNIT 0x2000000u /* GMS counts 32 MiB */

/* Only x86-64's kernel reads the DMAR table. A host test models the
 * Lenovo, an x86-64 machine, whatever processor the build machine has. */
#if defined(__x86_64__) || defined(RACH2_HOSTTEST)
#define I915_STOLEN_DMAR 1
#endif

/** The stolen memory that the GMCH control word `gmch` and BDSM `bsm`
 * describe, as Linux's gen6_stolen_size and intel_stolen_base: its base in
 * `*base` and its size, 0 for none, in `*size`. */
static void i915_stolen_range(uint16_t gmch, uint32_t bsm, uint64_t *base, uint64_t *size)
{
    *base = bsm & INTEL_BSM_MASK;
    *size = (uint64_t)((gmch >> SNB_GMCH_GMS_SHIFT) & SNB_GMCH_GMS_MASK) * I915_STOLEN_UNIT;
    if (*base == 0) {
        *size = 0;
    }
}

uint64_t i915_stolen_reserve_early(struct frame_map *map)
{
    uint32_t id =
        pci_read_config32(I915_GPU_BUS, I915_GPU_DEVICE, I915_GPU_FUNCTION, PCI_VENDOR_ID);
    uint64_t base, size, before;

    if ((id & 0xffffu) != PCI_VENDOR_INTEL || !i915_id_lookup((uint16_t)(id >> 16))) {
        return 0;
    }
    i915_stolen_range(
        pci_read_config16(I915_GPU_BUS, I915_GPU_DEVICE, I915_GPU_FUNCTION, SNB_GMCH_CTRL),
        pci_read_config32(I915_GPU_BUS, I915_GPU_DEVICE, I915_GPU_FUNCTION, INTEL_BSM), &base,
        &size);
    if (size == 0) {
        return 0;
    }
    before = map->reserved_bytes;
    if (frame_map_reserve(map, base, base + size) != 0) {
        klog(KLOG_ERR, "i915", "stolen: [mem 0x%lx-0x%lx] not reserved: the memory map is full",
             (unsigned long)base, (unsigned long)(base + size - 1));
        return 0;
    }
    klog(KLOG_INFO, "i915", "stolen: reserving [mem 0x%lx-0x%lx], %lu KiB of it usable memory",
         (unsigned long)base, (unsigned long)(base + size - 1),
         (unsigned long)((map->reserved_bytes - before) / 1024));
    return map->reserved_bytes - before;
}

/** Whether the frame allocator owns a frame of [base, base + size). */
static int i915_stolen_allocator_owns(uint64_t base, uint64_t size)
{
    uint64_t at;

    for (at = base; at < base + size; at += PAGE_SIZE) {
        if (at > (uint64_t)(uintptr_t)-1 || !paging_frame_is_device((uintptr_t)at)) {
            return 1;
        }
    }
    return 0;
}

#if defined(I915_STOLEN_DMAR)
/** The device's number and function, as the IOMMU's calls take them. */
static uint8_t i915_devfn(const struct drm_i915_private *i915)
{
    return (uint8_t)((i915->pci.device << 3) | i915->pci.function);
}

/**
 * Check [base, base + size) against the reserved regions the firmware's
 * DMAR table names for the device: 1 when it lies inside one, 0 when the
 * table names some and it lies outside them all, and -1 when it names
 * none, as without VT-d. Only x86-64's kernel reads the table.
 */
static int i915_stolen_in_rmrr(struct drm_i915_private *i915, uint64_t base, uint64_t size)
{
    struct iommu_region r[8];
    int i, n, named = 0;

    n = iommu_reserved_regions(i915->pci.bus, i915_devfn(i915), r, 8);
    for (i = 0; i < n; i++) {
        if (r[i].type == IOMMU_RESV_MSI) {
            continue;
        }
        named = 1;
        if (base >= r[i].start && base + size - 1 <= r[i].end) {
            return 1;
        }
    }
    return named ? 0 : -1;
}
#endif

/**
 * The part at the top of stolen memory that the firmware keeps, from
 * GEN6_STOLEN_RESERVED as Linux's gen7_get_stolen_reserved reads it, into
 * `st`: none when the register's enable is clear. 0, or -1 when the part
 * lies outside the stolen memory, which Linux then refuses whole.
 */
static int i915_stolen_reserved(struct drm_i915_private *i915, struct i915_stolen *st)
{
    uint32_t reg = i915_read(&i915->hw, GEN6_STOLEN_RESERVED);
    uint64_t top = st->base + st->size;

    st->reserved_base = top;
    st->reserved_size = 0;
    if (reg & GEN6_STOLEN_RESERVED_ENABLE) {
        st->reserved_size = (reg & GEN7_STOLEN_RESERVED_SIZE_MASK) == GEN7_STOLEN_RESERVED_256K
                                ? 0x40000u
                                : 0x100000u;
        st->reserved_base = reg & GEN7_STOLEN_RESERVED_ADDR_MASK;
        if (st->reserved_base == 0) {
            klog(KLOG_ERR, "i915", "stolen: inconsistent reservation 0 + 0x%lx; ignoring",
                 (unsigned long)st->reserved_size);
            st->reserved_base = top;
            st->reserved_size = 0;
        }
    }
    if (st->reserved_base < st->base || st->reserved_base + st->reserved_size > top) {
        klog(KLOG_ERR, "i915", "stolen: reserved [mem 0x%lx-0x%lx] outside stolen memory",
             (unsigned long)st->reserved_base,
             (unsigned long)(st->reserved_base + st->reserved_size - 1));
        return -1;
    }
    st->usable_size = st->reserved_base - st->base;
    return 0;
}

void i915_stolen_init(struct drm_i915_private *i915)
{
    struct i915_stolen *st = &i915->stolen;
    const char *where = "", *dmar = "";
    uint64_t base, size;
#if defined(I915_STOLEN_DMAR)
    int rmrr;
#endif

    k_memset(st, 0, sizeof(*st));
    i915_stolen_range(i915->gmch_ctrl, i915->bsm, &base, &size);
    if (size == 0) {
        klog(KLOG_INFO, "i915", "stolen: none");
        return;
    }
    if (base + size < base || (base & (PAGE_SIZE - 1)) != 0) {
        klog(KLOG_ERR, "i915", "stolen: base 0x%lx unusable", (unsigned long)base);
        return;
    }
    if (i915_stolen_allocator_owns(base, size)) {
        klog(KLOG_ERR, "i915", "stolen: [mem 0x%lx-0x%lx] holds frames the allocator owns; unused",
             (unsigned long)base, (unsigned long)(base + size - 1));
        return;
    }
#if defined(I915_STOLEN_DMAR)
    rmrr = i915_stolen_in_rmrr(i915, base, size);
    if (rmrr == 0) {
        klog(KLOG_ERR, "i915",
             "stolen: [mem 0x%lx-0x%lx] outside the device's reserved regions; unused",
             (unsigned long)base, (unsigned long)(base + size - 1));
        return;
    }
#endif
    st->base = base;
    st->size = size;
    if (i915_stolen_reserved(i915, st) < 0) {
        k_memset(st, 0, sizeof(*st));
        return;
    }
    st->for_objects = 1;
#if defined(I915_STOLEN_DMAR)
    if (rmrr == 1) {
        st->in_rmrr = 1;
        where = ", inside the device's reserved region";
    }
    /* As Linux on a chip before graphics version 8 (i915_vtd_active): no
     * buffers in stolen memory while VT-d translates for the device. i686's
     * kernel brings no unit up. */
    if (iommu_present() && iommu_unit_of(i915->pci.bus, i915_devfn(i915)) >= 0) {
        st->for_objects = 0;
        dmar = "; DMAR active, no buffers in it";
    }
#endif
    klog(KLOG_INFO, "i915", "stolen: [mem 0x%lx-0x%lx], %lu KiB usable%s%s", (unsigned long)base,
         (unsigned long)(base + size - 1), (unsigned long)(st->usable_size / 1024), where, dmar);
}
