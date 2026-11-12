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
 * References
 */

/** Add 1 to `*refs`, which the caller holds one of, as Linux's kref_get. */
static void refs_get(volatile uint32_t *refs)
{
    uint32_t old;

    do {
        old = *refs;
    } while (arch_atomic_cmpxchg((volatile unsigned int *)refs, old, old + 1u) != old);
}

/** Add 1 to `*refs` unless it is 0, as Linux's kref_get_unless_zero: 1
 * when it did, 0 when the last reference has gone. */
static int refs_get_unless_zero(volatile uint32_t *refs)
{
    uint32_t old;

    do {
        old = *refs;
        if (old == 0) {
            return 0;
        }
    } while (arch_atomic_cmpxchg((volatile unsigned int *)refs, old, old + 1u) != old);
    return 1;
}

/** Take 1 from `*refs`: 1 when that was the last, as Linux's kref_put. */
static int refs_put(volatile uint32_t *refs)
{
    uint32_t old;

    do {
        old = *refs;
    } while (arch_atomic_cmpxchg((volatile unsigned int *)refs, old, old - 1u) != old);
    return old == 1u;
}

/* ------------------------------------------------------------------------
 * Handle tables
 *
 * A slot holds its handle from the insert on. A GEM handle's `object` is
 * NULL while the object's `open` hook runs, which reserves the id and
 * hides it from every lookup, as Linux's drm_gem_handle_create_tail
 * (drm_gem.c:419-454) allocates the id with NULL and replaces it after.
 */

/** The slots a table of `cap` slots grows to so that it holds the id `id`,
 * doubling from 64, or 0 past what a size can say. */
static uint32_t slots_for(uint32_t cap, uint32_t id)
{
    if (!cap) {
        cap = 64u;
    }
    while (cap <= id) {
        if (cap > 0x7FFFFFFFu / sizeof(struct drm_handle *) / 2u) {
            return 0;
        }
        cap *= 2u;
    }
    return cap;
}

/**
 * Put `h` into `t` under the lowest free id from 1 and give the id in
 * `h->id` and `*id`: 0, or -ENOMEM. A table too small grows by slots
 * allocated with the lock let go, as Linux's idr_preload, and the search
 * starts again with the lock taken back, since another insert may have run
 * meanwhile.
 */
static int32_t handle_install(struct drm_handles *t, struct drm_handle *h, uint32_t *id)
{
    struct drm_handle **slot = NULL, **old = NULL;
    uint32_t want, have, cap = 0, flags;

    for (;;) {
        flags = spin_lock_irqsave(&t->lock);
        want = t->free ? t->free : 1u;
        while (want < t->cap && t->slot[want]) {
            want++;
        }
        if (want < t->cap) {
            break;
        }
        if (slot && cap > want) {
            if (t->slot) {
                k_memcpy(slot, t->slot, t->cap * sizeof(*slot));
            }
            old = t->slot;
            t->slot = slot;
            t->cap = cap;
            slot = NULL;
            break;
        }
        have = t->cap;
        spin_unlock_irqrestore(&t->lock, flags);
        kfree(slot);
        cap = slots_for(have, want);
        slot = cap ? (struct drm_handle **)kzalloc(cap * sizeof(*slot)) : NULL;
        if (!slot) {
            return -ENOMEM;
        }
    }
    h->id = want;
    t->slot[want] = h;
    t->free = want + 1u;
    spin_unlock_irqrestore(&t->lock, flags);
    kfree(slot);
    kfree(old);
    *id = want;
    return 0;
}

/** A new handle to `object`, every other member clear, or NULL. */
static struct drm_handle *handle_new(void *object)
{
    struct drm_handle *h = (struct drm_handle *)kzalloc(sizeof(*h));

    if (h) {
        h->object = object;
    }
    return h;
}

int32_t drm_handle_insert(struct drm_handles *t, void *object, uint32_t *id)
{
    struct drm_handle *h = handle_new(object);

    if (!h) {
        return -ENOMEM;
    }
    if (handle_install(t, h, id) != 0) {
        kfree(h);
        return -ENOMEM;
    }
    return 0;
}

/** The handle of `id` in `t`, reserved or not, or NULL. `t->lock` is
 * held. */
static struct drm_handle *handle_at(const struct drm_handles *t, uint32_t id)
{
    return id && id < t->cap ? t->slot[id] : NULL;
}

void *drm_handle_find(struct drm_handles *t, uint32_t id)
{
    uint32_t flags = spin_lock_irqsave(&t->lock);
    struct drm_handle *h = handle_at(t, id);
    void *object = h ? h->object : NULL;

    spin_unlock_irqrestore(&t->lock, flags);
    return object;
}

