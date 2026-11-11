/*
 * i915_gem_execbuffer.c: EXECBUFFER2, a program's batch run on one of the
 * GPU's rings, after Linux 6.12's gem/i915_gem_execbuffer.c on a
 * legacy ring: the objects looked up, bound and relocated, the batch
 * checked, its request built behind its fences, and the objects' implicit
 * fences and domains moved to it.
 */

#include "i915.h"
#include "i915_drm.h"
#include "i915_reg.h"

#include "../../dmabuf/dmabuf.h"
#include "../../fs/fd.h"
#include "../../lib/kerrno.h"
#include "../../lib/kstring.h"
#include "../../mm/kheap.h"
#include "../../mm/paging.h"
#include "../../mm/usercopy.h"
#include "../../proc/percpu.h"
#include "../../proc/process.h"
#include "../../rtc.h"

/* The user address a 64-bit field of a call names. */
#define EB_USER_PTR(v) ((void __user *)(uintptr_t)(v))

/* In an object's offset as the call keeps it, below the page: the object
 * is now there, and the offset goes back to the program (Linux's UPDATE). */
#define EB_UPDATE 0x1u

/* The lowest offset of a batch that has relocations and is not pinned, so
 * that a negative delta still gives an address in the GTT, as Linux's
 * BATCH_OFFSET_BIAS. */
#define EB_BATCH_BIAS (256u * 1024u)

/* The flags of an object beyond the user's EXEC_OBJECT_ bits: it is the
 * batch with relocations (Linux's __EXEC_OBJECT_NEEDS_BIAS), it holds a pin
 * of its binding (__EXEC_OBJECT_HAS_PIN), or it waits for room. */
#define EB_NEEDS_BIAS 0x80000000u
#define EB_HAS_PIN    0x40000000u
#define EB_UNBOUND    0x20000000u

/* The relocations copied from the program at once, 512 bytes, as Linux. */
#define EB_RELOC_CHUNK 16u

/* Where a relocation entry holds its presumed offset. */
#define EB_PRESUMED_AT 16u

/* How the batch starts: privileged, as Linux's I915_DISPATCH_SECURE. */
#define EB_DISPATCH_SECURE 0x1u

/* An object of the call: its entry, a reference to it, and its binding.
 * `relocs` is the kernel's copy of its relocations, made before the GEM
 * lock is taken, NULL for none, and `moved` marks, one byte to an entry,
 * the relocations written, whose presumed offsets go back after it. */
struct eb_vma {
    struct drm_i915_gem_exec_object2 *exec;
    struct drm_i915_gem_object *obj;
    struct i915_vma *vma;
    uint32_t flags; /* the entry's EXEC_OBJECT_ flags and the EB_ ones */
    struct drm_i915_gem_relocation_entry *relocs;
    uint8_t *moved; /* in the same allocation as `relocs` */
};

/*
 * A syncobj of the fence array or the timeline fences, with a reference:
 * the fence of its point, with a reference, that the request waits for,
 * or NULL; its I915_EXEC_FENCE_ flags; the point, 0 for a binary syncobj;
 * and for a point to signal on a timeline, the chain record made for it,
 * which the syncobj takes when the request is added.
 */
struct eb_fence {
    struct drm_syncobj *syncobj;
    struct dma_fence *fence;
    uint32_t flags;
    uint64_t point;
    struct dma_fence_chain *chain;
};

/*
 * What a request holds for the call until it retires: a reference to each
 * of `count` objects, the call's and the parser's shadow.
 */
struct eb_hold {
    struct i915_request_hold base;
    uint32_t count;
    struct drm_i915_gem_object **obj; /* in the same allocation */
};

/*
 * The state of one call, Linux's i915_execbuffer. `lut` finds an object's
 * index by its handle, open addressed with `lut_mask + 1` slots that hold
 * index + 1, NULL with I915_EXEC_HANDLE_LUT, where relocations name the
 * index itself. `has_reloc` is set while relocations must be applied and,
 * once the objects are placed, while their offsets go back to the program.
 * `shadow` is the parser's copy of the batch, with a reference, and
 * `shadow_vma` its pinned binding, which run in the batch's place. `hold`
 * goes to the request.
 */
struct eb {
    struct drm_i915_private *i915;
    struct drm_file *file;
    struct drm_i915_gem_execbuffer2 *args;
    struct drm_i915_gem_exec_object2 *exec;
    struct eb_vma *ev;
    uint32_t count;
    uint32_t *lut;
    uint32_t lut_mask;
    uint32_t batch;
    uint32_t batch_start;
    uint32_t batch_len;
    uint32_t dispatch;
    uint8_t has_reloc;
    struct i915_gem_context *ctx;
    struct intel_context *ce;
    struct eb_fence *fences;
    size_t nfences;
    struct drm_i915_gem_object *shadow;
    struct i915_vma *shadow_vma;
    struct eb_hold *hold;
    struct sync_file *out; /* the out-fence's file, attached after the lock */
};

/* ------------------------------------------------------------------------
 * The call's checks and objects
 */

/**
 * The checks of the call's words before anything is looked up, as Linux's
 * i915_gem_check_execbuffer: flags Linux does not know or Haswell's ring
 * submission refuses, cliprects, which only DRI1 had, the DR1 and DR4 of
 * that time, of which a DR4 of 0xffffffff is taken as 0 for old Mesa, and
 * a batch's start or length that is not of whole quadwords. 0 or -EINVAL.
 */
static int32_t eb_check(struct drm_i915_gem_execbuffer2 *args)
{
    if (args->flags &
        (__I915_EXEC_UNKNOWN_FLAGS | I915_EXEC_CONSTANTS_MASK | I915_EXEC_RESOURCE_STREAMER)) {
        return -EINVAL;
    }
    if (!(args->flags & (I915_EXEC_FENCE_ARRAY | I915_EXEC_USE_EXTENSIONS)) &&
        (args->num_cliprects != 0 || args->cliprects_ptr != 0)) {
        return -EINVAL;
    }
    if (args->DR4 == 0xffffffffu) {
        args->DR4 = 0;
    }
    if (args->DR1 || args->DR4) {
        return -EINVAL;
    }
    if ((args->batch_start_offset | args->batch_len) & 7u) {
        return -EINVAL;
    }
    return 0;
}

/** The slot of `handle` in the call's table: where it is, or the empty slot
 * where it would go. */
static uint32_t eb_lut_slot(const struct eb *eb, uint32_t handle)
{
    uint32_t at = (handle * 0x9e3779b1u) & eb->lut_mask;

    while (eb->lut[at] && eb->exec[eb->lut[at] - 1u].handle != handle) {
        at = (at + 1u) & eb->lut_mask;
    }
    return at;
}

/** The table of handles, with twice as many slots as objects at least, for
 * relocations that name their targets by handle. 0 or -ENOMEM. */
static int32_t eb_lut_build(struct eb *eb)
{
    uint32_t slots = 16, i, at;

    while (slots < 2u * eb->count && slots < 0x80000000u) {
        slots <<= 1;
    }
    if ((uint64_t)slots * sizeof(uint32_t) > (size_t)-1) {
        return -ENOMEM;
    }
    eb->lut = (uint32_t *)kzalloc((size_t)slots * sizeof(uint32_t));
    if (!eb->lut) {
        return -ENOMEM;
    }
    eb->lut_mask = slots - 1u;
    for (i = 0; i < eb->count; i++) {
        at = eb_lut_slot(eb, eb->exec[i].handle);
        if (!eb->lut[at]) {
            eb->lut[at] = i + 1u;
        }
    }
    return 0;
}

/** The object a relocation names, by its index with I915_EXEC_HANDLE_LUT or
 * by its handle, as Linux's eb_get_vma; NULL for none. */
static struct eb_vma *eb_target(const struct eb *eb, uint32_t handle)
{
    uint32_t at;

    if (!eb->lut) {
        return handle < eb->count ? &eb->ev[handle] : 0;
    }
    at = eb->lut[eb_lut_slot(eb, handle)];
    return at ? &eb->ev[at - 1u] : 0;
}

