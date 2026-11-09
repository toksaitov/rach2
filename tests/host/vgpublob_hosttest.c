/*
 * vgpublob_hosttest.c: virtio-gpu's blob resources, host-visible memory,
 * CONTEXT_INIT and rings, rach2/sys/drm/virtio_gpu_blob.c, over the 3D
 * model, which this test teaches CREATE_BLOB, MAP_BLOB, UNMAP_BLOB,
 * ASSIGN_UUID and SET_SCANOUT_BLOB; vgpublob64_hosttest.c runs the same
 * rows with x86-64's structures.
 *
 * host-deps: rach2/sys/lib/kstring.c rach2/sys/lib/kctypes.c
 *
 * The rows: the parameters a device with blobs, UUIDs, context
 * initialization and a host-visible region gives; blobs of host memory,
 * whose commands go before their CREATE_BLOB, which MAP_BLOB places first
 * fit in the region, which map whole with the host's cache type, whose
 * pages and kernel window reach their piece alone, and which unmap before
 * their UNREF; a full region and a refused map, which leak nothing; blobs
 * of guest memory and of both, with their entries, contexts, UUIDs,
 * transfers and refusals; a device without context initialization, UUIDs
 * or virgl; CONTEXT_INIT with capability sets 1, 2, 4 and 6, its refusals,
 * which leave the context as it was, and its name; rings, each its own
 * fence context, whose answers retire only their own ring's fences, whose
 * polled ring alone sends an event, and whose dependencies across rings
 * are waited for; PRIME export and import of a blob; and a blob as the
 * scanout. Each row ends with nothing allocated and no window mapped.
 *
 * Returns 0 when every row passes, 1 otherwise.
 */
#define VGPU_BLOB_TEST 1
#include "vgpu3d_hosttest.c"
#include "drm/drm_prime.c"

/* ------------------------------------------------------------------------
 * The kernel around PRIME: dma-bufs on a table of descriptors
 */

#define MAX_BUF_FDS 16
static struct dma_buf *buf_fds[MAX_BUF_FDS];

/** A buffer of `size` bytes with one reference. */
struct dma_buf *dma_buf_export(uint64_t size, const struct dma_buf_ops *ops, void *priv,
                               struct dma_resv *resv)
{
    struct dma_buf *b = (struct dma_buf *)kzalloc(sizeof(*b));

    if (!b) {
        return NULL;
    }
    b->refs = 1;
    b->size = size;
    b->ops = ops;
    b->priv = priv;
    b->resv = resv ? resv : &b->own;
    return b;
}

/** Take a reference. */
struct dma_buf *dma_buf_ref(struct dma_buf *buf)
{
    buf->refs++;
    return buf;
}

/** Drop one; the last releases the exporter's hold and frees the buffer. */
void dma_buf_put(struct dma_buf *buf)
{
    if (buf && --buf->refs == 0) {
        buf->ops->release(buf);
        kfree(buf);
    }
}

/** A descriptor holding its own reference, from 3. */
int32_t dma_buf_fd(struct dma_buf *buf, uint32_t flags)
{
    int32_t i;

    (void)flags;
    for (i = 3; i < MAX_BUF_FDS; i++) {
        if (!buf_fds[i]) {
            buf_fds[i] = dma_buf_ref(buf);
            return i;
        }
    }
    return -EMFILE;
}

/** The buffer of descriptor `fd`, referenced, or NULL. */
struct dma_buf *dma_buf_get(int32_t fd)
{
    return fd >= 0 && fd < MAX_BUF_FDS && buf_fds[fd] ? dma_buf_ref(buf_fds[fd]) : NULL;
}

/** Close every buffer descriptor. */
static void buf_fds_close(void)
{
    int i;

    for (i = 0; i < MAX_BUF_FDS; i++) {
        dma_buf_put(buf_fds[i]);
        buf_fds[i] = NULL;
    }
}

/** No other descriptors. */
fd_entry_t *fd_get(fd_table_t *t, int fd)
{
    (void)t;
    (void)fd;
    return NULL;
}

/** Imports of foreign buffers do not happen here. */
struct drm_gem_shmem_object *drm_gem_shmem_create(struct drm_device *d, uint64_t size, int wc)
{
    (void)d;
    (void)size;
    (void)wc;
    CHECK(0, "a self-import makes no new object");
    return NULL;
}

/* ------------------------------------------------------------------------
 * The device's blobs
 */

/* What the model knows of each resource that is a blob, by its slot of
 * gpu.res. */
static struct {
    uint32_t mem, flags, ctx, mapped;
    uint64_t blob_id, size, offset;
} blobs[MAX_RES];
static uint32_t map_cache; /* the cache type MAP_BLOB answers */
static uint32_t uuids;     /* ASSIGN_UUIDs answered */
static struct virtio_gpu_set_scanout_blob shown_blob;

/** The model's slot of resource `id`, or -1. */
static int slot_of(uint32_t id)
{
    struct model_res *r = res_find(id);

    return r ? (int)(r - gpu.res) : -1;
}

/** CREATE_BLOB of `n` bytes: a resource of the blob's bytes, with its
 * entries when it has guest memory. The answer's type. */
static uint32_t exec_create_blob(const uint8_t *req, uint32_t n)
{
    const struct virtio_gpu_resource_create_blob *c = (const void *)req;
    const struct virtio_gpu_mem_entry *e = (const void *)(c + 1);
    uint64_t total = 0;
    uint32_t i, k;

    CHECK(n == sizeof(*c) + c->nr_entries * sizeof(*e), "CREATE_BLOB carries exactly its entries");
    CHECK(c->size && !(c->size % PAGE_SIZE), "a blob is a whole number of pages");
    if (!c->resource_id || res_find(c->resource_id)) {
        return VIRTIO_GPU_RESP_ERR_INVALID_RESOURCE_ID;
    }
    if (c->blob_mem != VIRTIO_GPU_BLOB_MEM_GUEST && !context_present(c->hdr.ctx_id)) {
        return VIRTIO_GPU_RESP_ERR_INVALID_CONTEXT_ID;
    }
    for (i = 0; i < MAX_RES && gpu.res[i].id; i++) {
    }
    CHECK(i < MAX_RES && !blobs[i].mapped, "the model has room, and no freed blob stays mapped");
    if (i >= MAX_RES) {
        return VIRTIO_GPU_RESP_ERR_OUT_OF_MEMORY;
    }
    gpu.res[i].id = c->resource_id;
    gpu.res[i].width = (uint32_t)(c->size / 4);
    gpu.res[i].height = 1;
    gpu.res[i].pixels = (uint8_t *)calloc(1, (unsigned long)c->size);
    if (c->nr_entries) {
        gpu.res[i].nents = c->nr_entries;
        gpu.res[i].ents = (struct virtio_gpu_mem_entry *)calloc(c->nr_entries, sizeof(*e));
        k_memcpy(gpu.res[i].ents, e, c->nr_entries * sizeof(*e));
        for (k = 0; k < c->nr_entries; k++) {
            total += e[k].length;
        }
        CHECK(total == c->size, "a blob's entries cover its size");
    }
    k_memset(&blobs[i], 0, sizeof(blobs[i]));
    blobs[i].mem = c->blob_mem;
    blobs[i].flags = c->blob_flags;
    blobs[i].ctx = c->hdr.ctx_id;
    blobs[i].blob_id = c->blob_id;
    blobs[i].size = c->size;
    return VIRTIO_GPU_RESP_OK_NODATA;
}

