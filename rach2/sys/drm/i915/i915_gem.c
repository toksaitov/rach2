/*
 * i915_gem.c: the Intel graphics driver's buffer objects, their pages, cache
 * domains and caching, and the ioctls that make, read, write, wait on and
 * ask after them, after Linux 6.12's i915_gem.c and gem/i915_gem_create.c,
 * i915_gem_domain.c, i915_gem_object.c, i915_gem_busy.c and i915_gem_wait.c.
 */

#include "i915.h"
#include "i915_drm.h"

#include "../../dmabuf/dmabuf.h"
#include "../../klog.h"
#include "../../lib/kerrno.h"
#include "../../lib/kstring.h"
#include "../../mm/kheap.h"
#include "../../mm/paging.h"
#include "../../mm/pin.h"
#include "../../mm/usercopy.h"
#include "../../proc/sched.h"

/* ------------------------------------------------------------------------
 * Pages
 */

int32_t i915_gem_object_pin_pages(struct drm_i915_gem_object *obj)
{
    uint32_t n = (uint32_t)(obj->base.size >> PAGE_SHIFT);
    struct dma_buf *buf = obj->base.import;
    uintptr_t *frames;
    struct pin *pin = 0;
    uint32_t i;
    int err;

    if (obj->frames && !obj->userptr.stale) {
        obj->pages_pin_count++;
        return 0;
    }
    if (obj->base.dev->unplugged) {
        return -ENODEV;
    }
    if (obj->madv != I915_MADV_WILLNEED) {
        /* As Linux: a purgeable object without pages gets none. */
        return -EFAULT;
    }
    if (i915_gem_object_is_userptr(obj)) {
        return i915_gem_userptr_get_pages(obj);
    }
    frames = (uintptr_t *)kmalloc((size_t)n * sizeof(*frames));
    if (!frames) {
        return -ENOMEM;
    }
    if (buf) {
        /* The dma-buf, which the object holds, keeps its exporter's pages. */
        for (i = 0; i < n; i++) {
            frames[i] = buf->ops->page ? buf->ops->page(buf, i) : 0;
            if (!frames[i]) {
                kfree(frames);
                return -EFAULT;
            }
        }
    } else {
        /* As Linux's shmem_sg_alloc_table, memory is reclaimed for the
         * pages, purging other objects, and once more after a failure. */
        mem_balance();
        if (shmem_allocate(&obj->pages, 0, obj->base.size) != 0) {
            (void)mem_reclaim();
            if (shmem_allocate(&obj->pages, 0, obj->base.size) != 0) {
                kfree(frames);
                return -ENOMEM;
            }
        }
        for (i = 0; i < n; i++) {
            frames[i] = shmem_resident(&obj->pages, i);
        }
        err = pin_frames(0, frames, n, 0, &pin);
        if (err != 0) {
            kfree(frames);
            return err == -EINVAL ? -EFAULT : err;
        }
    }
    if (obj->frames || obj->madv != I915_MADV_WILLNEED) {
        /* Reclaim slept: another caller pinned the pages meanwhile, or the
         * object was made purgeable, or purged. */
        unpin(pin);
        kfree(frames);
        if (obj->frames) {
            obj->pages_pin_count++;
            return 0;
        }
        if (obj->madv == __I915_MADV_PURGED) {
            shmem_free(&obj->pages);
        }
        return -EFAULT;
    }
    obj->frames = frames;
    obj->pin = pin;
    obj->pages_pin_count = 1;
    return 0;
}

void i915_gem_object_unpin_pages(struct drm_i915_gem_object *obj)
{
    if (obj->pages_pin_count) {
        obj->pages_pin_count--;
        if (obj->pages_pin_count == 0 && obj->userptr.stale) {
            i915_gem_userptr_put_pages(obj);
        }
    }
}

int i915_gem_object_has_pages(const struct drm_i915_gem_object *obj)
{
    return obj->frames != 0 || obj->pages.held != 0;
}

/** Whether `f` is a fence that has not signalled. */
static int fence_busy(struct dma_fence *f)
{
    return f && !dma_fence_is_signaled(f);
}

int i915_gem_object_is_active_locked(const struct drm_i915_gem_object *obj)
{
    struct dma_resv *r = obj->base.resv;
    uint32_t i;
    int busy;

    lockdep_assert_held(&r->lock);
    busy = fence_busy(r->write);
    for (i = 0; !busy && i < r->nreads; i++) {
        busy = fence_busy(r->reads[i]);
    }
    return busy;
}

int i915_gem_object_is_active(const struct drm_i915_gem_object *obj)
{
    int busy;

    dma_resv_lock(obj->base.resv);
    busy = i915_gem_object_is_active_locked(obj);
    dma_resv_unlock(obj->base.resv);
    return busy;
}

/**
 * Wait for a fence of a reservation until `deadline`, or for ever when it
 * is negative, as Linux's i915_gem_object_wait_fence
 * (gem/i915_gem_wait.c:17-33): a request of the driver through
 * `i915_request_wait` with `flags`, which spins before it asks for the
 * signal, and any other fence through `dma_fence_wait_until`. Each leaf of
 * a merge or a timeline is waited for alone, in one walk that holds the
 * point it is at, so that the merge, which a wait would arm, asks for no
 * signal, and a leaf that signals meanwhile moves no other out of the
 * walk. Each sleep lets `i915`'s GEM lock go. 0, -ETIME or -EINTR.
 */
static int32_t object_fence_wait(struct drm_i915_private *i915, struct dma_fence *f, uint32_t flags,
                                 int64_t deadline)
{
    struct dma_fence_unwrap u;
    struct dma_fence *leaf;
    int64_t now, timeout;
    int32_t err = 0;
    int dropped;

    for (leaf = dma_fence_unwrap_first(f, &u); leaf && err == 0; leaf = dma_fence_unwrap_next(&u)) {
        if (dma_fence_is_signaled(leaf)) {
            continue;
        }
        if (leaf->ops == &i915_fence_ops) {
            timeout = -1;
            if (deadline >= 0) {
                now = dma_fence_now();
                timeout = deadline > now ? deadline - now : 0;
            }
            err = i915_request_wait((struct i915_request *)leaf, flags, timeout);
        } else {
            dropped = i915_gem_lock_drop(i915);
            err = dma_fence_wait_until(leaf, (flags & I915_WAIT_INTERRUPTIBLE) != 0, deadline);
            i915_gem_lock_retake(i915, dropped);
        }
    }
    dma_fence_unwrap_end(&u);
    return err;
}

/**
 * Ask for the signal of each leaf of `f` that has not signalled, as
 * Linux's flush_lazy_signals enables every fence an i915_active tracks
 * before __i915_active_wait sleeps (i915_active.c:559-571, 589-620): a
 * listened request that completes during a later spin then hands its
 * engine to retirement (gt/intel_breadcrumbs.c:220-235). The leaves are
 * armed alone, so that a merge asks for no signal. Never sleeps.
 */
static void object_fence_arm(struct dma_fence *f)
{
    struct dma_fence_unwrap u;
    struct dma_fence *leaf;

    for (leaf = dma_fence_unwrap_first(f, &u); leaf; leaf = dma_fence_unwrap_next(&u)) {
        if (!dma_fence_is_signaled(leaf)) {
            dma_fence_enable_sw_signaling(leaf);
        }
    }
    dma_fence_unwrap_end(&u);
}

int32_t i915_gem_object_wait_idle(struct drm_i915_gem_object *obj, int intr)
{
    struct dma_fence **fences, *f;
    uint32_t i, n;
    int32_t err;

    /* Every fence busy now is armed before any wait, as Linux's unbind; one
     * added while the wait sleeps is waited for but not armed. */
    if (dma_resv_get_fences(obj->base.resv, 1, &fences, &n) < 0) {
        return -ENOMEM;
    }
    for (i = 0; i < n; i++) {
        if (fence_busy(fences[i])) {
            object_fence_arm(fences[i]);
        }
    }
    dma_resv_put_fences(fences, n);
    for (;;) {
        /* A snapshot with references of its own, since the reservation may
         * change while the wait sleeps. */
        if (dma_resv_get_fences(obj->base.resv, 1, &fences, &n) < 0) {
            return -ENOMEM;
        }
        f = 0;
        for (i = 0; !f && i < n; i++) {
            if (fence_busy(fences[i])) {
                f = fences[i];
            }
        }
        err = f ? object_fence_wait(to_i915(obj->base.dev), f, intr ? I915_WAIT_INTERRUPTIBLE : 0u,
                                    -1)
                : 0;
        dma_resv_put_fences(fences, n);
        if (!f || err == -EINTR) {
            return err;
        }
    }
}

