/*
 * i915_reg_read.c: I915_REG_READ, a register read for programs, after
 * Linux 6.12's i915_ioctl.c: only the render ring's timestamp, which
 * Mesa's Vulkan drivers read to calibrate their GPU timestamps.
 */

#include "i915.h"
#include "i915_drm.h"
#include "i915_reg.h"

#include "../../lib/kerrno.h"

/* The registers a program may read, each of 8 bytes in two halves at `lo`
 * and `hi`: Linux's reg_read_whitelist for graphics 4 to 12, of which
 * Haswell is 7.5. A register's offset is a multiple of 8, so that the
 * call's low three bits are its flags. */
static const struct {
    uint32_t lo, hi;
} reg_read_allowed[] = {
    {RING_TIMESTAMP(RENDER_RING_BASE), RING_TIMESTAMP_UDW(RENDER_RING_BASE)},
};

/**
 * Read a register of `reg_read_allowed` for a program, as Linux's
 * i915_reg_read_ioctl, under forcewake and the register lock
 * (`i915_uncore_read64`): with I915_REG_READ_8B_WA in the offset's low
 * bits as Linux's intel_uncore_read64_2x32 reads it, so that a carry
 * between the halves is never seen half done; with no flag the low half
 * and then the high, as 32-bit Linux's readq does. The row runs without
 * the big lock; the register lock keeps an unbind's `i915_uncore_fini`
 * from revoking forcewake during the reads.
 *
 * @return 0 with the value in `val`; -EINVAL for a register not allowed or
 *         another flag; -ENODEV once the device is unbound; -ETIMEDOUT
 *         when the GT did not wake.
 */
int32_t i915_reg_read_ioctl(struct drm_device *dev, void *data, struct drm_file *file)
{
    struct drm_i915_reg_read *reg = (struct drm_i915_reg_read *)data;
    struct drm_i915_private *i915 = to_i915(dev);
    uint32_t flags = (uint32_t)(reg->offset & 7u), i, n, lo = 0, hi = 0;

    (void)file;
    n = sizeof(reg_read_allowed) / sizeof(reg_read_allowed[0]);
    for (i = 0; i < n && hi == 0; i++) {
        if (reg_read_allowed[i].lo == (reg->offset & ~(uint64_t)7u)) {
            lo = reg_read_allowed[i].lo;
            hi = reg_read_allowed[i].hi;
        }
    }
    if (hi == 0 || (flags != 0 && flags != I915_REG_READ_8B_WA)) {
        return -EINVAL;
    }
    return i915_uncore_read64(i915, lo, hi, flags == I915_REG_READ_8B_WA, &reg->val);
}
