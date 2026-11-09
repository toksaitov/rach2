/* vgpu3d_hosttest.c: virgl commands, pipelining and reset over the 2D model.
 * host-deps: rach2/sys/lib/kstring.c rach2/sys/lib/kctypes.c
 * Returns 0 when the new 3D rows pass. The included 2D rows do not run. */
#define VGPU_3D_TEST 1
#define main         vgpu_2d_rows_main
#include "virtio_gpu_hosttest.c"
#undef main

extern int memcmp(const void *, const void *, size_t);

static struct dma_fence *sync_fds[32];
static struct dma_fence *step_fence;
static uint32_t waits;
static const struct file_ops *reset_file_ops;
static debugfs_open_fn reset_file_open;
static void *reset_file_data;
static char registered_path[64];
static struct drm_syncobj syncs[4];

/** A model sync-file descriptor holds a real fence. */
int32_t sync_file_create(struct dma_fence *f, const char *name)
{
    uint32_t i;
    (void)name;
    for (i = 3; i < 32; i++) {
        if (!sync_fds[i]) {
            sync_fds[i] = dma_fence_get(f);
            return (int32_t)i;
        }
    }
    return -EMFILE;
}

/** A referenced fence from the model descriptor table. */
struct dma_fence *sync_file_get_fence(int32_t fd)
{
    return fd >= 0 && fd < 32 ? dma_fence_get(sync_fds[fd]) : NULL;
}

/** A scheduler step completes a foreign producer, counting sleeps. */
static void model_wait_step(void)
{
    waits++;
    if (step_fence) {
        dma_fence_signal(step_fence);
        step_fence = NULL;
    }
}

/** Resolve the four modeled sync objects. */
struct drm_syncobj *drm_syncobj_find(struct drm_file *f, uint32_t handle)
{
    (void)f;
    if (handle < 1 || handle > 4) {
        return NULL;
    }
    syncs[handle - 1].refs++;
    return &syncs[handle - 1];
}

/** Release one model reference. */
void drm_syncobj_put(struct drm_syncobj *s)
{
    if (s) {
        s->refs--;
    }
}

/** Replace a model sync object's held fence. */
void drm_syncobj_replace_fence(struct drm_syncobj *s, struct dma_fence *f)
{
    dma_fence_put(s->fence);
    s->fence = dma_fence_get(f);
}

/** Publish a real chain to a model sync object. */
void drm_syncobj_add_point(struct drm_syncobj *s, struct dma_fence_chain *c, struct dma_fence *f,
                           uint64_t point)
{
    dma_fence_chain_init(c, dma_fence_get(s->fence), dma_fence_get(f), point);
    dma_fence_put(s->fence);
    s->fence = &c->base;
}

/** Resolve a model object's binary fence or timeline point. */
int32_t drm_syncobj_find_fence(struct drm_file *f, uint32_t h, uint64_t p, uint32_t flags,
                               struct dma_fence **result)
{
    struct drm_syncobj *s = drm_syncobj_find(f, h);
    (void)flags;
    if (!s) {
        return -ENOENT;
    }
    *result = dma_fence_get(s->fence);
    drm_syncobj_put(s);
    if (!*result) {
        return -EINVAL;
    }
    if (p) {
        return dma_fence_chain_find_seqno(result, p);
    }
    return 0;
}

/** Capture the driver's reset registration and its lifetime hooks. */
int32_t debugfs_create_file(const char *path, uint32_t mode, const struct file_ops *ops,
                            debugfs_open_fn open, void *data)
{
    CHECK(mode == 0200, "reset is writable by root alone");
    k_strncpy(registered_path, path, sizeof(registered_path));
    reset_file_ops = ops;
    reset_file_open = open;
    reset_file_data = data;
    return 0;
}

/** Remove the reset entry, preserving operations held by an open. */
void debugfs_remove(const char *path)
{
    CHECK(!strcmp(path, registered_path), "unplug removes its own reset entry");
    registered_path[0] = 0;
}