/**
 * Check an object's entry, as Linux's eb_validate_vma: no flag Linux does
 * not know, an alignment of a power of two, a pinned offset of whole pages
 * in the canonical form, and a pad of whole pages, which is cleared without
 * EXEC_OBJECT_PAD_TO_SIZE, since the field was once reserved. The offset
 * is then kept in the GTT's own form, and EXEC_OBJECT_NEEDS_FENCE, which
 * only chips before graphics 4 heed, is dropped. 0 or -EINVAL.
 */
static int32_t eb_validate(struct drm_i915_gem_exec_object2 *entry)
{
    uint64_t canonical;

    if (entry->flags & __EXEC_OBJECT_UNKNOWN_FLAGS) {
        return -EINVAL;
    }
    if (entry->alignment & (entry->alignment - 1u)) {
        return -EINVAL;
    }
    if (entry->flags & EXEC_OBJECT_PINNED) {
        canonical = entry->offset & ~(uint64_t)(I915_GTT_PAGE_SIZE - 1u);
        if (canonical & ((uint64_t)1 << 47)) {
            canonical |= ~(((uint64_t)1 << 48) - 1u);
        } else {
            canonical &= ((uint64_t)1 << 48) - 1u;
        }
        if (entry->offset != canonical) {
            return -EINVAL;
        }
    }
    if (entry->flags & EXEC_OBJECT_PAD_TO_SIZE) {
        if (entry->pad_to_size & (I915_GTT_PAGE_SIZE - 1u)) {
            return -EINVAL;
        }
    } else {
        entry->pad_to_size = 0;
    }
    entry->offset &= ((uint64_t)1 << 48) - 1u;
    entry->flags &= ~(uint64_t)EXEC_OBJECT_NEEDS_FENCE;
    return 0;
}

/**
 * Look up every object of the call and check its entry, then the batch, as
 * Linux's eb_lookup_vmas and eb_add_vma: the batch the last object, or the
 * first with I915_EXEC_BATCH_FIRST, which may not be written by its own
 * run, must hold its start and length, and, with relocations and no fixed
 * place, keeps above EB_BATCH_BIAS. An object may be named twice.
 *
 * @return 0; -ENOENT for a handle with no object of the driver's; -EINVAL.
 */
static int32_t eb_lookup(struct eb *eb)
{
    struct drm_i915_gem_exec_object2 *entry;
    struct eb_vma *batch;
    uint64_t size;
    uint32_t i;
    int32_t err;

    for (i = 0; i < eb->count; i++) {
        entry = &eb->exec[i];
        eb->ev[i].exec = entry;
        eb->ev[i].obj = i915_gem_object_lookup(eb->file, entry->handle);
        if (!eb->ev[i].obj) {
            return -ENOENT;
        }
        err = eb_validate(entry);
        if (err < 0) {
            return err;
        }
        eb->ev[i].flags = (uint32_t)entry->flags;
    }
    eb->batch = (eb->args->flags & I915_EXEC_BATCH_FIRST) ? 0 : eb->count - 1u;
    batch = &eb->ev[eb->batch];
    if (batch->exec->relocation_count && !(batch->flags & EXEC_OBJECT_PINNED)) {
        batch->flags |= EB_NEEDS_BIAS;
    }
    if (batch->flags & EXEC_OBJECT_WRITE) {
        return -EINVAL; /* a batch that modifies itself */
    }
    size = batch->obj->base.size;
    if (eb->batch_start >= size || eb->args->batch_len > size - eb->batch_start) {
        return -EINVAL;
    }
    eb->batch_len = eb->args->batch_len ? eb->args->batch_len : (uint32_t)(size - eb->batch_start);
    return eb->batch_len ? 0 : -EINVAL;
}

/* ------------------------------------------------------------------------
 * Placement
 */

/** The object's binding of the whole of it, or NULL. */
static struct i915_vma *eb_binding(const struct drm_i915_gem_object *obj)
{
    struct i915_vma *v = obj->vmas;

    while (v && v->view_pages) {
        v = v->obj_next;
    }
    return v;
}

/** What the entry asks of its binding, as Linux's eb_pin_flags: its pad and
 * alignment, and its pinned offset or the batch's bias. */
static void eb_pin_args(const struct eb_vma *ev, struct i915_pin_args *a)
{
    k_memset(a, 0, sizeof(*a));
    a->align = ev->exec->alignment;
    a->size = ev->exec->pad_to_size;
    if (ev->flags & EXEC_OBJECT_PINNED) {
        a->flags = I915_PIN_OFFSET_FIXED;
        a->offset = ev->exec->offset;
    } else if (ev->flags & EB_NEEDS_BIAS) {
        a->flags = I915_PIN_OFFSET_BIAS;
        a->offset = EB_BATCH_BIAS;
    }
}

/** Whether the entry's binding fails what it asks, as Linux's
 * eb_vma_misplaced. */
static int eb_misplaced(const struct eb_vma *ev)
{
    const struct i915_vma *vma = ev->vma;

    if (vma->node_size < ev->exec->pad_to_size) {
        return 1;
    }
    if (ev->exec->alignment && (vma->start & (ev->exec->alignment - 1u))) {
        return 1;
    }
    if ((ev->flags & EXEC_OBJECT_PINNED) && vma->start != ev->exec->offset) {
        return 1;
    }
    return (ev->flags & EB_NEEDS_BIAS) && vma->start < EB_BATCH_BIAS;
}

/** Drop the entry's pin of its binding, if it holds one. */
static void eb_unpin(struct eb_vma *ev)
{
    if (ev->flags & EB_HAS_PIN) {
        i915_vma_unpin(ev->vma);
        ev->flags &= ~EB_HAS_PIN;
    }
}

/**
 * Pin the entry's binding where it lies, or else at the offset the program
 * last saw, or else anywhere free that serves it, evicting nothing, as
 * Linux's eb_pin_vma: 0, or -ENOSPC when the binding must move first or
 * has no place yet, which the reservation's passes then give it, as Linux
 * leaves every failure here to them.
 */
static int32_t eb_pin_first(struct eb_vma *ev)
{
    struct i915_vma *v = eb_binding(ev->obj);
    struct i915_pin_args a;
    int32_t err;

    k_memset(&a, 0, sizeof(a));
    a.flags = I915_PIN_NOEVICT | I915_PIN_OFFSET_FIXED;
    a.offset = v ? v->start : ev->exec->offset & ~(uint64_t)(I915_GTT_PAGE_SIZE - 1u);
    err = i915_vma_pin_args(ev->obj, &a, &ev->vma);
    if (err != 0) {
        if (ev->flags & EXEC_OBJECT_PINNED) {
            return -ENOSPC;
        }
        eb_pin_args(ev, &a);
        a.flags |= I915_PIN_NOEVICT;
        if (i915_vma_pin_args(ev->obj, &a, &ev->vma) != 0) {
            return -ENOSPC;
        }
    }
    ev->flags |= EB_HAS_PIN;
    if (eb_misplaced(ev)) {
        eb_unpin(ev);
        (void)i915_vma_unbind(ev->vma, 1);
        return -ENOSPC;
    }
    return 0;
}

/** Bind and pin the entry where it asks, as Linux's eb_reserve_vma, with
 * the pass's `flags`: 0, or the errors of `i915_vma_pin_args`. */
static int32_t eb_reserve_one(struct eb_vma *ev, uint32_t flags)
{
    struct i915_pin_args a;
    int32_t err;

    eb_pin_args(ev, &a);
    a.flags |= flags;
    err = i915_vma_pin_args(ev->obj, &a, &ev->vma);
    if (err == 0) {
        ev->flags = (ev->flags | EB_HAS_PIN) & ~EB_UNBOUND;
    }
    return err;
}

/** Give each object that waits a place, those with a fixed one first, as
 * the passes of Linux's eb_reserve: 0 or the first error. */
