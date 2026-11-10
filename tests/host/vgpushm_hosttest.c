/*
 * vgpushm_hosttest.c: the virtio transport over a shared region,
 * rach2/sys/virtio/shmem.c, against a model device that keeps the other
 * side of the region, through the virtio core as virtio-gpu drives it.
 *
 * host-deps: rach2/sys/virtio/virtqueue.c
 * host-deps: rach2/sys/lib/kstring.c rach2/sys/lib/kctypes.c
 * host-deps: tests/host/compat/kheap_host.c tests/host/compat/spinlock_host.c
 *
 * The region is a static arena of REGION_PAGES pages whose frames are
 * made-up addresses in an order of their own, so that the transport's
 * table of frames is sorted and searched. The model answers the driver's
 * block as the VM's device does, from inside the thread's sleep, which the
 * test's wait queue runs: an offer for each new epoch, RUNNING or FAILED
 * for the setup, and each command on the control queue returned with the
 * bytes it asked for. Rows check the handshake, the queues' place in the
 * ring area and their bounds, a round trip, segments merged into one
 * descriptor, a chain too long for the device, the heap filled to ENOMEM
 * and given back, resource backing from the heap, the doorbells' hooks, the
 * control node's report, a device that stops running, a refused setup and
 * the detach.
 */

#define REGION_PAGES 16u
#include "vgpushm_env.h"

static int failures;

/** Record a failed invariant. */
static void check(int ok, const char *what)
{
    if (!ok) {
        printf("vgpushm-hosttest: fail: %s\n", what);
        failures++;
    }
}

/* ------------------------------------------------------------------------
 * The model device
 */

struct model {
    uint32_t epoch;    /* the epoch the block answers */
    int refuse;        /* answer the setup with FAILED */
    int silent;        /* answer nothing */
    uint16_t used[2];  /* the used rings' indices as the model wrote them */
    uint16_t avail[2]; /* the avail rings' indices as the model read them */
    uint32_t served, last_nseg, last_out, chain_errors;
    struct vgs_driver drv;
    struct vgs_device dev;
};

static struct model m;

/** Store the model's block: every field, then the state. */
static void model_store(void)
{
    volatile struct vgs_device *v = VGS_DEVICE(region);
    const uint8_t *src = (const uint8_t *)&m.dev;
    volatile uint8_t *dst = (volatile uint8_t *)v;
    uint32_t i;

    v->state = VGS_DEV_NONE;
    for (i = 0; i < sizeof(m.dev); i++) {
        if (i < 4 || i >= 8) {
            dst[i] = src[i];
        }
    }
    v->state = m.dev.state;
}

/** The offer of the bed's device (bed/vm/backend/main.c) for `epoch`. */
static void model_offer(uint32_t epoch)
{
    k_memset(&m.dev, 0, sizeof(m.dev));
    k_memset(m.used, 0, sizeof(m.used));
    k_memset(m.avail, 0, sizeof(m.avail));
    m.epoch = epoch;
    m.dev.epoch = epoch;
    m.dev.state = VGS_DEV_READY;
    m.dev.peer = 1;
    m.dev.transport_features = VGS_TF_DOORBELL;
    m.dev.features = VGS_F_VERSION_1 | 1u;
    m.dev.limits.max_resources = 512;
    m.dev.limits.max_contexts = 64;
    m.dev.limits.max_backing = sizeof(region);
    m.dev.limits.max_entries = 1024;
    m.dev.limits.max_chain = VGS_CHAIN_MAX;
    m.dev.limits.max_cmd = 1u << 20;
    m.dev.limits.max_queue = 256;
    m.dev.num_scanouts = 1;
    model_store();
}

/** Return every chain the driver made available on queue 0, writing its
 * writable segments with 0x5a and returning the bytes written. */
