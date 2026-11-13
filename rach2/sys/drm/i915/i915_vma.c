/*
 * i915_vma.c: the bindings of buffer objects into the global GTT, the space
 * they take there, and their undoing, after Linux 6.12's i915_vma.c and
 * i915_gem_gtt.c.
 */

#include "i915.h"
#include "../../proc/smp.h"
#include "i915_drm.h"

#include "../../lib/kerrno.h"
#include "../../lib/kstring.h"
#include "../../mm/kheap.h"
#include "../../mm/paging.h"

/* The entries a bind writes at a time. */
#define I915_BIND_CHUNK 64u

/* ------------------------------------------------------------------------
 * The space
 */

/** Round `v` up to a multiple of `align`, a power of two, or all ones when
 * that wraps. */
static uint64_t align_up(uint64_t v, uint64_t align)
{
    uint64_t r = (v + align - 1) & ~(align - 1);

    return r < v ? ~(uint64_t)0 : r;
}

/** Whether [start, start + size) meets a kept surface of the firmware's;
 * if so, its range in `*a` and `*b`. */
static int kept_hit(const struct i915_ggtt *ggtt, uint64_t start, uint64_t size, uint64_t *a,
                    uint64_t *b)
{
    int i;

    for (i = 0; i < ggtt->nkept; i++) {
        if (ggtt->kept[i].start < start + size && start < ggtt->kept[i].end) {
            *a = ggtt->kept[i].start;
            *b = ggtt->kept[i].end;
            return 1;
        }
    }
    return 0;
}

/**
 * The highest range for `i915_ggtt_find`: a candidate below `hi` moves
 * below whatever it meets, and the search starts again.
 */
static int32_t ggtt_find_high(struct drm_i915_private *i915, uint64_t size, uint64_t align,
                              uint64_t hi, uint64_t *at)
{
    struct i915_vma *v;
    uint64_t start = (hi - size) & ~(align - 1);
    int moved = 1;

    while (moved) {
        uint64_t a = 0, b = 0;
        int hit = 0;

        moved = 0;
        for (v = i915->gem.bound; v && !hit; v = v->ggtt_next) {
            if (!v->evict && v->start < start + size && start < v->start + v->node_size) {
                a = v->start;
                hit = 1;
            }
        }
        if (!hit) {
            hit = kept_hit(&i915->ggtt, start, size, &a, &b);
        }
        if (hit) {
            if (a < size) {
                return -ENOSPC;
            }
            start = (a - size) & ~(align - 1);
            moved = 1;
        }
    }
    *at = start;
    return 0;
}

/* The lowest range is found in one pass over the bindings, which lie by
 * offset and never overlap: a binding that ends at or before the candidate
 * stays behind it however far it moves. */
int32_t i915_ggtt_find(struct drm_i915_private *i915, uint64_t size, uint64_t align, uint64_t lo,
                       uint64_t hi, uint32_t flags, uint64_t *at)
{
    struct i915_vma *v = i915->gem.bound;
    uint64_t start = align_up(lo, align), a, b;

    if (hi > i915->gt.ggtt_bottom) {
        hi = i915->gt.ggtt_bottom;
    }
    if (size == 0 || size > hi) {
        return -ENOSPC;
    }
    if (flags & I915_PIN_HIGH) {
        if (ggtt_find_high(i915, size, align, hi, at) != 0 || *at < lo) {
            return -ENOSPC;
        }
        return 0;
    }
    for (;;) {
        if (start > hi || size > hi - start) {
            return -ENOSPC;
        }
        while (v && (v->evict || v->start + v->node_size <= start)) {
            v = v->ggtt_next;
        }
        if (v && v->start < start + size) {
            start = align_up(v->start + v->node_size, align);
        } else if (kept_hit(&i915->ggtt, start, size, &a, &b)) {
            start = align_up(b, align);
        } else {
            break;
        }
    }
    *at = start;
    return 0;
}

