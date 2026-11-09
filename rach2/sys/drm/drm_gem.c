/*
 * drm_gem.c: buffer objects, the handles files have to them, their flink
 * names and mmap offsets, the shmem helper of objects made of frames, and
 * dumb buffers, as Linux 6.12's drm_gem.c, drm_gem_shmem_helper.c and
 * drm_dumb_buffers.c.
 */

#include "drm.h"
#include "../dmabuf/dmabuf.h"

#include "../lib/kerrno.h"
#include "../lib/kstring.h"
#include "../mm/kheap.h"

/* ------------------------------------------------------------------------
 * Handle tables
 */

int32_t drm_handle_insert(struct drm_handle **list, void *object, uint32_t *id)
{
    struct drm_handle *h = (struct drm_handle *)kmalloc(sizeof(*h));
    struct drm_handle **link = list;
    uint32_t want = 1;

    if (!h) {
        return -ENOMEM;
    }
    while (*link && (*link)->id == want) {
        want++;
        link = &(*link)->next;
    }
    h->id = want;
    h->object = object;
    h->next = *link;
    *link = h;
    *id = want;
    return 0;
}

void *drm_handle_find(struct drm_handle *list, uint32_t id)
{
    for (; list && list->id <= id; list = list->next) {
        if (list->id == id) {
            return list->object;
        }
    }
    return NULL;
}

void *drm_handle_remove(struct drm_handle **list, uint32_t id)
{
    struct drm_handle **link;

    for (link = list; *link && (*link)->id <= id; link = &(*link)->next) {
        if ((*link)->id == id) {
            struct drm_handle *h = *link;
            void *object = h->object;

            *link = h->next;
            kfree(h);
            return object;
        }
    }
    return NULL;
}

/* ------------------------------------------------------------------------
 * Buffer objects
 */

void drm_gem_object_init(struct drm_device *dev, struct drm_gem_object *obj, uint64_t size,
                         const struct drm_gem_object_funcs *funcs)
{
    k_memset(obj, 0, sizeof(*obj));
    obj->refs = 1;
    obj->dev = dev;
    obj->size = size;
    obj->funcs = funcs;
    obj->resv = &obj->resv_own;
    drm_dev_get(dev);
    obj->dev_next = dev->objects;
    dev->objects = obj;
}

void drm_gem_object_get(struct drm_gem_object *obj)
{
    if (obj) {
        obj->refs++;
    }
}

/** Take `obj` off the list at `head`, the device's objects linked through
 * `dev_next`, or with `mmap` set those with an offset, through
 * `mmap_next`. */
static void unlink_obj(struct drm_gem_object **head, struct drm_gem_object *obj, int mmap)
{
    struct drm_gem_object **p;

    for (p = head; *p; p = mmap ? &(*p)->mmap_next : &(*p)->dev_next) {
        if (*p == obj) {
            *p = mmap ? obj->mmap_next : obj->dev_next;
            return;
        }
    }
}

void drm_gem_object_put(struct drm_gem_object *obj)
{
    struct drm_device *dev;

    if (!obj || --obj->refs > 0) {
        return;
    }
    dev = obj->dev;
    unlink_obj(&dev->objects, obj, 0);
    if (obj->mmap_offset) {
        unlink_obj(&dev->mmap_objects, obj, 1);
    }
    dma_resv_fini(&obj->resv_own);
    if (obj->funcs && obj->funcs->free) {
        obj->funcs->free(obj);
    } else {
        kfree(obj);
    }
    drm_dev_put(dev);
}

int32_t drm_gem_handle_create(struct drm_file *file, struct drm_gem_object *obj, uint32_t *handle)
{
    int32_t rc = drm_handle_insert(&file->objects, obj, handle);

    if (rc == 0) {
        obj->handle_count++;
        drm_gem_object_get(obj);
    }
    return rc;
}

struct drm_gem_object *drm_gem_object_lookup(struct drm_file *file, uint32_t handle)
{
    struct drm_gem_object *obj = (struct drm_gem_object *)drm_handle_find(file->objects, handle);

    drm_gem_object_get(obj);
    return obj;
}

/** Drop a handle's hold on `obj`: the last handle takes its flink name,
 * as Linux's drm_gem_object_handle_put_unlocked. */