void *drm_handle_find_get(struct drm_handles *t, uint32_t id, void (*get)(void *object))
{
    uint32_t flags = spin_lock_irqsave(&t->lock);
    struct drm_handle *h = handle_at(t, id);
    void *object = h ? h->object : NULL;

    if (object) {
        get(object);
    }
    spin_unlock_irqrestore(&t->lock, flags);
    return object;
}

/* The buckets the PRIME hash of a table starts with, and the most it grows
 * to; it doubles when it holds twice as many caches as buckets. */
#define PRIME_BUCKETS_FIRST 16u
#define PRIME_BUCKETS_MAX   0x10000u

/** The bucket of `buf` among `buckets`, a power of two. */
static uint32_t prime_bucket(const struct dma_buf *buf, uint32_t buckets)
{
    uintptr_t k = (uintptr_t)buf >> 4;

    return (uint32_t)(k ^ (k >> 8) ^ (k >> 16)) & (buckets - 1u);
}

/** The buckets `t` should have before it takes one more cache: the first
 * PRIME_BUCKETS_FIRST, twice as many as it has when it holds twice as many
 * caches as buckets, or what it has. `t->lock` is held. */
static uint32_t prime_buckets_wanted(const struct drm_handles *t)
{
    if (!t->prime) {
        return PRIME_BUCKETS_FIRST;
    }
    if (t->prime_count >= 2u * t->prime_buckets && t->prime_buckets < PRIME_BUCKETS_MAX) {
        return t->prime_buckets * 2u;
    }
    return t->prime_buckets;
}

/** Move `t`'s caches into `b`, `n` zeroed buckets, and return the old
 * buckets for the caller to free once the lock is let go. `t->lock` is
 * held. */
static struct drm_handle **prime_rehash(struct drm_handles *t, struct drm_handle **b, uint32_t n)
{
    struct drm_handle **old = t->prime, *h, *next;
    uint32_t i, k;

    for (i = 0; i < t->prime_buckets; i++) {
        for (h = old[i]; h; h = next) {
            next = h->prime_next;
            k = prime_bucket(h->prime, n);
            h->prime_next = b[k];
            b[k] = h;
        }
    }
    t->prime = b;
    t->prime_buckets = n;
    return old;
}

int32_t drm_handle_set_prime(struct drm_handles *t, uint32_t id, const void *object,
                             struct dma_buf *buf)
{
    struct drm_handle **b = NULL, **old = NULL, **link, *h;
    uint32_t n = 0, want, flags;
    int32_t rc;

    /* Buckets are allocated with the lock let go; a table whose growth
     * finds no memory keeps the buckets it has, and only the first set
     * fails for it. */
    flags = spin_lock_irqsave(&t->lock);
    for (;;) {
        want = prime_buckets_wanted(t);
        if (want == t->prime_buckets) {
            break;
        }
        if (b && n == want) {
            old = prime_rehash(t, b, n);
            b = NULL;
            break;
        }
        spin_unlock_irqrestore(&t->lock, flags);
        kfree(b);
        n = want;
        b = (struct drm_handle **)kzalloc(n * sizeof(*b));
        flags = spin_lock_irqsave(&t->lock);
        if (!b) {
            if (!t->prime) {
                spin_unlock_irqrestore(&t->lock, flags);
                return -ENOMEM;
            }
            break;
        }
    }
    h = handle_at(t, id);
    rc = !h || !h->object || h->object != object ? -ENOENT : h->prime ? -EEXIST : 0;
    if (rc == 0) {
        link = &t->prime[prime_bucket(buf, t->prime_buckets)];
        h->prime = buf;
        h->prime_next = *link;
        *link = h;
        t->prime_count++;
    }
    spin_unlock_irqrestore(&t->lock, flags);
    kfree(b);
    kfree(old);
    return rc;
}

struct dma_buf *drm_handle_prime_get(struct drm_handles *t, uint32_t id)
{
    uint32_t flags = spin_lock_irqsave(&t->lock);
    struct drm_handle *h = handle_at(t, id);
    struct dma_buf *buf = h ? dma_buf_ref(h->prime) : NULL;

    spin_unlock_irqrestore(&t->lock, flags);
    return buf;
}

uint32_t drm_handle_prime_find(struct drm_handles *t, const struct dma_buf *buf)
{
    const struct drm_handle *h;
    uint32_t id = 0, flags = spin_lock_irqsave(&t->lock);

    if (t->prime) {
        for (h = t->prime[prime_bucket(buf, t->prime_buckets)]; h; h = h->prime_next) {
            if (h->prime == buf && (!id || h->id < id)) {
                id = h->id;
            }
        }
    }
    spin_unlock_irqrestore(&t->lock, flags);
    return id;
}