static void model_serve(void)
{
    const struct vgs_queue *q = &m.drv.queue[0];
    struct vgs_layout l;
    struct vgs_chain c;
    uint16_t idx;
    uint32_t head, put, i;

    if (vgs_snap_layout(region, &l) != VGS_OK || vgs_avail_read(region, q, m.avail[0], &idx)) {
        return;
    }
    while (m.avail[0] != idx) {
        head = vgs_avail_head(region, q, m.avail[0]);
        m.avail[0]++;
        if (vgs_chain_read(region, &l, q, &m.dev.limits, head, &c) != VGS_OK) {
            m.chain_errors++;
            continue;
        }
        m.last_nseg = c.n;
        m.last_out = c.nread;
        for (put = 0, i = c.nread; i < c.n; i++) {
            k_memset(&region[0][0] + c.seg[i].addr, 0x5a, c.seg[i].len);
            put += c.seg[i].len;
        }
        vgs_used_push(region, q, &m.used[0], head, put);
        m.served++;
    }
}

/** One look at the driver's block, as the device's loop makes it. */
static void model_step(void)
{
    struct vgs_layout l;
    int err;

    if (m.silent || vgs_snap_driver(region, &m.drv) != VGS_OK || m.drv.epoch == 0) {
        return;
    }
    if (m.drv.epoch != m.epoch) {
        if (m.drv.state == VGS_DRV_RESET) {
            model_offer(m.drv.epoch);
        }
        return;
    }
    if (m.dev.state == VGS_DEV_READY && m.drv.state == VGS_DRV_OK) {
        (void)vgs_snap_layout(region, &l);
        err = vgs_check_accept(&l, &m.drv, &m.dev);
        if (m.refuse && err == VGS_OK) {
            err = VGS_E_FEATURES;
        }
        m.dev.error = (uint32_t)err;
        m.dev.state = err == VGS_OK ? VGS_DEV_RUNNING : VGS_DEV_FAILED;
        model_store();
    } else if (m.dev.state == VGS_DEV_RUNNING) {
        model_serve();
    }
}

/* ------------------------------------------------------------------------
 * The rows
 */

static struct vshm *vs;
static struct virtqueue *ctrlq, *cursorq;
static uint32_t done_calls, done_len, removed_calls;
static void *done_token;

/** The control queue's completion. */
static void cmd_done(struct virtqueue *vq, void *token, uint32_t len)
{
    (void)vq;
    done_calls++;
    done_token = token;
    done_len = len;
}

/** The driver's removal, as virtio_gpu.c's unplugs its device. */
static void gpu_removed(struct virtio_dev *d)
{
    (void)d;
    removed_calls++;
}

/** Attach the region: its frames held, its layout written, magic last. */
static void attach(void)
{
    uintptr_t *frames = (uintptr_t *)kmalloc(REGION_PAGES * sizeof(uintptr_t));
    uint32_t i;
    int ok = 1;

    for (i = 0; i < REGION_PAGES; i++) {
        frames[i] = FRAME(i);
    }
    vs = vshm_new(0);
    check(vs != NULL, "a record made");
    check(vshm_take_region(vs, frames, REGION_PAGES, &region[0][0]) == 0, "the region taken");
    check(VGS_LAYOUT(region)->magic == VGS_MAGIC, "the layout's magic stored");
    check(vs->l.ring_off == VGS_CTRL_SIZE && vs->l.ring_len == VSHM_RING_LEN,
          "the ring area after the control page");
    check(vs->heap_pages == REGION_PAGES - 3u, "the heap is the rest");
    for (i = 0; i < REGION_PAGES; i++) {
        ok = ok && vshm_page_of(vs, FRAME(i) + 12u) == i;
    }
    check(ok, "every frame found at its page, whatever the order");
    check(vshm_page_of(vs, FOREIGN) == REGION_PAGES, "a foreign frame not found");
}

