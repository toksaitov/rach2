/*
 * virtqueue_hosttest.c: the virtio core of rach2/sys/virtio/virtqueue.c
 * against a model device reached only through the transport's table.
 *
 * host-deps: rach2/sys/virtio/virtqueue.c
 * host-deps: rach2/sys/lib/kstring.c rach2/sys/lib/kctypes.c
 * host-deps: tests/host/compat/kheap_host.c tests/host/compat/spinlock_host.c
 *
 * The model fills `struct virtio_ops` as a transport does: a status byte, 64
 * offered feature bits, a configuration space with its generation, queues
 * with their sizes and addresses, notifications and interrupts that it
 * counts, and memory that it gives a page at a time at device addresses that
 * are not host addresses, as a transport over one shared region would give
 * offsets. It reads the queues the core wrote only through those addresses,
 * walks each chain, direct or indirect, writes the device-writable buffers
 * and returns them on the used ring, as a device does. Rows check what the
 * core returned, what the model saw, and the memory the model still holds.
 */

#include <stdarg.h>

extern int printf(const char *format, ...);
extern int vsnprintf(char *out, unsigned long size, const char *fmt, va_list ap);
extern char *strstr(const char *haystack, const char *needle);

#include "virtio/virtio.h"
#include "klog.h"
#include "udelay.h"
#include "lib/kerrno.h"
#include "lib/kirq.h"
#include "lib/kstring.h"

/* The model's memory: ARENA_PAGES pages, page i at device address
 * DEV_BASE + i * 4096. */
#define ARENA_PAGES 48
#define DEV_BASE    0x40000000ull

static int failures;
static char last_log[256];
static int logs;
static uint32_t udelays, fences;

/** Record a failed invariant. */
static void check(int ok, const char *what)
{
    if (!ok) {
        printf("virtqueue-hosttest: fail: %s\n", what);
        failures++;
    }
}

/** Keep the record as the last line logged. */
void klog(int level, const char *subsystem, const char *format, ...)
{
    va_list ap;

    (void)level;
    (void)subsystem;
    va_start(ap, format);
    vsnprintf(last_log, sizeof(last_log), format, ap);
    va_end(ap);
    logs++;
}

/** Count a wait; the model's reset advances on each. */
void k_udelay(uint32_t usec)
{
    (void)usec;
    udelays++;
}

/** Count a fence. */
void arch_dma_fence(void)
{
    fences++;
}

/** One processor with interrupts as the rows leave them. */
uint32_t kirq_save(void)
{
    return 0;
}

/** Nothing to put back. */
void kirq_restore(uint32_t flags)
{
    (void)flags;
}

/* The model device. */
struct model_queue {
    int enabled;
    uint16_t size;
    uint64_t desc, avail, used;
    uint16_t last_avail; /* the driver's ring as the model has read it */
    uint16_t used_idx;   /* the device's ring as the model has written it */
    uint32_t notified;
};

struct model {
    struct virtio_dev dev;
    uint8_t status;
    uint8_t status_log[16]; /* each status write, in order */
    int nstatus;
    int reset_polls; /* reads of a nonzero status left before a reset ends;
                      * -1 never */
    int resetting;
    uint64_t offered, driver;
    int refuse; /* clear FEATURES_OK when it is written */
    uint8_t config[64];
    uint32_t generation;
    int flips; /* generation reads that each change it */
    uint16_t qmax[VIRTIO_MAX_QUEUES];
    struct model_queue q[VIRTIO_MAX_QUEUES];
    int irq_setups, irq_starts, irq_releases;
    uint16_t irq_nqueues;
    int started_before_ok; /* irq_start ran while DRIVER_OK was clear */
    int dma_fail;          /* refuse every allocation */
    int dma_out;           /* pages given and not freed */
};

static struct model m;
static uint8_t arena[ARENA_PAGES][4096] __attribute__((aligned(4096)));
static uint8_t arena_taken[ARENA_PAGES];

/** The host byte at device address `addr`, `len` bytes of it within one
 * page; a row fails and NULL comes back for anything else. */
static uint8_t *dev_ptr(uint64_t addr, uint32_t len)
{
    uint64_t off = addr - DEV_BASE;
    uint64_t page = off / 4096u;

    if (addr < DEV_BASE || page >= ARENA_PAGES || !arena_taken[page] || off % 4096u + len > 4096u) {
        check(0, "the core gave the device an address it was not given");
        return 0;
    }
    return &arena[page][off % 4096u];
}

