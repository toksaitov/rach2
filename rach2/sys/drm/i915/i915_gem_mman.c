/*
 * i915_gem_mman.c: programs' mappings of buffer objects, of their pages
 * write-back, write-combining or uncached, and through the aperture, whose
 * faults bind the object and take a fence, after Linux 6.12's
 * gem/i915_gem_mman.c.
 */

#include "i915.h"
#include "i915_drm.h"

#include "../../arch/x86/pat.h"
#include "../../lib/kerrno.h"
#include "../../mm/paging.h"
#include "../../mm/usercopy.h"

/* The pages a partial view of an object too large for the aperture binds
 * at least, Linux's MIN_CHUNK_PAGES: 1 MiB. */
#define I915_MIN_CHUNK_PAGES (0x100000u >> PAGE_SHIFT)

/** The object a mapping of `mmo` maps. */
static struct drm_i915_gem_object *mmo_object(const struct i915_mmap_offset *mmo)
{
    return (struct drm_i915_gem_object *)mmo->node.obj;
}

/** The `put` of every mapping of an object: the reference the region held. */
static void mmo_vm_put(void *data)
{
    i915_gem_object_put(mmo_object((struct i915_mmap_offset *)data));
}

/* ------------------------------------------------------------------------
 * Mappings of the pages
 */

/** The `page` of a mapping of the object's own pages: its frame, given on
 * the first touch; -1 past its end or once the device is gone. */
static int cpu_vm_page(void *data, uint32_t pgoff, uintptr_t *phys)
{
    struct drm_i915_gem_object *obj = mmo_object((struct i915_mmap_offset *)data);
    uintptr_t p;

    if (obj->base.dev->unplugged) {
        return -1;
    }
    p = i915_gem_object_frame(obj, pgoff);
    if (!p) {
        return -1;
    }
    *phys = p;
    return 0;
}

static const struct vm_device_ops cpu_vm_ops = {cpu_vm_page, mmo_vm_put};

int32_t i915_gem_object_mmap_cpu(struct drm_gem_object *base, vma_t *vma)
{
    struct drm_i915_gem_object *obj = (struct drm_i915_gem_object *)base;

    /* As Linux's i915_gem_dmabuf_mmap, the object's pages write-back, but a
     * dumb buffer's write-combined, as Linux's MAP_DUMB maps it, since the
     * display reads it uncached. An import's are another device's, and a
     * userptr object's the program's own. */
    if (obj->base.import || i915_gem_object_is_userptr(obj)) {
        return -ENODEV;
    }
    if (obj->dumb) {
        return vm_device_map(vma, &cpu_vm_ops, &obj->mmo[I915_MMAP_OFFSET_WC], VM_PROT_WC);
    }
    return vm_device_map(vma, &cpu_vm_ops, &obj->mmo[I915_MMAP_OFFSET_WB], 0u);
}

/* ------------------------------------------------------------------------
 * Mappings through the aperture
 */

/**
 * The binding a GTT fault at `pgoff` maps, as Linux's vm_fault_gtt: the
 * whole object inside the aperture when it fits without evicting, or a
 * partial view of at least I915_MIN_CHUNK_PAGES around the page, rounded to
 * whole tile rows (compute_partial_view). Pinned, which the caller drops.
 */
static int32_t gtt_fault_pin(struct drm_i915_gem_object *obj, uint32_t pgoff, struct i915_vma **vma)
{
    uint32_t pages = (uint32_t)(obj->base.size >> PAGE_SHIFT);
    uint32_t chunk = I915_MIN_CHUNK_PAGES, row, offset, n;

    if (i915_vma_pin(obj, 0, 0, I915_PIN_MAPPABLE | I915_PIN_NOEVICT, 0, vma) == 0) {
        return 0;
    }
    row = i915_gem_tile_row_size(obj->tiling, obj->stride) >> PAGE_SHIFT;
    if (row > 1) {
        chunk = (chunk + row - 1) / row * row;
    }
    if (chunk >= pages) {
        return i915_vma_pin(obj, 0, 0, I915_PIN_MAPPABLE, 0, vma);
    }
    offset = pgoff / chunk * chunk;
    n = pages - offset < chunk ? pages - offset : chunk;
    return i915_vma_pin(obj, offset, n == pages ? 0 : n, I915_PIN_MAPPABLE, 0, vma);
}

/**
 * The `page` of a GTT map, as Linux's vm_fault_gtt: the object bound inside
 * the aperture and fenced for its tiling, and the aperture's page mapped,
 * the binding marked as mapped by programs so that unbinding it or moving
 * its fence revokes the mapping. A partial view evicts idle bindings for
 * its room; a failure, no room even so or no fence free, raises SIGBUS.
 */
