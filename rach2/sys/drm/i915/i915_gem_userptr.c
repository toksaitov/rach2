/*
 * i915_gem_userptr.c: buffer objects over a program's own memory, whose
 * pages are pinned while the GPU may reach them and let go when what the
 * program maps there changes, after Linux 6.12's gem/i915_gem_userptr.c.
 */

#include "i915.h"
#include "i915_drm.h"

#include "../../lib/kerrno.h"
#include "../../mm/kheap.h"
#include "../../mm/filemap.h"
#include "../../mm/mm.h"
#include "../../mm/paging.h"
#include "../../mm/pin.h"
#include "../../mm/usercopy.h"
#include "../../proc/percpu.h"
#include "../../proc/process.h"
#include "../../proc/smp.h"

/* The pins of a userptr object's pages tried before a change to the range
 * during the pin gives -EAGAIN, as Linux's notifier sequence does. */
#define USERPTR_PIN_TRIES 3

void i915_gem_userptr_put_pages(struct drm_i915_gem_object *obj)
{
    uint32_t i, n = (uint32_t)(obj->base.size >> PAGE_SHIFT);

    if (obj->frames) {
        for (i = 0; i < n; i++) {
            frame_unpin(obj->frames[i]);
            if (obj->userptr.files[i]) {
                vm_file_put(obj->userptr.files[i]);
            }
        }
        kfree(obj->frames);
        kfree(obj->userptr.files);
        obj->frames = 0;
        obj->userptr.files = 0;
    }
    obj->userptr.stale = 0;
}

/**
 * Revoke a userptr object's view of the program's pages, with the GEM lock
 * held, before the program's pages there change: an
 * unmap, a change of protection, a fork, which makes them copy-on-write, a
 * copy-on-write break, a page dropped, or the end of the address space.
 * The bindings are revoked first, as Linux's did before the pages went: an
 * unpinned one is undone, and a pinned one is pointed at the scratch page
 * until its last pin goes. A binding the GPU still uses keeps its entries,
 * and with them the frames, until the work is done, as Linux keeps the
 * pinned pages of a busy object. Then the frames are let go, or, while a
 * pin still holds them, marked stale, to go with its last. Does not sleep,
 * and its clears and unbinds give no way (`i915_ggtt_clear_range`), since
 * a notifier's caller holds the big lock between a change to the regions
 * and its change to the page tables.
 */
static void userptr_revoke(struct drm_i915_gem_object *obj)
{
    struct drm_i915_private *i915 = to_i915(obj->base.dev);
    struct i915_vma *v, *next;

    if (!obj->frames || obj->userptr.stale) {
        return;
    }
    for (v = obj->vmas; v; v = next) {
        next = v->obj_next;
        if (v->pin_count) {
            (void)i915_ggtt_clear_range(i915, v->start, v->node_size, 0);
            i915_ppgtt_clear_range(i915, v->start, v->node_size);
        } else {
            (void)i915_vma_unbind(v, 0);
        }
    }
    if (obj->pages_pin_count) {
        obj->userptr.stale = 1;
    } else {
        i915_gem_userptr_put_pages(obj);
    }
}

void i915_gem_userptr_flush(struct drm_i915_private *i915)
{
    struct drm_i915_gem_object *obj;
    uint32_t flags;

    lockdep_assert_held(&i915->gem.lock);
    for (;;) {
        flags = spin_lock_irqsave(&i915->gem.userptr_lock);
        obj = i915->gem.userptr_pending;
        if (obj) {
            i915->gem.userptr_pending = obj->userptr.pending_next;
            obj->userptr.pending_next = 0;
            obj->userptr.pending = 0;
        }
        spin_unlock_irqrestore(&i915->gem.userptr_lock, flags);
        if (!obj) {
            return;
        }
        userptr_revoke(obj);
    }
}

/** Take `obj` off the device's list of invalidations, for its free, once
 * no invalidation can come. */
static void userptr_unpend(struct drm_i915_gem_object *obj)
{
    struct drm_i915_private *i915 = to_i915(obj->base.dev);
    struct drm_i915_gem_object **link;
    uint32_t flags = spin_lock_irqsave(&i915->gem.userptr_lock);

    if (obj->userptr.pending) {
        for (link = &i915->gem.userptr_pending; *link && *link != obj;
             link = &(*link)->userptr.pending_next) {
        }
        if (*link) {
            *link = obj->userptr.pending_next;
        }
        obj->userptr.pending = 0;
        obj->userptr.pending_next = 0;
    }
    spin_unlock_irqrestore(&i915->gem.userptr_lock, flags);
}