/** Put `vma`, whose `start` is set, on the GTT's list by offset. */
static void ggtt_link(struct drm_i915_private *i915, struct i915_vma *vma)
{
    struct i915_vma **link = &i915->gem.bound;

    while (*link && (*link)->start < vma->start) {
        link = &(*link)->ggtt_next;
    }
    vma->ggtt_next = *link;
    *link = vma;
}

/** Take `vma` off the GTT's list. */
static void ggtt_unlink(struct drm_i915_private *i915, struct i915_vma *vma)
{
    struct i915_vma **link;

    for (link = &i915->gem.bound; *link; link = &(*link)->ggtt_next) {
        if (*link == vma) {
            *link = vma->ggtt_next;
            return;
        }
    }
}

/** Take `vma` off the list by last use. */
static void lru_unlink(struct drm_i915_private *i915, struct i915_vma *vma)
{
    if (vma->lru_prev) {
        vma->lru_prev->lru_next = vma->lru_next;
    } else if (i915->gem.lru_head == vma) {
        i915->gem.lru_head = vma->lru_next;
    }
    if (vma->lru_next) {
        vma->lru_next->lru_prev = vma->lru_prev;
    } else if (i915->gem.lru_tail == vma) {
        i915->gem.lru_tail = vma->lru_prev;
    }
    vma->lru_prev = vma->lru_next = 0;
}

/** Make `vma` the most recently used binding, at the list's tail, as
 * Linux moves a binding it uses to the tail of its bound list. */
static void lru_bump(struct drm_i915_private *i915, struct i915_vma *vma)
{
    if (i915->gem.lru_tail == vma) {
        return;
    }
    lru_unlink(i915, vma);
    vma->lru_prev = i915->gem.lru_tail;
    if (i915->gem.lru_tail) {
        i915->gem.lru_tail->lru_next = vma;
    } else {
        i915->gem.lru_head = vma;
    }
    i915->gem.lru_tail = vma;
}

/**
 * Find room from `lo` for `size` bytes aligned to `align` inside the
 * aperture with I915_PIN_MAPPABLE, anywhere in the GTT without, making
 * room by eviction once unless I915_PIN_NOEVICT: 0 with the offset in
 * `*at`, -ENOSPC, or the eviction's -EINTR.
 */
static int32_t ggtt_room(struct drm_i915_private *i915, uint64_t size, uint64_t align, uint64_t lo,
                         uint32_t flags, uint64_t *at)
{
    uint64_t hi = (flags & I915_PIN_MAPPABLE) ? i915->ggtt.mappable_end : i915->ggtt.total;
    int32_t err = i915_ggtt_find(i915, size, align, lo, hi, flags, at);

    if (err == -ENOSPC && !(flags & I915_PIN_NOEVICT)) {
        err = i915_gem_evict_something(i915, size, align, lo, hi, flags);
        if (err == 0) {
            err = i915_ggtt_find(i915, size, align, lo, hi, flags, at);
        }
    }
    return err;
}

int32_t i915_ggtt_reserve(struct drm_i915_private *i915, uint64_t size, uint32_t flags,
                          uint64_t *offset)
{
    struct i915_vma *vma = (struct i915_vma *)kzalloc(sizeof(*vma));
    int32_t err;

    if (!vma) {
        return -ENOMEM;
    }
    err = ggtt_room(i915, size, I915_GTT_PAGE_SIZE, 0, flags, &vma->start);
    if (err != 0) {
        kfree(vma);
        return err;
    }
    vma->node_size = vma->size = size;
    vma->mappable = vma->start + size <= i915->ggtt.mappable_end;
    vma->pin_count = 1;
    ggtt_link(i915, vma);
    *offset = vma->start;
    return 0;
}

void i915_ggtt_release(struct drm_i915_private *i915, uint64_t offset)
{
    struct i915_vma *v;

    for (v = i915->gem.bound; v; v = v->ggtt_next) {
        if (!v->obj && v->start == offset) {
            ggtt_unlink(i915, v);
            kfree(v);
            return;
        }
    }
}

/* ------------------------------------------------------------------------
 * Bindings
 */

/** Whether `vma` serves a GTT map and a fence: it lies inside the aperture,
 * with the room and alignment a fence of its object's tiling needs, as
 * Linux's i915_vma_is_map_and_fenceable. */
