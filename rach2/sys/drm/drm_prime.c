/* PRIME exports GEM objects as dma-bufs and imports their shared pages. */
#include "drm.h"
#include "../dmabuf/dmabuf.h"
#include "../lib/kerrno.h"

/** Map the exporter's pages; its mmap takes the new object reference. */
static int32_t prime_mmap(struct dma_buf *buf, vma_t *vma)
{
    struct drm_gem_object *obj = buf->priv;
    int32_t rc;

    if (obj->dev->unplugged) {
        return -ENODEV;
    }
    if (!obj->funcs || !obj->funcs->mmap) {
        return -ENODEV;
    }
    drm_gem_object_get(obj);
    rc = obj->funcs->mmap(obj, vma);
    if (rc) {
        drm_gem_object_put(obj);
    }
    return rc;
}

/** Clear the weak export pointer, if it still names `buf`, before
 * releasing the exporter's object. */
static void prime_release(struct dma_buf *buf)
{
    struct drm_gem_object *obj = buf->priv;
    uint32_t flags = spin_lock_irqsave(&obj->dev->object_lock);

    if (obj->dma_buf == buf) {
        obj->dma_buf = NULL;
    }
    spin_unlock_irqrestore(&obj->dev->object_lock, flags);
    drm_gem_object_put(obj);
}

/** Give an importer a borrowed frame, or 0 on failure. The exporter's
 * `page` decides whether its frames outlive its device's loss. */
static uintptr_t prime_page(struct dma_buf *buf, uint32_t pgoff)
{
    struct drm_gem_object *obj = buf->priv;

    if ((uint64_t)pgoff >= obj->size >> PAGE_SHIFT || !obj->funcs || !obj->funcs->page) {
        return 0;
    }
    return obj->funcs->page(obj, pgoff);
}

/** The exporter's part of a CPU access sync, where its driver has one. */
static int32_t prime_cpu_access(struct dma_buf *buf, uint64_t flags)
{
    struct drm_gem_object *obj = buf->priv;

    return obj->funcs && obj->funcs->cpu_access ? obj->funcs->cpu_access(obj, flags) : 0;
}

static const struct dma_buf_ops prime_ops = {prime_mmap, prime_release, prime_cpu_access,
                                             prime_page};

/** The handle of `file` whose PRIME cache holds `buf`, or 0 for none. */
static uint32_t prime_cached(struct drm_file *file, const struct dma_buf *buf)
{
    return drm_handle_prime_find(&file->objects, buf);
}

/**
 * The dma-buf of `obj` with a reference for the caller: the one it was
 * imported from, the one exported of it already, or a new export with
 * `flags`' access, or NULL with no memory. The weak `dma_buf` is read and
 * set under the device's `object_lock`, as Linux's export under
 * `object_name_lock` (drm_prime.c:455-470), so that two exports at once
 * keep one buffer; the loser's put finds the pointer not its own.
 */
static struct dma_buf *prime_export(struct drm_gem_object *obj, uint32_t flags)
{
    struct drm_device *dev = obj->dev;
    struct dma_buf *buf, *won;
    uint32_t irq = spin_lock_irqsave(&dev->object_lock);

    buf = dma_buf_ref(obj->import ? obj->import : obj->dma_buf);
    spin_unlock_irqrestore(&dev->object_lock, irq);
    if (buf) {
        return buf;
    }
    buf = dma_buf_export(obj->size, &prime_ops, obj, obj->resv);
    if (!buf) {
        return NULL;
    }
    drm_gem_object_get(obj);
    buf->read_only = !(flags & DRM_RDWR);
    buf->map_flags = obj->map_wc ? VM_PROT_WC : 0;
    irq = spin_lock_irqsave(&dev->object_lock);
    won = dma_buf_ref(obj->dma_buf);
    if (!won) {
        obj->dma_buf = buf;
    }
    spin_unlock_irqrestore(&dev->object_lock, irq);
    if (won) {
        dma_buf_put(buf);
        return won;
    }
    return buf;
}

/** Export a handle as a dma-buf descriptor, caching its identity until close.
 * Flags accept only DRM_CLOEXEC and DRM_RDWR. Return 0 or -errno: ENOENT
 * also for a handle closed while the export was made. */
