/*
 * vgem.c: Linux's vgem, a device of shmem buffers with no hardware, which
 * IGT's tests export to another driver and hold busy with fences of their
 * own (vgem_drv.c and vgem_fence.c). It registers at DRM startup.
 */

#include "drm.h"

#include "../dmabuf/dmabuf.h"
#include "../klog.h"
#include "../lib/kdef.h"
#include "../lib/kerrno.h"
#include "../lib/kstring.h"
#include "../mm/kheap.h"
#include "uapi.h"

/* How long a fence may stay unsignalled before it signals by itself,
 * Linux's VGEM_FENCE_TIMEOUT (10 * HZ). */
#define VGEM_FENCE_TIMEOUT_MS 10000u

/*
 * A fence of VGEM_FENCE_ATTACH. `work` signals it once its time is up; while
 * the work waits it holds a reference of its own, so the fence outlives it.
 */
struct vgem_fence {
    struct dma_fence base; /* first, so that kfree of the fence frees this */
    struct drm_work work;
};

/*
 * An open file's fences, Linux's fence_idr: fence `id` is `fences[id - 1]`,
 * with a reference, until VGEM_FENCE_SIGNAL consumes it and leaves NULL. An
 * id is never given twice, since Linux's idr_replace keeps the slot. `lock`
 * keeps the table, as Linux's fence_mutex, since the two ioctls run without
 * the big lock (DRM_UNLOCKED), as Linux's do.
 */
struct vgem_file {
    struct mutex lock;
    struct dma_fence **fences;
    uint32_t count, cap;
};

/* The device, once `vgem_probe` has registered it. */
static struct drm_device *vgem_dev;

/** The fence's driver, as SYNC_IOC_FILE_INFO names it. */
static const char *vgem_fence_driver_name(struct dma_fence *fence)
{
    (void)fence;
    return "vgem";
}

/** The fence's timeline, Linux's "unbound", since each is its own. */
static const char *vgem_fence_timeline_name(struct dma_fence *fence)
{
    (void)fence;
    return "unbound";
}

static const struct dma_fence_ops vgem_fence_ops = {
    vgem_fence_driver_name, vgem_fence_timeline_name, NULL, NULL, NULL, NULL, 0};

/** The fence's time is up: it signals, as Linux's vgem_fence_timeout, and
 * the work's reference goes. Run by the commit worker. */
static void vgem_fence_timeout(struct drm_work *work)
{
    struct vgem_fence *f =
        (struct vgem_fence *)(void *)((char *)work - offsetof(struct vgem_fence, work));

    (void)dma_fence_signal(&f->base);
    dma_fence_put(&f->base);
}

/** A new unsignalled fence on a timeline of its own, which signals by
 * itself VGEM_FENCE_TIMEOUT_MS from now, as Linux's vgem_fence_create; one
 * reference, the caller's, or NULL with no memory. */
static struct dma_fence *vgem_fence_create(struct drm_device *dev)
{
    struct vgem_fence *f = (struct vgem_fence *)kzalloc(sizeof(*f));

    if (!f) {
        return NULL;
    }
    dma_fence_init(&f->base, &vgem_fence_ops, dma_fence_context_alloc(1), 1);
    f->work.fn = vgem_fence_timeout;
    (void)dma_fence_get(&f->base);
    drm_dev_work_later_ms(dev, &f->work, VGEM_FENCE_TIMEOUT_MS);
    return &f->base;
}

/** Signal `fence` and drop its timer's reference with the timer, for a
 * fence its owner signals or leaves; a timer the worker has taken to run
 * drops its reference itself. Process context. */
static void vgem_fence_end(struct dma_fence *fence)
{
    struct vgem_fence *f = (struct vgem_fence *)fence;

    (void)dma_fence_signal(fence);
    if (drm_dev_work_cancel(&f->work)) {
        dma_fence_put(fence);
    }
}

/** Give `fence` the file's next id, taking the caller's reference: the id,
 * or -ENOMEM. The file's `lock` is held. */
