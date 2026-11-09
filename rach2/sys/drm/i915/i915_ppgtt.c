/*
 * i915_ppgtt.c: Haswell's aliasing PPGTT, as Linux 6.12 runs it on a
 * chip of graphics version 7: one PPGTT that every context shares.
 */

#include "i915.h"

int32_t i915_ppgtt_enable_ring(struct drm_i915_private *i915, struct i915_engine *engine)
{
    /* No PPGTT yet: the ring's PP_DIR_DCLV, PP_DIR_BASE and RING_MODE_GEN7
     * keep what the stop left, and its commands use the global GTT. */
    (void)i915;
    (void)engine;
    return 0;
}