uintptr_t i915_gem_object_frame(struct drm_i915_gem_object *obj, uint32_t pgoff)
{
    uintptr_t phys;

    if ((uint64_t)pgoff >= obj->base.size >> PAGE_SHIFT) {
        return 0;
    }
    if (i915_gem_object_is_userptr(obj)) {
        return obj->frames && !obj->userptr.stale ? obj->frames[pgoff] : 0;
    }
    if (obj->frames) {
        return obj->frames[pgoff];
    }
    if (obj->base.import) {
        struct dma_buf *b = obj->base.import;

        return b->ops->page ? b->ops->page(b, pgoff) : 0;
    }
    /* A purgeable object is given no new page, and a purged one has
     * none. */
    phys = obj->madv == __I915_MADV_PURGED ? 0 : shmem_resident(&obj->pages, pgoff);
    if (!phys && obj->madv == I915_MADV_WILLNEED &&
        shmem_allocate(&obj->pages, (uint64_t)pgoff << PAGE_SHIFT, PAGE_SIZE) == 0) {
        phys = shmem_resident(&obj->pages, pgoff);
    }
    return phys;
}

/**
 * Flush the processor's cache of the object's pages, as Linux's
 * i915_gem_clflush_object: nothing while its pages are not held for the
 * GPU, or, unless `force`, while the GPU snoops the cache for its reads.
 */
static void i915_gem_clflush_object(struct drm_i915_gem_object *obj, int force)
{
    uint32_t i, n = (uint32_t)(obj->base.size >> PAGE_SHIFT);

    if (!obj->frames) {
        return;
    }
    if (!force && (obj->cache_coherent & I915_BO_CACHE_COHERENT_FOR_READ)) {
        return;
    }
    for (i = 0; i < n; i++) {
        void *va = kmap_temp(obj->frames[i]);

        if (va) {
            arch_dma_flush(va, PAGE_SIZE);
            kunmap_temp(va);
        }
    }
    obj->cache_dirty = 0;
}

/* ------------------------------------------------------------------------
 * Objects
 */

void i915_gem_object_set_cache_coherency(struct drm_i915_gem_object *obj, int level)
{
    struct drm_i915_private *i915 = to_i915(obj->base.dev);

    obj->cache_level = (uint8_t)level;
    if (level != I915_CACHE_NONE) {
        obj->cache_coherent = I915_BO_CACHE_COHERENT_FOR_READ | I915_BO_CACHE_COHERENT_FOR_WRITE;
    } else if (i915->id->platform->has_llc) {
        obj->cache_coherent = I915_BO_CACHE_COHERENT_FOR_READ;
    } else {
        obj->cache_coherent = 0;
    }
    obj->cache_dirty = !(obj->cache_coherent & I915_BO_CACHE_COHERENT_FOR_WRITE);
}

/**
 * Undo `obj`, whose last reference is gone, with the GEM lock held: every
 * binding undone whatever its reservation holds
 * (`i915_gem_object_force_unbind`), its mmap offsets taken out of the
 * device's space, its pages unpinned and freed, a userptr object's range
 * taken off. With `may_yield`, for a holder that took the lock for the
 * free and may sleep, it gives way every 512 entries of its clears and
 * every leaf of its pages, as Linux's free worker calls cond_resched
 * between batches of its pages (gem/i915_gem_shmem.c:29). No other
 * thread reaches the object, its last reference gone, and the GEM lock
 * keeps its bindings' range whole.
 */
static void object_release(struct drm_i915_gem_object *obj, int may_yield)
{
    int i;

    i915_gem_object_force_unbind(obj, may_yield);
    if (i915_gem_object_is_userptr(obj)) {
        i915_gem_userptr_release(obj);
    }
    for (i = 0; i < I915_MMAP_TYPES; i++) {
        drm_mmap_node_remove(&obj->mmo[i].node);
    }
    unpin(obj->pin);
    kfree(obj->frames);
    if (may_yield) {
        shmem_free_yielding(&obj->pages);
    } else {
        shmem_free(&obj->pages);
    }
    kfree(obj);
}

/**
 * Undo the objects on `gem.freed`, with the GEM lock that the caller took
 * and is about to let go, giving way in each and after each, as Linux's
 * free worker does with its list (gem/i915_gem_object.c:406-433). An object
 * whose last put comes meanwhile joins the list and goes too. Each holds a
 * reference on its import, which goes after it.
 */
static void free_deferred(struct drm_i915_private *i915)
{
    struct drm_i915_gem_object *obj;
    struct dma_buf *import;

    while ((obj = i915->gem.freed) != 0) {
        i915->gem.freed = obj->free_next;
        obj->free_next = 0;
        import = obj->base.import;
        object_release(obj, 1);
        dma_buf_put(import);
        sched_give_way();
    }
}

uint32_t i915_gem_free_deferred_atomic(struct drm_i915_private *i915)
{
    struct drm_i915_gem_object *obj;
    struct dma_buf *import;
    uint32_t freed = 0;

    lockdep_assert_held(&i915->gem.lock);
    while ((obj = i915->gem.freed) != 0) {
        i915->gem.freed = obj->free_next;
        obj->free_next = 0;
        import = obj->base.import;
        freed += obj->pages.held;
        object_release(obj, 0);
        dma_buf_put(import);
    }
    return freed;
}

/**
 * The `free` of an object, once its last reference goes, which no request
 * the chip has not passed holds. The DRM core has let its fences go by
 * then.
 *
 * A free that takes the GEM lock itself, from GEM_CLOSE, a file's close, a
 * region's release (`vm_file_reap`) or an exit, may sleep in that take, so
 * it undoes the object giving way. Any last put under a holder's GEM lock
 * puts the object on `gem.freed`: an ioctl's put of the object it looked
 * up, as each row that I915_GEM_LOCKED wraps makes, EXECBUFFER2's release
 * of its objects (`eb_release`), retirement's or a pool's trim's, any of
 * which is the last when another thread closed the handle meanwhile. That
 * holder may be part way through a change that every other thread must see
 * whole; the holder that took the lock undoes the list giving way as it
 * lets the lock go (`i915_gem_unlock`). So every free gives way, as Linux
 * sends every free to its worker, which calls cond_resched after each
 * object (gem/i915_gem_object.c:423, 471-472). A free under the shrinker,
 * which may not give way, undoes the object at once with none, and the
 * shrinker undoes the list the same way
 * (`i915_gem_free_deferred_atomic`).
 */
static void i915_gem_object_free(struct drm_gem_object *base)
{
    struct drm_i915_gem_object *obj = (struct drm_i915_gem_object *)base;
    struct drm_i915_private *i915 = to_i915(base->dev);
    int taken = i915_gem_lock(i915);

    if (!taken && !i915->gem.no_yield) {
        /* The DRM core puts the import once this returns. */
        (void)dma_buf_ref(base->import);
        obj->free_next = i915->gem.freed;
        i915->gem.freed = obj;
        return;
    }
    object_release(obj, taken);
    i915_gem_unlock(i915, taken);
}

/** The `page` of an object for another device's import: its frame. */
static uintptr_t i915_gem_object_page_for_import(struct drm_gem_object *base, uint32_t pgoff)
{
    struct drm_i915_private *i915 = to_i915(base->dev);
    uintptr_t frame;
    int taken;

    if (base->dev->unplugged) {
        return 0;
    }
    taken = i915_gem_lock(i915);
    frame = i915_gem_object_frame((struct drm_i915_gem_object *)base, pgoff);
    i915_gem_unlock(i915, taken);
    return frame;
}

const struct drm_gem_object_funcs i915_gem_object_funcs = {i915_gem_object_free,
                                                           i915_gem_object_mmap_cpu,
                                                           0,
                                                           0,
                                                           i915_gem_object_page_for_import,
                                                           0,
                                                           i915_gem_object_cpu_access,
                                                           0};

struct drm_i915_gem_object *i915_gem_object_alloc(struct drm_i915_private *i915, uint64_t size)
{
    struct drm_i915_gem_object *obj = (struct drm_i915_gem_object *)kzalloc(sizeof(*obj));
    int i;

    if (!obj) {
        return 0;
    }
    drm_gem_object_init(&i915->drm, &obj->base, size, &i915_gem_object_funcs);
    obj->pages.size = size;
    obj->read_domains = I915_GEM_DOMAIN_CPU;
    obj->write_domain = I915_GEM_DOMAIN_CPU;
    i915_gem_object_set_cache_coherency(obj, i915->id->platform->has_llc ? I915_CACHE_LLC
                                                                         : I915_CACHE_NONE);
    for (i = 0; i < I915_MMAP_TYPES; i++) {
        obj->mmo[i].node.obj = &obj->base;
        obj->mmo[i].node.mmap = i915_gem_mmo_mmap;
        obj->mmo[i].type = i;
    }
    return obj;
}

int32_t i915_gem_object_create(struct drm_i915_private *i915, uint64_t size,
                               struct drm_i915_gem_object **out)
{
    struct drm_i915_gem_object *obj;

    /* A size within a page of 2^64 rounds up to 0, which is EINVAL, as
     * Linux's __i915_gem_object_create_user_ext rounds before it tests. */
    size = (size + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1);
    if (size == 0) {
        return -EINVAL;
    }
    if (size > SHMEM_MAX_SIZE || size > (uint64_t)(size_t)-1) {
        return -E2BIG;
    }
    obj = i915_gem_object_alloc(i915, size);
    if (!obj) {
        return -ENOMEM;
    }
    *out = obj;
    return 0;
}

struct drm_i915_gem_object *i915_gem_object_lookup(struct drm_file *file, uint32_t handle)
{
    struct drm_gem_object *base = drm_gem_object_lookup(file, handle);

