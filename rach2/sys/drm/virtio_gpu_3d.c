/* virtio_gpu_3d.c: virgl capability sets, per-file contexts and transfers. */
#include "virtio_gpu.h"
#include "../dmabuf/dmabuf.h"
#include "../lib/kerrno.h"
#include "../lib/kstring.h"
#include "../mm/kheap.h"
#include "../mm/usercopy.h"

/** Allocate payload pages and their scatter list before submitting a command.
 * The pages are one block that the device sees as one segment where the
 * transport gives more than a page at once (`dma_max`), and a page each
 * otherwise; either way each page is freed alone. The virtqueue's
 * descriptor count bounds a single chain, including its header and
 * response; larger commands return E2BIG. Process context. */
int32_t vgpu_cmd_payload(struct virtio_gpu_cmd *cmd, uint64_t user, uint32_t bytes, int response)
{
    uint32_t n, i, left = bytes, off = 0, here;
    struct virtio_sg *sg;
    struct virtio_dev *vdev = cmd->vg->vdev;
    uint8_t *block = NULL;
    uint64_t addr = 0;

    n = bytes / PAGE_SIZE + (bytes % PAGE_SIZE != 0);
    if (!n) {
        return 0;
    }
    if (n > 1 && vdev->dma_max / PAGE_SIZE >= n) {
        block = (uint8_t *)virtio_dma_alloc(vdev, n * PAGE_SIZE, &addr);
    }
    if (!block && n > (uint32_t)cmd->vq->size - 2) {
        return -E2BIG;
    }
    cmd->payload = kzalloc(n * sizeof(*cmd->payload));
    sg = kzalloc((n + 2) * sizeof(*sg));
    if (!cmd->payload || !sg) {
        kfree(sg);
        if (block) {
            for (i = 0; i < n; i++) {
                virtio_dma_free(vdev, block + i * PAGE_SIZE, PAGE_SIZE);
            }
        }
        return -ENOMEM;
    }
    sg[0] = cmd->sg[0];
    cmd->segments = sg;
    for (i = 0; block && i < n; i++) {
        cmd->payload[i].page = block + i * PAGE_SIZE;
        cmd->payload[i].addr = addr + (uint64_t)i * PAGE_SIZE;
        cmd->npayload++;
    }
    for (i = 0; i < n; i++) {
        if (!block) {
            cmd->payload[i].page = virtio_dma_alloc(vdev, PAGE_SIZE, &cmd->payload[i].addr);
            if (!cmd->payload[i].page) {
                return -ENOMEM;
            }
            cmd->npayload++;
        }
        here = left < PAGE_SIZE ? left : PAGE_SIZE;
        if (!response && copy_from_user(cmd->payload[i].page,
                                        (const void __user *)(uintptr_t)(user + off), here)) {
            return -EFAULT;
        }
        sg[i + 1].addr = cmd->payload[i].addr;
        sg[i + 1].len = here;
        left -= here;
        off += here;
    }
    if (response) {
        cmd->response_size = bytes;
        cmd->nin = n;
    } else {
        cmd->nout = n + 1;
    }
    return 0;
}

/** Copy a completed capset payload to user space; return 0, EIO or EFAULT. */
int32_t vgpu_caps_copy(struct virtio_gpu_cmd *cmd, uint64_t user, uint32_t bytes)
{
    uint32_t off = sizeof(struct virtio_gpu_ctrl_hdr), here, page;

    if (cmd->response_len < off || bytes > cmd->response_len - off) {
        return -EIO;
    }
    while (bytes) {
        page = off / PAGE_SIZE;
        here = PAGE_SIZE - off % PAGE_SIZE;
        if (here > bytes) {
            here = bytes;
        }
        if (copy_to_user((void __user *)(uintptr_t)user,
                         (char *)cmd->payload[page].page + off % PAGE_SIZE, here)) {
            return -EFAULT;
        }
        user += here;
        off += here;
        bytes -= here;
    }
    return 0;
}

/**
 * Read every advertised capset description after DRIVER_OK; return 0 or
 * -errno.
 */