/**
 * The model core's unplug consumes its registration reference, as the core
 * does.
 */
void drm_dev_unplug(struct drm_device *d)
{
    if (!d->unplugged) {
        d->unplugged = 1;
        vgpu_unplug(d);
        drm_dev_put(d);
    }
}

/** Read each advertised capability set, including the one across two pages. */
static void capabilities(void)
{
    struct drm_virtgpu_getparam p;
    struct drm_virtgpu_get_caps a;
    static uint8_t bytes[5100];
    int32_t value = -1;
    uint32_t i;

    CHECK(vg->has_virgl && three.cap_queries == 2,
          "probe negotiates virgl and reads each capset info");
    p.param = VIRTGPU_PARAM_3D_FEATURES;
    p.value = UPTR(&value);
    CHECK(vgpu_getparam_ioctl(dev, &p, &file) == 0 && value == 1, "3D_FEATURES is one");
    p.param = VIRTGPU_PARAM_SUPPORTED_CAPSET_IDs;
    CHECK(vgpu_getparam_ioctl(dev, &p, &file) == 0 && value == 6, "capset bits are ids 1 and 2");
    k_memset(&a, 0, sizeof(a));
    a.cap_set_id = 2;
    a.cap_set_ver = 2;
    a.addr = UPTR(bytes);
    a.size = sizeof(bytes);
    k_memset(bytes, 0xa5, sizeof(bytes));
    CHECK(vgpu_get_caps_ioctl(dev, &a, &file) == 0, "GET_CAPS reads the multi-page set");
    for (i = 0; i < 5000; i++) {
        CHECK(bytes[i] == (uint8_t)(i * 7 + 2),
              "capset bytes equal the device's independent sequence");
    }
    CHECK(bytes[5000] == 0xa5, "GET_CAPS does not copy beyond the host's size");
    a.size = 17;
    a.cap_set_id = 1;
    CHECK(vgpu_get_caps_ioctl(dev, &a, &file) == 0 && bytes[16] == 113,
          "GET_CAPS permits short reads");
    a.size = 0;
    CHECK(vgpu_get_caps_ioctl(dev, &a, &file) == -EINVAL, "GET_CAPS refuses zero size");
    a.size = 1;
    a.cap_set_ver = 3;
    CHECK(vgpu_get_caps_ioctl(dev, &a, &file) == -EINVAL, "GET_CAPS refuses unsupported version");
    a.cap_set_ver = 0;
    a.addr = 1;
    CHECK(vgpu_get_caps_ioctl(dev, &a, &file) == -EFAULT, "GET_CAPS checks user copy");
}

/** Make a virgl buffer and verify every resource field at the device. */
static uint32_t resource(void)
{
    struct drm_virtgpu_resource_create a;

    k_memset(&a, 0, sizeof(a));
    a.target = 0;
    a.format = 64;
    a.bind = 8;
    a.width = 256;
    a.height = a.depth = a.array_size = 1;
    a.size = PAGE_SIZE;
    a.flags = 1;
    CHECK(vgpu_resource_create_ioctl(dev, &a, &file) == 0, "3D resource and backing created");
    CHECK(three.resource.target == 0 && three.resource.format == 64 && three.resource.bind == 8 &&
              three.resource.width == 256 && three.resource.height == 1 &&
              three.resource.depth == 1 && three.resource.array_size == 1 &&
              three.resource.flags == 1,
          "RESOURCE_CREATE_3D transmits the supplied description");
    CHECK(three.creates == 1 && three.attaches == 1 && a.res_handle,
          "a context is created lazily and the resource attached");
    return a.bo_handle;
}

/**
 * Transfer bytes in both directions, with the box and backing offset
 * preserved.
 */