/** MAP_BLOB, as QEMU's virgl maps a blob: the whole blob at the offset,
 * which must lie in the region and meet no other mapped blob; its bytes
 * are a pattern of its id. A MAP_BLOB the row fails maps nothing. The
 * answer's length. */
static uint32_t exec_map_blob(const uint8_t *req, uint32_t n, uint8_t *resp, uint32_t *ok)
{
    const struct virtio_gpu_resource_map_blob *m = (const void *)req;
    int i = slot_of(m->resource_id), j;
    uint64_t k;

    CHECK(n == sizeof(*m) && i >= 0 && blobs[i].mem == VIRTIO_GPU_BLOB_MEM_HOST3D &&
              !blobs[i].mapped,
          "MAP_BLOB names an unmapped blob of host memory alone");
    if (i < 0 || blobs[i].mapped || gpu.fail_type == VIRTIO_GPU_CMD_RESOURCE_MAP_BLOB) {
        *ok = VIRTIO_GPU_RESP_ERR_INVALID_RESOURCE_ID;
        return 24;
    }
    CHECK(!(m->offset % PAGE_SIZE) && m->offset + blobs[i].size <= gpu.hostmem_len,
          "a blob's piece lies in the host-visible region");
    for (j = 0; j < MAX_RES; j++) {
        CHECK(j == i || !blobs[j].mapped || m->offset + blobs[i].size <= blobs[j].offset ||
                  blobs[j].offset + blobs[j].size <= m->offset,
              "mapped blobs do not overlap");
    }
    blobs[i].mapped = 1;
    blobs[i].offset = m->offset;
    for (k = 0; k < blobs[i].size && m->offset + k < HOSTMEM_BACKING; k++) {
        hostmem[m->offset + k] = (uint8_t)(k * 3 + m->resource_id);
    }
    ((struct virtio_gpu_resp_map_info *)(void *)resp)->map_info = map_cache;
    *ok = VIRTIO_GPU_RESP_OK_MAP_INFO;
    return sizeof(struct virtio_gpu_resp_map_info);
}

/** The blob commands the 3D model does not know. */
static uint32_t execute_blob(const uint8_t *req, uint32_t n, uint8_t *resp, uint32_t *ok)
{
    const struct virtio_gpu_ctrl_hdr *hdr = (const void *)req;
    const struct virtio_gpu_resource_unref *u = (const void *)req;
    struct virtio_gpu_resp_resource_uuid *uuid = (void *)resp;
    int i, k;

    switch (hdr->type) {
    case VIRTIO_GPU_CMD_RESOURCE_CREATE_BLOB:
        *ok = exec_create_blob(req, n);
        return 24;
    case VIRTIO_GPU_CMD_RESOURCE_MAP_BLOB:
        return exec_map_blob(req, n, resp, ok);
    case VIRTIO_GPU_CMD_RESOURCE_UNMAP_BLOB:
        i = slot_of(u->resource_id);
        CHECK(n == 32 && i >= 0 && blobs[i].mapped, "UNMAP_BLOB names a mapped blob");
        if (i < 0 || !blobs[i].mapped) {
            *ok = VIRTIO_GPU_RESP_ERR_UNSPEC;
        } else {
            blobs[i].mapped = 0;
        }
        return 24;
    case VIRTIO_GPU_CMD_RESOURCE_ASSIGN_UUID:
        CHECK(n == 32 && res_find(u->resource_id), "ASSIGN_UUID names a resource");
        for (k = 0; k < 16; k++) {
            uuid->uuid[k] = (uint8_t)(u->resource_id * 16 + k);
        }
        uuids++;
        *ok = VIRTIO_GPU_RESP_OK_RESOURCE_UUID;
        return sizeof(*uuid);
    case VIRTIO_GPU_CMD_SET_SCANOUT_BLOB:
        shown_blob = *(const struct virtio_gpu_set_scanout_blob *)(const void *)req;
        CHECK(n == sizeof(shown_blob) && shown_blob.scanout_id < gpu.config.num_scanouts &&
                  res_find(shown_blob.resource_id),
              "SET_SCANOUT_BLOB names a scanout and a resource");
        gpu.shown[shown_blob.scanout_id].res = shown_blob.resource_id;
        gpu.shown[shown_blob.scanout_id].r = shown_blob.r;
        return 24;
    default:
        *ok = VIRTIO_GPU_RESP_ERR_UNSPEC;
        return 24;
    }
}

/* ------------------------------------------------------------------------
 * The rows
 */

#define ALL_FEATURES                                                                   \
    (VIRTIO_BIT(VIRTIO_GPU_F_RESOURCE_UUID) | VIRTIO_BIT(VIRTIO_GPU_F_RESOURCE_BLOB) | \
     VIRTIO_BIT(VIRTIO_GPU_F_CONTEXT_INIT))
#define HOSTMEM_LEN (64u * PAGE_SIZE)
#define VIRGL       VIRTIO_BIT(VIRTIO_GPU_F_VIRGL)

static struct drm_file file2;

/** Bind a device offering `features` with capability sets 1, 2, 4 and 6
 * under virgl, and a host-visible region of `region` bytes, and open the
 * file. */
static void blob_bind(uint64_t features, uint64_t region)
{
    static const uint32_t ids[4] = {1, 2, 4, 6};

    model_reset();
    k_memset(&three, 0, sizeof(three));
    k_memset(blobs, 0, sizeof(blobs));
    k_memset(hostmem, 0, sizeof(hostmem));
    map_cache = VIRTIO_GPU_MAP_CACHE_WC;
    uuids = 0;
    gpu.offered |= features;
    gpu.hostmem_len = region;
    if (features & VIRGL) {
        k_memcpy(three.capsets, ids, sizeof(ids));
        three.ncapsets = 4;
        gpu.config.num_capsets = 4;
    }
    CHECK(bind() == 1 && vgpu_open(dev, &file) == 0, "a blob device binds and a file opens");
}

