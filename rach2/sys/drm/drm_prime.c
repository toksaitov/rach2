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

/** Clear the weak export pointer before releasing the exporter's object. */
static void prime_release(struct dma_buf *buf)
{
    struct drm_gem_object *obj = buf->priv;

    obj->dma_buf = NULL;
    drm_gem_object_put(obj);
}

/** Give an importer a borrowed frame, or 0 after device loss or failure. */
static uintptr_t prime_page(struct dma_buf *buf, uint32_t pgoff)
{
    struct drm_gem_object *obj = buf->priv;

    if (obj->dev->unplugged || (uint64_t)pgoff >= obj->size >> PAGE_SHIFT || !obj->funcs ||
        !obj->funcs->page) {
        return 0;
    }
    return obj->funcs->page(obj, pgoff);
}

static const struct dma_buf_ops prime_ops = {prime_mmap, prime_release, NULL, prime_page};

/** Find a GEM handle's cache entry; the big kernel lock protects the table. */
static struct drm_handle *prime_handle(struct drm_file *file, uint32_t handle)
{
    struct drm_handle *h;

    for (h = file->objects; h; h = h->next) {
        if (h->id == handle) {
            return h;
        }
    }
    return NULL;
}

/** Export a handle as a dma-buf descriptor, caching its identity until close.
 * Flags accept only DRM_CLOEXEC and DRM_RDWR. Return 0 or -errno. */
int32_t drm_prime_handle_to_fd_ioctl(struct drm_device *dev, void *data, struct drm_file *file)
{
    struct drm_prime_handle *args = data;
    struct drm_handle *h;
    struct drm_gem_object *obj;
    struct dma_buf *buf;
    int32_t fd;

    if (args->flags & ~(DRM_CLOEXEC | DRM_RDWR)) {
        return -EINVAL;
    }
    if (!drm_core_check_feature(dev, DRIVER_GEM)) {
        return -EOPNOTSUPP;
    }
    h = prime_handle(file, args->handle);
    if (!h) {
        return -ENOENT;
    }
    obj = h->object;
    if (!h->prime) {
        buf = obj->import ? obj->import : obj->dma_buf;
        if (buf) {
            h->prime = dma_buf_ref(buf);
        } else {
            buf = dma_buf_export(obj->size, &prime_ops, obj, obj->resv);
            if (!buf) {
                return -ENOMEM;
            }
            drm_gem_object_get(obj);
            obj->dma_buf = buf;
            buf->read_only = !(args->flags & DRM_RDWR);
            buf->map_flags = obj->map_wc ? VM_PROT_WC : 0;
            h->prime = buf;
        }
    }
    fd = dma_buf_fd(h->prime, args->flags & DRM_CLOEXEC);
    if (fd < 0) {
        return fd;
    }
    args->fd = fd;
    return 0;
}

/** Import shared pages, keeping one buffer reference and its reservation.
 * A self-import returns the original object. Return one object reference,
 * or NULL with *err set; the caller holds the big kernel lock. */
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
    struct drm_handle *h;
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
    for (h = file->objects; h; h = h->next) {
        if (h->prime == buf) {
            args->handle = h->id;
            dma_buf_put(buf);
            return 0;
        }
    }
    obj = prime_import(dev, buf, &rc);
    if (obj) {
        /* A driver import may sleep while another thread imports this fd. */
        for (h = file->objects; h; h = h->next) {
            if (h->prime == buf) {
                args->handle = h->id;
                drm_gem_object_put(obj);
                dma_buf_put(buf);
                return 0;
            }
        }
        if (dev->unplugged) {
            drm_gem_object_put(obj);
            dma_buf_put(buf);
            return -ENODEV;
        }
        rc = drm_gem_handle_create(file, obj, &handle);
        if (!rc) {
            h = prime_handle(file, handle);
            h->prime = dma_buf_ref(buf);
            args->handle = handle;
        }
        drm_gem_object_put(obj);
    }
    dma_buf_put(buf);
    return rc;
}