static int vma_fenceable(struct drm_i915_private *i915, const struct i915_vma *vma)
{
    return vma->start + vma->node_size <= i915->ggtt.mappable_end &&
           vma->node_size >= vma->fence_size && (vma->start & (vma->fence_alignment - 1)) == 0;
}

void i915_vma_update_fenceable(struct i915_vma *vma)
{
    struct drm_i915_gem_object *obj = vma->obj;
    struct drm_i915_private *i915 = to_i915(obj->base.dev);

    vma->fence_size = i915_gem_fence_size(i915, (uint32_t)vma->size, obj->tiling, obj->stride);
    vma->fence_alignment = i915_gem_fence_alignment(i915, obj->tiling);
    vma->mappable = (uint8_t)vma_fenceable(i915, vma);
}

/** Write the entries of `vma`'s view at the object's cache level in the
 * global GTT and, at the same offset, in the aliasing PPGTT, which every
 * context's batches see, as Linux's aliasing_gtt_bind_vma; a remapped
 * view's pages are its rows', as Linux's intel_remap_pages lists them. 0,
 * or `i915_ggtt_insert`'s or `i915_ppgtt_insert`'s error. */
static int32_t vma_bind(struct drm_i915_private *i915, struct i915_vma *vma)
{
    struct drm_i915_gem_object *obj = vma->obj;
    uint32_t n = (uint32_t)(vma->size >> PAGE_SHIFT), done = 0;
    uint64_t chunk[I915_BIND_CHUNK];
    int32_t err;

    while (done < n) {
        uint32_t k = n - done < I915_BIND_CHUNK ? n - done : I915_BIND_CHUNK, i;
        uint64_t at = vma->start + ((uint64_t)done << PAGE_SHIFT);

        for (i = 0; i < k; i++) {
            uint32_t k_at = done + i;

            chunk[i] = obj->frames[vma->remap.width ? I915_REMAP_PAGE(&vma->remap, k_at)
                                                    : vma->view_offset + k_at];
        }
        err = i915_ggtt_insert(i915, at, chunk, k, obj->cache_level);
        if (err == 0) {
            err = i915_ppgtt_insert(i915, at, chunk, k, obj->cache_level);
            if (err != 0) {
                (void)i915_ggtt_clear_range(i915, at, (uint64_t)k << PAGE_SHIFT, 0);
            }
        }
        if (err != 0) {
            /* A failed insert writes nothing; the chunks before it go. */
            if (done) {
                (void)i915_ggtt_clear_range(i915, vma->start, (uint64_t)done << PAGE_SHIFT, 0);
                i915_ppgtt_clear_range(i915, vma->start, (uint64_t)done << PAGE_SHIFT);
            }
            return err;
        }
        done += k;
    }
    vma->cache = obj->cache_level;
    return 0;
}

/** Whether the remapped views `a` and `b` are one, NULL being none. */
static int remap_same(const struct i915_remap_info *a, const struct i915_remap_info *b)
{
    uint32_t aw = a ? a->width : 0, bw = b ? b->width : 0;

    if (aw == 0 || bw == 0) {
        return aw == bw;
    }
    return a->offset == b->offset && a->width == b->width && a->height == b->height &&
           a->src_stride == b->src_stride;
}

/** The binding of `obj`'s view of `view_pages` pages from `view_offset`, 0
 * pages for the whole object, or of the remapped view `remap` when it is
 * not NULL; or NULL. */
static struct i915_vma *vma_lookup(struct drm_i915_gem_object *obj, uint32_t view_offset,
                                   uint32_t view_pages, const struct i915_remap_info *remap)
{
    struct i915_vma *v;

    for (v = obj->vmas; v; v = v->obj_next) {
        if (v->view_offset == view_offset && v->view_pages == view_pages &&
            remap_same(&v->remap, remap)) {
            return v;
        }
    }
    return 0;
}

