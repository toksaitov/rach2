/* The i915 GEM lock's helpers (drm/i915/i915.h) for a host test of one
 * thread that links the display, PCI or reset files without i915_gem.c,
 * which has nothing for the lock to keep out. */
#include "../../../rach2/sys/drm/i915/i915.h"

/* Called at each take of the lock, a place where the kernel's caller may
 * sleep, when a row that asks whether a path sleeps sets it. */
void (*i915_gem_lock_host_hook)(void);

/** Take the GEM lock: nothing to wait for, and nothing to give back. */
int i915_gem_lock(struct drm_i915_private *i915)
{
    (void)i915;
    if (i915_gem_lock_host_hook) {
        i915_gem_lock_host_hook();
    }
    return 0;
}

/** Try the GEM lock: always had, and nothing to give back. */
int i915_gem_trylock(struct drm_i915_private *i915, int *taken)
{
    (void)i915;
    *taken = 0;
    return 1;
}

/** Give the GEM lock back. */
void i915_gem_unlock(struct drm_i915_private *i915, int taken)
{
    (void)i915;
    (void)taken;
}

/** Let the GEM lock go for a wait: nothing held. */
int i915_gem_lock_drop(struct drm_i915_private *i915)
{
    (void)i915;
    return 0;
}

/** Take the GEM lock back after a wait. */
void i915_gem_lock_retake(struct drm_i915_private *i915, int dropped)
{
    (void)i915;
    (void)dropped;
}

/** Give the GEM lock back without giving way. */
void i915_gem_unlock_atomic(struct drm_i915_private *i915, int taken)
{
    (void)i915;
    (void)taken;
}