static int32_t eb_reserve_pass(struct eb *eb, uint32_t flags)
{
    uint32_t i;
    int32_t err;
    int sweep;

    for (sweep = 0; sweep < 2; sweep++) {
        for (i = 0; i < eb->count; i++) {
            struct eb_vma *ev = &eb->ev[i];

            if (!(ev->flags & EB_UNBOUND) || (sweep == 0) != !!(ev->flags & EXEC_OBJECT_PINNED)) {
                continue;
            }
            err = eb_reserve_one(ev, flags);
            if (err != 0) {
                return err;
            }
        }
    }
    return 0;
}

/**
 * Bind and pin every object of the call, as Linux's eb_validate_vmas and
 * eb_reserve: each where it lies or was, if that serves; then those left
 * without evicting what the GPU still uses; then all but the pinned ones
 * placed again with eviction that waits; then all of them once the GPU is
 * idle and every binding nothing pins is undone. An object that moved
 * takes its new offset with EB_UPDATE, and relocations are then needed.
 *
 * @return 0; -ENOSPC; -EINVAL for a pinned offset outside the GTT or not
 *         aligned; -EINTR; -EAGAIN for a userptr object whose pages
 *         changed; or the errors of binding.
 */
static int32_t eb_reserve(struct eb *eb)
{
    uint32_t pass, i, unbound = 0;
    int32_t err = 0;

    for (i = 0; i < eb->count; i++) {
        if (eb_pin_first(&eb->ev[i]) != 0) {
            eb->ev[i].flags |= EB_UNBOUND;
            unbound = 1;
        }
    }
    for (pass = 0; unbound && pass <= 2; pass++) {
        if (pass >= 1) {
            for (i = 0; i < eb->count; i++) {
                struct eb_vma *ev = &eb->ev[i];

                if (pass == 1 && (ev->flags & EXEC_OBJECT_PINNED) && (ev->flags & EB_HAS_PIN)) {
                    continue;
                }
                eb_unpin(ev);
                ev->flags |= EB_UNBOUND;
            }
        }
        if (pass == 2) {
            err = i915_gem_evict_vm(eb->i915);
            if (err != 0) {
                return err;
            }
        }
        err = eb_reserve_pass(eb, pass == 0 ? I915_PIN_NONBLOCK : 0);
        if (err != -ENOSPC) {
            break;
        }
    }
    if (err != 0) {
        return err;
    }
    for (i = 0; i < eb->count; i++) {
        struct eb_vma *ev = &eb->ev[i];

        if (ev->exec->offset != ev->vma->start) {
            ev->exec->offset = ev->vma->start | EB_UPDATE;
            eb->has_reloc = 1;
        }
    }
    return 0;
}

/* ------------------------------------------------------------------------
 * Relocations
 */

/*
 * Where the relocations of one object are written: through the
 * processor's view, its pages pinned and its cache flushed as
 * `i915_gem_object_prepare_write` says, or, for an import, which has no
 * view of the processor's, through a page of the aperture taken for them,
 * as Linux's reloc_kmap and reloc_iomap.
 */
struct eb_reloc_cache {
    struct drm_i915_gem_object *obj;
    uint32_t clflush;
    uint8_t ready;
    uint8_t gtt;
    uint64_t node;
};

/** Make the cache ready to write `obj`: 0, or the errors of waiting for
 * the GPU, of the pages and of taking a page of the aperture. */
static int32_t eb_reloc_prepare(struct eb_reloc_cache *c, struct drm_i915_gem_object *obj)
{
    struct drm_i915_private *i915 = to_i915(obj->base.dev);
    int32_t err;

    c->obj = obj;
    if (!obj->base.import) {
        err = i915_gem_object_prepare_write(obj, &c->clflush);
    } else {
        err = i915_gem_object_set_to_gtt_domain(obj, 1);
        if (err == 0) {
            err = i915_ggtt_reserve(i915, I915_GTT_PAGE_SIZE, I915_PIN_MAPPABLE | I915_PIN_NOEVICT,
                                    &c->node);
        }
        c->gtt = 1;
    }
    c->ready = err == 0;
    return err;
}

/** Undo `eb_reloc_prepare`, the writes ordered before what follows. */
static void eb_reloc_finish(struct eb_reloc_cache *c)
{
    struct drm_i915_private *i915;

    if (!c->ready) {
        return;
    }
    if (c->gtt) {
        i915 = to_i915(c->obj->base.dev);
        (void)i915_ggtt_clear_range(i915, c->node, I915_GTT_PAGE_SIZE, 0);
        i915_ggtt_release(i915, c->node);
    } else {
        if (c->clflush & I915_CLFLUSH_AFTER) {
            arch_dma_fence();
        }
        i915_gem_object_unpin_pages(c->obj);
    }
    c->ready = 0;
}

/** Write the 32-bit `value` at `offset` of the cache's object: 0, -ENOMEM
 * without a mapping, -ENOSPC without a window, or the bind's error. */
static int32_t eb_reloc_write(struct eb_reloc_cache *c, uint64_t offset, uint32_t value)
{
    struct drm_i915_gem_object *obj = c->obj;
    struct drm_i915_private *i915 = to_i915(obj->base.dev);
    uint32_t pgoff = (uint32_t)(offset >> PAGE_SHIFT), in = (uint32_t)(offset & (PAGE_SIZE - 1));
    uint64_t page = obj->frames[pgoff];
    uint8_t *va;
    uint32_t *p;
    int32_t err;

    if (c->gtt) {
        err = i915_ggtt_insert(i915, c->node, &page, 1, I915_CACHE_NONE);
        if (err != 0) {
            return err;
        }
        va = (uint8_t *)i915_aperture_kmap(&i915->hw, &i915->gem.aperture, c->node);
        if (!va) {
            return -ENOSPC;
        }
        *(volatile uint32_t *)(void *)(va + in) = value;
        arch_dma_fence();
        i915_aperture_sync(&i915->hw, c->node, I915_GTT_PAGE_SIZE, 1);
        i915_aperture_kunmap(&i915->hw, &i915->gem.aperture, va);
        return 0;
    }
    va = (uint8_t *)kmap_temp(obj->frames[pgoff]);
    if (!va) {
        return -ENOMEM;
    }
    p = (uint32_t *)(void *)(va + in);
    if (c->clflush & I915_CLFLUSH_BEFORE) {
        arch_dma_flush(p, sizeof(*p));
    }
    *p = value;
    if (c->clflush & I915_CLFLUSH_AFTER) {
        arch_dma_flush(p, sizeof(*p));
    }
    kunmap_temp(va);
    return 0;
}

/**
 * Apply one relocation of `ev`'s object, as Linux's eb_relocate_entry and
 * relocate_entry: its target found, given EXEC_OBJECT_WRITE when it has a
 * write domain, and, unless the program presumed the target's offset
 * right, the target's offset plus the signed delta written at the
 * relocation's offset, the object kept out of EXEC_OBJECT_ASYNC, and the
 * target's offset given in `*presumed` for the program.
 *
 * @return 1 when written, 0 when the presumed offset was right; -ENOENT for
 *         no such target; -EINVAL for a write to more than one domain, to a
 *         domain not the GPU's, or for an offset past the object or not of
 *         whole dwords; or the errors of the write.
 */
static int32_t eb_relocate_entry(struct eb *eb, struct eb_vma *ev,
                                 const struct drm_i915_gem_relocation_entry *r,
                                 struct eb_reloc_cache *c, uint64_t *presumed)
{
    struct eb_vma *target = eb_target(eb, r->target_handle);
    int32_t err;

    if (!target) {
        return -ENOENT;
    }
    if (r->write_domain & (r->write_domain - 1u)) {
        return -EINVAL;
    }
    if ((r->write_domain | r->read_domains) & ~I915_GEM_GPU_DOMAINS) {
        return -EINVAL;
    }
    if (r->write_domain) {
        target->flags |= EXEC_OBJECT_WRITE;
    }
    if (target->vma->start == r->presumed_offset) {
        return 0;
    }
    if (ev->vma->size < 4u || r->offset > ev->vma->size - 4u || (r->offset & 3u)) {
        return -EINVAL;
    }
    ev->flags &= ~EXEC_OBJECT_ASYNC;
    if (!c->ready) {
        err = eb_reloc_prepare(c, ev->obj);
        if (err != 0) {
            return err;
        }
    }
    err = eb_reloc_write(c, r->offset, (uint32_t)target->vma->start + r->delta);
    if (err != 0) {
        return err;
    }
    *presumed = target->vma->start;
    return 1;
}