/**
 * The range's `invalidate`, under the GEM lock when it can be had without
 * sleeping. When another holds it, the object goes on the device's list
 * for the holder, which revokes it as it lets the lock go
 * (`i915_gem_unlock`), as Linux's invalidation only marks the range
 * and leaves its pages to the next user (gem/i915_gem_userptr.c:59-65);
 * meanwhile the frames stay pinned, so no page goes to another use while
 * the GPU may still read it. Does not sleep or give way, as a notifier's
 * callback must not (mm/notify.h), so a lock it took goes back with
 * `i915_gem_unlock_atomic`, which leaves the device's deferred frees to the
 * lock's next taker.
 */
static void userptr_invalidate(struct mmn_range *r, int event, uintptr_t start, uintptr_t end)
{
    struct drm_i915_gem_object *obj = (struct drm_i915_gem_object *)r->owner;
    struct drm_i915_private *i915 = to_i915(obj->base.dev);
    uint32_t flags;

    (void)event;
    (void)start;
    (void)end;
    if (mutex_held(&i915->gem.lock)) {
        userptr_revoke(obj);
        return;
    }
    if (mutex_trylock(&i915->gem.lock)) {
        userptr_revoke(obj);
        i915_gem_unlock_atomic(i915, 1);
        return;
    }
    flags = spin_lock_irqsave(&i915->gem.userptr_lock);
    if (!obj->userptr.pending) {
        obj->userptr.pending = 1;
        obj->userptr.pending_next = i915->gem.userptr_pending;
        i915->gem.userptr_pending = obj;
    }
    spin_unlock_irqrestore(&i915->gem.userptr_lock, flags);
    /* The holder may have let go between the try and the list. */
    if (mutex_trylock(&i915->gem.lock)) {
        i915_gem_unlock_atomic(i915, 1);
    }
}

static const struct mmn_ops userptr_mmn_ops = {userptr_invalidate};

/** Drop an attempt's file holds; every element was zeroed or held. */
static void userptr_files_put(struct vm_file **files, uint32_t n)
{
    uint32_t i;

    for (i = 0; i < n; i++) {
        if (files[i]) {
            vm_file_put(files[i]);
            files[i] = NULL;
        }
    }
}

/**
 * Keep the mapped disk file of each pinned page until the GPU gives its
 * frame back. Under the regions' read lock, after the pin's sequence check,
 * so a release writes back stores made after an unmap revoked a busy pin.
 */
static void userptr_files_get(struct mm *mm, uintptr_t start, uint32_t n, struct vm_file **files)
{
    uint32_t i;

    for (i = 0; i < n; i++) {
        const vma_t *vma = vm_find(&mm->vm, start + (uintptr_t)i * PAGE_SIZE);

        if (vma && filemap_disk_pages(vma->file)) {
            vm_file_get(vma->file);
            files[i] = vma->file;
        }
    }
}

int32_t i915_gem_userptr_get_pages(struct drm_i915_gem_object *obj)
{
    uint32_t i, n = (uint32_t)(obj->base.size >> PAGE_SHIFT);
    uintptr_t start = obj->userptr.ptr, end = start + (uintptr_t)obj->base.size;
    uintptr_t *frames;
    struct vm_file **files;
    struct mm *mm;
    uint32_t seq;
    int tries, err;

    if (obj->userptr.stale) {
        /* The unpinned bindings of the old frames go once the GPU is done
         * with them, and with the last pin the frames; a pinned binding
         * keeps them. */
        err = i915_gem_object_wait_idle(obj, 1);
        if (err != 0) {
            return err;
        }
        (void)i915_gem_object_unbind(obj, 1);
        if (obj->userptr.stale) {
            return -EAGAIN;
        }
    }
    if (!obj->userptr.range.mm || !current_proc || current_proc->mm != obj->userptr.range.mm) {
        return -EFAULT;
    }
    mm = obj->userptr.range.mm;
    frames = (uintptr_t *)kmalloc((size_t)n * sizeof(*frames));
    if (!frames) {
        return -ENOMEM;
    }
    files = kzalloc((size_t)n * sizeof(*files));
    if (!files) {
        kfree(frames);
        return -ENOMEM;
    }
    for (tries = 0; tries < USERPTR_PIN_TRIES; tries++) {
        seq = obj->userptr.range.seq;
        mmap_read_lock(mm);
        err = paging_pin_range(mm, start, end, PIN_WRITE, frames);
        if (err == 0 && seq == obj->userptr.range.seq) {
            userptr_files_get(mm, start, n, files);
        }
        mmap_unlock(mm);
        if (err != 0) {
            kfree(files);
            kfree(frames);
            return err;
        }
        if (seq == obj->userptr.range.seq && obj->frames) {
            /* Another caller pinned the range while this one slept: its
             * frames serve, unless a change has made them stale since. */
            for (i = 0; i < n; i++) {
                frame_unpin(frames[i]);
            }
            userptr_files_put(files, n);
            kfree(files);
            kfree(frames);
            if (obj->userptr.stale) {
                return -EAGAIN;
            }
            obj->pages_pin_count++;
            return 0;
        }
        if (seq == obj->userptr.range.seq) {
            obj->frames = frames;
            obj->userptr.files = files;
            obj->pages_pin_count = 1;
            return 0;
        }
        userptr_files_put(files, n);
        /* The range changed while the pin slept: its frames may be old. */
        for (i = 0; i < n; i++) {
            frame_unpin(frames[i]);
        }
        if (!obj->userptr.range.mm) {
            break;
        }
    }
    kfree(files);
    kfree(frames);
    return obj->userptr.range.mm ? -EAGAIN : -EFAULT;
}