/**
 * Before a binding that does not serve is undone, wait for the GPU to
 * finish with its object, as Linux's unbind does in i915_vma_sync: 0 when
 * the caller may look again, -ENOSPC under I915_PIN_NONBLOCK, or -EINTR
 * for a signal.
 */
static int32_t vma_wait_idle(struct drm_i915_gem_object *obj, uint32_t flags)
{
    if (flags & I915_PIN_NONBLOCK) {
        return -ENOSPC;
    }
    return i915_gem_object_wait_idle(obj, 1);
}

/*
 * What `vma_create` makes: the view, partial or remapped, the I915_PIN_
 * flags, the alignment, a power of two of at least a page, the least size
 * of the node, and the lowest offset, or with I915_PIN_OFFSET_FIXED the
 * offset.
 */
struct vma_place {
    uint32_t view_offset;
    uint32_t view_pages;
    struct i915_remap_info remap;
    uint32_t flags;
    uint64_t align;
    uint64_t size;
    uint64_t offset;
};

/**
 * The binding of the view `p` names, partial or remapped, that
 * `i915_vma_pin` may reuse, in `*out`, or NULL
 * once a binding that does not serve is undone: one on the scratch page
 * since a userptr object's pages changed, one outside the aperture when it
 * is asked for, or one not aligned to `p`'s alignment. An unpinned one that
 * the GPU still uses is waited for first and looked up again, since the
 * wait sleeps. 0; -EAGAIN when the first kind is held, -ENOSPC when another
 * is; or the wait's error.
 */
static int32_t vma_existing(struct drm_i915_gem_object *obj, const struct vma_place *p,
                            struct i915_vma **out)
{
    struct i915_vma *vma =
        vma_lookup(obj, p->view_offset, p->view_pages, p->remap.width ? &p->remap : 0);
    uint32_t flags = p->flags;
    uint64_t align = p->align;
    int32_t err;

    *out = 0;
    if (!vma) {
        return 0;
    }
    if ((obj->userptr.stale || ((flags & I915_PIN_MAPPABLE) && !vma->mappable) ||
         (vma->start & (align - 1))) &&
        vma->pin_count == 0 && i915_gem_object_is_active(obj)) {
        err = vma_wait_idle(obj, flags);
        if (err != 0) {
            return err;
        }
        return vma_existing(obj, p, out);
    }
    if (obj->userptr.stale) {
        /* Its entries map the scratch page since the program's pages
         * changed: undone, and bound again on new frames, unless held. */
        return i915_vma_unbind(vma, !(flags & I915_PIN_NONBLOCK)) == 0 ? 0 : -EAGAIN;
    }
    if (((flags & I915_PIN_MAPPABLE) && !vma->mappable) || (vma->start & (align - 1))) {
        /* Misplaced, as Linux's i915_vma_misplaced: moved, unless held. */
        err = i915_vma_unbind(vma, !(flags & I915_PIN_NONBLOCK));
        return err == -EAGAIN ? -ENOSPC : err;
    }
    *out = vma;
    return 0;
}

/**
 * A new binding of the view `p` names, pinned, in `*out`: the object's
 * pages pinned, room found, made by eviction or emptied at a fixed offset,
 * and the entries written. 1 when, while an eviction slept, a binding of
 * the view was made, which the caller then takes; or the errors of the
 * pin, the room and the bind.
 */
static int32_t vma_create(struct drm_i915_gem_object *obj, const struct vma_place *p,
                          struct i915_vma **out)
{
    struct drm_i915_private *i915 = to_i915(obj->base.dev);
    uint32_t pages = (uint32_t)(obj->base.size >> PAGE_SHIFT);
    uint64_t align = p->align, size;
    struct i915_vma *vma = (struct i915_vma *)kzalloc(sizeof(*vma));
    int32_t err;