/** Return the record behind `d`, the model. */
static struct model *mod(struct virtio_dev *d)
{
    return (struct model *)d->transport;
}

/** The status byte; a reset ends after `reset_polls` reads. */
static uint8_t m_get_status(struct virtio_dev *d)
{
    struct model *x = mod(d);

    if (x->resetting) {
        if (x->reset_polls == 0) {
            x->resetting = 0;
            x->status = 0;
        } else {
            if (x->reset_polls > 0) {
                x->reset_polls--;
            }
            return 0x01;
        }
    }
    return x->status;
}

/** Write the status byte; 0 starts a reset that clears the queues. */
static void m_set_status(struct virtio_dev *d, uint8_t status)
{
    struct model *x = mod(d);
    int i;

    if (x->nstatus < 16) {
        x->status_log[x->nstatus++] = status;
    }
    if (status == 0) {
        x->resetting = 1;
        x->driver = 0;
        for (i = 0; i < VIRTIO_MAX_QUEUES; i++) {
            k_memset(&x->q[i], 0, sizeof(x->q[i]));
        }
        return;
    }
    if ((status & VIRTIO_STATUS_FEATURES_OK) && x->refuse) {
        status &= (uint8_t)~VIRTIO_STATUS_FEATURES_OK;
    }
    x->status = status;
}

/** The offered bits. */
static uint64_t m_get_features(struct virtio_dev *d)
{
    return mod(d)->offered;
}

/** Keep the accepted bits. */
static void m_set_features(struct virtio_dev *d, uint64_t features)
{
    mod(d)->driver = features;
}

/** The generation, which changes on each of `flips` reads. */
static uint32_t m_generation(struct virtio_dev *d)
{
    struct model *x = mod(d);

    if (x->flips != 0) {
        if (x->flips > 0) {
            x->flips--;
        }
        x->generation++;
        x->config[0]++;
    }
    return x->generation;
}

/** Copy out of `config`. */
static void m_config_read(struct virtio_dev *d, uint32_t offset, void *buf, uint32_t len)
{
    k_memcpy(buf, mod(d)->config + offset, len);
}

/** Copy into `config`. */
static void m_config_write(struct virtio_dev *d, uint32_t offset, const void *buf, uint32_t len)
{
    k_memcpy(mod(d)->config + offset, buf, len);
}

/** The size the row gave queue `index`. */
static uint16_t m_queue_max(struct virtio_dev *d, uint16_t index)
{
    return index < VIRTIO_MAX_QUEUES ? mod(d)->qmax[index] : 0;
}

/** Take the queue's size and addresses; vector 3 + index. */
static int m_queue_enable(struct virtio_dev *d, struct virtqueue *vq)
{
    struct model_queue *q = &mod(d)->q[vq->index];

    q->enabled = 1;
    q->size = vq->size;
    q->desc = vq->desc_addr;
    q->avail = vq->avail_addr;
    q->used = vq->used_addr;
    vq->vector = (uint16_t)(3u + vq->index);
    return 0;
}

/** Count a notification. */
static void m_notify(struct virtio_dev *d, struct virtqueue *vq)
{
    mod(d)->q[vq->index].notified++;
}

/** Count the interrupt calls. */
static int m_irq_setup(struct virtio_dev *d, uint16_t nqueues)
{
    mod(d)->irq_setups++;
    mod(d)->irq_nqueues = nqueues;
    return 0;
}

/** Count a start, and whether it came before DRIVER_OK. */
static void m_irq_start(struct virtio_dev *d)
{
    mod(d)->irq_starts++;
    if (!(mod(d)->status & VIRTIO_STATUS_DRIVER_OK)) {
        mod(d)->started_before_ok = 1;
    }
}

/** Count a release. */
static void m_irq_release(struct virtio_dev *d)
{
    mod(d)->irq_releases++;
}

/** A page of the arena at its device address. */
static void *m_dma_alloc(struct virtio_dev *d, uint32_t bytes, uint64_t *addr)
{
    int i;

    if (mod(d)->dma_fail || bytes == 0 || bytes > 4096u) {
        return 0;
    }
    for (i = 0; i < ARENA_PAGES; i++) {
        if (!arena_taken[i]) {
            arena_taken[i] = 1;
            k_memset(arena[i], 0, 4096);
            *addr = DEV_BASE + (uint64_t)i * 4096u;
            mod(d)->dma_out++;
            return arena[i];
        }
    }
    return 0;
}

