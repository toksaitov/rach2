/*
 * vgpupayload_hosttest.c: virtio-gpu's command payloads past one chain's
 * descriptors, rach2/sys/drm/virtio_gpu_3d.c's vgpu_cmd_payload, through
 * EXECBUFFER and CREATE_BLOB's commands, over the blob test's model;
 * vgpupayload64_hosttest.c runs the same rows with x86-64's structures.
 *
 * host-deps: rach2/sys/tick.c
 * host-deps: rach2/sys/lib/kstring.c rach2/sys/lib/kctypes.c
 * host-deps: rach2/sys/cmdline.c rach2/sys/lib/kstd.c
 * host-deps: tests/host/compat/atomic_host.c tests/host/compat/resv_host.c
 * host-deps: tests/host/compat/mutex_none.c
 *
 * The rows: payloads of 1 MiB less SUBMIT_3D's header, 1 MiB and 4 MiB,
 * each through both ioctls, on a control queue with virtio-pci's bounds,
 * 1024 descriptors a chain and runs of any length, and with the shared
 * region's, 16 descriptors, runs of up to 64 MiB and a chain of up to the
 * 32 MiB region: each reaches the device whole, its bytes the caller's, in
 * one SUBMIT_3D whose chain fits the queue. Past the old bounds, 255 MiB
 * reaches the virtio-pci device whole in runs of more than a megabyte, and
 * 65 MiB the shared region's device that joins parts (VGS_TF_PARTS), as
 * does 4 MiB one of 1 MiB chains: the parts come in order, unfenced, and
 * the SUBMIT_3D after them takes their stream. A device that takes 1 MiB a
 * chain and joins no parts, and a transport that gives no runs, refuse a
 * longer payload with ENOMEM and send nothing; a bad pointer is EFAULT.
 * Where the transport refuses runs of more than a page at first, a payload
 * takes shorter runs; with none, 300 and 1022 pages go a page each and
 * 1023 are ENOMEM. A stream in parts with a buffer listed has its copies
 * counted from the caller's memory, and no part goes while its in-fence
 * is waited for. On a transport that gives frames the kernel does not map,
 * as virtio-pci of i686, the payloads, 255 MiB among them, take frames
 * filled through `kmap_cpu`, and a stream's copies are counted from them.
 * Each row ends with nothing allocated and the join lock free.
 *
 * Returns 0 when every row passes, 1 otherwise.
 */
#define VGPU_PAYLOAD_TEST 1
#include "vgpublob_hosttest.c"

#define MIB         (1024u * 1024u)
#define SUBMIT_HDR  ((uint32_t)sizeof(struct virtio_gpu_submit_3d))
#define PAYLOAD_MAX (4u * MIB)
/* Past 254 runs of a megabyte, virtio-pci's old bound, and past 64 MiB,
 * the largest shared region's. */
#define PCI_BIG     (255u * MIB)
#define SHARED_BIG  (65u * MIB)

static uint32_t *words; /* PCI_BIG bytes of a pattern */

/** Bind the blob test's device with the control queue's bounds on a chain,
 * `max_chain` descriptors and `max_bytes` each way, runs of at most
 * `dma_max` bytes from the DMA pool and, with `parts`, a device that joins
 * a stream sent in parts, and give the first file a virgl context. */
static void payload_bind(uint16_t max_chain, uint32_t max_bytes, uint32_t dma_max, int parts)
{
    static const uint32_t ids[4] = {1, 2, 4, 6};
    uint64_t p = VIRTGPU_CONTEXT_PARAM_CAPSET_ID, v = 1;

    model_reset();
    k_memset(&three, 0, sizeof(three));
    k_memset(blobs, 0, sizeof(blobs));
    k_memset(hostmem, 0, sizeof(hostmem));
    map_cache = VIRTIO_GPU_MAP_CACHE_WC;
    uuids = 0;
    gpu.offered |= VIRGL | ALL_FEATURES;
    gpu.hostmem_len = HOSTMEM_LEN;
    k_memcpy(three.capsets, ids, sizeof(ids));
    three.ncapsets = 4;
    gpu.config.num_capsets = 4;
    gpu.max_chain = max_chain;
    gpu.max_bytes = max_bytes;
    gpu.dev.dma_max = dma_max;
    gpu.dev.parts = parts;
    CHECK(bind() == 1 && vgpu_open(dev, &file) == 0, "a device with the bounds binds");
    CHECK(vg->ctrlq->max_chain == max_chain && vg->ctrlq->max_bytes == max_bytes,
          "the control queue has the transport's bounds");
    CHECK(context_init(&file, 1, &p, &v) == 0, "a virgl context for the payloads");
}