/**
 * Take the handle `id` out of `t`, its id free again, and return it, or
 * NULL when there is none: only a reserved handle when `reserved` is set,
 * and only a published one when it is clear. The caller frees it with
 * `handle_free`, once nothing else links it.
 */
static struct drm_handle *handle_take(struct drm_handles *t, uint32_t id, int reserved)
{
    uint32_t flags = spin_lock_irqsave(&t->lock);
    struct drm_handle *h = handle_at(t, id), **p;

    if (h && (h->object == NULL) != (reserved != 0)) {
        h = NULL;
    }
    if (h) {
        if (h->prime) {
            for (p = &t->prime[prime_bucket(h->prime, t->prime_buckets)]; *p != h;
                 p = &(*p)->prime_next) {
            }
            *p = h->prime_next;
            t->prime_count--;
        }
        t->slot[id] = NULL;
        if (id < t->free) {
            t->free = id;
        }
    }
    spin_unlock_irqrestore(&t->lock, flags);
    return h;
}

/** Free `h`, taken out of its table, and drop its PRIME cache's
 * reference. */
static void handle_free(struct drm_handle *h)
{
    dma_buf_put(h->prime);
    kfree(h);
}

void *drm_handle_remove(struct drm_handles *t, uint32_t id)
{
    struct drm_handle *h = handle_take(t, id, 0);
    void *object;

    if (!h) {
        return NULL;
    }
    object = h->object;
    handle_free(h);
    return object;
}

uint32_t drm_handle_next(struct drm_handles *t, uint32_t id)
{
    uint32_t flags = spin_lock_irqsave(&t->lock);

    while (id < t->cap && (!id || !t->slot[id])) {
        id++;
    }
    if (id >= t->cap) {
        id = 0;
    }
    spin_unlock_irqrestore(&t->lock, flags);
    return id;
}

void drm_handles_fini(struct drm_handles *t)
{
    kfree(t->slot);
    t->slot = NULL;
    t->cap = 0;
    t->free = 0;
    kfree(t->prime);
    t->prime = NULL;
    t->prime_buckets = 0;
    t->prime_count = 0;
}

/* ------------------------------------------------------------------------
 * Buffer objects
 */

void drm_gem_object_init(struct drm_device *dev, struct drm_gem_object *obj, uint64_t size,
                         const struct drm_gem_object_funcs *funcs)
{
    uint32_t flags;

    k_memset(obj, 0, sizeof(*obj));
    obj->refs = 1;
    obj->dev = dev;
    obj->size = size;
    obj->funcs = funcs;
    obj->resv = &obj->resv_own;
    dma_resv_init(&obj->resv_own);
    drm_dev_get(dev);
    flags = spin_lock_irqsave(&dev->object_lock);
    obj->dev_next = dev->objects;
    dev->objects = obj;
    spin_unlock_irqrestore(&dev->object_lock, flags);
}

void drm_gem_object_get(struct drm_gem_object *obj)
{
    if (obj) {
        refs_get(&obj->refs);
    }
}

/** Take `obj` off the list at `head`, the device's objects linked through
 * `dev_next`, or with `mmap` set those with an offset, through
 * `mmap_next`. The device's `object_lock` is held. */
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
    uint32_t flags;

    if (!obj || !refs_put(&obj->refs)) {
        return;
    }
    dev = obj->dev;
    import = obj->import;
    flags = spin_lock_irqsave(&dev->object_lock);
    unlink_obj(&dev->objects, obj, 0);
    if (obj->mmap_offset) {
        unlink_obj(&dev->mmap_objects, obj, 1);
    }
    spin_unlock_irqrestore(&dev->object_lock, flags);
    dma_resv_fini(&obj->resv_own);
    if (obj->funcs && obj->funcs->free) {
        obj->funcs->free(obj);
    } else {
        kfree(obj);
    }
    dma_buf_put(import);
    drm_dev_put(dev);
}

struct drm_gem_object *drm_gem_object_next(struct drm_device *dev, struct drm_gem_object *prev)
{
    struct drm_gem_object *o;
    uint32_t flags = spin_lock_irqsave(&dev->object_lock);

    /* `prev`, held, is still on the list: only its last put takes it off. */
    o = prev ? prev->dev_next : dev->objects;
    while (o && !refs_get_unless_zero(&o->refs)) {
        o = o->dev_next;
    }
    spin_unlock_irqrestore(&dev->object_lock, flags);
    drm_gem_object_put(prev);
    return o;
}

