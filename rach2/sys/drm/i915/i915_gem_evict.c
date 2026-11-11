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
 * so that a binding used more recently than the hole needed stays. 0, or
 * -ENOSPC.
 */
static int32_t evict_scan(struct drm_i915_private *i915, uint64_t size, uint64_t align,
                          uint64_t start, uint64_t end, uint32_t flags)
{
    struct i915_vma *v, *next, *last = 0;
    uint64_t at = 0;
    int32_t err = -ENOSPC;

    for (v = i915->gem.lru_head; v; v = v->lru_next) {
        if (!evictable(v) || v->start >= end || v->start + v->node_size <= start) {
            continue;
        }
        v->evict = 1;
        last = v;
        err = i915_ggtt_find(i915, size, align, start, end, flags, &at);
        if (err == 0) {
            break;
        }
    }
    /* Each binding the scan marked is unmarked; those in the range go. */
    for (v = i915->gem.lru_head; v && last; v = next) {
        next = v->lru_next;
        if (v->evict) {
            v->evict = 0;
            if (err == 0 && v->start < at + size && at < v->start + v->node_size) {
                (void)i915_vma_unbind(v, !(flags & I915_PIN_NONBLOCK));
                i915->gem.evictions++;
            }
        }
        if (v == last) {
            break;
        }
    }
    return err;
}

/*
 * When the idle bindings make no room, Linux's ggtt_flush idles the GPU,
 * so that the requests retire and their bindings fall idle, and the scan
 * runs once more, without a second wait.
 */
int32_t i915_gem_evict_something(struct drm_i915_private *i915, uint64_t size, uint64_t align,
                                 uint64_t start, uint64_t end, uint32_t flags)
{
    int32_t err;

    if (end <= start || size > end - start) {
        return -ENOSPC;
    }
    err = evict_scan(i915, size, align, start, end, flags);
    if (err != -ENOSPC || (flags & I915_PIN_NONBLOCK)) {
        return err;
    }
    err = i915_gt_wait_for_idle(i915, 1);
    if (err != 0) {
        return err;
    }
    return evict_scan(i915, size, align, start, end, flags);
}

/** Whether [start, end) meets a surface the firmware keeps or the GT's own
 * objects at the top of the GTT. */
static int range_reserved(const struct drm_i915_private *i915, uint64_t start, uint64_t end)
{
    int i;

    if (end > i915->gt.ggtt_bottom) {
        return 1;
    }
    for (i = 0; i < i915->ggtt.nkept; i++) {
        if (i915->ggtt.kept[i].start < end && start < i915->ggtt.kept[i].end) {
            return 1;
        }
    }
    return 0;
}

/*
 * The bindings in the range are taken one at a time, since a wait lets
 * others change the list: a busy one is waited for, which an unbind in
 * Linux does, and then looked for again. With I915_PIN_NOEVICT the range
 * must be free already, as Linux's i915_gem_gtt_reserve asks.
 */
int32_t i915_gem_evict_for_node(struct drm_i915_private *i915, uint64_t start, uint64_t size,
                                uint32_t flags)
{
    uint64_t end = start + size;
    struct i915_vma *v;
    int32_t err;

    if (range_reserved(i915, start, end)) {
        return -ENOSPC;
    }
    for (;;) {
        for (v = i915->gem.bound; v; v = v->ggtt_next) {
            if (v->start < end && start < v->start + v->node_size) {
                break;
            }
        }
        if (!v) {
            return 0;
        }
        if (!v->obj || v->pin_count || (flags & I915_PIN_NOEVICT)) {
            return -ENOSPC;
        }
        if (i915_gem_object_is_active(v->obj)) {
            if (flags & I915_PIN_NONBLOCK) {
                return -ENOSPC;
            }
            err = i915_gem_object_wait(v->obj, 1, -1);
            if (err != 0) {
                return err;
            }
            continue;
        }
        (void)i915_vma_unbind(v, !(flags & I915_PIN_NONBLOCK));
        i915->gem.evictions++;
    }
}

/*
 * As Linux's i915_gem_evict_vm on the global GTT: the GPU idled first, as
 * its ggtt_flush, so that no binding is held by a request.
 */
int32_t i915_gem_evict_vm(struct drm_i915_private *i915)
{
    struct i915_vma *v, *next;
    int32_t err = i915_gt_wait_for_idle(i915, 1);

    if (err != 0) {
        return err;
    }
    for (v = i915->gem.bound; v; v = next) {
        next = v->ggtt_next;
        if (evictable(v)) {
            (void)i915_vma_unbind(v, 1);
            i915->gem.evictions++;
        }
    }
    return 0;
}