/** EXECBUFFER of `bytes` of the pattern from `user`: the ioctl's result. */
static int32_t exec_payload(uint64_t user, uint32_t bytes)
{
    struct drm_virtgpu_execbuffer a;
    int32_t rc;

    k_memset(&a, 0, sizeof(a));
    a.command = user;
    a.size = bytes;
    rc = vgpu_execbuffer_ioctl(dev, &a, &file);
    answer_all();
    return rc;
}

/** CREATE_BLOB of a page of host memory with `bytes` of commands from
 * `user`: the ioctl's result. The blob's handle is let go. */
static int32_t blob_payload(uint64_t user, uint32_t bytes)
{
    static uint64_t id = 100;
    struct drm_virtgpu_resource_create_blob a;
    int32_t rc;

    k_memset(&a, 0, sizeof(a));
    a.blob_mem = VIRTGPU_BLOB_MEM_HOST3D;
    a.size = PAGE_SIZE;
    a.blob_id = id++;
    a.cmd = user;
    a.cmd_size = bytes;
    rc = vgpu_blob_ioctl(dev, &a, &file);
    answer_all();
    if (rc == 0) {
        (void)drm_gem_handle_delete(&file, a.bo_handle);
        answer_all();
    }
    return rc;
}

/** Send `bytes` of the pattern through EXECBUFFER (`blob` 0) or CREATE_BLOB
 * and check that one SUBMIT_3D carried every byte. */
static void payload_whole(int blob, uint32_t bytes, const char *row)
{
    uint32_t submits = three.submits;
    int32_t rc;

    three.submitted_size = three.submitted_sum = 0;
    rc = blob ? blob_payload(UPTR(words), bytes) : exec_payload(UPTR(words), bytes);
    CHECK(rc == 0, row);
    CHECK(three.submits == submits + 1 && three.submitted_size == bytes &&
              three.submitted_sum == fnv1a((const uint8_t *)words, bytes),
          row);
    if (rc != 0 || three.submitted_size != bytes) {
        printf("vgpupayload-test: %s: %s of %u bytes: rc %d, %u bytes came\n", row,
               blob ? "CREATE_BLOB" : "EXECBUFFER", (unsigned)bytes, (int)rc,
               (unsigned)three.submitted_size);
    }
}

/** Check that the stream of the last SUBMIT_3D came in `more` parts than
 * `parts`, the count before it, and the SUBMIT_3D took it joined, or with
 * `more` 0 that it came whole with no part. */
static void payload_parts(uint32_t parts, int more, const char *row)
{
    if (more) {
        CHECK(three.parts > parts && (three.submit_flags & VGS_FLAG_JOINED), row);
    } else {
        CHECK(three.parts == parts && !(three.submit_flags & VGS_FLAG_JOINED), row);
    }
    CHECK(!mutex_held(&vg->join_lock), "the join lock is free after the submission");
}

/** Send `bytes` through both ioctls and check each refused with `err` and
 * nothing sent. */
static void payload_refused(uint64_t user, uint32_t bytes, int32_t err, const char *row)
{
    uint32_t sent = gpu.nlog;

    CHECK(exec_payload(user, bytes) == err, row);
    CHECK(blob_payload(user, bytes) == err, row);
    CHECK(log_find(VIRTIO_GPU_CMD_SUBMIT_3D, sent) == MAX_LOG, row);
}

/** The three sizes through both ioctls on the bounds bound now. */
static void payload_sizes(const char *row)
{
    static const uint32_t sizes[3] = {MIB - SUBMIT_HDR, MIB, PAYLOAD_MAX};
    int i, blob;

    for (i = 0; i < 3; i++) {
        for (blob = 0; blob < 2; blob++) {
            payload_whole(blob, sizes[i], row);
        }
    }
}

/** virtio-pci: 1024 descriptors a chain, QEMU's most, as the driver's
 * indirect tables give the control queue, and runs of any length; then a
 * transport that gives no runs. */
