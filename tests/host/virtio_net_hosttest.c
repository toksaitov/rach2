/*
 * virtio_net_hosttest.c: the virtio network driver,
 * rach2/sys/network/driver/virtio_net.c, over the virtio core against a
 * model of the device reached only through the transport's table.
 *
 * host-deps: rach2/sys/virtio/virtqueue.c
 * host-deps: rach2/sys/lib/kstring.c rach2/sys/lib/kctypes.c
 * host-deps: tests/host/compat/kheap_host.c tests/host/compat/spinlock_host.c
 *
 * The model is a virtio network device as virtio 1.2, 5.1 describes one: a
 * status byte, the feature bits a row offers, the configuration with the
 * address, the status word, the MTU, the speed and the duplex, and three
 * queues it reads only through the addresses the driver gave it. It fills
 * receive buffers with a header and a frame, in one buffer or across
 * several; takes transmit buffers and keeps what they held; answers the
 * control queue's commands as it is told of them; and raises an interrupt
 * only where the driver's ring asks for one, which the row then delivers
 * as the transport does. Its configuration interrupt, which a row may have
 * it raise in the middle of a read of the status word, is delivered at
 * once, or at the end of the `net_lock` bracket it falls in, as a
 * processor with interrupts off takes it. The stack is a list of the
 * interfaces listed, the probe's first, with the registration, withdrawal
 * and naming the driver calls. The rows check what the stack was given,
 * what the model saw, the notifications and interrupts counted, and the
 * memory the model still holds. The driver is included whole, so that a row
 * starts with every record of it free.
 */

#include <stdarg.h>

extern int printf(const char *format, ...);
extern int vsnprintf(char *out, unsigned long size, const char *fmt, va_list ap);
extern char *strstr(const char *haystack, const char *needle);
extern int memcmp(const void *a, const void *b, unsigned long n);
extern int strcmp(const char *a, const char *b);

#include "network/driver/virtio_net.c"
#include "network/ethernet/ethernet.h"
#include "network/net.h"
#include "virtio/virtio.h"
#include "fs/synthfs.h"
#include "fs/sysfs.h"
#include "klog.h"
#include "udelay.h"
#include "lib/kerrno.h"
#include "lib/kirq.h"
#include "lib/krandom.h"
#include "lib/kstring.h"
#include "mm/kheap.h"

/* The model's memory: page i at device address DEV_BASE + i * 4096. */
#define ARENA_PAGES 128
#define DEV_BASE    0x40000000ull
#define NQ          3

static int failures;
static char logged[4096];

/** Record a failed invariant. */
static void check(int ok, const char *what)
{
    if (!ok) {
        printf("virtio-net-hosttest: fail: %s\n", what);
        failures++;
    }
}

/** Keep every record logged, one after another. */
void klog(int level, const char *subsystem, const char *format, ...)
{
    unsigned long used = (unsigned long)k_strlen(logged);
    va_list ap;

    (void)level;
    (void)subsystem;
    if (used + 200u < sizeof(logged)) {
        va_start(ap, format);
        vsnprintf(logged + used, sizeof(logged) - used - 2u, format, ap);
        va_end(ap);
        used = (unsigned long)k_strlen(logged);
        logged[used] = '\n';
        logged[used + 1u] = 0;
    }
}

/** Nothing to order on one host thread. */
void arch_dma_fence(void)
{}

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

static void config_irq_deliver(void);

/** The network's bracket, counted, so a row sees that a send took it, and
 * its depth, inside which the model's interrupts wait. */
static int net_locks, net_depth, config_irq_waits;
uint32_t net_lock(void)
{
    net_locks++;
    net_depth++;
    return 0;
}

/** The bracket's end: an interrupt that waited is taken as the outermost
 * bracket ends. */
void net_unlock(uint32_t flags)
{
    (void)flags;
    if (--net_depth == 0 && config_irq_waits) {
        config_irq_waits = 0;
        config_irq_deliver();
    }
}

/** A fixed pattern, so a row knows the address made up. */
void krandom_bytes(void *buf, uint32_t n)
{
    k_memset(buf, 0xA5, n);
}

/* The model device. */
struct mq {
    int enabled;
    uint16_t size;
    uint64_t desc, avail, used;
    uint16_t last_avail, used_idx;
    uint32_t notified, raised;
    int ask; /* ask to be told of the next buffer, as QEMU does when it has
              * none left */
};

struct ctrl_rec {
    uint8_t class, cmd, data[8];
    uint32_t len;
};

static struct model {
    struct virtio_dev dev;
    int present; /* the transport has the function */
    uint8_t status;
    uint64_t offered, driver;
    uint8_t config[32];
    uint16_t qmax[NQ];
    struct mq q[NQ];
    int polled;              /* the transport has no interrupt for it */
    int inits;               /* irq_setup calls, one a negotiation */
    int dma_left;            /* pages it still gives; -1 for any */
    int dma_out;             /* pages given and not freed */
    int puts;                /* virtio_put calls */
    int ctrl_mute;           /* leave commands unanswered */
    uint8_t ctrl_ack;        /* the answer it writes */
    struct ctrl_rec ctrl[8]; /* the commands, in order */
    int nctrl;
    int tx_per_delay;  /* transmit buffers it takes at each k_udelay */
    const char *bound; /* the driver shown bound, or NULL */
    int bound_sent, unbinds;
    int up_at_read;   /* the link comes up in the middle of the next read of
                       * the status word, which still reads the old one */
    int status_reads; /* reads of the status word */
    int limit_seen;   /* the receive queue's first notification came */
    uint32_t limit;   /* the interface's largest MTU and its MTU then */
    uint32_t mtu;
    int sys_dir; /* its directory on the virtio bus, or 0 */
} m;

static uint8_t arena[ARENA_PAGES][4096] __attribute__((aligned(4096)));
static uint8_t arena_taken[ARENA_PAGES];
static pci_device_t model_pci;
static struct netdev nd; /* the interface, defined with the stack below */
static struct netdev *listed[8];
static int nlisted;
static virtio_probe_t registered;
static uint32_t udelays;

static int dev_tx(uint8_t *out, uint32_t *len);

/** Count a wait, during which the model sends what a row lets it. */
void k_udelay(uint32_t usec)
{
    int i;

    (void)usec;
    udelays++;
    for (i = 0; i < m.tx_per_delay; i++) {
        (void)dev_tx(0, 0);
    }
}

/** The host bytes at device address `addr`, `len` of them within one
 * page the model gave; a row fails and NULL comes back for anything else. */
static uint8_t *dev_ptr(uint64_t addr, uint32_t len)
{
    uint64_t off = addr - DEV_BASE;
    uint64_t page = off / 4096u;

    if (addr < DEV_BASE || page >= ARENA_PAGES || !arena_taken[page] || off % 4096u + len > 4096u) {
        check(0, "the driver gave the device an address it was not given");
        return 0;
    }
    return &arena[page][off % 4096u];
}

/** The status byte. */
static uint8_t m_get_status(struct virtio_dev *d)
{
    (void)d;
    return m.status;
}

/** Write the status byte; 0 resets the queues and the features. */
static void m_set_status(struct virtio_dev *d, uint8_t status)
{
    (void)d;
    if (status == 0) {
        m.driver = 0;
        k_memset(m.q, 0, sizeof(m.q));
    }
    m.status = status;
}

/** The offered bits. */
static uint64_t m_get_features(struct virtio_dev *d)
{
    (void)d;
    return m.offered;
}

/** Keep the accepted bits. */
static void m_set_features(struct virtio_dev *d, uint64_t features)
{
    (void)d;
    m.driver = features;
}

/** A configuration that does not change under a read. */
static uint32_t m_generation(struct virtio_dev *d)
{
    (void)d;
    return 0;
}

/** Take the model's configuration interrupt as the transport's handler
 * does. */
static void config_irq_deliver(void)
{
    virtio_interrupt_config(&m.dev);
}

/** Raise the configuration interrupt: taken at once, or at the end of the
 * `net_lock` bracket that has interrupts off. */
static void config_irq(void)
{
    if (net_depth > 0) {
        config_irq_waits = 1;
    } else {
        config_irq_deliver();
    }
}

/** Copy out of the configuration. A row may have the link come up just
 * after the status word was read, with the interrupt that reports it. */
static void m_config_read(struct virtio_dev *d, uint32_t offset, void *buf, uint32_t len)
{
    (void)d;
    k_memcpy(buf, m.config + offset, len);
    if (offset == VNET_CFG_STATUS) {
        m.status_reads++;
        if (m.up_at_read) {
            uint16_t up = VIRTIO_NET_S_LINK_UP;

            m.up_at_read = 0;
            k_memcpy(m.config + VNET_CFG_STATUS, &up, 2);
            config_irq();
        }
    }
}