/** Negotiate through the core and set up both queues in the ring area. */
static void handshake(void)
{
    vs->vdev.removed = gpu_removed;
    check(virtio_init(&vs->vdev, VIRTIO_BIT(0), 2) == 0, "the core negotiated");
    check(m.epoch == 1 && vs->me.epoch == 1, "the first reset made epoch 1");
    check(virtio_has(&vs->vdev, VIRTIO_F_VERSION_1) && virtio_has(&vs->vdev, 0),
          "VERSION_1 and the device's feature taken");
    check(vs->me.transport_features == 0, "no doorbells without eventfds");
    check(virtio_queue_setup(&vs->vdev, 0, cmd_done, NULL, &ctrlq) == 0, "the control queue");
    check(virtio_queue_setup(&vs->vdev, 1, cmd_done, NULL, &cursorq) == 0, "the cursor queue");
    check(ctrlq->desc_addr == vs->l.ring_off && cursorq->desc_addr == vs->l.ring_off + VGS_PAGE,
          "each queue in its page of the ring area");
    check(ctrlq->max_chain == VGS_CHAIN_MAX && ctrlq->max_bytes == (1u << 20),
          "the device's bounds on a chain");
    check(vs->heap_used == 0, "the rings took no heap");
    check(virtio_ready(&vs->vdev) == 0, "DRIVER_OK taken");
    check(vs->dev.state == VGS_DEV_RUNNING && m.dev.state == VGS_DEV_RUNNING, "running");
    check(strstr(last_log, "polling") != NULL, "the transport polls");
    vs->bound = 1;
}

/** A command and its response, one segment each way. */
static void round_trip(void)
{
    struct virtio_sg sg[2];
    uint64_t addr;
    uint8_t *cmd = (uint8_t *)virtio_dma_alloc(&vs->vdev, 4096, &addr);

    check(cmd != NULL && addr >= vs->l.heap_off && addr < vs->size, "a command page in the heap");
    if (!cmd) {
        return;
    }
    sg[0].addr = addr;
    sg[0].len = 64;
    sg[1].addr = addr + 1024;
    sg[1].len = 24;
    check(virtqueue_add(ctrlq, sg, 1, 1, cmd) == 0, "a command added");
    virtqueue_kick(ctrlq);
    model_step();
    check(m.served == 1 && m.last_nseg == 2 && m.last_out == 1, "the device read the chain");
    k_memset(&region[0][0] + ctrlq->desc_addr, 0xff, (size_t)ctrlq->size * 16u);
    vshm_serve(vs, 0);
    check(done_calls == 1 && done_token == cmd && done_len == 24, "the response returned");
    check(ctrlq->nfree == ctrlq->size && ctrlq->free_head == 0 && ctrlq->next[0] == 1,
          "the chain freed by the core's own links, whatever the device wrote over the table");
    check(cmd[1024] == 0x5a && cmd[1047] == 0x5a && cmd[1048] == 0, "the device's bytes");
    virtio_dma_free(&vs->vdev, cmd, 4096);
    check(vs->heap_used == 0, "the page given back");
}

/** Adjacent segments merge into one descriptor; a chain longer than the
 * device's bound is -E2BIG before anything is queued. */
static void chains(void)
{
    struct virtio_sg sg[VGS_CHAIN_MAX + 2u];
    uint64_t addr;
    void *p = virtio_dma_alloc(&vs->vdev, 3 * 4096, &addr);
    uint32_t i;

    check(p != NULL, "three pages that follow each other");
    for (i = 0; i < 3; i++) {
        sg[i].addr = addr + i * 4096u;
        sg[i].len = 4096;
    }
    check(virtqueue_chain_fits(ctrlq, sg, 2, 1) == 1, "the merged chain fits");
    check(virtqueue_add(ctrlq, sg, 2, 1, p) == 0, "two pages out and one in added");
    model_step();
    check(m.served == 2 && m.last_nseg == 2, "the two pages out are one descriptor");
    vshm_serve(vs, 0);
    check(done_calls == 2 && done_len == 4096, "the merged chain returned");
    for (i = 0; i < VGS_CHAIN_MAX + 1u; i++) {
        sg[i].addr = addr + i * 32u;
        sg[i].len = 16;
    }
    check(!virtqueue_chain_fits(ctrlq, sg, VGS_CHAIN_MAX + 1u, 0),
          "seventeen segments with gaps do not fit");
    check(virtqueue_add(ctrlq, sg, VGS_CHAIN_MAX + 1u, 0, p) == -E2BIG, "-E2BIG");
    check(virtqueue_add(ctrlq, sg, VGS_CHAIN_MAX, 0, p) == 0, "sixteen fit");
    model_step();
    check(m.served == 3 && m.last_nseg == VGS_CHAIN_MAX && m.chain_errors == 0,
          "the device took sixteen");
    vshm_serve(vs, 0);
    virtio_dma_free(&vs->vdev, p, 3 * 4096);
}