static void transfers(uint32_t h)
{
    struct drm_virtgpu_3d_transfer a;
    struct drm_gem_object *obj = drm_gem_object_lookup(&file, h);
    struct virtio_gpu_object *bo = vgpu_bo(obj);
    uintptr_t page = drm_gem_shmem_page(&bo->shmem, 0);
    struct model_res *r = res_find(bo->res_id);
    uint32_t i, before = waits;

    k_memset(&a, 0, sizeof(a));
    a.bo_handle = h;
    a.box.x = 3;
    a.box.w = 32;
    a.box.h = a.box.d = 1;
    a.offset = 16;
    for (i = 0; i < 32; i++) {
        ((uint8_t *)page)[16 + i] = (uint8_t)(i ^ 0x9a);
    }
    CHECK(vgpu_transfer_3d_ioctl(dev, &a, &file, 0) == 0, "TRANSFER_TO_HOST queues");
    CHECK(three.transfer.x == 3 && three.transfer.offset == 16 && three.transfer.w == 32,
          "transfer box and offset reach the host");
    for (i = 0; i < 32; i++) {
        CHECK(r->pixels[3 + i] == (uint8_t)(i ^ 0x9a), "host bytes equal backing bytes");
        r->pixels[3 + i] = (uint8_t)(i ^ 0x65);
    }
    CHECK(vgpu_transfer_3d_ioctl(dev, &a, &file, 1) == 0, "TRANSFER_FROM_HOST queues");
    for (i = 0; i < 32; i++) {
        CHECK(((uint8_t *)page)[16 + i] == (uint8_t)(i ^ 0x65), "readback bytes equal host bytes");
    }
    CHECK(waits == before, "ordered transfers on this queue introduce no sleeps");
    a.stride = 16;
    CHECK(vgpu_transfer_3d_ioctl(dev, &a, &file, 0) == -EINVAL,
          "ordinary virgl resources refuse stride");
    a.stride = 0;
    a.layer_stride = 16;
    CHECK(vgpu_transfer_3d_ioctl(dev, &a, &file, 1) == -EINVAL,
          "ordinary virgl resources refuse layer stride");
    drm_gem_object_put(obj);
    vgpu_reap(vg);
}

/** Deliver a chosen response ahead of older responses, as an interrupt does. */
static void answer_at(uint32_t at)
{
    struct model_queue *q = &gpu.q[0];
    void *token = q->flight[at].token;
    uint32_t len = q->flight[at].len;

    CHECK(at < q->nflight, "the chosen response is outstanding");
    q->nflight--;
    k_memmove(&q->flight[at], &q->flight[at + 1], (q->nflight - at) * sizeof(q->flight[0]));
    q->vq.done(&q->vq, token, len);
}

/**
 * Submit without sleeping, with foreign and same-queue dependencies and
 * syncobjs.
 */
