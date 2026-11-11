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

/** Make room in `t` for the id `id`, doubling the slots: 0, or -ENOMEM. */
static int32_t handles_grow(struct drm_handles *t, uint32_t id)
{
    uint32_t cap = t->cap ? t->cap : 64u;
    struct drm_handle **slot;

    while (cap <= id) {
        if (cap > 0x7FFFFFFFu / sizeof(*slot) / 2u) {
            return -ENOMEM;
        }
        cap *= 2u;
    }
    slot = (struct drm_handle **)kzalloc(cap * sizeof(*slot));
    if (!slot) {
        return -ENOMEM;
    }
    if (t->slot) {
        k_memcpy(slot, t->slot, t->cap * sizeof(*slot));
        kfree(t->slot);
    }
    t->slot = slot;
    t->cap = cap;
    return 0;
}

int32_t drm_handle_insert(struct drm_handles *t, void *object, uint32_t *id)
{
    struct drm_handle *h;
    uint32_t want = t->free ? t->free : 1u;

    while (want < t->cap && t->slot[want]) {
        want++;
    }
    if (want >= t->cap && handles_grow(t, want) != 0) {
        return -ENOMEM;
    }
    h = (struct drm_handle *)kmalloc(sizeof(*h));
    if (!h) {
        return -ENOMEM;
    }
    h->id = want;
    h->object = object;
    h->prime = NULL;
    t->slot[want] = h;
    t->free = want + 1u;
    *id = want;
    return 0;
}

struct drm_handle *drm_handle_get(const struct drm_handles *t, uint32_t id)
{
    return id < t->cap ? t->slot[id] : NULL;
}

void *drm_handle_find(const struct drm_handles *t, uint32_t id)
{
    struct drm_handle *h = drm_handle_get(t, id);

    return h ? h->object : NULL;
}

void *drm_handle_remove(struct drm_handles *t, uint32_t id)
{
    struct drm_handle *h = drm_handle_get(t, id);
    void *object;

    if (!h || id == 0) {
        return NULL;
    }
    object = h->object;
    t->slot[id] = NULL;
    if (id < t->free) {
        t->free = id;
    }
    dma_buf_put(h->prime);
    kfree(h);
    return object;
}

uint32_t drm_handle_next(const struct drm_handles *t, uint32_t id)
{
    for (; id < t->cap; id++) {
        if (id && t->slot[id]) {
            return id;
        }
    }
    return 0;
}

void drm_handles_fini(struct drm_handles *t)
{
    kfree(t->slot);
    t->slot = NULL;
    t->cap = 0;
    t->free = 0;
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
    struct dma_buf *import;

    if (!obj || --obj->refs > 0) {
        return;
    }
    dev = obj->dev;
    import = obj->import;
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
    dma_buf_put(import);
    drm_dev_put(dev);
}

int32_t drm_gem_handle_create(struct drm_file *file, struct drm_gem_object *obj, uint32_t *handle)
{
    int32_t rc = drm_handle_insert(&file->objects, obj, handle);

    if (rc != 0) {
        return rc;
    }
    obj->handle_count++;
    drm_gem_object_get(obj);
    rc = obj->funcs && obj->funcs->open ? obj->funcs->open(obj, file) : 0;
    if (rc != 0) {
        drm_gem_handle_delete(file, *handle);
    }
    return rc;
}

struct drm_gem_object *drm_gem_object_lookup(struct drm_file *file, uint32_t handle)
{
    struct drm_gem_object *obj = (struct drm_gem_object *)drm_handle_find(&file->objects, handle);

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
    uint32_t i;

    for (i = drm_handle_next(&file->objects, 1); i; i = drm_handle_next(&file->objects, i + 1)) {
        drm_gem_handle_delete(file, i);
    }
    drm_handles_fini(&file->objects);
}

/**
 * The first page offset of `dev`'s mmap space from which `pages` pages
 * touch neither an object's offset nor a driver's node, into `*at`: 0, or
 * -ENOSPC. Each range in the way moves the candidate past it and starts the
 * search again, so the two lists need not be merged.
 */