/** Open another file of the device. */
static void open_file(struct drm_file *f)
{
    k_memset(f, 0, sizeof(*f));
    f->dev = dev;
    f->authenticated = 1;
    f->event_space = 4096;
    CHECK(vgpu_open(dev, f) == 0, "another file opens");
}

/** Close a file opened by open_file: its handles and its context. */
static void close_file(struct drm_file *f)
{
    int i;

    for (i = 0; i < MAX_HANDLES; i++) {
        if (handles[i].file == f) {
            (void)drm_gem_handle_delete(f, handles[i].id);
        }
    }
    vgpu_context_close(dev, f);
}

/** Answer every command still out, and reap them. */
static void answer_all(void)
{
    uint32_t i;

    for (i = 0; i < gpu.q[0].nflight; i++) {
        gpu.q[0].flight[i].answered = 1;
    }
    gpu.answer = ANSWER_KICK;
    vgpu_reap(vg);
}

/** Close the file, unbind and check that nothing is left: no allocation,
 * page, window, sync file, buffer or lock. */
static void blob_unbind(const char *row)
{
    uint32_t i;

    answer_all();
    buf_fds_close();
    for (i = 0; i < 32; i++) {
        dma_fence_put(sync_fds[i]);
        sync_fds[i] = NULL;
    }
    vgpu_context_close(dev, &file);
    unbind();
    CHECK(!gpu.windows, row);
    model_reset();
    CHECK(!allocations && !pages_held && !irq_depth, row);
}

/** CREATE_BLOB of `size` bytes of `mem` with `flags` and `id` in `f`, with
 * `cmd_size` bytes of commands at `cmd`: the ioctl's result, and the
 * handle in *handle. */
static int32_t blob_in(struct drm_file *f, uint32_t mem, uint32_t flags, uint64_t size, uint64_t id,
                       uint32_t *handle)
{
    struct drm_virtgpu_resource_create_blob a;
    int32_t rc;

    k_memset(&a, 0, sizeof(a));
    a.blob_mem = mem;
    a.blob_flags = flags;
    a.size = size;
    a.blob_id = id;
    rc = vgpu_blob_ioctl(dev, &a, f);
    *handle = rc == 0 ? a.bo_handle : 0;
    CHECK(rc != 0 || a.res_handle, "a blob has a resource id");
    return rc;
}

/** The same in the first file. */
static int32_t blob(uint32_t mem, uint32_t flags, uint64_t size, uint64_t id, uint32_t *handle)
{
    return blob_in(&file, mem, flags, size, id, handle);
}

/** The object of `handle` of the first file, which the handle holds. */
static struct drm_gem_object *obj_of(uint32_t handle)
{
    struct drm_gem_object *obj = drm_gem_object_lookup(&file, handle);

    if (obj) {
        drm_gem_object_put(obj);
    }
    return obj;
}

/** mmap of `obj` from page `pgoff`, `len` bytes, as drm_gem_mmap calls it
 * with a reference the mapping takes over: the hook's result. */
static int32_t map_obj(struct drm_gem_object *obj, uint32_t pgoff, uint64_t len)
{
    vma_t vma;
    int32_t rc;

    k_memset(&vma, 0, sizeof(vma));
    vma.start = 0x40000000u;
    vma.end = vma.start + (uintptr_t)len;
    vma.pgoff = pgoff;
    drm_gem_object_get(obj);
    rc = obj->funcs->mmap(obj, &vma);
    if (rc) {
        drm_gem_object_put(obj);
    }
    return rc;
}

/** Index of the first command `type` at or after `from` in the log, or
 * MAX_LOG. */
static uint32_t log_find(uint32_t type, uint32_t from)
{
    uint32_t i;

    for (i = from; i < gpu.nlog; i++) {
        if (gpu.log[i] == type) {
            return i;
        }
    }
    return MAX_LOG;
}

/** The value of GETPARAM `param`, or -1 on an error. */
static int32_t param(uint64_t param)
{
    struct drm_virtgpu_getparam p;
    int32_t value = -1;

    p.param = param;
    p.value = UPTR(&value);
    return vgpu_getparam_ioctl(dev, &p, &file) == 0 ? value : -1;
}

/** RESOURCE_INFO's blob_mem of `handle`, or -1. */
static int32_t info_mem(uint32_t handle)
{
    struct drm_virtgpu_resource_info ri;

    k_memset(&ri, 0, sizeof(ri));
    ri.bo_handle = handle;
    return vgpu_resource_info_ioctl(dev, &ri, &file) == 0 ? (int32_t)ri.blob_mem : -1;
}

/** CONTEXT_INIT of `f` with `n` parameters of `params`, values `values`. */
static int32_t context_init(struct drm_file *f, uint32_t n, const uint64_t *params,
                            const uint64_t *values)
{
    struct drm_virtgpu_context_set_param p[8];
    struct drm_virtgpu_context_init a;
    uint32_t i;

    for (i = 0; i < n && i < 8; i++) {
        p[i].param = params[i];
        p[i].value = values[i];
    }
    a.num_params = n;
    a.pad = 0;
    a.ctx_set_params = UPTR(p);
    return vgpu_context_init_ioctl(dev, &a, f);
}

/** The capability set the model's CTX_CREATE recorded for context `id`. */
static uint32_t context_init_of(uint32_t id)
{
    uint32_t i;

    for (i = 0; i < 64; i++) {
        if (id && three.contexts[i] == id) {
            return three.context_init[i];
        }
    }
    return 0xffffffffu;
}

/** The device's parameters and region. */
static void row_params(void)
{
    blob_bind(VIRGL | ALL_FEATURES, HOSTMEM_LEN);
    CHECK(vg->has_blob && vg->has_context_init && vg->has_uuid && vg->has_host_visible,
          "blobs, context init, UUIDs and the region are negotiated");
    CHECK(vg->hostmem_addr == HOSTMEM_ADDR && vg->hostmem_len == HOSTMEM_LEN && !gpu.windows,
          "the region is found at 14 TiB and none of it is mapped");
    CHECK(param(VIRTGPU_PARAM_RESOURCE_BLOB) == 1 && param(VIRTGPU_PARAM_HOST_VISIBLE) == 1 &&
              param(VIRTGPU_PARAM_CROSS_DEVICE) == 1 && param(VIRTGPU_PARAM_CONTEXT_INIT) == 1 &&
              param(VIRTGPU_PARAM_EXPLICIT_DEBUG_NAME) == 1,
          "GETPARAM gives blobs, host-visible memory, cross-device, context init and names");
    CHECK(param(VIRTGPU_PARAM_SUPPORTED_CAPSET_IDs) == 0x56,
          "the capability set mask has ids 1, 2, 4 and 6");
    blob_unbind("the parameter row leaves nothing");
}

