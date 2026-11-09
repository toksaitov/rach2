/* virtio_gpu_fence.c: the control queue's fences, waits and device loss. */
#include "virtio_gpu.h"
#include "../dmabuf/dmabuf.h"
#include "../fs/debugfs.h"
#include "../lib/kerrno.h"
#include "../lib/kstring.h"
#include "../mm/usercopy.h"

/** A fence's Linux driver name. */
static const char *fence_driver(struct dma_fence *f)
{
    (void)f;
    return "virtio_gpu";
}

/** A fence's Linux timeline name. */
static const char *fence_timeline(struct dma_fence *f)
{
    (void)f;
    return "controlq";
}

static const struct dma_fence_ops vgpu_fence_ops = {fence_driver, fence_timeline, NULL, NULL};

/**
 * Initialize a command's fence once before reservation or descriptor
 * publication.
 */
void vgpu_cmd_prepare(struct virtio_gpu_cmd *cmd)
{
    struct virtio_gpu_ctrl_hdr *hdr = (void *)cmd->page;
    struct virtio_gpu_device *vg = cmd->vg;
    uint32_t flags = spin_lock_irqsave(&vg->lock);

    if (cmd->fence && !cmd->prepared) {
        dma_fence_init(cmd->fence, &vgpu_fence_ops, vg->fence_context, ++vg->fence_seq);
        hdr->flags |= VIRTIO_GPU_FLAG_FENCE;
        hdr->fence_id = vg->fence_seq;
        cmd->prepared = 1;
    }
    spin_unlock_irqrestore(&vg->lock, flags);
}

/**
 * Wait interruptibly for foreign fence leaves; queue order covers this
 * device's leaves.
 */
int32_t vgpu_wait_dependency(struct virtio_gpu_device *vg, struct dma_fence *fence)
{
    struct dma_fence *leaf, *stable;
    uint32_t i, n;
    int32_t rc = 0;

    stable = dma_fence_unwrap_merge(&fence, 1);
    if (!stable) {
        return -ENOMEM;
    }
    n = dma_fence_count(stable);
    for (i = 0; i < n; i++) {
        leaf = dma_fence_at(stable, i);
        if (leaf && leaf->context != vg->fence_context) {
            rc = dma_fence_wait_until(leaf, 1, 0x7fffffffffffffffll);
            if (rc) {
                break;
            }
        }
    }
    dma_fence_put(stable);
    return rc ? rc : (vg->dead ? -ENODEV : 0);
}

/**
 * Implement Linux's buffer WAIT, with EBUSY for pending NOWAIT or a
 * 15-second expiry.
 */
int32_t vgpu_wait_ioctl(struct drm_device *dev, void *data, struct drm_file *file)
{
    struct virtio_gpu_device *vg = vgpu_device(dev);
    struct drm_virtgpu_3d_wait *a = data;
    struct drm_gem_object *obj;
    struct dma_fence *f;
    int32_t rc;

    if (vg->dead) {
        return -ENODEV;
    }
    vgpu_reap(vg);
    obj = drm_gem_object_lookup(file, a->handle);
    if (!obj) {
        return -ENOENT;
    }
    f = dma_resv_snapshot(obj->resv, 1);
    drm_gem_object_put(obj);
    if (!f) {
        return -ENOMEM;
    }
    if (a->flags & VIRTGPU_WAIT_NOWAIT) {
        rc = dma_fence_is_signaled(f) ? 0 : -EBUSY;
    } else {
        rc = dma_fence_wait_until(f, 1, dma_fence_now() + 15000000000ll);
        if (rc == -ETIME) {
            rc = -EBUSY;
        }
    }
    dma_fence_put(f);
    return rc;
}

/** Hold the device for the lifetime of an open reset descriptor. */
static int32_t reset_open(fd_entry_t *e, int flags)
{
    struct virtio_gpu_device *vg = e->priv;

    (void)flags;
    if (vg->dead) {
        return -ENODEV;
    }
    drm_dev_get(&vg->drm);
    return 0;
}

/** Drop the reset descriptor's device reference. */
static void reset_close(fd_entry_t *e)
{
    struct virtio_gpu_device *vg = e->priv;

    drm_dev_put(&vg->drm);
}

/** A write of 1 unplugs the device, revokes mappings and stops all DMA.
 * Every old descriptor then returns ENODEV. The descriptor holds the device. */
static int32_t reset_write(fd_entry_t *e, const char __user *buf, uint32_t count)
{
    struct virtio_gpu_device *vg = e->priv;
    char text[2];

    if (vg->dead) {
        return -ENODEV;
    }
    if (count != 1 && count != 2) {
        return -EINVAL;
    }
    if (copy_from_user(text, buf, count)) {
        return -EFAULT;
    }
    if (text[0] != '1' || (count == 2 && text[1] != '\n')) {
        return -EINVAL;
    }
    drm_dev_unplug(&vg->drm);
    return (int32_t)count;
}

static const struct file_ops reset_ops = {NULL, reset_write, NULL, NULL, reset_close, NULL, NULL};

/** Register the root-only reset trigger in the device's debugfs directory,
 * which `dri/N` links to; return 0 or the debugfs error. */
int32_t vgpu_reset_register(struct virtio_gpu_device *vg)
{
    if (!vg->has_virgl) {
        return 0;
    }
    drm_debugfs_path(&vg->drm, "virtio_gpu_reset", vg->reset_path, sizeof(vg->reset_path));
    return debugfs_create_file(vg->reset_path, 0200u, &reset_ops, reset_open, vg);
}

/** Remove the reset trigger; open descriptors retain their device reference. */
void vgpu_reset_remove(struct virtio_gpu_device *vg)
{
    if (vg->reset_path[0]) {
        debugfs_remove(vg->reset_path);
        vg->reset_path[0] = 0;
    }
}