/**
 * Copy in the relocations of every object, before the GEM lock is taken,
 * as Linux's eb_copy_relocations (gem/i915_gem_execbuffer.c:1644-1714),
 * so that no fault on a mapping of the device's objects, which takes the
 * lock, comes while it is held.
 *
 * @return 0; -EINVAL for a count whose bytes pass the address space;
 *         -EFAULT for an array outside user space or that cannot be read;
 *         -ENOMEM.
 */
static int32_t eb_relocs_copy(struct eb *eb)
{
    struct eb_vma *ev;
    uint64_t user, bytes;
    uint32_t i, n;

    for (i = 0; i < eb->count; i++) {
        ev = &eb->ev[i];
        n = ev->exec->relocation_count;
        user = ev->exec->relocs_ptr;
        if (n == 0) {
            continue;
        }
        bytes = (uint64_t)n * (sizeof(ev->relocs[0]) + 1u);
        if (bytes > (uint64_t)(uintptr_t)-1) {
            return -EINVAL;
        }
        if (user > (uint64_t)(uintptr_t)-1 ||
            !user_range_ok((uintptr_t)user, (size_t)n * sizeof(ev->relocs[0]))) {
            return -EFAULT;
        }
        ev->relocs = (struct drm_i915_gem_relocation_entry *)kmalloc((size_t)bytes);
        if (!ev->relocs) {
            return -ENOMEM;
        }
        ev->moved = (uint8_t *)(void *)(ev->relocs + n);
        k_memset(ev->moved, 0, n);
        if (copy_from_user(ev->relocs, EB_USER_PTR(user), (size_t)n * sizeof(ev->relocs[0])) != 0) {
            return -EFAULT;
        }
    }
    return 0;
}

/**
 * Give the program the presumed offset of each relocation written, after
 * the GEM lock is let go, as Linux's eb_relocate_vma gives it with the
 * fault handler kept out (gem/i915_gem_execbuffer.c:1575-1590); a fault
 * ends the object's, an error there ignored, as Linux ignores it.
 */
static void eb_relocs_out(struct eb *eb)
{
    struct eb_vma *ev;
    uint32_t i, j;

    for (i = 0; eb->ev && i < eb->count; i++) {
        ev = &eb->ev[i];
        for (j = 0; ev->relocs && j < ev->exec->relocation_count; j++) {
            if (ev->moved[j] &&
                copy_to_user(
                    EB_USER_PTR(ev->exec->relocs_ptr + j * sizeof(ev->relocs[0]) + EB_PRESUMED_AT),
                    &ev->relocs[j].presumed_offset, sizeof(uint64_t)) != 0) {
                break;
            }
        }
    }
}

/** Apply the relocations of `ev`'s object from the call's copy, as Linux's
 * eb_relocate_vma: each one written marked for `eb_relocs_out`, with the
 * target's offset as its presumed one. 0 or a relocation's error. */
static int32_t eb_relocate_vma(struct eb *eb, struct eb_vma *ev)
{
    struct eb_reloc_cache c;
    uint64_t presumed;
    uint32_t i, n = ev->exec->relocation_count;
    int32_t err = 0;

    k_memset(&c, 0, sizeof(c));
    for (i = 0; i < n && err == 0; i++) {
        err = eb_relocate_entry(eb, ev, &ev->relocs[i], &c, &presumed);
        if (err == 1) {
            err = 0;
            ev->relocs[i].presumed_offset = presumed;
            ev->moved[i] = 1;
        }
    }
    eb_reloc_finish(&c);
    return err;
}

/** Apply every object's relocations once the objects are placed, unless
 * none is needed: 0 or the first error. */
static int32_t eb_relocate(struct eb *eb)
{
    uint32_t i;
    int32_t err;

    if (!eb->has_reloc) {
        return 0;
    }
    for (i = 0; i < eb->count; i++) {
        if (eb->ev[i].relocs) {
            err = eb_relocate_vma(eb, &eb->ev[i]);
            if (err != 0) {
                return err;
            }
        }
    }
    return 0;
}

/* ------------------------------------------------------------------------
 * Fences
 */

/** Drop the call's syncobjs, the fences they held and the chain records
 * no syncobj took. */
static void eb_fences_put(struct eb *eb)
{
    size_t i;

    for (i = 0; i < eb->nfences; i++) {
        dma_fence_put(eb->fences[i].fence);
        drm_syncobj_put(eb->fences[i].syncobj);
        kfree(eb->fences[i].chain);
    }
    kfree(eb->fences);
    eb->fences = 0;
    eb->nfences = 0;
}

/**
 * Make room for `n` more syncobjs after the call's, as Linux's krealloc of
 * the array, after checking that `n` entries of `each` bytes at the user
 * address `user` fit the address space and are the program's.
 *
 * @return 0; -EINVAL for too many entries; -EFAULT; -ENOMEM.
 */
static int32_t eb_fences_grow(struct eb *eb, uint64_t n, uint64_t user, size_t each)
{
    struct eb_fence *f;
    uint64_t total = (uint64_t)eb->nfences + n;

    if (n > (uint64_t)(uintptr_t)-1 / each ||
        total > (uint64_t)((size_t)-1 / sizeof(struct eb_fence))) {
        return -EINVAL;
    }
    if (user > (uint64_t)(uintptr_t)-1 || !user_range_ok((uintptr_t)user, (size_t)n * each)) {
        return -EFAULT;
    }
    f = (struct eb_fence *)kzalloc((size_t)total * sizeof(struct eb_fence));
    if (!f) {
        return -ENOMEM;
    }
    if (eb->nfences) {
        k_memcpy(f, eb->fences, (size_t)eb->nfences * sizeof(struct eb_fence));
    }
    kfree(eb->fences);
    eb->fences = f;
    return 0;
}

/**
 * Take the syncobjs of I915_EXEC_FENCE_ARRAY, as Linux's add_fence_array:
 * each must exist, and one to wait for, whether or not it also signals,
 * must hold a fence, which is kept.
 *
 * @return 0; -EINVAL for too many entries, an unknown flag or a fence to
 *         wait for that is not there; -EFAULT; -ENOENT for no such
 *         syncobj; -ENOMEM.
 */
static int32_t eb_fence_array(struct eb *eb)
{
    struct drm_i915_gem_exec_fence uf;
    uint64_t user = eb->args->cliprects_ptr;
    uint32_t n = eb->args->num_cliprects, i;
    struct eb_fence *f;
    int32_t err;

    if (!(eb->args->flags & I915_EXEC_FENCE_ARRAY) || n == 0) {
        return 0;
    }
    err = eb_fences_grow(eb, n, user, sizeof(uf));
    if (err < 0) {
        return err;
    }
    for (i = 0; i < n; i++, user += sizeof(uf)) {
        if (copy_from_user(&uf, EB_USER_PTR(user), sizeof(uf)) != 0) {
            return -EFAULT;
        }
        if (uf.flags & __I915_EXEC_FENCE_UNKNOWN_FLAGS) {
            return -EINVAL;
        }
        f = &eb->fences[eb->nfences];
        f->syncobj = drm_syncobj_find(eb->file, uf.handle);
        if (!f->syncobj) {
            return -ENOENT;
        }
        f->flags = uf.flags;
        eb->nfences++;
        if (uf.flags & I915_EXEC_FENCE_WAIT) {
            f->fence = drm_syncobj_fence_get(f->syncobj);
            if (!f->fence) {
                return -EINVAL;
            }
        }
    }
    return 0;
}