    if (base && base->funcs != &i915_gem_object_funcs) {
        drm_gem_object_put(base);
        return 0;
    }
    return (struct drm_i915_gem_object *)base;
}

void i915_gem_object_put(struct drm_i915_gem_object *obj)
{
    if (obj) {
        drm_gem_object_put(&obj->base);
    }
}

/* ------------------------------------------------------------------------
 * Waits
 */

/** Boost the GT's clock for each request of `f`'s leaves that the chip has
 * not begun. */
static void fence_boost(struct dma_fence *f)
{
    struct dma_fence_unwrap u;
    struct dma_fence *leaf;

    for (leaf = dma_fence_unwrap_first(f, &u); leaf; leaf = dma_fence_unwrap_next(&u)) {
        if (leaf->ops == &i915_fence_ops && !i915_request_started((struct i915_request *)leaf)) {
            i915_rps_boost((struct i915_request *)leaf);
        }
    }
}

/**
 * Boost the GT's clock for every request of `n` fences, a snapshot of an
 * object's reservation, that the chip has not begun, before a wait, as
 * Linux's i915_gem_object_boost: all of them first, so that the boost does
 * not hang on the order the wait meets them.
 */
static void object_boost(struct dma_fence **fences, uint32_t n)
{
    uint32_t i;

    for (i = 0; i < n; i++) {
        fence_boost(fences[i]);
    }
}

int32_t i915_gem_object_wait(struct drm_i915_gem_object *obj, int all, int64_t timeout_ns)
{
    struct dma_fence **fences;
    int64_t deadline = -1;
    uint32_t i, n;
    int32_t err = 0;

    /* The fences of now, each with a reference, since the reservation may
     * change while a wait sleeps, as Linux's iterator walks a snapshot. */
    if (dma_resv_get_fences(obj->base.resv, all, &fences, &n) < 0) {
        return -ENOMEM;
    }
    object_boost(fences, n);
    if (timeout_ns == 0) {
        /* A test, which asks for no signal. */
        for (i = 0; i < n && !err; i++) {
            err = fence_busy(fences[i]) ? -ETIME : 0;
        }
        dma_resv_put_fences(fences, n);
        return err;
    }
    if (timeout_ns > 0) {
        int64_t now = dma_fence_now();

        deadline = timeout_ns > (int64_t)0x7fffffffffffffffLL - now ? -1 : now + timeout_ns;
    }
    for (i = 0; i < n && err == 0; i++) {
        if (fences[i]) {
            err = object_fence_wait(to_i915(obj->base.dev), fences[i],
                                    I915_WAIT_INTERRUPTIBLE | I915_WAIT_PRIORITY, deadline);
        }
    }
    dma_resv_put_fences(fences, n);
    return err;
}

/* ------------------------------------------------------------------------
 * Cache domains
 */

int i915_gem_cpu_write_needs_clflush(const struct drm_i915_gem_object *obj)
{
    if (obj->cache_dirty) {
        return 0;
    }
    return !(obj->cache_coherent & I915_BO_CACHE_COHERENT_FOR_WRITE);
}

/** Whether a write by the GPU leaves lines the processor's cache must
 * drop: not at the uncached and write-through levels. */
static int gpu_write_needs_clflush(const struct drm_i915_gem_object *obj)
{
    return !(obj->cache_level == I915_CACHE_NONE || obj->cache_level == I915_CACHE_WT);
}

void i915_gem_object_flush_write_domain(struct drm_i915_gem_object *obj, uint32_t flush_domains)
{
    struct i915_vma *vma;

    if (!(obj->write_domain & flush_domains)) {
        return;
    }
    switch (obj->write_domain) {
    case I915_GEM_DOMAIN_GTT:
        for (vma = obj->vmas; vma; vma = vma->obj_next) {
            i915_vma_flush_writes(vma);
        }
        break;
    case I915_GEM_DOMAIN_WC:
        arch_dma_fence();
        break;
    case I915_GEM_DOMAIN_CPU:
        i915_gem_clflush_object(obj, 0);
        break;
    case I915_GEM_DOMAIN_RENDER:
        if (gpu_write_needs_clflush(obj)) {
            obj->cache_dirty = 1;
        }
        break;
    default:
        break;
    }
    obj->write_domain = 0;
}

void i915_gem_object_flush_for_gpu(struct drm_i915_gem_object *obj)
{
    i915_gem_object_flush_write_domain(obj, I915_GEM_DOMAIN_CPU | I915_GEM_DOMAIN_GTT |
                                                I915_GEM_DOMAIN_WC);
    if (obj->cache_dirty) {
        i915_gem_clflush_object(obj, 0);
    }
}

/** Begin a write by the processor: the object in the CPU's domains alone. */
static void start_cpu_write(struct drm_i915_gem_object *obj)
{
    obj->read_domains = I915_GEM_DOMAIN_CPU;
    obj->write_domain = I915_GEM_DOMAIN_CPU;
    if (i915_gem_cpu_write_needs_clflush(obj)) {
        obj->cache_dirty = 1;
    }
}

int32_t i915_gem_object_set_to_wc_domain(struct drm_i915_gem_object *obj, int write)
{
    int32_t err = i915_gem_object_wait(obj, write, -1);

    if (err != 0) {
        return err;
    }
    if (obj->write_domain == I915_GEM_DOMAIN_WC) {
        return 0;
    }
    err = i915_gem_object_pin_pages(obj);
    if (err != 0) {
        return err;
    }
    i915_gem_object_flush_write_domain(obj, ~I915_GEM_DOMAIN_WC);
    if (!(obj->read_domains & I915_GEM_DOMAIN_WC)) {
        arch_dma_fence();
    }
    obj->read_domains |= I915_GEM_DOMAIN_WC;
    if (write) {
        obj->read_domains = I915_GEM_DOMAIN_WC;
        obj->write_domain = I915_GEM_DOMAIN_WC;
    }
    i915_gem_object_unpin_pages(obj);
    return 0;
}

int32_t i915_gem_object_set_to_gtt_domain(struct drm_i915_gem_object *obj, int write)
{
    struct drm_i915_private *i915 = to_i915(obj->base.dev);
    struct i915_vma *vma;
    int32_t err = i915_gem_object_wait(obj, write, -1);

    if (err != 0) {
        return err;
    }
    if (obj->write_domain == I915_GEM_DOMAIN_GTT) {
        return 0;
    }
    err = i915_gem_object_pin_pages(obj);
    if (err != 0) {
        return err;
    }
    i915_gem_object_flush_write_domain(obj, ~I915_GEM_DOMAIN_GTT);
    if (!(obj->read_domains & I915_GEM_DOMAIN_GTT)) {
        arch_dma_fence();
        /* A backend that stands in for the aperture takes what memory now
         * holds, as the chip's aperture reads it. */
        for (vma = obj->vmas; vma; vma = vma->obj_next) {
            if (vma->mappable) {
                i915_aperture_sync(&i915->hw, vma->start, vma->size, 0);
            }
        }
    }
    obj->read_domains |= I915_GEM_DOMAIN_GTT;
    if (write) {
        obj->read_domains = I915_GEM_DOMAIN_GTT;
        obj->write_domain = I915_GEM_DOMAIN_GTT;
        for (vma = obj->vmas; vma; vma = vma->obj_next) {
            vma->ggtt_write = 1;
        }
    }
    i915_gem_object_unpin_pages(obj);
    return 0;
}

int32_t i915_gem_object_set_to_cpu_domain(struct drm_i915_gem_object *obj, int write)
{
    int32_t err = i915_gem_object_wait(obj, write, -1);

    if (err != 0) {
        return err;
    }
    i915_gem_object_flush_write_domain(obj, ~I915_GEM_DOMAIN_CPU);
    if (!(obj->read_domains & I915_GEM_DOMAIN_CPU)) {
        i915_gem_clflush_object(obj, 0);
        obj->read_domains |= I915_GEM_DOMAIN_CPU;
    }
    if (write) {
        start_cpu_write(obj);
    }
    return 0;
}

int32_t i915_gem_object_set_cache_level(struct drm_i915_gem_object *obj, int level)
{
    int32_t err;

    if (obj->cache_level == level) {
        return 0;
    }
    err = i915_gem_object_wait(obj, 1, -1);
    if (err != 0) {
        return err;
    }
    i915_gem_object_set_cache_coherency(obj, level);
    obj->cache_dirty = 1;
    /* The entries hold the level: the next binding writes the new one. */
    return i915_gem_object_unbind(obj, 1);
}

int32_t i915_gem_object_prepare_read(struct drm_i915_gem_object *obj, uint32_t *needs_clflush)
{
    int32_t err;

    *needs_clflush = 0;
    if (obj->base.import) {
        return -ENODEV;
    }
    err = i915_gem_object_wait(obj, 0, -1);
    if (err == 0) {
        err = i915_gem_object_pin_pages(obj);
    }
    if (err != 0) {
        return err;
    }
    if (obj->cache_coherent & I915_BO_CACHE_COHERENT_FOR_READ) {
        err = i915_gem_object_set_to_cpu_domain(obj, 0);
        if (err != 0) {
            i915_gem_object_unpin_pages(obj);
        }
        return err;
    }
    i915_gem_object_flush_write_domain(obj, ~I915_GEM_DOMAIN_CPU);
    if (!obj->cache_dirty && !(obj->read_domains & I915_GEM_DOMAIN_CPU)) {
        *needs_clflush = I915_CLFLUSH_BEFORE;
    }
    return 0;
}