/** Give a page back. */
static void m_dma_free(struct virtio_dev *d, void *p, uint32_t bytes)
{
    long i = ((uint8_t *)p - &arena[0][0]) / 4096;

    (void)bytes;
    check(i >= 0 && i < ARENA_PAGES && arena_taken[i] && (uint8_t *)p == arena[i],
          "dma_free is given a page dma_alloc gave");
    if (i >= 0 && i < ARENA_PAGES) {
        arena_taken[i] = 0;
    }
    mod(d)->dma_out--;
}

/** Region 1 only. */
static int m_shm_region(struct virtio_dev *d, uint8_t id, uint64_t *addr, uint64_t *len)
{
    (void)d;
    if (id != 1) {
        return -ENOENT;
    }
    *addr = 0x380000000000ull;
    *len = 0x100000000ull;
    return 0;
}

static const struct virtio_ops model_ops = {
    "model",      m_get_status,  m_set_status,   m_get_features, m_set_features,
    m_generation, m_config_read, m_config_write, m_queue_max,    m_queue_enable,
    m_notify,     m_irq_setup,   m_irq_start,    m_irq_release,  m_dma_alloc,
    m_dma_free,   m_shm_region,  NULL,           NULL,           NULL,
    NULL,         NULL,          NULL,           NULL,           NULL};

/** A fresh model offering VERSION_1 and INDIRECT_DESC and device bits 0, 1
 * and 5, with two queues of 256. */
static struct virtio_dev *model_reset(void)
{
    k_memset(&m, 0, sizeof(m));
    k_memset(arena_taken, 0, sizeof(arena_taken));
    m.dev.ops = &model_ops;
    m.dev.transport = &m;
    m.dev.name = "model0";
    m.dev.id = VIRTIO_ID_GPU;
    m.offered = VIRTIO_BIT(VIRTIO_F_VERSION_1) | VIRTIO_BIT(VIRTIO_F_INDIRECT_DESC) |
                VIRTIO_BIT(VIRTIO_F_EVENT_IDX) | VIRTIO_BIT(VIRTIO_F_RING_PACKED) | 0x23u;
    m.qmax[0] = 256;
    m.qmax[1] = 256;
    udelays = 0;
    return &m.dev;
}

/* What the model's device did with a buffer: the segments it read and the
 * bytes it wrote. */
static uint32_t seen_out, seen_in, seen_indirect, seen_badflags;

/** Read descriptor `i` of the table at device address `table`. */
static struct virtq_desc model_desc(uint64_t table, uint32_t i)
{
    struct virtq_desc dsc;
    uint8_t *p = dev_ptr(table + 16u * i, 16);

    k_memset(&dsc, 0, sizeof(dsc));
    if (p) {
        k_memcpy(&dsc, p, sizeof(dsc));
    }
    return dsc;
}

/**
 * Walk the chain at `head` of a table of `n` at `table`, write `fill` into
 * each device-writable buffer and return the bytes written. A readable
 * segment after a writable one, a chain longer than its table, or a link
 * out of the table is counted in `seen_badflags`.
 */
static uint32_t model_walk(uint64_t table, uint32_t n, uint32_t head, uint8_t fill)
{
    uint32_t i = head, steps = 0, written = 0;
    int writing = 0;

    for (;;) {
        struct virtq_desc dsc = model_desc(table, i);
        uint8_t *buf;

        if (dsc.flags & VIRTQ_DESC_F_WRITE) {
            writing = 1;
            buf = dev_ptr(dsc.addr, dsc.len);
            if (buf) {
                k_memset(buf, fill, dsc.len);
            }
            written += dsc.len;
            seen_in++;
        } else {
            if (writing) {
                seen_badflags++;
            }
            seen_out++;
        }
        if (!(dsc.flags & VIRTQ_DESC_F_NEXT)) {
            break;
        }
        i = dsc.next;
        if (i >= n || ++steps > n) {
            seen_badflags++;
            break;
        }
    }
    return written;
}

/**
 * Take up to `max` buffers from queue `qi`'s driver ring, process each and
 * put it on the used ring with the bytes written. Returns the count taken.
 */
