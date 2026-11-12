/*
 * virtio_gpu_submit.c: asynchronous virgl submissions and their
 * dependencies.
 */
#include "virtio_gpu.h"
#include "../dmabuf/dmabuf.h"
#include "../lib/kerrno.h"
#include "../lib/kstring.h"
#include "../mm/kheap.h"
#include "../mm/usercopy.h"

struct vgpu_submit {
    struct drm_gem_object **bos; /* held until the device answers */
    uint32_t nbos;
    struct drm_pending_event *event; /* sent at the answer, for a polled ring */
};

struct vgpu_submit_sync {
    struct drm_syncobj *obj;
    struct dma_fence_chain *chain; /* allocated before a timeline submission */
    uint64_t point;
    uint32_t flags;
};

/** Drop all held buffers, including a partially built list, and an event
 * not sent. */
static void submit_free(struct vgpu_submit *s)
{
    uint32_t i;

    if (!s) {
        return;
    }
    if (s->event) {
        drm_event_cancel_free(s->event);
    }
    for (i = 0; i < s->nbos; i++) {
        drm_gem_object_put(s->bos[i]);
    }
    kfree(s->bos);
    kfree(s);
}

/** The device has finished with the command and every buffer it names: a
 * polled ring's file gets its event, unless the device was lost first. */
static void submit_done(struct virtio_gpu_cmd *cmd)
{
    struct vgpu_submit *s = cmd->arg;

    if (s->event && cmd->resp_type) {
        drm_send_event(s->event);
        s->event = NULL;
    }
    submit_free(s);
}

/* The virgl commands that copy a box between a resource and memory, whose
 * first eleven dwords after the header are the same: the resource, its
 * level, usage, stride and layer stride, and the box's x, y, z, width,
 * height and depth (Mesa's virgl_protocol.h). */
#define VIRGL_CCMD_RESOURCE_INLINE_WRITE 9u
#define VIRGL_CCMD_TRANSFER3D            43u
#define VIRGL_CCMD_COPY_TRANSFER3D       45u
#define VIRGL_BOX_DWORDS                 11u

/** Dword `i` of the command buffer in `cmd`'s payload, which holds it a page
 * at a time. */
static uint32_t payload_dword(const struct virtio_gpu_cmd *cmd, uint32_t i)
{
    uint32_t off = i * 4u;

    return *(const uint32_t *)(const void *)((const uint8_t *)cmd->payload[off / PAGE_SIZE].page +
                                             off % PAGE_SIZE);
}

/* A listed buffer's resource and format, which submit_copied looks up by
 * resource id in a table sorted by it. */
struct vgpu_box_res {
    uint32_t res_id;
    uint32_t format;
};

/** Sort `n` entries of `t` by resource id, a heap sort, so that the
 * submission's list sorts in n log n steps whatever its order. */
static void box_res_sort(struct vgpu_box_res *t, uint32_t n)
{
    uint32_t start, end, root, child;
    struct vgpu_box_res x;

    for (start = n / 2u; start-- > 0;) {
        for (root = start; (child = 2u * root + 1u) < n; root = child) {
            if (child + 1u < n && t[child].res_id < t[child + 1u].res_id) {
                child++;
            }
            if (t[root].res_id >= t[child].res_id) {
                break;
            }
            x = t[root];
            t[root] = t[child];
            t[child] = x;
        }
    }
    for (end = n; end-- > 1u;) {
        x = t[0];
        t[0] = t[end];
        t[end] = x;
        for (root = 0; (child = 2u * root + 1u) < end; root = child) {
            if (child + 1u < end && t[child].res_id < t[child + 1u].res_id) {
                child++;
            }
            if (t[root].res_id >= t[child].res_id) {
                break;
            }
            x = t[root];
            t[root] = t[child];
            t[child] = x;
        }
    }
}

/** The bytes of the box of the copy whose header is dword `i` of `cmd`'s
 * command buffer, at the format of its resource, which must be one of the
 * `n` entries of the sorted table `t`; 0 otherwise. */