int32_t i915_gem_object_prepare_write(struct drm_i915_gem_object *obj, uint32_t *needs_clflush)
{
    int32_t err;

    *needs_clflush = 0;
    if (obj->base.import) {
        return -ENODEV;
    }
    err = i915_gem_object_wait(obj, 1, -1);
    if (err == 0) {
        err = i915_gem_object_pin_pages(obj);
    }
    if (err != 0) {
        return err;
    }
    if (obj->cache_coherent & I915_BO_CACHE_COHERENT_FOR_WRITE) {
        err = i915_gem_object_set_to_cpu_domain(obj, 1);
        if (err != 0) {
            i915_gem_object_unpin_pages(obj);
        }
        return err;
    }
    i915_gem_object_flush_write_domain(obj, ~I915_GEM_DOMAIN_CPU);
    if (!obj->cache_dirty) {
        *needs_clflush |= I915_CLFLUSH_AFTER;
        if (!(obj->read_domains & I915_GEM_DOMAIN_CPU)) {
            *needs_clflush |= I915_CLFLUSH_BEFORE;
        }
    }
    return 0;
}

/* ------------------------------------------------------------------------
 * PREAD and PWRITE
 */

/** Read through the processor's view of the pages, as Linux's
 * i915_gem_shmem_pread: -ENODEV for an import, which has none. */
static int32_t shmem_pread(struct drm_i915_gem_object *obj, const struct drm_i915_gem_pread *args)
{
    uint8_t __user *user = (uint8_t __user *)(uintptr_t)args->data_ptr;
    uint64_t remain = args->size, at = args->offset;
    uint32_t needs_clflush;
    int32_t err = i915_gem_object_prepare_read(obj, &needs_clflush);

    if (err != 0) {
        return err;
    }
    while (remain && err == 0) {
        uint32_t off = (uint32_t)(at & (PAGE_SIZE - 1));
        uint32_t len = remain < PAGE_SIZE - off ? (uint32_t)remain : PAGE_SIZE - off;
        uint8_t *va = (uint8_t *)kmap_temp(obj->frames[at >> PAGE_SHIFT]);

        if (!va) {
            err = -ENOMEM;
            break;
        }
        if (needs_clflush) {
            arch_dma_flush(va + off, len);
        }
        if (copy_to_user(user, va + off, len) != 0) {
            err = -EFAULT;
        }
        kunmap_temp(va);
        remain -= len;
        user += len;
        at += len;
    }
    i915_gem_object_unpin_pages(obj);
    return err;
}

/** Write through the processor's view, as Linux's i915_gem_shmem_pwrite. */
static int32_t shmem_pwrite(struct drm_i915_gem_object *obj, const struct drm_i915_gem_pwrite *args)
{
    const uint8_t __user *user = (const uint8_t __user *)(uintptr_t)args->data_ptr;
    uint64_t remain = args->size, at = args->offset;
    uint32_t needs_clflush, partial = 0;
    int32_t err = i915_gem_object_prepare_write(obj, &needs_clflush);

    if (err != 0) {
        return err;
    }
    if (needs_clflush & I915_CLFLUSH_BEFORE) {
        partial = 63u; /* the line, as x86's clflush size */
    }
    while (remain && err == 0) {
        uint32_t off = (uint32_t)(at & (PAGE_SIZE - 1));
        uint32_t len = remain < PAGE_SIZE - off ? (uint32_t)remain : PAGE_SIZE - off;
        uint8_t *va = (uint8_t *)kmap_temp(obj->frames[at >> PAGE_SHIFT]);

        if (!va) {
            err = -ENOMEM;
            break;
        }
        if ((off | len) & partial) {
            arch_dma_flush(va + off, len);
        }
        if (copy_from_user_left(va + off, user, len) != 0) { /* the rest stays */
            err = -EFAULT;
        } else if (needs_clflush & I915_CLFLUSH_AFTER) {
            arch_dma_flush(va + off, len);
        }
        kunmap_temp(va);
        remain -= len;
        user += len;
        at += len;
    }
    i915_gem_object_unpin_pages(obj);
    return err;
}

/**
 * Bind the object for an access through the aperture, as Linux's
 * i915_gem_gtt_prepare: moved into the GTT's domain, then the whole object
 * bound inside the aperture when it is linear and fits, into `*vma`, or a
 * page of the aperture taken for the call alone, into `*node`, through
 * which each page is reached in turn. The object's pages are pinned for
 * either; `gtt_cleanup` undoes it.
 */
static int32_t gtt_prepare(struct drm_i915_gem_object *obj, int write, struct i915_vma **vma,
                           uint64_t *node)
{
    struct drm_i915_private *i915 = to_i915(obj->base.dev);
    int32_t err = i915_gem_object_set_to_gtt_domain(obj, write);

    *vma = 0;
    if (err != 0) {
        return err;
    }
    if (obj->tiling == I915_TILING_NONE &&
        i915_vma_pin(obj, 0, 0, I915_PIN_MAPPABLE | I915_PIN_NOEVICT, 0, vma) == 0) {
        err = 0;
    } else {
        *vma = 0;
        /* As Linux's insert_mappable_node, which evicts nothing. */
        err =
            i915_ggtt_reserve(i915, I915_GTT_PAGE_SIZE, I915_PIN_MAPPABLE | I915_PIN_NOEVICT, node);
        if (err != 0) {
            return err;
        }
    }
    err = i915_gem_object_pin_pages(obj);
    if (err != 0) {
        if (*vma) {
            i915_vma_unpin(*vma);
        } else {
            (void)i915_ggtt_clear_range(i915, *node, I915_GTT_PAGE_SIZE, 0);
            i915_ggtt_release(i915, *node);
        }
    }
    return err;
}

/** Undo `gtt_prepare`. */
static void gtt_cleanup(struct drm_i915_gem_object *obj, struct i915_vma *vma, uint64_t node)
{
    struct drm_i915_private *i915 = to_i915(obj->base.dev);

    i915_gem_object_unpin_pages(obj);
    if (vma) {
        i915_vma_unpin(vma);
    } else {
        (void)i915_ggtt_clear_range(i915, node, I915_GTT_PAGE_SIZE, 0);
        i915_ggtt_release(i915, node);
    }
}

/**
 * Copy `len` bytes between the user's `user` and the aperture's page at
 * `base`, from `off` in it, through a window: to the aperture when `write`.
 * A backend that stands in for the aperture is synced around it.
 *
 * @return 0, -EFAULT for a bad user address, or -ENOSPC when no window
 *         could be had.
 */
static int32_t gtt_copy(struct drm_i915_private *i915, uint64_t base, uint32_t off,
                        uint8_t __user *user, uint32_t len, int write)
{
    uint8_t *va = (uint8_t *)i915_aperture_kmap(&i915->hw, &i915->gem.aperture, base);
    int fault;

    if (!va) {
        return -ENOSPC;
    }
    if (write) {
        fault = copy_from_user_left(va + off, user, len) != 0; /* the rest stays */
        arch_dma_fence();
        i915_aperture_sync(&i915->hw, base, I915_GTT_PAGE_SIZE, 1);
    } else {
        i915_aperture_sync(&i915->hw, base, I915_GTT_PAGE_SIZE, 0);
        fault = copy_to_user(user, va + off, len) != 0;
    }
    i915_aperture_kunmap(&i915->hw, &i915->gem.aperture, va);
    return fault ? -EFAULT : 0;
}

/**
 * Read or write `size` bytes at `offset` through the aperture, as Linux's
 * i915_gem_gtt_pread and i915_gem_gtt_pwrite_fast: through the object's
 * binding, or a page at a time through a page of the aperture taken for
 * the call, whose entry maps each of the object's pages uncached in turn.
 */
static int32_t gtt_rw(struct drm_i915_gem_object *obj, uint64_t offset, uint64_t size,
                      uint64_t data_ptr, int write)
{
    struct drm_i915_private *i915 = to_i915(obj->base.dev);
    uint8_t __user *user = (uint8_t __user *)(uintptr_t)data_ptr;
    struct i915_vma *vma;
    uint64_t node = 0, remain = size, at = offset;
    int32_t err = gtt_prepare(obj, write, &vma, &node);

    if (err != 0) {
        return err;
    }
    while (remain && err == 0) {
        uint32_t off = (uint32_t)(at & (PAGE_SIZE - 1));
        uint32_t len = remain < PAGE_SIZE - off ? (uint32_t)remain : PAGE_SIZE - off;
        uint64_t base;

        if (vma) {
            base = vma->start + (at & ~(uint64_t)(PAGE_SIZE - 1));
        } else {
            uint64_t page = obj->frames[at >> PAGE_SHIFT];

            base = node;
            err = i915_ggtt_insert(i915, node, &page, 1, I915_CACHE_NONE);
            if (err != 0) {
                break;
            }
        }
        err = gtt_copy(i915, base, off, user, len, write);
        remain -= len;
        user += len;
        at += len;
    }
    gtt_cleanup(obj, vma, node);
    return err;
}