int32_t drm_prime_handle_to_fd_ioctl(struct drm_device *dev, void *data, struct drm_file *file)
{
    struct drm_prime_handle *args = data;
    struct drm_gem_object *obj;
    struct dma_buf *buf;
    int32_t fd, rc;

    if (args->flags & ~(DRM_CLOEXEC | DRM_RDWR)) {
        return -EINVAL;
    }
    if (!drm_core_check_feature(dev, DRIVER_GEM)) {
        return -EOPNOTSUPP;
    }
    obj = drm_gem_object_lookup(file, args->handle);
    if (!obj) {
        return -ENOENT;
    }
    /* A second turn takes the cache that another export set meanwhile. */
    while ((buf = drm_handle_prime_get(&file->objects, args->handle)) == NULL) {
        if (obj->no_export) {
            drm_gem_object_put(obj);
            return -EINVAL;
        }
        buf = prime_export(obj, args->flags);
        if (!buf) {
            drm_gem_object_put(obj);
            return -ENOMEM;
        }
        rc = drm_handle_set_prime(&file->objects, args->handle, obj, dma_buf_ref(buf));
        if (rc == 0) {
            break;
        }
        /* The set's reference, then the caller's; for a new export the
         * last, whose put releases it. */
        dma_buf_put(buf);
        dma_buf_put(buf);
        if (rc != -EEXIST) {
            drm_gem_object_put(obj);
            return rc;
        }
    }
    drm_gem_object_put(obj);
    fd = dma_buf_fd(buf, args->flags & DRM_CLOEXEC);
    dma_buf_put(buf);
    if (fd < 0) {
        return fd;
    }
    args->fd = fd;
    return 0;
}

/** Import shared pages, keeping one buffer reference and its reservation.
 * A self-import returns the original object. Return one object reference,
 * or NULL with *err set. */
static struct drm_gem_object *prime_import(struct drm_device *dev, struct dma_buf *buf,
                                           int32_t *err)
{
    struct drm_gem_object *obj;
    struct drm_gem_shmem_object *s;

    if (buf->ops == &prime_ops) {
        obj = buf->priv;
        if (obj->dev->unplugged) {
            *err = -ENODEV;
            return NULL;
        }
        if (obj->dev == dev) {
            drm_gem_object_get(obj);
            return obj;
        }
    }
    if (dev->driver->gem_prime_import) {
        obj = dev->driver->gem_prime_import(dev, buf, err);
        if (!obj) {
            return NULL;
        }
    } else {
        if (buf->ops == &prime_ops) {
            obj = buf->priv;
            if (!obj->funcs || !obj->funcs->page) {
                *err = -EINVAL;
                return NULL;
            }
        }
        if (!buf->ops->page || (buf->size & (PAGE_SIZE - 1)) || buf->size > SHMEM_MAX_SIZE) {
            *err = -EINVAL;
            return NULL;
        }
        s = drm_gem_shmem_create(dev, buf->size, (buf->map_flags & VM_PROT_WC) != 0);
        if (!s) {
            *err = -ENOMEM;
            return NULL;
        }
        obj = &s->base;
    }
    obj->import = dma_buf_ref(buf);
    obj->resv = buf->resv;
    return obj;
}

/** Import a dma-buf into this file, reusing an existing cached handle.
 * Linux ignores flags on import. Return 0, -EBADF for no descriptor,
 * -EINVAL for another descriptor type, or an allocation/import error. */
int32_t drm_prime_fd_to_handle_ioctl(struct drm_device *dev, void *data, struct drm_file *file)
{
    struct drm_prime_handle *args = data;
    struct drm_gem_object *obj;
    struct dma_buf *buf;
    uint32_t handle;
    int32_t rc = -EINVAL;

    if (!drm_core_check_feature(dev, DRIVER_GEM)) {
        return -EOPNOTSUPP;
    }
    buf = dma_buf_get(args->fd);
    if (!buf) {
        return current_proc && fd_get(current_proc->fds, args->fd) ? -EINVAL : -EBADF;
    }
    handle = prime_cached(file, buf);
    if (handle) {
        args->handle = handle;
        dma_buf_put(buf);
        return 0;
    }
    obj = prime_import(dev, buf, &rc);
    if (obj) {
        /* A driver import may sleep while another thread imports this fd. */
        handle = prime_cached(file, buf);
        if (handle) {
            args->handle = handle;
            drm_gem_object_put(obj);
            dma_buf_put(buf);
            return 0;
        }
        if (dev->unplugged) {
            drm_gem_object_put(obj);
            dma_buf_put(buf);
            return -ENODEV;
        }
        rc = drm_gem_handle_create(file, obj, &handle);
        if (!rc) {
            rc = drm_handle_set_prime(&file->objects, handle, obj, dma_buf_ref(buf));
            if (rc) {
                dma_buf_put(buf);
                drm_gem_handle_delete(file, handle);
            } else {
                args->handle = handle;
            }
        }
        drm_gem_object_put(obj);
    }
    dma_buf_put(buf);
    return rc;
}
