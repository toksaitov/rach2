/*
 * i915_vma.c: the bindings of buffer objects into the global GTT, the space
 * they take there, and their undoing, after Linux 6.12's i915_vma.c and
 * i915_gem_gtt.c.
 */

#include "i915.h"
#include "i915_drm.h"

#include "../../lib/kerrno.h"
#include "../../mm/kheap.h"
#include "../../mm/paging.h"

/* The entries a bind writes at a time. */
#define I915_BIND_CHUNK 64u

/* ------------------------------------------------------------------------
 * The space
 */

/** Round `v` up to a multiple of `align`, a power of two, or all ones when
 * that wraps. */
static uint64_t align_up(uint64_t v, uint64_t align)
{
    uint64_t r = (v + align - 1) & ~(align - 1);

    return r < v ? ~(uint64_t)0 : r;
}

/**
 * The first range of the GTT below `hi` of `size` bytes aligned to `align`
 * that no binding, no kept surface of the firmware's and none of the GT's
 * own objects touches, the lowest, or with I915_PIN_HIGH the highest, into
 * `*at`: 0, or -ENOSPC. A range in the way moves the candidate past it and
 * starts the search again.
 */
static int32_t ggtt_find(struct drm_i915_private *i915, uint64_t size, uint64_t align, uint64_t hi,
                         uint32_t flags, uint64_t *at)
{
    const struct i915_ggtt *ggtt = &i915->ggtt;
    struct i915_vma *v;
    uint64_t start;
    int i, moved = 1;

    if (hi > i915->gt.ggtt_bottom) {
        hi = i915->gt.ggtt_bottom;
    }
    if (size == 0 || size > hi) {
        return -ENOSPC;
    }
    start = (flags & I915_PIN_HIGH) ? (hi - size) & ~(align - 1) : 0;
    while (moved) {
        uint64_t a = 0, b = 0;
        int hit = 0;

        moved = 0;
        if (start > hi || size > hi - start) {
            return -ENOSPC;
        }
        for (v = i915->gem.bound; v && !hit; v = v->ggtt_next) {
            if (v->start < start + size && start < v->start + v->node_size) {
                a = v->start;
                b = v->start + v->node_size;
                hit = 1;
            }
        }
        for (i = 0; i < ggtt->nkept && !hit; i++) {
            if (ggtt->kept[i].start < start + size && start < ggtt->kept[i].end) {
                a = ggtt->kept[i].start;
                b = ggtt->kept[i].end;
                hit = 1;
            }
        }
        if (hit) {
            if (flags & I915_PIN_HIGH) {
                if (a < size) {
                    return -ENOSPC;
                }
                start = (a - size) & ~(align - 1);
            } else {
                start = align_up(b, align);
            }
            moved = 1;
        }
    }
    *at = start;
    return 0;
}

/** Put `vma`, whose `start` is set, on the GTT's list by offset. */
static void ggtt_link(struct drm_i915_private *i915, struct i915_vma *vma)
{
    struct i915_vma **link = &i915->gem.bound;

    while (*link && (*link)->start < vma->start) {
        link = &(*link)->ggtt_next;
    }
    vma->ggtt_next = *link;
    *link = vma;
}

/** Take `vma` off the GTT's list. */
static void ggtt_unlink(struct drm_i915_private *i915, struct i915_vma *vma)
{
    struct i915_vma **link;

    for (link = &i915->gem.bound; *link; link = &(*link)->ggtt_next) {
        if (*link == vma) {
            *link = vma->ggtt_next;
            return;
        }
    }
}

/**
 * Find room for `size` bytes aligned to `align` inside the aperture with
 * I915_PIN_MAPPABLE, anywhere in the GTT without, making room by eviction
 * once unless I915_PIN_NOEVICT: 0 with the offset in `*at`, or -ENOSPC.
 */
static int32_t ggtt_room(struct drm_i915_private *i915, uint64_t size, uint64_t align,
                         uint32_t flags, uint64_t *at)
{
    uint64_t hi = (flags & I915_PIN_MAPPABLE) ? i915->ggtt.mappable_end : i915->ggtt.total;
    int32_t err = ggtt_find(i915, size, align, hi, flags, at);

    if (err == -ENOSPC && !(flags & I915_PIN_NOEVICT)) {
        err = i915_gem_evict_something(i915, size, align, 0, hi);
        if (err == 0) {
            err = ggtt_find(i915, size, align, hi, flags, at);
        }
    }
    return err;
}