/** Copy into the configuration. */
static void m_config_write(struct virtio_dev *d, uint32_t offset, const void *buf, uint32_t len)
{
    (void)d;
    k_memcpy(m.config + offset, buf, len);
}

/** The size the row gave queue `index`. */
static uint16_t m_queue_max(struct virtio_dev *d, uint16_t index)
{
    (void)d;
    return index < NQ ? m.qmax[index] : 0;
}

/** Take the queue's size and addresses. */
static int m_queue_enable(struct virtio_dev *d, struct virtqueue *vq)
{
    struct mq *q = &m.q[vq->index];

    (void)d;
    q->enabled = 1;
    q->size = vq->size;
    q->desc = vq->desc_addr;
    q->avail = vq->avail_addr;
    q->used = vq->used_addr;
    q->ask = 1;
    vq->vector = (uint16_t)(1u + vq->index);
    return 0;
}

static void dev_ctrl(void);

/** Count a notification, which must not come before DRIVER_OK; one of the
 * control queue is served at once, as QEMU serves it in the exit. */
static void m_notify(struct virtio_dev *d, struct virtqueue *vq)
{
    (void)d;
    check((m.status & VIRTIO_STATUS_DRIVER_OK) != 0, "no notification comes before DRIVER_OK");
    m.q[vq->index].notified++;
    if (vq->index == VNET_Q_RX && !m.limit_seen) {
        const struct netdev *n = nlisted > 0 ? listed[nlisted - 1] : &nd;

        m.limit_seen = 1;
        m.limit = n->mtu_max;
        m.mtu = n->mtu;
    }
    if (vq->index == VNET_Q_CTRL && !m.ctrl_mute) {
        dev_ctrl();
    }
}

/** Count a negotiation, and say whether the device interrupts. */
static int m_irq_setup(struct virtio_dev *d, uint16_t nqueues)
{
    check(nqueues == NQ, "interrupts are prepared for three queues");
    m.inits++;
    d->polled = m.polled;
    return 0;
}

/** Nothing held back. */
static void m_irq_start(struct virtio_dev *d)
{
    (void)d;
}

/** Nothing to give back. */
static void m_irq_release(struct virtio_dev *d)
{
    (void)d;
}

/** A page of the arena at its device address, while the row allows. */
static void *m_dma_alloc(struct virtio_dev *d, uint32_t bytes, uint64_t *addr)
{
    int i;

    (void)d;
    if (m.dma_left == 0 || bytes == 0 || bytes > 4096u) {
        return 0;
    }
    for (i = 0; i < ARENA_PAGES; i++) {
        if (!arena_taken[i]) {
            arena_taken[i] = 1;
            k_memset(arena[i], 0, 4096);
            *addr = DEV_BASE + (uint64_t)i * 4096u;
            m.dma_out++;
            if (m.dma_left > 0) {
                m.dma_left--;
            }
            return arena[i];
        }
    }
    return 0;
}

/** A ring of more than a page, as a legacy queue's, in consecutive pages
 * of the arena, counted as one allocation; one page from m_dma_alloc. */
static void *m_ring_alloc(struct virtio_dev *d, uint16_t index, uint32_t bytes, uint64_t *addr)
{
    int i, k, n = (int)((bytes + 4095u) / 4096u);

    (void)index;
    if (n <= 1) {
        return m_dma_alloc(d, bytes, addr);
    }
    for (i = 0; i + n <= ARENA_PAGES; i++) {
        for (k = 0; k < n && !arena_taken[i + k]; k++) {
        }
        if (k == n) {
            for (k = 0; k < n; k++) {
                arena_taken[i + k] = 1;
                k_memset(arena[i + k], 0, 4096);
            }
            *addr = DEV_BASE + (uint64_t)i * 4096u;
            m.dma_out++;
            return arena[i];
        }
    }
    return 0;
}

/** Give a page back, or the run m_ring_alloc gave for `bytes`, which only
 * a device that was reset allows. */
static void m_dma_free(struct virtio_dev *d, void *p, uint32_t bytes)
{
    long i = ((uint8_t *)p - &arena[0][0]) / 4096, k,
         n = bytes > 4096u ? (bytes + 4095u) / 4096u : 1;

    (void)d;
    check(i >= 0 && i + n <= ARENA_PAGES && arena_taken[i] && (uint8_t *)p == arena[i],
          "dma_free is given a page dma_alloc gave");
    check(m.status == 0, "a page is freed only once the device was reset");
    for (k = i; k >= 0 && k < i + n && k < ARENA_PAGES; k++) {
        arena_taken[k] = 0;
    }
    m.dma_out--;
}

/** No shared memory region. */
static int m_shm_region(struct virtio_dev *d, uint8_t id, uint64_t *addr, uint64_t *len)
{
    (void)d;
    (void)id;
    (void)addr;
    (void)len;
    return -ENOENT;
}

/** Count the driver's letting go. */
static void m_put(struct virtio_dev *d)
{
    (void)d;
    m.puts++;
}

static const struct virtio_ops model_ops = {
    "model",      m_get_status,  m_set_status,   m_get_features, m_set_features,
    m_generation, m_config_read, m_config_write, m_queue_max,    m_queue_enable,
    m_notify,     m_irq_setup,   m_irq_start,    m_irq_release,  m_dma_alloc,
    m_dma_free,   m_shm_region,  m_ring_alloc,   NULL,           NULL,
    NULL,         m_put,         NULL,           NULL,           NULL,
    NULL};

/* The transport and the bus, as virtio_pci.c and virtio_bus.c give them. */

/** The model is the one network function, while it is present. */
int virtio_pci_find(uint16_t id, int nth, struct virtio_dev **out)
{
    if (id != VIRTIO_ID_NET || nth != 0 || !m.present) {
        return -ENODEV;
    }
    *out = &m.dev;
    return 0;
}

/** The model's function, 00:03.0. */
const pci_device_t *virtio_pci_function(const struct virtio_dev *d)
{
    return d == &m.dev ? &model_pci : 0;
}

/** Keep the probe a later bind is given to. */
int virtio_driver_register(uint16_t id, virtio_probe_t probe)
{
    check(id == VIRTIO_ID_NET, "the driver registers for network devices");
    registered = probe;
    return 0;
}

/** Show the device bound. */
int virtio_driver_bind(struct virtio_dev *d, const char *driver)
{
    (void)d;
    if (m.bound) {
        return -EBUSY;
    }
    m.bound = driver;
    return 0;
}

/** The driver's probe has succeeded. */
void virtio_driver_bound(struct virtio_dev *d)
{
    (void)d;
    m.bound_sent++;
}

/** Take the binding away. */
void virtio_driver_unbind(struct virtio_dev *d)
{
    (void)d;
    m.bound = 0;
    m.unbinds++;
}

/** The device's directory on the virtio bus, as the row has it. */
int virtio_bus_dir(const struct virtio_dev *d)
{
    (void)d;
    return m.sys_dir;
}

/* /sys as the driver uses it: the anchor of an interface's directory, and
 * the one link the driver adds below it and removes. A row makes the anchor
 * (`sys_fail` 1) or the link (2) fail with -ENOMEM, as a full heap does. */
#define SYS_ANCHOR 500
#define SYS_LINK   700
static int sys_link_parent, sys_link_target, sys_links, sys_unlinks, sys_fail;
static char sys_link_name[16];

/** The anchor of /sys/class/net/IF, which names the interface's index. */
int sysfs_anchor(int node, int key)
{
    check(node == SYSFS_NET_IF, "the driver anchors only an interface's directory");
    return sys_fail == 1 ? -ENOMEM : SYS_ANCHOR + key;
}

/** Keep the link added. */
int sysfs_add_link(int parent, const char *name, int target)
{
    check(parent > 0, "a link is added only below an anchor that was made");
    if (sys_fail == 2) {
        return -ENOMEM;
    }
    sys_link_parent = parent;
    sys_link_target = target;
    k_strncpy(sys_link_name, name, sizeof(sys_link_name) - 1u);
    sys_links++;
    return SYS_LINK;
}

/** Count the link's removal; a handle of 0 names nothing. */
void sysfs_remove(int handle)
{
    if (handle != 0) {
        check(handle == SYS_LINK, "the driver removes the link it added");
        sys_unlinks++;
    }
}

/* The stack above the driver: the probe's interface `nd`, and the list of
 * those listed, which the driver adds to at a bind and takes from at an
 * unbind. `ifc` is the interface a row expects frames on. A row makes the
 * next registration fail with `reg_fail`. */
static struct netdev nd;
static struct netdev *ifc = &nd;
static int next_index, registers, unregisters, withdraws, reg_fail;
static struct netdev *withdrawn;

/** The listed interface named `name`, or NULL. */
static struct netdev *listed_named(const char *name)
{
    int i;