static uint64_t submit_box_bytes(const struct virtio_gpu_cmd *cmd, const struct vgpu_box_res *t,
                                 uint32_t n, uint32_t i)
{
    uint32_t res = payload_dword(cmd, i + 1), lo = 0, hi = n, mid;

    while (lo < hi) {
        mid = lo + (hi - lo) / 2u;
        if (t[mid].res_id < res) {
            lo = mid + 1u;
        } else {
            hi = mid;
        }
    }
    if (lo == n || t[lo].res_id != res) {
        return 0;
    }
    return vgpu_box_bytes(t[lo].format, payload_dword(cmd, i + 9), payload_dword(cmd, i + 10),
                          payload_dword(cmd, i + 11));
}

/**
 * Set `cmd->copied` to the bytes that submission `s`, whose `size` bytes of
 * commands are in `cmd`'s payload, has the device copy, when the file's
 * context `ctx` speaks virgl. Mesa's virgl encodes its uploads and readbacks
 * in the submission when the host offers encoded transfers, as TRANSFER3D
 * from a resource's backing or COPY_TRANSFER3D from a staging buffer, and
 * small ones as RESOURCE_INLINE_WRITE; each box counts at its resource's
 * format. A Venus or native context's stream is another protocol and counts
 * nothing. Each copy looks its resource up by binary search in a sorted copy
 * of the buffer list, so a parse costs commands times log buffers; Linux
 * sends the commands unread and allocates nothing to count them
 * (virtgpu_submit.c:308, :355, :421 of 6.12.115). The count never fails the
 * submission: when the sorted copy cannot be allocated, the submission
 * counts nothing.
 */
static void submit_copied(struct virtio_gpu_cmd *cmd, const struct vgpu_submit *s,
                          const struct vgpu_context *ctx, uint32_t size)
{
    uint32_t n = size / 4u, i = 0, k, nt = 0, hdr, op, len;
    uint32_t capset = ctx->context_init & VIRTIO_GPU_CONTEXT_INIT_CAPSET_ID_MASK;
    uint64_t bytes = 0, box;
    struct vgpu_box_res *t;
    struct virtio_gpu_object *bo;

    cmd->copied = 0;
    if ((capset != 0 && capset != VIRTIO_GPU_CAPSET_VIRGL && capset != VIRTIO_GPU_CAPSET_VIRGL2) ||
        !s->nbos || (uint64_t)s->nbos * sizeof(*t) > (uint64_t)(size_t)-1) {
        return;
    }
    t = kmalloc((size_t)s->nbos * sizeof(*t));
    if (!t) {
        return;
    }
    for (k = 0; k < s->nbos; k++) {
        bo = vgpu_bo(s->bos[k]);
        if (bo) {
            t[nt].res_id = bo->res_id;
            t[nt].format = bo->format;
            nt++;
        }
    }
    box_res_sort(t, nt);
    while (i < n) {
        hdr = payload_dword(cmd, i);
        op = hdr & 0xffu;
        len = hdr >> 16;
        if ((op == VIRGL_CCMD_TRANSFER3D || op == VIRGL_CCMD_COPY_TRANSFER3D ||
             op == VIRGL_CCMD_RESOURCE_INLINE_WRITE) &&
            len >= VIRGL_BOX_DWORDS && len < n - i) {
            box = submit_box_bytes(cmd, t, nt, i);
            if (bytes + box >= bytes) {
                bytes += box;
            }
        }
        i += 1u + len;
    }
    kfree(t);
    cmd->copied = bytes;
}

/**
 * Reserve the event of a submission on a polled ring, Linux's
 * virtio_gpu_fence_event_create: a bare VIRTGPU_EVENT_FENCE_SIGNALED the
 * file reads once the fence signals. 0, or -ENOMEM.
 */
static int32_t submit_event(struct drm_file *file, struct vgpu_submit *s)
{
    struct drm_pending_event *e = kzalloc(sizeof(*e));

    if (!e) {
        return -ENOMEM;
    }
    e->event.base.type = VIRTGPU_EVENT_FENCE_SIGNALED;
    e->event.base.length = sizeof(e->event.base);
    e->length = sizeof(e->event.base);
    if (drm_event_reserve(file, e) != 0) {
        kfree(e);
        return -ENOMEM;
    }
    s->event = e;
    return 0;
}