static int model_run(int qi, int max, uint8_t fill)
{
    struct model_queue *q = &m.q[qi];
    int n = 0;

    while (n < max) {
        uint16_t *avail = (uint16_t *)(void *)dev_ptr(q->avail, 4u + 2u * q->size);
        uint8_t *used = dev_ptr(q->used, 4u + 8u * q->size);
        uint16_t head;
        uint32_t len, slot;
        struct virtq_desc dsc;

        if (!avail || !used || avail[1] == q->last_avail) {
            break;
        }
        head = avail[2 + (q->last_avail & (q->size - 1u))];
        q->last_avail++;
        dsc = model_desc(q->desc, head);
        if (dsc.flags & VIRTQ_DESC_F_INDIRECT) {
            seen_indirect++;
            if (dsc.flags & VIRTQ_DESC_F_NEXT) {
                seen_badflags++;
            }
            len = model_walk(dsc.addr, dsc.len / 16u, 0, fill);
        } else {
            len = model_walk(q->desc, q->size, head, fill);
        }
        slot = q->used_idx & (q->size - 1u);
        k_memcpy(used + 4u + 8u * slot, &(uint32_t){head}, 4);
        k_memcpy(used + 8u + 8u * slot, &len, 4);
        q->used_idx++;
        k_memcpy(used + 2u, &q->used_idx, 2);
        n++;
    }
    return n;
}

/* The completions `done` has seen, in order. */
#define DONE_MAX 512
static void *done_token[DONE_MAX];
static uint32_t done_len[DONE_MAX];
static int ndone;
static struct virtqueue *readd_vq; /* a queue `done` adds a buffer to */
static struct virtio_sg readd_sg;
static int readd_result = 1;

/** Record a completion; add a buffer to `readd_vq` when a row asks. */
static void done(struct virtqueue *vq, void *token, uint32_t len)
{
    (void)vq;
    if (ndone < DONE_MAX) {
        done_token[ndone] = token;
        done_len[ndone] = len;
    }
    ndone++;
    if (readd_vq) {
        struct virtqueue *q = readd_vq;

        readd_vq = 0;
        readd_result = virtqueue_add(q, &readd_sg, 1, 0, (void *)0x5eed);
    }
}

/** Buffers for the rows: one page of the arena, cut into 64-byte pieces. */
static uint64_t data_addr;
static uint8_t *data;

/** Piece `i` of the data page as a segment of `len` bytes. */
static struct virtio_sg piece(int i, uint32_t len)
{
    struct virtio_sg sg;

    sg.addr = data_addr + 64u * (uint32_t)i;
    sg.len = len;
    return sg;
}

/** The status sequence and features of a bring-up. */
static void negotiation(void)
{
    struct virtio_dev *d = model_reset();
    int r;

    m.reset_polls = 5;
    r = virtio_init(d, 0x21u | VIRTIO_BIT(VIRTIO_F_EVENT_IDX) | VIRTIO_BIT(40), 2);
    check(r == 0, "init succeeds");
    check(udelays == 5, "the reset waits as long as the status takes to clear");
    check(m.nstatus == 4 && m.status_log[0] == 0 && m.status_log[1] == VIRTIO_STATUS_ACKNOWLEDGE &&
              m.status_log[2] == (VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER) &&
              m.status_log[3] ==
                  (VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER | VIRTIO_STATUS_FEATURES_OK),
          "status goes reset, ACKNOWLEDGE, DRIVER, FEATURES_OK in order");
    check(m.driver == (VIRTIO_BIT(VIRTIO_F_VERSION_1) | VIRTIO_BIT(VIRTIO_F_INDIRECT_DESC) | 0x21u),
          "the device's wanted bits, VERSION_1 and INDIRECT_DESC are taken, the ring bits "
          "not handled are not");
    check(d->features == m.driver, "features records what was written");
    check(virtio_has(d, VIRTIO_F_VERSION_1) && virtio_has(d, 0) && !virtio_has(d, 1) &&
              !virtio_has(d, VIRTIO_F_EVENT_IDX) && !virtio_has(d, 99),
          "virtio_has answers each bit");
    check(m.irq_setups == 1 && m.irq_nqueues == 2 && d->nqueues == 2,
          "interrupts are prepared for the queues");
    virtio_ready(d);
    check(m.irq_starts == 1 && m.started_before_ok && (m.status & VIRTIO_STATUS_DRIVER_OK) &&
              (m.status & VIRTIO_STATUS_FEATURES_OK),
          "ready starts the interrupts, then sets DRIVER_OK");
}