    for (i = 0; i < nlisted; i++) {
        if (strcmp(listed[i]->name, name) == 0) {
            return listed[i];
        }
    }
    return 0;
}

/** Name `n` "ethN" for the lowest N free, as netdev.c does. */
void netdev_name_eth(struct netdev *n)
{
    int i;

    for (i = 0; i < 10; i++) {
        n->name[0] = 'e';
        n->name[1] = 't';
        n->name[2] = 'h';
        n->name[3] = (char)('0' + i);
        n->name[4] = 0;
        if (!listed_named(n->name)) {
            return;
        }
    }
}

/** List `n` with the next index, or fail as the row asks. */
int netdev_register(struct netdev *n)
{
    check(n->ops && n->ops->tx && n->name[0] && !listed_named(n->name),
          "an interface registered has a table and a name no other has");
    if (reg_fail) {
        int rc = reg_fail;

        reg_fail = 0;
        return rc;
    }
    if (nlisted == 4) {
        return -ENOSPC;
    }
    n->index = next_index++;
    listed[nlisted++] = n;
    registers++;
    return 0;
}

/** Take `n` off the list. */
void netdev_unregister(struct netdev *n)
{
    int i, j;

    for (i = 0; i < nlisted; i++) {
        if (listed[i] == n) {
            for (j = i; j + 1 < nlisted; j++) {
                listed[j] = listed[j + 1];
            }
            nlisted--;
            unregisters++;
            return;
        }
    }
    check(0, "only a listed interface is unregistered");
}

/** Withdraw `n` from the stack, which must be listed. */
void netdev_withdraw(struct netdev *n)
{
    withdraws++;
    withdrawn = n;
    netdev_unregister(n);
}

static uint8_t got[8][VNET_FRAME_MAX];
static uint16_t got_len[8];
static int ngot, rx_errors, carrier_changes;
static int reply_from_rx; /* send a frame from inside ethernet_rx */

/** Keep each frame the driver delivers; a row may answer from here, as
 * TCP answers a segment from the handler. */
void ethernet_rx(struct netdev *from, const uint8_t *data, uint16_t len)
{
    static const uint8_t reply[20] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 2, 0, 0, 0, 0, 1, 8, 6};

    check(from == ifc, "a frame names the interface it came in on");
    if (ngot < 8 && len <= VNET_FRAME_MAX) {
        k_memcpy(got[ngot], data, len);
        got_len[ngot] = len;
    }
    ngot++;
    if (reply_from_rx) {
        check(ifc->ops->tx(ifc->dev, reply, sizeof(reply)) == NETDEV_TX_OK,
              "a send from inside a delivery is taken");
    }
}

/** Count a frame the driver refused. */
void ethernet_note_rx_error(struct netdev *from)
{
    (void)from;
    rx_errors++;
}

/** Count a carrier change. */
void netdev_carrier_changed(struct netdev *from)
{
    (void)from;
    carrier_changes++;
}

/* The device's side of a queue. */

/** The driver's ring of queue `q`: flags, idx, ring[size], used_event. */
static uint16_t *q_avail(struct mq *q)
{
    return (uint16_t *)(void *)dev_ptr(q->avail, 6u + 2u * q->size);
}

/** Whether EVENT_IDX was taken. */
static int event_idx(void)
{
    return (m.driver & VIRTIO_BIT(VIRTIO_F_EVENT_IDX)) != 0;
}

/**
 * Take the next buffer of queue `qi` and return its segments in order,
 * from the queue's table or an indirect one: 1 with `*head`, the count in
 * `*n` and each segment's address, length and whether the device writes
 * it; 0 when the driver has offered none. With EVENT_IDX and `ask` set it
 * asks to be told of the next buffer.
 */
static int q_pop(int qi, uint16_t *head, struct virtq_desc *seg, int *n)
{
    struct mq *q = &m.q[qi];
    uint16_t *avail = q->enabled ? q_avail(q) : 0;
    uint8_t *used;
    uint64_t table;
    uint32_t i, size;

    if (!avail || avail[1] == q->last_avail) {
        return 0;
    }
    *head = avail[2 + (q->last_avail & (q->size - 1u))];
    q->last_avail++;
    used = dev_ptr(q->used, 6u + 8u * q->size);
    if (event_idx() && q->ask && used) {
        k_memcpy(used + 4u + 8u * q->size, &q->last_avail, 2);
    }
    table = q->desc;
    size = q->size;
    i = *head;
    k_memcpy(&seg[0], dev_ptr(table + 16u * i, 16), 16);
    if (seg[0].flags & VIRTQ_DESC_F_INDIRECT) {
        table = seg[0].addr;
        size = seg[0].len / 16u;
        i = 0;
    }
    for (*n = 0; *n < 4; (*n)++) {
        k_memcpy(&seg[*n], dev_ptr(table + 16u * i, 16), 16);
        if (!(seg[*n].flags & VIRTQ_DESC_F_NEXT)) {
            (*n)++;
            break;
        }
        i = seg[*n].next;
        check(i < size, "a chain stays inside its table");
    }
    return 1;
}

/**
 * Return the buffer `head` on queue `qi` with `len` bytes written, and
 * count an interrupt where the driver's ring asks for one: with EVENT_IDX
 * when the return passes `used_event`, and otherwise unless the driver set
 * VIRTQ_AVAIL_F_NO_INTERRUPT.
 */
static void q_push(int qi, uint16_t head, uint32_t len)
{
    struct mq *q = &m.q[qi];
    uint16_t *avail = q_avail(q);
    uint8_t *used = dev_ptr(q->used, 6u + 8u * q->size);
    uint32_t slot = q->used_idx & (q->size - 1u), id = head;
    uint16_t old = q->used_idx;

    if (!avail || !used) {
        return;
    }
    k_memcpy(used + 4u + 8u * slot, &id, 4);
    k_memcpy(used + 8u + 8u * slot, &len, 4);
    q->used_idx++;
    k_memcpy(used + 2u, &q->used_idx, 2);
    if (event_idx()) {
        uint16_t event = avail[2u + q->size];

        if ((uint16_t)(q->used_idx - event - 1u) < (uint16_t)(q->used_idx - old)) {
            q->raised++;
        }
    } else if (!(avail[0] & VIRTQ_AVAIL_F_NO_INTERRUPT)) {
        q->raised++;
    }
}

/* The header the model's device puts before each frame: VNET_HDR, or
 * VNET_HDR_LEGACY for a legacy device without mergeable buffers. */
static uint32_t dev_hdr = VNET_HDR;

/**
 * Receive the frame of `len` bytes at `frame` across `nbufs` receive
 * buffers, the header's `num_buffers` being `stated`: the header and the
 * first share of the frame in the first buffer, the rest in equal shares.
 * Returns 0 when the driver had not offered the buffers.
 */
static int dev_rx(const uint8_t *frame, uint32_t len, int nbufs, uint16_t stated)
{
    uint32_t share = (len + (uint32_t)nbufs - 1u) / (uint32_t)nbufs, done = 0;
    struct virtio_net_hdr hdr;
    struct virtq_desc seg[4];
    uint16_t head;
    int b, n;

    k_memset(&hdr, 0, sizeof(hdr));
    hdr.num_buffers = stated;
    for (b = 0; b < nbufs; b++) {
        uint32_t take = len - done < share ? len - done : share;
        uint8_t *buf;

        if (!q_pop(VNET_Q_RX, &head, seg, &n)) {
            return 0;
        }
        check(n == 1 && (seg[0].flags & VIRTQ_DESC_F_WRITE) && seg[0].len == VNET_BUF,
              "a receive buffer is one writable segment of 2048 bytes");
        buf = dev_ptr(seg[0].addr, seg[0].len);
        if (!buf) {
            return 0;
        }
        if (b == 0) {
            k_memcpy(buf, &hdr, dev_hdr);
            buf += dev_hdr;
        }
        k_memcpy(buf, frame + done, take);
        done += take;
        q_push(VNET_Q_RX, head, take + (b == 0 ? dev_hdr : 0u));
    }
    return 1;
}

/** Take one transmit buffer: its header must be zeros, and the frame goes
 * to `out` with its length when `out` is given. 0 when none is offered. */
static int dev_tx(uint8_t *out, uint32_t *len)
{
    static const uint8_t zeros[VNET_HDR];
    struct virtq_desc seg[4];
    uint16_t head;
    uint8_t *buf;
    int n;

    if (!q_pop(VNET_Q_TX, &head, seg, &n)) {
        return 0;
    }
    check(n == 1 && !(seg[0].flags & VIRTQ_DESC_F_WRITE) && seg[0].len > dev_hdr,
          "a frame sent is one readable segment");
    buf = dev_ptr(seg[0].addr, seg[0].len);
    if (buf) {
        check(memcmp(buf, zeros, dev_hdr) == 0, "a frame sent has a header of zeros");
        if (out) {
            k_memcpy(out, buf + dev_hdr, seg[0].len - dev_hdr);
            *len = seg[0].len - dev_hdr;
        }
    }
    q_push(VNET_Q_TX, head, 0);
    return 1;
}

