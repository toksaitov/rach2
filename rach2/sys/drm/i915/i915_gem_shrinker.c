/*
 * i915_gem_shrinker.c: the purging of buffer objects whose pages programs
 * said they can lose, when reclaim asks for memory, after Linux 6.12's
 * gem/i915_gem_shrinker.c.
 */

#include "i915.h"
#include "i915_drm.h"

#include "../../mm/kheap.h"
#include "../../mm/paging.h"
#include "../../mm/pin.h"

uint32_t i915_gem_object_truncate(struct drm_i915_gem_object *obj)
{
    uint32_t freed = obj->pages.held;
    int i;

    /* As Linux's __i915_gem_object_put_pages: the programs' maps go first,
     * so that the next touch faults to an object that has no pages. */
    for (i = 0; i < I915_MMAP_TYPES; i++) {
        paging_zap_device(&obj->mmo[i], 0, (uint32_t)(obj->base.size >> PAGE_SHIFT));
    }
    unpin(obj->pin);
    obj->pin = 0;
    kfree(obj->frames);
    obj->frames = 0;
    obj->pages_pin_count = 0;
    shmem_free(&obj->pages);
    obj->madv = __I915_MADV_PURGED;
    return freed;
}

/**
 * Whether reclaim may purge `obj` now, as Linux's can_release_pages for a
 * purgeable object: its own pages, DONTNEED, idle, and no pin of its pages
 * but its bindings', none of which is pinned.
 */
static int purgeable(const struct drm_i915_gem_object *obj)
{
    const struct i915_vma *v;
    uint32_t bound = 0;

    if (obj->madv != I915_MADV_DONTNEED || obj->base.import || i915_gem_object_is_userptr(obj) ||
        !i915_gem_object_has_pages(obj) || i915_gem_object_is_active(obj)) {
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

uint32_t i915_gem_shrink(struct drm_i915_private *i915, uint32_t want)
{
    struct drm_gem_object *base;
    uint32_t got = 0;

    for (base = i915->drm.objects; base && got < want; base = base->dev_next) {
        struct drm_i915_gem_object *obj = (struct drm_i915_gem_object *)base;

        if (base->funcs != &i915_gem_object_funcs || !purgeable(obj)) {
            continue;
        }
        if (i915_gem_object_unbind(obj) != 0 || obj->pages_pin_count) {
            continue;
        }
        got += i915_gem_object_truncate(obj);
        i915->gem.purged++;
    }
    return got;
}

/** The shrinker's `shrink`, which reclaim calls under the big kernel lock:
 * purging never sleeps, so `may_sleep` changes nothing. */
static uint32_t i915_gem_shrinker_scan(struct mem_shrinker *s, uint32_t want, int may_sleep)
{
    (void)may_sleep;
    return i915_gem_shrink(((struct i915_shrinker *)s)->i915, want);
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
