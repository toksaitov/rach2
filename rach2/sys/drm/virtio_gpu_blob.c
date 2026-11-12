/*
 * virtio_gpu_blob.c: virtio-gpu's blob resources, Linux 6.12's: blobs of
 * guest memory, host memory or both, the host-visible region that MAP_BLOB
 * places host memory in for programs to map, or the device's own pages
 * that MAP_BLOB names where the transport has no such region, and
 * CONTEXT_INIT, which gives a file's context its capability set, its rings
 * and its name.
 */
#include "virtio_gpu.h"
#include "../dmabuf/dmabuf.h"
#include "../klog.h"
#include "../lib/kerrno.h"
#include "../lib/kstring.h"
#include "../mm/kheap.h"
#include "../mm/paging.h"
#include "../mm/usercopy.h"
#include "../virtio/vgpushm.h"

/* The bytes of MAP_BLOB's inline answer, and the runs of a page list one
 * answer holds after its heads. */
#define VGPU_MAP_ANSWER (PAGE_SIZE - VIRTIO_GPU_RESP_OFF)
#define VGPU_MAP_RUNS   ((VGPU_MAP_ANSWER - VGS_PAGES_OFF) / sizeof(struct vgs_run))

/* The highest capability set id CONTEXT_INIT takes, and the most of its
 * parameters one call gives, Linux's. */
#define VGPU_MAX_CAPSET_ID  63
#define VGPU_CONTEXT_PARAMS 4

/* ------------------------------------------------------------------------
 * The host-visible region
 */

/**
 * Find shared memory region 1, the host-visible memory MAP_BLOB places
 * blobs in, as Linux's virtio_gpu_init does. Its pieces are mapped as they
 * are needed, never the whole, which is 4 GiB at 14 TiB under QEMU. A
 * region whose end lies past what a page table entry holds, past 4 GiB on
 * i686, is refused with a record.
 */
void vgpu_hostmem_init(struct virtio_gpu_device *vg)
{
    uint64_t addr, len;

    if (vg->vdev->pages && vg->has_blob) {
        vg->has_host_visible = 1;
        vg->host_pages = 1;
        klog(KLOG_INFO, "virtio_gpu", "host memory: the device's own pages, by MAP_BLOB");
        return;
    }
    if (virtio_shm_region(vg->vdev, VIRTIO_GPU_SHM_ID_HOST_VISIBLE, &addr, &len) != 0) {
        return;
    }
    if ((uint64_t)(uintptr_t)(addr + len - 1) != addr + len - 1) {
        klog(KLOG_WARN, "virtio_gpu",
             "host memory: refused: 0x%x%08x +0x%x%08x lies past what a page table entry holds",
             (unsigned)(addr >> 32), (unsigned)addr, (unsigned)(len >> 32), (unsigned)len);
        return;
    }
    vg->hostmem_addr = addr;
    vg->hostmem_len = len;
    vg->has_host_visible = 1;
    klog(KLOG_INFO, "virtio_gpu", "host memory window: 0x%x%08x +0x%x%08x", (unsigned)(addr >> 32),
         (unsigned)addr, (unsigned)(len >> 32), (unsigned)len);
}

/**
 * Give `bo` the first piece of the host-visible region its size fits in,
 * in `hv_offset`, as Linux's drm_mm gives one: 0, or -ENOSPC.
 */
static int32_t hostmem_place(struct virtio_gpu_device *vg, struct virtio_gpu_object *bo)
{
    uint64_t size = bo->shmem.base.size, at = 0;
    struct virtio_gpu_object **p;

    for (p = &vg->hostmem_bo; *p; p = &(*p)->hv_next) {
        if ((*p)->hv_offset - at >= size) {
            break;
        }
        at = (*p)->hv_offset + (*p)->shmem.base.size;
    }
    if (at > vg->hostmem_len || vg->hostmem_len - at < size) {
        return -ENOSPC;
    }
    bo->hv_offset = at;
    bo->hv_next = *p;
    *p = bo;
    bo->host_visible = 1;
    return 0;
}

/** Take away the kernel's window onto `bo`'s piece, if any, and give the
 * piece back to the region, or let go of the device's pages it maps. */
