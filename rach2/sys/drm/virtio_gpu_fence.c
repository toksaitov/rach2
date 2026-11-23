/* virtio_gpu_fence.c: the control queue's fences, waits and device loss. */
#include "virtio_gpu.h"
#include "../dmabuf/dmabuf.h"
#include "../fs/debugfs.h"
#include "../lib/kerrno.h"
#include "../lib/kstring.h"
#include "../mm/usercopy.h"
#include "../proc/smp.h"
#ifdef RACH2_VGPU_PROFILE
#include "../rtc.h"
#endif

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

static const struct dma_fence_ops vgpu_fence_ops = {fence_driver, fence_timeline, NULL, NULL,
                                                    NULL,         NULL,           0};

/**
 * Initialize a command's fence once before reservation or descriptor
 * publication: the next id of the device's, on the command's fence
 * context, and with a ring, VIRTIO_GPU_FLAG_INFO_RING_IDX and the ring's
 * index, as Linux's virtio_gpu_fence_emit.
 */
void vgpu_cmd_prepare(struct virtio_gpu_cmd *cmd)
{
    struct virtio_gpu_ctrl_hdr *hdr = (void *)cmd->page;
    struct virtio_gpu_device *vg = cmd->vg;
    uint32_t flags = spin_lock_irqsave(&vg->lock);

    if (cmd->fence && !cmd->prepared) {
        dma_fence_init(cmd->fence, &vgpu_fence_ops,
                       cmd->fence_ctx ? cmd->fence_ctx : vg->fence_context, ++vg->fence_seq);
        hdr->flags |= VIRTIO_GPU_FLAG_FENCE;
        hdr->fence_id = vg->fence_seq;
        if (cmd->ring) {
            hdr->flags |= VIRTIO_GPU_FLAG_INFO_RING_IDX;
            hdr->ring_idx = (uint8_t)cmd->ring_idx;
        }
        cmd->prepared = 1;
    }
    spin_unlock_irqrestore(&vg->lock, flags);
}

/**
 * Wait interruptibly for each leaf of `fence` on a fence context other than
 * `context`, as Linux's virtio_gpu_dma_fence_wait and
 * virtio_gpu_do_fence_wait: the device keeps the order of one context's
 * fences, those of the control queue or of one ring, and of nothing across
 * them. The walk holds the point it is at, so the leaf stays across the
 * sleep and a leaf that signals meanwhile moves no other out of the walk.
 */
int32_t vgpu_wait_dependency(struct virtio_gpu_device *vg, struct dma_fence *fence,
                             uint64_t context)
{
    struct dma_fence_unwrap u;
    struct dma_fence *leaf;
    int32_t rc = 0;

    for (leaf = dma_fence_unwrap_first(fence, &u); leaf; leaf = dma_fence_unwrap_next(&u)) {
        if (leaf->context != context) {
            rc = dma_fence_wait_until(leaf, 1, 0x7fffffffffffffffll);
            if (rc) {
                break;
            }
        }
    }
    dma_fence_unwrap_end(&u);
    return rc ? rc : (vg->dead ? -ENODEV : 0);
}

/** Drop a reference a handler without the big lock took on `obj`: the last
 * one frees the object, which needs that lock (`vgpu_obj_free`). */
static void obj_put_unlocked(struct drm_gem_object *obj)
{
    if (drm_gem_object_put_unless_last(obj)) {
        return;
    }
    bkl_enter();
    drm_gem_object_put(obj);
    bkl_exit();
}

/**
 * VIRTGPU_WAIT with VIRTGPU_WAIT_NOWAIT, without the big lock: whether the
 * object's fences have signalled, from the snapshot its reservation's lock
 * gives and each fence's mark, as Linux's virtio_gpu_wait_ioctl tests them
 * (dma_resv_test_signaled). The answers' interrupts signal the fences, so
 * nothing is reaped. 0, -EBUSY or -ENOMEM.
 */
static int32_t wait_nowait(struct drm_gem_object *obj)
{
    struct dma_fence *f = dma_resv_snapshot(obj->resv, 1);
    int32_t rc;

    if (!f) {
        return -ENOMEM;
    }
    rc = dma_fence_is_signaled(f) ? 0 : -EBUSY;
    dma_fence_put(f);
    return rc;
}

