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
 * 256 descriptors a chain and runs of up to a megabyte, and with the
 * shared region's, 16 descriptors, runs of up to 64 MiB and a chain of up
 * to the 32 MiB region: each reaches the device whole, its bytes the
 * caller's, in one SUBMIT_3D whose chain fits the queue. A device that
 * takes 1 MiB a chain, the backend's bound before it took its region's,
 * and a transport that gives no runs, refuse a longer payload with ENOMEM
 * and send nothing; a bad pointer is EFAULT. Each row ends with nothing
 * allocated.
 *
 * Returns 0 when every row passes, 1 otherwise.
 */
#define VGPU_PAYLOAD_TEST 1
#include "vgpublob_hosttest.c"

#define MIB         (1024u * 1024u)
#define SUBMIT_HDR  ((uint32_t)sizeof(struct virtio_gpu_submit_3d))
#define PAYLOAD_MAX (4u * MIB)

static uint32_t *words; /* PAYLOAD_MAX bytes of a pattern */

/** Bind the blob test's device with the control queue's bounds on a chain,
 * `max_chain` descriptors and `max_bytes` each way, and runs of at most
 * `dma_max` bytes from the DMA pool, and give the first file a virgl
 * context. */
static void payload_bind(uint16_t max_chain, uint32_t max_bytes, uint32_t dma_max)
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

/** virtio-pci: 256 descriptors a chain, as QEMU's control queue, and runs
 * of up to a megabyte; then a transport that gives no runs. */
static void row_pci(void)
{
    payload_bind(256, 0, 256u * PAGE_SIZE);
    payload_sizes("a payload reaches a virtio-pci device whole in one command");
    payload_refused(16, PAYLOAD_MAX, -EFAULT, "a bad pointer is EFAULT and sends nothing");
    blob_unbind("the virtio-pci row leaves nothing");

    payload_bind(256, 0, 0);
    payload_whole(0, 254u * PAGE_SIZE, "a page each fills the chain without runs");
    payload_refused(UPTR(words), PAYLOAD_MAX, -ENOMEM,
                    "without runs a longer payload is ENOMEM and sends nothing");
    blob_unbind("the row without runs leaves nothing");
}

/** The shared region: 16 descriptors a chain, runs of up to 64 MiB and a
 * chain of up to the bed's 32 MiB region; then the backend's old 1 MiB. */
static void row_shared(void)
{
    payload_bind(16, 32u * MIB, 64u * MIB);
    payload_sizes("a payload reaches the shared region's device whole in one command");
    blob_unbind("the shared region's row leaves nothing");

    payload_bind(16, MIB, 64u * MIB);
    payload_whole(0, MIB - SUBMIT_HDR, "a 1 MiB chain carries 1 MiB less the header");
    payload_whole(1, MIB - SUBMIT_HDR, "a 1 MiB chain carries 1 MiB less the header");
    payload_refused(UPTR(words), MIB, -ENOMEM,
                    "past a 1 MiB chain a payload is ENOMEM and sends nothing");
    payload_refused(UPTR(words), PAYLOAD_MAX, -ENOMEM,
                    "past a 1 MiB chain a payload is ENOMEM and sends nothing");
    blob_unbind("the 1 MiB row leaves nothing");
}

/** Run the payload rows against the real driver. */
int main(void)
{
    uint32_t i;

    current_proc = &self;
    self.tg = &self_tg;
    k_memcpy(self.comm, "vkcube", sizeof("vkcube"));
    words = (uint32_t *)calloc(1, PAYLOAD_MAX);
    if (!words) {
        printf("vgpupayload-test: FAIL: no memory for the pattern\n");
        return 1;
    }
    for (i = 0; i < PAYLOAD_MAX / 4u; i++) {
        words[i] = i * 2654435761u;
    }
    row_pci();
    row_shared();
    free(words);
    printf("vgpupayload-test: %s\n", failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
