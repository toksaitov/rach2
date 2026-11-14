/*
 * virtio_blk_hosttest.c: the virtio block driver of
 * rach2/sys/block/virtio_blk.c on the virtio core, against a model device.
 *
 * host-deps: rach2/sys/virtio/virtqueue.c
 * host-deps: rach2/sys/lib/kstring.c rach2/sys/lib/kctypes.c
 * host-deps: tests/host/compat/kheap_host.c tests/host/compat/spinlock_host.c
 * host-cflags: -DRACH2_HOST_CPU
 *
 * QEMU's virtio-blk answers every request, offers what its options give and
 * keeps 512-byte blocks. A boot cannot make on demand a device that refuses
 * a feature, has blocks of 4096 bytes, answers a command with an error,
 * never answers one, changes its capacity or goes away. The test includes
 * virtio_blk.c and links the real core, virtqueue.c; the model fills the
 * core's table of operations as a transport does, and walks each chain the
 * core published, direct or through an indirect table, reading the request
 * header, moving the data to and from its disk in memory and writing the
 * status byte, as the device does. A device address of the model's own
 * memory is the host address; one of a buffer is its page spread out
 * (`paging_virt_to_phys`).
 *
 * The model answers a chain when it is told of it, and raises the queue's
 * interrupt only when a row says so (`model_irq`), since on the machine the
 * handler runs after the notify has returned and the driver's lock is free.
 * A row checks what the driver returned, what the model saw, what it holds,
 * and the log where the log is the only account a failure gives.
 */

#include <stdarg.h>

extern int printf(const char *fmt, ...);
extern char *strstr(const char *haystack, const char *needle);
extern int vsnprintf(char *out, unsigned long size, const char *fmt, va_list ap);
extern int posix_memalign(void **out, unsigned long align, unsigned long size);
extern void free(void *p);

#include "../../rach2/sys/block/virtio_blk.c"

unsigned int host_cpu;

static int failures;

/** Record a failed row. */
static void check(int ok, const char *what)
{
    if (!ok) {
        printf("virtio-blk-hosttest: fail: %s\n", what);
        failures++;
    }
}

/*
 * The kernel around the driver.
 */

static char logbuf[32768];
static unsigned long logused;
static int irq_on = 1;
static uint32_t delays;
static int cmdline_poll, policy_ro;

/** Append the record to the test's log. */
void klog(int level, const char *subsystem, const char *format, ...)
{
    va_list ap;
    int n;

    (void)level;
    if (logused + 512 > sizeof(logbuf)) {
        logused = 0;
    }
    for (n = 0; subsystem[n] && n < 32; n++) {
        logbuf[logused++] = subsystem[n];
    }
    logbuf[logused++] = ':';
    logbuf[logused++] = ' ';
    va_start(ap, format);
    n = vsnprintf(logbuf + logused, 440, format, ap);
    va_end(ap);
    logused += (unsigned long)(n > 0 ? (n < 440 ? n : 439) : 0);
    logbuf[logused++] = '\n';
    logbuf[logused] = 0;
}