int32_t vgpu_capsets_read(struct virtio_gpu_device *vg)
{
    struct virtio_gpu_cmd *cmd;
    struct virtio_gpu_capset_info info;
    uint32_t i;
    int32_t rc;

    if (!vg->has_virgl || !vg->num_capsets) {
        vg->num_capsets = 0;
        return 0;
    }
    if ((uint64_t)vg->num_capsets * sizeof(*vg->capsets) > (uint64_t)(size_t)-1) {
        return -ENOMEM;
    }
    vg->capsets = kzalloc((size_t)vg->num_capsets * sizeof(*vg->capsets));
    if (!vg->capsets) {
        return -ENOMEM;
    }
    for (i = 0; i < vg->num_capsets; i++) {
        cmd = vgpu_cmd_new(vg, vg->ctrlq, VIRTIO_GPU_CMD_GET_CAPSET_INFO,
                           sizeof(struct virtio_gpu_get_capset_info));
        if (!cmd) {
            return -ENOMEM;
        }
        ((struct virtio_gpu_get_capset_info *)(void *)cmd->page)->index = i;
        rc = vgpu_cmd_send_wait(cmd, &info, sizeof(info));
        if (rc) {
            return rc;
        }
        if (info.hdr.type != VIRTIO_GPU_RESP_OK_CAPSET_INFO || !info.id || !info.size ||
            info.size > 0xffffffffu - sizeof(info.hdr) ||
            (info.id < 32 && (vg->capset_mask & (1u << info.id)))) {
            return -EIO;
        }
        vg->capsets[i].id = info.id;
        vg->capsets[i].version = info.version;
        vg->capsets[i].size = info.size;
        if (info.id < 32) {
            vg->capset_mask |= 1u << info.id;
        }
    }
    return 0;
}

/**
 * Read a supported capset version, copying the smaller of user and host
 * sizes.
 */
int32_t vgpu_get_caps_ioctl(struct drm_device *dev, void *data, struct drm_file *file)
{
    struct virtio_gpu_device *vg = vgpu_device(dev);
    struct drm_virtgpu_get_caps *args = data;
    struct virtio_gpu_cmd *cmd;
    struct virtio_gpu_get_capset *req;
    uint32_t i, size, copy;
    int32_t rc;

    (void)file;
    if (vg->dead) {
        return -ENODEV;
    }
    if (!vg->num_capsets) {
        return -ENOSYS;
    }
    if (!args->size) {
        return -EINVAL;
    }
    for (i = 0; i < vg->num_capsets; i++) {
        if (vg->capsets[i].id == args->cap_set_id && vg->capsets[i].version >= args->cap_set_ver) {
            break;
        }
    }
    if (i == vg->num_capsets) {
        return -EINVAL;
    }
    size = vg->capsets[i].size;
    copy = args->size < size ? args->size : size;
    cmd = vgpu_cmd_new(vg, vg->ctrlq, VIRTIO_GPU_CMD_GET_CAPSET, sizeof(*req));
    if (!cmd) {
        return -ENOMEM;
    }
    req = (struct virtio_gpu_get_capset *)(void *)cmd->page;
    req->id = args->cap_set_id;
    req->version = args->cap_set_ver;
    rc = vgpu_cmd_payload(cmd, 0, size + sizeof(req->hdr), 1);
    if (rc) {
        vgpu_cmd_free(cmd);
        return rc;
    }
    rc = vgpu_cmd_send_wait(cmd, NULL, 0);
    if (rc == -ETIMEDOUT) {
        return -EBUSY;
    }
    if (!rc) {
        rc = cmd->resp_type == VIRTIO_GPU_RESP_OK_CAPSET ? vgpu_caps_copy(cmd, args->addr, copy)
                                                         : -EIO;
    }
    vgpu_cmd_free(cmd);
    return rc;
}

/** Allocate a lazy context record for an open file; return 0 or -errno. */
int32_t vgpu_context_open(struct virtio_gpu_device *vg, struct drm_file *file)
{
    struct vgpu_context *ctx;

    if (vg->dead) {
        return -ENODEV;
    }
    if (!vg->has_virgl || file->driver_priv) {
        return 0;
    }
    ctx = kzalloc(sizeof(*ctx));
    if (!ctx) {
        return -ENOMEM;
    }
    ctx->id = 1;
    for (;;) {
        struct vgpu_context *other;
        int taken = 0;

        for (other = vg->contexts; other; other = other->next) {
            if (other->id == ctx->id) {
                taken = 1;
                break;
            }
        }
        if (!taken) {
            break;
        }
        if (++ctx->id == 0) {
            kfree(ctx);
            return -ENOSPC;
        }
    }
    ctx->next = vg->contexts;
    vg->contexts = ctx;
    file->driver_priv = ctx;
    return 0;
}