/**
 * GEM_PREAD, as Linux's i915_gem_pread_ioctl: nothing for 0 bytes; EFAULT
 * for a buffer outside user space, ENOENT for no such object, EINVAL past
 * its end or for a userptr object; the object's writes waited for, then
 * read through the processor's view, or through the aperture for an
 * import. May sleep.
 */
int32_t i915_gem_pread_ioctl(struct drm_device *dev, void *data, struct drm_file *file)
{
    struct drm_i915_gem_pread *args = (struct drm_i915_gem_pread *)data;
    struct drm_i915_gem_object *obj;
    int32_t err;

    (void)dev;
    if (args->size == 0) {
        return 0;
    }
    if (args->data_ptr + args->size < args->data_ptr || args->data_ptr > (uintptr_t)-1 ||
        args->size > (uint64_t)(size_t)-1 ||
        !user_range_ok((uintptr_t)args->data_ptr, (size_t)args->size)) {
        return -EFAULT;
    }
    obj = i915_gem_object_lookup(file, args->handle);
    if (!obj) {
        return -ENOENT;
    }
    if (args->offset > obj->base.size || args->size > obj->base.size - args->offset) {
        err = -EINVAL;
    } else if (i915_gem_object_is_userptr(obj)) {
        /* As Linux's i915_gem_userptr_pread. */
        err = -EINVAL;
    } else {
        err = i915_gem_object_wait(obj, 0, -1);
        if (err == 0) {
            err = shmem_pread(obj, args);
            if (err == -EFAULT || err == -ENODEV) {
                err = gtt_rw(obj, args->offset, args->size, args->data_ptr, 0);
            }
        }
    }
    i915_gem_object_put(obj);
    return err;
}

/**
 * GEM_PWRITE, as Linux's i915_gem_pwrite_ioctl: its refusals are PREAD's;
 * every fence waited for, then written through the aperture when the
 * object has no view of the processor's or its caching would need the
 * lines flushed, and otherwise, or when the aperture has no room or the
 * user's buffer faults there, through the processor's view. May sleep.
 */
int32_t i915_gem_pwrite_ioctl(struct drm_device *dev, void *data, struct drm_file *file)
{
    struct drm_i915_gem_pwrite *args = (struct drm_i915_gem_pwrite *)data;
    struct drm_i915_gem_object *obj;
    int32_t err;

    (void)dev;
    if (args->size == 0) {
        return 0;
    }
    if (args->data_ptr + args->size < args->data_ptr || args->data_ptr > (uintptr_t)-1 ||
        args->size > (uint64_t)(size_t)-1 ||
        !user_range_ok((uintptr_t)args->data_ptr, (size_t)args->size)) {
        return -EFAULT;
    }
    obj = i915_gem_object_lookup(file, args->handle);
    if (!obj) {
        return -ENOENT;
    }
    if (args->offset > obj->base.size || args->size > obj->base.size - args->offset) {
        err = -EINVAL;
    } else if (i915_gem_object_is_userptr(obj)) {
        err = -EINVAL;
    } else {
        err = i915_gem_object_wait(obj, 1, -1);
        if (err == 0) {
            err = -EFAULT;
            if (obj->base.import || i915_gem_cpu_write_needs_clflush(obj)) {
                err = gtt_rw(obj, args->offset, args->size, args->data_ptr, 1);
            }
            if ((err == -EFAULT || err == -ENOSPC) && !obj->base.import) {
                err = shmem_pwrite(obj, args);
            }
        }
    }
    i915_gem_object_put(obj);
    return err;
}

/* ------------------------------------------------------------------------
 * CREATE, SET_DOMAIN, SW_FINISH, SET_CACHING and GET_CACHING
 */

/** GEM_CREATE, as Linux's i915_gem_create_ioctl: an object of `size`
 * bytes rounded up to pages, written back, with a handle; EINVAL for 0,
 * E2BIG past what an object holds, ENOMEM. */
int32_t i915_gem_create_ioctl(struct drm_device *dev, void *data, struct drm_file *file)
{
    struct drm_i915_gem_create *args = (struct drm_i915_gem_create *)data;
    struct drm_i915_gem_object *obj;
    int32_t err = i915_gem_object_create(to_i915(dev), args->size, &obj);

    if (err != 0) {
        return err;
    }
    err = drm_gem_handle_create(file, &obj->base, &args->handle);
    if (err == 0) {
        args->size = obj->base.size;
    }
    i915_gem_object_put(obj);
    return err;
}

#define I915_USER_PTR(v) ((void __user *)(uintptr_t)(v))

/* The most regions MEMORY_REGIONS may name, Linux's INTEL_REGION_UNKNOWN. */
#define I915_GEM_MAX_REGIONS 7u

/* What GEM_CREATE_EXT's extensions set: whether MEMORY_REGIONS came. */
struct gem_create_ext {
    struct drm_i915_private *i915;
    uint32_t n_placements;
};

/**
 * GEM_CREATE_EXT's MEMORY_REGIONS, as Linux's set_placements: Haswell has
 * one region a program may name, system memory, class 0 instance 0, since
 * Linux keeps its stolen memory's region private. EINVAL for a pad not 0,
 * no regions, more than Linux's seven, a region the device lacks, one
 * named twice, or a second MEMORY_REGIONS in the chain; EFAULT.
 */
static int32_t ext_set_placements(uintptr_t ext, void *data)
{
    struct gem_create_ext *ext_data = (struct gem_create_ext *)data;
    struct drm_i915_gem_create_ext_memory_regions args;
    struct drm_i915_gem_memory_class_instance region;
    uint32_t i, mask = 0;

    if (copy_from_user(&args, I915_USER_PTR(ext), sizeof(args)) != 0) {
        return -EFAULT;
    }
    if (args.pad || !args.num_regions || args.num_regions > I915_GEM_MAX_REGIONS) {
        return -EINVAL;
    }
    for (i = 0; i < args.num_regions; i++) {
        if (copy_from_user(&region, I915_USER_PTR(args.regions + i * sizeof(region)),
                           sizeof(region)) != 0) {
            return -EFAULT;
        }
        if (region.memory_class != I915_MEMORY_CLASS_SYSTEM || region.memory_instance != 0 ||
            (mask & 1u)) {
            return -EINVAL;
        }
        mask |= 1u;
    }
    if (ext_data->n_placements) {
        return -EINVAL;
    }
    ext_data->n_placements = args.num_regions;
    return 0;
}

/** PROTECTED_CONTENT, as Linux's ext_set_protected: EINVAL for flags,
 * else ENODEV, since Haswell has no PXP. */
static int32_t ext_set_protected(uintptr_t ext, void *data)
{
    uint32_t flags;

    (void)data;
    if (copy_from_user(&flags, I915_USER_PTR(ext + sizeof(struct i915_user_extension)),
                       sizeof(flags)) != 0) {
        return -EFAULT;
    }
    return flags ? -EINVAL : -ENODEV;
}

/** SET_PAT, as Linux's ext_set_pat: ENODEV before graphics 12.70. */
static int32_t ext_set_pat(uintptr_t ext, void *data)
{
    (void)ext;
    (void)data;
    return -ENODEV;
}

static const i915_ext_fn gem_create_extensions[3] = {ext_set_placements, ext_set_protected,
                                                     ext_set_pat};

/**
 * GEM_CREATE_EXT, as Linux's i915_gem_create_ext_ioctl: GEM_CREATE's
 * object, after its extensions. EINVAL for a flag but NEEDS_CPU_ACCESS,
 * and for that one too, which asks for a second region to spill to, and
 * Haswell has one; the extensions' errors; then GEM_CREATE's.
 */
int32_t i915_gem_create_ext_ioctl(struct drm_device *dev, void *data, struct drm_file *file)
{
    struct drm_i915_gem_create_ext *args = (struct drm_i915_gem_create_ext *)data;
    struct gem_create_ext ext_data;
    struct drm_i915_gem_object *obj;
    int32_t err;

    if (args->flags & ~I915_GEM_CREATE_EXT_FLAG_NEEDS_CPU_ACCESS) {
        return -EINVAL;
    }
    ext_data.i915 = to_i915(dev);
    ext_data.n_placements = 0;
    err = i915_user_extensions(args->extensions, gem_create_extensions, 3u, &ext_data);
    if (err != 0) {
        return err;
    }
    if (args->flags & I915_GEM_CREATE_EXT_FLAG_NEEDS_CPU_ACCESS) {
        return -EINVAL;
    }
    err = i915_gem_object_create(to_i915(dev), args->size, &obj);
    if (err != 0) {
        return err;
    }
    err = drm_gem_handle_create(file, &obj->base, &args->handle);
    if (err == 0) {
        args->size = obj->base.size;
    }
    i915_gem_object_put(obj);
    return err;
}