static int32_t vgem_fence_record(struct vgem_file *vf, struct dma_fence *fence)
{
    struct dma_fence **grown;
    uint32_t cap;

    if (vf->count == 0x7fffffffu) {
        return -ENOMEM;
    }
    if (vf->count == vf->cap) {
        cap = vf->cap ? vf->cap * 2u : 16u;
        if (cap > 0x7fffffffu || (uint64_t)cap * sizeof(*grown) > 0x7fffffffu) {
            cap = 0x7fffffffu / (uint32_t)sizeof(*grown);
        }
        if (cap <= vf->count) {
            return -ENOMEM;
        }
        grown = (struct dma_fence **)kzalloc(cap * sizeof(*grown));
        if (!grown) {
            return -ENOMEM;
        }
        if (vf->fences) {
            k_memcpy(grown, vf->fences, vf->count * sizeof(*grown));
            kfree(vf->fences);
        }
        vf->fences = grown;
        vf->cap = cap;
    }
    vf->fences[vf->count++] = fence;
    return (int32_t)vf->count;
}

/**
 * VGEM_FENCE_ATTACH, Linux's vgem_fence_attach_ioctl: a new fence on the
 * buffer's reservation, its writer with VGEM_FENCE_WRITE and a reader
 * without, so that the importers of its dma-buf wait for it; it must be
 * signalled within VGEM_FENCE_TIMEOUT_MS, or it signals by itself. 0,
 * -EINVAL for other flags or a pad, -ENOENT for no such handle, -EBUSY when
 * a fence it would wait for is pending (any for a writer, a writer for a
 * reader), or -ENOMEM.
 */
static int32_t vgem_fence_attach_ioctl(struct drm_device *dev, void *data, struct drm_file *file)
{
    struct drm_vgem_fence_attach *arg = (struct drm_vgem_fence_attach *)data;
    struct vgem_file *vf = (struct vgem_file *)file->driver_priv;
    int write = (arg->flags & VGEM_FENCE_WRITE) != 0;
    struct drm_gem_object *obj;
    struct dma_fence *fence, *busy;
    int32_t ret;

    if ((arg->flags & ~(uint32_t)VGEM_FENCE_WRITE) || arg->pad) {
        return -EINVAL;
    }
    obj = drm_gem_object_lookup(file, arg->handle);
    if (!obj) {
        return -ENOENT;
    }
    fence = vgem_fence_create(dev);
    if (!fence) {
        drm_gem_object_put(obj);
        return -ENOMEM;
    }
    /* The add under the reservation's lock, as Linux's
     * vgem_fence_attach_ioctl (vgem_fence.c:162-167), with the test too, so
     * that no fence is added between them. */
    dma_resv_lock(obj->resv);
    busy = dma_resv_snapshot_locked(obj->resv, write);
    if (!busy) {
        ret = -ENOMEM;
    } else if (!dma_fence_is_signaled(busy)) {
        ret = -EBUSY;
    } else {
        ret = dma_resv_add_fence_locked(obj->resv, fence, write);
    }
    dma_resv_unlock(obj->resv);
    dma_fence_put(busy);
    if (ret == 0) {
        mutex_lock(&vf->lock);
        ret = vgem_fence_record(vf, fence);
        mutex_unlock(&vf->lock);
        if (ret > 0) {
            arg->out_fence = (uint32_t)ret;
            ret = 0;
        }
    }
    if (ret != 0) {
        vgem_fence_end(fence);
        dma_fence_put(fence);
    }
    drm_gem_object_put(obj);
    return ret;
}

/**
 * VGEM_FENCE_SIGNAL, Linux's vgem_fence_signal_ioctl: signal and consume a
 * fence of VGEM_FENCE_ATTACH. 0, -EINVAL for flags, -ENOENT for an id this
 * file was never given or one already consumed, or -ETIMEDOUT when the
 * fence had signalled by itself.
 */