/**
 * Take one syncobj of the timeline fences at `point`, as an entry of
 * Linux's add_timeline_fence_array. The request waits for the fence of the
 * point whenever the syncobj holds one, whatever the flags; for a point to
 * signal that the timeline has not reached, it waits for the newest. A
 * point to wait for that has signalled and gone is left out. A timeline
 * point to signal gets its chain record now, since the add that takes it
 * cannot fail.
 *
 * @return 0; -ENOENT for no such syncobj; -EINVAL for a fence to wait for
 *         that is not there, a point the timeline has not reached, or a
 *         timeline point both waited for and signalled; -ENOMEM.
 */
static int32_t eb_timeline_fence(struct eb *eb, uint32_t handle, uint64_t point, uint32_t flags)
{
    struct eb_fence *f = &eb->fences[eb->nfences];
    struct drm_syncobj *s = drm_syncobj_find(eb->file, handle);
    struct dma_fence *fence;
    int32_t err;

    if (!s) {
        return -ENOENT;
    }
    fence = drm_syncobj_fence_get(s);
    if (!fence && flags && !(flags & I915_EXEC_FENCE_SIGNAL)) {
        drm_syncobj_put(s);
        return -EINVAL;
    }
    if (fence && dma_fence_chain_find_seqno(&fence, point) != 0 &&
        !(flags & I915_EXEC_FENCE_SIGNAL)) {
        dma_fence_put(fence);
        drm_syncobj_put(s);
        return -EINVAL;
    }
    if (!fence && !(flags & I915_EXEC_FENCE_SIGNAL)) {
        drm_syncobj_put(s);
        return 0;
    }
    if (point != 0 && (flags & I915_EXEC_FENCE_SIGNAL)) {
        f->chain = (flags & I915_EXEC_FENCE_WAIT) ? 0 : dma_fence_chain_alloc();
        if (!f->chain) {
            err = (flags & I915_EXEC_FENCE_WAIT) ? -EINVAL : -ENOMEM;
            dma_fence_put(fence);
            drm_syncobj_put(s);
            return err;
        }
    }
    f->syncobj = s;
    f->fence = fence;
    f->flags = flags;
    f->point = point;
    eb->nfences++;
    return 0;
}

/**
 * DRM_I915_GEM_EXECBUFFER_EXT_TIMELINE_FENCES, Linux's
 * parse_timeline_fences: `fence_count` syncobjs at `handles_ptr`, each
 * with its point at `values_ptr`, taken after those of any extension
 * before it in the chain.
 *
 * @return 0; the errors of `eb_fences_grow` and `eb_timeline_fence`;
 *         -EINVAL for an unknown flag; -EFAULT.
 */
static int32_t eb_ext_timeline_fences(uintptr_t ext, void *data)
{
    struct eb *eb = (struct eb *)data;
    struct drm_i915_gem_execbuffer_ext_timeline_fences t;
    struct drm_i915_gem_exec_fence uf;
    uint64_t i, point;
    int32_t err;

    if (copy_from_user(&t, EB_USER_PTR(ext), sizeof(t)) != 0) {
        return -EFAULT;
    }
    if (t.fence_count == 0) {
        return 0;
    }
    err = eb_fences_grow(eb, t.fence_count, t.handles_ptr, sizeof(uf));
    if (err < 0) {
        return err;
    }
    if (t.values_ptr > (uint64_t)(uintptr_t)-1 ||
        !user_range_ok((uintptr_t)t.values_ptr, (size_t)t.fence_count * sizeof(point))) {
        return -EFAULT;
    }
    for (i = 0; i < t.fence_count; i++) {
        if (copy_from_user(&uf, EB_USER_PTR(t.handles_ptr + i * sizeof(uf)), sizeof(uf)) != 0) {
            return -EFAULT;
        }
        if (uf.flags & __I915_EXEC_FENCE_UNKNOWN_FLAGS) {
            return -EINVAL;
        }
        if (copy_from_user(&point, EB_USER_PTR(t.values_ptr + i * sizeof(point)), sizeof(point)) !=
            0) {
            return -EFAULT;
        }
        err = eb_timeline_fence(eb, uf.handle, point, uf.flags);
        if (err < 0) {
            return err;
        }
    }
    return 0;
}

/* The call's extensions, Linux's execbuf_extensions. */
static const i915_ext_fn eb_extensions[] = {eb_ext_timeline_fences};

/**
 * Walk I915_EXEC_USE_EXTENSIONS's chain, which `cliprects_ptr` holds, as
 * Linux's parse_execbuf2_extensions: not with a fence array or cliprects.
 * 0, -EINVAL, -E2BIG, -EFAULT or an extension's error.
 */
static int32_t eb_parse_extensions(struct eb *eb)
{
    if (!(eb->args->flags & I915_EXEC_USE_EXTENSIONS)) {
        return 0;
    }
    if ((eb->args->flags & I915_EXEC_FENCE_ARRAY) || eb->args->num_cliprects != 0) {
        return -EINVAL;
    }
    return i915_user_extensions(eb->args->cliprects_ptr, eb_extensions,
                                sizeof(eb_extensions) / sizeof(eb_extensions[0]), eb);
}

/**
 * Make the request wait for the call's fences, as Linux's eb_fences_add:
 * the sync file of I915_EXEC_FENCE_IN until it signals, or of
 * I915_EXEC_FENCE_SUBMIT until its work is given to the chip, then the
 * fence array's. 0 or -ENOMEM.
 */
static int32_t eb_await_fences(struct eb *eb, struct i915_request *rq, struct dma_fence *in)
{
    size_t i;
    int32_t err;

    if (in) {
        err = i915_request_await_fence(rq, in, !!(eb->args->flags & I915_EXEC_FENCE_SUBMIT));
        if (err < 0) {
            return err;
        }
    }
    for (i = 0; i < eb->nfences; i++) {
        if (eb->fences[i].fence) {
            err = i915_request_await_fence(rq, eb->fences[i].fence, 0);
            if (err < 0) {
                return err;
            }
        }
    }
    return 0;
}

/** Give each syncobj to signal the request's fence, as Linux's
 * signal_fence_array: a timeline's as a new point, whose chain record it
 * takes, a binary one's in place of its fence. */
static void eb_signal_fences(struct eb *eb, struct i915_request *rq)
{
    struct eb_fence *f;
    size_t i;

    for (i = 0; i < eb->nfences; i++) {
        f = &eb->fences[i];
        if (!(f->flags & I915_EXEC_FENCE_SIGNAL)) {
            continue;
        }
        if (f->chain) {
            drm_syncobj_add_point(f->syncobj, f->chain, &rq->fence.base, f->point);
            f->chain = 0;
        } else {
            drm_syncobj_replace_fence(f->syncobj, &rq->fence.base);
        }
    }
}

/* ------------------------------------------------------------------------
 * Submission
 */

/**
 * Move every object to the request, last first, as Linux's eb_move_to_gpu:
 * what the processor wrote flushed for the GPU; unless EXEC_OBJECT_ASYNC,
 * the request held until the object's writer, and for a write its readers
 * too, are done; then the request's fence made the object's writer or a
 * reader, and its domains the GPU's. A userptr object whose program's
 * pages changed since it was bound fails the call, which the program runs
 * again, as Linux's i915_gem_object_userptr_submit_done.
 *
 * @return 0, -ENOMEM, or -EAGAIN.
 */