/** Answer every control command offered: keep its class, command and
 * data, and write the answer. */
static void dev_ctrl(void)
{
    struct virtq_desc seg[4];
    uint16_t head;
    int n;

    while (q_pop(VNET_Q_CTRL, &head, seg, &n)) {
        struct ctrl_rec *r = &m.ctrl[m.nctrl < 8 ? m.nctrl : 7];
        uint8_t *p;

        check(n == 3 && seg[0].len == 2 && !(seg[1].flags & VIRTQ_DESC_F_WRITE) &&
                  (seg[2].flags & VIRTQ_DESC_F_WRITE) && seg[2].len == 1,
              "a command is its class and command, its data, and a byte to answer in");
        if (n != 3) {
            return;
        }
        p = dev_ptr(seg[0].addr, 2);
        r->class = p ? p[0] : 0xFF;
        r->cmd = p ? p[1] : 0xFF;
        r->len = seg[1].len;
        p = dev_ptr(seg[1].addr, seg[1].len);
        if (p) {
            k_memcpy(r->data, p, seg[1].len < 8 ? seg[1].len : 8);
        }
        p = dev_ptr(seg[2].addr, 1);
        if (p) {
            *p = m.ctrl_ack;
        }
        m.nctrl++;
        q_push(VNET_Q_CTRL, head, 1);
    }
}

/** Deliver the receive queue's interrupt when the device raised one since
 * the last call, as virtio-pci's handler does; returns whether it did. */
static int rx_irq(void)
{
    static uint32_t seen;
    int raised = m.q[VNET_Q_RX].raised != seen;

    seen = m.q[VNET_Q_RX].raised;
    if (raised) {
        virtio_interrupt_queue(m.dev.queues[VNET_Q_RX]);
    }
    return raised;
}

/* Every feature QEMU's virtio-net-pci offers, the offloads among them, and
 * the three the rows turn on by hand. */
#define OFFLOADS                                                                    \
    (VIRTIO_BIT(VIRTIO_NET_F_CSUM) | VIRTIO_BIT(VIRTIO_NET_F_GUEST_CSUM) |          \
     VIRTIO_BIT(VIRTIO_NET_F_GUEST_TSO4) | VIRTIO_BIT(VIRTIO_NET_F_GUEST_TSO6) |    \
     VIRTIO_BIT(VIRTIO_NET_F_GUEST_UFO) | VIRTIO_BIT(VIRTIO_NET_F_HOST_TSO4) |      \
     VIRTIO_BIT(VIRTIO_NET_F_HOST_TSO6) | VIRTIO_BIT(VIRTIO_NET_F_HOST_UFO) |       \
     VIRTIO_BIT(VIRTIO_NET_F_CTRL_VLAN) | VIRTIO_BIT(VIRTIO_NET_F_GUEST_ANNOUNCE) | \
     VIRTIO_BIT(VIRTIO_NET_F_MQ))
#define QEMU_OFFER                                                              \
    (VIRTIO_BIT(VIRTIO_F_VERSION_1) | VIRTIO_BIT(VIRTIO_F_INDIRECT_DESC) |      \
     VIRTIO_BIT(VIRTIO_F_EVENT_IDX) | OFFLOADS | VIRTIO_BIT(VIRTIO_NET_F_MAC) | \
     VIRTIO_BIT(VIRTIO_NET_F_MRG_RXBUF) | VIRTIO_BIT(VIRTIO_NET_F_STATUS) |     \
     VIRTIO_BIT(VIRTIO_NET_F_CTRL_VQ) | VIRTIO_BIT(VIRTIO_NET_F_CTRL_RX) |      \
     VIRTIO_BIT(VIRTIO_NET_F_CTRL_MAC_ADDR))

static const uint8_t model_mac[6] = {0x52, 0x54, 0x00, 0x12, 0x34, 0x56};

/**
 * Free the queue records of the device the last row left bound, before the
 * model under them is cleared: a machine keeps a bound device for good, so
 * the driver never frees them, while the rings are the model's arena's. A
 * record on a device that no longer runs is one the driver failed to free.
 */
static void drop_bound(void)
{
    int i;

    for (i = 0; i < VIRTIO_MAX_QUEUES; i++) {
        if (m.dev.queues[i]) {
            check((m.status & VIRTIO_STATUS_DRIVER_OK) != 0,
                  "a queue is left only on a running device");
            kfree(m.dev.queues[i]);
            m.dev.queues[i] = 0;
        }
    }
}

/** A fresh model offering `offered`, with QEMU's queues of 256, 256 and
 * 64, its address and the link up, and a fresh interface. */
static void fresh(uint64_t offered)
{
    uint16_t up = VIRTIO_NET_S_LINK_UP;

    drop_bound();
    k_memset(&m, 0, sizeof(m));
    k_memset(arena_taken, 0, sizeof(arena_taken));
    m.dev.ops = &model_ops;
    m.dev.transport = &m;
    m.dev.name = "virtio0";
    m.dev.id = VIRTIO_ID_NET;
    m.dev.config_atomic = 1;
    m.present = 1;
    m.offered = offered;
    m.qmax[0] = 256;
    m.qmax[1] = 256;
    m.qmax[2] = 64;
    m.dma_left = -1;
    m.sys_dir = 55;
    sys_links = sys_unlinks = sys_link_parent = sys_link_target = sys_fail = 0;
    k_memcpy(m.config + VNET_CFG_MAC, model_mac, 6);
    k_memcpy(m.config + VNET_CFG_STATUS, &up, 2);
    model_pci.bus = 0;
    model_pci.device = 3;
    model_pci.function = 0;
    k_memset(vnets, 0, sizeof(vnets));
    k_memset(&nd, 0, sizeof(nd));
    nd.ops = &virtio_net_netdev;
    nd.index = 1;
    nd.pci_key = -1;
    nd.mtu = NETDEV_ETH_MTU_MAX;
    k_memcpy(nd.name, "eth0", 5);
    ifc = &nd;
    listed[0] = &nd;
    nlisted = 1;
    next_index = 3; /* lo is 2 */
    registers = unregisters = withdraws = reg_fail = 0;
    withdrawn = 0;
    ngot = rx_errors = carrier_changes = reply_from_rx = 0;
    udelays = 0;
    logged[0] = 0;
    dev_hdr = VNET_HDR;
    (void)rx_irq();
}

/** Bring the model up as the probe does; the driver's verdict. */
static int up(uint64_t offered)
{
    fresh(offered);
    return virtio_net_netdev.init(&nd, 0);
}

/** A frame of `len` bytes whose bytes count up from `seed`. */
static const uint8_t *frame_of(uint32_t len, uint8_t seed)
{
    static uint8_t f[4096];
    uint32_t i;

    for (i = 0; i < len; i++) {
        f[i] = (uint8_t)(seed + i);
    }
    return f;
}

/** Whether delivery `k` was the frame `frame_of(len, seed)`. */
static int got_is(int k, uint32_t len, uint8_t seed)
{
    return k < ngot && k < 8 && got_len[k] == len && memcmp(got[k], frame_of(len, seed), len) == 0;
}

/**
 * The bring-up against QEMU's offer: the features Linux's driver would
 * take less the offloads, the status that ends at DRIVER_OK, the receive
 * queue full of buffers and told of once, the receive filter set, and the
 * interface given the device's address and, as its `device` in /sys, the
 * virtio device's directory, which Linux's virtnet_probe makes its parent;
 * a device with no directory leaves the PCI function as the parent.
 */