int32_t i915_ggtt_reserve(struct drm_i915_private *i915, uint64_t size, uint32_t flags,
                          uint64_t *offset)
{
    struct i915_vma *vma = (struct i915_vma *)kzalloc(sizeof(*vma));
    int32_t err;

    if (!vma) {
        return -ENOMEM;
    }
    err = ggtt_room(i915, size, I915_GTT_PAGE_SIZE, flags, &vma->start);
    if (err != 0) {
        kfree(vma);
        return err;
    }
    vma->node_size = vma->size = size;
    vma->mappable = vma->start + size <= i915->ggtt.mappable_end;
    vma->pin_count = 1;
    ggtt_link(i915, vma);
    *offset = vma->start;
    return 0;
}

void i915_ggtt_release(struct drm_i915_private *i915, uint64_t offset)
{
    struct i915_vma *v;

    for (v = i915->gem.bound; v; v = v->ggtt_next) {
        if (!v->obj && v->start == offset) {
            ggtt_unlink(i915, v);
            kfree(v);
            return;
        }
    }
}

/* ------------------------------------------------------------------------
 * Bindings
 */

/** Whether `vma` serves a GTT map and a fence: it lies inside the aperture,
 * with the room and alignment a fence of its object's tiling needs, as
 * Linux's i915_vma_is_map_and_fenceable. */
static int vma_fenceable(struct drm_i915_private *i915, const struct i915_vma *vma)
{
    return vma->start + vma->node_size <= i915->ggtt.mappable_end &&
           vma->node_size >= vma->fence_size && (vma->start & (vma->fence_alignment - 1)) == 0;
}

void i915_vma_update_fenceable(struct i915_vma *vma)
{
    struct drm_i915_gem_object *obj = vma->obj;
    struct drm_i915_private *i915 = to_i915(obj->base.dev);

    vma->fence_size = i915_gem_fence_size(i915, (uint32_t)vma->size, obj->tiling, obj->stride);
    vma->fence_alignment = i915_gem_fence_alignment(i915, obj->tiling);
    vma->mappable = (uint8_t)vma_fenceable(i915, vma);
}

/** Write the GTT's entries of `vma`'s view at the object's cache level:
 * 0, or `i915_ggtt_insert`'s error. */
static int32_t vma_bind(struct drm_i915_private *i915, struct i915_vma *vma)
{
    struct drm_i915_gem_object *obj = vma->obj;
    uint32_t n = (uint32_t)(vma->size >> PAGE_SHIFT), done = 0;
    uint64_t chunk[I915_BIND_CHUNK];
    int32_t err = 0;

    while (done < n) {
        uint32_t k = n - done < I915_BIND_CHUNK ? n - done : I915_BIND_CHUNK, i;

        for (i = 0; i < k; i++) {
            chunk[i] = obj->frames[vma->view_offset + done + i];
        }
        err = i915_ggtt_insert(i915, vma->start + ((uint64_t)done << PAGE_SHIFT), chunk, k,
                               obj->cache_level);
        if (err != 0) {
            /* A failed insert writes nothing; the chunks before it go. */
            if (done) {
                (void)i915_ggtt_clear_range(i915, vma->start, (uint64_t)done << PAGE_SHIFT);
            }
            return err;
        }
        done += k;
    }
    vma->cache = obj->cache_level;
    return 0;
}

/** The binding of `obj`'s view of `view_pages` pages from `view_offset`, 0
 * pages for the whole object, or NULL. */
static struct i915_vma *vma_lookup(struct drm_i915_gem_object *obj, uint32_t view_offset,
                                   uint32_t view_pages)
{
    struct i915_vma *v;

    for (v = obj->vmas; v; v = v->obj_next) {
        if (v->view_offset == view_offset && v->view_pages == view_pages) {
            return v;
        }
    }
    return 0;
}