/** Make a mappable blob of host memory with commands, and check what
 * reached the host, in order: the blob's handle. */
static uint32_t host_first(void)
{
    static const uint32_t words[4] = {0x11, 0x22, 0x33, 0x44};
    struct drm_virtgpu_resource_create_blob a;
    uint32_t at, ctx;
    int i;

    k_memset(&a, 0, sizeof(a));
    a.blob_mem = VIRTGPU_BLOB_MEM_HOST3D;
    a.blob_flags = VIRTGPU_BLOB_FLAG_USE_MAPPABLE;
    a.size = 3 * PAGE_SIZE + 100;
    a.blob_id = 7;
    a.cmd = UPTR(words);
    a.cmd_size = sizeof(words);
    CHECK(vgpu_blob_ioctl(dev, &a, &file) == 0, "a mappable blob of host memory is made");
    ctx = ((struct vgpu_context *)file.driver_priv)->id;
    at = log_find(VIRTIO_GPU_CMD_SUBMIT_3D, 0);
    CHECK(at < log_find(VIRTIO_GPU_CMD_RESOURCE_CREATE_BLOB, 0) &&
              log_find(VIRTIO_GPU_CMD_RESOURCE_CREATE_BLOB, 0) <
                  log_find(VIRTIO_GPU_CMD_RESOURCE_MAP_BLOB, 0) &&
              log_find(VIRTIO_GPU_CMD_RESOURCE_MAP_BLOB, 0) <
                  log_find(VIRTIO_GPU_CMD_CTX_ATTACH_RESOURCE, 0) &&
              log_find(VIRTIO_GPU_CMD_CTX_CREATE, 0) < at,
          "the context, the commands, CREATE_BLOB, MAP_BLOB and the attach go in order");
    CHECK(three.submitted_size == sizeof(words) && !memcmp(three.submitted, words, sizeof(words)),
          "the blob's commands reach the host");
    i = slot_of(a.res_handle);
    CHECK(i >= 0 && blobs[i].mem == VIRTIO_GPU_BLOB_MEM_HOST3D &&
              blobs[i].flags == VIRTIO_GPU_BLOB_FLAG_USE_MAPPABLE && blobs[i].blob_id == 7 &&
              blobs[i].size == 4 * PAGE_SIZE && blobs[i].ctx == ctx && !gpu.res[i].nents,
          "CREATE_BLOB has the memory, flags, id, whole pages, context and no entries");
    CHECK(i >= 0 && blobs[i].mapped && blobs[i].offset == 0,
          "the host maps it at the region's start");
    return a.bo_handle;
}

/** The first blob's placement, mapping, pages and kernel window. */
static void host_access(uint32_t ha)
{
    struct drm_gem_object *obj = obj_of(ha);
    struct virtio_gpu_object *bo = vgpu_bo(obj);
    volatile uint8_t *v;

    CHECK(bo && bo->host_visible && bo->hv_offset == 0 && bo->map_ok &&
              bo->map_info == VIRTIO_GPU_MAP_CACHE_WC && obj->map_wc && obj->mmap_offset &&
              obj->size == 4 * PAGE_SIZE,
          "MAP_BLOB places the blob at the region's start, write-combining");
    CHECK(info_mem(ha) == VIRTGPU_BLOB_MEM_HOST3D, "RESOURCE_INFO says host memory");
    maps_cache = 0xffffffffu;
    CHECK(map_obj(obj, 0, obj->size) == 0 && maps_cache == VM_PROT_WC && obj->refs == 1,
          "the whole blob maps write-combining, and the mapping's hold is dropped with it");
    CHECK(map_obj(obj, 0, obj->size - PAGE_SIZE) == -EINVAL &&
              map_obj(obj, 1, obj->size - PAGE_SIZE) == -EINVAL && obj->refs == 1,
          "part of a blob does not map");
    CHECK(obj->funcs->page(obj, 0) == (uintptr_t)HOSTMEM_ADDR &&
              obj->funcs->page(obj, 3) == (uintptr_t)(HOSTMEM_ADDR + 3 * PAGE_SIZE) &&
              obj->funcs->page(obj, 4) == 0,
          "the blob's pages are its piece of the region alone");
    v = obj->funcs->vmap(obj);
    CHECK(v == hostmem && gpu.windows == 1 && gpu.window_cache == PTE_WC &&
              v[5] == (uint8_t)(15 + bo->res_id),
          "the kernel's window is onto the piece, write-combining, with the host's bytes");
    CHECK(obj->funcs->vmap(obj) == v && gpu.windows == 1, "the window is made once");
}

/** An unmappable blob, which is not placed, and an uncached one, placed
 * after the first. */
static void host_others(void)
{
    struct drm_gem_object *obj;
    uint32_t hb, hc;

    CHECK(blob(VIRTGPU_BLOB_MEM_HOST3D, 0, PAGE_SIZE, 8, &hb) == 0, "an unmappable blob is made");
    obj = obj_of(hb);
    CHECK(log_find(VIRTIO_GPU_CMD_RESOURCE_MAP_BLOB,
                   log_find(VIRTIO_GPU_CMD_RESOURCE_MAP_BLOB, 0) + 1) == MAX_LOG &&
              !vgpu_bo(obj)->host_visible,
          "an unmappable blob is not placed");
    CHECK(map_obj(obj, 0, obj->size) == -EINVAL && !obj->funcs->page(obj, 0) &&
              !obj->funcs->vmap(obj) && gpu.windows == 1,
          "an unmappable blob has no mapping, pages or window");
    map_cache = VIRTIO_GPU_MAP_CACHE_UNCACHED;
    CHECK(blob(VIRTGPU_BLOB_MEM_HOST3D, VIRTGPU_BLOB_FLAG_USE_MAPPABLE, 2 * PAGE_SIZE, 9, &hc) == 0,
          "an uncached blob is made");
    map_cache = VIRTIO_GPU_MAP_CACHE_WC;
    obj = obj_of(hc);
    CHECK(vgpu_bo(obj)->hv_offset == 4 * PAGE_SIZE && !obj->map_wc,
          "the next blob is placed after the first");
    CHECK(map_obj(obj, 0, obj->size) == 0 && maps_cache == VM_PROT_UC,
          "an uncached blob maps uncached");
    CHECK(obj->funcs->vmap(obj) == hostmem + 4 * PAGE_SIZE && gpu.windows == 2 &&
              gpu.window_cache == PTE_NOCACHE,
          "its window is uncached, onto its own piece");
}

