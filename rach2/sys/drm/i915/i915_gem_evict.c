/*
 * i915_gem_evict.c: room in the global GTT made by undoing idle bindings,
 * least recently used first, after Linux 6.12's i915_gem_evict.c.
 */

#include "i915.h"

#include "../../lib/kerrno.h"

/** Whether eviction may undo `vma`: a binding of an object, not pinned,
 * whose object the GPU has finished with, as Linux's mark_free asks. */
static int evictable(const struct i915_vma *vma)
{
    return vma->obj && vma->pin_count == 0 && !i915_gem_object_is_active(vma->obj);
}

/*
 * As Linux's drm_mm scan: the idle bindings are added to the scan in the
 * order of their last use, each counted as free, until a range of the size
 * fits; of the scanned, only those that lie in the range found are undone,
 * so that a binding used more recently than the hole needed stays. Linux
 * goes on to idle the GPU and scan its busy bindings too; a binding the
 * GPU may still use is never undone here, and the caller sees -ENOSPC.
 */
int32_t i915_gem_evict_something(struct drm_i915_private *i915, uint64_t size, uint64_t align,
                                 uint64_t start, uint64_t end, uint32_t flags)
{
    struct i915_vma *v, *next, *last = 0;
    uint64_t at = 0;
    int32_t err = -ENOSPC;

    if (end <= start || size > end - start) {
        return -ENOSPC;
    }
    for (v = i915->gem.lru_head; v; v = v->lru_next) {
        if (!evictable(v) || v->start >= end || v->start + v->node_size <= start) {
            continue;
        }
        v->evict = 1;
        last = v;
        err = i915_ggtt_find(i915, size, align, end, flags, &at);
        if (err == 0 && at >= start) {
            break;
        }
        err = -ENOSPC;
    }
    /* Each binding the scan marked is unmarked; those in the range go. */
    for (v = i915->gem.lru_head; v && last; v = next) {
        next = v->lru_next;
        if (v->evict) {
            v->evict = 0;
            if (err == 0 && v->start < at + size && at < v->start + v->node_size) {
                (void)i915_vma_unbind(v);
                i915->gem.evictions++;
            }
        }
        if (v == last) {
            break;
        }
    }
    return err;
}
