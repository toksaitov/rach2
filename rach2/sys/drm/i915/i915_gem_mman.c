/*
 * i915_gem_mman.c: programs' mappings of buffer objects, of their pages
 * write-back, write-combining or uncached, and through the aperture, whose
 * faults bind the object and take a fence, after Linux 6.12's
 * gem/i915_gem_mman.c.
 */

#include "i915.h"
#include "i915_drm.h"

#include "../../arch/arch.h"
#include "../../arch/x86/pat.h"
#include "../../lib/kerrno.h"
#include "../../lib/kstring.h"
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

static int mmo_access(struct drm_i915_gem_object *obj, uint64_t offset, uint8_t *b, uint32_t len,
                      int write);

/**
 * The `access` of every mapping of an object, for ptrace and
 * /proc/PID/mem, as Linux's vm_access: `len` bytes at the byte `offset`
 * through the object's own pages, whatever the mapping's kind, a GTT map's
 * as they lie in memory, after the writes made through the aperture are
 * flushed. 0; -EINVAL past the object's end; -ENODEV for an object without
 * pages of its own, an import or a userptr object, or once the device is
 * gone; -ENOMEM when a page cannot be had or mapped.
 */
static int mmo_vm_access(void *data, uint64_t offset, void *buf, uint32_t len, int write)
{
    struct drm_i915_gem_object *obj = mmo_object((struct i915_mmap_offset *)data);
    struct drm_i915_private *i915 = to_i915(obj->base.dev);
    int rc, taken;

    if (offset > obj->base.size || len > obj->base.size - offset) {
        return -EINVAL;
    }
    if (obj->base.dev->unplugged || obj->base.import || i915_gem_object_is_userptr(obj)) {
        return -ENODEV;
    }
    taken = i915_gem_lock(i915);
    rc = mmo_access(obj, offset, (uint8_t *)buf, len, write);
    i915_gem_unlock(i915, taken);
    return rc;
}

/** The copy of `mmo_vm_access`, under the GEM lock. */
static int mmo_access(struct drm_i915_gem_object *obj, uint64_t offset, uint8_t *b, uint32_t len,
                      int write)
{
    struct i915_vma *v;
    uint32_t done = 0;

    for (v = obj->vmas; v; v = v->obj_next) {
        i915_vma_flush_writes(v);
    }
    while (done < len) {
        uint64_t at = offset + done;
        uint32_t in = (uint32_t)(at & (PAGE_SIZE - 1));
        uint32_t n = len - done < PAGE_SIZE - in ? len - done : PAGE_SIZE - in;
        uintptr_t phys = i915_gem_object_frame(obj, (uint32_t)(at >> PAGE_SHIFT));
        uint8_t *va = phys ? (uint8_t *)kmap_temp(phys) : 0;

        if (!va) {
            return -ENOMEM;
        }
        if (write) {
            k_memcpy(va + in, b + done, n);
            if (!(obj->cache_coherent & I915_BO_CACHE_COHERENT_FOR_WRITE)) {
                arch_dma_flush(va + in, n);
            }
        } else {
            if (!(obj->cache_coherent & I915_BO_CACHE_COHERENT_FOR_READ)) {
                arch_dma_flush(va + in, n);
            }
            k_memcpy(b + done, va + in, n);
        }
        kunmap_temp(va);
        done += n;
    }
    return 0;
}

/* ------------------------------------------------------------------------
 * Mappings of the pages
 */

/** The `page` of a mapping of the object's own pages: its frame, given on
 * the first touch; -1 past its end or once the device is gone. */
static int cpu_vm_page(void *data, uint32_t pgoff, uintptr_t *phys)
{
    struct drm_i915_gem_object *obj = mmo_object((struct i915_mmap_offset *)data);
    struct drm_i915_private *i915 = to_i915(obj->base.dev);
    uintptr_t p;
    int taken;

    if (obj->base.dev->unplugged) {
        return -1;
    }
    taken = i915_gem_lock(i915);
    p = i915_gem_object_frame(obj, pgoff);
    i915_gem_unlock(i915, taken);
    if (!p) {
        return -1;
    }
    *phys = p;
    return 0;
}