static void row_pci(void)
{
    payload_bind(VIRTQ_INDIRECT_MAX, 0, VIRTIO_DMA_ANY, 0);
    payload_sizes("a payload reaches a virtio-pci device whole in one command");
    /* The runs the payload asks for; virtio_pci_shm_hosttest.c shows the
     * transport giving such a run. */
    gpu.dma_most = 0;
    payload_whole(0, PCI_BIG, "255 MiB reaches a virtio-pci device whole in one command");
    CHECK(gpu.dma_most == 64u * PAGE_SIZE, "255 MiB goes in runs of 64 pages on 1022 descriptors");
    payload_refused(16, PAYLOAD_MAX, -EFAULT, "a bad pointer is EFAULT and sends nothing");
    blob_unbind("the virtio-pci row leaves nothing");

    payload_bind(VIRTQ_INDIRECT_MAX, 0, 0, 0);
    payload_whole(0, 1022u * PAGE_SIZE, "a page each fills the chain without runs");
    payload_refused(UPTR(words), PAYLOAD_MAX, -ENOMEM,
                    "without runs a longer payload is ENOMEM and sends nothing");
    blob_unbind("the row without runs leaves nothing");
}

/** virtio-pci whose free frames are scattered: where the transport has no
 * run as long as the chain's descriptors need, the payload takes shorter
 * ones and sizes the rest on the descriptors left; a page each fills the
 * chain's 1022, 300 pages among them, as Linux gives each page of a
 * submission a descriptor, and past them, with no run of two pages free,
 * the payload is ENOMEM and sends nothing. */
static void row_scattered(void)
{
    int blob;

    payload_bind(VIRTQ_INDIRECT_MAX, 0, VIRTIO_DMA_ANY, 0);
    for (blob = 0; blob < 2; blob++) {
        gpu.dma_short = 10;
        gpu.dma_runs = 0;
        payload_whole(blob, 1500u * PAGE_SIZE, "a payload takes shorter runs where long ones fail");
        CHECK(gpu.dma_short == 0 && gpu.dma_runs > 0,
              "the runs refused are passed over and later ones taken");
    }
    gpu.dma_short = 0xffffffffu;
    for (blob = 0; blob < 2; blob++) {
        payload_whole(blob, 300u * PAGE_SIZE, "300 scattered pages go a page each");
        payload_whole(blob, 1022u * PAGE_SIZE, "scattered pages fill the chain a page each");
    }
    payload_refused(UPTR(words), 1023u * PAGE_SIZE, -ENOMEM,
                    "past the chain's pages with no run free a payload is ENOMEM, nothing sent");
    gpu.dma_short = 0;
    blob_unbind("the scattered row leaves nothing");
}

/** The shared region: 16 descriptors a chain, runs of up to 64 MiB and a
 * chain of up to the bed's 32 MiB region; then the backend's old 1 MiB. */
static void row_shared(void)
{
    payload_bind(16, 32u * MIB, 64u * MIB, 0);
    payload_sizes("a payload reaches the shared region's device whole in one command");
    blob_unbind("the shared region's row leaves nothing");

    payload_bind(16, MIB, 64u * MIB, 0);
    payload_whole(0, MIB - SUBMIT_HDR, "a 1 MiB chain carries 1 MiB less the header");
    payload_whole(1, MIB - SUBMIT_HDR, "a 1 MiB chain carries 1 MiB less the header");
    payload_refused(UPTR(words), MIB, -ENOMEM,
                    "past a 1 MiB chain a payload is ENOMEM and sends nothing");
    payload_refused(UPTR(words), PAYLOAD_MAX, -ENOMEM,
                    "past a 1 MiB chain a payload is ENOMEM and sends nothing");
    blob_unbind("the 1 MiB row leaves nothing");
}

/** The shared region's device that joins parts: a stream past one part,
 * 896 KiB on 16 descriptors, goes ahead of its SUBMIT_3D in parts, up to
 * 65 MiB, past the largest region, and on chains of 1 MiB; a shorter one
 * goes whole; a bad pointer sends nothing and frees the join lock. */