static void handle_put(struct drm_gem_object *obj)
{
    if (--obj->handle_count == 0) {
        obj->name = 0;
    }
    drm_gem_object_put(obj);
}

int32_t drm_gem_handle_delete(struct drm_file *file, uint32_t handle)
{
    struct drm_gem_object *obj = (struct drm_gem_object *)drm_handle_remove(&file->objects, handle);

    if (!obj) {
        return -EINVAL;
    }
    handle_put(obj);
    return 0;
}

void drm_gem_release(struct drm_file *file)
{
    while (file->objects) {
        drm_gem_handle_delete(file, file->objects->id);
    }
}

int32_t drm_gem_create_mmap_offset(struct drm_gem_object *obj)
{
    struct drm_device *dev = obj->dev;
    struct drm_gem_object **link = &dev->mmap_objects;
    uint64_t pages = obj->size >> PAGE_SHIFT;
    uint64_t start = DRM_FILE_PAGE_OFFSET_START;
    uint64_t end = start + DRM_FILE_PAGE_OFFSET_SIZE;

    if (obj->mmap_offset) {
        return 0;
    }
    /* The first gap that fits, in the order of the offsets. */
    for (; *link; link = &(*link)->mmap_next) {
        uint64_t at = (*link)->mmap_offset >> PAGE_SHIFT;

        if (at - start >= pages) {
            break;
        }
        start = at + ((*link)->size >> PAGE_SHIFT);
    }
    if (pages > end - start) {
        return -ENOSPC;
    }
    obj->mmap_offset = start << PAGE_SHIFT;
    obj->mmap_next = *link;
    *link = obj;
    return 0;
}

/** Whether `file` has a handle to `obj`, which Linux's mmap asks
 * (drm_vma_node_is_allowed). */
static int file_has_handle(const struct drm_file *file, const struct drm_gem_object *obj)
{
    const struct drm_handle *h;

    for (h = file->objects; h; h = h->next) {
        if (h->object == obj) {
            return 1;
        }
    }
    return 0;
}

int32_t drm_gem_mmap(struct drm_file *file, vma_t *vma)
{
    struct drm_device *dev = file->dev;
    struct drm_gem_object *obj;
    uint32_t pages = (uint32_t)((vma->end - vma->start) >> PAGE_SHIFT);
    uint32_t start;
    int32_t rc;

    if (dev->unplugged) {
        return -ENODEV;
    }
    for (obj = dev->mmap_objects; obj; obj = obj->mmap_next) {
        if ((obj->mmap_offset >> PAGE_SHIFT) == vma->pgoff) {
            break;
        }
    }
    if (!obj || pages > obj->size >> PAGE_SHIFT) {
        return -EINVAL;
    }
    if (!file_has_handle(file, obj)) {
        return -EACCES;
    }
    if (!obj->funcs || !obj->funcs->mmap) {
        return -ENODEV;
    }
    start = (uint32_t)(obj->mmap_offset >> PAGE_SHIFT);
    drm_gem_object_get(obj);
    vma->pgoff -= start;
    rc = obj->funcs->mmap(obj, vma);
    if (rc != 0) {
        vma->pgoff += start;
        drm_gem_object_put(obj);
    }
    return rc;
}

/* ------------------------------------------------------------------------
 * The shmem helper
 */

uintptr_t drm_gem_shmem_page(struct drm_gem_shmem_object *obj, uint32_t pgoff)
{
    uintptr_t phys;

    if ((uint64_t)pgoff >= obj->base.size >> PAGE_SHIFT) {
        return 0;
    }
    phys = shmem_resident(&obj->pages, pgoff);
    if (!phys && shmem_allocate(&obj->pages, (uint64_t)pgoff << PAGE_SHIFT, PAGE_SIZE) == 0) {
        phys = shmem_resident(&obj->pages, pgoff);
    }
    return phys;
}

/** The `page` of a mapping of a shmem object: its frame, given on the first
 * touch; none past its end or once its device is unplugged. */