void vgpu_blob_release(struct virtio_gpu_device *vg, struct virtio_gpu_object *bo)
{
    struct virtio_gpu_object **p;

    vgpu_blob_pages_drop(vg, bo, 0);
    if (bo->vmap) {
        mmio_unmap(bo->vmap, (size_t)bo->shmem.base.size);
        bo->vmap = 0;
    }
    if (!bo->host_visible) {
        return;
    }
    for (p = &vg->hostmem_bo; *p && *p != bo; p = &(*p)->hv_next) {
    }
    if (*p) {
        *p = bo->hv_next;
    }
    bo->host_visible = 0;
}

/**
 * The transport took the frames of `bo`'s page list back, the device's
 * memory having changed or its VM gone: every program's mapping of the blob
 * goes, and the next touch of one raises SIGBUS, as a revoked mapping does
 * on Linux. Under the big lock; does not sleep.
 */
static void pages_revoked(void *owner)
{
    struct virtio_gpu_object *bo = (struct virtio_gpu_object *)owner;

    paging_zap_device(&bo->shmem.base, 0, (uint32_t)(bo->shmem.base.size >> PAGE_SHIFT));
    bo->map_ok = 0;
}

void vgpu_blob_pages_drop(struct virtio_gpu_device *vg, struct virtio_gpu_object *bo, int zap)
{
    if (!bo->pages_map) {
        return;
    }
    if (zap) {
        paging_zap_device(&bo->shmem.base, 0, (uint32_t)(bo->shmem.base.size >> PAGE_SHIFT));
    }
    virtio_map_free(vg->vdev, bo->pages_map);
    bo->pages_map = NULL;
    bo->map_ok = 0;
}

/* A page list as its answers come: the runs so far, which cover the blob's
 * pages below `first` of its `total`, and the first answer's cache type. */
struct pages_gather {
    uint64_t total, first;
    uint32_t info, n;
    struct virtio_run *runs; /* `n` of them */
};

/**
 * Add the runs of one answer of a page list, `got` bytes at `answer`, to
 * those `g` gathered: the heads checked against the blob and the request,
 * each run checked alone, and the cache type the same as the first
 * answer's. 0, or -EIO for an answer that breaks a rule, -ENOMEM.
 */
static int32_t pages_take(const uint8_t *answer, uint32_t got, struct pages_gather *g)
{
    struct vgs_pages head;
    struct vgs_run run;
    struct virtio_run *more;
    uint64_t left;
    uint32_t i;

    if (got < VGS_PAGES_OFF) {
        return -EIO;
    }
    k_memcpy(&head, answer + 24, sizeof(head));
    if (vgs_check_pages(&head, got, g->total, g->first, &left) != VGS_OK ||
        head.count > VGPU_MAP_RUNS || (g->first != 0 && head.map_info != g->info)) {
        return -EIO;
    }
    g->info = head.map_info;
    more = (struct virtio_run *)kmalloc(((size_t)g->n + head.count) * sizeof(*more));
    if (!more) {
        return -ENOMEM;
    }
    if (g->n) {
        k_memcpy(more, g->runs, (size_t)g->n * sizeof(*more));
    }
    kfree(g->runs);
    g->runs = more;
    for (i = 0; i < head.count; i++) {
        k_memcpy(&run, answer + VGS_PAGES_OFF + i * sizeof(run), sizeof(run));
        if (vgs_check_run(&run, left) != VGS_OK) {
            return -EIO;
        }
        more[g->n].addr = run.addr;
        more[g->n].pages = run.pages;
        g->n++;
        left -= run.pages;
        g->first += run.pages;
    }
    return 0;
}

/**
 * RESOURCE_MAP_BLOB on a device that maps blobs from its own pages: the
 * page list asked for from the first page on, and from the page after its
 * last run again until it covers `bo`, then its frames held through the
 * transport. The device pins the blob's pages at its first answer and lets
 * them go at UNMAP_BLOB, which the free sends once any answer came. 0; the
 * answer's error, -EIO for a list that breaks the transport's rules,
 * -EFAULT for pages the transport cannot share, -ENOMEM, -ETIMEDOUT. May
 * sleep.
 */
