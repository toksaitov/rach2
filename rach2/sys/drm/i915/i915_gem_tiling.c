/*
 * i915_gem_tiling.c: the tiling of buffer objects and the fence registers
 * that detile their GTT maps, after Linux 6.12's gem/i915_gem_tiling.c and
 * the fence management of gt/intel_ggtt_fencing.c.
 */

#include "i915.h"
#include "i915_drm.h"
#include "i915_reg.h"

#include "../../lib/kerrno.h"

/* Linux's I965_FENCE_PAGE, the granularity of a fence's range. */
#define I965_FENCE_PAGE 4096u

/** A tile's height in rows: 8 for X, 32 for Y, 1 untiled. */
static uint32_t tile_height(uint32_t tiling)
{
    return tiling == I915_TILING_Y ? 32u : tiling == I915_TILING_X ? 8u : 1u;
}

uint32_t i915_gem_tile_row_size(uint32_t tiling, uint32_t stride)
{
    return tiling == I915_TILING_NONE ? 0 : stride * tile_height(tiling);
}

uint32_t i915_gem_fence_size(struct drm_i915_private *i915, uint32_t size, uint32_t tiling,
                             uint32_t stride)
{
    uint32_t row;
    uint64_t n;

    (void)i915;
    if (tiling == I915_TILING_NONE || stride == 0) {
        return size;
    }
    /* As Linux on graphics 4 and later: whole tile rows. */
    row = stride * tile_height(tiling);
    n = (uint64_t)(size / row + (size % row != 0)) * row;
    return n > 0xfffff000u ? 0xfffff000u : (uint32_t)n;
}

uint32_t i915_gem_fence_alignment(struct drm_i915_private *i915, uint32_t tiling)
{
    (void)i915;
    (void)tiling;
    /* A page, untiled for the GTT and tiled for the fence alike. */
    return I965_FENCE_PAGE;
}

/** Whether `stride` suits `tiling` on Haswell, as Linux's i915_tiling_ok:
 * at most GEN7_FENCE_MAX_PITCH_VAL units of 128 bytes, and a multiple of a
 * tile's width, 512 bytes for X and 128 for Y. */
static int tiling_ok(uint32_t tiling, uint32_t stride)
{
    uint32_t width;

    if (tiling == I915_TILING_NONE) {
        return 1;
    }
    if (tiling > I915_TILING_Y) {
        return 0;
    }
    if (stride / 128u > GEN7_FENCE_MAX_PITCH_VAL) {
        return 0;
    }
    width = tiling == I915_TILING_Y ? 128u : 512u;
    return stride != 0 && stride % width == 0;
}

/* ------------------------------------------------------------------------
 * Fences
 */

/**
 * Give `fence` to `vma`, or with NULL take it from its binding, and write
 * the register, as Linux's fence_update: the old binding's writes through
 * the aperture reach memory first, and when the fence changes hands the
 * programs mapping the old binding fault again. 0, or -EINVAL for a binding
 * that cannot take a fence.
 */
static int32_t fence_update(struct drm_i915_private *i915, struct i915_fence_reg *fence,
                            struct i915_vma *vma)
{
    struct i915_vma *old;

    if (vma && !vma->mappable) {
        return -EINVAL;
    }
    fence->dirty = 0;
    old = fence->vma;
    fence->vma = 0;
    if (old) {
        i915_vma_flush_writes(old);
        if (old != vma) {
            i915_vma_revoke_mmap(old);
            old->fence = 0;
        }
        fence->used = 0;
    }
    fence->vma = vma;
    if (vma) {
        i915_fence_write(i915, fence->id, vma->start, vma->fence_size, vma->obj->stride,
                         vma->obj->tiling);
        vma->fence = fence;
        fence->used = ++i915->gem.fence_clock;
    } else {
        i915_fence_write(i915, fence->id, 0, 0, 0, I915_TILING_NONE);
    }
    return 0;
}

/** A fence to take, as Linux's fence_find: a free one first, then the one
 * least recently used that nobody pins; NULL when every one is pinned. */
static struct i915_fence_reg *fence_find(struct drm_i915_private *i915)
{
    struct i915_fence_reg *best = 0;
    int i;

    for (i = 0; i < I915_NUM_FENCES; i++) {
        struct i915_fence_reg *f = &i915->gem.fences[i];

        if (f->pin_count) {
            continue;
        }
        if (!f->vma) {
            return f;
        }
        if (!best || f->used < best->used) {
            best = f;
        }
    }
    return best;
}

int32_t i915_vma_pin_fence(struct i915_vma *vma)
{
    struct drm_i915_private *i915 = to_i915(vma->obj->base.dev);
    struct i915_vma *set = vma->obj->tiling != I915_TILING_NONE ? vma : 0;
    struct i915_fence_reg *fence;
    int32_t err;

    if (vma->fence) {
        fence = vma->fence;
        fence->pin_count++;
        if (!fence->dirty) {
            fence->used = ++i915->gem.fence_clock;
            return 0;
        }
    } else if (set) {
        fence = fence_find(i915);
        if (!fence) {
            return -ENOBUFS;
        }
        fence->pin_count++;
    } else {
        return 0;
    }
    err = fence_update(i915, fence, set);
    if (err == 0 && set) {
        return 0;
    }
    fence->pin_count--;
    return err;
}