/** The refusals of a bring-up. */
static void refusals(void)
{
    struct virtio_dev *d = model_reset();

    m.offered &= ~VIRTIO_BIT(VIRTIO_F_VERSION_1);
    check(virtio_init(d, 0, 1) == -ENODEV && (m.status & VIRTIO_STATUS_FAILED),
          "a legacy device is refused and marked FAILED");
    check(m.irq_setups == 0, "a refused device has no interrupts prepared");

    d = model_reset();
    m.refuse = 1;
    check(virtio_init(d, 1, 1) == -EIO && (m.status & VIRTIO_STATUS_FAILED) && d->features == 0,
          "features the device refuses fail the bring-up");

    d = model_reset();
    m.reset_polls = -1;
    check(virtio_init(d, 0, 1) == -ETIMEDOUT && udelays == 1000,
          "a reset that never ends gives up after 100 ms");

    d = model_reset();
    check(virtio_init(d, 0, VIRTIO_MAX_QUEUES + 1) == -EINVAL && m.nstatus == 0,
          "too many queues are refused before the device is touched");
}

/** Queue sizes, layout and the refusals of a queue's setup. */
static void queue_setup(void)
{
    struct virtio_dev *d = model_reset();
    struct virtqueue *vq = 0, *vq2 = 0, *spare = 0;
    int pages;

    m.qmax[1] = 100;
    m.qmax[2] = 0;
    check(virtio_init(d, 0, 3) == 0, "init for the queue rows");
    check(virtio_queue_setup(d, 0, done, 0, &vq) == 0 && vq && vq->size == VIRTQ_MAX_SIZE,
          "a queue of 256 is made with VIRTQ_MAX_SIZE");
    check(vq->ring_bytes <= 4096u && vq->avail_addr == vq->desc_addr + 16u * vq->size &&
              vq->used_addr % 4u == 0 && vq->used_addr >= vq->avail_addr + 6u + 2u * vq->size &&
              vq->used_addr + 6u + 8u * vq->size <= vq->desc_addr + vq->ring_bytes,
          "descriptors, the driver's ring and the device's ring lie in one page in order");
    check(m.q[0].enabled && m.q[0].size == VIRTQ_MAX_SIZE && m.q[0].desc == vq->desc_addr &&
              m.q[0].avail == vq->avail_addr && m.q[0].used == vq->used_addr,
          "the transport is given the size and the device addresses");
    check(vq->vector == 3 && vq->indirect != 0, "the vector is the transport's, and indirect "
                                                "tables are made when negotiated");
    check(virtio_queue_setup(d, 1, done, 0, &vq2) == 0 && vq2->size == 64,
          "a queue of 100 is made with 64, a power of two");
    check(virtio_queue_setup(d, 0, done, 0, &spare) == -EBUSY, "a queue is set up once");
    check(virtio_queue_setup(d, 2, done, 0, &spare) == -ENOENT, "a queue of size 0 is absent");
    check(virtio_queue_setup(d, 3, done, 0, &spare) == -ENOENT,
          "a queue past the count given to init is absent");
    check(virtio_init(d, 0, 1) == -EBUSY, "init refuses a device with queues still set up");
    pages = m.dma_out;
    check(pages == 4, "each queue holds its ring and its indirect page");
    virtio_teardown(d);
    check(m.dma_out == 0 && m.irq_releases == 1 && !d->queues[0] && !d->queues[1],
          "teardown after a reset frees every page and stops the interrupts");
    check(virtio_init(d, 0, 1) == 0 && virtio_queue_setup(d, 0, done, 0, &vq) == 0,
          "a device torn down comes up again");

    d = model_reset();
    m.offered &= ~VIRTIO_BIT(VIRTIO_F_INDIRECT_DESC);
    check(virtio_init(d, 0, 1) == 0 && virtio_queue_setup(d, 0, done, 0, &vq) == 0 &&
              vq->indirect == 0 && m.dma_out == 1,
          "no indirect tables without the feature");
    virtio_teardown(d);

    d = model_reset();
    check(virtio_init(d, 0, 1) == 0, "init for the allocation row");
    m.dma_fail = 1;
    check(virtio_queue_setup(d, 0, done, 0, &vq) == -ENOMEM && m.dma_out == 0 && !d->queues[0],
          "a queue whose ring cannot be had is refused with nothing kept");

    d = model_reset();
    check(virtio_init(d, 0, 1) == 0 && virtio_queue_setup(d, 0, done, 0, &vq) == 0,
          "a queue for the stuck reset");
    m.reset_polls = -1;
    virtio_teardown(d);
    check(m.dma_out == 2 && !d->queues[0],
          "teardown after a reset that did not end keeps the pages the device may write");
}