static int32_t blob_map_pages(struct virtio_gpu_device *vg, struct virtio_gpu_object *bo)
{
    struct pages_gather g;
    struct virtio_gpu_resource_map_blob *req;
    struct virtio_gpu_cmd *cmd;
    uint32_t got;
    uint8_t *answer = (uint8_t *)kmalloc(VGPU_MAP_ANSWER);
    int32_t rc = answer ? 0 : -ENOMEM;

    k_memset(&g, 0, sizeof(g));
    g.total = bo->shmem.base.size >> PAGE_SHIFT;
    while (rc == 0 && g.first < g.total) {
        cmd = vgpu_cmd_new(vg, vg->ctrlq, VIRTIO_GPU_CMD_RESOURCE_MAP_BLOB, sizeof(*req));
        if (!cmd) {
            rc = -ENOMEM;
            break;
        }
        req = (struct virtio_gpu_resource_map_blob *)(void *)cmd->page;
        req->resource_id = bo->res_id;
        req->offset = g.first;
        rc = vgpu_cmd_send_wait_map(cmd, answer, VGPU_MAP_ANSWER, &got);
        if (rc == 0 || rc == -ETIMEDOUT) {
            bo->host_visible = 1; /* the device may hold the pages: UNMAP_BLOB at the free */
        }
        if (rc == 0 && ((const struct virtio_gpu_ctrl_hdr *)(const void *)answer)->type !=
                           VIRTIO_GPU_RESP_OK_MAP_INFO) {
            rc = -EIO;
        }
        if (rc == 0) {
            rc = pages_take(answer, got, &g);
        }
    }
    if (rc == 0) {
        rc = virtio_map_pages(vg->vdev, g.runs, g.n, pages_revoked, bo, &bo->pages_map);
    }
    if (rc == 0) {
        bo->map_info = g.info & VIRTIO_GPU_MAP_CACHE_MASK;
        bo->map_ok = 1;
        bo->shmem.base.map_wc = bo->map_info == VIRTIO_GPU_MAP_CACHE_WC;
    } else {
        klog(KLOG_WARN, "virtio_gpu", "resource %u: refused: its page list: error %d",
             (unsigned)bo->res_id, (int)rc);
    }
    kfree(g.runs);
    kfree(answer);
    return rc;
}

/**
 * RESOURCE_MAP_BLOB, Linux's virtio_gpu_vram_map: place `bo` in the
 * host-visible region and have the host map its memory there, waited for,
 * with the cache type the answer gives in `map_info`. 0; -EINVAL without
 * the region, -ENOSPC when no piece fits, -ENOMEM, or the answer's error,
 * after which the piece goes back. On -ETIMEDOUT the piece stays placed for
 * the UNMAP_BLOB the free sends, since the host may yet map it. May sleep.
 */
static int32_t blob_map(struct virtio_gpu_device *vg, struct virtio_gpu_object *bo)
{
    struct virtio_gpu_resource_map_blob *req;
    struct virtio_gpu_resp_map_info info;
    struct virtio_gpu_cmd *cmd;
    int32_t rc;

    if (!vg->has_host_visible) {
        return -EINVAL;
    }
    if (vg->host_pages) {
        return blob_map_pages(vg, bo);
    }
    cmd = vgpu_cmd_new(vg, vg->ctrlq, VIRTIO_GPU_CMD_RESOURCE_MAP_BLOB, sizeof(*req));
    if (!cmd) {
        return -ENOMEM;
    }
    rc = hostmem_place(vg, bo);
    if (rc != 0) {
        vgpu_cmd_free(cmd);
        return rc;
    }
    req = (struct virtio_gpu_resource_map_blob *)(void *)cmd->page;
    req->resource_id = bo->res_id;
    req->offset = bo->hv_offset;
    rc = vgpu_cmd_send_wait_map(cmd, &info, sizeof(info), NULL);
    if (rc == 0 && info.hdr.type != VIRTIO_GPU_RESP_OK_MAP_INFO) {
        rc = -EIO;
    }
    if (rc == 0) {
        bo->map_info = info.map_info & VIRTIO_GPU_MAP_CACHE_MASK;
        bo->map_ok = 1;
        bo->shmem.base.map_wc = bo->map_info == VIRTIO_GPU_MAP_CACHE_WC;
    } else if (rc != -ETIMEDOUT) {
        vgpu_blob_release(vg, bo);
    }
    return rc;
}