/** Release parsed sync objects, including unused timeline chains. */
static void sync_free(struct vgpu_submit_sync *syncs, uint32_t n)
{
    uint32_t i;

    if (!syncs) {
        return;
    }
    for (i = 0; i < n; i++) {
        drm_syncobj_put(syncs[i].obj);
        kfree(syncs[i].chain);
    }
    kfree(syncs);
}

/** Read dependency descriptors with Linux's extensible stride. In dependencies
 * are waited on now; output objects and chains are held until publication.
 * Failure leaves a partial array for the caller's cleanup. May sleep. */
static int32_t sync_parse(struct virtio_gpu_device *vg, struct drm_file *file,
                          struct drm_virtgpu_execbuffer *args, int out, uint64_t context,
                          struct vgpu_submit_sync **result)
{
    struct drm_virtgpu_execbuffer_syncobj a;
    struct vgpu_submit_sync *syncs;
    struct dma_fence *f;
    uint64_t user = out ? args->out_syncobjs : args->in_syncobjs;
    uint32_t n = out ? args->num_out_syncobjs : args->num_in_syncobjs;
    uint32_t stride = args->syncobj_stride;
    uint32_t i, bytes = stride < sizeof(a) ? stride : sizeof(a);
    int32_t rc;

    if (!n) {
        return 0;
    }
    if ((uint64_t)n * sizeof(*syncs) > (uint64_t)(size_t)-1 ||
        user + (uint64_t)(n - 1) * stride < user) {
        return -ENOMEM;
    }
    syncs = kzalloc((size_t)n * sizeof(*syncs));
    if (!syncs) {
        return -ENOMEM;
    }
    *result = syncs;
    for (i = 0; i < n; i++) {
        k_memset(&a, 0, sizeof(a));
        if (copy_from_user(&a, (const void __user *)(uintptr_t)(user + (uint64_t)i * stride),
                           bytes)) {
            return -EFAULT;
        }
        if ((out && a.flags) || (a.flags & ~VIRTGPU_EXECBUF_SYNCOBJ_RESET)) {
            return -EINVAL;
        }
        syncs[i].obj = drm_syncobj_find(file, a.handle);
        if (!syncs[i].obj) {
            return -EINVAL;
        }
        syncs[i].flags = a.flags;
        syncs[i].point = a.point;
        if (out && a.point) {
            syncs[i].chain = dma_fence_chain_alloc();
            if (!syncs[i].chain) {
                return -ENOMEM;
            }
        }
        if (!out) {
            rc = drm_syncobj_find_fence(file, a.handle, a.point, 0, &f);
            if (rc) {
                return rc;
            }
            rc = vgpu_wait_dependency(vg, f, context);
            dma_fence_put(f);
            if (rc) {
                return rc;
            }
        }
    }
    return 0;
}

/** Resolve and hold all submitted buffer handles before any dependency wait,
 * then count the copies of the commands in `cmd`'s payload with
 * submit_copied. Return 0 or -errno; submit_free releases a partial
 * result. */
static int32_t submit_buffers(struct drm_file *file, struct drm_virtgpu_execbuffer *a,
                              struct virtio_gpu_cmd *cmd, struct vgpu_submit *s)
{
    struct virtio_gpu_object *bo;
    uint32_t i, handle;
    int32_t rc;

    if ((uint64_t)a->num_bo_handles * sizeof(*s->bos) > (uint64_t)(size_t)-1) {
        rc = -ENOMEM;
        return rc;
    }
    if (a->num_bo_handles) {
        s->bos = kzalloc((size_t)a->num_bo_handles * sizeof(*s->bos));
        if (!s->bos) {
            rc = -ENOMEM;
            return rc;
        }
    }
    for (i = 0; i < a->num_bo_handles; i++) {
        if (copy_from_user(&handle,
                           (const void __user *)(uintptr_t)(a->bo_handles + (uint64_t)i * 4), 4)) {
            rc = -EFAULT;
            return rc;
        }
        s->bos[i] = drm_gem_object_lookup(file, handle);
        if (!s->bos[i]) {
            rc = -ENOENT;
            return rc;
        }
        s->nbos++;
        bo = vgpu_bo(s->bos[i]);
        if (!bo || !bo->res_id) {
            rc = -EINVAL;
            return rc;
        }
    }
    submit_copied(cmd, s, file->driver_priv, a->size);
    return 0;
}