static void row_bring_up(void)
{
    uint64_t want = VIRTIO_BIT(VIRTIO_F_VERSION_1) | VIRTIO_BIT(VIRTIO_F_INDIRECT_DESC) |
                    VIRTIO_BIT(VIRTIO_F_EVENT_IDX) | VIRTIO_BIT(VIRTIO_NET_F_MAC) |
                    VIRTIO_BIT(VIRTIO_NET_F_MRG_RXBUF) | VIRTIO_BIT(VIRTIO_NET_F_STATUS) |
                    VIRTIO_BIT(VIRTIO_NET_F_CTRL_VQ) | VIRTIO_BIT(VIRTIO_NET_F_CTRL_RX) |
                    VIRTIO_BIT(VIRTIO_NET_F_CTRL_MAC_ADDR);
    struct netdev_link link;
    uint16_t *avail;

    check(up(QEMU_OFFER) == NETDEV_UP, "QEMU's device comes up");
    check(m.driver == want, "the features are Linux's less the offloads, with EVENT_IDX");
    check((m.driver & OFFLOADS) == 0, "no checksum or segmentation offload is taken");
    check(m.status == (VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER |
                       VIRTIO_STATUS_FEATURES_OK | VIRTIO_STATUS_DRIVER_OK),
          "the status ends at DRIVER_OK");
    check(m.q[0].enabled && m.q[1].enabled && m.q[2].enabled, "three queues are set up");
    avail = q_avail(&m.q[VNET_Q_RX]);
    check(avail && avail[1] == 128, "the receive queue is filled, 128 buffers");
    check(m.q[VNET_Q_RX].notified == 1, "the device is told of them once");
    check(m.nctrl == 2 && m.ctrl[0].class == VIRTIO_NET_CTRL_RX &&
              m.ctrl[0].cmd == VIRTIO_NET_CTRL_RX_ALLMULTI && m.ctrl[0].data[0] == 1 &&
              m.ctrl[1].class == VIRTIO_NET_CTRL_RX &&
              m.ctrl[1].cmd == VIRTIO_NET_CTRL_RX_PROMISC && m.ctrl[1].data[0] == 0,
          "every multicast frame is asked for, then promiscuous mode turned off");
    check(memcmp(nd.mac, model_mac, 6) == 0, "the interface has the device's address");
    check(nd.dev != 0 && sys_links == 1 && sys_link_parent == SYS_ANCHOR + 1 &&
              sys_link_target == 55 && strcmp(sys_link_name, "device") == 0,
          "the interface's device in /sys is a link to the virtio device's directory");
    check(nd.pci_key == -1, "and no link to the PCI function stands in its place");
    check(nd.mtu == 1500 && nd.mtu_max == 0, "without VIRTIO_NET_F_MTU the MTU stays 1500");
    check(nd.ops->poll == 0, "a device that interrupts has no poll");
    check(m.bound && strcmp(m.bound, "virtio_net") == 0 && m.bound_sent == 1,
          "the device is shown bound to virtio_net and its bind announced");
    check(nd.ops->link(nd.dev, &link) == 1 && link.up == 1 && link.mbps == 0 &&
              link.full_duplex == -1,
          "the link is up, its speed and duplex unknown");
    check(registered == virtio_net_bind, "the driver is registered for a later bind");
    check(m.dma_out == 64 + 32 + 1 + 3 + 3,
          "128 receive and 64 transmit buffers, two to a page, a command page, the rings and "
          "their tables");
    fresh(QEMU_OFFER);
    m.sys_dir = 0;
    check(virtio_net_netdev.init(&nd, 0) == NETDEV_UP && sys_links == 0 && nd.pci_key == (3 << 3),
          "a device with no directory leaves the interface's device at the PCI function");
}

/**
 * Receive: a frame is delivered as it came and its buffer given back; five
 * frames before one interrupt are one reap and at most one notification;
 * a frame that arrives while the reap runs raises an interrupt of its own.
 */
static void row_receive(void)
{
    uint16_t *avail;
    uint32_t told;
    int i;

    check(up(QEMU_OFFER) == NETDEV_UP, "the device comes up");
    told = m.q[VNET_Q_RX].notified;
    check(dev_rx(frame_of(60, 1), 60, 1, 1) && rx_irq(), "a frame raises an interrupt");
    check(ngot == 1 && got_is(0, 60, 1), "the frame is delivered whole");
    avail = q_avail(&m.q[VNET_Q_RX]);
    check(avail && avail[1] == 129, "its buffer goes back to the queue");
    for (i = 0; i < 5; i++) {
        check(dev_rx(frame_of(1514, (uint8_t)(10 + i)), 1514, 1, 1), "a full frame is taken");
    }
    check(m.q[VNET_Q_RX].raised == 2, "five frames raise one interrupt");
    check(rx_irq() && ngot == 6 && got_is(5, 1514, 14), "one reap delivers all five");
    check(avail && avail[1] == 134, "all five buffers go back");
    check(m.q[VNET_Q_RX].notified - told <= 2, "the buffers are given back a batch at a time");
    reply_from_rx = 1;
    check(dev_rx(frame_of(100, 3), 100, 1, 1) && rx_irq(), "a frame arrives");
    check(m.q[VNET_Q_TX].notified == 1 && dev_tx(0, 0), "an answer sent from the delivery leaves");
    reply_from_rx = 0;
    for (i = 0; i < 300; i++) {
        if (!dev_rx(frame_of(64, (uint8_t)i), 64, 1, 1) || !rx_irq()) {
            break;
        }
    }
    check(i == 300 && ngot == 307, "300 frames pass through 128 buffers, the indices wrapping");
    told = m.q[VNET_Q_RX].notified;
    for (i = 0; i < 128; i++) {
        check(dev_rx(frame_of(64, 1), 64, 1, 1), "the device fills every buffer it has");
    }
    check(!dev_rx(frame_of(64, 1), 64, 1, 1), "and has none left");
    check(rx_irq() && ngot == 307 + 128, "one reap delivers the 128 frames");
    check(m.q[VNET_Q_RX].notified == told + 1, "and tells the device once of its buffers");
    check(dev_rx(frame_of(64, 1), 64, 1, 1) && rx_irq(), "which it then fills again");
    check(rx_errors == 0, "no frame was refused");
}

/**
 * VIRTIO_NET_F_MRG_RXBUF: a frame across two and three buffers is put
 * together; one that outgrows a frame, one shorter than a header's worth
 * and a runt are counted and dropped; the next frame is whole. Without the
 * feature `num_buffers` is not read.
 */
static void row_mergeable(void)
{
    uint8_t runt[4] = {1, 2, 3, 4};

    check(up(QEMU_OFFER) == NETDEV_UP, "the device comes up");
    check(dev_rx(frame_of(1514, 7), 1514, 2, 2) && rx_irq(), "a frame arrives in two buffers");
    check(ngot == 1 && got_is(0, 1514, 7), "it is delivered whole");
    check(dev_rx(frame_of(900, 9), 900, 3, 3) && rx_irq(), "a frame arrives in three buffers");
    check(ngot == 2 && got_is(1, 900, 9), "it is delivered whole");
    check(dev_rx(frame_of(3000, 1), 3000, 2, 2) && rx_irq(), "3000 bytes arrive in two buffers");
    check(ngot == 2 && rx_errors == 1, "a frame past 1514 bytes is counted, not delivered");
    check(dev_rx(frame_of(1515, 1), 1515, 1, 1) && rx_irq() && ngot == 2 && rx_errors == 2,
          "1515 bytes in one buffer are counted, not delivered");
    check(dev_rx(runt, 4, 1, 1) && rx_irq() && ngot == 2 && rx_errors == 3,
          "a frame shorter than an Ethernet header is counted");
    check(dev_rx(frame_of(14, 5), 14, 1, 1) && rx_irq() && ngot == 3 && got_is(2, 14, 5),
          "a frame of the header alone is delivered");
    check(q_avail(&m.q[VNET_Q_RX])[1] == 128 + 10, "every buffer went back");
    check(up(QEMU_OFFER & ~VIRTIO_BIT(VIRTIO_NET_F_MRG_RXBUF)) == NETDEV_UP,
          "a device without mergeable buffers comes up");
    check(dev_rx(frame_of(1514, 2), 1514, 1, 0x7777) && rx_irq() && ngot == 1 && got_is(0, 1514, 2),
          "its frame is delivered whatever num_buffers holds");
}

/**
 * A legacy device, without VIRTIO_F_VERSION_1: its header is 10 bytes
 * without mergeable buffers and 12 with them, as Linux's virtnet_probe
 * sizes it, both ways.
 */
static void row_legacy_header(void)
{
    uint64_t legacy = QEMU_OFFER & ~VIRTIO_BIT(VIRTIO_F_VERSION_1);
    uint8_t out[VNET_FRAME_MAX];
    uint32_t len = 0;

    fresh(legacy & ~VIRTIO_BIT(VIRTIO_NET_F_MRG_RXBUF));
    m.dev.legacy = 1;
    dev_hdr = VNET_HDR_LEGACY;
    check(virtio_net_netdev.init(&nd, 0) == NETDEV_UP && !(m.status & VIRTIO_STATUS_FEATURES_OK),
          "a legacy device comes up without FEATURES_OK");
    check(dev_rx(frame_of(60, 1), 60, 1, 0) && rx_irq() && ngot == 1 && got_is(0, 60, 1),
          "a frame after a header of 10 bytes is delivered whole");
    check(dev_rx(frame_of(1514, 2), 1514, 1, 0) && rx_irq() && ngot == 2 && got_is(1, 1514, 2),
          "and so is a full one");
    check(nd.ops->tx(nd.dev, frame_of(60, 4), 60) == NETDEV_TX_OK && dev_tx(out, &len) &&
              len == 60 && memcmp(out, frame_of(60, 4), 60) == 0,
          "a frame sent leaves after a header of 10 zeros");
    fresh(legacy);
    m.dev.legacy = 1;
    check(virtio_net_netdev.init(&nd, 0) == NETDEV_UP, "a legacy device with mergeable buffers");
    check(dev_rx(frame_of(1514, 7), 1514, 2, 2) && rx_irq() && ngot == 1 && got_is(0, 1514, 7) &&
              nd.ops->tx(nd.dev, frame_of(61, 4), 61) == NETDEV_TX_OK && dev_tx(out, &len) &&
              len == 61,
          "has the header of 12 bytes both ways");
    check(rx_errors == 0, "no frame was refused");
}