    if (!vma) {
        return -ENOMEM;
    }
    vma->obj = obj;
    vma->view_offset = p->view_offset;
    vma->view_pages = p->view_pages;
    vma->remap = p->remap;
    vma->size = size = (uint64_t)(p->view_pages ? p->view_pages : pages) << PAGE_SHIFT;
    vma->fence_size = i915_gem_fence_size(i915, (uint32_t)size, obj->tiling, obj->stride);
    vma->fence_alignment = i915_gem_fence_alignment(i915, obj->tiling);
    vma->node_size = size < p->size ? p->size : size;
    if (p->flags & I915_PIN_MAPPABLE) {
        if (vma->fence_size > vma->node_size) {
            vma->node_size = vma->fence_size;
        }
        if (vma->fence_alignment > align) {
            align = vma->fence_alignment;
        }
    }
    err = i915_gem_object_pin_pages(obj);
    if (err == 0) {
        if (p->flags & I915_PIN_OFFSET_FIXED) {
            vma->start = p->offset;
            err = i915_gem_evict_for_node(i915, p->offset, vma->node_size, p->flags);
        } else {
            err = ggtt_room(i915, vma->node_size, align, p->offset, p->flags, &vma->start);
        }
        if (err == 0 && vma_lookup(obj, p->view_offset, p->view_pages, &p->remap)) {
            err = 1;
        }
        if (err == 0) {
            err = vma_bind(i915, vma);
        }
        if (err != 0) {
            i915_gem_object_unpin_pages(obj);
        }
    }
    if (err != 0) {
        kfree(vma);
        return err;
    }
    vma->mappable = (uint8_t)vma_fenceable(i915, vma);
    vma->pin_count = 1;
    vma->obj_next = obj->vmas;
    obj->vmas = vma;
    ggtt_link(i915, vma);
    lru_bump(i915, vma);
    *out = vma;
    return 0;
}

/** Pin the binding of the view `p` names, reused or made, in `*out`: 0,
 * or the errors of `vma_existing` and `vma_create`. */
static int32_t vma_pin_place(struct drm_i915_gem_object *obj, const struct vma_place *p,
                             struct i915_vma **out)
{
    struct drm_i915_private *i915 = to_i915(obj->base.dev);
    struct i915_vma *vma;
    int32_t err;

    do {
        err = vma_existing(obj, p, &vma);
        if (err != 0) {
            return err;
        }
        if (vma) {
            vma->pin_count++;
            lru_bump(i915, vma);
            *out = vma;
            return 0;
        }
        err = vma_create(obj, p, out);
    } while (err == 1);
    return err;
}

int32_t i915_vma_pin(struct drm_i915_gem_object *obj, uint32_t view_offset, uint32_t view_pages,
                     uint32_t flags, uint32_t align, struct i915_vma **out)
{
    uint32_t pages = (uint32_t)(obj->base.size >> PAGE_SHIFT);
    struct vma_place p;

    if (view_pages && (uint64_t)view_offset + view_pages > pages) {
        return -EINVAL;
    }
    if (align < I915_GTT_PAGE_SIZE) {
        align = I915_GTT_PAGE_SIZE;
    }
    k_memset(&p, 0, sizeof(p));
    p.view_offset = view_offset;
    p.view_pages = view_pages;
    p.flags = flags & ~(I915_PIN_OFFSET_FIXED | I915_PIN_OFFSET_BIAS);
    p.align = align;
    return vma_pin_place(obj, &p, out);
}

int32_t i915_vma_pin_remapped(struct drm_i915_gem_object *obj, const struct i915_remap_info *remap,
                              uint32_t flags, uint32_t align, struct i915_vma **out)
{
    uint32_t pages = (uint32_t)(obj->base.size >> PAGE_SHIFT);
    struct vma_place p;

    if (remap->width == 0 || remap->height == 0 || remap->width > remap->src_stride ||
        (uint64_t)remap->offset + (uint64_t)(remap->height - 1u) * remap->src_stride +
                remap->width >
            pages) {
        return -EINVAL;
    }
    if (align < I915_GTT_PAGE_SIZE) {
        align = I915_GTT_PAGE_SIZE;
    }
    k_memset(&p, 0, sizeof(p));
    p.view_pages = remap->width * remap->height;
    p.remap = *remap;
    p.flags = flags & ~(I915_PIN_OFFSET_FIXED | I915_PIN_OFFSET_BIAS);
    p.align = align;
    return vma_pin_place(obj, &p, out);
}
/** Whether `vma` serves what `a` asks with its alignment `align`, as
 * Linux's i915_vma_misplaced and eb_vma_misplaced ask the other way. */
