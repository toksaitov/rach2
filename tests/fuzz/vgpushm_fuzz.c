/**
 * Fuzz harness for Rach2's side of the shared-memory GPU transport
 * (rach2/sys/virtio/shmem.c) and the virtio core under it
 * (rach2/sys/virtio/virtqueue.c): the parser of what the VM's device writes
 * into the region, its block, the used rings and any other byte it chooses
 * to change. The kernel around them is tests/host/compat/vgpushm_env.h's,
 * over a region of 32 KiB. The input is a run of steps, each an operation
 * byte and its operands, and the run ends where the input does:
 *
 *   0 block  80 bytes: the device's block, stored with its state last
 *   1 used   a queue, an index and up to 8 elements of an id and a length,
 *            stored in the queue's used ring before the index
 *   2 poke   an offset, a length and the bytes, stored anywhere in the
 *            region, as a device that ignores the rules may
 *   3 add    a queue and two lengths: a command from a heap page with that
 *            many bytes out and that many in
 *   4 reap   one pass of the transport's thread over a bound device
 *   5 bind   the core's bring-up of both queues, as virtio-gpu makes it,
 *            after a teardown of the last
 *   6 config the configuration read and its events cleared
 *   7 back   a resource's backing of 1 to 4 pages filled, each page's
 *            device address taken, and given back
 *
 * While the driver waits for the device, each pass of its sleep takes the
 * next step when it is the device's, 0 to 2; any other ends the wait as a
 * detach would. Every completion must name a command outstanding on its
 * queue and report no more bytes than the command could take, and at the
 * end, after the detach, every frame of the region must be given back.
 *
 * fuzz-deps: rach2/sys/virtio/virtqueue.c
 * fuzz-deps: rach2/sys/lib/kstring.c rach2/sys/lib/kctypes.c
 * fuzz-deps: tests/host/compat/kheap_host.c tests/host/compat/spinlock_host.c
 * fuzz-cflags: -iquote rach2/sys/arch/x86 -iquote rach2/sys/arch/i686
 */
#define REGION_PAGES 8u
#include "vgpushm_env.h"
#include "../fuzz/fuzz.h"

/* Commands outstanding on one queue at most, and the page each holds. */
#define CMDS 16

struct cmd {
    uint8_t *page;
    uint32_t in;
    int queue;
};

static const uint8_t *in_p, *in_end;
static struct vshm *vs;
static struct cmd cmds[CMDS];

/** Stop at a broken invariant, as a sanitizer does. */
static void check(int ok, const char *what)
{
    if (!ok) {
        printf("vgpushm-fuzz: fault: %s\n", what);
        __builtin_trap();
    }
}

/** The next `n` bytes of the input, or NULL with the input ended. */
static const uint8_t *take(uint32_t n)
{
    const uint8_t *p = in_p;

    if ((uint32_t)(in_end - in_p) < n) {
        in_p = in_end;
        return NULL;
    }
    in_p += n;
    return p;
}

/** A little-endian 16-bit operand, 0 past the input's end. */
static uint32_t take16(void)
{
    const uint8_t *p = take(2);

    return p ? (uint32_t)p[0] | (uint32_t)p[1] << 8 : 0;
}