static int32_t vgem_fence_signal_ioctl(struct drm_device *dev, void *data, struct drm_file *file)
{
    struct drm_vgem_fence_signal *arg = (struct drm_vgem_fence_signal *)data;
    struct vgem_file *vf = (struct vgem_file *)file->driver_priv;
    struct dma_fence *fence;
    int32_t ret = 0;

    (void)dev;
    if (arg->flags) {
        return -EINVAL;
    }
    mutex_lock(&vf->lock);
    if (arg->fence == 0 || arg->fence > vf->count || !vf->fences[arg->fence - 1]) {
        mutex_unlock(&vf->lock);
        return -ENOENT;
    }
    fence = vf->fences[arg->fence - 1];
    vf->fences[arg->fence - 1] = NULL;
    mutex_unlock(&vf->lock);
    if (dma_fence_is_signaled(fence)) {
        ret = -ETIMEDOUT;
    }
    vgem_fence_end(fence);
    dma_fence_put(fence);
    return ret;
}

/** The driver's `open`: an empty fence table. 0 or -ENOMEM. */
static int32_t vgem_open(struct drm_device *dev, struct drm_file *file)
{
    struct vgem_file *vf = (struct vgem_file *)kzalloc(sizeof(*vf));

    (void)dev;
    if (!vf) {
        return -ENOMEM;
    }
    mutex_init(&vf->lock, "vgem_fence_lock");
    file->driver_priv = vf;
    return 0;
}

/** The driver's `postclose`: every fence the file still holds signalled and
 * let go, as Linux's vgem_fence_close, and the table freed. */
static void vgem_postclose(struct drm_device *dev, struct drm_file *file)
{
    struct vgem_file *vf = (struct vgem_file *)file->driver_priv;
    uint32_t i;

    (void)dev;
    if (!vf) {
        return;
    }
    for (i = 0; i < vf->count; i++) {
        if (vf->fences[i]) {
            vgem_fence_end(vf->fences[i]);
            dma_fence_put(vf->fences[i]);
        }
    }
    kfree(vf->fences);
    kfree(vf);
    file->driver_priv = NULL;
}

/**
 * The driver's `dumb_create`, Linux's drm_gem_shmem_dumb_create on an
 * object its vgem_gem_create_object maps write-combined, since vgem has no
 * call that would bracket a processor's access for an importer.
 */
static int32_t vgem_dumb_create(struct drm_file *file, struct drm_device *dev,
                                struct drm_mode_create_dumb *args)
{
    return drm_gem_shmem_dumb_create_wc(file, dev, args);
}

static const struct drm_ioctl_desc vgem_ioctls[] = {
    {DRM_IOCTL_VGEM_FENCE_ATTACH, DRM_RENDER_ALLOW | DRM_UNLOCKED, vgem_fence_attach_ioctl,
     "VGEM_FENCE_ATTACH"},
    {DRM_IOCTL_VGEM_FENCE_SIGNAL, DRM_RENDER_ALLOW | DRM_UNLOCKED, vgem_fence_signal_ioctl,
     "VGEM_FENCE_SIGNAL"}};

/** The driver's `release`, after a failed registration. */
static void vgem_release(struct drm_device *dev)
{
    kfree(dev);
}

static const struct drm_driver vgem_driver = {DRM_IDENT_VGEM,   vgem_open, vgem_postclose,
                                              vgem_dumb_create, NULL,      NULL,
                                              vgem_ioctls,      2,         vgem_release};

int32_t vgem_probe(void)
{
    struct drm_device *dev;
    int32_t err;

    if (vgem_dev) {
        return -EBUSY;
    }
    dev = (struct drm_device *)kzalloc(sizeof(*dev));
    if (!dev) {
        return -ENOMEM;
    }
    drm_dev_init(dev, &vgem_driver);
    err = drm_dev_register(dev);
    if (err != 0) {
        klog(KLOG_ERR, "vgem", "register: error %d", (int)err);
        drm_dev_put(dev);
        return err;
    }
    vgem_dev = dev;
    klog(KLOG_INFO, "vgem", "registered: vgem, card %d", dev->index);
    return 1;
}