/** Whether the strings `a` and `b` are the same. */
static int same(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

/** Whether a record since the last `log_clear` holds `needle`. */
static int logged(const char *needle)
{
    return strstr(logbuf, needle) != 0;
}

/** Forget the records logged so far. */
static void log_clear(void)
{
    logused = 0;
    logbuf[0] = 0;
}

void k_udelay(uint32_t us)
{
    (void)us;
    delays++;
}

void arch_dma_fence(void)
{}

uint32_t kirq_save(void)
{
    uint32_t was = (uint32_t)irq_on;

    irq_on = 0;
    return was;
}

void kirq_restore(uint32_t flags)
{
    irq_on = (int)flags;
}

int kirq_enabled(void)
{
    return irq_on;
}

void smp_service(void)
{}

int kcmdline_has(const char *flag)
{
    return cmdline_poll && strstr(flag, "vblkpoll") != 0;
}

void latency_span(int kind, uint32_t from, uint32_t to)
{
    (void)kind;
    (void)from;
    (void)to;
}

uint32_t timer_now_us(void)
{
    return 0;
}

/* A buffer's frames as the model reads them: each page of the host's
 * twice as far up, tagged, so that no two pages of a buffer follow each
 * other as they do in the host's memory, as the frames of a buffer rarely
 * do on the machine. The model's own pages keep their host address. */
#define FRAME_TAG (1ull << 62)

uintptr_t paging_virt_to_phys(uintptr_t va)
{
    return (uintptr_t)(FRAME_TAG | (((uint64_t)va & ~0xFFFull) << 1) | ((uint64_t)va & 0xFFFu));
}

static int commands, flush_counts, polls;

void block_count_command(void)
{
    commands++;
}

void block_count_flush(void)
{
    flush_counts++;
}

void block_count_poll(void)
{
    polls++;
}

static int completions;

/** Answer request `r`: its status, once, for the generation it was given. */
void block_request_complete(struct block_request *request, uint32_t generation, int status)
{
    check(generation == request->generation, "a completion names the request's generation");
    check(request->state == 0, "a request is completed once");
    request->status = status;
    request->state = 1;
    completions++;
}

int block_device_holds(block_device_t *device)
{
    (void)device;
    return 0;
}

int vfs_device_holds(block_device_t *dev)
{
    (void)dev;
    return 0;
}

static int gone_views;

void loop_device_gone(block_device_t *device)
{
    (void)device;
}

void bcache_device_gone(block_device_t *dev)
{
    (void)dev;
    gone_views++;
}

block_device_t *block0;

static struct disk disks[VBLK_DISKS];
static int registered, released;

/** Register the disk, as the registry does, with the serial of words 10
 * to 19, first character in each word's high byte, its spaces dropped. */
struct disk *disk_register(block_device_t *dev, const char *transport, int location,
                           const volatile uint16_t *identify)
{
    struct disk *d = &disks[location];
    int i, n = 0;

    k_memset(d, 0, sizeof(*d));
    d->present = 1;
    d->number = location;
    d->dev = dev;
    d->transport = transport;
    d->location = location;
    for (i = 0; i < 20; i++) {
        char c = (char)(i & 1 ? identify[10 + i / 2] & 0xFF : identify[10 + i / 2] >> 8);

        if (c != ' ') {
            d->serial[n++] = c;
        }
    }
    d->serial[n] = 0;
    check(identify[27] == 0x2020 && identify[46] == 0x2020, "the model words are spaces");
    registered++;
    return d;
}

void disk_release(struct disk *d)
{
    d->released = 1;
    released++;
}

int disk_scan(struct disk *d, block_device_t *backing)
{
    (void)d;
    (void)backing;
    return 0;
}

const char diskcmd_policy_banner[] = "the test's read-only policy";

/** The test's policy: everything, or under `policy_ro` the reads and
 * IDENTIFY alone. */
int diskcmd_allowed(uint8_t command)
{
    return !policy_ro || command == 0x25 || command == 0xEC;
}

/*
 * The model device.
 */

#define DISK_SECTORS 8192u
#define MAX_DEVS     2

struct model {
    struct virtio_dev dev;
    uint8_t status;
    uint64_t offered, taken;
    uint8_t cfg[VBLK_CFG_BYTES];
    uint16_t qsize;
    struct virtqueue *vq[VIRTIO_MAX_QUEUES];
    uint16_t last_avail[VIRTIO_MAX_QUEUES];
    int irq_pending[VIRTIO_MAX_QUEUES];
    int notifies[VIRTIO_MAX_QUEUES];
    int polled;      /* the transport gives no interrupt */
    int stuck_reset; /* a write of 0 to the status does not reset it */
    int dma_on;
    int hold;   /* tell of no chain: leave each unanswered */
    int answer; /* the status byte the next request of `fail_type` gets */
    int fail_type;
    int resets, puts;
    char serial[VBLK_ID_BYTES + 1];
    uint8_t *disk;
    int reads, writes, flushes, discards, zeroes, ids, longest;
    int dirty;           /* writes answered since the last flush */
    int flushed_writes;  /* writes a flush made durable */
    uint64_t last_range; /* the last discard's or write of zeroes' sector */
    uint32_t last_num;   /* and its sectors */
};

static struct model models[MAX_DEVS];
static int ndevs, pages_held;
static virtio_probe_t probe_fn;

/** The model of device `d`. */
static struct model *mod(struct virtio_dev *d)
{
    return (struct model *)d->transport;
}

/** The device's status byte. */
static uint8_t m_get_status(struct virtio_dev *d)
{
    return mod(d)->status;
}

/** Store the status byte; 0 resets the device, unless its reset is stuck,
 * and forgets the chains it has taken. */
static void m_set_status(struct virtio_dev *d, uint8_t status)
{
    struct model *m = mod(d);

    if (status == 0) {
        m->resets++;
        if (m->stuck_reset) {
            return;
        }
        k_memset(m->last_avail, 0, sizeof(m->last_avail));
        k_memset(m->irq_pending, 0, sizeof(m->irq_pending));
    }
    m->status = status;
}

/** The features the device offers. */
static uint64_t m_get_features(struct virtio_dev *d)
{
    return mod(d)->offered;
}

/** Record the features the driver took. */
static void m_set_features(struct virtio_dev *d, uint64_t features)
{
    mod(d)->taken = features;
}

/** The configuration's generation, which never changes under a read. */
static uint32_t m_generation(struct virtio_dev *d)
{
    (void)d;
    return 0;
}

/** Read the device's configuration. */
static void m_config_read(struct virtio_dev *d, uint32_t offset, void *buf, uint32_t len)
{
    k_memcpy(buf, mod(d)->cfg + offset, len);
}

/** Write the device's configuration. */
static void m_config_write(struct virtio_dev *d, uint32_t offset, const void *buf, uint32_t len)
{
    k_memcpy(mod(d)->cfg + offset, buf, len);
}

/** The entries each queue may have. */
static uint16_t m_queue_max(struct virtio_dev *d, uint16_t index)
{
    (void)index;
    return mod(d)->qsize;
}

/** Take queue `vq`, from its first chain. */
static int m_queue_enable(struct virtio_dev *d, struct virtqueue *vq)
{
    struct model *m = mod(d);

    m->vq[vq->index] = vq;
    m->last_avail[vq->index] = 0;
    return 0;
}

static void model_service(struct model *m, int qi);

/** The driver tells the device of queue `vq`, which it answers at once. */
static void m_notify(struct virtio_dev *d, struct virtqueue *vq)
{
    struct model *m = mod(d);

    m->notifies[vq->index]++;
    model_service(m, vq->index);
}

/** Give the device its interrupts, or none on a polling transport. */
static int m_irq_setup(struct virtio_dev *d, uint16_t nqueues)
{
    (void)nqueues;
    d->polled = mod(d)->polled;
    return 0;
}

/** Let the interrupts through. */
static void m_irq_start(struct virtio_dev *d)
{
    (void)d;
}

/** Give the interrupts back. */
static void m_irq_release(struct virtio_dev *d)
{
    (void)d;
}

/** A zeroed page of the device's memory, counted while held. */
static void *m_dma_alloc(struct virtio_dev *d, uint32_t bytes, uint64_t *addr)
{
    void *p = 0;

    (void)d;
    if (bytes == 0 || bytes > 4096 || posix_memalign(&p, 4096, 4096) != 0) {
        return 0;
    }
    k_memset(p, 0, 4096);
    *addr = (uint64_t)(uintptr_t)p;
    pages_held++;
    return p;
}

/** Give a page back, which a device still running must not reach. */
static void m_dma_free(struct virtio_dev *d, void *p, uint32_t bytes)
{
    struct model *m = mod(d);

    (void)bytes;
    if (!p) {
        return;
    }
    check(m->status == 0 || !m->dma_on, "memory is freed only once the device stopped");
    free(p);
    pages_held--;
}

/** Let the device reach memory, or stop it. */
static void m_dma_enable(struct virtio_dev *d, int on)
{
    mod(d)->dma_on = on;
}

/** The driver gives the device back. */
static void m_put(struct virtio_dev *d)
{
    mod(d)->puts++;
}

static const struct virtio_ops ops = {
    .name = "model",
    .get_status = m_get_status,
    .set_status = m_set_status,
    .get_features = m_get_features,
    .set_features = m_set_features,
    .config_generation = m_generation,
    .config_read = m_config_read,
    .config_write = m_config_write,
    .queue_max = m_queue_max,
    .queue_enable = m_queue_enable,
    .notify = m_notify,
    .irq_setup = m_irq_setup,
    .irq_start = m_irq_start,
    .irq_release = m_irq_release,
    .dma_alloc = m_dma_alloc,
    .dma_free = m_dma_free,
    .put = m_put,
    .dma_enable = m_dma_enable,
};

/** The host byte a device address names. */
static uint8_t *at(uint64_t addr)
{
    if (addr & FRAME_TAG) {
        addr = ((addr & ~FRAME_TAG & ~0xFFFull) >> 1) | (addr & 0xFFFu);
    }
    return (uint8_t *)(uintptr_t)addr;
}

/** A little-endian number of `n` bytes at `p`. */
static uint64_t le(const uint8_t *p, int n)
{
    uint64_t v = 0;

    while (n-- > 0) {
        v = (v << 8) | p[n];
    }
    return v;
}

/** Store `v` at `p` in `n` little-endian bytes. */
static void put_le(uint8_t *p, uint64_t v, int n)
{
    int i;

    for (i = 0; i < n; i++) {
        p[i] = (uint8_t)(v >> (8 * i));
    }
}

struct seg {
    uint8_t *p;
    uint32_t len;
    int write;
};

#define SEGS_MAX 300

/** The segments of the chain at `head` of queue `vq`, following an
 * indirect table where the chain has one; their count. */
static int model_chain(struct virtqueue *vq, uint16_t head, struct seg *s)
{
    volatile struct virtq_desc *t = vq->desc;
    uint32_t i = head, n = 0, tlen = vq->size;

    if (t[i].flags & VIRTQ_DESC_F_INDIRECT) {
        tlen = t[i].len / 16u;
        t = (volatile struct virtq_desc *)at(t[i].addr);
        i = 0;
        check(tlen >= 1 && tlen <= VIRTQ_INDIRECT_MAX, "an indirect table fits its limit");
    }
    for (;;) {
        check(n < SEGS_MAX && i < tlen, "a chain stays inside its table");
        if (n >= SEGS_MAX || i >= tlen) {
            return 0;
        }
        s[n].p = at(t[i].addr);
        s[n].len = t[i].len;
        s[n].write = (t[i].flags & VIRTQ_DESC_F_WRITE) != 0;
        n++;
        if (!(t[i].flags & VIRTQ_DESC_F_NEXT)) {
            return (int)n;
        }
        i = t[i].next;
    }
}

/** Move the data of a read or a write between the disk and segments 1 to
 * `n` - 2, from `sector`; the bytes the device wrote. */
static uint32_t model_data(struct model *m, struct seg *s, int n, uint64_t sector, int write)
{
    uint64_t off = sector * 512u;
    uint32_t moved = 0;
    int k;

    for (k = 1; k < n - 1; k++) {
        check(s[k].write == !write, "a read's buffers are written, a write's read");
        check(off + s[k].len <= (uint64_t)DISK_SECTORS * 512u, "a transfer stays on the disk");
        if (off + s[k].len > (uint64_t)DISK_SECTORS * 512u) {
            return moved;
        }
        if (write) {
            k_memcpy(m->disk + off, s[k].p, s[k].len);
        } else {
            k_memcpy(s[k].p, m->disk + off, s[k].len);
            moved += s[k].len;
        }
        off += s[k].len;
    }
    return moved;
}

/** Do the request whose header is `h` with data `s` 1 to `n` - 2, and
 * give its status; `*wrote` the bytes written into its buffers. */
static int model_request(struct model *m, struct seg *s, int n, uint32_t *wrote)
{
    uint32_t type = (uint32_t)le(s[0].p, 4);
    uint64_t sector = le(s[0].p + 8, 8);

    if (n - 2 > m->longest) {
        m->longest = n - 2;
    }
    if ((int)type == m->fail_type) {
        m->fail_type = -1;
        return m->answer;
    }
    switch (type) {
    case VBLK_T_IN:
        m->reads++;
        *wrote += model_data(m, s, n, sector, 0);
        return VBLK_S_OK;
    case VBLK_T_OUT:
        m->writes++;
        m->dirty++;
        (void)model_data(m, s, n, sector, 1);
        return VBLK_S_OK;
    case VBLK_T_FLUSH:
        m->flushes++;
        m->flushed_writes += m->dirty;
        m->dirty = 0;
        return VBLK_S_OK;
    case VBLK_T_GET_ID:
        m->ids++;
        check(n == 3 && s[1].len == VBLK_ID_BYTES && s[1].write, "GET_ID has 20 bytes to write");
        k_memset(s[1].p, 0, VBLK_ID_BYTES);
        k_memcpy(s[1].p, m->serial, k_strlen(m->serial));
        *wrote += VBLK_ID_BYTES;
        return m->serial[0] ? VBLK_S_OK : VBLK_S_UNSUPP;
    case VBLK_T_DISCARD:
    case VBLK_T_WRITE_ZEROES:
        check(n == 3 && s[1].len == 16 && !s[1].write, "a range is one segment of 16 bytes");
        m->last_range = le(s[1].p, 8);
        m->last_num = (uint32_t)le(s[1].p + 8, 4);
        if (m->last_range + m->last_num <= DISK_SECTORS) {
            k_memset(m->disk + m->last_range * 512u, type == VBLK_T_DISCARD ? 0xDD : 0,
                     m->last_num * 512u);
        }
        if (type == VBLK_T_DISCARD) {
            m->discards++;
        } else {
            m->zeroes++;
        }
        return VBLK_S_OK;
    default:
        return VBLK_S_UNSUPP;
    }
}

/** Answer every chain published on queue `qi` that the model has not
 * taken, unless it holds them, and mark the queue's interrupt due. */
static void model_service(struct model *m, int qi)
{
    static struct seg s[SEGS_MAX];
    struct virtqueue *vq = m->vq[qi];

    while (vq && !m->hold && m->last_avail[qi] != vq->avail[1]) {
        uint16_t head = vq->avail[2 + m->last_avail[qi] % vq->size];
        int n = model_chain(vq, head, s);
        uint32_t wrote = 1;
        uint16_t u = vq->used[1];

        check(n >= 2 && !s[0].write && s[0].len == 16 && s[n - 1].write && s[n - 1].len == 1,
              "a chain is a header, its data and a status byte");
        if (n >= 2) {
            *s[n - 1].p = (uint8_t)model_request(m, s, n, &wrote);
        }
        vq->used_ring[u % vq->size].id = head;
        vq->used_ring[u % vq->size].len = wrote;
        vq->used[1] = (uint16_t)(u + 1);
        m->last_avail[qi]++;
        m->irq_pending[qi] = 1;
    }
    if (vq && vq->event) {
        /* avail_event, after the used ring's elements, as QEMU leaves it */
        *(volatile uint16_t *)((volatile uint8_t *)vq->used + 4 + 8u * vq->size) =
            m->last_avail[qi];
    }
}

/** Raise the interrupt of every queue of model `m` that has answered. */
static void model_irq(struct model *m)
{
    int i;

    for (i = 0; i < VIRTIO_MAX_QUEUES; i++) {
        if (m->irq_pending[i] && m->vq[i]) {
            m->irq_pending[i] = 0;
            virtio_interrupt_queue(m->vq[i]);
        }
    }
}

/** Raise model `m`'s interrupts until none is due, as the handler runs
 * again for answers given while it ran. */
static void model_drain(struct model *m)
{
    int i, round;

    for (round = 0; round < 64; round++) {
        int due = 0;

        for (i = 0; i < VIRTIO_MAX_QUEUES; i++) {
            due |= m->irq_pending[i];
        }
        if (!due) {
            return;
        }
        model_irq(m);
    }
    check(0, "the interrupts end");
}

/** Answer what model `m` held and raise its interrupts. */
static void model_release(struct model *m)
{
    int i;

    m->hold = 0;
    for (i = 0; i < VIRTIO_MAX_QUEUES; i++) {
        model_service(m, i);
    }
    model_irq(m);
}

int virtio_pci_find(uint16_t id, int nth, struct virtio_dev **out)
{
    if (id != VIRTIO_ID_BLOCK || nth < 0 || nth >= ndevs) {
        return -ENODEV;
    }
    *out = &models[nth].dev;
    return 0;
}

static int binds, bounds, unbinds;

int virtio_driver_bind(struct virtio_dev *d, const char *driver)
{
    (void)d;
    check(same(driver, "virtio_blk"), "the driver binds as virtio_blk");
    binds++;
    return 0;
}

void virtio_driver_bound(struct virtio_dev *d)
{
    (void)d;
    bounds++;
}

void virtio_driver_unbind(struct virtio_dev *d)
{
    (void)d;
    unbinds++;
}

int virtio_driver_register(uint16_t id, virtio_probe_t probe)
{
    check(id == VIRTIO_ID_BLOCK, "the driver registers for block devices");
    probe_fn = probe;
    return 0;
}

#define F(bit) VIRTIO_BIT(bit)

/** The features QEMU's virtio-blk-pci offers by default, with
 * `num-queues=4`. */
static uint64_t qemu_features(void)
{
    return F(VIRTIO_F_VERSION_1) | F(VIRTIO_F_INDIRECT_DESC) | F(VIRTIO_F_EVENT_IDX) |
           F(VBLK_F_SEG_MAX) | F(VBLK_F_BLK_SIZE) | F(VBLK_F_FLUSH) | F(VBLK_F_CONFIG_WCE) |
           F(VBLK_F_MQ) | F(VBLK_F_DISCARD) | F(VBLK_F_WRITE_ZEROES);
}

/** Make `n` model devices as QEMU gives them, and the driver's state as at
 * boot. */
static void model_reset(int n)
{
    int i;

    for (i = 0; i < VBLK_DISKS; i++) {
        if (vblks[i].dev && vblks[i].up) {
            virtio_teardown(vblks[i].dev);
            vblk_free_pages(&vblks[i]);
        }
    }
    for (i = 0; i < MAX_DEVS; i++) {
        uint8_t *disk = models[i].disk;
        struct model *m = &models[i];

        k_memset(m, 0, sizeof(*m));
        m->disk = disk;
        if (!m->disk) {
            void *p = 0;

            (void)posix_memalign(&p, 4096, DISK_SECTORS * 512u);
            m->disk = (uint8_t *)p;
        }
        k_memset(m->disk, 0, DISK_SECTORS * 512u);
        m->dev.ops = &ops;
        m->dev.transport = m;
        m->dev.id = VIRTIO_ID_BLOCK;
        m->dev.name = i == 0 ? "virtio0" : "virtio1";
        m->offered = qemu_features();
        m->qsize = 128;
        m->fail_type = -1;
        m->dma_on = 1;
        put_le(m->cfg + VBLK_CFG_CAPACITY, DISK_SECTORS, 8);
        put_le(m->cfg + VBLK_CFG_SEG_MAX, 126, 4);
        put_le(m->cfg + VBLK_CFG_BLK_SIZE, 512, 4);
        m->cfg[VBLK_CFG_WRITEBACK] = 1;
        put_le(m->cfg + VBLK_CFG_NUM_QUEUES, 4, 2);
        put_le(m->cfg + VBLK_CFG_MAX_DISCARD, 64, 4);
        put_le(m->cfg + VBLK_CFG_MAX_ZEROES, 32, 4);
        k_memcpy(m->serial, i == 0 ? "rach2vblk" : "second", i == 0 ? 9 : 6);
    }
    k_memset(vblks, 0, sizeof(vblks));
    k_memset(disks, 0, sizeof(disks));
    ndevs = n;
    block0 = 0;
    vblk_registered = 0;
    probe_fn = 0;
    irq_on = 1;
    cmdline_poll = 0;
    policy_ro = 0;
    host_cpu = 0;
    log_clear();
}

/** A request of `count` sectors at `lba`, through `buf`, with `flags`. */
static void request_init(struct block_request *r, int write, uint64_t lba, uint32_t count,
                         uint8_t *buf, uint32_t flags)
{
    static uint32_t generation = 1;

    k_memset(r, 0, sizeof(*r));
    r->write = write;
    r->lba = lba;
    r->count = count;
    r->page = buf;
    r->flags = flags;
    r->generation = generation++;
}

/** The disk of model 0's device, as the driver published it. */
static block_device_t *disk0(void)
{
    return &vblks[0].bdev;
}

/*
 * Rows.
 */

/** QEMU's device: the features taken, a queue for each of four processors,
 * the tables, the limits and the serial, polled at first. */
static void bring_up(void)
{
    struct model *m = &models[0];
    struct vblk *vb = &vblks[0];
    int i;

    model_reset(1);
    check(virtio_blk_init() == 0, "QEMU's device is published");
    check(m->taken == qemu_features(), "every feature offered that the driver knows is taken");
    check(vb->nqueues == 4 && m->vq[3] && !m->vq[4], "a queue for each of the four offered");
    check(m->vq[0]->ind_len == VIRTQ_INDIRECT_MAX && m->vq[0]->ind_tables == VBLK_TABLES &&
              m->vq[0]->ind_pages == VBLK_TABLES && m->vq[0]->max_chain == VIRTQ_INDIRECT_MAX,
          "each queue has 8 tables of 256 descriptors, a page each");
    check(vb->seg_max == 126 && vb->seg_bytes == 4096, "seg_max cuts the pieces of a chain");
    check(disk0()->max_sectors == 126 * 8, "a request is the pages 126 pieces cover");
    check(disk0()->sector_count == DISK_SECTORS, "the capacity is the configuration's");
    check(vb->cache && (disk0()->flags & BLOCK_WRITE_THROUGH), "the write cache is on");
    check((disk0()->flags & (BLOCK_FAULT_SAFE | BLOCK_POLLABLE)) ==
                  (BLOCK_FAULT_SAFE | BLOCK_POLLABLE) &&
              !disk0()->submit,
          "the disk is polled until it is started");
    check(registered == 1 && disks[0].present && same(disks[0].transport, "virtio"),
          "the disk is registered as transport virtio");
    check(same(disks[0].serial, "rach2vblk") && m->ids == 1, "GET_ID gives the serial");
    check(block0 == disk0() && binds == 1 && bounds == 1 && probe_fn != 0,
          "the first disk is block0, the device bound, the driver registered");
    for (i = 0; i < 4; i++) {
        check(m->vq[i]->event, "each queue has the event index");
    }
    check(logged("4 queues, 126 pieces of 4096 bytes, 1008 sectors a request, cache on, "
                 "discard, write zeroes"),
          "the bring-up is logged");
}

/** Polled reads and writes: data across pages and through a buffer that
 * is not aligned, each write flushed. */
static void polled_rw(void)
{
    static uint8_t buf[3 * 4096 + 512];
    struct model *m = &models[0];
    uint8_t *odd = buf + 100;
    int i, ok = 1;

    for (i = 0; i < 2 * 4096; i++) {
        odd[i] = (uint8_t)(i * 13 + 7);
    }
    check(disk0()->write(disk0(), 10, 16, odd) == 0, "a polled write of 16 sectors");
    check(m->writes == 1 && m->flushes == 1 && m->dirty == 0, "a polled write is flushed");
    for (i = 0; i < 2 * 4096; i++) {
        ok &= m->disk[10 * 512 + i] == (uint8_t)(i * 13 + 7);
    }
    check(ok, "the write reached the disk");
    check(m->longest == 3, "an unaligned buffer of 8 KiB is three pieces");
    k_memset(buf, 0, sizeof(buf));
    check(disk0()->read(disk0(), 10, 16, odd) == 0, "a polled read");
    for (i = 0, ok = 1; i < 2 * 4096; i++) {
        ok &= odd[i] == (uint8_t)(i * 13 + 7);
    }
    check(ok, "the read gave the data back");
    check(disk0()->read(disk0(), DISK_SECTORS - 1, 2, buf) == -EIO, "a read past the end");
}

/** A device without a cache, one that is read-only, one of 4096-byte
 * blocks, one with a single queue, and one whose transport polls. */
static void refusals(void)
{
    static uint8_t buf[512];
    struct model *m = &models[0];
    int before;

    model_reset(1);
    m->offered &= ~F(VBLK_F_FLUSH);
    check(virtio_blk_init() == 0 && !vblks[0].cache && !(disk0()->flags & BLOCK_WRITE_THROUGH),
          "a device without FLUSH has no cache");
    check(disk0()->write(disk0(), 0, 1, buf) == 0 && m->flushes == 0, "and no flush follows");
    model_reset(1);
    m->cfg[VBLK_CFG_WRITEBACK] = 0;
    check(virtio_blk_init() == 0 && !vblks[0].cache, "CONFIG_WCE with writethrough is no cache");
    model_reset(1);
    m->offered |= F(VBLK_F_RO);
    check(virtio_blk_init() == 0 && (disk0()->flags & BLOCK_READ_ONLY), "RO is read-only");
    check(disk0()->write(disk0(), 0, 1, buf) == -EIO && m->writes == 0,
          "a write to a read-only disk fails before the device");
    model_reset(1);
    put_le(m->cfg + VBLK_CFG_BLK_SIZE, 4096, 4);
    before = registered;
    check(virtio_blk_init() == -1 && registered == before, "4096-byte blocks are refused");
    check(logged("refused: blocks of 4096 bytes") && logged("unusable: a block size"),
          "the refusal is logged");
    check(m->status == 0 && pages_held == 0, "a refused device is reset and holds nothing");
    model_reset(1);
    m->offered &= ~F(VBLK_F_MQ);
    check(virtio_blk_init() == 0 && vblks[0].nqueues == 1, "without MQ one queue");
    model_reset(1);
    m->offered &= ~F(VIRTIO_F_INDIRECT_DESC);
    check(virtio_blk_init() == 0 && vblks[0].seg_max == 126 && m->vq[0]->max_chain == 128,
          "without indirect tables a chain is the queue's length");
    model_reset(1);
    m->qsize = 16;
    m->offered &= ~F(VIRTIO_F_INDIRECT_DESC);
    check(virtio_blk_init() == 0 && vblks[0].seg_max == 14 && disk0()->max_sectors == 112,
          "a queue of 16 cuts a request to 14 pages");
    model_reset(1);
    m->polled = 1;
    check(virtio_blk_init() == 0, "a device of a polling transport is published");
    virtio_blk_start();
    check(!disk0()->submit && logged("polled: the transport gives it no interrupt"),
          "and stays polled");
    model_reset(1);
    cmdline_poll = 1;
    check(virtio_blk_init() == 0, "a device is published under rach2.vblkpoll");
    virtio_blk_start();
    check(!disk0()->submit && logged("polled: rach2.vblkpoll"), "and stays polled");
}

/** The interrupt-driven path: queued writes, NOFLUSH ones marking the
 * disk, a durable one answered after its flush, and a flush clearing the
 * mark. */
static void queued(void)
{
    static uint8_t buf[4096];
    static struct block_request r[3];
    struct model *m = &models[0];

    model_reset(1);
    (void)virtio_blk_init();
    virtio_blk_start();
    check(disk0()->submit && disk0()->flush && !(disk0()->flags & BLOCK_FAULT_SAFE) &&
              (disk0()->flags & BLOCK_FRAMES),
          "start gives the disk submit and flush and BLOCK_FRAMES");
    check(disk0()->max_discard == 64 && disk0()->max_zeroes == 32,
          "the discard and zeroes limits are the configuration's");
    host_cpu = 2;
    request_init(&r[0], 1, 100, 8, buf, BLOCK_REQ_NOFLUSH);
    check(disk0()->submit(disk0(), &r[0]) == 0, "a write is queued");
    check(m->notifies[2] == 1 && m->writes == 1 && r[0].state == 0,
          "processor 2's queue tells the device, which answers");
    model_irq(m);
    check(r[0].state == 1 && r[0].status == 0 && disk0()->unflushed && m->flushes == 0,
          "a NOFLUSH write is answered unflushed and marks the disk");
    request_init(&r[1], 1, 108, 8, buf, 0);
    check(disk0()->submit(disk0(), &r[1]) == 0, "a durable write is queued");
    model_irq(m);
    check(r[1].state == 0 && m->flushes == 1,
          "a durable write's answer issues its flush, and the device is told of it");
    model_irq(m);
    check(r[1].state == 1 && r[1].status == 0 && m->flushed_writes == 2 && !disk0()->unflushed,
          "the durable write is answered once its flush is, which clears the mark");
    request_init(&r[2], 0, 0, 0, 0, BLOCK_REQ_FLUSH);
    check(disk0()->flush(disk0(), &r[2]) == 0, "a flush request is queued");
    model_irq(m);
    check(r[2].state == 1 && r[2].status == 0 && m->flushes == 2, "and answered");
    host_cpu = 0;
}

/** More requests than a queue's slots: those that wait are offered as
 * slots come back, oldest first. */
static void full_queue(void)
{
    static uint8_t buf[24][512];
    static struct block_request r[24];
    struct model *m = &models[0];
    int i, done = 1;

    model_reset(1);
    (void)virtio_blk_init();
    virtio_blk_start();
    m->hold = 1;
    for (i = 0; i < 24; i++) {
        buf[i][0] = (uint8_t)i;
        request_init(&r[i], 1, (uint64_t)i, 1, buf[i], BLOCK_REQ_NOFLUSH);
        check(disk0()->submit(disk0(), &r[i]) == 0, "a request is queued");
    }
    check(vblks[0].q[0].head == &r[VBLK_SLOTS] && m->vq[0]->nfree < m->vq[0]->size,
          "16 requests are in slots and 8 wait");
    model_release(m);
    model_drain(m);
    for (i = 0; i < 24; i++) {
        done &= r[i].state == 1 && r[i].status == 0 && m->disk[i * 512] == (uint8_t)i;
    }
    check(done && m->writes == 24, "every request is answered in turn");
}

/** A discard and a write of zeroes, each one range, and a range longer
 * than the device takes split by the block layer's limit. */
static void discard(void)
{
    static struct block_request r[2];
    struct model *m = &models[0];

    model_reset(1);
    (void)virtio_blk_init();
    virtio_blk_start();
    k_memset(m->disk, 0x55, 64 * 512);
    request_init(&r[0], 1, 8, 16, 0, BLOCK_REQ_DISCARD);
    check(disk0()->submit(disk0(), &r[0]) == 0, "a discard is queued");
    model_irq(m);
    check(r[0].state == 1 && r[0].status == 0 && m->discards == 1 && m->last_range == 8 &&
              m->last_num == 16,
          "a discard carries its range");
    request_init(&r[1], 1, 40, 8, 0, BLOCK_REQ_ZEROES);
    check(disk0()->submit(disk0(), &r[1]) == 0, "a write of zeroes is queued");
    model_irq(m);
    check(r[1].state == 1 && m->zeroes == 1 && m->disk[40 * 512] == 0 && m->disk[48 * 512] == 0x55,
          "a write of zeroes zeroes its range alone");
    check(m->flushes == 0, "neither is flushed");
}

/** An error and an unsupported request, each answered with its errno. */
static void errors(void)
{
    static uint8_t buf[512];
    static struct block_request r[2];
    struct model *m = &models[0];

    model_reset(1);
    (void)virtio_blk_init();
    virtio_blk_start();
    m->fail_type = VBLK_T_IN;
    m->answer = VBLK_S_IOERR;
    request_init(&r[0], 0, 0, 1, buf, 0);
    (void)disk0()->submit(disk0(), &r[0]);
    model_irq(m);
    check(r[0].state == 1 && r[0].status == -EIO, "IOERR is EIO");
    m->fail_type = VBLK_T_DISCARD;
    m->answer = VBLK_S_UNSUPP;
    request_init(&r[1], 1, 0, 1, 0, BLOCK_REQ_DISCARD);
    (void)disk0()->submit(disk0(), &r[1]);
    model_irq(m);
    check(r[1].state == 1 && r[1].status == -EOPNOTSUPP, "UNSUPP is EOPNOTSUPP");
    check(logged("request type 0 failed: status 1"), "the failure is logged");
    check(!vblks[0].dead, "a failed request leaves the disk in use");
    policy_ro = 1;
    check(disk0()->write(disk0(), 0, 1, buf) == -EIO && logged("refused: request type 1"),
          "the read-only policy refuses a write before the device");
    check(disk0()->read(disk0(), 0, 1, buf) == 0, "and lets a read through");
}

/** A polled command the device never answers: the device is reset, every
 * request it holds fails, and every later one. */
static void unanswered(void)
{
    static uint8_t buf[512];
    static struct block_request r;
    struct model *m = &models[0];

    model_reset(1);
    (void)virtio_blk_init();
    virtio_blk_start();
    m->hold = 1;
    request_init(&r, 0, 0, 1, buf, 0);
    (void)disk0()->submit(disk0(), &r);
    irq_on = 0;
    delays = 0;
    check(disk0()->read(disk0(), 0, 1, buf) == -EIO, "an unanswered polled read fails");
    irq_on = 1;
    check(delays >= VBLK_CMD_STEPS_MASKED, "it waited five seconds with interrupts off");
    check(vblks[0].dead && m->resets >= 2 && m->status == 0, "the device is reset");
    check(r.state == 1 && r.status == -EIO, "the request it held fails");
    check(logged("no answer in five seconds with interrupts off") &&
              logged("out of use: a command was not answered"),
          "the log says why");
    request_init(&r, 0, 0, 1, buf, 0);
    check(disk0()->submit(disk0(), &r) == -EIO, "a later request fails");
    check(disk0()->read(disk0(), 0, 1, buf) == -EIO, "and a later polled read");
    model_reset(1);
    (void)virtio_blk_init();
    m->hold = 1;
    m->stuck_reset = 1;
    check(disk0()->read(disk0(), 0, 1, buf) == -EIO && !m->dma_on,
          "a device whose reset does not end loses its DMA");
    m->stuck_reset = 0;
    m->status = 0;
}

/** A configuration change, read at the next submit, and the removal of
 * the device, which fails what it held and gives everything back. */
static void change_and_removal(void)
{
    static uint8_t buf[512];
    static struct block_request r[2];
    struct model *m = &models[0];

    model_reset(1);
    (void)virtio_blk_init();
    virtio_blk_start();
    put_le(m->cfg + VBLK_CFG_CAPACITY, 2 * DISK_SECTORS, 8);
    virtio_interrupt_config(&m->dev);
    check(disk0()->sector_count == DISK_SECTORS, "a change is not read in the interrupt");
    request_init(&r[0], 0, 0, 1, buf, 0);
    (void)disk0()->submit(disk0(), &r[0]);
    model_irq(m);
    check(disk0()->sector_count == 2 * DISK_SECTORS && logged("capacity: 8 MiB, was 4 MiB"),
          "the next submit reads the new capacity");
    put_le(m->cfg + VBLK_CFG_CAPACITY, DISK_SECTORS, 8);
    m->hold = 1;
    request_init(&r[1], 0, 0, 1, buf, 0);
    (void)disk0()->submit(disk0(), &r[1]);
    m->dev.removed(&m->dev);
    check(r[1].state == 1 && r[1].status == -EIO, "removal fails the request in flight");
    check(m->status == 0 && pages_held == 0 && unbinds >= 1 && m->puts == 1,
          "the device is reset, its pages freed, unbound and given back");
    check(disks[0].released && gone_views >= 1 && logged("disk0: released"),
          "the disk's views are gone and its number released");
    check(m->dev.priv == 0 && m->dev.removed == 0, "the device names the driver no more");
}

/** Two devices: the second is disk 1, a later bind through the
 * registered probe is published too, and the queue count is the lesser of
 * the offer and VIRTIO_MAX_QUEUES. */
static void two_devices(void)
{
    model_reset(2);
    put_le(models[1].cfg + VBLK_CFG_NUM_QUEUES, 16, 2);
    check(virtio_blk_init() == 0 && registered >= 2 && disks[1].present, "two disks");
    check(vblks[1].nqueues == VIRTIO_MAX_QUEUES, "16 queues offered, 8 taken");
    check(same(disks[1].serial, "second") && block0 == disk0(),
          "the second has its own serial, and the first stays block0");
    check(virtio_blk_init() == 0 && models[0].resets == 1, "a second init resets nothing");
}

int main(void)
{
    bring_up();
    polled_rw();
    refusals();
    queued();
    full_queue();
    discard();
    errors();
    unanswered();
    change_and_removal();
    two_devices();
    model_reset(0);
    if (failures) {
        printf("virtio-blk-hosttest: %d failures\n", failures);
        return 1;
    }
    printf("virtio-blk-hosttest: ok\n");
    return 0;
}
