/*
 * i915_gem_prime.c: buffer objects shared with other devices through
 * dma-bufs, after Linux 6.12's gem/i915_gem_dmabuf.c. The DRM core's PRIME
 * exports an object and returns the object itself for an import of its
 * own export; this file makes the object of another device's buffer and
 * moves an exported object's domains for a program's DMA_BUF_IOCTL_SYNC.
 */

#include "i915.h"
#include "i915_drm.h"

#include "../../dmabuf/dmabuf.h"
#include "../../lib/kerrno.h"
#include "../uapi.h"

struct drm_gem_object *i915_gem_prime_import(struct drm_device *dev, struct dma_buf *buf,
                                             int32_t *err)
{
    struct drm_i915_private *i915 = to_i915(dev);
    struct drm_i915_gem_object *obj;

    if (buf->size > SHMEM_MAX_SIZE || buf->size > (uint64_t)0x7fffffffu << PAGE_SHIFT) {
        *err = -E2BIG;
        return 0;
    }
    if (!buf->ops->page || buf->size == 0 || (buf->size & (PAGE_SIZE - 1))) {
        *err = -EINVAL;
        return 0;
    }
    obj = i915_gem_object_alloc(i915, buf->size);
    if (!obj) {
        *err = -ENOMEM;
        return 0;
    }
    /* As Linux: the GTT's domain stands for one coherent with memory, in
     * neither the GPU's caches nor the processor's. */
    obj->read_domains = I915_GEM_DOMAIN_GTT;
    obj->write_domain = 0;
    return &obj->base;
}

/* Under the GEM lock, which the domain's wait lets go for its sleep. */
int32_t i915_gem_object_cpu_access(struct drm_gem_object *base, uint64_t flags)
{
    struct drm_i915_gem_object *obj = (struct drm_i915_gem_object *)base;
    struct drm_i915_private *i915 = to_i915(base->dev);
    int taken = i915_gem_lock(i915);
    int32_t err = i915_gem_object_pin_pages(obj);

    if (err != 0) {
        i915_gem_unlock(i915, taken);
        return err;
    }
    /* Linux's i915_gem_begin_cpu_access and i915_gem_end_cpu_access. */
    if (flags & DMA_BUF_SYNC_END) {
        err = i915_gem_object_set_to_gtt_domain(obj, 0);
    } else {
        err = i915_gem_object_set_to_cpu_domain(obj, (flags & DMA_BUF_SYNC_WRITE) != 0);
    }
    i915_gem_object_unpin_pages(obj);
    i915_gem_unlock(i915, taken);
    return err;
}
