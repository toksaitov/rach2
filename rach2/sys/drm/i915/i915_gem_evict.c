/*
 * i915_gem_evict.c: room in the global GTT made by undoing idle bindings,
 * after Linux 6.12's i915_gem_evict.c. Until eviction is written, a binding
 * that does not fit fails.
 */

#include "i915.h"

#include "../../lib/kerrno.h"

int32_t i915_gem_evict_something(struct drm_i915_private *i915, uint64_t size, uint64_t align,
                                 uint64_t start, uint64_t end)
{
    (void)i915;
    (void)size;
    (void)align;
    (void)start;
    (void)end;
    return -ENOSPC;
}