static const struct vm_device_ops cpu_vm_ops = {cpu_vm_page, mmo_vm_put, mmo_vm_access, 0};

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
 * The binding a GTT fault at `pgoff` maps, as Linux's vm_fault_gtt tries
 * three: the whole object inside the aperture when it is there or fits
 * without evicting or waiting; then the view of at least
 * I915_MIN_CHUNK_PAGES around the page, rounded to whole tile rows
 * (compute_partial_view), without waiting when that is the whole object;
 * then that view as a partial one, even of the whole object, which is a
 * binding apart, so that a binding the GPU still uses outside the aperture
 * is never waited for. Pinned, which the caller drops.
 */
static int32_t gtt_fault_pin(struct drm_i915_gem_object *obj, uint32_t pgoff, struct i915_vma **vma)
{
    uint32_t pages = (uint32_t)(obj->base.size >> PAGE_SHIFT);
    uint32_t chunk = I915_MIN_CHUNK_PAGES, row, offset = 0, n = pages;

    if (i915_vma_pin(obj, 0, 0, I915_PIN_MAPPABLE | I915_PIN_NOEVICT | I915_PIN_NONBLOCK, 0, vma) ==
        0) {
        return 0;
    }
    row = i915_gem_tile_row_size(obj->tiling, obj->stride) >> PAGE_SHIFT;
    if (row > 1) {
        chunk = (chunk + row - 1) / row * row;
    }
    if (chunk < pages) {
        offset = pgoff / chunk * chunk;
        n = pages - offset < chunk ? pages - offset : chunk;
    }
    if (n == pages && i915_vma_pin(obj, 0, 0, I915_PIN_MAPPABLE | I915_PIN_NONBLOCK, 0, vma) == 0) {
        return 0;
    }
    return i915_vma_pin(obj, offset, n, I915_PIN_MAPPABLE, 0, vma);
}

/**
 * What a GTT fault's error `err` makes of the access, as Linux's
 * i915_error_to_vmf_fault: a signal that ended a wait, or no room or fence
 * for now, runs it again (1), so that the signal is handled or the GPU
 * moves on; any other error raises SIGBUS (-1).
 */
static int gtt_fault_error(int32_t err)
{
    switch (err) {
    case -EINTR:
    case -EAGAIN:
    case -ENOSPC:
    case -EBUSY:
    case -ENOBUFS:
        return 1;
    default:
        return -1;
    }
}

/**
 * The `page` of a GTT map, as Linux's vm_fault_gtt: the object bound inside
 * the aperture and fenced for its tiling, and the aperture's page mapped,
 * the binding marked as mapped by programs so that unbinding it or moving
 * its fence revokes the mapping. A partial view evicts idle bindings for
 * its room, waiting for the GPU, until a signal, when only busy bindings
 * stand in the way. An error gives what `gtt_fault_error` says: the access
 * again for a signal or for no room or fence yet, SIGBUS for the rest.
 */
static int gtt_vm_page(void *data, uint32_t pgoff, uintptr_t *phys)
{
    struct i915_mmap_offset *mmo = (struct i915_mmap_offset *)data;
    struct drm_i915_gem_object *obj = mmo_object(mmo);
    struct drm_i915_private *i915 = to_i915(obj->base.dev);
    struct i915_vma *vma;
    int32_t err;
    int taken;

    if (obj->base.dev->unplugged || (uint64_t)pgoff >= obj->base.size >> PAGE_SHIFT) {
        return -1;
    }
    /* The aperture answers only while the device is awake; the map stays
     * until a sleep revokes it, as Linux's vm_fault_gtt holds a wakeref and
     * then the userfault wakeref's 250 ms. */
    i915_rpm_get(i915);
    taken = i915_gem_lock(i915);
    err = gtt_fault_pin(obj, pgoff, &vma);
    if (err != 0) {
        i915_gem_unlock(i915, taken);
        i915_rpm_put(i915);
        return gtt_fault_error(err);
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
            i915_rpm_gtt_fault(i915);
            /* The fault does not say whether it writes; the writes are
             * flushed before the binding or its fence goes either way. */
            vma->ggtt_write = 1;
        }
        i915_vma_unpin_fence(vma);
    }
    i915_vma_unpin(vma);
    i915_gem_unlock(i915, taken);
    i915_rpm_put(i915);
    return err == 0 ? 0 : gtt_fault_error(err);
}