/** Free the first blob, whose piece is the first fit again. */
static void host_release(uint32_t ha)
{
    uint32_t before = gpu.nlog, at, hd, he;

    CHECK(drm_gem_handle_delete(&file, ha) == 0, "the first blob's handle closes");
    vgpu_reap(vg);
    at = log_find(VIRTIO_GPU_CMD_RESOURCE_UNMAP_BLOB, before);
    CHECK(at < MAX_LOG && log_find(VIRTIO_GPU_CMD_RESOURCE_UNREF, before) > at &&
              log_find(VIRTIO_GPU_CMD_RESOURCE_UNREF, before) < MAX_LOG,
          "a placed blob is unmapped before its UNREF");
    CHECK(gpu.windows == 1, "its window goes with it");
    CHECK(blob(VIRTGPU_BLOB_MEM_HOST3D, VIRTGPU_BLOB_FLAG_USE_MAPPABLE, PAGE_SIZE, 10, &hd) == 0 &&
              vgpu_bo(obj_of(hd))->hv_offset == 0,
          "the freed piece is the first fit again");
    CHECK(blob(VIRTGPU_BLOB_MEM_HOST3D, VIRTGPU_BLOB_FLAG_USE_MAPPABLE, 4 * PAGE_SIZE, 11, &he) ==
                  0 &&
              vgpu_bo(obj_of(he))->hv_offset == 6 * PAGE_SIZE,
          "a blob too big for the gap goes after the last");
}

/** A blob the region cannot hold, and a MAP_BLOB the host refuses. */
static void host_failures(void)
{
    uint32_t before = gpu.nlog, hf;
    int32_t rc;

    rc = blob(VIRTGPU_BLOB_MEM_HOST3D, VIRTGPU_BLOB_FLAG_USE_MAPPABLE, HOSTMEM_LEN, 12, &hf);
    vgpu_reap(vg);
    CHECK(rc == -ENOSPC && log_find(VIRTIO_GPU_CMD_RESOURCE_UNREF, before) < MAX_LOG &&
              log_find(VIRTIO_GPU_CMD_RESOURCE_MAP_BLOB, before) == MAX_LOG &&
              log_find(VIRTIO_GPU_CMD_RESOURCE_UNMAP_BLOB, before) == MAX_LOG && !vg->dead,
          "a blob the region cannot hold is refused with ENOSPC and its resource unrefed");
    before = gpu.nlog;
    gpu.fail_type = VIRTIO_GPU_CMD_RESOURCE_MAP_BLOB;
    rc = blob(VIRTGPU_BLOB_MEM_HOST3D, VIRTGPU_BLOB_FLAG_USE_MAPPABLE, PAGE_SIZE, 13, &hf);
    gpu.fail_type = 0;
    vgpu_reap(vg);
    CHECK(rc != 0 && log_find(VIRTIO_GPU_CMD_RESOURCE_UNREF, before) < MAX_LOG &&
              log_find(VIRTIO_GPU_CMD_RESOURCE_UNMAP_BLOB, before) == MAX_LOG && !vg->dead,
          "a refused MAP_BLOB fails the blob, which is unrefed and not unmapped");
    CHECK(blob(VIRTGPU_BLOB_MEM_HOST3D, VIRTGPU_BLOB_FLAG_USE_MAPPABLE, PAGE_SIZE, 14, &hf) == 0 &&
              vgpu_bo(obj_of(hf))->hv_offset == PAGE_SIZE,
          "the refused blob's piece went back");
}

/** Blobs of host memory alone: commands, placement, mapping, pages, the
 * kernel window, their order of release, and failures that leak nothing. */
static void row_host(void)
{
    uint32_t ha;

    blob_bind(VIRGL | ALL_FEATURES, HOSTMEM_LEN);
    ha = host_first();
    host_access(ha);
    host_others();
    host_release(ha);
    host_failures();
    blob_unbind("the host-memory row leaves nothing allocated or mapped");
}

/** Blobs of guest memory, and of both: entries, contexts, UUIDs, transfers
 * and refusals. */
static void row_guest(void)
{
    struct drm_virtgpu_resource_create_blob a;
    struct drm_virtgpu_3d_transfer t;
    struct virtio_gpu_object *bo;
    uint32_t hg, hh, before, ctx;
    int i;

    blob_bind(VIRGL | ALL_FEATURES, HOSTMEM_LEN);
    CHECK(blob(VIRTGPU_BLOB_MEM_GUEST,
               VIRTGPU_BLOB_FLAG_USE_MAPPABLE | VIRTGPU_BLOB_FLAG_USE_SHAREABLE, 2 * PAGE_SIZE, 0,
               &hg) == 0,
          "a shareable blob of guest memory is made");
    vgpu_reap(vg);
    bo = vgpu_bo(obj_of(hg));
    i = slot_of(bo->res_id);
    CHECK(i >= 0 && blobs[i].mem == VIRTIO_GPU_BLOB_MEM_GUEST && blobs[i].ctx == 0 &&
              gpu.res[i].nents >= 1 && !blobs[i].blob_id && !bo->host_visible,
          "a guest blob has its pages' entries and no context or blob id");
    CHECK(uuids == 1 && bo->uuid_ok && bo->uuid[0] == (uint8_t)(bo->res_id * 16) &&
              bo->uuid[15] == (uint8_t)(bo->res_id * 16 + 15),
          "a shareable blob is given the host's UUID");
    CHECK(info_mem(hg) == VIRTGPU_BLOB_MEM_GUEST, "RESOURCE_INFO says guest memory");
    k_memset(&t, 0, sizeof(t));
    t.bo_handle = hg;
    t.box.w = t.box.h = t.box.d = 1;
    CHECK(vgpu_transfer_3d_ioctl(dev, &t, &file, 0) == -EINVAL, "a guest blob takes no transfer");

    before = gpu.nlog;
    k_memset(&a, 0, sizeof(a));
    a.blob_mem = VIRTGPU_BLOB_MEM_GUEST;
    a.size = PAGE_SIZE;
    a.blob_id = 3;
    CHECK(vgpu_blob_ioctl(dev, &a, &file) == -EINVAL, "a guest blob takes no blob id");
    a.blob_id = 0;
    a.cmd = UPTR(&before);
    a.cmd_size = 4;
    CHECK(vgpu_blob_ioctl(dev, &a, &file) == -EINVAL, "a guest blob takes no commands");
    a.cmd_size = 0;
    a.blob_flags = 8;
    CHECK(vgpu_blob_ioctl(dev, &a, &file) == -EINVAL, "an unknown flag is refused");
    a.blob_flags = 0;
    a.blob_mem = 4;
    CHECK(vgpu_blob_ioctl(dev, &a, &file) == -EINVAL, "an unknown memory is refused");
    a.blob_mem = 0;
    CHECK(vgpu_blob_ioctl(dev, &a, &file) == -EINVAL, "no memory is refused");
    a.blob_mem = VIRTGPU_BLOB_MEM_HOST3D;
    a.cmd_size = 3;
    CHECK(vgpu_blob_ioctl(dev, &a, &file) == -EINVAL, "commands not of whole words are refused");
    a.cmd_size = 0;
    a.size = 0;
    CHECK(vgpu_blob_ioctl(dev, &a, &file) == -EINVAL, "an empty blob is refused");
    CHECK(gpu.nlog == before, "a refused blob sends nothing");

    CHECK(blob(VIRTGPU_BLOB_MEM_HOST3D_GUEST, VIRTGPU_BLOB_FLAG_USE_MAPPABLE, PAGE_SIZE, 9, &hh) ==
              0,
          "a blob of host and guest memory is made");
    ctx = ((struct vgpu_context *)file.driver_priv)->id;
    bo = vgpu_bo(obj_of(hh));
    i = slot_of(bo->res_id);
    CHECK(i >= 0 && blobs[i].mem == VIRTIO_GPU_BLOB_MEM_HOST3D_GUEST && blobs[i].ctx == ctx &&
              blobs[i].blob_id == 9 && gpu.res[i].nents >= 1 && !bo->host_visible &&
              !blobs[i].mapped,
          "a blob of both has entries, its context and blob id, and no MAP_BLOB");
    CHECK(info_mem(hh) == VIRTGPU_BLOB_MEM_HOST3D, "RESOURCE_INFO says host memory for both");
    t.bo_handle = hh;
    t.box.w = 16;
    t.stride = 16;
    CHECK(vgpu_transfer_3d_ioctl(dev, &t, &file, 0) == 0 && three.transfer.stride == 16 &&
              three.transfer.w == 16,
          "a blob with host memory takes a transfer with a stride");
    blob_unbind("the guest-memory row leaves nothing");
}