int32_t i915_vma_pin(struct drm_i915_gem_object *obj, uint32_t view_offset, uint32_t view_pages,
                     uint32_t flags, uint32_t align, struct i915_vma **out)
{
    struct drm_i915_private *i915 = to_i915(obj->base.dev);
    uint32_t pages = (uint32_t)(obj->base.size >> PAGE_SHIFT);
    struct i915_vma *vma;
    uint64_t size;
    int32_t err;

    if (view_pages && ((uint64_t)view_offset + view_pages > pages || view_pages == pages)) {
        return -EINVAL;
    }
    if (align < I915_GTT_PAGE_SIZE) {
        align = I915_GTT_PAGE_SIZE;
    }
    vma = vma_lookup(obj, view_offset, view_pages);
    if (vma && ((flags & I915_PIN_MAPPABLE) && !vma->mappable)) {
        /* Misplaced, as Linux's i915_vma_misplaced: moved, unless held. */
        err = i915_vma_unbind(vma);
        if (err != 0) {
            return err == -EAGAIN ? -ENOSPC : err;
        }
        vma = 0;
    }
    if (vma && (vma->start & (align - 1))) {
        err = i915_vma_unbind(vma);
        if (err != 0) {
            return err == -EAGAIN ? -ENOSPC : err;
        }
        vma = 0;
    }
    if (vma) {
        vma->pin_count++;
        *out = vma;
        return 0;
    }
    vma = (struct i915_vma *)kzalloc(sizeof(*vma));
    if (!vma) {
        return -ENOMEM;
    }
    vma->obj = obj;
    vma->view_offset = view_offset;
    vma->view_pages = view_pages;
    vma->size = size = (uint64_t)(view_pages ? view_pages : pages) << PAGE_SHIFT;
    vma->fence_size = i915_gem_fence_size(i915, (uint32_t)size, obj->tiling, obj->stride);
    vma->fence_alignment = i915_gem_fence_alignment(i915, obj->tiling);
    vma->node_size = size;
    if (flags & I915_PIN_MAPPABLE) {
        if (vma->fence_size > size) {
            vma->node_size = vma->fence_size;
        }
        if (vma->fence_alignment > align) {
            align = vma->fence_alignment;
        }
    }
    err = i915_gem_object_pin_pages(obj);
    if (err == 0) {
        err = ggtt_room(i915, vma->node_size, align, flags, &vma->start);
        if (err == 0) {
            err = vma_bind(i915, vma);
        }
        if (err != 0) {
            i915_gem_object_unpin_pages(obj);
        }
    }
    if (err != 0) {
        kfree(vma);
        return err;
    }
    vma->mappable = (uint8_t)vma_fenceable(i915, vma);
    vma->pin_count = 1;
    vma->obj_next = obj->vmas;
    obj->vmas = vma;
    ggtt_link(i915, vma);
    *out = vma;
    return 0;
}

void i915_vma_unpin(struct i915_vma *vma)
{
    if (vma->pin_count) {
        vma->pin_count--;
    }
}

void i915_vma_revoke_mmap(struct i915_vma *vma)
{
    struct drm_i915_gem_object *obj = vma->obj;
    uint32_t pages = (uint32_t)(vma->size >> PAGE_SHIFT);

    if (!vma->userfault) {
        return;
    }
    paging_zap_device(&obj->mmo[I915_MMAP_OFFSET_GTT], vma->view_offset, pages);
    vma->userfault = 0;
    obj->userfault_count--;
}

void i915_vma_flush_writes(struct i915_vma *vma)
{
    struct drm_i915_private *i915 = to_i915(vma->obj->base.dev);

    if (!vma->ggtt_write) {
        return;
    }
    arch_dma_fence();
    if (vma->mappable) {
        i915_aperture_sync(&i915->hw, vma->start, vma->size, 1);
    }
    vma->ggtt_write = 0;
}

int32_t i915_vma_unbind(struct i915_vma *vma)
{
    struct drm_i915_gem_object *obj = vma->obj;
    struct drm_i915_private *i915 = to_i915(obj->base.dev);
    struct i915_vma **link;

    if (vma->pin_count) {
        return -EAGAIN;
    }
    /* As Linux's __i915_vma_evict: programs fault again, the writes through
     * the aperture reach memory, and only then is the fence let go. */
    i915_vma_revoke_mmap(vma);
    i915_vma_flush_writes(vma);
    i915_vma_revoke_fence(vma);
    (void)i915_ggtt_clear_range(i915, vma->start, vma->node_size);
    i915_gem_object_unpin_pages(obj);
    ggtt_unlink(i915, vma);
    for (link = &obj->vmas; *link; link = &(*link)->obj_next) {
        if (*link == vma) {
            *link = vma->obj_next;
            break;
        }
    }
    kfree(vma);
    return 0;
}

int32_t i915_gem_object_unbind(struct drm_i915_gem_object *obj)
{
    struct i915_vma *vma = obj->vmas, *next;
    int32_t err = 0;

    while (vma) {
        next = vma->obj_next;
        if (i915_vma_unbind(vma) != 0) {
            err = -EAGAIN;
        }
        vma = next;
    }
    return err;
}

struct i915_vma *i915_vma_for_page(struct drm_i915_gem_object *obj, uint32_t pgoff)
{
    struct i915_vma *v;

    for (v = obj->vmas; v; v = v->obj_next) {
        uint32_t n = (uint32_t)(v->size >> PAGE_SHIFT);

        if (v->mappable && pgoff >= v->view_offset && pgoff - v->view_offset < n) {
            return v;
        }
    }
    return 0;
}