/* The notifier's list is kept under the big lock (mm/notify.h), which a
 * free on a processor without it takes for the unregister. */
void i915_gem_userptr_release(struct drm_i915_gem_object *obj)
{
    int big = !bkl_held();

    if (big) {
        bkl_enter();
    }
    mmn_unregister(&obj->userptr.range);
    if (big) {
        bkl_exit();
    }
    userptr_unpend(obj);
    i915_gem_userptr_put_pages(obj);
}

/**
 * Whether every page of [addr, addr + len) of `mm` lies in a region of
 * memory with pages, none of a device's, as Linux's probe_range: 0, or
 * -EFAULT for a hole or a device's region. The caller holds `mm`'s
 * regions' lock to read.
 */
static int32_t probe_range(struct mm *mm, uintptr_t addr, uintptr_t len)
{
    uintptr_t end = addr + len;
    const vma_t *r = vm_find(&mm->vm, addr);

    while (r && addr < end) {
        if (r->start > addr || vm_device_object(r->file)) {
            return -EFAULT;
        }
        addr = r->end;
        r = vm_next(r);
    }
    return addr < end ? -EFAULT : 0;
}

/**
 * GEM_USERPTR, as Linux's i915_gem_userptr_ioctl: an object over the
 * caller's memory, cached in the LLC, with a handle. ENODEV on a part
 * without an LLC; EINVAL for an unknown flag, no bytes, or a pointer or
 * size not of whole pages; E2BIG for a size past what an object holds;
 * EFAULT for a range that ends past user space, Linux's access_ok, which
 * passes a pointer of 0 that the pin at the object's first use refuses, or
 * with PROBE one not wholly in memory with pages; ENODEV for
 * UNSYNCHRONIZED, which Linux 6.12 refuses to everyone, and for READ_ONLY,
 * since Haswell's GTT has no read-only entries (Linux's has_read_only);
 * ENOMEM. Its pages are pinned when it is
 * first used, by the address space that made it alone. PRIME does not
 * export it, as Linux's i915_gem_userptr_dmabuf_export.
 */
int32_t i915_gem_userptr_ioctl(struct drm_device *dev, void *data, struct drm_file *file)
{
    struct drm_i915_gem_userptr *args = (struct drm_i915_gem_userptr *)data;
    struct drm_i915_private *i915 = to_i915(dev);
    struct drm_i915_gem_object *obj;
    struct mm *mm = current_proc ? current_proc->mm : 0;
    int32_t err;

    if (!i915->id->platform->has_llc) {
        return -ENODEV;
    }
    if (args->flags &
        ~(I915_USERPTR_READ_ONLY | I915_USERPTR_UNSYNCHRONIZED | I915_USERPTR_PROBE)) {
        return -EINVAL;
    }
    if (args->user_size > SHMEM_MAX_SIZE || args->user_size > (uint64_t)(size_t)-1) {
        return -E2BIG;
    }
    if (!args->user_size) {
        return -EINVAL;
    }
    if ((args->user_ptr | args->user_size) & (PAGE_SIZE - 1)) {
        return -EINVAL;
    }
    if (args->user_ptr > (uintptr_t)-1 ||
        !user_span_ok((uintptr_t)args->user_ptr, (size_t)args->user_size) || !mm) {
        return -EFAULT;
    }
    if (args->flags & (I915_USERPTR_UNSYNCHRONIZED | I915_USERPTR_READ_ONLY)) {
        return -ENODEV;
    }
    if (args->flags & I915_USERPTR_PROBE) {
        mmap_read_lock(mm);
        err = probe_range(mm, (uintptr_t)args->user_ptr, (uintptr_t)args->user_size);
        mmap_unlock(mm);
        if (err != 0) {
            return err;
        }
    }
    obj = i915_gem_object_alloc(i915, args->user_size);
    if (!obj) {
        return -ENOMEM;
    }
    obj->userptr.ptr = (uintptr_t)args->user_ptr;
    obj->userptr.set = 1;
    obj->base.no_export = 1;
    mmn_register(mm, &obj->userptr.range, obj->userptr.ptr,
                 obj->userptr.ptr + (uintptr_t)args->user_size, &userptr_mmn_ops, obj);
    err = drm_gem_handle_create(file, &obj->base, &args->handle);
    i915_gem_object_put(obj);
    return err;
}