/** Take `h` off `obj`'s handles and drop the hold it gave: the last handle
 * takes the object's flink name, as Linux's
 * drm_gem_object_handle_put_unlocked (drm_gem.c:279-288), and then the
 * handle's reference goes. */
static void handle_put(struct drm_gem_object *obj, struct drm_handle *h)
{
    struct drm_device *dev = obj->dev;
    struct drm_handle **p;
    uint32_t flags = spin_lock_irqsave(&dev->object_lock);

    for (p = &obj->handles; *p && *p != h; p = &(*p)->obj_next) {
    }
    if (*p) {
        *p = h->obj_next;
    }
    if (--obj->handle_count == 0) {
        obj->name = 0;
    }
    spin_unlock_irqrestore(&dev->object_lock, flags);
    drm_gem_object_put(obj);
}

/**
 * As Linux's drm_gem_handle_create_tail (drm_gem.c:406-460): the object's
 * handle count rises first, so that a racing close of another handle
 * leaves its flink name; the id is reserved while the `open` hook runs,
 * and published after it.
 */
int32_t drm_gem_handle_create(struct drm_file *file, struct drm_gem_object *obj, uint32_t *handle)
{
    struct drm_device *dev = obj->dev;
    struct drm_handle *h = handle_new(NULL);
    uint32_t flags;
    int32_t rc;

    if (!h) {
        return -ENOMEM;
    }
    h->file = file;
    drm_gem_object_get(obj);
    flags = spin_lock_irqsave(&dev->object_lock);
    h->obj_next = obj->handles;
    obj->handles = h;
    obj->handle_count++;
    spin_unlock_irqrestore(&dev->object_lock, flags);
    rc = handle_install(&file->objects, h, handle);
    if (rc == 0 && obj->funcs && obj->funcs->open) {
        rc = obj->funcs->open(obj, file);
        if (rc != 0) {
            handle_take(&file->objects, *handle, 1);
        }
    }
    if (rc != 0) {
        handle_put(obj, h);
        kfree(h);
        return rc;
    }
    flags = spin_lock_irqsave(&file->objects.lock);
    h->object = obj;
    spin_unlock_irqrestore(&file->objects.lock, flags);
    return 0;
}

/** As Linux's objects_lookup (drm_gem.c:710-731): the reference is taken
 * under the table's lock, so a racing close cannot free the object
 * first. */
struct drm_gem_object *drm_gem_object_lookup(struct drm_file *file, uint32_t handle)
{
    uint32_t flags = spin_lock_irqsave(&file->objects.lock);
    struct drm_handle *h = handle_at(&file->objects, handle);
    struct drm_gem_object *obj = h ? (struct drm_gem_object *)h->object : NULL;

    drm_gem_object_get(obj);
    spin_unlock_irqrestore(&file->objects.lock, flags);
    return obj;
}

int32_t drm_gem_handle_delete(struct drm_file *file, uint32_t handle)
{
    struct drm_handle *h = handle_take(&file->objects, handle, 0);

    if (!h) {
        return -EINVAL;
    }
    handle_put((struct drm_gem_object *)h->object, h);
    handle_free(h);
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
 * search again, so the two lists need not be merged. The device's
 * `object_lock` is held.
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
    uint32_t flags = spin_lock_irqsave(&dev->object_lock);
    int32_t rc = 0;

    if (!obj->mmap_offset && (rc = mmap_space_find(dev, obj->size >> PAGE_SHIFT, &start)) == 0) {
        while (*link && (*link)->mmap_offset < start << PAGE_SHIFT) {
            link = &(*link)->mmap_next;
        }
        obj->mmap_offset = start << PAGE_SHIFT;
        obj->mmap_next = *link;
        *link = obj;
    }
    spin_unlock_irqrestore(&dev->object_lock, flags);
    return rc;
}

int32_t drm_mmap_node_add(struct drm_mmap_node *node)
{
    struct drm_device *dev = node->obj->dev;
    struct drm_mmap_node **link = &dev->mmap_nodes;
    uint64_t start;
    uint32_t flags = spin_lock_irqsave(&dev->object_lock);
    int32_t rc = 0;

    if (!node->offset && (rc = mmap_space_find(dev, node->obj->size >> PAGE_SHIFT, &start)) == 0) {
        while (*link && (*link)->offset < start << PAGE_SHIFT) {
            link = &(*link)->next;
        }
        node->offset = start << PAGE_SHIFT;
        node->next = *link;
        *link = node;
    }
    spin_unlock_irqrestore(&dev->object_lock, flags);
    return rc;
}