/* The widest stride a linear framebuffer of Haswell's primary plane
 * takes, which a dumb buffer's pitch past it is rounded to pages for, as
 * Linux's intel_plane_fb_max_stride gives it. */
#define I915_DUMB_MAX_STRIDE (32u * 1024u)

int32_t i915_gem_dumb_create(struct drm_file *file, struct drm_device *dev,
                             struct drm_mode_create_dumb *args)
{
    struct drm_i915_gem_object *obj;
    uint32_t cpp = (args->bpp + 7u) / 8u;
    uint64_t pitch;
    int32_t err;

    if (cpp != 1 && cpp != 2 && cpp != 4) {
        return -EINVAL;
    }
    pitch = ((uint64_t)args->width * cpp + 63u) & ~(uint64_t)63u;
    if (pitch > I915_DUMB_MAX_STRIDE) {
        pitch = (pitch + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1);
    }
    if (pitch > 0xffffffffu || pitch < args->width) {
        return -EINVAL;
    }
    err = i915_gem_object_create(to_i915(dev), pitch * args->height, &obj);
    if (err != 0) {
        return err;
    }
    obj->dumb = 1;
    err = drm_gem_handle_create(file, &obj->base, &args->handle);
    if (err == 0) {
        args->pitch = (uint32_t)pitch;
        args->size = obj->base.size;
    }
    i915_gem_object_put(obj);
    return err;
}

int32_t i915_gem_object_pin_to_display(struct drm_i915_gem_object *obj,
                                       const struct i915_remap_info *remap, uint32_t align,
                                       struct i915_vma **vma)
{
    int32_t err;

    /* The display engine does not snoop the LLC, so it reads memory. */
    err = i915_gem_object_set_cache_level(obj, I915_CACHE_NONE);
    if (err != 0) {
        return err;
    }
    /* A plane of a chip without a GMCH reads anywhere in the GTT, so the
     * aperture is only tried first, without a wait, for a normal view, as
     * Linux's i915_gem_object_pin_to_display_plane. */
    if (remap) {
        err = i915_vma_pin_remapped(obj, remap, 0, align, vma);
    } else {
        err = i915_vma_pin(obj, 0, 0, I915_PIN_MAPPABLE | I915_PIN_NONBLOCK, align, vma);
        if (err != 0) {
            err = i915_vma_pin(obj, 0, 0, 0, align, vma);
        }
    }
    if (err != 0) {
        return err;
    }
    if (obj->cache_dirty) {
        i915_gem_clflush_object(obj, 1);
    }
    obj->read_domains |= I915_GEM_DOMAIN_GTT;
    return 0;
}

void i915_gem_object_flush_range(struct drm_i915_gem_object *obj, uint64_t start, uint64_t end)
{
    uint64_t at = start & ~(uint64_t)(PAGE_SIZE - 1);

    /* Forced, whatever the caching, as Linux's flush for display: the
     * display engine does not snoop. */
    if (!obj->frames) {
        return;
    }
    if (end > obj->base.size) {
        end = obj->base.size;
    }
    for (; at < end; at += PAGE_SIZE) {
        uint64_t from = at < start ? start : at;
        uint64_t to = at + PAGE_SIZE < end ? at + PAGE_SIZE : end;
        uint8_t *va = (uint8_t *)kmap_temp(obj->frames[at >> PAGE_SHIFT]);

        if (va) {
            arch_dma_flush(va + (from - at), (uint32_t)(to - from));
            kunmap_temp(va);
        }
    }
}

/**
 * GEM_SET_DOMAIN, as Linux's i915_gem_set_domain_ioctl: EINVAL for a GPU
 * domain or a write domain other than the read domains, nothing for no
 * read domain, ENOENT for no such object; otherwise the object's work
 * waited for and
 * the object moved into the WC, GTT or CPU domain, the first of those the
 * read domains name; for a userptr object, its pages pinned and its work
 * waited for, EFAULT when its memory is gone. May sleep.
 */
int32_t i915_gem_set_domain_ioctl(struct drm_device *dev, void *data, struct drm_file *file)
{
    struct drm_i915_gem_set_domain *args = (struct drm_i915_gem_set_domain *)data;
    uint32_t read = args->read_domains, write = args->write_domain;
    struct drm_i915_gem_object *obj;
    int32_t err;

    (void)dev;
    if ((write | read) & I915_GEM_GPU_DOMAINS) {
        return -EINVAL;
    }
    if (write && read != write) {
        return -EINVAL;
    }
    if (!read) {
        return 0;
    }
    obj = i915_gem_object_lookup(file, args->handle);
    if (!obj) {
        return -ENOENT;
    }
    if (i915_gem_object_is_userptr(obj)) {
        /* As Linux, for a userptr object, which Mesa asks whether its
         * memory is still there: its pages pinned, and its work waited
         * for, with no move of domain. */
        err = i915_gem_object_pin_pages(obj);
        if (err == 0) {
            i915_gem_object_unpin_pages(obj);
            err = i915_gem_object_wait(obj, write != 0, -1);
        }
        i915_gem_object_put(obj);
        return err;
    }
    err = i915_gem_object_wait(obj, write != 0, -1);
    if (err == 0) {
        err = i915_gem_object_pin_pages(obj);
    }
    if (err == 0) {
        if (obj->write_domain != read) {
            if (read & I915_GEM_DOMAIN_WC) {
                err = i915_gem_object_set_to_wc_domain(obj, write != 0);
            } else if (read & I915_GEM_DOMAIN_GTT) {
                err = i915_gem_object_set_to_gtt_domain(obj, write != 0);
            } else {
                err = i915_gem_object_set_to_cpu_domain(obj, write != 0);
            }
        }
        i915_gem_object_unpin_pages(obj);
    }
    i915_gem_object_put(obj);
    return err;
}

/** GEM_SW_FINISH: ENOENT for no such object; nothing else until objects
 * are scanned out, whose caches it flushes then. */
int32_t i915_gem_sw_finish_ioctl(struct drm_device *dev, void *data, struct drm_file *file)
{
    struct drm_i915_gem_sw_finish *args = (struct drm_i915_gem_sw_finish *)data;
    struct drm_i915_gem_object *obj = i915_gem_object_lookup(file, args->handle);

    (void)dev;
    if (!obj) {
        return -ENOENT;
    }
    i915_gem_object_put(obj);
    return 0;
}

/** GEM_GET_CACHING: CACHED at the LLC levels, DISPLAY at write-through and
 * NONE uncached; ENOENT for no such object. */
int32_t i915_gem_get_caching_ioctl(struct drm_device *dev, void *data, struct drm_file *file)
{
    struct drm_i915_gem_caching *args = (struct drm_i915_gem_caching *)data;
    struct drm_i915_gem_object *obj = i915_gem_object_lookup(file, args->handle);

    (void)dev;
    if (!obj) {
        return -ENOENT;
    }
    if (obj->cache_level == I915_CACHE_LLC || obj->cache_level == I915_CACHE_L3_LLC) {
        args->caching = I915_CACHING_CACHED;
    } else if (obj->cache_level == I915_CACHE_WT) {
        args->caching = I915_CACHING_DISPLAY;
    } else {
        args->caching = I915_CACHING_NONE;
    }
    i915_gem_object_put(obj);
    return 0;
}

/**
 * GEM_SET_CACHING, as Linux's i915_gem_set_caching_ioctl on an LLC part:
 * NONE uncached, CACHED in the LLC, DISPLAY write-through where the chip
 * has it, which Haswell's GT2 without eDRAM has not, so uncached; EINVAL
 * for another value, ENOENT for no such object, ENXIO for a userptr object
 * but for CACHED, which it is. The object's work is waited for and its
 * bindings undone, so that the next takes the new level. May sleep.
 */
int32_t i915_gem_set_caching_ioctl(struct drm_device *dev, void *data, struct drm_file *file)
{
    struct drm_i915_gem_caching *args = (struct drm_i915_gem_caching *)data;
    struct drm_i915_private *i915 = to_i915(dev);
    struct drm_i915_gem_object *obj;
    int level;
    int32_t err;

    switch (args->caching) {
    case I915_CACHING_NONE:
        level = I915_CACHE_NONE;
        break;
    case I915_CACHING_CACHED:
        if (!i915->id->platform->has_llc) {
            return -ENODEV;
        }
        level = I915_CACHE_LLC;
        break;
    case I915_CACHING_DISPLAY:
        level = I915_CACHE_NONE;
        break;
    default:
        return -EINVAL;
    }
    obj = i915_gem_object_lookup(file, args->handle);
    if (!obj) {
        return -ENOENT;
    }
    if (i915_gem_object_is_userptr(obj)) {
        /* As Linux: a userptr object stays cached, which may be asked. */
        err = args->caching == I915_CACHING_CACHED ? 0 : -ENXIO;
    } else {
        err = i915_gem_object_set_cache_level(obj, level);
    }
    i915_gem_object_put(obj);
    return err;
}

/* ------------------------------------------------------------------------
 * BUSY and WAIT
 */

/** BUSY's bit for a reading engine of class `id`: bit 16 + `id`, or the
 * whole high half for a fence of no engine. */