/** Fill the heap: ENOMEM for the driver, never memory from elsewhere, and
 * the heap usable again once a page comes back. */
static void full_region(void)
{
    void *pages[REGION_PAGES];
    struct shmem sh;
    uint64_t addr, refused = vs->refused;
    uint32_t n = 0, i;

    k_memset(&sh, 0, sizeof(sh));
    while (n < REGION_PAGES && (pages[n] = virtio_dma_alloc(&vs->vdev, 4096, &addr)) != NULL) {
        n++;
    }
    check(n == vs->heap_pages && vs->heap_used == vs->heap_pages, "every heap page given");
    check(vs->refused == refused + 1u, "the refusal counted");
    check(virtio_dma_alloc(&vs->vdev, 1, &addr) == NULL, "a full heap gives nothing");
    sh.size = 4096;
    check(virtio_backing_fill(&vs->vdev, &sh, 4096) == -ENOMEM, "backing: ENOMEM");
    check(sh_frames[0] == 0, "backing: nothing taken");
    virtio_dma_free(&vs->vdev, pages[4], 4096);
    check(virtio_dma_alloc(&vs->vdev, 8192, &addr) == NULL, "no two pages in a row");
    pages[4] = virtio_dma_alloc(&vs->vdev, 4096, &addr);
    check(pages[4] != NULL, "the page given back taken again");
    for (i = 0; i < n; i++) {
        virtio_dma_free(&vs->vdev, pages[i], 4096);
    }
    check(vs->heap_used == 0, "the heap empty again");
    check(virtio_dma_alloc(&vs->vdev, (vs->heap_pages + 1u) * 4096u, &addr) == NULL,
          "more than the heap gives nothing");
}

/** A resource's backing from the heap: each frame the region's, adopted
 * with a reference of its own, its device address its offset; the ring
 * area's frames and a foreign frame have none. */
static void backing(void)
{
    struct shmem sh;
    uint64_t addr;
    uint32_t page, i;
    int ok = 1;

    k_memset(&sh, 0, sizeof(sh));
    sh.size = 3 * 4096 - 100;
    check(virtio_backing_fill(&vs->vdev, &sh, sh.size) == 0, "backing filled");
    check(vs->heap_used == 3, "three heap pages");
    page = vshm_page_of(vs, sh_frames[0]);
    for (i = 0; i < 3; i++) {
        ok = ok && sh_frames[i] == FRAME(page + i) && frame_refs[page + i] == 1;
        ok = ok && virtio_dma_addr(&vs->vdev, sh_frames[i] + 8u, &addr) == 0 &&
             addr == (uint64_t)(page + i) * VGS_PAGE + 8u;
    }
    check(ok, "the region's frames in a run, each held once, at their offsets");
    check(virtio_dma_addr(&vs->vdev, FRAME(1), &addr) == -EFAULT, "a ring frame has no address");
    check(virtio_dma_addr(&vs->vdev, FOREIGN, &addr) == -EFAULT, "a foreign frame has none");
    virtio_backing_release(&vs->vdev, &sh);
    shmem_free(&sh);
    check(vs->heap_used == 0 && frame_refs[page] == 0, "the backing given back");
}