/** Wait only for foreign producers, then attach each buffer in queue order.
 * Held buffer references survive every wait. Return 0 or -errno. */
static int32_t submit_dependencies(struct virtio_gpu_device *vg, struct drm_file *file,
                                   struct vgpu_submit *s, uint64_t context)
{
    struct dma_fence *f;
    uint32_t i;
    int32_t rc;

    for (i = 0; i < s->nbos; i++) {
        f = dma_resv_snapshot(s->bos[i]->resv, 1);
        rc = f ? vgpu_wait_dependency(vg, f, context) : -ENOMEM;
        dma_fence_put(f);
        if (rc) {
            return rc;
        }
        rc = vgpu_context_attach(vg, file, vgpu_bo(s->bos[i]));
        if (rc) {
            return rc;
        }
    }
    return 0;
}

/** Publish output fences and reset inputs after all allocations succeed. */
static void submit_sync_publish(struct virtio_gpu_cmd *cmd, struct vgpu_submit_sync *ins,
                                struct vgpu_submit_sync *outs, struct drm_virtgpu_execbuffer *a)
{
    uint32_t i;

    for (i = 0; i < a->num_in_syncobjs; i++) {
        if (ins[i].flags & VIRTGPU_EXECBUF_SYNCOBJ_RESET) {
            drm_syncobj_replace_fence(ins[i].obj, NULL);
        }
    }
    for (i = 0; i < a->num_out_syncobjs; i++) {
        if (outs[i].chain) {
            drm_syncobj_add_point(outs[i].obj, outs[i].chain, cmd->fence, outs[i].point);
            outs[i].chain = NULL;
        } else {
            drm_syncobj_replace_fence(outs[i].obj, cmd->fence);
        }
    }
}

/**
 * Put a submission on ring `a->ring_idx` of the file's context, whose fence
 * context is `context`, reserving the event its answer sends when the ring
 * is in the context's poll mask. 0 or -ENOMEM.
 */
static int32_t submit_ring(struct drm_file *file, const struct drm_virtgpu_execbuffer *a,
                           uint64_t context, struct virtio_gpu_cmd *cmd, struct vgpu_submit *s)
{
    struct vgpu_context *ctx = file->driver_priv;

    cmd->fence_ctx = context;
    cmd->ring = 1;
    cmd->ring_idx = a->ring_idx;
    return ((ctx->ring_mask >> a->ring_idx) & 1u) ? submit_event(file, s) : 0;
}

/** With VIRTGPU_EXECBUF_FENCE_FD_IN, wait for the in-fence's producers on
 * fence contexts other than `context`. 0, -EINVAL for a descriptor with no
 * fence, or the wait's error. May sleep. */
static int32_t submit_fence_in(struct virtio_gpu_device *vg, const struct drm_virtgpu_execbuffer *a,
                               uint64_t context)
{
    struct dma_fence *f;
    int32_t rc;

    if (!(a->flags & VIRTGPU_EXECBUF_FENCE_FD_IN)) {
        return 0;
    }
    f = sync_file_get_fence(a->fence_fd);
    rc = f ? vgpu_wait_dependency(vg, f, context) : -EINVAL;
    dma_fence_put(f);
    return rc;
}

/**
 * Prepare the submission `cmd` with its fence where something reads the
 * fence, an out-fence sync file, an output syncobj, a buffer's reservation
 * or a polled ring's event, and take the fence off otherwise, as Linux's
 * virtio_gpu_init_submit (virtgpu_submit.c:383-397): QEMU answers a fenced
 * command only at its next poll of virgl's fences.
 */
static void submit_prepare(struct virtio_gpu_cmd *cmd, const struct drm_virtgpu_execbuffer *a,
                           const struct vgpu_submit *s)
{
    if (!(a->flags & VIRTGPU_EXECBUF_FENCE_FD_OUT) && !a->num_out_syncobjs && !a->num_bo_handles &&
        !s->event) {
        vgpu_cmd_unfence(cmd);
    }
    vgpu_cmd_prepare(cmd);
}

