/*
 * i915_gem_evict.c: room in the global GTT made by undoing idle bindings,
 * least recently used first, after Linux 6.12's i915_gem_evict.c.
 */

#include "i915.h"

#include "../../lib/kerrno.h"

/** Whether eviction may undo `vma`: a binding of an object, not pinned,
 * that no request may still use (`i915_vma_is_active`), as Linux's
 * mark_free and defer_evict ask (i915_gem_evict.c:96-121). */
static int evictable(const struct i915_vma *vma)
{
    return vma->obj && vma->pin_count == 0 && !i915_vma_is_active(vma);
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

/** Whether [start, end) meets a kept range or the GT's own objects at
 * the top of the GTT. */
static int range_reserved(const struct drm_i915_private *i915, uint64_t start, uint64_t end)
{
    if (end > i915->gt.ggtt_bottom) {
        return 1;
    }
    return i915_ggtt_kept_overlap(&i915->ggtt, start, end) != 0;
}

/*
 * The bindings in the range are taken one at a time, since a wait lets
 * others change the list: a busy one is waited for, which an unbind in
 * Linux does, and then looked for again. The wait lets the GEM lock go, and
 * another thread's close or retirement may then drop the object's last
 * reference, so a reference is held across it, as Linux's grab_vma takes
 * one with i915_gem_object_get_rcu before the unbind's wait and ungrab_vma
 * puts it (i915_gem_evict.c:66-92). The put comes with the lock retaken,
 * so that a last put leaves the object on `gem.freed`. A binding of an
 * object with no references is idle and goes with no wait, as Linux's
 * grab_vma takes a dying binding with no lock and its unbind waits only for
 * the binding's own requests (i915_gem_evict.c:67-83, 410-436;
 * i915_vma.c:2123): such an object is on `gem.freed`, which the wait's
 * drop of the GEM lock lets another thread free. One whose last reference
 * goes between the test and the get is such an object too. With
 * I915_PIN_NOEVICT the range must be free already, as Linux's
 * i915_gem_gtt_reserve asks.
 */
int32_t i915_gem_evict_for_node(struct drm_i915_private *i915, uint64_t start, uint64_t size,
                                uint32_t flags)
{
    uint64_t end = start + size;
    struct drm_i915_gem_object *obj;
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
        if (i915_vma_is_active(v)) {
            if (flags & I915_PIN_NONBLOCK) {
                return -ENOSPC;
            }
            obj = v->obj;
            if (drm_gem_object_get_unless_zero(&obj->base)) {
                err = i915_gem_object_wait(obj, 1, -1);
                i915_gem_object_put(obj);
                if (err != 0) {
                    return err;
                }
                continue;
            }
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