/**
 * Queue RESOURCE_UNMAP_BLOB of `bo`, whose piece goes back to the region
 * once the host has also answered the UNREF after it. 0, or -ENOMEM.
 *
 * The command carries no fence, as Linux's virtio_gpu_cmd_unmap: QEMU
 * suspends an UNMAP_BLOB until the mapping is gone, and keeps an unfenced
 * one at the head of the queue meanwhile, but moves a fenced one aside and
 * runs the UNREF behind it on a resource still mapped, which then fails.
 */
int32_t vgpu_blob_unmap(struct virtio_gpu_device *vg, struct virtio_gpu_object *bo)
{
    struct virtio_gpu_cmd *cmd = vgpu_cmd_new(vg, vg->ctrlq, VIRTIO_GPU_CMD_RESOURCE_UNMAP_BLOB,
                                              sizeof(struct virtio_gpu_resource_unref));

    if (!cmd) {
        return -ENOMEM;
    }
    kfree(cmd->fence);
    cmd->fence = NULL;
    ((struct virtio_gpu_resource_unref *)(void *)cmd->page)->resource_id = bo->res_id;
    vgpu_cmd_send(cmd, NULL, NULL);
    return 0;
}

/* ------------------------------------------------------------------------
 * Blobs of host memory alone
 */

/** The physical address of page `pgoff` of a mapped blob of host memory,
 * or 0 when it has none there: past its end, not mapped, its pages taken
 * back or its device gone. */
static uintptr_t host_phys(struct virtio_gpu_object *bo, uint32_t pgoff)
{
    struct virtio_gpu_device *vg = vgpu_device(bo->shmem.base.dev);

    if (bo->shmem.base.dev->unplugged || !bo->map_ok ||
        (uint64_t)pgoff >= bo->shmem.base.size >> PAGE_SHIFT) {
        return 0;
    }
    if (bo->pages_map) {
        return virtio_map_frame(vg->vdev, bo->pages_map, pgoff);
    }
    return (uintptr_t)(vg->hostmem_addr + bo->hv_offset + ((uint64_t)pgoff << PAGE_SHIFT));
}

/** A mapping's page of a blob of host memory: its page of the host-visible
 * region. */
static int host_vm_page(void *obj, uint32_t pgoff, uintptr_t *phys)
{
    uintptr_t p = host_phys((struct virtio_gpu_object *)obj, pgoff);

    if (!p) {
        return -1;
    }
    *phys = p;
    return 0;
}

/** A mapping's release of its reference to the object. */
static void host_vm_put(void *obj)
{
    drm_gem_object_put((struct drm_gem_object *)obj);
}

static const struct vm_device_ops host_vm_ops = {host_vm_page, host_vm_put, NULL, 1};

/**
 * The `mmap` of a blob of host memory, Linux's virtio_gpu_vram_mmap: the
 * whole of a mappable blob that MAP_BLOB placed, with the cache type the
 * host answered, write-combining or uncached, else cached, and every page
 * mapped at the first fault (`whole`), as Linux's io_remap_pfn_range maps
 * them at the mmap. EINVAL for a blob not mappable or not mapped, or a
 * mapping of part of it.
 */
static int32_t host_mmap(struct drm_gem_object *obj, vma_t *vma)
{
    struct virtio_gpu_object *bo = (struct virtio_gpu_object *)obj;
    uint32_t cache = 0;

    if (!(bo->blob_flags & VIRTGPU_BLOB_FLAG_USE_MAPPABLE) || !bo->map_ok || vma->pgoff != 0 ||
        (uint64_t)(vma->end - vma->start) != obj->size) {
        return -EINVAL;
    }
    if (bo->map_info == VIRTIO_GPU_MAP_CACHE_WC) {
        cache = VM_PROT_WC;
    } else if (bo->map_info == VIRTIO_GPU_MAP_CACHE_UNCACHED) {
        cache = VM_PROT_UC;
    }
    return vm_device_map(vma, &host_vm_ops, obj, cache);
}

/** A blob's page for a PRIME importer, or 0: none of a blob mapped from
 * the device's pages, whose taking back would not reach an importer. */