static int vma_placed(const struct i915_vma *vma, const struct i915_pin_args *a, uint64_t align)
{
    if (vma->node_size < a->size || (vma->start & (align - 1))) {
        return 0;
    }
    if ((a->flags & I915_PIN_OFFSET_FIXED) && vma->start != a->offset) {
        return 0;
    }
    if ((a->flags & I915_PIN_OFFSET_BIAS) && vma->start < a->offset) {
        return 0;
    }
    return !(a->flags & I915_PIN_MAPPABLE) || vma->mappable;
}

int32_t i915_vma_pin_args(struct drm_i915_gem_object *obj, const struct i915_pin_args *a,
                          struct i915_vma **out)
{
    struct drm_i915_private *i915 = to_i915(obj->base.dev);
    uint64_t align = a->align < I915_GTT_PAGE_SIZE ? I915_GTT_PAGE_SIZE : a->align;
    uint64_t hi = (a->flags & I915_PIN_MAPPABLE) ? i915->ggtt.mappable_end : i915->ggtt.total;
    uint64_t size = obj->base.size < a->size ? a->size : obj->base.size;
    struct vma_place p;
    struct i915_vma *vma;
    int32_t err;

    if ((a->flags & I915_PIN_OFFSET_FIXED) &&
        ((a->offset & (align - 1)) || a->offset > hi || size > hi - a->offset)) {
        return -EINVAL;
    }
    k_memset(&p, 0, sizeof(p));
    p.flags = a->flags;
    p.align = align;
    p.size = a->size;
    p.offset = (a->flags & (I915_PIN_OFFSET_FIXED | I915_PIN_OFFSET_BIAS)) ? a->offset : 0;
    do {
        vma = vma_lookup(obj, 0, 0, 0);
        if (vma && (obj->userptr.stale || !vma_placed(vma, a, align)) && vma->pin_count == 0 &&
            i915_gem_object_is_active(obj)) {
            err = vma_wait_idle(obj, a->flags);
            if (err != 0) {
                return err;
            }
            err = 1; /* look again, since the wait slept */
            continue;
        }
        if (vma && (obj->userptr.stale || !vma_placed(vma, a, align))) {
            /* Misplaced, or on the scratch page since a userptr object's
             * pages changed: undone, unless held. */
            err = i915_vma_unbind(vma, !(a->flags & I915_PIN_NONBLOCK));
            if (err != 0) {
                return obj->userptr.stale ? -EAGAIN : -ENOSPC;
            }
            vma = 0;
        }
        if (vma) {
            vma->pin_count++;
            lru_bump(i915, vma);
            *out = vma;
            return 0;
        }
        err = vma_create(obj, &p, out);
    } while (err == 1);
    return err;
}

void i915_vma_unpin(struct i915_vma *vma)
{
    if (vma->pin_count) {
        vma->pin_count--;
        if (vma->pin_count == 0 && vma->obj && vma->obj->userptr.stale) {
            /* It maps the scratch page since the program's pages changed;
             * with it go the old frames. */
            (void)i915_vma_unbind(vma, 0);
        }
    }
}

void i915_vma_revoke_mmap(struct i915_vma *vma)
{
    struct drm_i915_gem_object *obj = vma->obj;
    uint32_t pages = (uint32_t)(vma->size >> PAGE_SHIFT);
    int big;

    if (!vma->userfault) {
        return;
    }
    /* The walk of every address space is the big lock's (mm/paging.c),
     * which EXECBUFFER2's eviction, under the GEM lock alone, takes. */
    big = !bkl_held();
    if (big) {
        bkl_enter();
    }
    paging_zap_device(&obj->mmo[I915_MMAP_OFFSET_GTT], vma->view_offset, pages);
    if (big) {
        bkl_exit();
    }
    vma->userfault = 0;
    obj->userfault_count--;
}