/**
 * Submit copied commands asynchronously after foreign dependencies, as
 * Linux's virtio_gpu_execbuffer_ioctl: with VIRTGPU_EXECBUF_RING_IDX on a
 * ring CONTEXT_INIT gave the file's context, whose fence context the
 * submission's fence takes, and which sends the file an event at the
 * answer when the ring is in its poll mask. Return 0 or -errno: EINVAL for
 * an unknown flag or a ring the context lacks.
 */
int32_t vgpu_execbuffer_ioctl(struct drm_device *dev, void *data, struct drm_file *file)
{
    struct virtio_gpu_device *vg = vgpu_device(dev);
    struct drm_virtgpu_execbuffer *a = data;
    struct virtio_gpu_cmd *cmd = NULL;
    struct virtio_gpu_submit_3d *req;
    struct vgpu_submit *s = NULL;
    struct vgpu_submit_sync *ins = NULL, *outs = NULL;
    struct vgpu_context *ctx = file->driver_priv;
    uint64_t context = vg->fence_context;
    uint32_t i;
    int32_t rc, fd = -1;

    if (vg->dead) {
        return -ENODEV;
    }
    if (!vg->has_virgl) {
        return -ENOSYS;
    }
    if (a->flags & ~VIRTGPU_EXECBUF_FLAGS) {
        return -EINVAL;
    }
    if (a->flags & VIRTGPU_EXECBUF_RING_IDX) {
        if (!ctx || a->ring_idx >= ctx->num_rings || !ctx->base_fence_ctx) {
            return -EINVAL;
        }
        context = ctx->base_fence_ctx + a->ring_idx;
    }
    vgpu_reap(vg);
    rc = vgpu_context_ensure(vg, file);
    if (rc) {
        return rc;
    }
    ctx = file->driver_priv;
    cmd = vgpu_cmd_new(vg, vg->ctrlq, VIRTIO_GPU_CMD_SUBMIT_3D, sizeof(*req));
    s = kzalloc(sizeof(*s));
    if (!cmd || !s) {
        rc = -ENOMEM;
        goto out;
    }
    req = (struct virtio_gpu_submit_3d *)(void *)cmd->page;
    req->hdr.ctx_id = ctx->id;
    req->size = a->size;
    rc = (a->flags & VIRTGPU_EXECBUF_RING_IDX) ? submit_ring(file, a, context, cmd, s) : 0;
    if (!rc) {
        rc = vgpu_cmd_payload(cmd, a->command, a->size, 0);
    }
    if (rc) {
        goto out;
    }
    rc = submit_buffers(file, a, cmd, s);
    if (rc) {
        goto out;
    }
    rc = submit_fence_in(vg, a, context);
    if (!rc) {
        rc = sync_parse(vg, file, a, 1, context, &outs);
    }
    if (!rc) {
        rc = sync_parse(vg, file, a, 0, context, &ins);
    }
    if (rc) {
        goto out;
    }
    rc = submit_dependencies(vg, file, s, context);
    if (rc) {
        goto out;
    }
    submit_prepare(cmd, a, s);
    for (i = 0; i < s->nbos; i++) {
        rc = dma_resv_add_fence(s->bos[i]->resv, cmd->fence, 1);
        if (rc) {
            goto out;
        }
    }
    if (a->flags & VIRTGPU_EXECBUF_FENCE_FD_OUT) {
        fd = sync_file_create(cmd->fence, NULL);
        if (fd < 0) {
            rc = fd;
            goto out;
        }
        a->fence_fd = fd;
    }
    submit_sync_publish(cmd, ins, outs, a);
    vgpu_cmd_send_sleepable(cmd, submit_done, s);
    cmd = NULL;
    s = NULL;
    rc = 0;
out:
    if (cmd) {
        if (cmd->prepared) {
            dma_fence_set_error(cmd->fence, rc);
            dma_fence_signal(cmd->fence);
        }
        vgpu_cmd_free(cmd);
    }
    submit_free(s);
    sync_free(ins, a->num_in_syncobjs);
    sync_free(outs, a->num_out_syncobjs);
    return rc;
}