static uintptr_t host_page(struct drm_gem_object *obj, uint32_t pgoff)
{
    if (((struct virtio_gpu_object *)obj)->pages_map) {
        return 0;
    }
    return host_phys((struct virtio_gpu_object *)obj, pgoff);
}

/**
 * The kernel's address of a mapped blob: a window of the MMIO region onto
 * its piece of the host-visible region alone, made at the first call with
 * the host's cache type and kept until the blob is freed. NULL when it is
 * not mapped, the region has no room for it, or it is mapped from the
 * device's pages, which have no window, as Linux's blobs of host memory
 * have no vmap.
 */
static volatile uint8_t *host_vmap(struct drm_gem_object *obj)
{
    struct virtio_gpu_object *bo = (struct virtio_gpu_object *)obj;
    struct virtio_gpu_device *vg = vgpu_device(obj->dev);
    pte_t cache = 0;

    if (!bo->map_ok || obj->dev->unplugged || bo->pages_map) {
        return NULL;
    }
    if (!bo->vmap) {
        if (bo->map_info == VIRTIO_GPU_MAP_CACHE_WC) {
#ifdef PTE_WC
            cache = PTE_WC;
#else
            cache = PTE_NOCACHE; /* no write-combining here: uncached */
#endif
        } else if (bo->map_info == VIRTIO_GPU_MAP_CACHE_UNCACHED) {
            cache = PTE_NOCACHE;
        }
        bo->vmap = mmio_map(vg->hostmem_addr + bo->hv_offset, (size_t)obj->size, cache);
    }
    return (volatile uint8_t *)bo->vmap;
}

const struct drm_gem_object_funcs vgpu_host_funcs = {vgpu_obj_free, host_mmap, NULL, NULL,
                                                     host_page,     host_vmap, NULL, vgpu_obj_open};

/* ------------------------------------------------------------------------
 * RESOURCE_CREATE_BLOB
 */

/**
 * Check a RESOURCE_CREATE_BLOB as Linux's verify_blob does: the device has
 * blobs, the flags are known, the memory is guest, host3d or both, host
 * memory comes with virgl, and a blob of guest memory alone has no blob id
 * and no commands. Sets *guest and *host by the memory. 0, or -EINVAL.
 */
static int32_t blob_verify(struct virtio_gpu_device *vg,
                           const struct drm_virtgpu_resource_create_blob *a, int *guest, int *host)
{
    if (!vg->has_blob || (a->blob_flags & ~VIRTGPU_BLOB_FLAG_USE_MASK) || a->cmd_size % 4) {
        return -EINVAL;
    }
    *guest = a->blob_mem == VIRTGPU_BLOB_MEM_GUEST || a->blob_mem == VIRTGPU_BLOB_MEM_HOST3D_GUEST;
    *host = a->blob_mem == VIRTGPU_BLOB_MEM_HOST3D || a->blob_mem == VIRTGPU_BLOB_MEM_HOST3D_GUEST;
    if (!*guest && !*host) {
        return -EINVAL;
    }
    if (*host ? !vg->has_virgl : (a->blob_id != 0 || a->cmd_size != 0)) {
        return -EINVAL;
    }
    if (a->size == 0 || a->size > SHMEM_MAX_SIZE) {
        return -EINVAL;
    }
    return 0;
}

/**
 * Queue the commands a blob of host memory is made from, `size` bytes at
 * the user's `user`, as a SUBMIT_3D in the file's context, ahead of the
 * CREATE_BLOB that names them by blob id. 0, -ENOMEM, -EFAULT or -E2BIG.
 */
static int32_t blob_commands(struct virtio_gpu_device *vg, uint32_t ctx_id, uint64_t user,
                             uint32_t size)
{
    struct virtio_gpu_cmd *cmd =
        vgpu_cmd_new(vg, vg->ctrlq, VIRTIO_GPU_CMD_SUBMIT_3D, sizeof(struct virtio_gpu_submit_3d));
    struct virtio_gpu_submit_3d *req;
    int32_t rc;

    if (!cmd) {
        return -ENOMEM;
    }
    req = (struct virtio_gpu_submit_3d *)(void *)cmd->page;
    req->hdr.ctx_id = ctx_id;
    req->size = size;
    rc = vgpu_cmd_payload(cmd, user, size, 0);
    if (rc != 0) {
        vgpu_cmd_free(cmd);
        return rc;
    }
    vgpu_cmd_send(cmd, NULL, NULL);
    return 0;
}