static void submissions(uint32_t h)
{
    struct drm_virtgpu_execbuffer a;
    struct drm_virtgpu_3d_wait w;
    struct drm_virtgpu_execbuffer_syncobj out;
    struct dma_fence *first, *second, *foreign;
    static uint32_t commands[1200];
    uint32_t i, before = waits;
    int fd1, fd2, foreign_fd;

    for (i = 0; i < 1200; i++) {
        commands[i] = i ^ 0x12345678u;
    }
    k_memset(&a, 0, sizeof(a));
    a.command = UPTR(commands);
    a.size = sizeof(commands);
    a.bo_handles = UPTR(&h);
    a.num_bo_handles = 1;
    a.flags = VIRTGPU_EXECBUF_FENCE_FD_OUT;
    gpu.answer = ANSWER_NEVER;
    CHECK(vgpu_execbuffer_ioctl(dev, &a, &file) == 0,
          "first submission returns with its fence pending");
    fd1 = a.fence_fd;
    first = sync_file_get_fence(fd1);
    CHECK(!dma_fence_is_signaled(first), "the returned out-fence remains pending");
    a.flags |= VIRTGPU_EXECBUF_FENCE_FD_IN;
    out.handle = 1;
    out.flags = 0;
    out.point = 7;
    a.out_syncobjs = UPTR(&out);
    a.num_out_syncobjs = 1;
    a.syncobj_stride = sizeof(out);
    CHECK(vgpu_execbuffer_ioctl(dev, &a, &file) == 0,
          "same-queue in-fence does not stall the next submission");
    fd2 = a.fence_fd;
    second = sync_file_get_fence(fd2);
    CHECK(!dma_fence_is_signaled(first) && !dma_fence_is_signaled(second) && waits == before,
          "two frames queue with zero sleeps and both fences pending");
    CHECK(three.submitted_size == sizeof(commands) &&
              !memcmp(three.submitted, commands, sizeof(commands)),
          "multi-page command bytes reach the host exactly");
    w.handle = h;
    w.flags = VIRTGPU_WAIT_NOWAIT;
    CHECK(vgpu_wait_ioctl(dev, &w, &file) == -EBUSY, "WAIT_NOWAIT reports outstanding work");
    answer_at(1);
    CHECK(dma_fence_is_signaled(first) && dma_fence_is_signaled(second),
          "a later fence response retires prior points on its ordered timeline");
    CHECK(dma_fence_is_signaled(syncs[0].fence), "the output syncobj timeline reaches its point");
    answer_at(0);
    gpu.answer = ANSWER_KICK;
    CHECK(vgpu_wait_ioctl(dev, &w, &file) == 0, "WAIT_NOWAIT sees the completed resource");
    w.flags = 0;
    CHECK(vgpu_wait_ioctl(dev, &w, &file) == 0, "blocking WAIT sees completion");
    foreign = kzalloc(sizeof(*foreign));
    dma_fence_init(foreign, &model_fence_ops, dma_fence_context_alloc(1), 1);
    foreign_fd = sync_file_create(foreign, "foreign");
    step_fence = foreign;
    a.flags = VIRTGPU_EXECBUF_FENCE_FD_IN;
    a.fence_fd = foreign_fd;
    a.num_out_syncobjs = 0;
    CHECK(vgpu_execbuffer_ioctl(dev, &a, &file) == 0 && waits == before + 1 && !step_fence,
          "a foreign producer is waited for exactly once");
    a.fence_fd = -1;
    CHECK(vgpu_execbuffer_ioctl(dev, &a, &file) == -EINVAL, "an invalid in-fence is refused");
    a.flags = 0;
    a.command = 1;
    CHECK(vgpu_execbuffer_ioctl(dev, &a, &file) == -EFAULT, "command-copy faults submit nothing");
    a.command = UPTR(commands);
    a.flags = VIRTGPU_EXECBUF_RING_IDX;
    CHECK(vgpu_execbuffer_ioctl(dev, &a, &file) == -EINVAL, "rings require context init");
    dma_fence_put(first);
    dma_fence_put(second);
    dma_fence_put(foreign);
    vgpu_reap(vg);
}