static int32_t mmap_space_find(struct drm_device *dev, uint64_t pages, uint64_t *at)
{
    uint64_t start = DRM_FILE_PAGE_OFFSET_START;
    uint64_t end = start + DRM_FILE_PAGE_OFFSET_SIZE;
    struct drm_gem_object *o;
    struct drm_mmap_node *n;
    int moved = 1;

    while (moved) {
        moved = 0;
        for (o = dev->mmap_objects; o; o = o->mmap_next) {
            uint64_t a = o->mmap_offset >> PAGE_SHIFT, b = a + (o->size >> PAGE_SHIFT);

            if (a < start + pages && start < b) {
                start = b;
                moved = 1;
            }
        }
        for (n = dev->mmap_nodes; n; n = n->next) {
            uint64_t a = n->offset >> PAGE_SHIFT, b = a + (n->obj->size >> PAGE_SHIFT);

            if (a < start + pages && start < b) {
                start = b;
                moved = 1;
            }
        }
    }
    if (pages > end - start) {
        return -ENOSPC;
    }
    *at = start;
    return 0;
}

int32_t drm_gem_create_mmap_offset(struct drm_gem_object *obj)
{
    struct drm_device *dev = obj->dev;
    struct drm_gem_object **link = &dev->mmap_objects;
    uint64_t start;

    if (obj->mmap_offset) {
        return 0;
    }
    if (mmap_space_find(dev, obj->size >> PAGE_SHIFT, &start) != 0) {
        return -ENOSPC;
    }
    while (*link && (*link)->mmap_offset < start << PAGE_SHIFT) {
        link = &(*link)->mmap_next;
    }
    obj->mmap_offset = start << PAGE_SHIFT;
    obj->mmap_next = *link;
    *link = obj;
    return 0;
}

int32_t drm_mmap_node_add(struct drm_mmap_node *node)
{
    struct drm_device *dev = node->obj->dev;
    struct drm_mmap_node **link = &dev->mmap_nodes;
    uint64_t start;

    if (node->offset) {
        return 0;
    }
    if (mmap_space_find(dev, node->obj->size >> PAGE_SHIFT, &start) != 0) {
        return -ENOSPC;
    }
    while (*link && (*link)->offset < start << PAGE_SHIFT) {
        link = &(*link)->next;
    }
    node->offset = start << PAGE_SHIFT;
    node->next = *link;
    *link = node;
    return 0;
}

void drm_mmap_node_remove(struct drm_mmap_node *node)
{
    struct drm_mmap_node **link;

    if (!node->offset) {
        return;
    }
    for (link = &node->obj->dev->mmap_nodes; *link; link = &(*link)->next) {
        if (*link == node) {
            *link = node->next;
            break;
        }
    }
    node->offset = 0;
    node->next = NULL;
}

/** Whether `file` has a handle to `obj`, which Linux's mmap asks
 * (drm_vma_node_is_allowed). */
static int file_has_handle(const struct drm_file *file, const struct drm_gem_object *obj)
{
    uint32_t i;

    for (i = drm_handle_next(&file->objects, 1); i; i = drm_handle_next(&file->objects, i + 1)) {
        if (drm_handle_get(&file->objects, i)->object == obj) {
            return 1;
        }
    }
    return 0;
}

/** Whether the object at mmap offset `offset` holds page `pgoff` of the
 * device's mmap space. */
static int mmap_covers(uint64_t offset, const struct drm_gem_object *obj, uint32_t pgoff)
{
    uint64_t first = offset >> PAGE_SHIFT;

    return offset != 0 && pgoff >= first && pgoff - first < obj->size >> PAGE_SHIFT;
}