/**
 * Queue `bo`'s RESOURCE_CREATE_BLOB under resource id `id`, with its pages'
 * entries for a blob with guest memory, without a wait, as Linux's
 * virtio_gpu_cmd_resource_create_blob, and put its fence in the object's
 * reservation. Once queued, `bo` has the id, which its UNREF undoes, and
 * an error answer is logged. 0, or the entries' -ENOMEM, -EINVAL or -E2BIG
 * with nothing queued and no id given, or the reservation's -ENOMEM after
 * the queueing. May sleep.
 */
static int32_t blob_create_cmd(struct virtio_gpu_device *vg, struct virtio_gpu_object *bo,
                               const struct drm_virtgpu_resource_create_blob *a, uint32_t ctx_id,
                               uint32_t id)
{
    struct virtio_gpu_resource_create_blob *req;
    struct virtio_gpu_cmd *cmd =
        vgpu_cmd_new(vg, vg->ctrlq, VIRTIO_GPU_CMD_RESOURCE_CREATE_BLOB, sizeof(*req));
    uint32_t n = 0;
    int32_t rc;

    if (!cmd) {
        return -ENOMEM;
    }
    if (a->blob_mem != VIRTGPU_BLOB_MEM_HOST3D) {
        rc = vgpu_backing_entries(cmd, bo, &n);
        if (rc != 0) {
            vgpu_cmd_free(cmd);
            return rc;
        }
    }
    req = (struct virtio_gpu_resource_create_blob *)(void *)cmd->page;
    req->hdr.ctx_id = a->blob_mem == VIRTGPU_BLOB_MEM_GUEST ? 0 : ctx_id;
    req->resource_id = id;
    req->blob_mem = a->blob_mem;
    req->blob_flags = a->blob_flags;
    req->nr_entries = n;
    req->blob_id = a->blob_mem == VIRTGPU_BLOB_MEM_GUEST ? 0 : a->blob_id;
    req->size = bo->shmem.base.size;
    vgpu_cmd_prepare(cmd);
    rc = dma_resv_add_fence(bo->shmem.base.resv, cmd->fence, 1);
    vgpu_cmd_send(cmd, NULL, NULL);
    bo->res_id = id;
    return rc;
}

/** The answer to ASSIGN_UUID: the blob's UUID, kept, and the hold on the
 * blob that the command kept dropped. */
static void uuid_done(struct virtio_gpu_cmd *cmd)
{
    struct virtio_gpu_object *bo = (struct virtio_gpu_object *)cmd->arg;
    const struct virtio_gpu_resp_resource_uuid *resp =
        (const struct virtio_gpu_resp_resource_uuid *)(const void *)(cmd->page +
                                                                     VIRTIO_GPU_RESP_OFF);

    if (cmd->resp_type == VIRTIO_GPU_RESP_OK_RESOURCE_UUID && cmd->response_len >= sizeof(*resp)) {
        k_memcpy(bo->uuid, resp->uuid, sizeof(bo->uuid));
        bo->uuid_ok = 1;
    }
    drm_gem_object_put(&bo->shmem.base);
}

/**
 * Queue RESOURCE_ASSIGN_UUID for a shareable blob, as Linux's
 * virtio_gpu_resource_assign_uuid, where the device gives UUIDs; the blob
 * is held until the answer. 0, or -ENOMEM.
 */
static int32_t blob_uuid(struct virtio_gpu_device *vg, struct virtio_gpu_object *bo)
{
    struct virtio_gpu_cmd *cmd;

    if (!vg->has_uuid) {
        return 0;
    }
    cmd = vgpu_cmd_new(vg, vg->ctrlq, VIRTIO_GPU_CMD_RESOURCE_ASSIGN_UUID,
                       sizeof(struct virtio_gpu_resource_unref));
    if (!cmd) {
        return -ENOMEM;
    }
    ((struct virtio_gpu_resource_unref *)(void *)cmd->page)->resource_id = bo->res_id;
    drm_gem_object_get(&bo->shmem.base);
    vgpu_cmd_send(cmd, uuid_done, bo);
    return 0;
}