/** Devices without context init, UUIDs or the region, and without virgl. */
static void row_features(void)
{
    uint64_t p[1] = {VIRTGPU_CONTEXT_PARAM_CAPSET_ID}, v[1] = {4};
    uint32_t h, before;
    int i;

    blob_bind(VIRGL | VIRTIO_BIT(VIRTIO_GPU_F_RESOURCE_BLOB), 0);
    CHECK(param(VIRTGPU_PARAM_RESOURCE_BLOB) == 1 && param(VIRTGPU_PARAM_HOST_VISIBLE) == 0 &&
              param(VIRTGPU_PARAM_CROSS_DEVICE) == 0 && param(VIRTGPU_PARAM_CONTEXT_INIT) == 0,
          "GETPARAM gives blobs alone");
    CHECK(context_init(&file, 1, p, v) == -EINVAL, "CONTEXT_INIT needs its feature");
    CHECK(blob(VIRTGPU_BLOB_MEM_GUEST, VIRTGPU_BLOB_FLAG_USE_SHAREABLE, PAGE_SIZE, 0, &h) == 0,
          "a shareable blob is made without UUIDs");
    vgpu_reap(vg);
    CHECK(!uuids && log_find(VIRTIO_GPU_CMD_RESOURCE_ASSIGN_UUID, 0) == MAX_LOG,
          "no ASSIGN_UUID goes to a device without UUIDs");
    before = gpu.nlog;
    CHECK(blob(VIRTGPU_BLOB_MEM_HOST3D, VIRTGPU_BLOB_FLAG_USE_MAPPABLE, PAGE_SIZE, 1, &h) ==
              -EINVAL,
          "a mappable blob of host memory needs the region");
    vgpu_reap(vg);
    CHECK(log_find(VIRTIO_GPU_CMD_RESOURCE_UNREF, before) < MAX_LOG && !vg->dead,
          "and its resource is unrefed");
    blob_unbind("the feature row leaves nothing");

    blob_bind(VIRTIO_BIT(VIRTIO_GPU_F_RESOURCE_BLOB), HOSTMEM_LEN);
    CHECK(!vg->has_virgl && blob(VIRTGPU_BLOB_MEM_GUEST, 0, PAGE_SIZE, 0, &h) == 0,
          "a guest blob is made without virgl");
    i = slot_of(vgpu_bo(obj_of(h))->res_id);
    CHECK(i >= 0 && !blobs[i].ctx && !three.creates, "it needs no context");
    CHECK(blob(VIRTGPU_BLOB_MEM_HOST3D, 0, PAGE_SIZE, 1, &h) == -EINVAL &&
              blob(VIRTGPU_BLOB_MEM_HOST3D_GUEST, 0, PAGE_SIZE, 1, &h) == -EINVAL,
          "host memory needs virgl");
    blob_unbind("the no-virgl row leaves nothing");

    blob_bind(VIRGL, HOSTMEM_LEN);
    CHECK(blob(VIRTGPU_BLOB_MEM_GUEST, 0, PAGE_SIZE, 0, &h) == -EINVAL &&
              param(VIRTGPU_PARAM_RESOURCE_BLOB) == 0,
          "blobs need their feature");
    blob_unbind("the no-blob row leaves nothing");
}

