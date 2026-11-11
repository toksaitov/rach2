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

/** Copy a completed capset payload into `to`; return 0 or EIO. */
int32_t vgpu_caps_copy(struct virtio_gpu_cmd *cmd, unsigned char *to, uint32_t bytes)
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
        k_memcpy(to, (char *)cmd->payload[page].page + off % PAGE_SIZE, here);
        to += here;
        off += here;
        bytes -= here;
    }
    return 0;
}

void vgpu_caps_cache_free(struct virtio_gpu_device *vg)
{
    struct vgpu_caps_cache *c;

    while ((c = vg->caps_cache) != NULL) {
        vg->caps_cache = c->next;
        kfree(c);
    }
}

/** The cached set `id` of `version`, or NULL. */
static struct vgpu_caps_cache *vgpu_caps_cached(struct virtio_gpu_device *vg, uint32_t id,
                                                uint32_t version)
{
    struct vgpu_caps_cache *c;

    for (c = vg->caps_cache; c; c = c->next) {
        if (c->id == id && c->version == version) {
            return c;
        }
    }
    return NULL;
}

/**
 * Read set `id` of `version`, `size` bytes, from the host into a new cache
 * entry, or find the entry another process added while this one waited.
 * Returns 0 with *out set, or -errno.
 */
static int32_t vgpu_caps_fetch(struct virtio_gpu_device *vg, uint32_t id, uint32_t version,
                               uint32_t size, struct vgpu_caps_cache **out)
{
    struct virtio_gpu_get_capset *req;
    struct virtio_gpu_cmd *cmd;
    struct vgpu_caps_cache *c, *other;
    int32_t rc;

    c = kzalloc(sizeof(*c) + size);
    if (!c) {
        return -ENOMEM;
    }
    c->id = id;
    c->version = version;
    c->size = size;
    c->data = (unsigned char *)(c + 1);
    cmd = vgpu_cmd_new(vg, vg->ctrlq, VIRTIO_GPU_CMD_GET_CAPSET, sizeof(*req));
    if (!cmd) {
        kfree(c);
        return -ENOMEM;
    }
    req = (struct virtio_gpu_get_capset *)(void *)cmd->page;
    req->id = id;
    req->version = version;
    rc = vgpu_cmd_payload(cmd, 0, size + sizeof(req->hdr), 1);
    if (rc) {
        vgpu_cmd_free(cmd);
        kfree(c);
        return rc;
    }
    rc = vgpu_cmd_send_wait(cmd, NULL, 0);
    if (rc == -ETIMEDOUT) {
        kfree(c);
        return -EBUSY;
    }
    if (!rc) {
        rc =
            cmd->resp_type == VIRTIO_GPU_RESP_OK_CAPSET ? vgpu_caps_copy(cmd, c->data, size) : -EIO;
    }
    vgpu_cmd_free(cmd);
    if (rc) {
        kfree(c);
        return rc;
    }
    other = vgpu_caps_cached(vg, id, version);
    if (other) {
        kfree(c);
        c = other;
    } else {
        c->next = vg->caps_cache;
        vg->caps_cache = c;
    }
    *out = c;
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
 * sizes. The host is asked once for each set and version; later calls get
 * the same bytes, as under Linux.
 */
int32_t vgpu_get_caps_ioctl(struct drm_device *dev, void *data, struct drm_file *file)
{
    struct virtio_gpu_device *vg = vgpu_device(dev);
    struct drm_virtgpu_get_caps *args = data;
    struct vgpu_caps_cache *c;
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
    c = vgpu_caps_cached(vg, args->cap_set_id, args->cap_set_ver);
    if (!c) {
        rc = vgpu_caps_fetch(vg, args->cap_set_id, args->cap_set_ver, size, &c);
        if (rc) {
            return rc;
        }
    }
    if (copy_to_user((void __user *)(uintptr_t)args->addr, c->data, copy)) {
        return -EFAULT;
    }
    return 0;
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

/* Bytes the devices' answered transfers have copied since boot, both
 * ways, which /proc/stat reports as virtio_gpu_copied_bytes; under
 * vgpu_copied_lock, since commands are freed on any CPU. */
static uint64_t vgpu_copied;
static struct spinlock vgpu_copied_lock = SPINLOCK_INIT;

/* A virgl format's block: its width and height in texels and its bytes. */
struct vgpu_block {
    uint8_t w, h, bytes;
};

/* Each virgl format's block, by its number in Mesa 25.0.7's virgl_hw.h,
 * from the block and channels of the pipe format of the same name in
 * u_format.yaml: NONE, 0, which a blob has, a byte a texel as a buffer;
 * {0, 0, 0} for a number without a format and for the planar and 3D ASTC
 * formats, whose blocks a box of texels does not measure. */
static const struct vgpu_block vgpu_blocks[] = {
    {1, 1, 1},    /* 0 NONE */
    {1, 1, 4},    /* 1 B8G8R8A8_UNORM */
    {1, 1, 4},    /* 2 B8G8R8X8_UNORM */
    {1, 1, 4},    /* 3 A8R8G8B8_UNORM */
    {1, 1, 4},    /* 4 X8R8G8B8_UNORM */
    {1, 1, 2},    /* 5 B5G5R5A1_UNORM */
    {1, 1, 2},    /* 6 B4G4R4A4_UNORM */
    {1, 1, 2},    /* 7 B5G6R5_UNORM */
    {1, 1, 4},    /* 8 R10G10B10A2_UNORM */
    {1, 1, 1},    /* 9 L8_UNORM */
    {1, 1, 1},    /* 10 A8_UNORM */
    {1, 1, 1},    /* 11 I8_UNORM */
    {1, 1, 2},    /* 12 L8A8_UNORM */
    {1, 1, 2},    /* 13 L16_UNORM */
    {2, 1, 4},    /* 14 UYVY */
    {2, 1, 4},    /* 15 YUYV */
    {1, 1, 2},    /* 16 Z16_UNORM */
    {1, 1, 4},    /* 17 Z32_UNORM */
    {1, 1, 4},    /* 18 Z32_FLOAT */
    {1, 1, 4},    /* 19 Z24_UNORM_S8_UINT */
    {1, 1, 4},    /* 20 S8_UINT_Z24_UNORM */
    {1, 1, 4},    /* 21 Z24X8_UNORM */
    {1, 1, 4},    /* 22 X8Z24_UNORM */
    {1, 1, 1},    /* 23 S8_UINT */
    {1, 1, 8},    /* 24 R64_FLOAT */
    {1, 1, 16},   /* 25 R64G64_FLOAT */
    {1, 1, 24},   /* 26 R64G64B64_FLOAT */
    {1, 1, 32},   /* 27 R64G64B64A64_FLOAT */
    {1, 1, 4},    /* 28 R32_FLOAT */
    {1, 1, 8},    /* 29 R32G32_FLOAT */
    {1, 1, 12},   /* 30 R32G32B32_FLOAT */
    {1, 1, 16},   /* 31 R32G32B32A32_FLOAT */
    {1, 1, 4},    /* 32 R32_UNORM */
    {1, 1, 8},    /* 33 R32G32_UNORM */
    {1, 1, 12},   /* 34 R32G32B32_UNORM */
    {1, 1, 16},   /* 35 R32G32B32A32_UNORM */
    {1, 1, 4},    /* 36 R32_USCALED */
    {1, 1, 8},    /* 37 R32G32_USCALED */
    {1, 1, 12},   /* 38 R32G32B32_USCALED */
    {1, 1, 16},   /* 39 R32G32B32A32_USCALED */
    {1, 1, 4},    /* 40 R32_SNORM */
    {1, 1, 8},    /* 41 R32G32_SNORM */
    {1, 1, 12},   /* 42 R32G32B32_SNORM */
    {1, 1, 16},   /* 43 R32G32B32A32_SNORM */
    {1, 1, 4},    /* 44 R32_SSCALED */
    {1, 1, 8},    /* 45 R32G32_SSCALED */
    {1, 1, 12},   /* 46 R32G32B32_SSCALED */
    {1, 1, 16},   /* 47 R32G32B32A32_SSCALED */
    {1, 1, 2},    /* 48 R16_UNORM */
    {1, 1, 4},    /* 49 R16G16_UNORM */
    {1, 1, 6},    /* 50 R16G16B16_UNORM */
    {1, 1, 8},    /* 51 R16G16B16A16_UNORM */
    {1, 1, 2},    /* 52 R16_USCALED */
    {1, 1, 4},    /* 53 R16G16_USCALED */
    {1, 1, 6},    /* 54 R16G16B16_USCALED */
    {1, 1, 8},    /* 55 R16G16B16A16_USCALED */
    {1, 1, 2},    /* 56 R16_SNORM */
    {1, 1, 4},    /* 57 R16G16_SNORM */
    {1, 1, 6},    /* 58 R16G16B16_SNORM */
    {1, 1, 8},    /* 59 R16G16B16A16_SNORM */
    {1, 1, 2},    /* 60 R16_SSCALED */
    {1, 1, 4},    /* 61 R16G16_SSCALED */
    {1, 1, 6},    /* 62 R16G16B16_SSCALED */
    {1, 1, 8},    /* 63 R16G16B16A16_SSCALED */
    {1, 1, 1},    /* 64 R8_UNORM */
    {1, 1, 2},    /* 65 R8G8_UNORM */
    {1, 1, 3},    /* 66 R8G8B8_UNORM */
    {1, 1, 4},    /* 67 R8G8B8A8_UNORM */
    {1, 1, 4},    /* 68 X8B8G8R8_UNORM */
    {1, 1, 1},    /* 69 R8_USCALED */
    {1, 1, 2},    /* 70 R8G8_USCALED */
    {1, 1, 3},    /* 71 R8G8B8_USCALED */
    {1, 1, 4},    /* 72 R8G8B8A8_USCALED */
    {0, 0, 0},    /* 73 none */
    {1, 1, 1},    /* 74 R8_SNORM */
    {1, 1, 2},    /* 75 R8G8_SNORM */
    {1, 1, 3},    /* 76 R8G8B8_SNORM */
    {1, 1, 4},    /* 77 R8G8B8A8_SNORM */
    {0, 0, 0},    /* 78 none */
    {0, 0, 0},    /* 79 none */
    {0, 0, 0},    /* 80 none */
    {0, 0, 0},    /* 81 none */
    {1, 1, 1},    /* 82 R8_SSCALED */
    {1, 1, 2},    /* 83 R8G8_SSCALED */
    {1, 1, 3},    /* 84 R8G8B8_SSCALED */
    {1, 1, 4},    /* 85 R8G8B8A8_SSCALED */
    {0, 0, 0},    /* 86 none */
    {1, 1, 4},    /* 87 R32_FIXED */
    {1, 1, 8},    /* 88 R32G32_FIXED */
    {1, 1, 12},   /* 89 R32G32B32_FIXED */
    {1, 1, 16},   /* 90 R32G32B32A32_FIXED */
    {1, 1, 2},    /* 91 R16_FLOAT */
    {1, 1, 4},    /* 92 R16G16_FLOAT */
    {1, 1, 6},    /* 93 R16G16B16_FLOAT */
    {1, 1, 8},    /* 94 R16G16B16A16_FLOAT */
    {1, 1, 1},    /* 95 L8_SRGB */
    {1, 1, 2},    /* 96 L8A8_SRGB */
    {1, 1, 3},    /* 97 R8G8B8_SRGB */
    {1, 1, 4},    /* 98 A8B8G8R8_SRGB */
    {1, 1, 4},    /* 99 X8B8G8R8_SRGB */
    {1, 1, 4},    /* 100 B8G8R8A8_SRGB */
    {1, 1, 4},    /* 101 B8G8R8X8_SRGB */
    {1, 1, 4},    /* 102 A8R8G8B8_SRGB */
    {1, 1, 4},    /* 103 X8R8G8B8_SRGB */
    {1, 1, 4},    /* 104 R8G8B8A8_SRGB */
    {4, 4, 8},    /* 105 DXT1_RGB */
    {4, 4, 8},    /* 106 DXT1_RGBA */
    {4, 4, 16},   /* 107 DXT3_RGBA */
    {4, 4, 16},   /* 108 DXT5_RGBA */
    {4, 4, 8},    /* 109 DXT1_SRGB */
    {4, 4, 8},    /* 110 DXT1_SRGBA */
    {4, 4, 16},   /* 111 DXT3_SRGBA */
    {4, 4, 16},   /* 112 DXT5_SRGBA */
    {4, 4, 8},    /* 113 RGTC1_UNORM */
    {4, 4, 8},    /* 114 RGTC1_SNORM */
    {4, 4, 16},   /* 115 RGTC2_UNORM */
    {4, 4, 16},   /* 116 RGTC2_SNORM */
    {2, 1, 4},    /* 117 R8G8_B8G8_UNORM */
    {2, 1, 4},    /* 118 G8R8_G8B8_UNORM */
    {1, 1, 4},    /* 119 R8SG8SB8UX8U_NORM */
    {1, 1, 2},    /* 120 R5SG5SB6U_NORM */
    {1, 1, 4},    /* 121 A8B8G8R8_UNORM */
    {1, 1, 2},    /* 122 B5G5R5X1_UNORM */
    {1, 1, 4},    /* 123 R10G10B10A2_USCALED */
    {1, 1, 4},    /* 124 R11G11B10_FLOAT */
    {1, 1, 4},    /* 125 R9G9B9E5_FLOAT */
    {1, 1, 8},    /* 126 Z32_FLOAT_S8X24_UINT */
    {8, 1, 1},    /* 127 R1_UNORM */
    {1, 1, 4},    /* 128 R10G10B10X2_USCALED */
    {1, 1, 4},    /* 129 R10G10B10X2_SNORM */
    {1, 1, 1},    /* 130 L4A4_UNORM */
    {1, 1, 4},    /* 131 B10G10R10A2_UNORM */
    {1, 1, 4},    /* 132 R10SG10SB10SA2U_NORM */
    {1, 1, 2},    /* 133 R8G8Bx_SNORM */
    {1, 1, 4},    /* 134 R8G8B8X8_UNORM */
    {1, 1, 2},    /* 135 B4G4R4X4_UNORM */
    {1, 1, 4},    /* 136 X24S8_UINT */
    {1, 1, 4},    /* 137 S8X24_UINT */
    {1, 1, 8},    /* 138 X32_S8X24_UINT */
    {1, 1, 1},    /* 139 B2G3R3_UNORM */
    {1, 1, 4},    /* 140 L16A16_UNORM */
    {1, 1, 2},    /* 141 A16_UNORM */
    {1, 1, 2},    /* 142 I16_UNORM */
    {4, 4, 8},    /* 143 LATC1_UNORM */
    {4, 4, 8},    /* 144 LATC1_SNORM */
    {4, 4, 16},   /* 145 LATC2_UNORM */
    {4, 4, 16},   /* 146 LATC2_SNORM */
    {1, 1, 1},    /* 147 A8_SNORM */
    {1, 1, 1},    /* 148 L8_SNORM */
    {1, 1, 2},    /* 149 L8A8_SNORM */
    {1, 1, 1},    /* 150 I8_SNORM */
    {1, 1, 2},    /* 151 A16_SNORM */
    {1, 1, 2},    /* 152 L16_SNORM */
    {1, 1, 4},    /* 153 L16A16_SNORM */
    {1, 1, 2},    /* 154 I16_SNORM */
    {1, 1, 2},    /* 155 A16_FLOAT */
    {1, 1, 2},    /* 156 L16_FLOAT */
    {1, 1, 4},    /* 157 L16A16_FLOAT */
    {1, 1, 2},    /* 158 I16_FLOAT */
    {1, 1, 4},    /* 159 A32_FLOAT */
    {1, 1, 4},    /* 160 L32_FLOAT */
    {1, 1, 8},    /* 161 L32A32_FLOAT */
    {1, 1, 4},    /* 162 I32_FLOAT */
    {0, 0, 0},    /* 163 YV12 */
    {0, 0, 0},    /* 164 YV16 */
    {0, 0, 0},    /* 165 IYUV */
    {0, 0, 0},    /* 166 NV12 */
    {0, 0, 0},    /* 167 NV21 */
    {1, 1, 1},    /* 168 A4R4_UNORM */
    {1, 1, 1},    /* 169 R4A4_UNORM */
    {1, 1, 2},    /* 170 R8A8_UNORM */
    {1, 1, 2},    /* 171 A8R8_UNORM */
    {1, 1, 4},    /* 172 R10G10B10A2_SSCALED */
    {1, 1, 4},    /* 173 R10G10B10A2_SNORM */
    {1, 1, 4},    /* 174 B10G10R10A2_USCALED */
    {1, 1, 4},    /* 175 B10G10R10A2_SSCALED */
    {1, 1, 4},    /* 176 B10G10R10A2_SNORM */
    {1, 1, 1},    /* 177 R8_UINT */
    {1, 1, 2},    /* 178 R8G8_UINT */
    {1, 1, 3},    /* 179 R8G8B8_UINT */
    {1, 1, 4},    /* 180 R8G8B8A8_UINT */
    {1, 1, 1},    /* 181 R8_SINT */
    {1, 1, 2},    /* 182 R8G8_SINT */
    {1, 1, 3},    /* 183 R8G8B8_SINT */
    {1, 1, 4},    /* 184 R8G8B8A8_SINT */
    {1, 1, 2},    /* 185 R16_UINT */
    {1, 1, 4},    /* 186 R16G16_UINT */
    {1, 1, 6},    /* 187 R16G16B16_UINT */
    {1, 1, 8},    /* 188 R16G16B16A16_UINT */
    {1, 1, 2},    /* 189 R16_SINT */
    {1, 1, 4},    /* 190 R16G16_SINT */
    {1, 1, 6},    /* 191 R16G16B16_SINT */
    {1, 1, 8},    /* 192 R16G16B16A16_SINT */
    {1, 1, 4},    /* 193 R32_UINT */
    {1, 1, 8},    /* 194 R32G32_UINT */
    {1, 1, 12},   /* 195 R32G32B32_UINT */
    {1, 1, 16},   /* 196 R32G32B32A32_UINT */
    {1, 1, 4},    /* 197 R32_SINT */
    {1, 1, 8},    /* 198 R32G32_SINT */
    {1, 1, 12},   /* 199 R32G32B32_SINT */
    {1, 1, 16},   /* 200 R32G32B32A32_SINT */
    {1, 1, 1},    /* 201 A8_UINT */
    {1, 1, 1},    /* 202 I8_UINT */
    {1, 1, 1},    /* 203 L8_UINT */
    {1, 1, 2},    /* 204 L8A8_UINT */
    {1, 1, 1},    /* 205 A8_SINT */
    {1, 1, 1},    /* 206 I8_SINT */
    {1, 1, 1},    /* 207 L8_SINT */
    {1, 1, 2},    /* 208 L8A8_SINT */
    {1, 1, 2},    /* 209 A16_UINT */
    {1, 1, 2},    /* 210 I16_UINT */
    {1, 1, 2},    /* 211 L16_UINT */
    {1, 1, 4},    /* 212 L16A16_UINT */
    {1, 1, 2},    /* 213 A16_SINT */
    {1, 1, 2},    /* 214 I16_SINT */
    {1, 1, 2},    /* 215 L16_SINT */
    {1, 1, 4},    /* 216 L16A16_SINT */
    {1, 1, 4},    /* 217 A32_UINT */
    {1, 1, 4},    /* 218 I32_UINT */
    {1, 1, 4},    /* 219 L32_UINT */
    {1, 1, 8},    /* 220 L32A32_UINT */
    {1, 1, 4},    /* 221 A32_SINT */
    {1, 1, 4},    /* 222 I32_SINT */
    {1, 1, 4},    /* 223 L32_SINT */
    {1, 1, 8},    /* 224 L32A32_SINT */
    {1, 1, 4},    /* 225 B10G10R10A2_UINT */
    {4, 4, 8},    /* 226 ETC1_RGB8 */
    {2, 1, 4},    /* 227 R8G8_R8B8_UNORM */
    {2, 1, 4},    /* 228 G8R8_B8R8_UNORM */
    {1, 1, 4},    /* 229 R8G8B8X8_SNORM */
    {1, 1, 4},    /* 230 R8G8B8X8_SRGB */
    {1, 1, 4},    /* 231 R8G8B8X8_UINT */
    {1, 1, 4},    /* 232 R8G8B8X8_SINT */
    {1, 1, 4},    /* 233 B10G10R10X2_UNORM */
    {1, 1, 8},    /* 234 R16G16B16X16_UNORM */
    {1, 1, 8},    /* 235 R16G16B16X16_SNORM */
    {1, 1, 8},    /* 236 R16G16B16X16_FLOAT */
    {1, 1, 8},    /* 237 R16G16B16X16_UINT */
    {1, 1, 8},    /* 238 R16G16B16X16_SINT */
    {1, 1, 16},   /* 239 R32G32B32X32_FLOAT */
    {1, 1, 16},   /* 240 R32G32B32X32_UINT */
    {1, 1, 16},   /* 241 R32G32B32X32_SINT */
    {1, 1, 2},    /* 242 R8A8_SNORM */
    {1, 1, 4},    /* 243 R16A16_UNORM */
    {1, 1, 4},    /* 244 R16A16_SNORM */
    {1, 1, 4},    /* 245 R16A16_FLOAT */
    {1, 1, 8},    /* 246 R32A32_FLOAT */
    {1, 1, 2},    /* 247 R8A8_UINT */
    {1, 1, 2},    /* 248 R8A8_SINT */
    {1, 1, 4},    /* 249 R16A16_UINT */
    {1, 1, 4},    /* 250 R16A16_SINT */
    {1, 1, 8},    /* 251 R32A32_UINT */
    {1, 1, 8},    /* 252 R32A32_SINT */
    {1, 1, 4},    /* 253 R10G10B10A2_UINT */
    {1, 1, 2},    /* 254 B5G6R5_SRGB */
    {4, 4, 16},   /* 255 BPTC_RGBA_UNORM */
    {4, 4, 16},   /* 256 BPTC_SRGBA */
    {4, 4, 16},   /* 257 BPTC_RGB_FLOAT */
    {4, 4, 16},   /* 258 BPTC_RGB_UFLOAT */
    {1, 1, 2},    /* 259 A8L8_UNORM */
    {1, 1, 2},    /* 260 A8L8_SNORM */
    {1, 1, 2},    /* 261 A8L8_SRGB */
    {1, 1, 4},    /* 262 A16L16_UNORM */
    {1, 1, 2},    /* 263 G8R8_UNORM */
    {1, 1, 2},    /* 264 G8R8_SNORM */
    {1, 1, 4},    /* 265 G16R16_UNORM */
    {1, 1, 4},    /* 266 G16R16_SNORM */
    {1, 1, 4},    /* 267 A8B8G8R8_SNORM */
    {1, 1, 4},    /* 268 X8B8G8R8_SNORM */
    {4, 4, 8},    /* 269 ETC2_RGB8 */
    {4, 4, 8},    /* 270 ETC2_SRGB8 */
    {4, 4, 8},    /* 271 ETC2_RGB8A1 */
    {4, 4, 8},    /* 272 ETC2_SRGB8A1 */
    {4, 4, 16},   /* 273 ETC2_RGBA8 */
    {4, 4, 16},   /* 274 ETC2_SRGBA8 */
    {4, 4, 8},    /* 275 ETC2_R11_UNORM */
    {4, 4, 8},    /* 276 ETC2_R11_SNORM */
    {4, 4, 16},   /* 277 ETC2_RG11_UNORM */
    {4, 4, 16},   /* 278 ETC2_RG11_SNORM */
    {4, 4, 16},   /* 279 ASTC_4x4 */
    {5, 4, 16},   /* 280 ASTC_5x4 */
    {5, 5, 16},   /* 281 ASTC_5x5 */
    {6, 5, 16},   /* 282 ASTC_6x5 */
    {6, 6, 16},   /* 283 ASTC_6x6 */
    {8, 5, 16},   /* 284 ASTC_8x5 */
    {8, 6, 16},   /* 285 ASTC_8x6 */
    {8, 8, 16},   /* 286 ASTC_8x8 */
    {10, 5, 16},  /* 287 ASTC_10x5 */
    {10, 6, 16},  /* 288 ASTC_10x6 */
    {10, 8, 16},  /* 289 ASTC_10x8 */
    {10, 10, 16}, /* 290 ASTC_10x10 */
    {12, 10, 16}, /* 291 ASTC_12x10 */
    {12, 12, 16}, /* 292 ASTC_12x12 */
    {4, 4, 16},   /* 293 ASTC_4x4_SRGB */
    {5, 4, 16},   /* 294 ASTC_5x4_SRGB */
    {5, 5, 16},   /* 295 ASTC_5x5_SRGB */
    {6, 5, 16},   /* 296 ASTC_6x5_SRGB */
    {6, 6, 16},   /* 297 ASTC_6x6_SRGB */
    {8, 5, 16},   /* 298 ASTC_8x5_SRGB */
    {8, 6, 16},   /* 299 ASTC_8x6_SRGB */
    {8, 8, 16},   /* 300 ASTC_8x8_SRGB */
    {10, 5, 16},  /* 301 ASTC_10x5_SRGB */
    {10, 6, 16},  /* 302 ASTC_10x6_SRGB */
    {10, 8, 16},  /* 303 ASTC_10x8_SRGB */
    {10, 10, 16}, /* 304 ASTC_10x10_SRGB */
    {12, 10, 16}, /* 305 ASTC_12x10_SRGB */
    {12, 12, 16}, /* 306 ASTC_12x12_SRGB */
    {0, 0, 0},    /* 307 none */
    {1, 1, 4},    /* 308 R10G10B10X2_UNORM */
    {0, 0, 0},    /* 309 none */
    {0, 0, 0},    /* 310 none */
    {1, 1, 2},    /* 311 A4B4G4R4_UNORM */
    {1, 1, 1},    /* 312 R8_SRGB */
    {1, 1, 2},    /* 313 R8G8_SRGB */
    {0, 0, 0},    /* 314 P010 */
    {0, 0, 0},    /* 315 P012 */
    {0, 0, 0},    /* 316 P016 */
    {1, 1, 3},    /* 317 B8G8R8_UNORM */
    {1, 1, 1},    /* 318 R3G3B2_UNORM */
    {1, 1, 2},    /* 319 R4G4B4A4_UNORM */
    {1, 1, 2},    /* 320 R5G5B5A1_UNORM */
    {1, 1, 2},    /* 321 R5G6B5_UNORM */
    {1, 1, 1},    /* 322 Y8_400_UNORM */
    {0, 0, 0},    /* 323 Y8_U8_V8_444_UNORM */
    {0, 0, 0},    /* 324 Y8_U8_V8_422_UNORM */
    {0, 0, 0},    /* 325 NV16 */
    {1, 1, 1},    /* 326 Y8_UNORM */
    {2, 1, 4},    /* 327 YVYU */
    {1, 1, 3},    /* 328 Z16_UNORM_S8_UINT */
    {1, 1, 4},    /* 329 Z24_UNORM_S8_UINT_AS_R8G8B8A8 */
    {1, 1, 2},    /* 330 A1B5G5R5_UINT */
    {1, 1, 2},    /* 331 A1B5G5R5_UNORM */
    {1, 1, 2},    /* 332 A1R5G5B5_UINT */
    {1, 1, 2},    /* 333 A1R5G5B5_UNORM */
    {1, 1, 4},    /* 334 A2B10G10R10_UINT */
    {1, 1, 4},    /* 335 A2B10G10R10_UNORM */
    {1, 1, 4},    /* 336 A2R10G10B10_UINT */
    {1, 1, 4},    /* 337 A2R10G10B10_UNORM */
    {1, 1, 2},    /* 338 A4B4G4R4_UINT */
    {1, 1, 2},    /* 339 A4R4G4B4_UINT */
    {1, 1, 2},    /* 340 A4R4G4B4_UNORM */
    {1, 1, 4},    /* 341 A8B8G8R8_SINT */
    {1, 1, 4},    /* 342 A8B8G8R8_SSCALED */
    {1, 1, 4},    /* 343 A8B8G8R8_UINT */
    {1, 1, 4},    /* 344 A8B8G8R8_USCALED */
    {1, 1, 4},    /* 345 A8R8G8B8_SINT */
    {1, 1, 4},    /* 346 A8R8G8B8_SNORM */
    {1, 1, 4},    /* 347 A8R8G8B8_UINT */
    {0, 0, 0},    /* 348 ASTC_3x3x3 */
    {0, 0, 0},    /* 349 ASTC_3x3x3_SRGB */
    {0, 0, 0},    /* 350 ASTC_4x3x3 */
    {0, 0, 0},    /* 351 ASTC_4x3x3_SRGB */
    {0, 0, 0},    /* 352 ASTC_4x4x3 */
    {0, 0, 0},    /* 353 ASTC_4x4x3_SRGB */
    {0, 0, 0},    /* 354 ASTC_4x4x4 */
    {0, 0, 0},    /* 355 ASTC_4x4x4_SRGB */
    {0, 0, 0},    /* 356 ASTC_5x4x4 */
    {0, 0, 0},    /* 357 ASTC_5x4x4_SRGB */
    {0, 0, 0},    /* 358 ASTC_5x5x4 */
    {0, 0, 0},    /* 359 ASTC_5x5x4_SRGB */
    {0, 0, 0},    /* 360 ASTC_5x5x5 */
    {0, 0, 0},    /* 361 ASTC_5x5x5_SRGB */
    {0, 0, 0},    /* 362 ASTC_6x5x5 */
    {0, 0, 0},    /* 363 ASTC_6x5x5_SRGB */
    {0, 0, 0},    /* 364 ASTC_6x6x5 */
    {0, 0, 0},    /* 365 ASTC_6x6x5_SRGB */
    {0, 0, 0},    /* 366 ASTC_6x6x6 */
    {0, 0, 0},    /* 367 ASTC_6x6x6_SRGB */
    {4, 4, 8},    /* 368 ATC_RGB */
    {4, 4, 16},   /* 369 ATC_RGBA_EXPLICIT */
    {4, 4, 16},   /* 370 ATC_RGBA_INTERPOLATED */
    {1, 1, 4},    /* 371 AYUV */
    {1, 1, 4},    /* 372 B10G10R10A2_SINT */
    {1, 1, 4},    /* 373 B10G10R10X2_SINT */
    {1, 1, 4},    /* 374 B10G10R10X2_SNORM */
    {1, 1, 1},    /* 375 B2G3R3_UINT */
    {1, 1, 2},    /* 376 B4G4R4A4_UINT */
    {1, 1, 2},    /* 377 B5G5R5A1_UINT */
    {1, 1, 2},    /* 378 B5G6R5_UINT */
    {1, 1, 4},    /* 379 B8G8R8A8_SINT */
    {1, 1, 4},    /* 380 B8G8R8A8_SNORM */
    {1, 1, 4},    /* 381 B8G8R8A8_SSCALED */
    {1, 1, 4},    /* 382 B8G8R8A8_UINT */
    {1, 1, 4},    /* 383 B8G8R8A8_USCALED */
    {2, 1, 4},    /* 384 B8G8_R8G8_UNORM */
    {1, 1, 3},    /* 385 B8G8R8_SINT */
    {1, 1, 3},    /* 386 B8G8R8_SNORM */
    {1, 1, 3},    /* 387 B8G8R8_SRGB */
    {1, 1, 3},    /* 388 B8G8R8_SSCALED */
    {1, 1, 3},    /* 389 B8G8R8_UINT */
    {1, 1, 3},    /* 390 B8G8R8_USCALED */
    {1, 1, 4},    /* 391 B8G8R8X8_SINT */
    {1, 1, 4},    /* 392 B8G8R8X8_SNORM */
    {1, 1, 4},    /* 393 B8G8R8X8_UINT */
    {2, 1, 4},    /* 394 B8R8_G8R8_UNORM */
    {8, 4, 16},   /* 395 FXT1_RGB */
    {8, 4, 16},   /* 396 FXT1_RGBA */
    {1, 1, 4},    /* 397 G16R16_SINT */
    {2, 1, 4},    /* 398 G8B8_G8R8_UNORM */
    {0, 0, 0},    /* 399 G8_B8_R8_420_UNORM */
    {0, 0, 0},    /* 400 G8_B8R8_420_UNORM */
    {1, 1, 2},    /* 401 G8R8_SINT */
    {0, 0, 0},    /* 402 P030 */
    {1, 1, 4},    /* 403 R10G10B10A2_SINT */
    {1, 1, 4},    /* 404 R10G10B10X2_SINT */
    {1, 1, 1},    /* 405 R3G3B2_UINT */
    {1, 1, 2},    /* 406 R4G4B4A4_UINT */
    {1, 1, 2},    /* 407 R4G4B4X4_UNORM */
    {1, 1, 2},    /* 408 R5G5B5A1_UINT */
    {1, 1, 2},    /* 409 R5G5B5X1_UNORM */
    {1, 1, 2},    /* 410 R5G6B5_SRGB */
    {1, 1, 2},    /* 411 R5G6B5_UINT */
    {1, 1, 32},   /* 412 R64G64B64A64_SINT */
    {1, 1, 32},   /* 413 R64G64B64A64_UINT */
    {1, 1, 24},   /* 414 R64G64B64_SINT */
    {1, 1, 24},   /* 415 R64G64B64_UINT */
    {1, 1, 16},   /* 416 R64G64_SINT */
    {1, 1, 16},   /* 417 R64G64_UINT */
    {1, 1, 8},    /* 418 R64_SINT */
    {1, 1, 8},    /* 419 R64_UINT */
    {0, 0, 0},    /* 420 R8_B8_G8_420_UNORM */
    {0, 0, 0},    /* 421 R8_B8G8_420_UNORM */
    {2, 1, 4},    /* 422 R8B8_R8G8_UNORM */
    {0, 0, 0},    /* 423 R8_G8_B8_420_UNORM */
    {0, 0, 0},    /* 424 R8_G8B8_420_UNORM */
    {0, 0, 0},    /* 425 R8_G8_B8_UNORM */
    {2, 1, 4},    /* 426 VYUY */
    {1, 1, 2},    /* 427 X1B5G5R5_UNORM */
    {1, 1, 2},    /* 428 X1R5G5B5_UNORM */
    {1, 1, 4},    /* 429 XYUV */
    {1, 1, 4},    /* 430 X8B8G8R8_SINT */
    {1, 1, 4},    /* 431 X8R8G8B8_SINT */
    {1, 1, 4},    /* 432 X8R8G8B8_SNORM */
    {0, 0, 0},    /* 433 Y16_U16_V16_420_UNORM */
    {0, 0, 0},    /* 434 Y16_U16_V16_422_UNORM */
    {0, 0, 0},    /* 435 Y16_U16V16_422_UNORM */
    {0, 0, 0},    /* 436 Y16_U16_V16_444_UNORM */
    {2, 1, 8},    /* 437 Y210 */
    {2, 1, 8},    /* 438 Y212 */
    {2, 1, 8},    /* 439 Y216 */
    {1, 1, 4},    /* 440 Y410 */
    {1, 1, 8},    /* 441 Y412 */
    {1, 1, 8},    /* 442 Y416 */
    {0, 0, 0},    /* 443 NV15 */
    {0, 0, 0},    /* 444 NV20 */
    {0, 0, 0},    /* 445 Y8_U8_V8_440_UNORM */
    {0, 0, 0},    /* 446 R10_G10B10_420_UNORM */
    {0, 0, 0},    /* 447 R10_G10B10_422_UNORM */
    {0, 0, 0},    /* 448 X6G10_X6B10X6R10_420_UNORM */
    {0, 0, 0},    /* 449 X4G12_X4B12X4R12_420_UNORM */
    {1, 1, 2},    /* 450 X6R10_UNORM */
    {1, 1, 4},    /* 451 X6R10X6G10_UNORM */
    {1, 1, 2},    /* 452 X4R12_UNORM */
    {1, 1, 4},    /* 453 X4R12X4G12_UNORM */
    {0, 0, 0},    /* 454 R8_G8B8_422_UNORM */
};

uint64_t vgpu_box_bytes(uint32_t format, uint32_t w, uint32_t h, uint32_t d)
{
    const struct vgpu_block *b;
    uint64_t across, n;

    if (format >= sizeof(vgpu_blocks) / sizeof(vgpu_blocks[0]) || !vgpu_blocks[format].bytes) {
        return 0;
    }
    b = &vgpu_blocks[format];
    across = ((uint64_t)w + b->w - 1) / b->w * b->bytes;
    n = ((uint64_t)h + b->h - 1) / b->h * (d ? d : 1);
    /* No device takes a box this large; it counts nothing. */
    if (across && n > (uint64_t)-1 / across) {
        return 0;
    }
    return across * n;
}

void vgpu_count_copied(uint64_t bytes)
{
    uint32_t flags = spin_lock_irqsave(&vgpu_copied_lock);

    vgpu_copied += bytes;
    spin_unlock_irqrestore(&vgpu_copied_lock, flags);
}

uint64_t virtio_gpu_copied_bytes(void)
{
    uint32_t flags = spin_lock_irqsave(&vgpu_copied_lock);
    uint64_t bytes = vgpu_copied;

    spin_unlock_irqrestore(&vgpu_copied_lock, flags);
    return bytes;
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
        cmd->copied = vgpu_box_bytes(bo->format, a->box.w, a->box.h, a->box.d);
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