/**
 * Make the blob `a` asks for in the context `ctx_id`: an object of its
 * size in whole pages, of shmem pages when it has guest memory, its
 * CREATE_BLOB, and for a mappable blob of host memory alone its MAP_BLOB.
 * One reference, the caller's; NULL with *err set. May sleep.
 */
static struct virtio_gpu_object *blob_create(struct virtio_gpu_device *vg,
                                             const struct drm_virtgpu_resource_create_blob *a,
                                             uint32_t ctx_id, int32_t *err)
{
    struct virtio_gpu_object *bo = (struct virtio_gpu_object *)kzalloc(sizeof(*bo));
    uint64_t size = (a->size + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1);
    uint32_t id;

    if (!bo) {
        *err = -ENOMEM;
        return NULL;
    }
    vgpu_obj_init(vg, bo, size, a->blob_mem == VIRTGPU_BLOB_MEM_HOST3D);
    bo->blob_mem = a->blob_mem;
    bo->blob_flags = a->blob_flags;
    id = vgpu_res_id_get(vg);
    *err = id ? blob_create_cmd(vg, bo, a, ctx_id, id) : -ENOSPC;
    if (id && !bo->res_id) {
        vgpu_res_id_put(vg, id);
    }
    if (*err == 0 && a->blob_mem == VIRTGPU_BLOB_MEM_HOST3D &&
        (a->blob_flags & VIRTGPU_BLOB_FLAG_USE_MAPPABLE)) {
        *err = blob_map(vg, bo);
    }
    if (*err == 0 && (a->blob_flags & VIRTGPU_BLOB_FLAG_USE_SHAREABLE)) {
        *err = blob_uuid(vg, bo);
    }
    if (*err == 0 && a->blob_mem == VIRTGPU_BLOB_MEM_HOST3D) {
        *err = drm_gem_create_mmap_offset(&bo->shmem.base);
    }
    if (*err != 0) {
        drm_gem_object_put(&bo->shmem.base);
        return NULL;
    }
    return bo;
}

/**
 * RESOURCE_CREATE_BLOB, Linux's virtio_gpu_resource_create_blob_ioctl: the
 * blob is checked (`blob_verify`), the file's context made under virgl, the
 * commands that make a blob of host memory queued in it, the blob made
 * (`blob_create`), attached to the context, and given a handle. EINVAL for
 * a blob the device cannot make, ENODEV for a dead device, or the error of
 * a step. May sleep.
 */
int32_t vgpu_blob_ioctl(struct drm_device *dev, void *data, struct drm_file *file)
{
    struct virtio_gpu_device *vg = vgpu_device(dev);
    struct drm_virtgpu_resource_create_blob *a = data;
    struct virtio_gpu_object *bo;
    struct vgpu_context *ctx;
    uint32_t handle = 0, ctx_id = 0;
    int guest, host;
    int32_t rc;

    if (vg->dead) {
        return -ENODEV;
    }
    if (blob_verify(vg, a, &guest, &host) != 0) {
        return -EINVAL;
    }
    if (vg->has_virgl) {
        rc = vgpu_context_ensure(vg, file);
        if (rc != 0) {
            return rc;
        }
        ctx = file->driver_priv;
        ctx_id = ctx ? ctx->id : 0;
    }
    if (a->cmd_size) {
        rc = blob_commands(vg, ctx_id, a->cmd, a->cmd_size);
        if (rc != 0) {
            return rc;
        }
    }
    bo = blob_create(vg, a, ctx_id, &rc);
    if (!bo) {
        return rc;
    }
    rc = vg->has_virgl ? vgpu_context_attach(vg, file, bo) : 0;
    if (rc == 0) {
        rc = drm_gem_handle_create(file, &bo->shmem.base, &handle);
    }
    if (rc == 0) {
        a->res_handle = bo->res_id;
        a->bo_handle = handle;
    }
    drm_gem_object_put(&bo->shmem.base);
    return rc;
}

/* ------------------------------------------------------------------------
 * CONTEXT_INIT
 */