static int32_t eb_move_to_gpu(struct eb *eb, struct i915_request *rq)
{
    struct dma_fence *f;
    uint32_t i = eb->count;
    int32_t err;

    while (i--) {
        struct eb_vma *ev = &eb->ev[i];
        struct drm_i915_gem_object *obj = ev->obj;
        int write = !!(ev->flags & EXEC_OBJECT_WRITE);

        i915_gem_object_flush_for_gpu(obj);
        /* The wait and the add under the reservation's lock, so that a
         * producer of another device adds nothing between them. */
        err = 0;
        dma_resv_lock(obj->base.resv);
        if (!(ev->flags & EXEC_OBJECT_ASYNC)) {
            f = dma_resv_snapshot_locked(obj->base.resv, write);
            err = f ? i915_request_await_fence(rq, f, 0) : -ENOMEM;
            dma_fence_put(f);
        }
        if (err == 0) {
            err = dma_resv_add_fence_locked(obj->base.resv, &rq->fence.base, write);
        }
        dma_resv_unlock(obj->base.resv);
        if (err < 0) {
            return err;
        }
        if (write) {
            obj->write_domain = I915_GEM_DOMAIN_RENDER;
            obj->read_domains = 0;
        }
        obj->read_domains |= I915_GEM_GPU_DOMAINS;
    }
    if (eb->shadow) {
        i915_gem_object_flush_for_gpu(eb->shadow);
        err = dma_resv_add_fence(eb->shadow->base.resv, &rq->fence.base, 0);
        if (err < 0) {
            return err;
        }
    }
    for (i = 0; i < eb->count; i++) {
        if (eb->ev[i].obj->userptr.stale) {
            return -EAGAIN;
        }
    }
    return 0;
}

/**
 * Emit MI_BATCH_BUFFER_START of the batch at `addr`, as hsw_emit_bb_start
 * on the render ring and gen6_emit_bb_start on the others: unprivileged
 * through the PPGTT unless `secure`. 0, or -EIO when the ring refuses it.
 */
static int32_t eb_emit_start(struct i915_request *rq, uint32_t addr, int secure)
{
    struct i915_engine *rcs = &rq->i915->gt.engine[I915_RCS0];
    uint32_t *cs = i915_request_begin(rq, 2);

    if (!cs) {
        return -EIO;
    }
    if (secure) {
        cs[0] = MI_BATCH_BUFFER_START;
    } else if (rq->engine == rcs) {
        cs[0] = MI_BATCH_BUFFER_START | MI_BATCH_PPGTT_HSW | MI_BATCH_NON_SECURE_HSW;
    } else {
        cs[0] = MI_BATCH_BUFFER_START | MI_BATCH_NON_SECURE_I965;
    }
    cs[1] = addr;
    return 0;
}

/**
 * Emit the batch's start, as Linux's eb_request_submit: with
 * I915_EXEC_GEN7_SOL_RESET first the four stream output write offsets
 * zeroed, which only the render ring has (i915_reset_gen7_sol_offsets);
 * then the batch, unprivileged unless secure, or the parser's shadow,
 * privileged, and its trampoline past the batch's length, unprivileged.
 * The aliasing PPGTT maps the shadow where the global GTT does, so its
 * one binding serves both starts.
 *
 * @return 0; -EINVAL for the reset off the render ring; -EIO when the ring
 *         refuses the commands.
 */
static int32_t eb_emit(struct eb *eb, struct i915_request *rq)
{
    struct i915_engine *rcs = &eb->i915->gt.engine[I915_RCS0];
    uint32_t addr = (uint32_t)eb->ev[eb->batch].vma->start + eb->batch_start, *cs, i;
    int32_t err;

    if (eb->args->flags & I915_EXEC_GEN7_SOL_RESET) {
        if (rq->engine != rcs) {
            return -EINVAL;
        }
        cs = i915_request_begin(rq, 10);
        if (!cs) {
            return -EIO;
        }
        cs[0] = MI_LOAD_REGISTER_IMM(4);
        for (i = 0; i < 4; i++) {
            cs[1 + 2 * i] = GEN7_SO_WRITE_OFFSET(i);
            cs[2 + 2 * i] = 0;
        }
        cs[9] = MI_NOOP;
    }
    if (!eb->shadow_vma) {
        rq->batch = addr;
        return eb_emit_start(rq, addr, (eb->dispatch & EB_DISPATCH_SECURE) != 0);
    }
    addr = (uint32_t)eb->shadow_vma->start;
    rq->batch = addr;
    err = eb_emit_start(rq, addr, 1);
    if (err == 0) {
        err = eb_emit_start(rq, addr + eb->batch_len, 0);
    }
    return err;
}

/**
 * The context's state the flags name, as Linux's eb_select_engine: with
 * the context's engine map, the slot in I915_EXEC_RING_MASK; without, the
 * legacy ring of eb_select_legacy_ring, where the default ring and RENDER
 * run on rcs0, BLT on bcs0, BSD on vcs0 whatever its I915_EXEC_BSD_MASK
 * says, since Haswell has one video ring, and VEBOX on vecs0; BSD's flags
 * with another ring and any other number name no engine. With a reference
 * in `*out`; 0, -EINVAL, or -EIO for a state a hang banned, which runs
 * nothing more, as Linux's __eb_pin_engine refuses it.
 */
static int32_t eb_select_engine(struct eb *eb, struct intel_context **out)
{
    uint32_t ring = (uint32_t)(eb->args->flags & I915_EXEC_RING_MASK), idx;

    if (eb->ctx->nmap >= 0) {
        idx = ring;
    } else if (ring != I915_EXEC_BSD && (eb->args->flags & I915_EXEC_BSD_MASK)) {
        return -EINVAL;
    } else if (ring == I915_EXEC_DEFAULT || ring == I915_EXEC_RENDER) {
        idx = I915_RCS0;
    } else if (ring == I915_EXEC_BLT) {
        idx = I915_BCS0;
    } else if (ring == I915_EXEC_BSD) {
        idx = I915_VCS0;
    } else if (ring == I915_EXEC_VEBOX) {
        idx = I915_VECS0;
    } else {
        return -EINVAL;
    }
    *out = i915_gem_context_engine(eb->ctx, idx);
    if (!*out) {
        return -EINVAL;
    }
    return (*out)->banned ? -EIO : 0;
}

/* The lock of every GT's pool of shadow batches: its lists and each take
 * of a shadow's reference. */
static struct spinlock pool_lock = SPINLOCK_INIT_NAMED("pool_lock");

/* How long a shadow stays in the pool once it is idle, Linux's HZ in
 * pool_free_work (gt/intel_gt_buffer_pool.c:93). */
#define I915_POOL_KEEP_MS 1000u

/** The bucket of a shadow of `size` bytes, Linux's bucket_for_size
 * (gt/intel_gt_buffer_pool.c:14-28): 2^n pages in bucket n, 8 or more in
 * the last. */
static uint32_t pool_bucket(uint64_t size)
{
    uint64_t pages = size >> PAGE_SHIFT;
    uint32_t n = 0;

    while (pages > 1u && n < I915_POOL_BUCKETS - 1u) {
        pages >>= 1;
        n++;
    }
    return n;
}

/**
 * A shadow of at least `size` bytes for the parser, as Linux's
 * intel_gt_get_buffer_pool (gt/intel_gt_buffer_pool.c:170-215): the first
 * idle one of its bucket, its pages and binding kept, or a new one put in
 * the pool. The caller gets a reference of its own and the object in the
 * processor's domain for its copy. No new shadow goes in the pool of a GT
 * the unplug wedged for good, since the unplug's trim of every shadow may
 * have run already, and a shadow it left would hold the device for good;
 * Linux flushes its pool at the driver's remove and again at the GT's
 * release (gt/intel_gt.c:792,849).
 *
 * @return 0; -EIO once the device is unplugged; or the errors of
 *         `i915_gem_object_create` and `i915_gem_object_set_to_cpu_domain`.
 */
static int32_t pool_get(struct drm_i915_private *i915, uint64_t size,
                        struct drm_i915_gem_object **out)
{
    struct i915_gt *gt = &i915->gt;
    struct drm_i915_gem_object *obj;
    uint32_t b, flags;
    int32_t err;