void i915_vma_flush_writes(struct i915_vma *vma)
{
    struct drm_i915_private *i915 = to_i915(vma->obj->base.dev);

    if (!vma->ggtt_write) {
        return;
    }
    arch_dma_fence();
    if (vma->mappable) {
        i915_aperture_sync(&i915->hw, vma->start, vma->size, 1);
    }
    vma->ggtt_write = 0;
}

/**
 * Undo `vma` whatever its pin and its object's reservation hold, as Linux's
 * __i915_vma_evict: the caller has settled that no request of the chip's
 * can still use it. `may_yield` is `i915_vma_unbind`'s.
 */
static void vma_evict(struct i915_vma *vma, int may_yield)
{
    struct drm_i915_gem_object *obj = vma->obj;
    struct drm_i915_private *i915 = to_i915(obj->base.dev);
    struct i915_vma **link;

    /* As Linux's __i915_vma_evict: programs fault again, the writes through
     * the aperture reach memory, and only then is the fence let go. */
    i915_vma_revoke_mmap(vma);
    i915_vma_flush_writes(vma);
    i915_vma_revoke_fence(vma);
    (void)i915_ggtt_clear_range(i915, vma->start, vma->node_size, may_yield);
    i915_ppgtt_clear_range(i915, vma->start, vma->node_size);
    i915_gem_object_unpin_pages(obj);
    ggtt_unlink(i915, vma);
    lru_unlink(i915, vma);
    for (link = &obj->vmas; *link; link = &(*link)->obj_next) {
        if (*link == vma) {
            *link = vma->obj_next;
            break;
        }
    }
    kfree(vma);
}

int i915_vma_is_active(const struct i915_vma *vma)
{
    return vma->obj->base.refs != 0 && i915_gem_object_is_active(vma->obj);
}

/** `i915_vma_unbind`, by a caller that holds the object's reservation when
 * `held` is set, whose activity is then read without taking it again. */
static int32_t vma_unbind(struct i915_vma *vma, int may_yield, int held)
{
    struct drm_i915_gem_object *obj = vma->obj;

    if (vma->pin_count) {
        return -EAGAIN;
    }
    if (held ? obj->base.refs != 0 && i915_gem_object_is_active_locked(obj)
             : i915_vma_is_active(vma)) {
        return -EBUSY;
    }
    vma_evict(vma, may_yield);
    return 0;
}

int32_t i915_vma_unbind(struct i915_vma *vma, int may_yield)
{
    return vma_unbind(vma, may_yield, 0);
}

void i915_gem_object_force_unbind(struct drm_i915_gem_object *obj, int may_yield)
{
    struct i915_vma *vma;

    while ((vma = obj->vmas) != 0) {
        vma->pin_count = 0;
        vma_evict(vma, may_yield);
    }
}

/** Undo every binding of `obj` with `vma_unbind`: 0, or -EAGAIN when one
 * is kept. */
static int32_t object_unbind(struct drm_i915_gem_object *obj, int may_yield, int held)
{
    struct i915_vma *vma = obj->vmas, *next;
    int32_t err = 0;

    while (vma) {
        next = vma->obj_next;
        if (vma_unbind(vma, may_yield, held) != 0) {
            err = -EAGAIN;
        }
        vma = next;
    }
    return err;
}

int32_t i915_gem_object_unbind(struct drm_i915_gem_object *obj, int may_yield)
{
    return object_unbind(obj, may_yield, 0);
}

int32_t i915_gem_object_unbind_locked(struct drm_i915_gem_object *obj, int may_yield)
{
    lockdep_assert_held(&obj->base.resv->lock);
    return object_unbind(obj, may_yield, 1);
}

struct i915_vma *i915_vma_for_page(struct drm_i915_gem_object *obj, uint32_t pgoff)
{
    struct i915_vma *v;

    for (v = obj->vmas; v; v = v->obj_next) {
        uint32_t n = (uint32_t)(v->size >> PAGE_SHIFT);

        if (v->mappable && !v->remap.width && pgoff >= v->view_offset &&
            pgoff - v->view_offset < n) {
            return v;
        }
    }
    return 0;
}