/* The parameters of one CONTEXT_INIT, gathered before any takes effect. */
struct ctx_params {
    uint32_t capset;
    uint32_t rings;
    int rings_set;
    uint64_t mask;
    char name[VGPU_DEBUG_NAME_MAX];
    int name_set;
};

/**
 * Take one parameter of CONTEXT_INIT into `c`, refusing as Linux's
 * virtio_gpu_context_init_ioctl does a capability set the device lacks or
 * past 63, more than 64 rings, and a parameter given twice. 0, -EINVAL, or
 * -EFAULT for a name that cannot be read.
 */
static int32_t ctx_param(struct virtio_gpu_device *vg,
                         const struct drm_virtgpu_context_set_param *p, struct ctx_params *c)
{
    int rc;

    switch (p->param) {
    case VIRTGPU_CONTEXT_PARAM_CAPSET_ID:
        if (p->value > VGPU_MAX_CAPSET_ID || p->value >= 32 ||
            !(vg->capset_mask & (1u << p->value)) || c->capset) {
            return -EINVAL;
        }
        c->capset = (uint32_t)p->value;
        return 0;
    case VIRTGPU_CONTEXT_PARAM_NUM_RINGS:
        if (c->rings_set || p->value > VGPU_MAX_RINGS) {
            return -EINVAL;
        }
        c->rings = (uint32_t)p->value;
        c->rings_set = 1;
        return 0;
    case VIRTGPU_CONTEXT_PARAM_POLL_RINGS_MASK:
        if (c->mask) {
            return -EINVAL;
        }
        c->mask = p->value;
        return 0;
    case VIRTGPU_CONTEXT_PARAM_DEBUG_NAME:
        if (c->name_set) {
            return -EINVAL;
        }
        rc = strncpy_from_user(c->name, (const char __user *)(uintptr_t)p->value,
                               sizeof(c->name) - 1);
        if (rc < 0 && rc != -ENAMETOOLONG) {
            return rc;
        }
        c->name[sizeof(c->name) - 1] = 0;
        c->name_set = 1;
        return 0;
    default:
        return -EINVAL;
    }
}

/**
 * CONTEXT_INIT, Linux's virtio_gpu_context_init_ioctl: up to four
 * parameters, the capability set, the number of rings, the mask of rings
 * whose fences send an event, and the context's name, then the context
 * created with them. EINVAL without context initialization or virgl, for
 * more than four parameters, one refused (`ctx_param`) or a poll mask past
 * the rings; EEXIST once the context exists; EFAULT; ENOMEM.
 */
int32_t vgpu_context_init_ioctl(struct drm_device *dev, void *data, struct drm_file *file)
{
    struct virtio_gpu_device *vg = vgpu_device(dev);
    struct drm_virtgpu_context_init *a = data;
    struct drm_virtgpu_context_set_param p[VGPU_CONTEXT_PARAMS];
    struct vgpu_context *ctx = file->driver_priv;
    struct ctx_params c;
    uint32_t i;
    int32_t rc;

    if (!vg->has_context_init || !vg->has_virgl || a->num_params > VGPU_CONTEXT_PARAMS) {
        return -EINVAL;
    }
    if (vg->dead) {
        return -ENODEV;
    }
    if (a->num_params && copy_from_user(p, (const void __user *)(uintptr_t)a->ctx_set_params,
                                        a->num_params * sizeof(p[0])) != 0) {
        return -EFAULT;
    }
    if (!ctx) {
        return -EINVAL;
    }
    if (ctx->created) {
        return -EEXIST;
    }
    k_memset(&c, 0, sizeof(c));
    for (i = 0; i < a->num_params; i++) {
        rc = ctx_param(vg, &p[i], &c);
        if (rc != 0) {
            return rc;
        }
    }
    if (c.rings < 64 && (c.mask >> c.rings) != 0) {
        return -EINVAL;
    }
    ctx->context_init = c.capset;
    if (c.rings_set) {
        ctx->base_fence_ctx = dma_fence_context_alloc(c.rings);
        ctx->num_rings = c.rings;
    }
    ctx->ring_mask = c.mask;
    if (c.name_set) {
        k_memcpy(ctx->debug_name, c.name, sizeof(ctx->debug_name));
    }
    return vgpu_context_ensure(vg, file);
}
