/*
 * i915_gem_userptr.c: buffer objects over a program's own memory, whose
 * pages are pinned while the GPU may reach them and let go when what the
 * program maps there changes, after Linux 6.12's gem/i915_gem_userptr.c.
 */

#include "i915.h"
#include "i915_drm.h"

#include "../../lib/kerrno.h"
#include "../../mm/kheap.h"
#include "../../mm/mm.h"
#include "../../mm/paging.h"
#include "../../mm/pin.h"
#include "../../mm/usercopy.h"
#include "../../proc/percpu.h"
#include "../../proc/process.h"

/* The pins of a userptr object's pages tried before a change to the range
 * during the pin gives -EAGAIN, as Linux's notifier sequence does. */
#define USERPTR_PIN_TRIES 3

void i915_gem_userptr_put_pages(struct drm_i915_gem_object *obj)
{
    uint32_t i, n = (uint32_t)(obj->base.size >> PAGE_SHIFT);

    if (obj->frames) {
        for (i = 0; i < n; i++) {
            frame_unpin(obj->frames[i]);
        }
        kfree(obj->frames);
        obj->frames = 0;
    }
    obj->userptr.stale = 0;
}

/**
 * The range's `invalidate`, before the program's pages there change: an
 * unmap, a change of protection, a fork, which makes them copy-on-write, a
 * copy-on-write break, a page dropped, or the end of the address space.
 * The bindings are revoked first, as Linux's did before the pages went: an
 * unpinned one is undone, and a pinned one is pointed at the scratch page
 * until its last pin goes. A binding the GPU still uses keeps its entries,
 * and with them the frames, until the work is done, as Linux keeps the
 * pinned pages of a busy object. Then the frames are let go, or, while a
 * pin still holds them, marked stale, to go with its last. Does not sleep.
 */
static void userptr_invalidate(struct mmn_range *r, int event, uintptr_t start, uintptr_t end)
{
    struct drm_i915_gem_object *obj = (struct drm_i915_gem_object *)r->owner;
    struct drm_i915_private *i915 = to_i915(obj->base.dev);
    struct i915_vma *v, *next;

    (void)event;
    (void)start;
    (void)end;
    if (!obj->frames || obj->userptr.stale) {
        return;
    }
    for (v = obj->vmas; v; v = next) {
        next = v->obj_next;
        if (v->pin_count) {
            (void)i915_ggtt_clear_range(i915, v->start, v->node_size);
            i915_ppgtt_clear_range(i915, v->start, v->node_size);
        } else {
            (void)i915_vma_unbind(v);
        }
    }
    if (obj->pages_pin_count) {
        obj->userptr.stale = 1;
    } else {
        i915_gem_userptr_put_pages(obj);
    }
}

static const struct mmn_ops userptr_mmn_ops = {userptr_invalidate};

int32_t i915_gem_userptr_get_pages(struct drm_i915_gem_object *obj)
{
    uint32_t i, n = (uint32_t)(obj->base.size >> PAGE_SHIFT);
    uintptr_t start = obj->userptr.ptr, end = start + (uintptr_t)obj->base.size;
    uintptr_t *frames;
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
        (void)i915_gem_object_unbind(obj);
        if (obj->userptr.stale) {
            return -EAGAIN;
        }
    }
    if (!obj->userptr.range.mm || !current_proc || current_proc->mm != obj->userptr.range.mm) {
        return -EFAULT;
    }
    frames = (uintptr_t *)kmalloc((size_t)n * sizeof(*frames));
    if (!frames) {
        return -ENOMEM;
    }
    for (tries = 0; tries < USERPTR_PIN_TRIES; tries++) {
        seq = obj->userptr.range.seq;
        err = paging_pin_range(obj->userptr.range.mm, start, end, 1, frames);
        if (err != 0) {
            kfree(frames);
            return err;
        }
        if (seq == obj->userptr.range.seq) {
            obj->frames = frames;
            obj->pages_pin_count = 1;
            return 0;
        }
        /* The range changed while the pin slept: its frames may be old. */
        for (i = 0; i < n; i++) {
            frame_unpin(frames[i]);
        }
        if (!obj->userptr.range.mm) {
            break;
        }
    }
    kfree(frames);
    return obj->userptr.range.mm ? -EAGAIN : -EFAULT;
}

void i915_gem_userptr_release(struct drm_i915_gem_object *obj)
{
    mmn_unregister(&obj->userptr.range);
    i915_gem_userptr_put_pages(obj);
}

/**
 * Whether every page of [addr, addr + len) of `mm` lies in a region of
 * memory with pages, none of a device's, as Linux's probe_range: 0, or
 * -EFAULT for a hole or a device's region.
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
 * EFAULT for a range outside user space, or with PROBE one not wholly in
 * memory with pages; ENODEV for UNSYNCHRONIZED, which Linux 6.12 refuses
 * to everyone, and for READ_ONLY, since Haswell's GTT has no read-only
 * entries (Linux's has_read_only); ENOMEM. Its pages are pinned when it is
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
    if (args->user_size > (uint64_t)(size_t)-1) {
        return -E2BIG;
    }
    if (!args->user_size) {
        return -EINVAL;
    }
    if ((args->user_ptr | args->user_size) & (PAGE_SIZE - 1)) {
        return -EINVAL;
    }
    if (args->user_ptr > (uintptr_t)-1 ||
        !user_range_ok((uintptr_t)args->user_ptr, (size_t)args->user_size) || !mm) {
        return -EFAULT;
    }
    if (args->flags & (I915_USERPTR_UNSYNCHRONIZED | I915_USERPTR_READ_ONLY)) {
        return -ENODEV;
    }
    if (args->flags & I915_USERPTR_PROBE) {
        err = probe_range(mm, (uintptr_t)args->user_ptr, (uintptr_t)args->user_size);
        if (err != 0) {
            return err;
        }
    }
    obj = i915_gem_object_alloc(i915, args->user_size);
    if (!obj) {
        return -ENOMEM;
    }
    obj->userptr.ptr = (uintptr_t)args->user_ptr;
    obj->base.no_export = 1;
    mmn_register(mm, &obj->userptr.range, obj->userptr.ptr,
                 obj->userptr.ptr + (uintptr_t)args->user_size, &userptr_mmn_ops, obj);
    err = drm_gem_handle_create(file, &obj->base, &args->handle);
    i915_gem_object_put(obj);
    return err;
}