/** CONTEXT_INIT with each capability set, its refusals and its name. */
static void row_context(void)
{
    static const uint32_t ids[4] = {1, 2, 4, 6};
    static const char *names[4] = {"virgl", "virgl2", "venus", "drm"};
    static char long_name[100];
    uint64_t p[5], v[5];
    struct vgpu_context *ctx;
    uint32_t i, creates;

    blob_bind(VIRGL | ALL_FEATURES, HOSTMEM_LEN);
    for (i = 0; i < 4; i++) {
        open_file(&file2);
        p[0] = VIRTGPU_CONTEXT_PARAM_CAPSET_ID;
        v[0] = ids[i];
        p[1] = VIRTGPU_CONTEXT_PARAM_NUM_RINGS;
        v[1] = 2;
        p[2] = VIRTGPU_CONTEXT_PARAM_POLL_RINGS_MASK;
        v[2] = 2;
        p[3] = VIRTGPU_CONTEXT_PARAM_DEBUG_NAME;
        v[3] = UPTR(names[i]);
        CHECK(context_init(&file2, 4, p, v) == 0, "CONTEXT_INIT takes each offered capset");
        ctx = file2.driver_priv;
        CHECK(ctx && ctx->created && context_init_of(ctx->id) == ids[i] &&
                  !strcmp(three.name, names[i]) && ctx->num_rings == 2 && ctx->ring_mask == 2 &&
                  ctx->base_fence_ctx && ctx->base_fence_ctx != vg->fence_context,
              "CTX_CREATE carries the capset and the name, and the rings have their contexts");
        CHECK(context_init(&file2, 1, p, v) == -EEXIST, "a second CONTEXT_INIT is refused");
        close_file(&file2);
    }

    open_file(&file2);
    ctx = file2.driver_priv;
    creates = three.creates;
    p[0] = VIRTGPU_CONTEXT_PARAM_CAPSET_ID;
    v[0] = 3;
    CHECK(context_init(&file2, 1, p, v) == -EINVAL, "a capset the device lacks is refused");
    v[0] = 64;
    CHECK(context_init(&file2, 1, p, v) == -EINVAL, "a capset past 63 is refused");
    v[0] = 4;
    p[1] = VIRTGPU_CONTEXT_PARAM_CAPSET_ID;
    v[1] = 4;
    CHECK(context_init(&file2, 2, p, v) == -EINVAL, "a parameter given twice is refused");
    p[1] = VIRTGPU_CONTEXT_PARAM_NUM_RINGS;
    v[1] = 65;
    CHECK(context_init(&file2, 2, p, v) == -EINVAL, "more than 64 rings are refused");
    v[1] = 2;
    p[2] = VIRTGPU_CONTEXT_PARAM_POLL_RINGS_MASK;
    v[2] = 4;
    CHECK(context_init(&file2, 3, p, v) == -EINVAL, "a poll mask past the rings is refused");
    CHECK(context_init(&file2, 1, p + 2, v + 2) == -EINVAL, "a poll mask without rings is refused");
    p[2] = 9;
    CHECK(context_init(&file2, 3, p, v) == -EINVAL, "an unknown parameter is refused");
    p[2] = VIRTGPU_CONTEXT_PARAM_DEBUG_NAME;
    v[2] = 1;
    CHECK(context_init(&file2, 3, p, v) == -EFAULT, "a name that cannot be read is refused");
    p[2] = VIRTGPU_CONTEXT_PARAM_POLL_RINGS_MASK;
    v[2] = 1;
    p[3] = VIRTGPU_CONTEXT_PARAM_DEBUG_NAME;
    v[3] = UPTR("x");
    p[4] = VIRTGPU_CONTEXT_PARAM_DEBUG_NAME;
    v[4] = UPTR("y");
    CHECK(context_init(&file2, 5, p, v) == -EINVAL, "more than four parameters are refused");
    CHECK(three.creates == creates && !ctx->created && !ctx->context_init && !ctx->num_rings &&
              !ctx->ring_mask && !ctx->base_fence_ctx && !ctx->debug_name[0],
          "a refused CONTEXT_INIT creates nothing and changes nothing");
    k_memset(long_name, 'n', sizeof(long_name) - 1);
    v[0] = UPTR(long_name);
    p[0] = VIRTGPU_CONTEXT_PARAM_DEBUG_NAME;
    CHECK(context_init(&file2, 1, p, v) == 0 && k_strlen(three.name) == VGPU_DEBUG_NAME_MAX - 1 &&
              context_init_of(ctx->id) == 0,
          "a long name is cut to 63 bytes, and the default capset stays");
    close_file(&file2);

    open_file(&file2);
    CHECK(context_init(&file2, 0, p, v) == 0 && !strcmp(three.name, "rach2"),
          "CONTEXT_INIT with no parameters makes the default context");
    close_file(&file2);
    blob_unbind("the context row leaves nothing");
}

/** Submit one word on ring `ring` of file2, with an out fence: the fence. */
static struct dma_fence *ring_submit(uint32_t ring, uint32_t flags, int32_t fence_in)
{
    static uint32_t word;
    struct drm_virtgpu_execbuffer a;

    k_memset(&a, 0, sizeof(a));
    a.command = UPTR(&word);
    a.size = 4;
    a.flags = VIRTGPU_EXECBUF_RING_IDX | VIRTGPU_EXECBUF_FENCE_FD_OUT | flags;
    a.ring_idx = ring;
    a.fence_fd = fence_in;
    if (vgpu_execbuffer_ioctl(dev, &a, &file2) != 0) {
        return NULL;
    }
    return sync_file_get_fence(a.fence_fd);
}

/** The sync-file descriptor that holds `f`, or -1. */
static int32_t fd_of(struct dma_fence *f)
{
    int32_t i;

    for (i = 3; i < 32; i++) {
        if (f && sync_fds[i] == f) {
            return i;
        }
    }
    return -1;
}

/** Refused rings and flags, and dependencies on the same ring and on
 * another, on file2's two rings; `sent` is the events sent before the row. */
static void ring_waits(uint32_t sent)
{
    struct dma_fence *fa, *fb, *fc, *fd;
    uint32_t w = waits;

    CHECK(!ring_submit(2, 0, 0), "a ring past the context's is refused");
    CHECK(!ring_submit(0, 8, 0), "an unknown flag is refused");
    gpu.answer = ANSWER_NEVER;
    fa = ring_submit(0, 0, 0);
    fb = ring_submit(0, VIRTGPU_EXECBUF_FENCE_FD_IN, fd_of(fa));
    CHECK(fa && fb && waits == w, "a dependency on the same ring is not waited for");
    fc = ring_submit(1, 0, 0);
    step_fence = fc;
    fd = ring_submit(0, VIRTGPU_EXECBUF_FENCE_FD_IN, fd_of(fc));
    CHECK(fc && fd && waits == w + 1 && !step_fence, "a dependency on another ring is waited for");
    answer_all();
    CHECK(fa && fb && fd && dma_fence_is_signaled(fa) && dma_fence_is_signaled(fb) &&
              dma_fence_is_signaled(fd) && events_sent == sent + 2 && file2.event_space == 4096,
          "every answer is in, and ring 1 sent its second event");
    dma_fence_put(fa);
    dma_fence_put(fb);
    dma_fence_put(fc);
    dma_fence_put(fd);
}

