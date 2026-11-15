/*
 * i915_fence.c: the fence registers, which detile the aperture's accesses
 * to a tiled object, and the swizzle of address bit 6 that the memory
 * controller gives tiles, after Linux 6.12's gt/intel_ggtt_fencing.c.
 */

#include "i915.h"
#include "i915_drm.h"
#include "i915_reg.h"

#include "../../klog.h"

/* The fence's granularity, Linux's I965_FENCE_PAGE. */
#define I965_FENCE_PAGE 4096u

/**
 * Write fence register `id` to detile [start, start + size) of the global
 * GTT with `tiling` and `stride`, or clear it when `tiling` is none. Called
 * under the big lock.
 */
void i915_fence_write(struct drm_i915_private *i915, int id, uint64_t start, uint64_t size,
                      uint32_t stride, uint32_t tiling)
{
    struct i915_hw *hw = &i915->hw;
    uint64_t val = 0;

    if (tiling != I915_TILING_NONE) {
        val = (start + size - I965_FENCE_PAGE) << 32;
        val |= start;
        val |= (uint64_t)(stride / 128u - 1u) << GEN6_FENCE_PITCH_SHIFT;
        if (tiling == I915_TILING_Y) {
            val |= 1u << I965_FENCE_TILING_Y_SHIFT;
        }
        val |= I965_FENCE_REG_VALID;
    }
    /* As Linux's i965_write_fence_reg: the fence off first, so that no
     * half-written pair is ever in force, then the high half, then the low
     * half with its valid bit, each landing before the next. The device is
     * held awake meanwhile, as Linux's fence update holds a wakeref; a
     * resume's restore runs inside the wake, whose hold only counts. */
    i915_rpm_get(i915);
    i915_write(hw, FENCE_REG_GEN6_LO(id), 0);
    i915_posting_read(hw, FENCE_REG_GEN6_LO(id));
    i915_write(hw, FENCE_REG_GEN6_HI(id), (uint32_t)(val >> 32));
    i915_write(hw, FENCE_REG_GEN6_LO(id), (uint32_t)val);
    i915_posting_read(hw, FENCE_REG_GEN6_LO(id));
    i915_rpm_put(i915);
}

/**
 * Read the swizzle the memory controller gives X and Y tiles, as Linux's
 * detect_bit_6_swizzle on graphics 6 and 7: the firmware's when it left a
 * tiled surface on the screen, which DISP_ARB_CTL then says; otherwise
 * bits 9 and 10 for X and bit 9 for Y when both channels hold DIMMs of one
 * size, which interleaves them, and none when they differ.
 */
static void i915_gem_detect_swizzle(struct drm_i915_private *i915)
{
    uint32_t x = I915_BIT_6_SWIZZLE_NONE, y = I915_BIT_6_SWIZZLE_NONE;

    if (i915->ggtt.fw_tiled) {
        if (i915_uncore_read(i915, DISP_ARB_CTL) & DISP_TILE_SURFACE_SWIZZLING) {
            x = I915_BIT_6_SWIZZLE_9_10;
            y = I915_BIT_6_SWIZZLE_9;
        }
    } else {
        uint32_t c0 = i915_uncore_read(i915, MAD_DIMM_C0);
        uint32_t c1 = i915_uncore_read(i915, MAD_DIMM_C1);

        if ((c0 & (MAD_DIMM_A_SIZE_MASK | MAD_DIMM_B_SIZE_MASK)) ==
            (c1 & (MAD_DIMM_A_SIZE_MASK | MAD_DIMM_B_SIZE_MASK))) {
            x = I915_BIT_6_SWIZZLE_9_10;
            y = I915_BIT_6_SWIZZLE_9;
        }
    }
    i915->gem.swizzle_x = x;
    i915->gem.swizzle_y = y;
}

/** Have the GPU swizzle its tiles as the memory controller does, as
 * Linux's intel_gt_init_swizzling on graphics 7, when it swizzles. */