/** A doorbell rung: counted, noted for the thread and taken once; its
 * eventfd closed: the transport polls. And what VGS_IOC_INFO reports. */
static void bells_and_info(void)
{
    struct vshm_bell b;
    struct vgs_info info;

    k_memset(&b, 0, sizeof(b));
    b.vs = vs;
    b.vector = VGS_VEC_QUEUE;
    b.hooked = 1;
    vs->bells = 1;
    check(vshm_bell_signal(&b.hook) == 1, "the bell's count taken");
    check(vs->woken[VGS_VEC_QUEUE] == 1, "the bell counted");
    check(vshm_take_rang() == 1u << VGS_VEC_QUEUE && vshm_take_rang() == 0, "rung, taken once");
    vshm_bell_hangup(&b.hook);
    check(!b.hooked && !vs->bells, "a closed bell: polling");
    vshm_info(vs, &info);
    check(info.attached == 1 && info.bound == 1 && info.epoch == 1 &&
              info.drv_state == VGS_DRV_OK && info.dev_state == VGS_DEV_RUNNING,
          "info: the connection");
    check(info.heap_pages == vs->heap_pages && info.heap_used == 0 && info.refused == 5 &&
              info.woken[VGS_VEC_QUEUE] == 1 && info.binds == 0,
          "info: the heap and the counters");
    vshm_info(NULL, &info);
    check(info.attached == 0 && info.epoch == 0, "info: no region");
}

/** A device that leaves RUNNING has the driver drop it. */
static void device_lost(void)
{
    m.dev.state = VGS_DEV_NEEDS_RESET;
    m.dev.error = VGS_E_CHAIN;
    model_store();
    vshm_serve(vs, 0);
    check(removed_calls == 1 && !vs->bound, "the driver dropped the device");
    check(strstr(last_log, "lost") != NULL, "the loss logged");
}

/** A new epoch whose setup the device refuses: DRIVER_OK fails. */
static void refused_setup(void)
{
    struct virtqueue *q0, *q1;

    virtio_teardown(&vs->vdev);
    check(vs->me.epoch == 2 && vs->me.state == VGS_DRV_RESET, "the teardown's reset");
    m.refuse = 1;
    check(virtio_init(&vs->vdev, VIRTIO_BIT(0), 2) == 0, "negotiated in epoch 3");
    check(m.epoch == 3, "the device answered epoch 3");
    check(virtio_queue_setup(&vs->vdev, 0, cmd_done, NULL, &q0) == 0 &&
              virtio_queue_setup(&vs->vdev, 1, cmd_done, NULL, &q1) == 0,
          "the queues again");
    check(virtio_ready(&vs->vdev) == -EIO, "DRIVER_OK refused");
    check(strstr(last_log, "refused") != NULL, "the refusal logged");
    virtio_teardown(&vs->vdev);
    m.refuse = 0;
}

/** The detach: GONE in a new epoch, the frames and the window given back
 * and the record freed with its last reference. */
static void detach(void)
{
    uint32_t epoch = vs->me.epoch;

    vs->stop = 1 + (int)VGS_DETACH_LOST;
    vshm_cur = vs;
    vshm_finish(vs);
    check(VGS_DRIVER(region)->state == VGS_DRV_GONE && VGS_DRIVER(region)->epoch == epoch + 1u,
          "GONE in a new epoch");
    check(unrefs == REGION_PAGES && unmaps == 1, "every frame and the window given back");
    check(vshm_cur == NULL, "no region attached");
    check(strstr(last_log, "died") != NULL, "the death logged");
}

/** Run the rows. */
int main(void)
{
    attach();
    handshake();
    round_trip();
    chains();
    full_region();
    backing();
    bells_and_info();
    device_lost();
    refused_setup();
    detach();
    if (!failures) {
        printf("vgpushm-hosttest: result: pass: layout, handshake, ring area, round trip, "
               "merged and refused chains, a full region, backing, doorbells, info, a lost device, "
               "a refused "
               "setup and the detach\n");
    }
    return failures != 0;
}