/** Rings: their fence contexts, answers, events and dependencies. */
static void row_rings(void)
{
    uint64_t p[3] = {VIRTGPU_CONTEXT_PARAM_CAPSET_ID, VIRTGPU_CONTEXT_PARAM_NUM_RINGS,
                     VIRTGPU_CONTEXT_PARAM_POLL_RINGS_MASK};
    uint64_t v[3] = {4, 2, 2};
    struct dma_fence *f0, *f1, *f2;
    struct vgpu_context *ctx;
    uint64_t base;
    uint32_t sent;

    blob_bind(VIRGL | ALL_FEATURES, HOSTMEM_LEN);
    open_file(&file2);
    CHECK(context_init(&file2, 3, p, v) == 0, "a Venus context with two rings");
    ctx = file2.driver_priv;
    base = ctx->base_fence_ctx;
    sent = events_sent;
    gpu.answer = ANSWER_NEVER;
    f0 = ring_submit(0, 0, 0);
    CHECK(f0 && (three.submit_flags & VIRTIO_GPU_FLAG_INFO_RING_IDX) && three.submit_ring == 0,
          "a submission on ring 0 names its ring");
    f1 = ring_submit(1, 0, 0);
    CHECK(f1 && (three.submit_flags & VIRTIO_GPU_FLAG_INFO_RING_IDX) && three.submit_ring == 1 &&
              file2.event_space == 4096 - 8,
          "a submission on polled ring 1 names its ring and reserves its event");
    f2 = ring_submit(0, 0, 0);
    CHECK(f0 && f1 && f2 && f0->context == base && f1->context == base + 1 && f2->context == base,
          "each ring is its own fence context");
    if (!f0 || !f1 || !f2) {
        close_file(&file2);
        blob_unbind("the ring row leaves nothing");
        return;
    }
    answer_at(1);
    vgpu_reap(vg);
    CHECK(dma_fence_is_signaled(f1) && !dma_fence_is_signaled(f0) && events_sent == sent + 1 &&
              last_event_type == VIRTGPU_EVENT_FENCE_SIGNALED && file2.event_space == 4096,
          "ring 1's answer retires its fence alone, and the reaper sends its event");
    answer_at(1);
    vgpu_reap(vg);
    CHECK(dma_fence_is_signaled(f2) && dma_fence_is_signaled(f0) && events_sent == sent + 1,
          "a later answer on ring 0 retires the earlier fence of ring 0, with no event");
    answer_at(0);
    vgpu_reap(vg);
    CHECK(gpu.q[0].nflight == 0 && events_sent == sent + 1, "the first answer comes last");
    dma_fence_put(f0);
    dma_fence_put(f1);
    dma_fence_put(f2);
    ring_waits(sent);
    close_file(&file2);
    blob_unbind("the ring row leaves nothing");
}

/** PRIME export of a blob of host memory and its import back. */
static void row_prime(void)
{
    struct drm_prime_handle e, in;
    struct drm_gem_object *obj, *other;
    struct dma_buf *buf;
    uint32_t h;
    vma_t vma;

    blob_bind(VIRGL | ALL_FEATURES, HOSTMEM_LEN);
    prime_drop = dma_buf_put;
    CHECK(blob(VIRTGPU_BLOB_MEM_HOST3D,
               VIRTGPU_BLOB_FLAG_USE_MAPPABLE | VIRTGPU_BLOB_FLAG_USE_SHAREABLE, 2 * PAGE_SIZE, 5,
               &h) == 0,
          "a shareable mappable blob is made");
    vgpu_reap(vg);
    obj = obj_of(h);
    CHECK(obj->refs == 1 && vgpu_bo(obj)->uuid_ok, "ASSIGN_UUID's hold goes with its answer");
    k_memset(&e, 0, sizeof(e));
    e.handle = h;
    e.flags = DRM_CLOEXEC | DRM_RDWR;
    CHECK(drm_prime_handle_to_fd_ioctl(dev, &e, &file) == 0 && e.fd >= 3, "the blob exports");
    buf = buf_fds[e.fd];
    CHECK(buf && buf->size == 2 * PAGE_SIZE && buf->map_flags == VM_PROT_WC && !buf->read_only,
          "the dma-buf has the blob's size and write-combining");
    CHECK(buf && buf->ops->page(buf, 1) == (uintptr_t)(HOSTMEM_ADDR + PAGE_SIZE),
          "an importer's page is the blob's piece");
    k_memset(&vma, 0, sizeof(vma));
    vma.start = 0x40000000u;
    vma.end = vma.start + 2 * PAGE_SIZE;
    maps_cache = 0;
    CHECK(buf && buf->ops->mmap(buf, &vma) == 0 && maps_cache == VM_PROT_WC && obj->refs == 2,
          "the dma-buf maps the blob write-combining");
    k_memset(&in, 0, sizeof(in));
    in.fd = e.fd;
    CHECK(drm_prime_fd_to_handle_ioctl(dev, &in, &file) == 0 && in.handle == h,
          "the import into the same file is the same handle");
    open_file(&file2);
    in.handle = 0;
    CHECK(drm_prime_fd_to_handle_ioctl(dev, &in, &file2) == 0 && in.handle, "another file imports");
    other = drm_gem_object_lookup(&file2, in.handle);
    CHECK(other == obj, "a self-import is the same object");
    drm_gem_object_put(other);
    close_file(&file2);
    blob_unbind("the PRIME row leaves nothing");
    prime_drop = NULL;
}

/** A blob of guest memory as the scanout, with SET_SCANOUT_BLOB. */
static void row_scanout_blob(void)
{
    struct drm_mode_modeinfo modes[64];
    struct drm_mode_crtc set;
    uint32_t h, fb, res;

    blob_bind(VIRGL | ALL_FEATURES, HOSTMEM_LEN);
    k_memset(&shown_blob, 0, sizeof(shown_blob));
    (void)get_modes(modes, 64, NULL);
    CHECK(blob(VIRTGPU_BLOB_MEM_GUEST, 0, 1280u * 800 * 4, 0, &h) == 0,
          "a guest blob of 1280x800 pixels is made");
    res = vgpu_bo(obj_of(h))->res_id;
    fb = addfb(h, 1280, 800);
    k_memset(&set, 0, sizeof(set));
    set.crtc_id = vg->outputs[0].crtc.base.id;
    set.fb_id = fb;
    set.set_connectors_ptr = UPTR(&vg->outputs[0].connector.base.id);
    set.count_connectors = 1;
    set.mode = modes[0];
    set.mode_valid = 1;
    CHECK(drm_mode_setcrtc(dev, &set, &file) == 0, "SETCRTC of the blob");
    CHECK(shown_blob.resource_id == res && shown_blob.scanout_id == 0 && shown_blob.width == 1280 &&
              shown_blob.height == 800 && shown_blob.format == VIRTIO_GPU_FORMAT_B8G8R8X8_UNORM &&
              shown_blob.strides[0] == 1280 * 4 && shown_blob.offsets[0] == 0 &&
              rect_is(&shown_blob.r, 0, 0, 1280, 800) && gpu.shown[0].res == res,
          "SET_SCANOUT_BLOB shows the blob with its format, size and pitch");
    CHECK(log_find(VIRTIO_GPU_CMD_SET_SCANOUT, 0) == MAX_LOG && !gpu.transfers &&
              rect_is(&gpu.last_flush, 0, 0, 1280, 800),
          "a blob takes no SET_SCANOUT or transfer, only a flush");
    blob_unbind("the scanout row leaves nothing");
}

/** Run the blob rows against the real driver, fence and PRIME code. */
int main(void)
{
    current_proc = &self;
    row_params();
    row_host();
    row_guest();
    row_features();
    row_context();
    row_rings();
    row_prime();
    row_scanout_blob();
    printf("vgpublob-test: %s\n", failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