static void i915_gem_init_swizzling(struct drm_i915_private *i915)
{
    if (i915->gem.swizzle_x != I915_BIT_6_SWIZZLE_NONE) {
        (void)i915_rmw(&i915->hw, DISP_ARB_CTL, 0, DISP_TILE_SURFACE_SWIZZLING);
        (void)i915_rmw(&i915->hw, TILECTL, 0, TILECTL_SWZCTL);
        i915_uncore_write(i915, ARB_MODE, I915_MASKED_ENABLE(ARB_MODE_SWIZZLE_IVB));
    }
}

/**
 * Set the GEM state the hardware holds: read the swizzle, clear every fence
 * and have the GPU swizzle as the memory controller does. Called once the
 * device's registers are reachable, under the big lock.
 */
void i915_gem_init_hw(struct drm_i915_private *i915)
{
    static const char *const names[] = {"none", "9", "9/10", "9/11", "9/10/11"};
    int i;

    i915_gem_detect_swizzle(i915);
    /* Every fence cleared, as Linux's intel_ggtt_restore_fences writes
     * each, whatever the firmware left. */
    for (i = 0; i < I915_NUM_FENCES; i++) {
        i915->gem.fences[i].id = i;
        i915->gem.fences[i].vma = 0;
        i915->gem.fences[i].pin_count = 0;
        i915->gem.fences[i].used = 0;
        i915->gem.fences[i].dirty = 0;
        i915_fence_write(i915, i, 0, 0, 0, I915_TILING_NONE);
    }
    i915_gem_init_swizzling(i915);
    klog(KLOG_INFO, "i915", "gem: %d fences, swizzle %s for X tiles and %s for Y", I915_NUM_FENCES,
         names[i915->gem.swizzle_x], names[i915->gem.swizzle_y]);
}

void i915_gem_restore_fences(struct drm_i915_private *i915)
{
    struct i915_vma *vma;
    int i;

    for (i = 0; i < I915_NUM_FENCES; i++) {
        vma = i915->gem.fences[i].vma;
        if (vma) {
            i915_fence_write(i915, i, vma->start, vma->fence_size, vma->obj->stride,
                             vma->obj->tiling);
        } else {
            i915_fence_write(i915, i, 0, 0, 0, I915_TILING_NONE);
        }
    }
    i915_gem_init_swizzling(i915);
}

/** Bit `bit` of `v` moved down to bit 6, as Linux's selftest swizzle_bit. */
static uint32_t swizzle_bit(unsigned bit, uint32_t v)
{
    return (v & (1u << bit)) >> (bit - 6);
}

/**
 * Return where byte `v` of a linear surface of `stride` lies in memory once
 * tiled with `tiling` and swizzled with `swizzle`, as Linux's selftest
 * tiled_offset: the arithmetic a fence does for an access through it. The
 * global GTT is 2 GiB, so 32 bits hold every offset.
 */
uint32_t i915_tiled_offset(uint32_t tiling, uint32_t stride, uint32_t swizzle, uint32_t v)
{
    uint32_t x, y, t;
    uint32_t height = tiling == I915_TILING_Y ? 32u : 8u;

    if (tiling == I915_TILING_NONE || stride == 0) {
        return v;
    }
    y = v / stride;
    x = v % stride;
    t = y / height;
    y %= height;
    v = t * stride * height;
    if (tiling == I915_TILING_X) {
        /* A tile is 8 rows of 512 bytes. */
        v += y * 512u + (x / 512u) * 4096u + x % 512u;
    } else {
        /* A tile is 8 columns of 32 rows of 16 bytes, Haswell's 128-byte Y. */
        v += y * 16u + (x / 16u) * 512u + x % 16u;
    }
    switch (swizzle) {
    case I915_BIT_6_SWIZZLE_9:
        v ^= swizzle_bit(9, v);
        break;
    case I915_BIT_6_SWIZZLE_9_10:
        v ^= swizzle_bit(9, v) ^ swizzle_bit(10, v);
        break;
    case I915_BIT_6_SWIZZLE_9_11:
        v ^= swizzle_bit(9, v) ^ swizzle_bit(11, v);
        break;
    case I915_BIT_6_SWIZZLE_9_10_11:
        v ^= swizzle_bit(9, v) ^ swizzle_bit(10, v) ^ swizzle_bit(11, v);
        break;
    default:
        break;
    }
    return v;
}