/**
 * Transmit: the frame leaves after a header of zeros; with EVENT_IDX three
 * frames the device has not taken cost one notification; the device's
 * returns raise no interrupt and are taken at the next send; a full pool
 * is waited on ten intervals, then the frame is dropped; and a frame of no
 * bytes or past 1514 is refused.
 */
static void row_transmit(void)
{
    uint8_t out[VNET_FRAME_MAX];
    uint32_t len = 0;
    int i, sent = 0;

    check(up(QEMU_OFFER) == NETDEV_UP, "the device comes up");
    m.q[VNET_Q_TX].ask = 0; /* as QEMU, which has a send scheduled */
    for (i = 0; i < 3; i++) {
        check(nd.ops->tx(nd.dev, frame_of(60 + (uint32_t)i, 4), (uint16_t)(60 + i)) == NETDEV_TX_OK,
              "a frame is taken");
    }
    check(m.q[VNET_Q_TX].notified == 1, "three frames the device has not taken cost one notify");
    check(dev_tx(out, &len) && len == 60 && memcmp(out, frame_of(60, 4), 60) == 0,
          "the first frame leaves as it was given, unpadded");
    check(dev_tx(out, &len) && len == 61 && dev_tx(out, &len) && len == 62 && !dev_tx(0, 0),
          "the others follow in order");
    check(m.q[VNET_Q_TX].raised == 0, "the device's returns raise no interrupt");
    check(nd.ops->tx(nd.dev, frame_of(0, 0), 0) == NETDEV_TX_DROPPED, "no bytes are refused");
    check(nd.ops->tx(nd.dev, frame_of(1515, 0), 1515) == NETDEV_TX_DROPPED &&
              strstr(logged, "too large") != 0,
          "1515 bytes are refused and logged");
    for (i = 0; i < 70; i++) {
        sent += nd.ops->tx(nd.dev, frame_of(100, 1), 100) == NETDEV_TX_OK;
    }
    check(sent == 64, "the pool is 64 frames while the device takes none");
    check(udelays == 6u * VNET_TX_WAIT_STEPS, "each refused frame waited ten intervals first");
    m.tx_per_delay = 1;
    udelays = 0;
    check(nd.ops->tx(nd.dev, frame_of(100, 1), 100) == NETDEV_TX_OK && udelays == 1,
          "a frame is taken as soon as the device returns a buffer");
    m.tx_per_delay = 0;
    while (dev_tx(0, 0)) {
    }
    sent = 0;
    for (i = 0; i < 1000; i++) {
        sent += nd.ops->tx(nd.dev, frame_of(1514, 1), 1514) == NETDEV_TX_OK;
        (void)dev_tx(0, 0);
    }
    check(sent == 1000, "1000 frames pass through the pool as the device takes them");
    check(m.q[VNET_Q_TX].raised == 0, "and none of their returns interrupts");
}

/**
 * Without EVENT_IDX the queues work by the flags: the transmit queue sets
 * VIRTQ_AVAIL_F_NO_INTERRUPT and the receive queue does not.
 */
static void row_no_event_idx(void)
{
    check(up(QEMU_OFFER & ~VIRTIO_BIT(VIRTIO_F_EVENT_IDX)) == NETDEV_UP, "the device comes up");
    check(q_avail(&m.q[VNET_Q_TX])[0] == VIRTQ_AVAIL_F_NO_INTERRUPT &&
              q_avail(&m.q[VNET_Q_RX])[0] == 0,
          "transmit asks for no interrupt, receive for each");
    check(nd.ops->tx(nd.dev, frame_of(60, 1), 60) == NETDEV_TX_OK && dev_tx(0, 0) &&
              m.q[VNET_Q_TX].raised == 0,
          "a frame sent returns without an interrupt");
    check(dev_rx(frame_of(60, 1), 60, 1, 1) && rx_irq() && ngot == 1, "a frame is received");
}

/**
 * The link follows the configuration: down and up at its interrupt, with
 * the interface told; always up without VIRTIO_NET_F_STATUS; the speed and
 * duplex with VIRTIO_NET_F_SPEED_DUPLEX, each only when valid.
 */
static void row_link(void)
{
    struct netdev_link link;
    uint16_t status = 0;
    uint32_t speed = 1000;

    fresh(QEMU_OFFER | VIRTIO_BIT(VIRTIO_NET_F_SPEED_DUPLEX));
    k_memset(m.config + VNET_CFG_SPEED, 0xFF, 5);
    check(virtio_net_netdev.init(&nd, 0) == NETDEV_UP, "it comes up");
    check(nd.ops->link(nd.dev, &link) && link.up && link.mbps == 0 && link.full_duplex == -1,
          "a speed of 0xFFFFFFFF and a duplex of 0xFF are unknown");
    k_memcpy(m.config + VNET_CFG_STATUS, &status, 2);
    virtio_interrupt_config(&m.dev);
    check(nd.ops->link(nd.dev, &link) && !link.up && carrier_changes == 1,
          "the link goes down at the configuration's interrupt, and the interface is told");
    virtio_interrupt_config(&m.dev);
    check(carrier_changes == 1, "an interrupt without a change tells nothing");
    status = VIRTIO_NET_S_LINK_UP;
    k_memcpy(m.config + VNET_CFG_STATUS, &status, 2);
    k_memcpy(m.config + VNET_CFG_SPEED, &speed, 4);
    m.config[VNET_CFG_DUPLEX] = 1;
    virtio_interrupt_config(&m.dev);
    check(nd.ops->link(nd.dev, &link) && link.up && link.mbps == 1000 && link.full_duplex == 1 &&
              carrier_changes == 2,
          "it comes up again at 1000 Mb/s, full duplex");
    check(strstr(logged, "link: virtio0: down") && strstr(logged, "link: virtio0: up"),
          "both changes are logged");
    status = 0;
    check(up(QEMU_OFFER & ~VIRTIO_BIT(VIRTIO_NET_F_STATUS)) == NETDEV_UP, "it comes up");
    k_memcpy(m.config + VNET_CFG_STATUS, &status, 2);
    virtio_interrupt_config(&m.dev);
    check(nd.ops->link(nd.dev, &link) && link.up, "without the status word the link is up");
}

/**
 * The link comes up while the bring-up reads it: the read has the old
 * status word, down, and the interrupt that reports the change arrives
 * before the read's result is kept. The interrupt's reading, the newer
 * one, must stand, as it does when it is taken after the first read is
 * over; kept the other way round, the link stays down until the next
 * change.
 */
static void row_link_race(void)
{
    struct netdev_link link;
    uint16_t status = 0;

    fresh(QEMU_OFFER);
    k_memcpy(m.config + VNET_CFG_STATUS, &status, 2);
    m.up_at_read = 1;
    check(virtio_net_netdev.init(&nd, 0) == NETDEV_UP && m.up_at_read == 0,
          "the device comes up, its link changing under the first read");
    check(nd.ops->link(nd.dev, &link) && link.up,
          "the interrupt's newer reading stands over the first read's");
    check(strstr(logged, "link: virtio0: up") != 0, "and the change is logged");
}

/**
 * VIRTIO_NET_F_MTU: a device's 1400 is the interface's MTU and its limit;
 * 9000 leaves 1500; and 60, below the least an interface may have, makes
 * the driver negotiate again without the feature, as Linux does.
 */
static void row_mtu(void)
{
    uint16_t mtu = 1400;

    fresh(QEMU_OFFER | VIRTIO_BIT(VIRTIO_NET_F_MTU));
    k_memcpy(m.config + VNET_CFG_MTU, &mtu, 2);
    check(virtio_net_netdev.init(&nd, 0) == NETDEV_UP && nd.mtu == 1400 && nd.mtu_max == 1400,
          "a device's 1400 is the MTU and its limit");
    check(m.driver & VIRTIO_BIT(VIRTIO_NET_F_MTU), "the feature is taken");
    mtu = 9000;
    fresh(QEMU_OFFER | VIRTIO_BIT(VIRTIO_NET_F_MTU));
    k_memcpy(m.config + VNET_CFG_MTU, &mtu, 2);
    check(virtio_net_netdev.init(&nd, 0) == NETDEV_UP && nd.mtu == 1500 && nd.mtu_max == 0,
          "a device's 9000 leaves the stack's 1500");
    mtu = 60;
    fresh(QEMU_OFFER | VIRTIO_BIT(VIRTIO_NET_F_MTU));
    k_memcpy(m.config + VNET_CFG_MTU, &mtu, 2);
    check(virtio_net_netdev.init(&nd, 0) == NETDEV_UP && nd.mtu == 1500 && nd.mtu_max == 0,
          "a device's 60 is ignored");
    check(!(m.driver & VIRTIO_BIT(VIRTIO_NET_F_MTU)) && m.inits == 2,
          "by a second negotiation without the feature");
    check(m.dma_out == 64 + 32 + 1 + 3 + 3, "which leaves no page of the first behind");
}