static uint32_t busy_read_flag(uint16_t id)
{
    return id == I915_ENGINE_CLASS_INVALID ? 0xffff0000u : 0x10000u << id;
}

/** BUSY's value for a writing engine of class `id`: one more than the
 * class, and its reading bit, as the interface promises a writer reads. */
static uint32_t busy_write_id(uint16_t id)
{
    return id == I915_ENGINE_CLASS_INVALID ? 0xffffffffu : (uint32_t)(id + 1) | busy_read_flag(id);
}

/** What `fence` adds to BUSY: for each of its leaves that is the driver's
 * and has not signalled, `flag` of its engine's class. Another driver's
 * fence adds nothing, as on Linux. */
static uint32_t busy_check(struct dma_fence *fence, uint32_t (*flag)(uint16_t))
{
    struct dma_fence_unwrap u;
    struct dma_fence *leaf;
    uint32_t busy = 0;

    for (leaf = dma_fence_unwrap_first(fence, &u); leaf; leaf = dma_fence_unwrap_next(&u)) {
        if (leaf->ops == &i915_fence_ops && !dma_fence_is_signaled(leaf)) {
            busy |= flag(((struct i915_fence *)leaf)->uabi_class);
        }
    }
    return busy;
}

/** GEM_BUSY, as Linux's i915_gem_busy_ioctl: the engines that still read
 * and write the object by its reservation's fences; ENOENT for no such
 * object. */
int32_t i915_gem_busy_ioctl(struct drm_device *dev, void *data, struct drm_file *file)
{
    struct drm_i915_gem_busy *args = (struct drm_i915_gem_busy *)data;
    struct drm_i915_gem_object *obj = i915_gem_object_lookup(file, args->handle);
    struct dma_fence **fences;
    uint32_t i, n;
    int32_t err;

    (void)dev;
    if (!obj) {
        return -ENOENT;
    }
    /* A snapshot under the reservation's lock, where Linux's iterator reads
     * without one and restarts (i915_gem_busy.c:145-157). */
    err = dma_resv_get_fences(obj->base.resv, 1, &fences, &n);
    i915_gem_object_put(obj);
    if (err < 0) {
        return err;
    }
    args->busy = busy_check(fences[0], busy_write_id);
    for (i = 1; i < n; i++) {
        args->busy |= busy_check(fences[i], busy_read_flag);
    }
    dma_resv_put_fences(fences, n);
    return 0;
}

/**
 * GEM_WAIT, as Linux's i915_gem_wait_ioctl: wait for every fence of the
 * object for `timeout_ns`, for ever when it is negative, and write back
 * the time left. EINVAL for flags, ENOENT for no such object, ETIME while
 * it is busy at the timeout, EINTR for a signal. May sleep.
 */
int32_t i915_gem_wait_ioctl(struct drm_device *dev, void *data, struct drm_file *file)
{
    struct drm_i915_gem_wait *args = (struct drm_i915_gem_wait *)data;
    struct drm_i915_gem_object *obj;
    int64_t start;
    int32_t err;

    (void)dev;
    if (args->flags != 0) {
        return -EINVAL;
    }
    obj = i915_gem_object_lookup(file, args->bo_handle);
    if (!obj) {
        return -ENOENT;
    }
    start = dma_fence_now();
    err = i915_gem_object_wait(obj, 1, args->timeout_ns);
    if (args->timeout_ns > 0) {
        args->timeout_ns -= dma_fence_now() - start;
        if (args->timeout_ns < 0) {
            args->timeout_ns = 0;
        }
        /* A wait that ended before its time had passed, as Linux's
         * jiffies may end it, asks to be restarted. */
        if (err == -ETIME && args->timeout_ns) {
            err = -EAGAIN;
        }
    }
    i915_gem_object_put(obj);
    return err;
}

/* ------------------------------------------------------------------------
 * GET_APERTURE and MADVISE
 */

/**
 * GEM_GET_APERTURE, as Linux's i915_gem_get_aperture_ioctl: the global
 * GTT's bytes, and those left once the pinned bindings, the ranges the
 * kernel reserved, the firmware's kept surfaces and the GT's own objects
 * are taken away, which Linux counts as pinned or reserved too.
 */
int32_t i915_gem_get_aperture_ioctl(struct drm_device *dev, void *data, struct drm_file *file)
{
    struct drm_i915_gem_get_aperture *args = (struct drm_i915_gem_get_aperture *)data;
    struct drm_i915_private *i915 = to_i915(dev);
    struct i915_vma *v;
    uint64_t pinned = i915->ggtt.total - i915->gt.ggtt_bottom;
    int i;

    (void)file;
    for (i = 0; i < i915->ggtt.nkept; i++) {
        pinned += i915->ggtt.kept[i].end - i915->ggtt.kept[i].start;
    }
    for (v = i915->gem.bound; v; v = v->ggtt_next) {
        if (v->pin_count) {
            pinned += v->node_size;
        }
    }
    args->aper_size = i915->ggtt.total;
    args->aper_available_size = pinned < args->aper_size ? args->aper_size - pinned : 0;
    return 0;
}

/**
 * GEM_MADVISE, as Linux's i915_gem_madvise_ioctl: EINVAL for a state other
 * than WILLNEED and DONTNEED, ENOENT for no such object. The object takes
 * the state unless it was purged; a DONTNEED object of its own pages that
 * has none is purged at once, as Linux discards a shmem object's storage
 * then. `retained` says whether its pages were not purged.
 */
int32_t i915_gem_madvise_ioctl(struct drm_device *dev, void *data, struct drm_file *file)
{
    struct drm_i915_gem_madvise *args = (struct drm_i915_gem_madvise *)data;
    struct drm_i915_private *i915 = to_i915(dev);
    struct drm_i915_gem_object *obj;
    int purge, taken;

    if (args->madv != I915_MADV_DONTNEED && args->madv != I915_MADV_WILLNEED) {
        return -EINVAL;
    }
    obj = i915_gem_object_lookup(file, args->handle);
    if (!obj) {
        return -ENOENT;
    }
    /* Under the object's lock, its reservation's, as Linux's
     * (i915_gem.c:1049-1099), which the shrinker tries before it purges. */
    dma_resv_lock(obj->base.resv);
    if (obj->madv != __I915_MADV_PURGED) {
        obj->madv = (uint8_t)args->madv;
    }
    purge = obj->madv == I915_MADV_DONTNEED && !i915_gem_object_has_pages(obj) &&
            !obj->base.import && !i915_gem_object_is_userptr(obj);
    dma_resv_unlock(obj->base.resv);
    if (purge) {
        /* The purge changes the pages, the GEM lock's, which comes first. */
        taken = i915_gem_lock(i915);
        dma_resv_lock(obj->base.resv);
        if (obj->madv == I915_MADV_DONTNEED && !i915_gem_object_has_pages(obj)) {
            (void)i915_gem_object_truncate(obj);
        }
        dma_resv_unlock(obj->base.resv);
        i915_gem_unlock(i915, taken);
    }
    args->retained = obj->madv != __I915_MADV_PURGED;
    i915_gem_object_put(obj);
    return 0;
}

/* ------------------------------------------------------------------------
 * The device's buffers
 */

int i915_gem_lock(struct drm_i915_private *i915)
{
    if (mutex_held(&i915->gem.lock)) {
        return 0;
    }
    mutex_lock(&i915->gem.lock);
    return 1;
}

/**
 * Let go of the GEM lock, which the caller holds, after the invalidations
 * that found it held, and look once more after: an invalidation that came
 * after the flush and found it still held left its object for whoever
 * takes it next, which may be no one but this holder.
 */
static void gem_release(struct drm_i915_private *i915)
{
    i915_gem_userptr_flush(i915);
    mutex_unlock(&i915->gem.lock);
    if (i915->gem.userptr_pending && mutex_trylock(&i915->gem.lock)) {
        i915_gem_userptr_flush(i915);
        mutex_unlock(&i915->gem.lock);
    }
}

void i915_gem_unlock(struct drm_i915_private *i915, int taken)
{
    if (taken) {
        free_deferred(i915);
        gem_release(i915);
    }
}

void i915_gem_unlock_atomic(struct drm_i915_private *i915, int taken)
{
    if (taken) {
        gem_release(i915);
    }
}

int i915_gem_lock_drop(struct drm_i915_private *i915)
{
    if (!mutex_held(&i915->gem.lock)) {
        return 0;
    }
    gem_release(i915);
    return 1;
}

void i915_gem_lock_retake(struct drm_i915_private *i915, int dropped)
{
    if (dropped) {
        mutex_lock(&i915->gem.lock);
    }
}

void i915_gem_unplug(struct drm_i915_private *i915)
{
    struct drm_gem_object *base;
    int i;

    /* Every program's mapping goes, so that its next access faults to a
     * device that refuses it. The walk holds each object it is at, since
     * a last put on another processor takes one off the list. */
    for (base = drm_gem_object_next(&i915->drm, 0); base;
         base = drm_gem_object_next(&i915->drm, base)) {
        struct drm_i915_gem_object *obj = (struct drm_i915_gem_object *)base;

        if (base->funcs != &i915_gem_object_funcs) {
            continue;
        }
        for (i = 0; i < I915_MMAP_TYPES; i++) {
            paging_zap_device(&obj->mmo[i], 0, (uint32_t)(base->size >> PAGE_SHIFT));
        }
    }
}