/** Fill a queue, drain it, and wrap its 16-bit indices. */
static void fill_wrap_drain(void)
{
    struct virtio_dev *d = model_reset();
    struct virtqueue *vq = 0;
    struct virtio_sg sg[3];
    int i, round, ok = 1, order = 1;
    uint32_t total = 0;

    m.qmax[0] = 8;
    m.offered &= ~VIRTIO_BIT(VIRTIO_F_INDIRECT_DESC);
    check(virtio_init(d, 0, 1) == 0 && virtio_queue_setup(d, 0, done, 0, &vq) == 0, "a queue of 8");
    data = (uint8_t *)virtio_dma_alloc(d, 4096, &data_addr);
    check(data != 0 && data_addr >= DEV_BASE, "data memory comes through the table");
    virtio_ready(d);
    ndone = 0;
    for (i = 0; i < 8; i++) {
        sg[0] = piece(i, 16);
        ok &= virtqueue_add(vq, sg, 1, 0, (void *)(uintptr_t)(0x100 + i)) == 0;
    }
    check(ok && virtqueue_free_count(vq) == 0, "eight buffers fill a queue of 8");
    sg[0] = piece(9, 16);
    check(virtqueue_add(vq, sg, 1, 0, (void *)0x1) == -ENOSPC, "a full queue refuses another");
    check(virtqueue_add(vq, sg, 0, 0, (void *)0x1) == -EINVAL &&
              virtqueue_add(vq, sg, 1, 0, 0) == -EINVAL,
          "no segments or no token is refused");
    virtqueue_kick(vq);
    check(m.q[0].notified == 1, "a kick notifies");
    check(virtqueue_reap(vq) == 0 && ndone == 0, "nothing is reaped before the device returns");
    check(model_run(0, 8, 0) == 8 && seen_badflags == 0, "the device takes all eight");
    check(virtqueue_reap(vq) == 8 && ndone == 8 && virtqueue_free_count(vq) == 8,
          "a reap takes all eight and frees their descriptors");
    for (i = 0; i < 8; i++) {
        order &= done_token[i] == (void *)(uintptr_t)(0x100 + i) && done_len[i] == 0;
    }
    check(order, "completions come in the device's order with their tokens");

    /* Chains of one to three segments, a device-written last segment in
     * each, round after round until the 16-bit indices have wrapped. */
    ok = 1;
    order = 1;
    for (round = 0; round < 30000 && ok; round++) {
        int n = 1 + round % 3, k, added = 0, start;
        uint32_t want;

        ndone = 0;
        start = round;
        while (virtqueue_free_count(vq) >= (uint16_t)n) {
            for (k = 0; k < n; k++) {
                sg[k] = piece(k, k == n - 1 ? 24u : 8u);
            }
            ok &= virtqueue_add(vq, sg, (unsigned)(n - 1), 1,
                                (void *)(uintptr_t)(1 + start * 16 + added)) == 0;
            added++;
        }
        virtqueue_kick(vq);
        ok &= model_run(0, 64, (uint8_t)round) == added;
        ok &= virtqueue_reap(vq) == added && ndone == added;
        ok &= virtqueue_free_count(vq) == 8;
        want = 24;
        for (k = 0; k < added && k < DONE_MAX; k++) {
            order &=
                done_token[k] == (void *)(uintptr_t)(1 + start * 16 + k) && done_len[k] == want;
        }
        ok &= data[64u * (uint32_t)(n - 1)] == (uint8_t)round;
        total += (uint32_t)added;
    }
    check(ok, "every round fills, drains and frees the queue");
    check(order, "every round's completions carry their tokens and the bytes written");
    check(total > 65536u && vq->avail_idx == (uint16_t)(total + 8u) &&
              vq->last_used == vq->avail_idx,
          "the indices wrapped past 65535 and agree");
    check(seen_badflags == 0, "the device saw readable segments before writable ones");
    check(m.q[0].notified == 1u + 30000u, "each kick notified once");
    virtio_dma_free(d, data, 4096);
    virtio_teardown(d);
}

