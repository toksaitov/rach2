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
 * and given back, backing in one run and in pieces, partial adoption, the
 * doorbells' hooks, an eventfd given both as a doorbell the device rings
 * and one it is rung by refused, the reap a ring of the queue's doorbell has
 * the interrupt thread make while the driver's interrupts are on, the
 * control node's report, a device that stops running, a refused setup, the
 * detach, a backing held through a detach while the same frames are
 * attached, detached and attached again, and a backing released across a
 * missing leaf of its table while the region detaches between its leaves.
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

/** Check the `n` pages adopted from the isolated gaps of the heap: distinct
 * frames, each held once, zeroed and addressed at its region offset. */
static void fragmented_frames(uint32_t n)
{
    uint8_t seen[REGION_PAGES] = {0};
    uint64_t addr;
    uint32_t i, page;
    int ok = 1;

    for (i = 0; i < n; i++) {
        page = vshm_page_of(vs, sh_frames[i]);
        if (page < heap_page(vs, 0) || page >= vs->npages) {
            ok = 0;
            continue;
        }
        ok = ok && !seen[page] && (page - heap_page(vs, 0)) % 2u == 0 && frame_refs[page] == 1 &&
             heap_bit(vs, page - heap_page(vs, 0));
        seen[page] = 1;
        ok = ok && virtio_dma_addr(&vs->vdev, sh_frames[i], &addr) == 0 &&
             addr == (uint64_t)page * VGS_PAGE && region[page][0] == 0;
    }
    check(ok, "fragmented backing: distinct zeroed heap frames, each held once at its offset");
}

/** Fill a backing from isolated free pages, refuse more than the heap has
 * free, and give every page back without touching the command pages. */
static void fragmented_backing(void)
{
    struct shmem sh;
    uint8_t *heap;
    uint64_t addr, refused = vs->refused;
    uint32_t i, free_pages = (vs->heap_pages + 1u) / 2u;
    int ok = 1;

    heap = virtio_dma_alloc(&vs->vdev, vs->heap_pages * VGS_PAGE, &addr);
    check(heap != NULL, "fragmented backing: reserve the whole heap");
    if (!heap) {
        return;
    }
    k_memset(heap, 0xa5, vs->heap_pages * VGS_PAGE);
    for (i = 0; i < vs->heap_pages; i += 2) {
        virtio_dma_free(&vs->vdev, heap + i * VGS_PAGE, VGS_PAGE);
    }
    k_memset(&sh, 0, sizeof(sh));
    sh.size = (free_pages + 1u) * VGS_PAGE;
    check(virtio_backing_fill(&vs->vdev, &sh, sh.size) == -ENOMEM && vs->refused == refused + 1u &&
              vs->heap_used == vs->heap_pages - free_pages,
          "fragmented backing: a request above the free pages takes nothing and counts once");
    for (i = 0; i < SH_PAGES; i++) {
        ok = ok && sh_frames[i] == 0;
    }
    for (i = 0; i < vs->heap_pages; i++) {
        ok = ok && heap_bit(vs, i) == (i % 2u != 0) && frame_refs[heap_page(vs, i)] == 0;
    }
    check(ok, "fragmented backing: refusal keeps every heap bit and reference");
    sh.size = free_pages * VGS_PAGE;
    check(virtio_backing_fill(&vs->vdev, &sh, sh.size) == 0 && vs->heap_used == vs->heap_pages &&
              vs->refused == refused + 1u,
          "fragmented backing: every isolated free page taken, with no refusal");
    fragmented_frames(free_pages);
    virtio_backing_release(&vs->vdev, &sh);
    shmem_free(&sh);
    for (i = 0, ok = 1; i < vs->heap_pages; i++) {
        ok = ok && heap_bit(vs, i) == (i % 2u != 0) && frame_refs[heap_page(vs, i)] == 0;
        if (i % 2u) {
            ok = ok && heap[i * VGS_PAGE] == 0xa5;
        }
    }
    check(ok && vs->heap_used == vs->heap_pages - free_pages,
          "fragmented backing: release restores the gaps and keeps the commands");
    virtio_dma_free(&vs->vdev, heap, vs->heap_pages * VGS_PAGE);
    check(vs->heap_used == 0, "fragmented backing: the whole heap free again");
}

/** Refuse adoption part way through the third piece: the model holds only
 * eight pages, so its ninth fails after two pages of that piece. */