/** Reset while commands are outstanding, retaining old handles and mappings. */
static void reset_pending(uint32_t h)
{
    struct drm_virtgpu_execbuffer a;
    struct drm_virtgpu_3d_wait w;
    struct drm_gem_object *obj;
    struct dma_fence *pending, *completed;
    fd_entry_t e;
    uint32_t word = 0;
    uintptr_t phys;

    completed = sync_file_get_fence(3);
    CHECK(dma_fence_status(completed) == 1, "a previously completed fence before reset");
    obj = drm_gem_object_lookup(&file, h);
    k_memset(&a, 0, sizeof(a));
    a.command = UPTR(&word);
    a.size = 4;
    a.bo_handles = UPTR(&h);
    a.num_bo_handles = 1;
    a.flags = VIRTGPU_EXECBUF_FENCE_FD_OUT;
    gpu.answer = ANSWER_NEVER;
    CHECK(vgpu_execbuffer_ioctl(dev, &a, &file) == 0, "a submission pending at reset");
    pending = sync_file_get_fence(a.fence_fd);
    k_memset(&e, 0, sizeof(e));
    e.priv = reset_file_data;
    CHECK(!strcmp(registered_path, "dri/0/virtio_gpu_reset") && reset_file_open(&e, 1) == 0,
          "the root reset entry opens and holds the device");
    CHECK(reset_file_ops->write(&e, "x", 1) == -EINVAL && !vg->dead,
          "invalid reset text does nothing");
    drm_dev_get(dev);
    CHECK(reset_file_ops->write(&e, "1\n", 2) == 2, "the debugfs trigger resets the device");
    CHECK(gpu.torn_down && vg->dead && !registered_path[0],
          "reset stops DMA and removes its trigger");
    CHECK(dma_fence_status(completed) == 1, "reset preserves a completed fence's success");
    dma_fence_put(completed);
    CHECK(dma_fence_status(pending) == -ENODEV, "pending sync-file fence reports device loss");
    CHECK(vgpu_bo(obj)->res_id == 0 && pages_held == 0 && !vg->live,
          "reset frees every backing page and outstanding command");
    CHECK(!vg->contexts->id && !vg->contexts->created && !vg->contexts->attached,
          "reset destroys the context and its attachments");
    CHECK(obj_vm_page(obj, 0, &phys) == -1, "old mapping faults after reset");
    w.handle = h;
    w.flags = 0;
    CHECK(vgpu_wait_ioctl(dev, &w, &file) == -ENODEV &&
              vgpu_execbuffer_ioctl(dev, &a, &file) == -ENODEV,
          "old handles reject waits and submissions");
    reset_file_ops->close(&e);
    dma_fence_put(pending);
    drm_gem_object_put(obj);
}

/** Force loss while releasing backing, before a host could stop reading it. */
static void release_failure(int no_memory)
{
    uint32_t h;

    model_reset();
    k_memset(&three, 0, sizeof(three));
    gpu.offered |= VIRTIO_BIT(VIRTIO_GPU_F_VIRGL);
    gpu.config.num_capsets = 2;
    CHECK(bind() == 1 && vgpu_open(dev, &file) == 0, "bind for resource-release failure");
    h = resource();
    drm_dev_get(dev);
    if (no_memory) {
        fail_after = 0;
    } else {
        gpu.fail_type = VIRTIO_GPU_CMD_RESOURCE_UNREF;
    }
    CHECK(drm_gem_handle_delete(&file, h) == 0, "close the resource in the forced failure");
    fail_after = -1;
    vgpu_reap(vg);
    CHECK(vg->dead && gpu.torn_down && !pages_held && !vg->resources,
          "an unref allocation failure or error stops DMA before backing is freed");
    vgpu_context_close(dev, &file);
    unbind();
    model_reset();
    CHECK(!allocations && !irq_depth, "the forced release failure leaves no allocations or locks");
}

/** Run only the new 3D rows against the real driver and fence code. */
int main(void)
{
    uint32_t h, i;

    current_proc = &self;
    model_reset();
    gpu.offered |= VIRTIO_BIT(VIRTIO_GPU_F_VIRGL);
    gpu.config.num_capsets = 2;
    CHECK(bind() == 1, "virgl device binds");
    CHECK(vgpu_open(dev, &file) == 0 && !three.creates, "open allocates a lazy per-file context");
    capabilities();
    h = resource();
    transfers(h);
    submissions(h);
    reset_pending(h);
    vgpu_context_close(dev, &file);
    for (i = 0; i < 32; i++) {
        dma_fence_put(sync_fds[i]);
        sync_fds[i] = NULL;
    }
    for (i = 0; i < 4; i++) {
        dma_fence_put(syncs[i].fence);
        syncs[i].fence = NULL;
    }
    unbind();
    model_reset();
    CHECK(!allocations && !pages_held && !irq_depth,
          "all model allocations, pages and locks are released");
    release_failure(1);
    release_failure(0);
    printf("vgpu3d-test: %s\n", failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
