/* The i915 runtime PM's wakerefs (drm/i915/i915.h) for a host test that
 * links the GTT, fence, display or PCI files without i915_runtime_pm.c:
 * the device never suspends, and the wakerefs are counted in `usage`. */
#include "../../../rach2/sys/drm/i915/i915.h"

/** Start with the bind's wakeref held and the device active. */
void i915_rpm_init(struct drm_i915_private *i915)
{
    i915->runtime_pm.usage = 1;
    i915->runtime_pm.status = I915_RPM_ACTIVE;
}

/** Put the bind's wakeref; nothing suspends. */
void i915_rpm_enable(struct drm_i915_private *i915)
{
    i915_rpm_put(i915);
}

/** Hold the device for good, as the unplug does. */
void i915_rpm_disable(struct drm_i915_private *i915)
{
    i915_rpm_get(i915);
}

/** Take a wakeref of a device that is always awake. */
void i915_rpm_get(struct drm_i915_private *i915)
{
    i915->runtime_pm.usage++;
}

/** Take a wakeref: 1, since the device is always awake. */
int i915_rpm_get_if_active(struct drm_i915_private *i915)
{
    i915_rpm_get(i915);
    return 1;
}

/** Put a wakeref; the count does not go below 0. */
void i915_rpm_put(struct drm_i915_private *i915)
{
    if (i915->runtime_pm.usage != 0) {
        i915->runtime_pm.usage--;
    }
}

/** Take a wakeref for a GTT clear: 1, since nothing is ever deferred. */
int i915_rpm_get_or_defer_clear(struct drm_i915_private *i915, uint64_t offset, uint64_t size)
{
    (void)offset;
    (void)size;
    return i915_rpm_get_if_active(i915);
}

/** The wakerefs held now. */
uint32_t i915_rpm_usage(struct drm_i915_private *i915)
{
    return i915->runtime_pm.usage;
}