    size = (size + PAGE_SIZE - 1u) & ~(uint64_t)(PAGE_SIZE - 1u);
    b = pool_bucket(size);
    flags = spin_lock_irqsave(&pool_lock);
    for (obj = gt->pool[b]; obj; obj = obj->pool_next) {
        if (obj->base.size >= size && obj->base.refs == 1u) {
            drm_gem_object_get(&obj->base);
            obj->pool_idle = 0;
            break;
        }
    }
    spin_unlock_irqrestore(&pool_lock, flags);
    if (obj) {
        err = i915_gem_object_set_to_cpu_domain(obj, 1);
        if (err != 0) {
            i915_gem_object_put(obj);
            return err;
        }
        *out = obj;
        return 0;
    }
    err = i915_gem_object_create(i915, size, &obj);
    if (err != 0) {
        return err;
    }
    drm_gem_object_get(&obj->base);
    obj->pool_idle = 0;
    flags = spin_lock_irqsave(&pool_lock);
    if (gt->wedged_terminal) {
        spin_unlock_irqrestore(&pool_lock, flags);
        i915_gem_object_put(obj);
        i915_gem_object_put(obj);
        return -EIO;
    }
    obj->pool_next = gt->pool[b];
    gt->pool[b] = obj;
    spin_unlock_irqrestore(&pool_lock, flags);
    *out = obj;
    return 0;
}

/** Whether trim `how` drops `obj`, at the tick `now`, never 0: every
 * shadow for I915_POOL_ALL, and an idle one for I915_POOL_IDLE, or for
 * I915_POOL_STALE once it has been idle for I915_POOL_KEEP_MS. A shadow a
 * pass finds idle with no stamp is stamped, as Linux's pool_retire stamps
 * a node's age when its last request retires
 * (gt/intel_gt_buffer_pool.c:118); the retire pass that lets a request's
 * hold go trims right after. `pool_lock` is held. */
static int pool_drops(struct drm_i915_gem_object *obj, int how, uint64_t now)
{
    if (how == I915_POOL_ALL) {
        return 1;
    }
    if (obj->base.refs != 1u) {
        return 0;
    }
    if (how == I915_POOL_IDLE) {
        return 1;
    }
    if (obj->pool_idle == 0) {
        obj->pool_idle = now;
    }
    return now - obj->pool_idle >= MS_TO_TICKS(I915_POOL_KEEP_MS);
}

uint32_t i915_gem_pool_trim(struct drm_i915_private *i915, int how)
{
    struct i915_gt *gt = &i915->gt;
    struct drm_i915_gem_object *drop = 0, **link, *obj;
    uint64_t now = rtc_ticks_now();
    uint32_t b, flags, freed = 0;

    if (now == 0) {
        now = 1;
    }
    flags = spin_lock_irqsave(&pool_lock);
    for (b = 0; b < I915_POOL_BUCKETS; b++) {
        link = &gt->pool[b];
        while ((obj = *link) != 0) {
            if (pool_drops(obj, how, now)) {
                if (obj->base.refs == 1u) {
                    freed += obj->pages.held;
                }
                *link = obj->pool_next;
                obj->pool_next = drop;
                drop = obj;
            } else {
                link = &obj->pool_next;
            }
        }
    }
    spin_unlock_irqrestore(&pool_lock, flags);
    while ((obj = drop) != 0) {
        drop = obj->pool_next;
        obj->pool_next = 0;
        i915_gem_object_put(obj);
    }
    return freed;
}

/**
 * Check the batch before it runs, as Linux's eb_parse on graphics 7: a
 * batch whose length the call gives, on an engine that parses, whoever the
 * caller is, is copied to a shadow of at least its length and the
 * trampoline's from the GT's pool (`pool_get`, as Linux's eb_parse takes
 * it from intel_gt_get_buffer_pool, i915_gem_execbuffer.c:2298-2303),
 * bound in the global GTT, which runs in its place (i915_cmd_parser); any
 * other batch runs as it is.
 *
 * @return 0; the parser's -EINVAL; -ENOMEM; -EIO once the device is
 *         unplugged; -ENODEV for a batch that is an import; -ENOSPC without
 *         room in the GTT; or the errors of the wait for the batch's writers.
 */
static int32_t eb_parse(struct eb *eb)
{
    struct eb_vma *ev = &eb->ev[eb->batch];
    struct i915_cmd_parse p;
    uint32_t clflush;
    int32_t err;

    if (eb->args->batch_len == 0 || !i915_engine_using_cmd_parser(eb->ce->engine)) {
        return 0;
    }
    err =
        pool_get(eb->i915, (uint64_t)eb->batch_len + I915_CMD_PARSER_TRAMPOLINE_SIZE, &eb->shadow);
    if (err != 0) {
        return err;
    }
    err = i915_gem_object_pin_pages(eb->shadow);
    if (err != 0) {
        return err;
    }
    err = i915_gem_object_prepare_read(ev->obj, &clflush);
    if (err == 0) {
        p.batch = ev->obj->frames;
        p.offset = eb->batch_start;
        p.len = eb->batch_len;
        p.addr = (uint32_t)ev->vma->start + eb->batch_start;
        p.clflush = clflush;
        p.shadow = eb->shadow->frames;
        p.shadow_size = (uint32_t)eb->shadow->base.size;
        err = i915_cmd_parser(eb->ce->engine, &p);
        i915_gem_object_unpin_pages(ev->obj);
    }
    if (err == 0) {
        err = i915_vma_pin(eb->shadow, 0, 0, 0, 0, &eb->shadow_vma);
    }
    i915_gem_object_unpin_pages(eb->shadow);
    if (err == 0) {
        eb->dispatch |= EB_DISPATCH_SECURE;
    }
    return err;
}

/** Free a hold and let go of what it holds. */
static void eb_hold_release(struct i915_request_hold *base)
{
    struct eb_hold *hold = (struct eb_hold *)base;
    uint32_t i;

    for (i = 0; i < hold->count; i++) {
        i915_gem_object_put(hold->obj[i]);
    }
    kfree(hold);
}

/** Make the call's hold, with a reference to each object and the shadow.
 * 0, or -ENOMEM. */
static int32_t eb_hold_make(struct eb *eb)
{
    uint32_t n = eb->count + (eb->shadow ? 1u : 0u), i;
    struct eb_hold *hold;

    hold = (struct eb_hold *)kmalloc(sizeof(*hold) + (size_t)n * sizeof(hold->obj[0]));
    if (!hold) {
        return -ENOMEM;
    }
    hold->base.release = eb_hold_release;
    hold->obj = (struct drm_i915_gem_object **)(void *)(hold + 1);
    hold->count = n;
    for (i = 0; i < eb->count; i++) {
        hold->obj[i] = eb->ev[i].obj;
        drm_gem_object_get(&hold->obj[i]->base);
    }
    if (eb->shadow) {
        hold->obj[i] = eb->shadow;
        drm_gem_object_get(&eb->shadow->base);
    }
    eb->hold = hold;
    return 0;
}

/**
 * Build and add the call's request, as the second half of Linux's
 * i915_gem_do_execbuffer: once the request exists it is added whatever
 * happens, a failure set on its fence so that its batch does not run, and
 * the fence array is signalled with it; the out-fence's sync file is made
 * with the request, as Linux's eb_requests_create makes it, and goes to
 * the program after the GEM lock, only when all went well. Under the GEM
 * lock, which nothing between the request's creation and its add lets go.
 *
 * The request takes the call's hold, made first.
 *
 * @return 0; the errors of `i915_request_create`; -ENOMEM; -EAGAIN; -EINVAL
 *         for I915_EXEC_GEN7_SOL_RESET off the render ring; -ENOENT for a
 *         context destroyed meanwhile; -EIO.
 */
static int32_t eb_submit(struct eb *eb, struct dma_fence *in, int32_t out_fd)
{
    struct i915_request *rq;
    int32_t err, err2;

    lockdep_assert_held(&eb->i915->gem.lock);
    err = eb_hold_make(eb);
    if (err < 0) {
        return err;
    }
    err = i915_request_create(eb->ce, &rq);
    if (err < 0) {
        return err;
    }
    rq->hold = &eb->hold->base;
    eb->hold = 0;
    err = 0;
    if (out_fd >= 0) {
        eb->out = sync_file_new(&rq->fence.base, NULL);
        err = eb->out ? 0 : -ENOMEM;
    }
    if (err == 0) {
        err = eb_await_fences(eb, rq, in);
    }
    if (err == 0) {
        err = eb_move_to_gpu(eb, rq);
    }
    if (err == 0) {
        err = eb_emit(eb, rq);
    }
    if (err == 0 && eb->ctx->closed) {
        err = -ENOENT;
    }
    if (err != 0 && !rq->fence.base.error) {
        dma_fence_set_error(&rq->fence.base, err);
    }
    err2 = i915_request_add(rq);
    if (err == 0) {
        err = err2;
    }
    eb_signal_fences(eb, rq);
    i915_request_put(rq);
    return err;
}