static const struct vm_device_ops gtt_vm_ops = {gtt_vm_page, mmo_vm_put, mmo_vm_access, 0};

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

/* ------------------------------------------------------------------------
 * GEM_MMAP
 */

/* What GEM_MMAP maps: the object and the cache type of its pages. */
struct gem_mmap {
    struct drm_i915_gem_object *obj;
    uint32_t type;
    uint32_t pgoff;
};

/** The `map` of mmap_device for GEM_MMAP: the object's pages from `pgoff`,
 * the region holding a reference to the object, which mmo_vm_put drops. */
static int32_t gem_mmap_region(vma_t *vma, void *arg)
{
    struct gem_mmap *g = (struct gem_mmap *)arg;
    int32_t rc;

    vma->pgoff = g->pgoff;
    drm_gem_object_get(&g->obj->base);
    rc = vm_device_map(vma, &cpu_vm_ops, &g->obj->mmo[g->type],
                       g->type == I915_MMAP_OFFSET_WC ? VM_PROT_WC : 0u);
    if (rc != 0) {
        i915_gem_object_put(g->obj);
    }
    return rc;
}

/**
 * GEM_MMAP, as Linux's i915_gem_mmap_ioctl: `size` bytes of the object's
 * pages from `offset` mapped shared into the caller, readable and
 * writable, write-back or under I915_MMAP_WC write-combining, the address
 * in `addr_ptr`; the mapping holds the object, as Linux's holds its shmem
 * file. EINVAL for another flag, for an offset or size outside the object
 * or an offset not a multiple of the page, and for a size of 0; ENODEV for
 * write-combining without PAT; ENOENT for no such object; ENXIO for an
 * import or a userptr object, which have no pages of their own to map;
 * ENOMEM with no room in the address space.
 */
int32_t i915_gem_mmap_ioctl(struct drm_device *dev, void *data, struct drm_file *file)
{
    struct drm_i915_gem_mmap *args = (struct drm_i915_gem_mmap *)data;
    struct gem_mmap g;
    long addr;

    (void)dev;
    if (args->flags & ~(uint64_t)I915_MMAP_WC) {
        return -EINVAL;
    }
    if ((args->flags & I915_MMAP_WC) && !pat_write_combining()) {
        return -ENODEV;
    }
    g.obj = i915_gem_object_lookup(file, args->handle);
    if (!g.obj) {
        return -ENOENT;
    }
    g.type = (args->flags & I915_MMAP_WC) ? I915_MMAP_OFFSET_WC : I915_MMAP_OFFSET_WB;
    g.pgoff = (uint32_t)(args->offset >> PAGE_SHIFT);
    if (g.obj->base.import || i915_gem_object_is_userptr(g.obj)) {
        addr = -ENXIO;
    } else if (args->offset >= g.obj->base.size || args->size > g.obj->base.size - args->offset ||
               (args->offset & (PAGE_SIZE - 1u))) {
        addr = -EINVAL;
    } else {
        addr =
            mmap_device((uintptr_t)args->size, VM_PROT_READ | VM_PROT_WRITE, gem_mmap_region, &g);
    }
    i915_gem_object_put(g.obj);
    if (addr < 0 && addr > -4096L) {
        return (int32_t)addr;
    }
    args->addr_ptr = (uint64_t)(uintptr_t)addr;
    return 0;
}