/**
 * The address and the control queue: without VIRTIO_NET_F_MAC an address
 * is made up, locally administered, and given to the device before the
 * filter is set; without the command that gives it the device stays
 * promiscuous; without a control queue two queues are set up; a command
 * the device refuses or never answers leaves the device up.
 */
static void row_control(void)
{
    static const uint8_t made[6] = {0xA6, 0xA5, 0xA5, 0xA5, 0xA5, 0xA5};
    uint64_t no_mac = QEMU_OFFER & ~VIRTIO_BIT(VIRTIO_NET_F_MAC);

    check(up(no_mac) == NETDEV_UP && memcmp(nd.mac, made, 6) == 0,
          "a made-up address is unicast and locally administered");
    check(m.nctrl == 3 && m.ctrl[0].class == VIRTIO_NET_CTRL_MAC &&
              m.ctrl[0].cmd == VIRTIO_NET_CTRL_MAC_ADDR_SET && m.ctrl[0].len == 6 &&
              memcmp(m.ctrl[0].data, made, 6) == 0,
          "the device is given it first, then the filter is set");
    check(up(no_mac & ~VIRTIO_BIT(VIRTIO_NET_F_CTRL_MAC_ADDR)) == NETDEV_UP && m.nctrl == 0,
          "a device that cannot be told the address is left accepting every frame");
    check(up(QEMU_OFFER & ~(VIRTIO_BIT(VIRTIO_NET_F_CTRL_VQ) | VIRTIO_BIT(VIRTIO_NET_F_CTRL_RX) |
                            VIRTIO_BIT(VIRTIO_NET_F_CTRL_MAC_ADDR))) == NETDEV_UP &&
              !m.q[VNET_Q_CTRL].enabled && m.nctrl == 0,
          "a device without a control queue comes up on two queues");
    fresh(QEMU_OFFER);
    m.ctrl_ack = 1;
    check(virtio_net_netdev.init(&nd, 0) == NETDEV_UP && m.nctrl == 1,
          "a refused ALLMULTI leaves the device up and promiscuous");
    fresh(QEMU_OFFER);
    m.ctrl_mute = 1;
    check(virtio_net_netdev.init(&nd, 0) == NETDEV_UP && udelays == VNET_CTRL_STEPS &&
              strstr(logged, "was not answered") != 0,
          "a command never answered is waited on 100 ms, logged, and the device is up");
    check(m.q[VNET_Q_CTRL].notified == 1, "and no second command is sent");
    check(dev_rx(frame_of(60, 1), 60, 1, 1) && rx_irq() && ngot == 1, "which still receives");
}

/**
 * The driver table's `stop`: the frames given to the device are waited
 * for, the device reset and every page given back; the link reads down and
 * a send is refused; a second stop does nothing. And the probe's failures:
 * no device, no memory at each allocation, each leaving no page, no
 * binding and the device given back.
 */
static void row_stop_and_failures(void)
{
    struct netdev_link link;
    void *dev;
    int k;

    check(up(QEMU_OFFER) == NETDEV_UP, "the device comes up");
    dev = nd.dev;
    check(nd.ops->tx(dev, frame_of(60, 1), 60) == NETDEV_TX_OK, "a frame is given to it");
    m.tx_per_delay = 1;
    udelays = 0;
    nd.ops->stop(dev);
    check(udelays == 1, "stop waits until the device has sent the frame");
    check(m.status == 0 && m.dma_out == 0, "then resets it and gives every page back");
    check(nd.ops->link(dev, &link) && !link.up, "the link reads down");
    check(nd.ops->tx(dev, frame_of(60, 1), 60) == NETDEV_TX_DROPPED, "a send is refused");
    nd.ops->stop(dev);
    nd.ops->stop(0);
    check(m.dma_out == 0, "a second stop, and one of no device, do nothing");
    fresh(QEMU_OFFER);
    m.present = 0;
    check(virtio_net_netdev.init(&nd, 0) == NETDEV_ABSENT && m.status == 0,
          "no device is absent, with nothing touched");
    check(virtio_net_netdev.init(&nd, 1) == NETDEV_ABSENT, "and so is a second one");
    for (k = 0; k < 64 + 32 + 1 + 3 + 3; k += 7) {
        fresh(QEMU_OFFER);
        m.dma_left = k;
        if (virtio_net_netdev.init(&nd, 0) == NETDEV_UP) {
            /* The control queue's page and tables are optional. */
            check(m.dma_out >= 64 + 32 + 2, "a device up without a table has its buffers");
            continue;
        }
        check(m.status == 0 || (m.status & VIRTIO_STATUS_FAILED), "a failed device is reset");
        check(m.dma_out == 0 && m.bound == 0 && m.puts == 1 && nd.dev == 0,
              "it leaves no page and no binding, and is given back");
        check(virtio_net_netdev.error()[0] != 0, "with a reason");
        nd.ops->stop(nd.dev);
    }
}

/** The interface the last bind registered, or NULL. */
static struct netdev *bound_if(void)
{
    return nlisted > 0 ? listed[nlisted - 1] : 0;
}

/**
 * An unbind, as Linux's virtnet_remove: the transport's `removed` stops the
 * device, gives it back and withdraws its interface from the stack.
 */
static void row_unbind(void)
{
    struct netdev_link link;
    void *dev;

    check(up(QEMU_OFFER) == NETDEV_UP && m.dev.removed != 0, "the device comes up");
    dev = nd.dev;
    m.dev.removed(&m.dev);
    check(m.status == 0 && m.dma_out == 0 && m.puts == 1 && m.bound == 0 && m.unbinds == 1,
          "removal resets the device, frees its pages, unbinds it and gives it back");
    check(sys_links == 1 && sys_unlinks == 1, "and takes away the interface's device in /sys");
    check(withdraws == 1 && withdrawn == &nd && nlisted == 0,
          "and withdraws its interface from the stack");
    check(strstr(logged, "removed: virtio0: eth0") != 0, "the removal names the interface");
    check(nd.ops->link(dev, &link) && !link.up, "the old record reads no link");
    check(nd.ops->tx(dev, frame_of(60, 1), 60) == NETDEV_TX_DROPPED, "and sends nothing");
}

/**
 * A bind after the unbind brings the function up on a new interface, the
 * lowest free `ethN` with a new index, down, with its device in /sys, which
 * receives and sends, as Linux's virtnet_probe makes it.
 */
static void row_rebind(void)
{
    struct netdev_link link;
    struct netdev *n;

    check(up(QEMU_OFFER) == NETDEV_UP, "the device comes up");
    m.dev.removed(&m.dev);
    check(virtio_net_bind(&m.dev) == 0 && registers == 1, "a bind registers an interface");
    n = bound_if();
    check(n != 0 && n != &nd && strcmp(n->name, "eth0") == 0 && n->index == 3,
          "a new one, eth0 again since it is free, with a new index");
    check(n && n->up == 0, "down, as Linux leaves a new interface");
    check(n && n->dev != 0 && n->ops->link(n->dev, &link) && link.up && m.bound != 0 &&
              memcmp(n->mac, model_mac, 6) == 0,
          "whose device is bound with its link up, and it has the device's address");
    check(sys_links == 2 && sys_link_target == 55 && sys_link_parent == SYS_ANCHOR + 3 &&
              strstr(logged, "bound: virtio0: eth0") != 0,
          "its device in /sys is the virtio device, and the bind is logged");
    ifc = n;
    check(dev_rx(frame_of(60, 1), 60, 1, 1) && rx_irq() && ngot == 1, "which receives");
    check(n && n->ops->tx(n->dev, frame_of(60, 1), 60) == NETDEV_TX_OK && dev_tx(0, 0),
          "and sends");
    ifc = &nd;
}

/**
 * Binds after the first: a second unbind withdraws the bound interface, and
 * the next bind has a new index and the lowest free name. A bind with every
 * record busy, or whose registration fails, leaves no interface and gives
 * the device back untouched.
 */