static int gtt_vm_page(void *data, uint32_t pgoff, uintptr_t *phys)
{
    struct i915_mmap_offset *mmo = (struct i915_mmap_offset *)data;
    struct drm_i915_gem_object *obj = mmo_object(mmo);
    struct drm_i915_private *i915 = to_i915(obj->base.dev);
    struct i915_vma *vma;
    int32_t err;

    if (obj->base.dev->unplugged || (uint64_t)pgoff >= obj->base.size >> PAGE_SHIFT) {
        return -1;
    }
    if (gtt_fault_pin(obj, pgoff, &vma) != 0) {
        return -1;
    }
    err = i915_vma_pin_fence(vma);
    if (err == 0) {
        err = i915_aperture_phys(&i915->hw, &i915->gem.aperture,
                                 vma->start + ((uint64_t)(pgoff - vma->view_offset) << PAGE_SHIFT),
                                 phys);
        if (err == 0) {
            if (!vma->userfault) {
                vma->userfault = 1;
                obj->userfault_count++;
            }
            /* The fault does not say whether it writes; the writes are
             * flushed before the binding or its fence goes either way. */
            vma->ggtt_write = 1;
        }
        i915_vma_unpin_fence(vma);
    }
    i915_vma_unpin(vma);
    return err == 0 ? 0 : -1;
}

static const struct vm_device_ops gtt_vm_ops = {gtt_vm_page, mmo_vm_put};

void i915_gem_object_release_mmap_gtt(struct drm_i915_gem_object *obj)
{
    struct i915_vma *vma;

    for (vma = obj->vmas; vma; vma = vma->obj_next) {
        /* The chip's aperture holds nothing; a backend's that stands in
         * for it gives its bytes back before the next fault fills it. */
        i915_vma_flush_writes(vma);
        i915_vma_revoke_mmap(vma);
    }
}

int32_t i915_gem_mmo_mmap(struct drm_mmap_node *node, vma_t *vma)
{
    struct i915_mmap_offset *mmo = (struct i915_mmap_offset *)node;

    switch (mmo->type) {
    case I915_MMAP_OFFSET_GTT:
        return vm_device_map(vma, &gtt_vm_ops, mmo, VM_PROT_WC);
    case I915_MMAP_OFFSET_WC:
        return vm_device_map(vma, &cpu_vm_ops, mmo, VM_PROT_WC);
    case I915_MMAP_OFFSET_UC:
        return vm_device_map(vma, &cpu_vm_ops, mmo, VM_PROT_UC);
    default:
        return vm_device_map(vma, &cpu_vm_ops, mmo, 0u);
    }
}

/* ------------------------------------------------------------------------
 * MMAP_OFFSET
 */

/**
 * GEM_MMAP_OFFSET, and GEM_MMAP_GTT, which shares its number with `flags`
 * and `extensions` 0, as Linux's i915_gem_mmap_offset_ioctl: the fake
 * offset in the device's mmap space for a mapping of the kind `flags`
 * names. EINVAL for another kind or for an extension, which none is
 * defined, EFAULT for one that cannot be read; ENODEV for write-combining
 * and uncached without PAT, for FIXED, which only discrete parts have, for
 * a mapping of an import's pages, which are another device's, and for any
 * mapping of a userptr object, whose pages the program maps already; ENOENT
 * for no such object; ENOSPC when the mmap space is full. `pad` is not
 * read, as Linux has never refused it.
 */
int32_t i915_gem_mmap_offset_ioctl(struct drm_device *dev, void *data, struct drm_file *file)
{
    struct drm_i915_gem_mmap_offset *args = (struct drm_i915_gem_mmap_offset *)data;
    struct drm_i915_gem_object *obj;
    int32_t err;

    (void)dev;
    if (args->extensions) {
        /* Linux's i915_user_extensions with no table: the header's
         * flags, reserved words and name are read, then refused. */
        uint8_t header[24];

        if (args->extensions > (uintptr_t)-1 - 8u ||
            copy_from_user(header, (const void __user *)(uintptr_t)(args->extensions + 8u),
                           sizeof(header)) != 0) {
            return -EFAULT;
        }
        return -EINVAL;
    }
    switch (args->flags) {
    case I915_MMAP_OFFSET_GTT:
    case I915_MMAP_OFFSET_WB:
        break;
    case I915_MMAP_OFFSET_WC:
    case I915_MMAP_OFFSET_UC:
        if (!pat_write_combining()) {
            return -ENODEV;
        }
        break;
    case I915_MMAP_OFFSET_FIXED:
        break;
    default:
        return -EINVAL;
    }
    obj = i915_gem_object_lookup(file, args->handle);
    if (!obj) {
        return -ENOENT;
    }
    if (args->flags == I915_MMAP_OFFSET_FIXED || i915_gem_object_is_userptr(obj) ||
        (args->flags != I915_MMAP_OFFSET_GTT && obj->base.import)) {
        err = -ENODEV;
    } else {
        err = drm_mmap_node_add(&obj->mmo[args->flags].node);
        if (err == 0) {
            args->offset = obj->mmo[args->flags].node.offset;
        }
    }
    i915_gem_object_put(obj);
    return err;
}
