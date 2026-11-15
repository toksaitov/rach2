/*
 * i915_gem_shrinker.c: the purging of buffer objects whose pages programs
 * said they can lose, when reclaim asks for memory, after Linux 6.12's
 * gem/i915_gem_shrinker.c.
 */

#include "i915.h"
#include "../../dmabuf/dmabuf.h"
#include "../../proc/smp.h"
#include "i915_drm.h"

#include "../../mm/kheap.h"
#include "../../mm/paging.h"
#include "../../mm/pin.h"

uint32_t i915_gem_object_truncate(struct drm_i915_gem_object *obj)
{
    uint32_t freed = obj->pages.held;
    int i, big = !bkl_held();

    /* As Linux's __i915_gem_object_put_pages: the programs' maps go first,
     * so that the next touch faults to an object that has no pages; the
     * walk of the address spaces under the big lock (mm/paging.c). */
    if (big) {
        bkl_enter();
    }
    for (i = 0; i < I915_MMAP_TYPES; i++) {
        paging_zap_device(&obj->mmo[i], 0, (uint32_t)(obj->base.size >> PAGE_SHIFT));
    }
    if (big) {
        bkl_exit();
    }
    unpin(obj->pin);
    obj->pin = 0;
    kfree(obj->frames);
    obj->frames = 0;
    obj->pages_pin_count = 0;
    obj->pages_given = 0;
    shmem_free(&obj->pages);
    obj->madv = __I915_MADV_PURGED;
    return freed;
}

/**
 * Whether reclaim may purge `obj` now, as Linux's can_release_pages for a
 * purgeable object: its own pages, DONTNEED, idle, not exported, and no pin
 * of its pages but its bindings', none of which is pinned. An exported
 * object is kept because another device's import maps its frames, which
 * `page` lends without a pin; Linux's attachment pins the pages instead
 * (i915_gem_dmabuf_attach), and `dma_buf` stays set while any import holds
 * the dma-buf.
 */
static int purgeable(const struct drm_i915_gem_object *obj)
{
    const struct i915_vma *v;
    uint32_t bound = 0;

    if (obj->madv != I915_MADV_DONTNEED || obj->base.import || obj->base.dma_buf ||
        i915_gem_object_is_userptr(obj) || !i915_gem_object_has_pages(obj) ||
        i915_gem_object_is_active_locked(obj)) {
        return 0;
    }
    for (v = obj->vmas; v; v = v->obj_next) {
        if (v->pin_count) {
            return 0;
        }
        bound++;
    }
    return obj->pages_pin_count == bound;
}

/* With the GEM lock held. The walk holds the object it is at, since a
 * last put on another processor takes one off the device's list; a put of
 * the walk's that is an object's last frees it here, under the lock the
 * free takes again, and waits on nothing, since a request that has not
 * retired holds every object it uses. */
uint32_t i915_gem_shrink_flags(struct drm_i915_private *i915, uint32_t want, uint32_t flags)
{
    struct drm_gem_object *base;
    uint32_t got = 0;

    lockdep_assert_held(&i915->gem.lock);
    for (base = drm_gem_object_next(&i915->drm, 0); base;
         base = drm_gem_object_next(&i915->drm, base)) {
        struct drm_i915_gem_object *obj = (struct drm_i915_gem_object *)base;

        if (got >= want) {
            drm_gem_object_put(base);
            break;
        }
        /* The object's lock, which MADVISE takes, only tried, as Linux's
         * shrinker tries it (gem/i915_gem_shrinker.c:216-220). */
        if (base->funcs != &i915_gem_object_funcs || !dma_resv_trylock(base->resv)) {
            continue;
        }
        if (purgeable(obj) && (flags & (obj->vmas ? I915_SHRINK_BOUND : I915_SHRINK_UNBOUND)) &&
            i915_gem_object_unbind_locked(obj, 0) == 0 && !obj->pages_pin_count) {
            got += i915_gem_object_truncate(obj);
            i915->gem.purged++;
        }
        dma_resv_unlock(base->resv);
    }
    return got;
}

uint32_t i915_gem_shrink(struct drm_i915_private *i915, uint32_t want)
{
    return i915_gem_shrink_flags(i915, want, I915_SHRINK_BOUND | I915_SHRINK_UNBOUND);
}

/** The shrinker's `shrink`, which reclaim calls under the big kernel lock:
 * the objects on `gem.freed` undone first, in place of Linux's free
 * worker, which runs beside the holder (gem/i915_gem_object.c:471-472),
 * since Linux's shrinker skips every object whose last reference has gone
 * (gem/i915_gem_shrinker.c:212-213), then the pool's idle shadows dropped,
 * as Linux's pool_retire makes an idle shadow purgeable for its shrinker
 * (gt/intel_gt_buffer_pool.c:107-112), then the purgeable objects purged.
 * None sleeps, and their unbinds give no way (`i915_gem_object_unbind`),
 * so `may_sleep` changes nothing and the big lock stays held throughout.
 * It only tries the GEM lock, as Linux's shrinker only tries each object's
 * lock (gem/i915_gem_shrinker.c:216-220), and frees nothing while another
 * holds it; a caller that holds it already, whose allocation reclaims,
 * frees under it, its own deferred frees among them, which Linux's worker
 * would run beside it. An object whose last put comes here is freed at
 * once, with no give-way (`gem.no_yield`), so that its frames count now,
 * and the lock goes back with the list empty. */
static uint32_t i915_gem_shrinker_scan(struct mem_shrinker *s, uint32_t want, int may_sleep)
{
    struct drm_i915_private *i915 = ((struct i915_shrinker *)s)->i915;
    uint32_t got;
    int taken = 0;
    uint8_t no_yield;

    (void)may_sleep;
    if (!mutex_held(&i915->gem.lock)) {
        if (!mutex_trylock(&i915->gem.lock)) {
            return 0;
        }
        taken = 1;
    }
    no_yield = i915->gem.no_yield;
    i915->gem.no_yield = 1;
    got = i915_gem_free_deferred_atomic(i915);
    got += i915_gem_pool_trim(i915, I915_POOL_IDLE);
    if (got < want) {
        got += i915_gem_shrink(i915, want - got);
    }
    i915->gem.no_yield = no_yield;
    i915_gem_unlock_atomic(i915, taken);
    return got;
}

void i915_gem_shrinker_register(struct drm_i915_private *i915)
{
    i915->gem.shrinker.base.shrink = i915_gem_shrinker_scan;
    i915->gem.shrinker.i915 = i915;
    mem_shrinker_register(&i915->gem.shrinker.base);
}

void i915_gem_shrinker_unregister(struct drm_i915_private *i915)
{
    if (i915->gem.shrinker.base.shrink) {
        mem_shrinker_unregister(&i915->gem.shrinker.base);
        i915->gem.shrinker.base.shrink = 0;
    }
}