static void row_parts(void)
{
    static const uint32_t sizes[4] = {64u * 1024u, MIB - SUBMIT_HDR, PAYLOAD_MAX, SHARED_BIG};
    uint32_t parts, sent;
    int i, blob;

    payload_bind(16, 32u * MIB, 64u * MIB, 1);
    sent = gpu.nlog;
    payload_refused(16, PAYLOAD_MAX, -EFAULT, "a bad pointer in parts is EFAULT, nothing sent");
    CHECK(log_find(VGS_CMD_PART, sent) == MAX_LOG && !mutex_held(&vg->join_lock),
          "a refused stream sends no part and frees the join lock");
    for (i = 0; i < 4; i++) {
        for (blob = 0; blob < 2; blob++) {
            parts = three.parts;
            payload_whole(blob, sizes[i], "a stream reaches the device that joins parts whole");
            payload_parts(parts, i != 0, "a stream past one part goes in parts, joined");
        }
    }
    blob_unbind("the row of parts leaves nothing");

    payload_bind(16, MIB, 64u * MIB, 1);
    parts = three.parts;
    payload_whole(0, PAYLOAD_MAX, "4 MiB reaches a device of 1 MiB chains in parts");
    payload_parts(parts, 1, "4 MiB on 1 MiB chains goes in parts, joined");
    blob_unbind("the row of parts on 1 MiB chains leaves nothing");
}

/* A stream of JOINED_DWORDS dwords, 2 MiB, which goes in parts. */
#define JOINED_DWORDS (512u * 1024u)
static uint32_t *joined;

/** Fill the stream with one-dword commands of nothing and the copies of
 * the resource `res`, a 7 by 3 texel box at 4 bytes a texel each: at the
 * start, across a page, across the first part's end, 896 KiB, and at the
 * end. */
static void joined_fill(uint32_t res)
{
    static const uint32_t box[3] = {7, 3, 1};
    static const uint32_t at[4] = {0, 1020, 229370, JOINED_DWORDS - 14};
    int i;

    k_memset(joined, 0, JOINED_DWORDS * 4u);
    for (i = 0; i < 4; i++) {
        (void)copy_cmd(joined, at[i], VIRGL_CCMD_TRANSFER3D, 13, res, box);
    }
}

/* What the foreign fence's wait saw: VGS_CMD_PART logged since `joined_sent`
 * and the join lock held. */
static uint32_t joined_sent;
static int joined_parts_at_wait, joined_locked_at_wait;

/** The submission waits for its foreign fence: note what has gone. */
static void joined_wait(void)
{
    joined_parts_at_wait = log_find(VGS_CMD_PART, joined_sent) != MAX_LOG;
    joined_locked_at_wait = mutex_held(&vg->join_lock);
    wait_step = NULL;
}

/** A stream in parts with buffers listed: its copies are counted from the
 * caller's memory, and with a foreign in-fence no part goes and the join
 * lock is free until the fence signals, so that a submission the fence's
 * producer waits behind may send its own parts. */
static void row_joined(void)
{
    struct drm_virtgpu_execbuffer a;
    struct dma_fence *foreign;
    uint32_t h, parts;
    uint64_t before;
    int32_t rc;

    payload_bind(16, 32u * MIB, 64u * MIB, 1);
    h = copy_res(&file, 1);
    joined_fill(res_of(&file, h));
    k_memset(&a, 0, sizeof(a));
    a.command = UPTR(joined);
    a.size = JOINED_DWORDS * 4u;
    a.bo_handles = UPTR(&h);
    a.num_bo_handles = 1;
    answer_all();
    before = virtio_gpu_copied_bytes();
    parts = three.parts;
    three.submitted_size = three.submitted_sum = 0;
    rc = vgpu_execbuffer_ioctl(dev, &a, &file);
    answer_all();
    CHECK(rc == 0 && three.submitted_size == a.size &&
              three.submitted_sum == fnv1a((const uint8_t *)joined, a.size),
          "a stream in parts with a buffer listed reaches the device whole");
    payload_parts(parts, 1, "a stream with a buffer listed goes in parts, joined");
    CHECK(virtio_gpu_copied_bytes() - before == 4u * 84u,
          "its copies are counted, across a page and across a part");

    foreign = kzalloc(sizeof(*foreign));
    CHECK(foreign != NULL, "a foreign fence");
    if (!foreign) {
        return;
    }
    dma_fence_init(foreign, &model_fence_ops, dma_fence_context_alloc(1), 1);
    a.flags = VIRTGPU_EXECBUF_FENCE_FD_IN;
    a.fence_fd = sync_file_create(foreign, "foreign");
    step_fence = foreign;
    joined_sent = gpu.nlog;
    joined_parts_at_wait = joined_locked_at_wait = -1;
    wait_step = joined_wait;
    parts = three.parts;
    rc = vgpu_execbuffer_ioctl(dev, &a, &file);
    answer_all();
    CHECK(rc == 0 && !step_fence && wait_step == NULL, "a stream in parts waits for its in-fence");
    CHECK(joined_parts_at_wait == 0 && joined_locked_at_wait == 0,
          "no part goes and the join lock is free while the in-fence is waited for");
    payload_parts(parts, 1, "after the wait the stream goes in parts, joined");
    wait_step = NULL;
    dma_fence_put(foreign);
    blob_unbind("the joined row leaves nothing");
}