/** A little-endian 32-bit operand, 0 past the input's end. */
static uint32_t take32(void)
{
    const uint8_t *p = take(4);

    return p ? (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24
             : 0;
}

/** Step 0: the device's block from the input, its state stored last. */
static void dev_block(void)
{
    const uint8_t *p = take(sizeof(struct vgs_device));
    volatile uint8_t *dst = &region[0][0] + VGS_DEVICE_OFF;
    uint32_t i;

    if (!p) {
        return;
    }
    for (i = 0; i < sizeof(struct vgs_device); i++) {
        if (i < 4 || i >= 8) {
            dst[i] = p[i];
        }
    }
    for (i = 4; i < 8; i++) {
        dst[i] = p[i];
    }
}

/** Step 1: elements and an index stored in a queue's used ring. */
static void dev_used(void)
{
    const uint8_t *q = take(1);
    struct virtqueue *vq;
    uint32_t idx, n, k;

    if (!q) {
        return;
    }
    vq = vs->vdev.queues[*q & 1u];
    idx = take16();
    n = (q[0] >> 1) % 9u;
    for (k = 0; k < n; k++) {
        uint32_t id = take32(), len = take32();

        if (vq) {
            vq->used_ring[(idx - n + k) & (vq->size - 1u)].id = id;
            vq->used_ring[(idx - n + k) & (vq->size - 1u)].len = len;
        }
    }
    if (vq) {
        vq->used[1] = (uint16_t)idx;
    }
}

/** Step 2: bytes stored anywhere in the region. */
static void dev_poke(void)
{
    uint32_t off = take16() % sizeof(region), len;
    const uint8_t *n = take(1), *p;

    len = n ? *n : 0;
    if (off + len > sizeof(region)) {
        len = (uint32_t)sizeof(region) - off;
    }
    p = take(len);
    if (p) {
        k_memcpy(&region[0][0] + off, p, len);
    }
}

/** The device's step from the input inside a wait: the next step when it
 * is the device's, else the wait ended as a detach ends it. */
static void model_step(void)
{
    if (in_p == in_end || *in_p % 8u > 2) {
        vs->stop = 1;
        return;
    }
    switch (*in_p++ % 8u) {
    case 0:
        dev_block();
        break;
    case 1:
        dev_used();
        break;
    default:
        dev_poke();
        break;
    }
}

/** A command's completion: one outstanding, and no more bytes than it can
 * take. */
static void cmd_done(struct virtqueue *vq, void *token, uint32_t len)
{
    struct cmd *c = (struct cmd *)token;

    check(c >= cmds && c < cmds + CMDS && c->page, "a completion of no command");
    check(c->queue == vq->index, "a completion on another queue");
    check(len <= c->in, "a completion of more bytes than the command takes");
    virtio_dma_free(&vs->vdev, c->page, VGS_PAGE);
    c->page = NULL;
}

/** The driver's removal: the device is unbound. */
static void gpu_removed(struct virtio_dev *d)
{
    (void)d;
}

/** Give back every command's page: the core has dropped them. */
static void cmds_drop(void)
{
    int i;

    for (i = 0; i < CMDS; i++) {
        if (cmds[i].page) {
            virtio_dma_free(&vs->vdev, cmds[i].page, VGS_PAGE);
            cmds[i].page = NULL;
        }
    }
}

/** Step 3: a command on a queue, from a heap page. */
static void drv_add(void)
{
    const uint8_t *q = take(1);
    uint32_t out = take16() % 2048u + 1u, inb = take16() % 2048u;
    struct virtqueue *vq = q ? vs->vdev.queues[*q & 1u] : NULL;
    struct virtio_sg sg[2];
    uint64_t addr;
    struct cmd *c = NULL;
    int i;

    for (i = 0; i < CMDS && !c; i++) {
        c = cmds[i].page ? NULL : &cmds[i];
    }
    if (!vq || !vs->bound || !c) {
        return;
    }
    c->page = (uint8_t *)virtio_dma_alloc(&vs->vdev, VGS_PAGE, &addr);
    if (!c->page) {
        return;
    }
    check(addr >= vs->l.heap_off && addr + VGS_PAGE <= vs->size, "a command outside the heap");
    c->in = inb;
    c->queue = vq->index;
    sg[0].addr = addr;
    sg[0].len = out;
    sg[1].addr = addr + 2048u;
    sg[1].len = inb;
    if (virtqueue_add(vq, sg, 1, inb ? 1 : 0, c) != 0) {
        virtio_dma_free(&vs->vdev, c->page, VGS_PAGE);
        c->page = NULL;
        return;
    }
    virtqueue_kick(vq);
}

/** Step 5: the core's bring-up of both queues after a teardown. */
static void drv_bind(void)
{
    struct virtqueue *q0, *q1;

    if (vs->bound) {
        return;
    }
    virtio_teardown(&vs->vdev);
    cmds_drop();
    vs->stop = 0;
    if (virtio_init(&vs->vdev, VIRTIO_BIT(0) | VIRTIO_BIT(1), 2) == 0 &&
        virtio_queue_setup(&vs->vdev, 0, cmd_done, NULL, &q0) == 0 &&
        virtio_queue_setup(&vs->vdev, 1, cmd_done, NULL, &q1) == 0 &&
        virtio_ready(&vs->vdev) == 0) {
        vs->bound = 1;
    }
    vs->stop = 0;
}

/** Step 6: the configuration as virtio-gpu reads it, events cleared. */
static void drv_config(void)
{
    uint32_t cfg[4], clear = 1;

    virtio_config_read(&vs->vdev, 0, cfg, sizeof(cfg));
    check(cfg[1] == 0, "events_clear reads nonzero");
    virtio_config_write(&vs->vdev, 4, &clear, 4);
}

/** Step 7: a resource's backing from the heap, its addresses, and back. */
static void drv_backing(void)
{
    const uint8_t *n = take(1);
    struct shmem sh;
    uint64_t addr;
    uint32_t i, pages = n ? *n % 4u + 1u : 1u;

    k_memset(&sh, 0, sizeof(sh));
    sh.size = (uint64_t)pages * VGS_PAGE;
    if (virtio_backing_fill(&vs->vdev, &sh, sh.size) != 0) {
        check(sh_frames[0] == 0, "a refused backing kept a page");
        return;
    }
    for (i = 0; i < pages; i++) {
        check(virtio_dma_addr(&vs->vdev, sh_frames[i], &addr) == 0 && addr >= vs->l.heap_off &&
                  addr < vs->size,
              "a backing page outside the heap");
    }
    virtio_backing_release(&vs->vdev, &sh);
    shmem_free(&sh);
}

/** Attach a fresh region, as the control node does. */
static void attach(void)
{
    uintptr_t *frames = (uintptr_t *)kmalloc(REGION_PAGES * sizeof(uintptr_t));
    uint32_t i;

    k_memset(region, 0, sizeof(region));
    k_memset(frame_refs, 0, sizeof(frame_refs));
    k_memset(cmds, 0, sizeof(cmds));
    unrefs = unmaps = 0;
    vshm_rang = 0;
    for (i = 0; i < REGION_PAGES; i++) {
        frames[i] = FRAME(i);
    }
    vs = vshm_new(0);
    check(vshm_take_region(vs, frames, REGION_PAGES, &region[0][0]) == 0, "the region refused");
    vs->vdev.removed = gpu_removed;
}

/** Detach: every frame and the window given back, the record freed. */
static void detach(void)
{
    uint32_t i;

    virtio_teardown(&vs->vdev);
    cmds_drop();
    check(vs->heap_used == 0, "heap pages kept after every command was given back");
    vs->stop = 1 + (int)VGS_DETACH_LOST;
    vshm_cur = vs;
    vshm_finish(vs);
    check(unrefs == REGION_PAGES && unmaps == 1, "a frame of the region kept after the detach");
    for (i = 0; i < REGION_PAGES; i++) {
        check(frame_refs[i] == 0, "a reference to a frame kept after the detach");
    }
    vs = NULL;
}

int LLVMFuzzerTestOneInput(const unsigned char *data, unsigned long size)
{
    in_p = data;
    in_end = data + size;
    attach();
    while (in_p < in_end) {
        switch (*in_p++ % 8u) {
        case 0:
            dev_block();
            break;
        case 1:
            dev_used();
            break;
        case 2:
            dev_poke();
            break;
        case 3:
            drv_add();
            break;
        case 4:
            if (vs->bound) {
                vshm_serve(vs, 0);
            }
            break;
        case 5:
            drv_bind();
            break;
        case 6:
            drv_config();
            break;
        default:
            drv_backing();
            break;
        }
        check(vs->heap_used <= vs->heap_pages, "more heap pages used than the heap has");
    }
    detach();
    return 0;
}