/**
 * Queue the file's CTX_CREATE once, with the capability set and the name
 * CONTEXT_INIT gave, if any, else the device's default set and "rach2";
 * return 0 or -errno without a fence wait.
 */
int32_t vgpu_context_ensure(struct virtio_gpu_device *vg, struct drm_file *file)
{
    struct vgpu_context *ctx;
    struct virtio_gpu_cmd *cmd;
    struct virtio_gpu_ctx_create *req;
    int32_t rc = vgpu_context_open(vg, file);

    if (rc) {
        return rc;
    }
    ctx = file->driver_priv;
    if (!ctx || ctx->created) {
        return 0;
    }
    cmd = vgpu_cmd_new(vg, vg->ctrlq, VIRTIO_GPU_CMD_CTX_CREATE, sizeof(*req));
    if (!cmd) {
        return -ENOMEM;
    }
    req = (struct virtio_gpu_ctx_create *)(void *)cmd->page;
    req->hdr.ctx_id = ctx->id;
    req->context_init = ctx->context_init;
    req->nlen = (uint32_t)k_strlen(ctx->debug_name[0] ? ctx->debug_name : "rach2");
    k_memcpy(req->debug_name, ctx->debug_name[0] ? ctx->debug_name : "rach2", req->nlen);
    vgpu_cmd_send(cmd, NULL, NULL);
    ctx->created = 1;
    return 0;
}

/**
 * Destroy a closing file's context in queue order and free its attachment
 * records.
 */
void vgpu_context_close(struct drm_device *dev, struct drm_file *file)
{
    struct virtio_gpu_device *vg = vgpu_device(dev);
    struct vgpu_context *ctx = file->driver_priv, **p;
    struct virtio_gpu_cmd *cmd;
    struct vgpu_attachment *a;

    if (!ctx) {
        return;
    }
    if (ctx->created && !vg->dead) {
        cmd = vgpu_cmd_new(vg, vg->ctrlq, VIRTIO_GPU_CMD_CTX_DESTROY,
                           sizeof(struct virtio_gpu_ctrl_hdr));
        if (cmd) {
            ((struct virtio_gpu_ctrl_hdr *)(void *)cmd->page)->ctx_id = ctx->id;
            vgpu_cmd_send(cmd, NULL, NULL);
        } else {
            drm_dev_unplug(&vg->drm);
        }
    }
    for (p = &vg->contexts; *p && *p != ctx; p = &(*p)->next) {
    }
    if (*p) {
        *p = ctx->next;
    }
    while ((a = ctx->attached) != NULL) {
        ctx->attached = a->next;
        kfree(a);
    }
    file->driver_priv = NULL;
    kfree(ctx);
    vgpu_reap(vg);
}

/** Attach a buffer to the context once; borrow the buffer until its detach. */
int32_t vgpu_context_attach(struct virtio_gpu_device *vg, struct drm_file *file,
                            struct virtio_gpu_object *bo)
{
    struct virtio_gpu_cmd *cmd;
    struct virtio_gpu_resource_unref *req;
    struct vgpu_context *ctx = file->driver_priv;
    struct vgpu_attachment *a;

    if (vg->dead || !bo->res_id || !ctx || !ctx->created) {
        return -ENODEV;
    }
    for (a = ctx->attached; a; a = a->next) {
        if (a->bo == bo) {
            return 0;
        }
    }
    a = kzalloc(sizeof(*a));
    if (!a) {
        return -ENOMEM;
    }
    cmd = vgpu_cmd_new(vg, vg->ctrlq, VIRTIO_GPU_CMD_CTX_ATTACH_RESOURCE, sizeof(*req));
    if (!cmd) {
        kfree(a);
        return -ENOMEM;
    }
    a->bo = bo;
    a->next = ctx->attached;
    ctx->attached = a;
    req = (struct virtio_gpu_resource_unref *)(void *)cmd->page;
    req->hdr.ctx_id = ctx->id;
    req->resource_id = bo->res_id;
    vgpu_cmd_send(cmd, NULL, NULL);
    return 0;
}

/**
 * Queue every context's detach before UNREF, or forget attachments after
 * reset.
 */