/**
 * A row of the table below under the GEM lock: `fn##_gem` runs the row's
 * handler `fn` with the lock held, which each wait inside lets go for its
 * sleep (i915.h's Locking rule).
 */
#define I915_GEM_LOCKED(fn)                                                            \
    static int32_t fn##_gem(struct drm_device *dev, void *data, struct drm_file *file) \
    {                                                                                  \
        int taken = i915_gem_lock(to_i915(dev));                                       \
        int32_t rc = fn(dev, data, file);                                              \
                                                                                       \
        i915_gem_unlock(to_i915(dev), taken);                                          \
        return rc;                                                                     \
    }

I915_GEM_LOCKED(i915_gem_create_ioctl)
I915_GEM_LOCKED(i915_gem_create_ext_ioctl)
I915_GEM_LOCKED(i915_gem_pread_ioctl)
I915_GEM_LOCKED(i915_gem_pwrite_ioctl)
I915_GEM_LOCKED(i915_gem_mmap_ioctl)
I915_GEM_LOCKED(i915_gem_mmap_offset_ioctl)
I915_GEM_LOCKED(i915_gem_set_domain_ioctl)
I915_GEM_LOCKED(i915_gem_sw_finish_ioctl)
I915_GEM_LOCKED(i915_gem_set_tiling_ioctl)
I915_GEM_LOCKED(i915_gem_get_tiling_ioctl)
I915_GEM_LOCKED(i915_gem_wait_ioctl)
I915_GEM_LOCKED(i915_gem_set_caching_ioctl)
I915_GEM_LOCKED(i915_gem_get_caching_ioctl)
I915_GEM_LOCKED(i915_gem_get_aperture_ioctl)
I915_GEM_LOCKED(i915_gem_userptr_ioctl)

/* All rows run without the big lock, as Linux's i915 table. The buffer
 * wrappers hold the GEM mutex; EXECBUFFER2 takes it between its copies.
 * BUSY uses the handle and reservation locks, MADVISE the reservation and
 * GEM locks, contexts their file's mutex and the GEM mutex for their state,
 * and THROTTLE the context mutex and engine locks. REG_READ has forcewake's
 * uncore lock; GETPARAM and QUERY initialize topology under the GEM mutex.
 * GET_PIPE_FROM_CRTC_ID holds the mode mutex. The core's KMS rows run under
 * that mutex too, without the big lock on DRIVER_KMS_UNLOCKED devices:
 * i915's display hooks use it, the GEM mutex and the event, vblank,
 * backlight and hardware locks. */
const struct drm_ioctl_desc i915_ioctls[I915_NUM_IOCTLS] = {
    {DRM_IOCTL_I915_GETPARAM, DRM_RENDER_ALLOW | DRM_UNLOCKED, i915_getparam_ioctl,
     "I915_GETPARAM"},
    {DRM_IOCTL_I915_GET_PIPE_FROM_CRTC_ID, DRM_MODESET_LOCK | DRM_UNLOCKED,
     intel_get_pipe_from_crtc_id_ioctl, "I915_GET_PIPE_FROM_CRTC_ID"},
    {DRM_IOCTL_I915_GEM_CONTEXT_CREATE_EXT, DRM_RENDER_ALLOW | DRM_UNLOCKED,
     i915_gem_context_create_ioctl, "I915_GEM_CONTEXT_CREATE"},
    {DRM_IOCTL_I915_GEM_CONTEXT_DESTROY, DRM_RENDER_ALLOW | DRM_UNLOCKED,
     i915_gem_context_destroy_ioctl, "I915_GEM_CONTEXT_DESTROY"},
    {DRM_IOCTL_I915_REG_READ, DRM_RENDER_ALLOW | DRM_UNLOCKED, i915_reg_read_ioctl,
     "I915_REG_READ"},
    {DRM_IOCTL_I915_GET_RESET_STATS, DRM_RENDER_ALLOW | DRM_UNLOCKED,
     i915_gem_context_reset_stats_ioctl, "I915_GET_RESET_STATS"},
    {DRM_IOCTL_I915_GEM_CONTEXT_GETPARAM, DRM_RENDER_ALLOW | DRM_UNLOCKED,
     i915_gem_context_getparam_ioctl, "I915_GEM_CONTEXT_GETPARAM"},
    {DRM_IOCTL_I915_GEM_CONTEXT_SETPARAM, DRM_RENDER_ALLOW | DRM_UNLOCKED,
     i915_gem_context_setparam_ioctl, "I915_GEM_CONTEXT_SETPARAM"},
    {DRM_IOCTL_I915_QUERY, DRM_RENDER_ALLOW | DRM_UNLOCKED, i915_query_ioctl, "I915_QUERY"},
    {DRM_IOCTL_I915_GEM_BUSY, DRM_RENDER_ALLOW | DRM_UNLOCKED, i915_gem_busy_ioctl,
     "I915_GEM_BUSY"},
    {DRM_IOCTL_I915_GEM_THROTTLE, DRM_RENDER_ALLOW | DRM_UNLOCKED, i915_gem_throttle_ioctl,
     "I915_GEM_THROTTLE"},
    {DRM_IOCTL_I915_GEM_CREATE, DRM_RENDER_ALLOW | DRM_UNLOCKED, i915_gem_create_ioctl_gem,
     "I915_GEM_CREATE"},
    {DRM_IOCTL_I915_GEM_CREATE_EXT, DRM_RENDER_ALLOW | DRM_UNLOCKED, i915_gem_create_ext_ioctl_gem,
     "I915_GEM_CREATE_EXT"},
    {DRM_IOCTL_I915_GEM_PREAD, DRM_RENDER_ALLOW | DRM_UNLOCKED, i915_gem_pread_ioctl_gem,
     "I915_GEM_PREAD"},
    {DRM_IOCTL_I915_GEM_PWRITE, DRM_RENDER_ALLOW | DRM_UNLOCKED, i915_gem_pwrite_ioctl_gem,
     "I915_GEM_PWRITE"},
    {DRM_IOCTL_I915_GEM_MMAP, DRM_RENDER_ALLOW | DRM_UNLOCKED, i915_gem_mmap_ioctl_gem,
     "I915_GEM_MMAP"},
    {DRM_IOCTL_I915_GEM_MMAP_OFFSET, DRM_RENDER_ALLOW | DRM_UNLOCKED,
     i915_gem_mmap_offset_ioctl_gem, "I915_GEM_MMAP_OFFSET"},
    {DRM_IOCTL_I915_GEM_SET_DOMAIN, DRM_RENDER_ALLOW | DRM_UNLOCKED, i915_gem_set_domain_ioctl_gem,
     "I915_GEM_SET_DOMAIN"},
    {DRM_IOCTL_I915_GEM_SW_FINISH, DRM_RENDER_ALLOW | DRM_UNLOCKED, i915_gem_sw_finish_ioctl_gem,
     "I915_GEM_SW_FINISH"},
    {DRM_IOCTL_I915_GEM_EXECBUFFER2_WR, DRM_RENDER_ALLOW | DRM_UNLOCKED, i915_gem_execbuffer2_ioctl,
     "I915_GEM_EXECBUFFER2"},
    {DRM_IOCTL_I915_GEM_SET_TILING, DRM_RENDER_ALLOW | DRM_UNLOCKED, i915_gem_set_tiling_ioctl_gem,
     "I915_GEM_SET_TILING"},
    {DRM_IOCTL_I915_GEM_GET_TILING, DRM_RENDER_ALLOW | DRM_UNLOCKED, i915_gem_get_tiling_ioctl_gem,
     "I915_GEM_GET_TILING"},
    {DRM_IOCTL_I915_GEM_WAIT, DRM_RENDER_ALLOW | DRM_UNLOCKED, i915_gem_wait_ioctl_gem,
     "I915_GEM_WAIT"},
    {DRM_IOCTL_I915_GEM_SET_CACHING, DRM_RENDER_ALLOW | DRM_UNLOCKED,
     i915_gem_set_caching_ioctl_gem, "I915_GEM_SET_CACHING"},
    {DRM_IOCTL_I915_GEM_GET_CACHING, DRM_RENDER_ALLOW | DRM_UNLOCKED,
     i915_gem_get_caching_ioctl_gem, "I915_GEM_GET_CACHING"},
    {DRM_IOCTL_I915_GEM_GET_APERTURE, DRM_RENDER_ALLOW | DRM_UNLOCKED,
     i915_gem_get_aperture_ioctl_gem, "I915_GEM_GET_APERTURE"},
    {DRM_IOCTL_I915_GEM_MADVISE, DRM_RENDER_ALLOW | DRM_UNLOCKED, i915_gem_madvise_ioctl,
     "I915_GEM_MADVISE"},
    {DRM_IOCTL_I915_GEM_USERPTR, DRM_RENDER_ALLOW | DRM_UNLOCKED, i915_gem_userptr_ioctl_gem,
     "I915_GEM_USERPTR"}};