/** Indirect tables: a chain longer than the free descriptors. */
static void indirect(void)
{
    struct virtio_dev *d = model_reset();
    struct virtqueue *vq = 0;
    struct virtio_sg sg[VIRTQ_INDIRECT_LEN + 1];
    int k;

    m.qmax[0] = 4;
    check(virtio_init(d, 0, 1) == 0 && virtio_queue_setup(d, 0, done, 0, &vq) == 0 &&
              vq->size == 4 && vq->indirect,
          "a queue of 4 with indirect tables");
    data = (uint8_t *)virtio_dma_alloc(d, 4096, &data_addr);
    for (k = 0; k <= VIRTQ_INDIRECT_LEN; k++) {
        sg[k] = piece(k, 32);
    }
    seen_indirect = 0;
    seen_out = seen_in = seen_badflags = 0;
    ndone = 0;
    check(virtqueue_add(vq, sg, 4, 2, (void *)0x600) == 0 && virtqueue_free_count(vq) == 3,
          "six segments go into one table and take one descriptor");
    check(vq->indirect_free == 0xFFFEu, "the first table is taken");
    check(virtqueue_add(vq, sg, 3, 0, (void *)0x300) == 0 && virtqueue_free_count(vq) == 0,
          "three segments fit the three free descriptors directly");
    check(virtqueue_add(vq, sg, 1, 0, (void *)0x1) == -ENOSPC,
          "a full queue refuses even an indirect buffer");
    virtqueue_kick(vq);
    check(model_run(0, 4, 0xAB) == 2 && seen_indirect == 1 && seen_out == 7 && seen_in == 2 &&
              seen_badflags == 0,
          "the device walks the table and the direct chain");
    check(virtqueue_reap(vq) == 2 && done_token[0] == (void *)0x600 && done_len[0] == 64 &&
              done_token[1] == (void *)0x300 && virtqueue_free_count(vq) == 4 &&
              vq->indirect_free == 0xFFFFu,
          "the table and the descriptors come back");
    check(data[64u * 4u] == 0xAB && data[64u * 5u + 31u] == 0xAB && data[64u * 3u] != 0xAB,
          "the device wrote only the writable segments");
    check(virtqueue_add(vq, sg, VIRTQ_INDIRECT_LEN + 1, 0, (void *)0x1) == -ENOSPC,
          "a chain longer than a table and the queue is refused");
    check(virtqueue_add(vq, sg, VIRTQ_INDIRECT_LEN, 0, (void *)0x1600) == 0 &&
              virtqueue_free_count(vq) == 3,
          "a chain as long as a table goes into one");
    virtqueue_kick(vq);
    check(model_run(0, 4, 0) == 1 && virtqueue_reap(vq) == 1 && virtqueue_free_count(vq) == 4,
          "it comes back whole");
    virtio_dma_free(d, data, 4096);
    virtio_teardown(d);
}

/** The device's flags, a bad device and a done that adds a buffer. */
static void device_faults(void)
{
    struct virtio_dev *d = model_reset();
    struct virtqueue *vq = 0;
    struct virtio_sg sg[1];
    uint8_t *used;
    uint32_t bad;

    m.qmax[0] = 8;
    check(virtio_init(d, 0, 1) == 0 && virtio_queue_setup(d, 0, done, 0, &vq) == 0,
          "a queue for the fault rows");
    data = (uint8_t *)virtio_dma_alloc(d, 4096, &data_addr);
    sg[0] = piece(0, 8);
    check(virtqueue_add(vq, sg, 1, 0, (void *)0x10) == 0, "a buffer");
    used = dev_ptr(m.q[0].used, 4u + 8u * 8u);
    used[0] = VIRTQ_USED_F_NO_NOTIFY;
    virtqueue_kick(vq);
    check(m.q[0].notified == 0, "a device that asked not to be notified is not");
    used[0] = 0;

    /* The device returns a head that is not in use, then the right one. */
    ndone = 0;
    bad = 5;
    k_memcpy(used + 4u, &bad, 4);
    m.q[0].used_idx = 1;
    k_memcpy(used + 2u, &m.q[0].used_idx, 2);
    logs = 0;
    check(virtqueue_reap(vq) == 0 && ndone == 0 && logs == 1 && strstr(last_log, "head 5"),
          "a head not in use is logged and passed over");
    check(model_run(0, 8, 0) == 1 && virtqueue_reap(vq) == 1 && done_token[0] == (void *)0x10,
          "the next element completes the buffer");

    /* An index that runs further ahead than the queue is long. */
    m.q[0].used_idx = (uint16_t)(m.q[0].used_idx + 9u);
    k_memcpy(used + 2u, &m.q[0].used_idx, 2);
    logs = 0;
    check(virtqueue_reap(vq) == 0 && logs == 1 && strstr(last_log, "ran 9 past"),
          "an index ahead by more than the queue stops the reap");
    virtio_teardown(d);

    /* done adds to its own queue: the lock is not held around it. */
    d = model_reset();
    m.qmax[0] = 8;
    check(virtio_init(d, 0, 1) == 0 && virtio_queue_setup(d, 0, done, 0, &vq) == 0,
          "a queue for the reentry row");
    data = (uint8_t *)virtio_dma_alloc(d, 4096, &data_addr);
    sg[0] = piece(0, 8);
    readd_sg = piece(1, 8);
    readd_result = 1;
    check(virtqueue_add(vq, sg, 1, 0, (void *)0x11) == 0, "a buffer for the reentry row");
    model_run(0, 8, 0);
    readd_vq = vq;
    ndone = 0;
    check(virtqueue_reap(vq) == 1 && readd_result == 0 && virtqueue_free_count(vq) == 7,
          "done adds a buffer to its own queue");
    model_run(0, 8, 0);
    check(virtqueue_reap(vq) == 1 && done_token[1] == (void *)0x5eed,
          "the buffer done added completes");
    virtio_teardown(d);
}