static void row_rebind_again(void)
{
    static struct netdev other;
    struct netdev *n;

    check(up(QEMU_OFFER) == NETDEV_UP, "the device comes up");
    m.dev.removed(&m.dev);
    check(virtio_net_bind(&m.dev) == 0 && (n = bound_if()) != 0, "and is bound again");
    m.dev.removed(&m.dev);
    check(withdraws == 2 && withdrawn == n && nlisted == 0, "a second unbind withdraws it");
    k_memcpy(other.name, "eth0", 5);
    listed[nlisted++] = &other;
    check(virtio_net_bind(&m.dev) == 0, "a third bind succeeds");
    n = bound_if();
    check(n && strcmp(n->name, "eth1") == 0 && n->index == 4,
          "with eth0 taken, it is eth1, with a new index again");
    m.dev.removed(&m.dev);
    nlisted = 0;
    vnets[0].nd = vnets[1].nd = vnets[2].nd = vnets[3].nd = &other;
    m.puts = 0;
    check(virtio_net_bind(&m.dev) == -ENOSPC && m.puts == 1 && registers == 2,
          "with every record busy, a bind is refused with no interface and the device given back");
    vnets[0].nd = vnets[1].nd = vnets[2].nd = vnets[3].nd = 0;
    reg_fail = -ENOSPC;
    m.puts = 0;
    check(virtio_net_bind(&m.dev) == -ENOSPC && m.puts == 1 && m.status == 0 && m.bound == 0 &&
              nlisted == 0,
          "a registration that fails leaves the device untouched and given back");
    ifc = &nd;
}

/**
 * A bring-up whose `device` in /sys cannot be made fails and is undone, as
 * Linux's device_add fails register_netdev: the device is reset, its pages
 * freed, its binding taken away and the device given back, and no link to
 * the PCI function stands in for the one that failed, at the probe; at a
 * bind the interface registered for it is taken off the list too. A device
 * with no directory has the PCI function as its parent, and an unbind takes
 * that away too.
 */
static void row_sysfs_failure(void)
{
    struct netdev *n;
    int fail;

    for (fail = 1; fail <= 2; fail++) {
        fresh(QEMU_OFFER);
        sys_fail = fail;
        check(virtio_net_netdev.init(&nd, 0) == NETDEV_FAILED,
              "a bring-up whose link in /sys fails fails");
        check(m.status == 0 && m.dma_out == 0 && m.bound == 0 && m.puts == 1 && nd.dev == 0,
              "the device is reset, its pages freed, unbound and given back");
        check(nd.pci_key == -1 && sys_links == 0, "and the interface has no parent in /sys");
        check(strstr(virtio_net_netdev.error(), "/sys") != 0, "the reason names /sys");

        check(up(QEMU_OFFER) == NETDEV_UP, "the device comes up");
        m.dev.removed(&m.dev);
        sys_fail = fail;
        m.puts = 0;
        check(virtio_net_bind(&m.dev) == -ENOMEM, "a bind whose link in /sys fails fails");
        check(m.status == 0 && m.dma_out == 0 && m.bound == 0 && m.puts == 1,
              "the device is reset, its pages freed, unbound and given back");
        check(registers == 1 && unregisters == 2 && nlisted == 0,
              "and the interface registered for it is off the list");
    }
    fresh(QEMU_OFFER);
    m.sys_dir = 0;
    check(virtio_net_netdev.init(&nd, 0) == NETDEV_UP && nd.pci_key == (3 << 3),
          "a device with no directory has the PCI function as its parent");
    m.dev.removed(&m.dev);
    check(nd.pci_key == -1, "which an unbind takes away");
    check(virtio_net_bind(&m.dev) == 0 && (n = bound_if()) != 0 && n->pci_key == (3 << 3),
          "and a bind gives the new interface");
    n->ops->stop(n->dev);
    check(n->pci_key == -1, "as a stop takes it away");
}

/**
 * A function bound again as a device with another largest MTU: each new
 * interface has the device's limit, as Linux's virtnet_probe sets max_mtu
 * at every probe, by the time the device is first told of buffers, and its
 * MTU is the limit where that is below 1500.
 */
static void row_rebind_mtu(void)
{
    uint16_t mtu = 1400;
    struct netdev *n;

    fresh(QEMU_OFFER | VIRTIO_BIT(VIRTIO_NET_F_MTU));
    k_memcpy(m.config + VNET_CFG_MTU, &mtu, 2);
    check(virtio_net_netdev.init(&nd, 0) == NETDEV_UP && nd.mtu == 1400 && nd.mtu_max == 1400,
          "the device comes up with 1400 as the MTU and its limit");
    m.dev.removed(&m.dev);
    mtu = 1300;
    k_memcpy(m.config + VNET_CFG_MTU, &mtu, 2);
    m.limit_seen = 0;
    ifc = 0;
    check(virtio_net_bind(&m.dev) == 0 && (n = bound_if()) != 0 && n->mtu_max == 1300 &&
              n->mtu == 1300,
          "bound again with 1300, the new interface's limit and MTU are 1300");
    check(m.limit_seen && m.limit == 1300 && m.mtu == 1300,
          "before the device is told of a buffer");
    m.dev.removed(&m.dev);
    mtu = 1450;
    k_memcpy(m.config + VNET_CFG_MTU, &mtu, 2);
    check(virtio_net_bind(&m.dev) == 0 && (n = bound_if()) != 0 && n->mtu_max == 1450 &&
              n->mtu == 1450,
          "bound again with 1450, they are 1450");
    m.dev.removed(&m.dev);
    m.offered &= ~VIRTIO_BIT(VIRTIO_NET_F_MTU);
    check(virtio_net_bind(&m.dev) == 0 && (n = bound_if()) != 0 && n->mtu_max == 0 &&
              n->mtu == 1500,
          "bound again with no largest MTU, the interface has the stack's 1500");
    ifc = &nd;
}

/**
 * A device the transport cannot interrupt for: the interface takes the
 * table with a `poll`, which delivers what the device returned, and reads
 * the link once in VNET_LINK_POLLS polls, since no interrupt reports it:
 * a device started with its cable out sends once the cable is in.
 */
static void row_polled(void)
{
    struct netdev_link link;
    uint16_t status = 0;
    int i, reads;

    fresh(QEMU_OFFER);
    m.polled = 1;
    check(virtio_net_netdev.init(&nd, 0) == NETDEV_UP && nd.ops->poll != 0,
          "a device without interrupts gets a poll");
    check(dev_rx(frame_of(60, 1), 60, 1, 1) && ngot == 0, "a frame waits");
    nd.ops->poll(nd.dev);
    check(ngot == 1 && got_is(0, 60, 1), "the poll delivers it");
    check(nd.ops->tx(nd.dev, frame_of(60, 1), 60) == NETDEV_TX_OK && dev_tx(0, 0), "and it sends");
    fresh(QEMU_OFFER);
    m.polled = 1;
    k_memcpy(m.config + VNET_CFG_STATUS, &status, 2);
    check(virtio_net_netdev.init(&nd, 0) == NETDEV_UP && nd.ops->link(nd.dev, &link) && !link.up,
          "a polled device started with its cable out has no link");
    status = VIRTIO_NET_S_LINK_UP;
    k_memcpy(m.config + VNET_CFG_STATUS, &status, 2);
    reads = m.status_reads;
    for (i = 0; i < VNET_LINK_POLLS - 1; i++) {
        nd.ops->poll(nd.dev);
    }
    check(m.status_reads == reads && nd.ops->link(nd.dev, &link) && !link.up,
          "the polls short of the interval read no configuration");
    nd.ops->poll(nd.dev);
    check(m.status_reads == reads + 1 && nd.ops->link(nd.dev, &link) && link.up &&
              carrier_changes == 1 && strstr(logged, "link: virtio0: up") != 0,
          "the poll at the interval finds the cable in, and the interface is told");
    status = 0;
    k_memcpy(m.config + VNET_CFG_STATUS, &status, 2);
    for (i = 0; i < VNET_LINK_POLLS; i++) {
        nd.ops->poll(nd.dev);
    }
    check(nd.ops->link(nd.dev, &link) && !link.up && carrier_changes == 2,
          "and an interval later, the cable pulled out");
}

int main(void)
{
    row_bring_up();
    row_receive();
    row_mergeable();
    row_legacy_header();
    row_transmit();
    row_no_event_idx();
    row_link();
    row_link_race();
    row_mtu();
    row_control();
    row_stop_and_failures();
    row_unbind();
    row_rebind();
    row_rebind_again();
    row_rebind_mtu();
    row_sysfs_failure();
    row_polled();
    check(net_locks > 0, "the sends ran under net_lock");
    drop_bound();
    if (failures) {
        printf("virtio-net-hosttest: %d failed\n", failures);
        return 1;
    }
    printf("virtio-net-hosttest: ok\n");
    return 0;
}
