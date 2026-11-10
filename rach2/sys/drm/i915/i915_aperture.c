/*
 * i915_aperture.c: the kernel's and programs' reach into the aperture, BAR
 * 2, through windows that move over it, as Linux's io_mapping, or through a
 * backend that stands in for it (i915_hw.h).
 */

#include "i915_hw.h"

#include "../../arch/x86/pat.h"
#include "../../lib/kerrno.h"
#include "../../lib/kstring.h"
#include "../../mm/paging.h"

/* The aperture's page, the GTT's. */
#define I915_APERTURE_PAGE 4096u

void i915_aperture_init(struct i915_aperture *ap, uint64_t phys, uint64_t size)
{
    k_memset(ap, 0, sizeof(*ap));
    ap->phys = phys;
    ap->size = size;
}

void i915_aperture_fini(struct i915_aperture *ap)
{
    int i;

    for (i = 0; i < I915_APERTURE_WINDOWS; i++) {
        if (ap->win[i].va) {
            mmio_unmap(ap->win[i].va, ap->win[i].size);
            ap->win[i].va = 0;
        }
    }
}

int32_t i915_aperture_phys(struct i915_hw *hw, const struct i915_aperture *ap, uint64_t offset,
                           uintptr_t *phys)
{
    if (offset >= ap->size || (offset & (I915_APERTURE_PAGE - 1)) != 0) {
        return -EINVAL;
    }
    if (hw->ops->aperture_page) {
        return hw->ops->aperture_page(hw, offset, phys) == 0 ? 0 : -EINVAL;
    }
    if (ap->phys + offset > (uint64_t)(uintptr_t)-1) {
        return -EINVAL;
    }
    *phys = (uintptr_t)(ap->phys + offset);
    return 0;
}

void *i915_aperture_kmap(struct i915_hw *hw, struct i915_aperture *ap, uint64_t offset)
{
    struct i915_aperture_window *w = 0, *lru = 0;
    uint64_t base = offset & ~(uint64_t)(I915_APERTURE_WINDOW - 1);
    uintptr_t phys;
    int i;

    if (offset >= ap->size || (offset & (I915_APERTURE_PAGE - 1)) != 0) {
        return 0;
    }
    if (hw->ops->aperture_page) {
        return hw->ops->aperture_page(hw, offset, &phys) == 0 ? kmap_temp(phys) : 0;
    }
    for (i = 0; i < I915_APERTURE_WINDOWS; i++) {
        w = &ap->win[i];
        if (w->va && w->offset == base) {
            break;
        }
        if (w->users == 0 && (!lru || !w->va || (lru->va && w->used < lru->used))) {
            lru = w;
        }
    }
    if (i == I915_APERTURE_WINDOWS) {
        if (!lru) {
            return 0;
        }
        w = lru;
        if (w->va) {
            mmio_unmap(w->va, w->size);
            w->va = 0;
        }
        w->size = ap->size - base < I915_APERTURE_WINDOW ? (uint32_t)(ap->size - base)
                                                         : I915_APERTURE_WINDOW;
        w->va = mmio_map(ap->phys + base, w->size, pat_write_combining() ? PTE_WC : PTE_NOCACHE);
        if (!w->va) {
            return 0;
        }
        w->offset = base;
        ap->maps++;
    }
    w->users++;
    w->used = ++ap->clock;
    return (void *)(w->va + (uintptr_t)(offset - base));
}

void i915_aperture_kunmap(struct i915_hw *hw, struct i915_aperture *ap, void *va)
{
    uintptr_t at = (uintptr_t)va;
    int i;

    if (hw->ops->aperture_page) {
        kunmap_temp(va);
        return;
    }
    for (i = 0; i < I915_APERTURE_WINDOWS; i++) {
        struct i915_aperture_window *w = &ap->win[i];

        if (w->va && at >= w->va && at - w->va < w->size && w->users) {
            w->users--;
            return;
        }
    }
}

void i915_aperture_sync(struct i915_hw *hw, uint64_t offset, uint64_t size, int to_memory)
{
    if (hw->ops->aperture_sync && size) {
        hw->ops->aperture_sync(hw, offset, size, to_memory);
    }
}