/** Configuration reads, interrupts and the pass-through calls. */
static int config_calls;

/** Count a configuration change. */
static void config_changed(struct virtio_dev *d)
{
    (void)d;
    config_calls++;
}

/** The configuration, the interrupt entries and the table's other calls. */
static void config_and_interrupts(void)
{
    struct virtio_dev *d = model_reset();
    struct virtqueue *q0 = 0, *q1 = 0;
    struct virtio_sg sg[1];
    uint8_t buf[4];
    uint64_t addr = 0, len = 0;

    m.config[0] = 7;
    m.config[1] = 8;
    m.flips = 2;
    virtio_config_read(d, 0, buf, 2);
    check(buf[0] == 9 && buf[1] == 8 && m.flips == 0,
          "a read repeats while the generation changes, and returns the last snapshot");
    m.flips = -1;
    virtio_config_read(d, 0, buf, 1);
    check(m.flips == -1, "a generation that never settles ends the read after eight tries");
    m.flips = 0;
    buf[0] = 0x5A;
    virtio_config_write(d, 3, buf, 1);
    check(m.config[3] == 0x5A, "a write reaches the configuration");

    check(virtio_init(d, 0, 2) == 0 && virtio_queue_setup(d, 0, done, 0, &q0) == 0 &&
              virtio_queue_setup(d, 1, done, 0, &q1) == 0,
          "two queues for the interrupt rows");
    data = (uint8_t *)virtio_dma_alloc(d, 4096, &data_addr);
    sg[0] = piece(0, 8);
    virtqueue_add(q0, sg, 1, 0, (void *)0x20);
    virtqueue_add(q1, sg, 1, 0, (void *)0x21);
    model_run(0, 1, 0);
    model_run(1, 1, 0);
    ndone = 0;
    virtio_interrupt_queue(q1);
    check(ndone == 1 && done_token[0] == (void *)0x21, "a queue's interrupt reaps that queue");
    virtio_interrupt_queues(d);
    check(ndone == 2 && done_token[1] == (void *)0x20, "a shared interrupt reaps every queue");
    virtio_interrupt_queue(0);
    d->config_changed = config_changed;
    virtio_interrupt_config(d);
    check(config_calls == 1 && d->config_events == 1, "a configuration interrupt reaches the "
                                                      "driver");
    check(virtio_shm_region(d, 1, &addr, &len) == 0 && addr == 0x380000000000ull &&
              len == 0x100000000ull && virtio_shm_region(d, 2, &addr, &len) == -ENOENT,
          "shared memory regions come from the transport");
    virtio_dma_free(d, data, 4096);
    virtio_dma_free(d, 0, 4096);
    virtio_teardown(d);
    check(m.dma_out == 0, "every page given is back");
}

int main(void)
{
    negotiation();
    refusals();
    queue_setup();
    fill_wrap_drain();
    indirect();
    device_faults();
    config_and_interrupts();
    if (!failures) {
        printf("virtqueue-hosttest: result: pass: negotiation, refusals, queue setup, fill, "
               "wrap and drain, indirect tables, device faults, configuration and "
               "interrupts\n");
    }
    return failures != 0;
}
