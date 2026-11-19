/* The i915 runtime power management's calls (drm/i915/i915.h) for a host
 * test that links the driver's files without i915_runtime_pm.c: the model
 * sleeps only when a test sets `hw.asleep`, which a get clears, so a hold
 * otherwise only counts, and a test may read the count. */
#include "../../../rach2/sys/drm/i915/i915.h"

/* The holds taken and not let go, over every device. */
int i915_rpm_host_holds;

/** Nothing to make: the model is awake. */
void i915_rpm_init(struct drm_i915_private *i915)
{
    (void)i915;
}

/** Nothing to register: the model never sleeps. */
void i915_rpm_register(struct drm_i915_private *i915)
{
    (void)i915;
}

/** Nothing to wake. */
void i915_rpm_unplug(struct drm_i915_private *i915)
{
    (void)i915;
}

/** Count a hold, and wake a device a test marked asleep, as the driver's
 * get resumes one that sleeps. */
void i915_rpm_get(struct drm_i915_private *i915)
{
    i915->hw.asleep = 0;
    i915_rpm_host_holds++;
}

/** Count a hold: the model is always awake. */
int i915_rpm_get_if_awake(struct drm_i915_private *i915)
{
    (void)i915;
    i915_rpm_host_holds++;
    return 1;
}

/** Count a hold. */
void i915_rpm_get_noresume(struct drm_i915_private *i915)
{
    (void)i915;
    i915_rpm_host_holds++;
}

/** Let a hold go. */
void i915_rpm_put(struct drm_i915_private *i915)
{
    (void)i915;
    i915_rpm_host_holds--;
}

/** Count the fault as the driver does. */
void i915_rpm_gtt_fault(struct drm_i915_private *i915)
{
    i915->rpm.faults++;
}

/** The DRM core's hold. */
void i915_drm_pm_get(struct drm_device *dev)
{
    i915_rpm_get(to_i915(dev));
}

/** The DRM core's hold let go. */
void i915_drm_pm_put(struct drm_device *dev)
{
    i915_rpm_put(to_i915(dev));
}

/** A pipe's hold, taken while it is active, as the driver's. */
void intel_crtc_rpm_sync(struct intel_crtc *c)
{
    if (c->cfg.active && !c->rpm_held) {
        c->rpm_held = 1;
        i915_rpm_host_holds++;
    } else if (!c->cfg.active && c->rpm_held) {
        c->rpm_held = 0;
        i915_rpm_host_holds--;
    }
}