static void partial_backing(void)
{
    struct shmem sh;
    uint8_t *heap;
    uint64_t addr, refused = vs->refused;
    uint32_t i;
    int ok = 1;

    heap = virtio_dma_alloc(&vs->vdev, vs->heap_pages * VGS_PAGE, &addr);
    check(heap != NULL, "partial backing: reserve the whole heap");
    if (!heap) {
        return;
    }
    for (i = 0; i < 11; i++) {
        if (i % 4u != 3) {
            virtio_dma_free(&vs->vdev, heap + i * VGS_PAGE, VGS_PAGE);
        }
    }
    k_memset(&sh, 0, sizeof(sh));
    sh.size = 9 * VGS_PAGE;
    check(virtio_backing_fill(&vs->vdev, &sh, sh.size) == -ENOMEM && vs->refused == refused + 1u &&
              vs->heap_used == vs->heap_pages - 9u,
          "partial backing: failed adoption gives every piece back and counts once");
    for (i = 0; i < SH_PAGES; i++) {
        ok = ok && sh_frames[i] == 0;
    }
    for (i = 0; i < vs->heap_pages; i++) {
        ok = ok && heap_bit(vs, i) == (i >= 11 || i % 4u == 3) && frame_refs[heap_page(vs, i)] == 0;
    }
    check(ok, "partial backing: no adopted frame or reference left, only the commands held");
    virtio_dma_free(&vs->vdev, heap, vs->heap_pages * VGS_PAGE);
    check(vs->heap_used == 0, "partial backing: the whole heap free again");
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

/** The references on the four doorbells' eventfds, summed. */
static unsigned bell_held(void)
{
    return bell_refs[0] + bell_refs[1] + bell_refs[2] + bell_refs[3];
}

/** An attach's doorbells: an eventfd given both as one the device rings and
 * one it is rung by refused with EINVAL, and nothing held once the attach
 * gives its record back; four distinct ones taken, and each given back. */
static void bells_crossed(void)
{
    struct vgs_attach a;
    struct vshm *v;

    k_memset(&a, 0, sizeof(a));
    a.bell_in[0] = 0;
    a.bell_in[1] = 1;
    a.bell_out[0] = 2;
    a.bell_out[1] = 0;
    v = vshm_new(0);
    check(v != NULL && vshm_bells_get(v, &a) == -EINVAL,
          "bells: bell_out[1] as bell_in[0] refused with EINVAL");
    vshm_release(v);
    vshm_free(v);
    check(bell_held() == 0, "bells: nothing held after the refusal");
    a.bell_out[0] = 1;
    a.bell_out[1] = 3;
    v = vshm_new(0);
    check(v != NULL && vshm_bells_get(v, &a) == -EINVAL,
          "bells: bell_out[0] as bell_in[1] refused with EINVAL");
    vshm_release(v);
    vshm_free(v);
    a.bell_out[0] = 2;
    v = vshm_new(0);
    check(v != NULL && vshm_bells_get(v, &a) == 0 && bell_refs[0] == 1 && bell_refs[1] == 1 &&
              bell_refs[2] == 1 && bell_refs[3] == 1,
          "bells: four distinct eventfds taken");
    vshm_release(v);
    vshm_free(v);
    check(bell_held() == 0, "bells: each given back");
}

/** The queue's doorbell has the interrupt thread reap the queues while the
 * driver's interrupts are on, never the hook itself, which runs under the
 * eventfd's lock; after their release a ring reaps nothing. */
static void bell_reaps(void)
{
    struct vshm_bell b;
    struct virtio_sg sg[2];
    uint64_t addr;
    uint8_t *cmd = (uint8_t *)virtio_dma_alloc(&vs->vdev, 4096, &addr);
    uint32_t calls;

    k_memset(&b, 0, sizeof(b));
    b.vs = vs;
    b.vector = VGS_VEC_QUEUE;
    check(cmd != NULL && vs->irq_on, "bell: a command page, the driver's interrupts on");
    if (!cmd) {
        return;
    }
    vshm_irq_run(vs); /* the ring of bells_and_info, with nothing out */
    sg[0].addr = addr;
    sg[0].len = 64;
    sg[1].addr = addr + 1024;
    sg[1].len = 24;
    check(virtqueue_add(ctrlq, sg, 1, 1, cmd) == 0, "bell: a command added");
    virtqueue_kick(ctrlq);
    model_step();
    calls = done_calls;
    vshm_irq_run(vs);
    check(done_calls == calls, "bell: no reap before the doorbell rings");
    check(vshm_bell_signal(&b.hook) == 1 && done_calls == calls && vs->irq_due && vshm_irq_rang,
          "bell: the hook reaps nothing itself and wakes the interrupt thread");
    vshm_irq_run(vs);
    check(done_calls == calls + 1 && done_token == cmd && !vs->irq_due,
          "bell: the interrupt thread's reap answers the waiter");
    vs_irq_release(&vs->vdev);
    check(virtqueue_add(ctrlq, sg, 1, 1, cmd) == 0, "bell: a command after the release");
    virtqueue_kick(ctrlq);
    model_step();
    (void)vshm_bell_signal(&b.hook);
    vshm_irq_run(vs);
    check(done_calls == calls + 1, "bell: no reap once the interrupts are released");
    vshm_serve(vs, 0);
    check(done_calls == calls + 2, "bell: the thread's pass answers it");
    vs_irq_start(&vs->vdev);
    vshm_irq_rang = 0;
    (void)vshm_take_rang();
    virtio_dma_free(&vs->vdev, cmd, 4096);
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

/** The second region of `held_through_detach` detached while `old` still
 * holds the backing at `page` and the next: the marks its attach made for
 * them leave its heap, so its record is not kept, and a third region
 * attached in its place leaves them out again. */
static void detach_while_held(struct vshm *old, uint32_t page)
{
    struct vshm *second = vs;

    second->refs++; /* the driver's bind */
    second->stop = 1 + (int)VGS_DETACH_ORDERLY;
    vshm_finish(second);
    check(second->heap_used == 0 && !second->gone && second->heap_map == NULL && vshm_kept == old &&
              old->gone_next == NULL && old->heap_used == 2,
          "held: a detach of the second region clears the old record's marks and keeps no record");
    vs_put(&second->vdev);
    attach();
    vshm_cur = vs;
    check(vs->heap_used == 2 && heap_bit(vs, page - heap_page(vs, 0)) &&
              heap_bit(vs, page + 1u - heap_page(vs, 0)),
          "held: a third region leaves the backing's frames out of its heap");
}

/** A backing and a command's page held through the detach, as an exported
 * buffer's backing is through a reset: the record keeps its heap's tables,
 * a region attached again over the same frames leaves the held pages out of
 * its heap, which the attach's `vshm_take_region` does, and a detach of
 * that region clears those marks from its heap, so that its record is not
 * kept for them; a third attach leaves them out again, and each page's
 * release gives it back to that heap, the last freeing the detached
 * record's tables. */
static void held_through_detach(void)
{
    struct vshm *old;
    struct shmem sh;
    uint64_t addr;
    uint32_t page, cmd_page, u0;
    void *cmd;

    attach();
    old = vs;
    k_memset(&sh, 0, sizeof(sh));
    sh.size = 2 * VGS_PAGE;
    check(virtio_backing_fill(&old->vdev, &sh, sh.size) == 0 && old->heap_used == 2,
          "held: a backing of two pages");
    page = vshm_page_of(old, sh_frames[0]);
    cmd = virtio_dma_alloc(&old->vdev, VGS_PAGE, &addr);
    cmd_page = (uint32_t)(addr / VGS_PAGE);
    check(cmd != NULL && old->heap_used == 3, "held: and a command's page");
    old->refs++; /* the driver's bind */
    old->stop = 1 + (int)VGS_DETACH_ORDERLY;
    vshm_cur = old;
    u0 = unrefs;
    vshm_finish(old);
    check(old->gone && vshm_kept == old && !old->attached && old->heap_used == 3 &&
              old->heap_map != NULL && old->rev != NULL && old->frames != NULL,
          "held: the detached record keeps its heap's tables");
    attach();
    vshm_cur = vs;
    check(vs->heap_used == 3 && heap_bit(vs, page - heap_page(vs, 0)) &&
              heap_bit(vs, page + 1u - heap_page(vs, 0)) &&
              heap_bit(vs, cmd_page - heap_page(vs, 0)),
          "held: the same frames left out of the new heap");
    check(virtio_dma_alloc(&vs->vdev, vs->heap_pages * VGS_PAGE, &addr) == NULL,
          "held: the new heap is not whole");
    virtio_dma_free(&old->vdev, cmd, VGS_PAGE);
    check(vs->heap_used == 2 && old->heap_used == 2 && old->gone &&
              !heap_bit(vs, cmd_page - heap_page(vs, 0)),
          "held: the command's page given back to both heaps after the detach");
    detach_while_held(old, page);
    virtio_backing_release(&old->vdev, &sh);
    shmem_free(&sh);
    check(vs->heap_used == 0 && !old->gone && vshm_kept == NULL && old->heap_map == NULL &&
              old->frames == NULL,
          "held: the release gives the pages to the new heap and frees the old tables");
    check(unrefs - u0 == 2u * REGION_PAGES && frame_refs[page] == 0 && frame_refs[page + 1u] == 0,
          "held: every reference given back once");
    vs_put(&old->vdev);
    vs->stop = 1 + (int)VGS_DETACH_ORDERLY;
    vshm_finish(vs);
    check(vshm_cur == NULL && vshm_kept == NULL, "held: the new region detached with none kept");
}

/* What `detach_between_leaves` saw: the record attached, and its heap's
 * pages in use. */
static int hook_attached;
static uint32_t hook_used;

/** The detach of `vs` that another processor makes while a backing's
 * release has let vshm_lock go between leaves, with the driver's bind. */
static void detach_between_leaves(void)
{
    hook_attached = vs->attached;
    hook_used = vs->heap_used;
    vs->refs++; /* the driver's bind */
    vs->stop = 1 + (int)VGS_DETACH_ORDERLY;
    vshm_cur = vs;
    vshm_finish(vs);
}

/** A backing whose pages lie in leaf 0 and in leaf 2 of its table, leaf 1
 * missing, released while the region detaches between the leaves: the
 * release walks no page of the missing leaf, gives leaf 0's page to the
 * attached heap, and the far page to the detached record, which it frees
 * with the record's last page. */
static void release_across_leaves(void)
{
    struct shmem sh;
    uint32_t page, far_page;

    attach();
    vshm_cur = vs;
    k_memset(&sh, 0, sizeof(sh));
    sh.size = 2 * VGS_PAGE;
    check(virtio_backing_fill(&vs->vdev, &sh, sh.size) == 0 && vs->heap_used == 2 &&
              sh.nleaves == 1,
          "leaves: a backing of two pages in leaf 0");
    page = vshm_page_of(vs, sh_frames[0]);
    far_page = vshm_page_of(vs, sh_frames[1]);
    sh_far[0] = sh_frames[1];
    sh_frames[1] = 0;
    sh_dir[SH_LEAVES - 1u] = sh_far;
    sh.nleaves = SH_LEAVES;
    sh.npages = SH_LEAVES * SHMEM_LEAF;
    sh.size = ((SH_LEAVES - 1u) * SHMEM_LEAF + 1u) * (uint64_t)VGS_PAGE;
    hook_attached = 0;
    hook_used = 0;
    sh_lookups = 0;
    irq_on_hook = detach_between_leaves;
    virtio_backing_release(&vs->vdev, &sh);
    check(irq_on_hook == NULL && hook_attached && hook_used == 1,
          "leaves: the lock let go after leaf 0, its page given back and the far page held");
    check(sh_lookups < 2u * SHMEM_LEAF, "leaves: no page of the missing leaf walked");
    check(vs->heap_used == 0 && !vs->gone && vshm_kept == NULL && vs->heap_map == NULL &&
              vs->frames == NULL,
          "leaves: the far page given to the detached record, which is freed with it");
    shmem_free(&sh);
    check(frame_refs[page] == 0 && frame_refs[far_page] == 0,
          "leaves: both pages' references given back");
    vs_put(&vs->vdev);
    check(vshm_cur == NULL && vshm_kept == NULL, "leaves: no region attached and none kept");
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
    bells_crossed();
    bell_reaps();
    fragmented_backing();
    partial_backing();
    device_lost();
    refused_setup();
    detach();
    held_through_detach();
    release_across_leaves();
    if (!failures) {
        printf("vgpushm-hosttest: result: pass: layout, handshake, ring area, round trip, "
               "merged and refused chains, a full region, backing in one run and in pieces, "
               "partial adoption, doorbells, a crossed doorbell refused, the doorbell's reap, "
               "info, a lost device, "
               "a refused setup, the detach, a backing held through it and one released "
               "across a missing leaf and a detach\n");
    }
    return failures != 0;
}