static int shmem_vm_page(void *obj, uint32_t pgoff, uintptr_t *phys)
{
    struct drm_gem_shmem_object *s = (struct drm_gem_shmem_object *)obj;
    uintptr_t p;

    if (s->base.dev->unplugged) {
        return -1;
    }
    p = drm_gem_shmem_page(s, pgoff);
    if (!p) {
        return -1;
    }
    *phys = p;
    return 0;
}

/** The `put` of a mapping of a shmem object: the region's reference. */
static void shmem_vm_put(void *obj)
{
    drm_gem_object_put((struct drm_gem_object *)obj);
}

static const struct vm_device_ops shmem_vm_ops = {shmem_vm_page, shmem_vm_put};

/** The `mmap` of a shmem object: a device region over its frames. */
static int32_t shmem_obj_mmap(struct drm_gem_object *obj, vma_t *vma)
{
    struct drm_gem_shmem_object *s = (struct drm_gem_shmem_object *)obj;

    return vm_device_map(vma, &shmem_vm_ops, obj, s->map_wc ? VM_PROT_WC : 0u);
}

/** The `free` of a shmem object: its frames and its record. */
static void shmem_obj_free(struct drm_gem_object *obj)
{
    struct drm_gem_shmem_object *s = (struct drm_gem_shmem_object *)obj;

    shmem_free(&s->pages);
    kfree(s);
}

static const struct drm_gem_object_funcs shmem_funcs = {shmem_obj_free, shmem_obj_mmap, NULL, NULL};

struct drm_gem_shmem_object *drm_gem_shmem_create(struct drm_device *dev, uint64_t size, int map_wc)
{
    struct drm_gem_shmem_object *s;

    if (size == 0 || size > SHMEM_MAX_SIZE) {
        return NULL;
    }
    size = (size + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1);
    s = (struct drm_gem_shmem_object *)kzalloc(sizeof(*s));
    if (!s) {
        return NULL;
    }
    drm_gem_object_init(dev, &s->base, size, &shmem_funcs);
    s->pages.size = size;
    s->map_wc = map_wc;
    return s;
}

int32_t drm_gem_shmem_dumb_create(struct drm_file *file, struct drm_device *dev,
                                  struct drm_mode_create_dumb *args)
{
    uint32_t min_pitch = (args->width * args->bpp + 7) / 8;
    struct drm_gem_shmem_object *s;
    int32_t rc;

    if (!args->pitch || !args->size) {
        args->pitch = min_pitch;
        args->size =
            ((uint64_t)args->pitch * args->height + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1);
    } else {
        if (args->pitch < min_pitch) {
            args->pitch = min_pitch;
        }
        if (args->size < (uint64_t)args->pitch * args->height) {
            args->size =
                ((uint64_t)args->pitch * args->height + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1);
        }
    }
    if (args->size > SHMEM_MAX_SIZE) {
        return -EINVAL;
    }
    s = drm_gem_shmem_create(dev, args->size, 0);
    if (!s) {
        return -ENOMEM;
    }
    rc = drm_gem_handle_create(file, &s->base, &args->handle);
    drm_gem_object_put(&s->base);
    return rc;
}

/* ------------------------------------------------------------------------
 * The ioctls
 */

/** GEM_CLOSE: drop a handle. EINVAL for a nonzero pad or no such handle. */
int32_t drm_gem_close_ioctl(struct drm_device *dev, void *data, struct drm_file *file)
{
    struct drm_gem_close *args = (struct drm_gem_close *)data;

    if (!drm_core_check_feature(dev, DRIVER_GEM)) {
        return -EOPNOTSUPP;
    }
    if (args->pad) {
        return -EINVAL;
    }
    return drm_gem_handle_delete(file, args->handle);
}

/** GEM_FLINK: the object's global name, given the first time as the lowest
 * free on the device, from 1. ENOENT for no such handle. */
int32_t drm_gem_flink_ioctl(struct drm_device *dev, void *data, struct drm_file *file)
{
    struct drm_gem_flink *args = (struct drm_gem_flink *)data;
    struct drm_gem_object *obj, *o;
    uint32_t name = 1;

    if (!drm_core_check_feature(dev, DRIVER_GEM)) {
        return -EOPNOTSUPP;
    }
    obj = drm_gem_object_lookup(file, args->handle);
    if (!obj) {
        return -ENOENT;
    }
    if (!obj->name) {
        /* The lowest name no object has, as Linux's idr gives it: each
         * hit raises the candidate and starts the walk again. */
        for (o = dev->objects; o;) {
            if (o->name == name) {
                name++;
                o = dev->objects;
            } else {
                o = o->dev_next;
            }
        }
        obj->name = name;
    }
    args->name = obj->name;
    drm_gem_object_put(obj);
    return 0;
}