/** virtio-pci of i686, whose payloads take frames of any memory, which the
 * kernel fills through `kmap_cpu`, past the kernel heap: the sizes and 255
 * MiB reach the device whole, scattered frames take shorter runs, a
 * stream's copies are counted from its frames across a page, a bad
 * pointer sends nothing, and every frame is given back. */
static void row_frames(void)
{
    struct drm_virtgpu_execbuffer a;
    uint32_t h;
    uint64_t before;
    int32_t rc;

    payload_bind(VIRTQ_INDIRECT_MAX, 0, VIRTIO_DMA_ANY, 0);
    gpu.frames = 1;
    payload_sizes("a payload in frames reaches a virtio-pci device whole");
    CHECK(gpu.kmaps > 0 && gpu.frames_live == 0 && !gpu.kmapped,
          "the frames are filled through kmap_cpu and given back");
    gpu.dma_most = 0;
    payload_whole(0, PCI_BIG, "255 MiB in frames reaches a virtio-pci device whole");
    CHECK(gpu.dma_most == 64u * PAGE_SIZE, "255 MiB of frames goes in runs of 64 pages");
    gpu.dma_short = 10;
    gpu.dma_runs = 0;
    payload_whole(1, 1500u * PAGE_SIZE,
                  "a payload in frames takes shorter runs where long ones fail");
    CHECK(gpu.dma_short == 0 && gpu.dma_runs > 0, "the runs of frames refused are passed over");
    gpu.dma_short = 0;

    h = copy_res(&file, 1);
    joined_fill(res_of(&file, h));
    k_memset(&a, 0, sizeof(a));
    a.command = UPTR(joined);
    a.size = JOINED_DWORDS * 4u;
    a.bo_handles = UPTR(&h);
    a.num_bo_handles = 1;
    answer_all();
    before = virtio_gpu_copied_bytes();
    three.submitted_size = three.submitted_sum = 0;
    gpu.kmaps = 0;
    rc = vgpu_execbuffer_ioctl(dev, &a, &file);
    answer_all();
    CHECK(rc == 0 && three.submitted_size == a.size &&
              three.submitted_sum == fnv1a((const uint8_t *)joined, a.size),
          "a stream in frames with a buffer listed reaches the device whole");
    CHECK(virtio_gpu_copied_bytes() - before == 4u * 84u,
          "its copies are counted from its frames, across a page");
    CHECK(gpu.kmaps > JOINED_DWORDS * 4u / PAGE_SIZE,
          "the count reads the frames through kmap_cpu");
    payload_refused(16, PAYLOAD_MAX, -EFAULT, "a bad pointer in frames is EFAULT, nothing sent");
    CHECK(gpu.frames_live == 0 && !gpu.kmapped, "every frame is given back");
    blob_unbind("the row of frames leaves nothing");
}

/** Run the payload rows against the real driver. */
int main(void)
{
    uint32_t i;

    current_proc = &self;
    self.tg = &self_tg;
    k_memcpy(self.comm, "vkcube", sizeof("vkcube"));
    words = (uint32_t *)calloc(1, PCI_BIG);
    if (!words) {
        printf("vgpupayload-test: FAIL: no memory for the pattern\n");
        return 1;
    }
    for (i = 0; i < PCI_BIG / 4u; i++) {
        words[i] = i * 2654435761u;
    }
    joined = (uint32_t *)calloc(JOINED_DWORDS, 4u);
    if (!joined) {
        printf("vgpupayload-test: FAIL: no memory for the stream\n");
        free(words);
        return 1;
    }
    row_pci();
    row_scattered();
    row_shared();
    row_parts();
    row_joined();
    row_frames();
    free(joined);
    free(words);
    printf("vgpupayload-test: %s\n", failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