void drm_mmap_node_remove(struct drm_mmap_node *node)
{
    struct drm_device *dev = node->obj->dev;
    struct drm_mmap_node **link;
    uint32_t flags = spin_lock_irqsave(&dev->object_lock);

    if (node->offset) {
        for (link = &dev->mmap_nodes; *link; link = &(*link)->next) {
            if (*link == node) {
                *link = node->next;
                break;
            }
        }
        node->offset = 0;
        node->next = NULL;
    }
    spin_unlock_irqrestore(&dev->object_lock, flags);
}

/** Whether `file` has a handle to `obj`, which Linux's mmap asks
 * (drm_vma_node_is_allowed): a walk over the object's handles, not the
 * file's. The device's `object_lock` is held. */
static int file_has_handle(const struct drm_file *file, const struct drm_gem_object *obj)
{
    const struct drm_handle *h;

    for (h = obj->handles; h; h = h->obj_next) {
        if (h->file == file) {
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

/** As Linux's drm_gem_mmap (drm_gem.c:1150-1200): an object found in the
 * mmap space whose last reference is going is no object, since its free
 * is about to take it out. */
int32_t drm_gem_mmap(struct drm_file *file, vma_t *vma)
{
    struct drm_device *dev = file->dev;
    struct drm_gem_object *obj;
    struct drm_mmap_node *node = NULL;
    uint32_t pages = (uint32_t)((vma->end - vma->start) >> PAGE_SHIFT);
    uint32_t start = 0, flags;
    int32_t rc = 0;
    int held = 0;

    if (dev->unplugged) {
        return -ENODEV;
    }
    flags = spin_lock_irqsave(&dev->object_lock);
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
        rc = -EINVAL;
    } else if (!(held = refs_get_unless_zero(&obj->refs))) {
        rc = -EINVAL;
    } else if (!file_has_handle(file, obj)) {
        rc = -EACCES;
    } else if (!node && (!obj->funcs || !obj->funcs->mmap)) {
        rc = -ENODEV;
    }
    spin_unlock_irqrestore(&dev->object_lock, flags);
    if (rc == 0) {
        vma->pgoff -= start;
        rc = node ? node->mmap(node, vma) : obj->funcs->mmap(obj, vma);
        if (rc != 0) {
            vma->pgoff += start;
        }
    }
    if (rc != 0 && held) {
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

static const struct vm_device_ops shmem_vm_ops = {shmem_vm_page, shmem_vm_put, NULL, 0};

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
 * free on the device, from 1. ENOENT for no such handle, or for one closed
 * meanwhile, as Linux (drm_gem.c:895-900), since the last handle's close
 * takes the name. */
int32_t drm_gem_flink_ioctl(struct drm_device *dev, void *data, struct drm_file *file)
{
    struct drm_gem_flink *args = (struct drm_gem_flink *)data;
    struct drm_gem_object *obj, *o;
    uint32_t name = 1, flags;
    int32_t rc = 0;

    if (!drm_core_check_feature(dev, DRIVER_GEM)) {
        return -EOPNOTSUPP;
    }
    obj = drm_gem_object_lookup(file, args->handle);
    if (!obj) {
        return -ENOENT;
    }
    flags = spin_lock_irqsave(&dev->object_lock);
    if (obj->handle_count == 0) {
        rc = -ENOENT;
    } else if (!obj->name) {
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
    if (rc == 0) {
        args->name = obj->name;
    }
    spin_unlock_irqrestore(&dev->object_lock, flags);
    drm_gem_object_put(obj);
    return rc;
}

/** GEM_OPEN: a new handle to the object of a flink name, and its size.
 * ENOENT for no such name. An object with a name has a handle, which holds
 * a reference, so the reference taken here under the device's
 * `object_lock` is never the first, as Linux's (drm_gem.c:942-946). */
int32_t drm_gem_open_ioctl(struct drm_device *dev, void *data, struct drm_file *file)
{
    struct drm_gem_open *args = (struct drm_gem_open *)data;
    struct drm_gem_object *obj;
    uint32_t handle, flags;
    int32_t rc;

    if (!drm_core_check_feature(dev, DRIVER_GEM)) {
        return -EOPNOTSUPP;
    }
    flags = spin_lock_irqsave(&dev->object_lock);
    for (obj = dev->objects; obj && (!args->name || obj->name != args->name); obj = obj->dev_next) {
    }
    drm_gem_object_get(obj);
    spin_unlock_irqrestore(&dev->object_lock, flags);
    if (!obj) {
        return -ENOENT;
    }
    rc = drm_gem_handle_create(file, obj, &handle);
    if (rc == 0) {
        args->handle = handle;
        args->size = obj->size;
    }
    drm_gem_object_put(obj);
    return rc;
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