int32_t drm_gem_mmap(struct drm_file *file, vma_t *vma)
{
    struct drm_device *dev = file->dev;
    struct drm_gem_object *obj;
    struct drm_mmap_node *node = NULL;
    uint32_t pages = (uint32_t)((vma->end - vma->start) >> PAGE_SHIFT);
    uint32_t start = 0;
    int32_t rc;

    if (dev->unplugged) {
        return -ENODEV;
    }
    for (obj = dev->mmap_objects; obj; obj = obj->mmap_next) {
        if (mmap_covers(obj->mmap_offset, obj, vma->pgoff)) {
            start = (uint32_t)(obj->mmap_offset >> PAGE_SHIFT);
            break;
        }
    }
    if (!obj) {
        for (node = dev->mmap_nodes; node; node = node->next) {
            if (mmap_covers(node->offset, node->obj, vma->pgoff)) {
                obj = node->obj;
                start = (uint32_t)(node->offset >> PAGE_SHIFT);
                break;
            }
        }
    }
    if (!obj || pages > (obj->size >> PAGE_SHIFT) - (vma->pgoff - start)) {
        return -EINVAL;
    }
    if (!file_has_handle(file, obj)) {
        return -EACCES;
    }
    if (!node && (!obj->funcs || !obj->funcs->mmap)) {
        return -ENODEV;
    }
    drm_gem_object_get(obj);
    vma->pgoff -= start;
    rc = node ? node->mmap(node, vma) : obj->funcs->mmap(obj, vma);
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
    if (obj->base.import) {
        struct dma_buf *b = obj->base.import;

        return b->ops->page ? b->ops->page(b, pgoff) : 0;
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

static const struct vm_device_ops shmem_vm_ops = {shmem_vm_page, shmem_vm_put, NULL};

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

/** Return a shmem object's shared physical frame to a PRIME importer. */
static uintptr_t shmem_obj_page(struct drm_gem_object *obj, uint32_t pgoff)
{
    if (obj->dev->unplugged) {
        return 0;
    }
    return drm_gem_shmem_page((struct drm_gem_shmem_object *)obj, pgoff);
}

static const struct drm_gem_object_funcs shmem_funcs = {shmem_obj_free, shmem_obj_mmap, NULL, NULL,
                                                        shmem_obj_page, NULL,           NULL, NULL};

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
    s->base.map_wc = map_wc;
    return s;
}

/** `drm_gem_shmem_dumb_create`'s body, the objects mapped write-combined
 * when `map_wc` is set. */
static int32_t shmem_dumb_create(struct drm_file *file, struct drm_device *dev,
                                 struct drm_mode_create_dumb *args, int map_wc)
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
    s = drm_gem_shmem_create(dev, args->size, map_wc);
    if (!s) {
        return -ENOMEM;
    }
    rc = drm_gem_handle_create(file, &s->base, &args->handle);
    drm_gem_object_put(&s->base);
    return rc;
}

int32_t drm_gem_shmem_dumb_create(struct drm_file *file, struct drm_device *dev,
                                  struct drm_mode_create_dumb *args)
{
    return shmem_dumb_create(file, dev, args, 0);
}

int32_t drm_gem_shmem_dumb_create_wc(struct drm_file *file, struct drm_device *dev,
                                     struct drm_mode_create_dumb *args)
{
    return shmem_dumb_create(file, dev, args, 1);
}

/* ------------------------------------------------------------------------
 * The ioctls
 */

/** GEM_CLOSE: drop a handle. EINVAL for no such handle; the pad is not
 * read, as Linux. */
int32_t drm_gem_close_ioctl(struct drm_device *dev, void *data, struct drm_file *file)
{
    struct drm_gem_close *args = (struct drm_gem_close *)data;

    if (!drm_core_check_feature(dev, DRIVER_GEM)) {
        return -EOPNOTSUPP;
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
 * an object made from a dma-buf, ENODEV for one without `mmap`, which
 * nothing could map, as Linux's i915 refuses a stolen object. */
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
    rc = obj->import                        ? -EINVAL
         : !obj->funcs || !obj->funcs->mmap ? -ENODEV
                                            : drm_gem_create_mmap_offset(obj);
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