/** Give back what the call took: the pins, the objects, the shadow, the
 * hold the request did not take, the context and its state, the syncobjs
 * and the table. */
static void eb_release(struct eb *eb)
{
    uint32_t i;

    if (eb->shadow_vma) {
        i915_vma_unpin(eb->shadow_vma);
    }
    i915_gem_object_put(eb->shadow);
    if (eb->hold) {
        eb_hold_release(&eb->hold->base);
    }
    for (i = 0; eb->ev && i < eb->count; i++) {
        eb_unpin(&eb->ev[i]);
        i915_gem_object_put(eb->ev[i].obj);
        eb->ev[i].obj = 0;
    }
    intel_context_put(eb->ce);
    if (eb->ctx) {
        i915_gem_context_put(eb->ctx);
    }
    eb_fences_put(eb);
    kfree(eb->lut);
}

/**
 * The part of the call under the GEM lock, `err` the error of what came
 * before: the objects placed and relocated, the batch checked, and its
 * request built and added. A failure before relocations are done clears
 * `has_reloc`, since a program that presumes offsets must not see new ones
 * in its entries.
 */
static int32_t eb_locked(struct eb *eb, int32_t err, struct dma_fence *in, int32_t out_fd)
{
    if (err == 0) {
        err = eb_reserve(eb);
    }
    if (err == 0) {
        err = eb_relocate(eb);
    }
    if (err == 0) {
        err = eb_parse(eb);
    }
    if (err != 0) {
        eb->has_reloc = 0;
        return err;
    }
    return eb_submit(eb, in, out_fd);
}

/**
 * The call once its objects' entries are copied in, as Linux's
 * i915_gem_do_execbuffer: secure batches, extensions, the fence array and
 * the in-fence checked, the out-fence's descriptor kept, the context and
 * its state on the engine found, the objects looked up and their
 * relocations copied in, all before the GEM lock is taken, as Linux's
 * (gem/i915_gem_execbuffer.c:3438-3450); then `eb_locked`'s part and the
 * call's release under it, and the out-fence's descriptor after.
 */
static int32_t eb_do(struct eb *eb)
{
    struct drm_i915_gem_execbuffer2 *args = eb->args;
    struct dma_fence *in = 0;
    int32_t err, out_fd = -1;
    int taken;

    eb->has_reloc = !(args->flags & I915_EXEC_NO_RELOC);
    eb->batch_start = args->batch_start_offset;
    if (args->flags & I915_EXEC_SECURE) {
        if (!drm_is_current_master(eb->file) || !i915_capable()) {
            return -EPERM;
        }
        eb->dispatch |= EB_DISPATCH_SECURE;
    }
    err = eb_parse_extensions(eb);
    if (err == 0) {
        err = eb_fence_array(eb);
    }
    if (err == 0 && (args->flags & (I915_EXEC_FENCE_IN | I915_EXEC_FENCE_SUBMIT))) {
        if ((args->flags & I915_EXEC_FENCE_IN) && (args->flags & I915_EXEC_FENCE_SUBMIT)) {
            err = -EINVAL;
        } else {
            in = sync_file_get_fence((int32_t)(uint32_t)args->rsvd2);
            err = in ? 0 : -EINVAL;
        }
    }
    if (err == 0 && (args->flags & I915_EXEC_FENCE_OUT)) {
        out_fd = current_proc ? fd_alloc(current_proc->fds) : -EBADF;
        err = out_fd < 0 ? out_fd : 0;
    }
    if (err == 0 && !(args->flags & I915_EXEC_HANDLE_LUT)) {
        err = eb_lut_build(eb);
    }
    if (err == 0) {
        err = i915_gem_context_lookup(eb->file, (uint32_t)(args->rsvd1 & I915_EXEC_CONTEXT_ID_MASK),
                                      &eb->ctx);
    }
    if (err == 0) {
        err = eb_select_engine(eb, &eb->ce);
    }
    if (err == 0) {
        err = eb_lookup(eb);
    }
    if (err == 0) {
        err = eb_relocs_copy(eb);
    }
    taken = i915_gem_lock(eb->i915);
    err = eb_locked(eb, err, in, out_fd);
    eb_release(eb);
    i915_gem_unlock(eb->i915, taken);
    /* The out-fence's descriptor goes in after the lock, as Linux's
     * fd_install (gem/i915_gem_execbuffer.c:3496-3505). */
    if (eb->out && err == 0) {
        sync_file_attach(out_fd, eb->out);
        args->rsvd2 = (args->rsvd2 & 0xffffffffu) | ((uint64_t)(uint32_t)out_fd << 32);
        out_fd = -1;
    } else {
        sync_file_drop(eb->out);
    }
    eb->out = 0;
    if (out_fd >= 0) {
        (void)fd_free(current_proc->fds, out_fd);
    }
    dma_fence_put(in);
    return err;
}

int32_t i915_gem_execbuffer2_ioctl(struct drm_device *dev, void *data, struct drm_file *file)
{
    struct drm_i915_gem_execbuffer2 *args = (struct drm_i915_gem_execbuffer2 *)data;
    const size_t each = sizeof(struct drm_i915_gem_exec_object2) + sizeof(struct eb_vma);
    struct drm_i915_gem_exec_object2 __user *user;
    struct eb eb;
    uint64_t offset;
    uint32_t i;
    int32_t err;

    if (args->buffer_count == 0 || args->buffer_count > 0x7fffffffu ||
        ((uint64_t)args->buffer_count + 2u) * sizeof(struct drm_i915_gem_exec_object2) >
            (size_t)-1) {
        return -EINVAL;
    }
    err = eb_check(args);
    if (err != 0) {
        return err;
    }
    if ((uint64_t)args->buffer_count * each > (size_t)-1) {
        return -ENOMEM;
    }
    k_memset(&eb, 0, sizeof(eb));
    eb.i915 = to_i915(dev);
    eb.file = file;
    eb.args = args;
    eb.count = args->buffer_count;
    eb.exec = (struct drm_i915_gem_exec_object2 *)kmalloc((size_t)eb.count * each);
    if (!eb.exec) {
        return -ENOMEM;
    }
    eb.ev = (struct eb_vma *)(void *)(eb.exec + eb.count);
    k_memset(eb.ev, 0, (size_t)eb.count * sizeof(struct eb_vma));
    user = (struct drm_i915_gem_exec_object2 __user *)EB_USER_PTR(args->buffers_ptr);
    if (args->buffers_ptr > (uint64_t)(uintptr_t)-1 ||
        copy_from_user(eb.exec, user, (size_t)eb.count * sizeof(eb.exec[0])) != 0) {
        kfree(eb.exec);
        return -EFAULT;
    }
    err = eb_do(&eb);
    /* The offsets of the objects that moved go back to the program after
     * the GEM lock, as Linux's (gem/i915_gem_execbuffer.c:3620-3637), even
     * when the request failed; a fault stops them. */
    eb_relocs_out(&eb);
    for (i = 0; eb.has_reloc && i < eb.count; i++) {
        if (!(eb.exec[i].offset & EB_UPDATE)) {
            continue;
        }
        offset = eb.exec[i].offset & ~(uint64_t)(I915_GTT_PAGE_SIZE - 1u);
        if (copy_to_user(&user[i].offset, &offset, sizeof(offset)) != 0) {
            break;
        }
    }
    for (i = 0; i < eb.count; i++) {
        kfree(eb.ev[i].relocs);
    }
    kfree(eb.exec);
    return err;
}