/** GEM_OPEN: a new handle to the object of a flink name, and its size.
 * ENOENT for no such name. */
int32_t drm_gem_open_ioctl(struct drm_device *dev, void *data, struct drm_file *file)
{
    struct drm_gem_open *args = (struct drm_gem_open *)data;
    struct drm_gem_object *obj;
    uint32_t handle;
    int32_t rc;

    if (!drm_core_check_feature(dev, DRIVER_GEM)) {
        return -EOPNOTSUPP;
    }
    for (obj = dev->objects; obj && (!args->name || obj->name != args->name); obj = obj->dev_next) {
    }
    if (!obj) {
        return -ENOENT;
    }
    rc = drm_gem_handle_create(file, obj, &handle);
    if (rc != 0) {
        return rc;
    }
    args->handle = handle;
    args->size = obj->size;
    return 0;
}

/**
 * MODE_CREATE_DUMB, as Linux 6.12's drm_mode_create_dumb: ENOSYS for a
 * driver without dumb buffers, EINVAL for a zero dimension or a size that
 * overflows 32 bits; the driver's `dumb_create` sets the handle, pitch and
 * size, which are zero when it fails.
 */
int32_t drm_mode_create_dumb_ioctl(struct drm_device *dev, void *data, struct drm_file *file)
{
    struct drm_mode_create_dumb *args = (struct drm_mode_create_dumb *)data;
    uint32_t cpp, stride, size;
    int32_t rc;

    if (!dev->driver->dumb_create) {
        rc = -ENOSYS;
    } else if (!args->width || !args->height || !args->bpp || args->bpp > 0xFFFFFFFFu - 8) {
        rc = -EINVAL;
    } else if ((cpp = (args->bpp + 7) / 8) > 0xFFFFFFFFu / args->width) {
        rc = -EINVAL;
    } else if (args->height > 0xFFFFFFFFu / (stride = cpp * args->width)) {
        rc = -EINVAL;
    } else if ((size = args->height * stride) > 0xFFFFFFFFu - (PAGE_SIZE - 1)) {
        rc = -EINVAL; /* PAGE_ALIGN wraps to 0 */
    } else {
        args->handle = 0;
        args->pitch = 0;
        args->size = 0;
        rc = dev->driver->dumb_create(file, dev, args);
    }
    (void)size;
    if (rc != 0) {
        args->handle = 0;
        args->pitch = 0;
        args->size = 0;
    }
    return rc;
}

/** MODE_MAP_DUMB: the object's mmap offset, made the first time. ENOSYS
 * for a driver without dumb buffers, ENOENT for no such handle, EINVAL for
 * an object made from a dma-buf. */
int32_t drm_mode_mmap_dumb_ioctl(struct drm_device *dev, void *data, struct drm_file *file)
{
    struct drm_mode_map_dumb *args = (struct drm_mode_map_dumb *)data;
    struct drm_gem_object *obj;
    int32_t rc;

    if (!dev->driver->dumb_create) {
        return -ENOSYS;
    }
    obj = drm_gem_object_lookup(file, args->handle);
    if (!obj) {
        return -ENOENT;
    }
    rc = obj->import ? -EINVAL : drm_gem_create_mmap_offset(obj);
    if (rc == 0) {
        args->offset = obj->mmap_offset;
    }
    drm_gem_object_put(obj);
    return rc;
}

/** MODE_DESTROY_DUMB: drop a handle, as GEM_CLOSE. */
int32_t drm_mode_destroy_dumb_ioctl(struct drm_device *dev, void *data, struct drm_file *file)
{
    struct drm_mode_destroy_dumb *args = (struct drm_mode_destroy_dumb *)data;

    if (!dev->driver->dumb_create) {
        return -ENOSYS;
    }
    return drm_gem_handle_delete(file, args->handle);
}