void vgpu_resource_detach(struct virtio_gpu_device *vg, struct virtio_gpu_object *bo)
{
    struct vgpu_context *ctx;
    struct vgpu_attachment **p, *a;
    struct virtio_gpu_cmd *cmd;
    struct virtio_gpu_resource_unref *req;

    for (ctx = vg->contexts; ctx; ctx = ctx->next) {
        for (p = &ctx->attached; *p;) {
            a = *p;
            if (a->bo != bo) {
                p = &a->next;
                continue;
            }
            *p = a->next;
            kfree(a);
            if (vg->dead) {
                continue;
            }
            cmd = vgpu_cmd_new(vg, vg->ctrlq, VIRTIO_GPU_CMD_CTX_DETACH_RESOURCE, sizeof(*req));
            if (!cmd) {
                drm_dev_unplug(&vg->drm);
                return;
            }
            req = (struct virtio_gpu_resource_unref *)(void *)cmd->page;
            req->hdr.ctx_id = ctx->id;
            req->resource_id = bo->res_id;
            vgpu_cmd_send(cmd, NULL, NULL);
        }
    }
}

/**
 * Release the transfer's buffer only after the device has finished using
 * it.
 */
static void transfer_done(struct virtio_gpu_cmd *cmd)
{
    drm_gem_object_put(cmd->arg);
}

/**
 * Queue an upload or readback, keeping its buffer until completion, as
 * Linux: a blob of guest memory alone has no host copy and is refused, and
 * only a blob with host memory takes a stride or a layer stride. Return 0
 * or -errno.
 */
int32_t vgpu_transfer_3d_ioctl(struct drm_device *dev, void *data, struct drm_file *file, int from)
{
    struct virtio_gpu_device *vg = vgpu_device(dev);
    struct drm_virtgpu_3d_transfer *a = data;
    struct drm_gem_object *obj;
    struct virtio_gpu_object *bo;
    struct virtio_gpu_cmd *cmd = NULL;
    struct virtio_gpu_transfer_3d *req;
    struct dma_fence *dependency;
    int32_t rc;

    rc = vgpu_context_ensure(vg, file);
    if (rc) {
        return rc;
    }
    obj = drm_gem_object_lookup(file, a->bo_handle);
    if (!obj) {
        return -ENOENT;
    }
    bo = vgpu_bo(obj);
    if (!bo || !bo->res_id || bo->blob_mem == VIRTGPU_BLOB_MEM_GUEST ||
        (bo->blob_mem != VIRTGPU_BLOB_MEM_HOST3D && bo->blob_mem != VIRTGPU_BLOB_MEM_HOST3D_GUEST &&
         (a->stride || a->layer_stride))) {
        rc = -EINVAL;
        goto out;
    }
    dependency = dma_resv_snapshot(obj->resv, 1);
    rc = dependency ? vgpu_wait_dependency(vg, dependency, vg->fence_context) : -ENOMEM;
    dma_fence_put(dependency);
    if (rc || vg->dead) {
        if (!rc) {
            rc = -ENODEV;
        }
        goto out;
    }
    rc = vgpu_context_attach(vg, file, bo);
    if (rc) {
        goto out;
    }
    cmd = vgpu_cmd_new(vg, vg->ctrlq,
                       from ? VIRTIO_GPU_CMD_TRANSFER_FROM_HOST_3D
                            : VIRTIO_GPU_CMD_TRANSFER_TO_HOST_3D,
                       sizeof(*req));
    if (!cmd) {
        rc = -ENOMEM;
        goto out;
    }
    req = (struct virtio_gpu_transfer_3d *)(void *)cmd->page;
    req->hdr.ctx_id = ((struct vgpu_context *)file->driver_priv)->id;
    req->resource_id = bo->res_id;
    req->x = a->box.x;
    req->y = a->box.y;
    req->z = a->box.z;
    req->w = a->box.w;
    req->h = a->box.h;
    req->d = a->box.d;
    req->offset = a->offset;
    req->level = a->level;
    req->stride = a->stride;
    req->layer_stride = a->layer_stride;
    vgpu_cmd_prepare(cmd);
    rc = dma_resv_add_fence(obj->resv, cmd->fence, 1);
    if (!rc) {
        vgpu_cmd_send(cmd, transfer_done, obj);
        return 0;
    }
    dma_fence_set_error(cmd->fence, rc);
    dma_fence_signal(cmd->fence);
    vgpu_cmd_free(cmd);
out:
    drm_gem_object_put(obj);
    return rc;
}