/**
 * Implement Linux's buffer WAIT, with EBUSY for pending NOWAIT or a
 * 15-second expiry. The row runs without the big lock: a NOWAIT on a
 * device whose answers come by interrupt tests the fences alone
 * (`wait_nowait`), and every other wait takes the lock, reaps the queues,
 * which answers a device without interrupts, and sleeps on the fence.
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
    if ((a->flags & VIRTGPU_WAIT_NOWAIT) && !vg->vdev->polled) {
        obj = drm_gem_object_lookup(file, a->handle);
        if (!obj) {
            return -ENOENT;
        }
        rc = wait_nowait(obj);
        obj_put_unlocked(obj);
        return rc;
    }
    bkl_enter();
    vgpu_reap(vg);
    obj = drm_gem_object_lookup(file, a->handle);
    if (!obj) {
        bkl_exit();
        return -ENOENT;
    }
    f = dma_resv_snapshot(obj->resv, 1);
    drm_gem_object_put(obj);
    if (!f) {
        bkl_exit();
        return -ENOMEM;
    }
    if (a->flags & VIRTGPU_WAIT_NOWAIT) {
        rc = dma_fence_is_signaled(f) ? 0 : -EBUSY;
    } else {
#ifdef RACH2_VGPU_PROFILE
        int64_t start = rtc_monotonic_ns();
        int busy = !dma_fence_is_signaled(f);
#endif
        rc = dma_fence_wait_until(f, 1, dma_fence_now() + 15000000000ll);
        if (rc == -ETIME) {
            rc = -EBUSY;
        }
#ifdef RACH2_VGPU_PROFILE
        if (busy) {
            vgpu_profile_event(vg, VGPU_EVENT_WAIT, start, 0);
        }
#endif
    }
    dma_fence_put(f);
    bkl_exit();
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

static const struct file_ops reset_ops = {NULL,        reset_write, NULL, NULL,
                                          reset_close, NULL,        NULL, 0};

/** Hold the device's answers while the descriptor is open, as
 * `vgpu_answers_held` says; refused once the device has died. */
static int32_t hold_open(fd_entry_t *e, int flags)
{
    struct virtio_gpu_device *vg = e->priv;

    (void)flags;
    if (vg->dead) {
        return -ENODEV;
    }
    drm_dev_get(&vg->drm);
    vg->answer_holds++;
    return 0;
}

/** Let the answers go at the last close of the hold, and run their
 * completions at once: the commands' references drop, and an object whose
 * last reference an answer held goes to the worker. */
static void hold_close(fd_entry_t *e)
{
    struct virtio_gpu_device *vg = e->priv;

    vg->answer_holds--;
    vgpu_reap(vg);
    drm_dev_put(&vg->drm);
}

static const struct file_ops hold_ops = {NULL, NULL, NULL, NULL, hold_close, NULL, NULL, 0};

/** Append the decimal `n` and then `end` to the text at `buf`, of `cap`
 * bytes, whose length is `len`, as far as they go; the new length. */
static uint32_t put_number(char *buf, uint32_t cap, uint32_t len, uint32_t n, char end)
{
    char digits[11];
    uint32_t i = 0;

    do {
        digits[i++] = (char)('0' + n % 10u);
        n /= 10u;
    } while (n);
    while (i) {
        if (len < cap) {
            buf[len] = digits[i - 1u];
        }
        len++;
        i--;
    }
    if (len < cap) {
        buf[len] = end;
    }
    return len + 1u;
}

/** The text of virtio_gpu_attachments: a line "CTX RES" for each resource
 * attached to a context, from CTX_ATTACH_RESOURCE to the DETACH, UNREF or
 * CTX_DESTROY that drops it (`struct vgpu_attachment`), each context's
 * newest first. */
static int32_t attachments_show(void *data, char *buf, uint32_t cap)
{
    const struct virtio_gpu_device *vg = data;
    const struct vgpu_context *ctx;
    const struct vgpu_attachment *a;
    uint32_t len = 0;

    for (ctx = vg->contexts; ctx; ctx = ctx->next) {
        for (a = ctx->attached; a; a = a->ctx_next) {
            len = put_number(buf, cap, len, ctx->id, ' ');
            len = put_number(buf, cap, len, a->bo->res_id, '\n');
        }
    }
    return (int32_t)len;
}

static const struct debugfs_text_ops attachments_ops = {attachments_show, NULL};

/** Register the root-only reset trigger, the answers' hold and the list of
 * attachments in the device's debugfs directory, which `dri/N` links to;
 * return 0 or the debugfs error. */
int32_t vgpu_reset_register(struct virtio_gpu_device *vg)
{
    int32_t rc;

    if (!vg->has_virgl) {
        return 0;
    }
    drm_debugfs_path(&vg->drm, "virtio_gpu_reset", vg->reset_path, sizeof(vg->reset_path));
    rc = debugfs_create_file(vg->reset_path, 0200u, &reset_ops, reset_open, vg);
    if (rc) {
        vg->reset_path[0] = 0;
        return rc;
    }
    drm_debugfs_path(&vg->drm, "virtio_gpu_hold", vg->hold_path, sizeof(vg->hold_path));
    rc = debugfs_create_file(vg->hold_path, 0200u, &hold_ops, hold_open, vg);
    if (rc) {
        vg->hold_path[0] = 0;
        return rc;
    }
    drm_debugfs_path(&vg->drm, "virtio_gpu_attachments", vg->attach_path, sizeof(vg->attach_path));
    rc = debugfs_create_text(vg->attach_path, 0400u, &attachments_ops, vg);
    if (rc) {
        vg->attach_path[0] = 0;
    }
    return rc;
}

/** Remove the device's debugfs files; open descriptors retain their device
 * reference. */
void vgpu_reset_remove(struct virtio_gpu_device *vg)
{
    char *paths[3];
    int i;

    paths[0] = vg->reset_path;
    paths[1] = vg->hold_path;
    paths[2] = vg->attach_path;
    for (i = 0; i < 3; i++) {
        if (paths[i][0]) {
            debugfs_remove(paths[i]);
            paths[i][0] = 0;
        }
    }
}