void i915_vma_unpin_fence(struct i915_vma *vma)
{
    if (vma->fence && vma->fence->pin_count) {
        vma->fence->pin_count--;
    }
}

void i915_vma_revoke_fence(struct i915_vma *vma)
{
    struct i915_fence_reg *fence = vma->fence;

    if (!fence) {
        return;
    }
    fence->vma = 0;
    fence->used = 0;
    fence->dirty = 0;
    vma->fence = 0;
    i915_fence_write(to_i915(vma->obj->base.dev), fence->id, 0, 0, 0, I915_TILING_NONE);
}

/* ------------------------------------------------------------------------
 * SET_TILING and GET_TILING
 */

/**
 * Change the object's layout, as Linux's i915_gem_object_set_tiling: the
 * bindings in the aperture that a fence of the new layout no longer fits
 * are undone, the rest learn the new fence's size and their fences are
 * rewritten at the next use, and the GTT maps fault again. 0, or -EAGAIN
 * when a binding that must go is held.
 */
static int32_t set_tiling(struct drm_i915_gem_object *obj, uint32_t tiling, uint32_t stride)
{
    struct drm_i915_private *i915 = to_i915(obj->base.dev);
    struct i915_vma *vma, *next;
    int32_t err;

    if (obj->tiling == tiling && obj->stride == stride) {
        return 0;
    }
    /* Linux's i915_gem_object_fence_prepare. */
    if (tiling != I915_TILING_NONE) {
        for (vma = obj->vmas; vma; vma = next) {
            next = vma->obj_next;
            if (!vma->mappable) {
                continue;
            }
            if (vma->node_size >= i915_gem_fence_size(i915, (uint32_t)vma->size, tiling, stride) &&
                (vma->start & (i915_gem_fence_alignment(i915, tiling) - 1)) == 0) {
                continue;
            }
            err = i915_vma_unbind(vma);
            if (err != 0) {
                return err;
            }
        }
    }
    obj->tiling = tiling;
    obj->stride = stride;
    for (vma = obj->vmas; vma; vma = vma->obj_next) {
        i915_vma_update_fenceable(vma);
        if (vma->fence) {
            vma->fence->dirty = 1;
        }
    }
    i915_gem_object_release_mmap_gtt(obj);
    return 0;
}

/**
 * GEM_SET_TILING, as Linux's i915_gem_set_tiling_ioctl: ENOENT for no such
 * object, ENXIO for a userptr object, EINVAL for a layout the chip cannot
 * fence; otherwise the swizzle
 * programs must apply is returned with the layout. Linear takes stride 0.
 * The object's layout and stride are written back even on an error, as
 * Linux keeps that ABI.
 */
int32_t i915_gem_set_tiling_ioctl(struct drm_device *dev, void *data, struct drm_file *file)
{
    struct drm_i915_gem_set_tiling *args = (struct drm_i915_gem_set_tiling *)data;
    struct drm_i915_private *i915 = to_i915(dev);
    struct drm_i915_gem_object *obj = i915_gem_object_lookup(file, args->handle);
    int32_t err;

    if (!obj) {
        return -ENOENT;
    }
    if (i915_gem_object_is_userptr(obj)) {
        /* As Linux: a proxy object's layout is not the program's to set. */
        i915_gem_object_put(obj);
        return -ENXIO;
    }
    if (!tiling_ok(args->tiling_mode, args->stride)) {
        i915_gem_object_put(obj);
        return -EINVAL;
    }
    if (args->tiling_mode == I915_TILING_NONE) {
        args->swizzle_mode = I915_BIT_6_SWIZZLE_NONE;
        args->stride = 0;
    } else {
        args->swizzle_mode =
            args->tiling_mode == I915_TILING_X ? i915->gem.swizzle_x : i915->gem.swizzle_y;
    }
    err = set_tiling(obj, args->tiling_mode, args->stride);
    args->stride = obj->stride;
    args->tiling_mode = obj->tiling;
    i915_gem_object_put(obj);
    return err;
}

/** GEM_GET_TILING, as Linux's i915_gem_get_tiling_ioctl: the object's
 * layout and its swizzle, which is also the physical one; ENOENT for no
 * such object. */
int32_t i915_gem_get_tiling_ioctl(struct drm_device *dev, void *data, struct drm_file *file)
{
    struct drm_i915_gem_get_tiling *args = (struct drm_i915_gem_get_tiling *)data;
    struct drm_i915_private *i915 = to_i915(dev);
    struct drm_i915_gem_object *obj = i915_gem_object_lookup(file, args->handle);

    if (!obj) {
        return -ENOENT;
    }
    args->tiling_mode = obj->tiling;
    switch (obj->tiling) {
    case I915_TILING_X:
        args->swizzle_mode = i915->gem.swizzle_x;
        break;
    case I915_TILING_Y:
        args->swizzle_mode = i915->gem.swizzle_y;
        break;
    default:
        args->swizzle_mode = I915_BIT_6_SWIZZLE_NONE;
        break;
    }
    args->phys_swizzle_mode = args->swizzle_mode;
    i915_gem_object_put(obj);
    return 0;
}
