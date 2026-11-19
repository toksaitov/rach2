/*
 * virtio_console.c: the virtio console (virtio 1.2, 5.3) over the shared
 * virtio core, as Linux's drivers/char/virtio_console.c with its hvc
 * console: every device and every port it announces, each port's nodes in
 * /dev, a byte device or an hvc terminal, the control queues, and the ports
 * that carry the kernel's console as lines of serial.h.
 */

#include "virtio_console.h"

#include "../arch/arch.h"
#include "../cmdline.h"
#include "../kio.h"
#include "../klog.h"
#include "../panic.h"
#include "../serial.h"
#include "../watchdog.h"
#include "../fs/devfs.h"
#include "../fs/fd.h"
#include "../fs/file.h"
#include "../io/poll.h"
#include "../lib/kerrno.h"
#include "../lib/kirq.h"
#include "../lib/kstring.h"
#include "../mm/kheap.h"
#include "../mm/usercopy.h"
#include "../proc/process.h"
#include "../proc/sched.h"
#include "../proc/wait_queue.h"
#include "../tty/pty.h"
#include "../tty/tty.h"

/* A port's buffers: VCON_RX_BUFS that the device fills, as many as the
 * receive ring of serial.h holds twice over, VCON_TX_BUFS that the driver
 * fills, and one more that only a panic fills, each a page. */
#define VCON_BUF     4096u
#define VCON_RX_BUFS 8
#define VCON_TX_BUFS 4

/* The control messages the driver may have given the device at once, each
 * 8 bytes of one page: a message the device has not taken in its wait
 * keeps its slot until a reap finds it returned. The control queue's
 * receive buffers are a page each, as many as the queue holds, as Linux
 * fills it, since QEMU drops a message that finds no buffer. */
#define VCON_CTL_TX 16

/*
 * How long a transmit waits for the device to return a buffer, as serial.c
 * waits for the UART: VCON_TX_WAIT_MS while the device takes what it is
 * given, long enough for a slow reader of the host's end, since QEMU keeps
 * a port's buffer while its reader falls behind; then, until the device
 * returns one again, VCON_TX_STUCK_MS, so that a host that stopped reading
 * costs one long wait and then a short one for each flush. The waits are
 * counted in cycles of the time-stamp counter at VCON_CYCLES_PER_MS, faster
 * than any processor's counter runs, so that no wait is shorter than its
 * milliseconds. A control message waits VCON_CTL_WAIT_MS, and the boot
 * waits VCON_BOOT_WAIT_MS for the devices to announce the ports the boot
 * line names.
 */
#define VCON_TX_WAIT_MS    30000UL
#define VCON_TX_STUCK_MS   2UL
#define VCON_CTL_WAIT_MS   1000UL
#define VCON_BOOT_WAIT_MS  2000UL
#define VCON_CYCLES_PER_MS 5000000UL

/* Passes of the console's service after one receive interrupt, as serial.c
 * bounds its handler: each moves at most a pump's budget from the ring. */
#define VCON_RX_PASSES 256

/* The ports the boot line may name. */
#define VCON_WANT_MAX 8

/* The hvc numbers, Linux's MAX_NR_HVC_CONSOLES, and the minors of the
 * vport nodes, one byte of the device number. */
#define VCON_HVC_MAX 16
#define VCON_MINORS  256

/* The longest name of a port that /dev/virtio-ports takes: the rest of a
 * node's path. */
#define VCON_LINK_DIR "virtio-ports/"
#define VCON_NAME_MAX (DEVFS_NODE_NAME_MAX - (int)sizeof(VCON_LINK_DIR))

/* The bytes an hvc terminal's output is moved in, from the pair to the
 * port's pages. */
#define VCON_HOLD 512u

/* The control replies a port is owed (`ctl_due` of a port). */
#define VCON_DUE_READY 1u /* PORT_READY */
#define VCON_DUE_OPEN  2u /* PORT_OPEN, with the guest's side as it is then */

/* A buffer of a port: a page the device reads or writes. */
struct vcon_buf {
    unsigned char *data;
    uint64_t addr;              /* as the device reads it */
    uint32_t len;               /* receive: the bytes the device wrote */
    uint32_t off;               /* receive: those a reader has taken */
    volatile unsigned int busy; /* transmit: the device has it */
};

/* A node of /dev that the driver registers: the record devfs keeps, its
 * name, and whether it is registered. */
struct vcon_node {
    struct devfs_node n;
    char name[DEVFS_NODE_NAME_MAX];
    int on;
};

struct vcon;

/* A port. Its buffers are made when the device first announces it and
 * stay until the device is reset; its queues are set up with the device's.
 * A port removed keeps its record, which a descriptor of its node may
 * still name, and its `life` tells such a descriptor that it has gone. */
struct vcon_port {
    struct vcon *dev;
    uint32_t id;
    int present;          /* the device has the port */
    int console;          /* a console port: port 0 without F_MULTIPORT, or one
                           * the device marked with CONSOLE_PORT */
    int hvc;              /* the N of the console port's name hvcN, given when it
                           * became a console port and kept until it goes; -1 */
    int minor;            /* the minor of its vport node, or -1 */
    int host_open;        /* the device's last PORT_OPEN */
    int guest_open;       /* a descriptor of its vport node is open */
    int sent_open;        /* the PORT_OPEN value the device was last told, -1 */
    unsigned int ctl_due; /* VCON_DUE_* replies not yet given the device */
    uint32_t life;        /* counts the port's removals */
    uint16_t cols, rows;
    int resize_due; /* a size to give the terminal from the thread */
    char *name;     /* PORT_NAME's, from the heap, or NULL */
    char vname[16]; /* vportNpM */
    struct virtqueue *rxq, *txq;
    struct vcon_buf rx[VCON_RX_BUFS];
    int nrx; /* the receive buffers made */
    /* The buffers the device returned whose bytes no reader has all
     * taken, oldest first: indexes into `rx`. */
    uint8_t rx_fifo[VCON_RX_BUFS];
    unsigned int rx_head, rx_n;
    /* The transmit side, under `vcon_tx_lock`: whether the port carries
     * the console's output, its buffers, the one being filled or -1, the
     * bytes in it, and whether the last wait reached its bound. */
    int out;
    struct vcon_buf tx[VCON_TX_BUFS];
    int tx_cur;
    uint32_t tx_fill;
    int tx_stuck;
    int tx_irq; /* the transmit queue asks for an interrupt */
    /* The panic's own buffer and the bytes in it: a panic that finds the
     * transmit side held fills this one, which the holder never touches. */
    struct vcon_buf ptx;
    uint32_t ptx_fill;
    /* The hvc terminal of a console port that is not the console's input:
     * the pair whose master the driver holds, its record on the master's
     * queue, and the output taken from it that no page had room for. */
    struct pty *tty;
    struct wq_waiter tty_wait;
    unsigned char *hold;
    uint32_t hold_n;
    /* A vport's readers, writers and pollers. */
    wait_queue_t wq;
    struct vcon_node vnode, hnode, lnode;
};

/*
 * A bound device. The ports' and the control queues' state is written
 * under the big lock by the boot, the thread, the removal and the shutdown;
 * the interrupt handlers, which run under the big lock too since the
 * driver does not ask for unlocked ones (`irq_unlocked`), only hand the
 * returned buffers on. `vcon_tx_lock` guards the transmit side, each port's
 * `out`, `tx`, `tx_cur`, `tx_fill` and `tx_stuck`, the output ports and
 * `vcon_in`, `vcon_only` and `vcon_early`, which a processor without the
 * big lock also uses when it writes a report, and is taken with interrupts
 * off. The console's lock (`console_lock`), which guards the receive ring
 * the line feeds, also guards each port's received buffers. `vcon_ctl_lock`
 * guards each device's list of control buffers to act on. A queue's lock is
 * taken inside any of them, and nothing else is.
 */
struct vcon {
    struct vcon *next;
    struct virtio_dev *vdev; /* NULL once the device is removed */
    int index;               /* the N of vportNpM */
    int multiport, emerg, size;
    /* The removal or the shutdown has begun: no port carries the console
     * again, no handler acts and no control message is sent. */
    int closed;
    uint32_t nports;
    struct vcon_port *port;
    struct virtqueue *ctl_rxq, *ctl_txq;
    struct vcon_buf *ctl_rx; /* a page each, `nctl` of them */
    int nctl;
    /* The control buffers the device returned that the thread has not
     * acted on, oldest first, and the bytes in each, under vcon_ctl_lock. */
    uint16_t *ctl_pend;
    uint32_t *ctl_len;
    int ctl_head, ctl_n;
    struct virtio_console_control *ctl_tx; /* VCON_CTL_TX of them */
    uint64_t ctl_tx_addr;
    volatile unsigned char ctl_tx_busy[VCON_CTL_TX];
    int owed;  /* ports with replies owed */
    int opens; /* vport descriptors open: the record outlives the device */
};

/* The bound devices, in the order bound, and the records of removed ones
 * that a descriptor holds. */
static struct vcon *vcon_list;

static struct spinlock vcon_tx_lock = SPINLOCK_INIT_NAMED("virtio_console");
static struct spinlock vcon_ctl_lock = SPINLOCK_INIT_NAMED("virtio_console_control");

/* The ports that carry the console's output, under `vcon_tx_lock`; the
 * port the console's input comes from; while a port is given the log's
 * ring, the one port output goes to; and a device whose `emerg_wr` carries
 * the console's output until the port the boot line names comes. */
static struct vcon_port *vcon_outs[VCON_WANT_MAX];
static int vcon_nouts;
static struct vcon_port *vcon_in;
static struct vcon_port *vcon_only;
static struct vcon *vcon_early;

static struct virtio_console_counters vcon_n;

/* The console ports by hvc number, and the ports by vport minor. */
static struct vcon_port *vcon_hvc[VCON_HVC_MAX];
static struct vcon_port *vcon_minor[VCON_MINORS];

/* The names of the hvc terminals, which a pair keeps for its life. */
static const char *const vcon_hvc_names[VCON_HVC_MAX] = {
    "hvc0", "hvc1", "hvc2",  "hvc3",  "hvc4",  "hvc5",  "hvc6",  "hvc7",
    "hvc8", "hvc9", "hvc10", "hvc11", "hvc12", "hvc13", "hvc14", "hvc15"};

/* The number, plus one, of the processor inside the transmit side, or 0: a
 * record that the transmit itself raises, as the core logs a device's
 * fault, finds it and stays in the log's ring. Only that processor writes
 * its own number. */
static volatile unsigned int vcon_tx_owner;

/* What the boot line asked for: the node names of the ports that are to
 * carry the console's output, in the order named, each once; the one the
 * console's input is to come from, the last named, or empty; and whether
 * the UART stays a console beside them. */
static char vcon_want[VCON_WANT_MAX][16];
static int vcon_want_n;
static char vcon_want_in[16];
static int vcon_want_uart;

/* The names of the boot line, a bit each, whose port has been given the
 * log's ring, once for the machine's life, whatever device carries it. */
static unsigned int vcon_replayed;

/* The port `vcon_attach_line` attaches, for `klog_replay`. */
static struct vcon_port *vcon_attaching;

/* The thread's queue, whether it has work, and the thread. */
static wait_queue_t vcon_wq;
static volatile int vcon_work;

static void vcon_out(char c);
static void vcon_flush(void);
static void vcon_rx_room(void);
static void vcon_rx_discard(void);

static const struct serial_line vcon_serial_line = {vcon_out, vcon_flush, vcon_rx_room,
                                                    vcon_rx_discard};

/** Whether `a` and `b` are the same string. */
static int vcon_same(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

/** Whether `s` begins with `prefix`. */
static int vcon_begins(const char *s, const char *prefix)
{
    while (*prefix && *s == *prefix) {
        s++;
        prefix++;
    }
    return *prefix == '\0';
}

/** Write `v` in decimal at `out`, which has room; the digits written. */
static int vcon_decimal(char *out, uint32_t v)
{
    char rev[10];
    int n = 0, i;

    do {
        rev[n++] = (char)('0' + v % 10u);
        v /= 10u;
    } while (v);
    for (i = 0; i < n; i++) {
        out[i] = rev[n - 1 - i];
    }
    return n;
}

/** Read a decimal number of at most four digits at `*s`, moving `*s` past
 * it; -1 when there is none. */
static int vcon_number(const char **s)
{
    int v = 0, n = 0;

    while (**s >= '0' && **s <= '9' && n < 4) {
        v = v * 10 + (**s - '0');
        (*s)++;
        n++;
    }
    return n ? v : -1;
}

/** Whether the boot line's name `want` names `p`: its `vportNpM`,
 * whatever its kind, or `hvcN` for a console port given N. */
static int vcon_names(const char *want, const struct vcon_port *p)
{
    if (want[0] == '\0' || !p->vname[0]) {
        return 0;
    }
    return vcon_same(want, p->vname) ||
           (p->console && p->hvc >= 0 && vcon_same(want, vcon_hvc_names[p->hvc]));
}

/** The name the log gives `p`: `hvcN` for a console port given N, else
 * its `vportNpM`, as Linux names their nodes. */
static const char *vcon_node_name(const struct vcon_port *p)
{
    return p->console && p->hvc >= 0 ? vcon_hvc_names[p->hvc] : p->vname;
}

/** The boot line's names that name `p`, a bit each. */
static unsigned int vcon_wanted(const struct vcon_port *p)
{
    unsigned int bits = 0;
    int i;

    for (i = 0; i < vcon_want_n; i++) {
        if (vcon_names(vcon_want[i], p)) {
            bits |= 1u << i;
        }
    }
    return bits;
}

/** Give console port `p` the lowest hvc number no other port has, as
 * Linux's hvc_alloc gives a console port its index once; it keeps it until
 * the port goes, so that a console port found later names no other. A
 * port past VCON_HVC_MAX has none, as Linux's hvc has no terminal past its
 * adapters. */
static void vcon_hvc_take(struct vcon_port *p)
{
    int n;

    if (p->hvc >= 0) {
        return;
    }
    for (n = 0; n < VCON_HVC_MAX; n++) {
        if (!vcon_hvc[n]) {
            vcon_hvc[n] = p;
            p->hvc = n;
            return;
        }
    }
    klog(KLOG_WARN, "virtio_console", "port: %s: %s has no hvc terminal: hvc0 to hvc%d are taken",
         p->dev->vdev->name, p->vname, VCON_HVC_MAX - 1);
}

/** The number of cycles `ms` milliseconds take at most. */
static uint64_t vcon_cycles(unsigned long ms)
{
    return (uint64_t)ms * VCON_CYCLES_PER_MS;
}

/** Tell the thread it has work, from any context. */
static void vcon_kick(void)
{
    vcon_work = 1;
    wq_wake(&vcon_wq, WQ_DATA);
}

/* ---- the transmit side ---- */

/** A transmit buffer came back: it is free again. From a reap, in the
 * transmit side or in the queue's interrupt. */
static void vcon_tx_done(struct virtqueue *vq, void *token, uint32_t len)
{
    (void)vq;
    (void)len;
    ((struct vcon_buf *)token)->busy = 0;
}

/** After a reap of a transmit queue that took buffers: a vport's writers
 * and the thread, for an hvc terminal, may go on. Nothing during a panic,
 * which wakes no one. */
static void vcon_tx_batch(struct virtqueue *vq)
{
    struct vcon_port *p = (struct vcon_port *)vq->priv;

    if (k_panicking()) {
        return;
    }
    if (p->tty) {
        vcon_kick();
    }
    if (p->guest_open) {
        wq_wake(&p->wq, WQ_ROOM);
    }
}

/** How many of `p`'s transmit buffers the device has. */
static int vcon_tx_busy(const struct vcon_port *p)
{
    int i, n = 0;

    for (i = 0; i < VCON_TX_BUFS; i++) {
        n += p->tx[i].busy != 0;
    }
    return n;
}

/**
 * Reap `p`'s transmit queue until the device has at most `most` of its
 * buffers and, with `room`, the queue has a free descriptor, waiting as
 * VCON_TX_WAIT_MS describes; `vcon_tx_lock` held. Returns 1 when it has,
 * 0 when the wait reached its bound, which `tx_waits` counts.
 */
static int vcon_tx_wait(struct vcon_port *p, int most, int room)
{
    uint64_t start = 0, limit = 0;
    int waiting = 0;

    for (;;) {
        (void)virtqueue_reap(p->txq);
        if (vcon_tx_busy(p) <= most && (!room || virtqueue_free_count(p->txq) > 0)) {
            p->tx_stuck = 0;
            return 1;
        }
        if (!waiting) {
            waiting = 1;
            limit = vcon_cycles(p->tx_stuck ? VCON_TX_STUCK_MS : VCON_TX_WAIT_MS);
            start = arch_cycles();
        } else if (arch_cycles() - start >= limit) {
            p->tx_stuck = 1;
            vcon_n.tx_waits++;
            return 0;
        }
        touch_nmi_watchdog();
        arch_relax();
    }
}

/**
 * Give `len` bytes of `b` to `p`'s device and tell it, first reaping what
 * it returned and, when every descriptor of the queue is in use, waiting
 * for one, as a queue smaller than the buffers needs; `vcon_tx_lock` held,
 * or a panic that holds every lock it needs. What finds no descriptor is
 * dropped and counted. Returns 1 when the device has the buffer.
 */
static int vcon_tx_give(struct vcon_port *p, struct vcon_buf *b, uint32_t len)
{
    struct virtio_sg sg;

    (void)virtqueue_reap(p->txq);
    if (virtqueue_free_count(p->txq) == 0 && !vcon_tx_wait(p, VCON_TX_BUFS, 1)) {
        vcon_n.tx_dropped += len;
        return 0;
    }
    sg.addr = b->addr;
    sg.len = len;
    b->busy = 1;
    if (virtqueue_add(p->txq, &sg, 1, 0, b) != 0) {
        b->busy = 0;
        vcon_n.tx_dropped += len;
        return 0;
    }
    vcon_n.tx_bytes += len;
    vcon_n.tx_buffers++;
    virtqueue_kick(p->txq);
    return 1;
}

/** Give the buffer being filled for `p` to the device; `vcon_tx_lock`
 * held. */
static void vcon_tx_submit(struct vcon_port *p)
{
    (void)vcon_tx_give(p, &p->tx[p->tx_cur], p->tx_fill);
    p->tx_cur = -1;
    p->tx_fill = 0;
}

/** Choose a free transmit buffer of `p` to fill, after a reap, and a wait
 * for the device when it has them all; `vcon_tx_lock` held. Returns 0 when
 * none came back. */
static int vcon_tx_take(struct vcon_port *p)
{
    int i;

    (void)virtqueue_reap(p->txq);
    if (vcon_tx_busy(p) == VCON_TX_BUFS && !vcon_tx_wait(p, VCON_TX_BUFS - 1, 0)) {
        return 0;
    }
    for (i = 0; i < VCON_TX_BUFS; i++) {
        if (!p->tx[i].busy) {
            p->tx_cur = i;
            p->tx_fill = 0;
            return 1;
        }
    }
    return 0;
}

/** Put `c` in the buffer being filled for `p`, giving the buffer to the
 * device when it is full; `vcon_tx_lock` held. */
static void vcon_tx_put(struct vcon_port *p, char c)
{
    if (p->tx_cur < 0 && !vcon_tx_take(p)) {
        vcon_n.tx_dropped++;
        return;
    }
    p->tx[p->tx_cur].data[p->tx_fill++] = (unsigned char)c;
    if (p->tx_fill == VCON_BUF) {
        vcon_tx_submit(p);
    }
}

/** The bytes `p`'s pages take now without a wait for the device, after a
 * reap; `vcon_tx_lock` held. */
static uint32_t vcon_tx_room(struct vcon_port *p)
{
    uint32_t room = 0;
    int i;

    if (!p->tx[0].data) {
        return 0;
    }
    (void)virtqueue_reap(p->txq);
    for (i = 0; i < VCON_TX_BUFS; i++) {
        if (i == p->tx_cur) {
            room += VCON_BUF - p->tx_fill;
        } else if (!p->tx[i].busy) {
            room += VCON_BUF;
        }
    }
    return room;
}

/** Put the `n` bytes at `bytes` in `p`'s pages, as many as they take now,
 * and give the device what they hold; `vcon_tx_lock` held. Returns the
 * bytes put. */
static uint32_t vcon_tx_write(struct vcon_port *p, const unsigned char *bytes, uint32_t n)
{
    uint32_t room = vcon_tx_room(p), i;

    if (n > room) {
        n = room;
    }
    for (i = 0; i < n; i++) {
        vcon_tx_put(p, (char)bytes[i]);
    }
    if (p->tx_cur >= 0 && p->tx_fill > 0) {
        vcon_tx_submit(p);
    }
    return n;
}

/**
 * Whether a panic may use `vq`: its lock is free. The panic stopped every
 * other processor first (`k_panic_begin`), so a lock free now stays free,
 * and one held belongs to a stopped processor or to the code the panic
 * interrupted, which may be halfway through the ring.
 */
static int vcon_vq_free(struct virtqueue *vq)
{
    if (!spin_trylock(&vq->lock)) {
        return 0;
    }
    spin_unlock(&vq->lock);
    return 1;
}

/**
 * Enter the transmit side: interrupts off and `vcon_tx_lock` taken, with
 * the flags to restore in `*flags`. Returns 0, with nothing taken, when
 * this processor is inside it already, and during a panic when the lock is
 * held, perhaps by a processor the panic stopped.
 */
static int vcon_tx_enter(uint32_t *flags)
{
    unsigned int me = (unsigned int)arch_cpu_id() + 1u;

    if (vcon_tx_owner == me) {
        return 0;
    }
    *flags = kirq_save();
    if (k_panicking()) {
        if (!spin_trylock(&vcon_tx_lock)) {
            kirq_restore(*flags);
            return 0;
        }
    } else {
        spin_lock(&vcon_tx_lock);
    }
    vcon_tx_owner = me;
    return 1;
}

/** Leave the transmit side `vcon_tx_enter` entered. */
static void vcon_tx_leave(uint32_t flags)
{
    vcon_tx_owner = 0;
    spin_unlock(&vcon_tx_lock);
    kirq_restore(flags);
}

/** Write `c` to `d`'s `emerg_wr`, which needs no queue (virtio 1.2,
 * 5.3.5) and which QEMU gives the first of its console ports whose host's
 * end is open; nothing for a device without it. */
static void vcon_emerg(struct vcon *d, char c)
{
    uint32_t v = (unsigned char)c;

    if (d && d->emerg && d->vdev && !d->closed) {
        virtio_config_write(d->vdev, VIRTIO_CONSOLE_CFG_EMERG_WR, &v, 4);
        vcon_n.emerg++;
    }
}

/** Write `c` to the `emerg_wr` of each device of the output ports that
 * `lost` marks, once a device. */
static void vcon_emerg_lost(const int *lost, char c)
{
    int i, k, seen;

    for (i = 0; i < vcon_nouts; i++) {
        if (!lost[i]) {
            continue;
        }
        for (seen = 0, k = 0; k < i; k++) {
            seen |= lost[k] && vcon_outs[k]->dev == vcon_outs[i]->dev;
        }
        if (!seen) {
            vcon_emerg(vcon_outs[i]->dev, c);
        }
    }
}

/** Wait, as a transmit waits, until the device has returned `p`'s panic
 * buffer; a panic that holds the queue's lock. Returns 0 when it has not. */
static int vcon_ptx_wait(struct vcon_port *p)
{
    uint64_t start = arch_cycles();
    uint64_t limit = vcon_cycles(p->tx_stuck ? VCON_TX_STUCK_MS : VCON_TX_WAIT_MS);

    while (p->ptx.busy) {
        (void)virtqueue_reap(p->txq);
        if (!p->ptx.busy) {
            break;
        }
        if (arch_cycles() - start >= limit) {
            p->tx_stuck = 1;
            vcon_n.tx_waits++;
            return 0;
        }
        touch_nmi_watchdog();
        arch_relax();
    }
    return 1;
}

/** Give `p`'s panic buffer, with what it holds, to the device and wait
 * until it has returned it, so that a reset that follows loses none. */
static void vcon_ptx_flush(struct vcon_port *p)
{
    if (p->ptx_fill == 0 || !vcon_ptx_wait(p)) {
        return;
    }
    vcon_n.tx_panic += p->ptx_fill;
    (void)vcon_tx_give(p, &p->ptx, p->ptx_fill);
    p->ptx_fill = 0;
    (void)vcon_ptx_wait(p);
}

/**
 * A panic's byte that found the transmit side held: it goes into each
 * output port's panic buffer, which no holder of the side uses, where the
 * port's queue is free (`vcon_vq_free`), and a full buffer to the device;
 * for a port whose queue is held, and for every port when the byte reached
 * none, it goes to the port's device's `emerg_wr`.
 */
static void vcon_panic_out(char c)
{
    int lost[VCON_WANT_MAX];
    int i, sent = 0;

    for (i = 0; i < vcon_nouts; i++) {
        struct vcon_port *p = vcon_outs[i];

        lost[i] = 0;
        if ((vcon_only && p != vcon_only) || !p->ptx.data) {
            continue;
        }
        if (!vcon_vq_free(p->txq)) {
            lost[i] = 1;
            continue;
        }
        if (p->ptx.busy && !vcon_ptx_wait(p)) {
            vcon_n.tx_dropped++;
            continue;
        }
        p->ptx.data[p->ptx_fill++] = (unsigned char)c;
        sent = 1;
        if (p->ptx_fill == VCON_BUF) {
            vcon_ptx_flush(p);
        }
    }
    for (i = 0; !sent && i < vcon_nouts; i++) {
        lost[i] = 1;
    }
    vcon_emerg_lost(lost, c);
}

/**
 * The line's `out`: put `c` in the buffer being filled for each port that
 * carries the console's output, or the one being given the log's ring, and
 * give a buffer to the device when it is full; and write it to the early
 * device's `emerg_wr` while the port the boot line names has not come. A
 * byte that finds no port, or no buffer after the wait for one, is dropped
 * and counted. During a panic, a port whose queue's lock is held is not
 * touched; a byte that so reaches no port, or that finds the transmit side
 * itself held, goes the panic's way (`vcon_panic_out`).
 */
static void vcon_out(char c)
{
    int lost[VCON_WANT_MAX];
    uint32_t flags;
    int i, sent = 0, nlost = 0;

    if (!vcon_tx_enter(&flags)) {
        if (k_panicking()) {
            vcon_panic_out(c);
        }
        return;
    }
    for (i = 0; i < vcon_nouts; i++) {
        struct vcon_port *p = vcon_outs[i];

        lost[i] = 0;
        if (vcon_only && p != vcon_only) {
            continue;
        }
        if (k_panicking() && !vcon_vq_free(p->txq)) {
            lost[i] = 1;
            nlost++;
            continue;
        }
        vcon_tx_put(p, c);
        sent = 1;
    }
    if (vcon_early && !vcon_only) {
        vcon_emerg(vcon_early, c);
        sent = 1;
    }
    if (!sent && !nlost) {
        vcon_n.tx_dropped++;
    }
    if (nlost) {
        vcon_emerg_lost(lost, c);
    }
    vcon_tx_leave(flags);
}

/**
 * The line's `flush`: give the device what `vcon_out` took for each output
 * port and wait until it has returned every buffer, as Linux's put_chars
 * waits for the host before it returns (`__send_to_port`), so that a reset
 * or a power off that follows the last byte loses none. During a panic it
 * also sends what the panic's own buffers hold, from any port whose queue
 * is free.
 */
static void vcon_flush(void)
{
    uint32_t flags;
    int i, in = vcon_tx_enter(&flags);

    if (!in && !k_panicking()) {
        return;
    }
    for (i = 0; i < vcon_nouts; i++) {
        struct vcon_port *p = vcon_outs[i];

        if (k_panicking() && !vcon_vq_free(p->txq)) {
            continue;
        }
        if (in) {
            if (p->tx_cur >= 0 && p->tx_fill > 0) {
                vcon_tx_submit(p);
            }
            if (vcon_tx_busy(p) > 0) {
                (void)vcon_tx_wait(p, 0, 0);
            }
        }
        if (k_panicking()) {
            vcon_ptx_flush(p);
        }
    }
    if (in) {
        vcon_tx_leave(flags);
    }
}

/* ---- the receive side ---- */

/** Give `b` of `p` back to the device to fill; the console's lock held.
 * The caller kicks. */
static void vcon_rx_queue(struct vcon_port *p, struct vcon_buf *b)
{
    struct virtio_sg sg;

    sg.addr = b->addr;
    sg.len = VCON_BUF;
    b->len = b->off = 0;
    (void)virtqueue_add(p->rxq, &sg, 0, 1, b);
}

/** The oldest buffer `p`'s device returned that a reader has not emptied,
 * or NULL; the console's lock held. */
static struct vcon_buf *vcon_rx_head(struct vcon_port *p)
{
    return p->rx_n ? &p->rx[p->rx_fifo[p->rx_head]] : NULL;
}

/** Take the first `n` bytes of `p`'s oldest buffer, giving it back to the
 * device once they were its last; the console's lock held. Returns 1 when
 * the device was given it, for the caller to kick. */
static int vcon_rx_took(struct vcon_port *p, uint32_t n)
{
    struct vcon_buf *b = vcon_rx_head(p);

    b->off += n;
    if (b->off < b->len) {
        return 0;
    }
    p->rx_head = (p->rx_head + 1u) % VCON_RX_BUFS;
    p->rx_n--;
    vcon_rx_queue(p, b);
    return 1;
}

/**
 * Move the bytes of `p`'s returned buffers into the console's ring, oldest
 * first, as far as the ring has room, and give each emptied buffer back to
 * the device; the console's lock held. A buffer the ring cannot take whole
 * stays first in the list, and the device, left without buffers, stops
 * reading its end, which is what holds a sender back. Returns the bytes
 * moved.
 */
static uint32_t vcon_rx_drain(struct vcon_port *p)
{
    uint32_t moved = 0;
    int kick = 0;

    while (p->rx_n > 0) {
        struct vcon_buf *b = vcon_rx_head(p);
        int n = serial_rx_feed(b->data + b->off, (int)(b->len - b->off));

        moved += (uint32_t)n;
        if (b->off + (uint32_t)n < b->len) {
            b->off += (uint32_t)n;
            vcon_n.rx_held++;
            break;
        }
        kick |= vcon_rx_took(p, (uint32_t)n);
    }
    if (kick) {
        virtqueue_kick(p->rxq);
    }
    vcon_n.rx_bytes += moved;
    return moved;
}

/**
 * The device returned a receive buffer of a port with `len` bytes, from
 * the port's interrupt, under the big lock. The console's input port's go
 * to the console's ring, and the console is then served as the UART's
 * interrupt serves it: a parked reader is woken, or the ring is moved into
 * the discipline, in passes while a pass takes bytes. An hvc terminal's and
 * an open vport's are kept for the thread and for the vport's reader, who
 * are told. Another port has its bytes dropped and counted, as Linux
 * discards what comes to a port nobody has open. Once the device is closed
 * the buffer stays with the driver, which frees it after the reset.
 */
static void vcon_rx_done(struct virtqueue *vq, void *token, uint32_t len)
{
    struct vcon_port *p = (struct vcon_port *)vq->priv;
    struct vcon_buf *b = (struct vcon_buf *)token;
    uint32_t flags, moved = 0;
    int pass, kept = 0;

    if (p->dev->closed) {
        return;
    }
    flags = console_lock();
    if (p == vcon_in || p->tty || p->guest_open) {
        b->len = len;
        b->off = 0;
        p->rx_fifo[(p->rx_head + p->rx_n) % VCON_RX_BUFS] = (uint8_t)(b - p->rx);
        p->rx_n++;
        if (p == vcon_in) {
            moved = vcon_rx_drain(p);
        } else {
            kept = p->tty ? 1 : 2;
        }
    } else {
        vcon_n.rx_dropped += len;
        vcon_rx_queue(p, b);
        virtqueue_kick(p->rxq);
    }
    console_unlock(flags);
    if (kept == 1) {
        vcon_kick();
    } else if (kept == 2) {
        wq_wake(&p->wq, WQ_DATA);
    }
    for (pass = 0; moved > 0 && pass < VCON_RX_PASSES; pass++) {
        uint32_t before = serial_rx_pending();

        if (before == 0) {
            break;
        }
        serial_rx_fed();
        if (serial_rx_pending() >= before) {
            break;
        }
    }
}

/** The line's `rx_room`: the console's ring has room again for what the
 * input port's returned buffers still hold. */
static void vcon_rx_room(void)
{
    uint32_t flags = console_lock();

    if (vcon_in) {
        (void)vcon_rx_drain(vcon_in);
    }
    console_unlock(flags);
}

/** Drop what `p`'s returned buffers still hold and give them back to the
 * device. */
static void vcon_rx_drop(struct vcon_port *p)
{
    uint32_t flags = console_lock();

    if (p->rx_n > 0) {
        while (p->rx_n > 0) {
            vcon_rx_queue(p, &p->rx[p->rx_fifo[p->rx_head]]);
            p->rx_head = (p->rx_head + 1u) % VCON_RX_BUFS;
            p->rx_n--;
        }
        virtqueue_kick(p->rxq);
    }
    console_unlock(flags);
}

/** The line's `rx_discard`: a flush of the console's input reaches the
 * bytes the input port's returned buffers still hold. */
static void vcon_rx_discard(void)
{
    if (vcon_in) {
        vcon_rx_drop(vcon_in);
    }
}

/**
 * Copy up to `n` of the bytes `p`'s oldest returned buffer holds into
 * `out`, leaving them there; the caller takes them with `vcon_rx_consume`
 * once it has used them. Only one taker, the thread or the vport's reader,
 * empties a port's buffers, and the interrupt only adds to them. Returns
 * the bytes copied, 0 when no buffer holds any.
 */
static uint32_t vcon_rx_peek(struct vcon_port *p, unsigned char *out, uint32_t n)
{
    uint32_t flags = console_lock();
    struct vcon_buf *b = vcon_rx_head(p);

    if (!b) {
        n = 0;
    } else {
        if (n > b->len - b->off) {
            n = b->len - b->off;
        }
        k_memcpy(out, b->data + b->off, n);
    }
    console_unlock(flags);
    return n;
}

/** Take `n` bytes `vcon_rx_peek` gave from `p`'s oldest buffer, unless a
 * drop took the buffer meanwhile. */
static void vcon_rx_consume(struct vcon_port *p, uint32_t n)
{
    uint32_t flags = console_lock();

    if (n > 0 && vcon_rx_head(p) && vcon_rx_took(p, n)) {
        virtqueue_kick(p->rxq);
    }
    console_unlock(flags);
}

/* ---- the control queues ---- */

/** A control message came back: its slot is free again. */
static void vcon_ctl_sent(struct virtqueue *vq, void *token, uint32_t len)
{
    struct vcon *d = (struct vcon *)vq->priv;

    (void)len;
    d->ctl_tx_busy[(struct virtio_console_control *)token - d->ctl_tx] = 0;
}

/**
 * Send the control message `event` with `value` for port `id` of `d`, and
 * wait until the device has taken it, at most VCON_CTL_WAIT_MS, as Linux's
 * __send_control_msg waits; under the big lock, in process context or the
 * boot's wait. The queue is reaped first, so that a message the device
 * took after its own wait ended frees its slot, since the transmit queue
 * asks for no interrupt; a message not taken in its wait stays in the
 * queue, in its slot. Returns 0, at once without F_MULTIPORT, or -EBUSY
 * when every slot is still the device's, or the queue's error, with
 * nothing sent.
 */
static int vcon_ctl_send(struct vcon *d, uint32_t id, uint16_t event, uint16_t value)
{
    struct virtio_console_control *m;
    struct virtio_sg sg;
    uint64_t start, limit = vcon_cycles(VCON_CTL_WAIT_MS);
    int i, r;

    if (!d->multiport || !d->ctl_txq) {
        return 0;
    }
    if (d->closed) {
        return -EIO;
    }
    (void)virtqueue_reap(d->ctl_txq);
    for (i = 0; i < VCON_CTL_TX && d->ctl_tx_busy[i]; i++) {
    }
    if (i == VCON_CTL_TX) {
        klog(KLOG_WARN, "virtio_console", "control: %s: event %d not sent: the device holds %d",
             d->vdev->name, (int)event, VCON_CTL_TX);
        return -EBUSY;
    }
    m = &d->ctl_tx[i];
    m->id = id;
    m->event = event;
    m->value = value;
    sg.addr = d->ctl_tx_addr + (uint64_t)i * sizeof(*m);
    sg.len = (uint32_t)sizeof(*m);
    d->ctl_tx_busy[i] = 1;
    r = virtqueue_add(d->ctl_txq, &sg, 1, 0, m);
    if (r != 0) {
        d->ctl_tx_busy[i] = 0;
        return r;
    }
    vcon_n.control_out++;
    virtqueue_kick(d->ctl_txq);
    start = arch_cycles();
    while (d->ctl_tx_busy[i]) {
        (void)virtqueue_reap(d->ctl_txq);
        if (!d->ctl_tx_busy[i]) {
            break;
        }
        if (arch_cycles() - start >= limit) {
            klog(KLOG_WARN, "virtio_console", "control: %s: event %d not taken in %d ms",
                 d->vdev->name, (int)event, (int)VCON_CTL_WAIT_MS);
            break;
        }
        arch_relax();
    }
    return 0;
}

/** Whether the guest's end of `p` is open: always for a console port, as
 * Linux opens it for its hvc, and for another while it carries the console
 * or a descriptor of its node is open. */
static int vcon_guest_side(const struct vcon_port *p)
{
    return p->console || p->out || p->guest_open;
}

/** Give the device the replies `p` is owed (`ctl_due`), PORT_READY before
 * PORT_OPEN; one that cannot be sent stays owed, for the next control
 * message the device sends. */
static void vcon_ctl_owed(struct vcon_port *p)
{
    struct vcon *d = p->dev;
    int side;

    if ((p->ctl_due & VCON_DUE_READY) &&
        vcon_ctl_send(d, p->id, VIRTIO_CONSOLE_PORT_READY, 1) == 0) {
        p->ctl_due &= ~VCON_DUE_READY;
    }
    side = vcon_guest_side(p);
    if ((p->ctl_due & VCON_DUE_OPEN) && !(p->ctl_due & VCON_DUE_READY)) {
        if (side == p->sent_open ||
            vcon_ctl_send(d, p->id, VIRTIO_CONSOLE_PORT_OPEN, (uint16_t)side) == 0) {
            p->sent_open = side;
            p->ctl_due &= ~VCON_DUE_OPEN;
        }
    }
    if (!p->ctl_due) {
        d->owed--;
    }
}

/** Owe `p` the reply `due`, VCON_DUE_*, and give it now if the device
 * takes it. */
static void vcon_ctl_reply(struct vcon_port *p, unsigned int due)
{
    if (!p->ctl_due) {
        p->dev->owed++;
    }
    p->ctl_due |= due;
    vcon_ctl_owed(p);
}

/** Set the size of `p`'s terminal, the console terminal while `p` is the
 * console's input or its hvc terminal, to the size the device gave, as
 * Linux's hvc_resize; process context. */
static void vcon_resize(struct vcon_port *p)
{
    struct tty_winsize ws;
    struct tty *t = p == vcon_in ? &console_tty : p->tty ? &p->tty->slave : NULL;

    p->resize_due = 0;
    if (!t || p->cols == 0 || p->rows == 0) {
        return;
    }
    ws = t->winsize;
    ws.ws_col = p->cols;
    ws.ws_row = p->rows;
    tty_resize(t, &ws);
}

/** Make the console's line match the ports that carry its output and the
 * early device (`serial_line_attach`): attached while any does, with the
 * input from `vcon_in` or the UART, and the UART kept as a console when the
 * boot line names it or while only the early device writes. Under the big
 * lock with interrupts off. */
static void vcon_line_update(void)
{
    if (vcon_nouts > 0) {
        serial_line_attach(&vcon_serial_line, vcon_in != 0, vcon_want_uart);
    } else if (vcon_early) {
        serial_line_attach(&vcon_serial_line, 0, 1);
    } else {
        serial_line_detach(&vcon_serial_line);
    }
}

/** Make `vcon_attaching` carry the console's output, and its input when
 * the boot line names it last, and attach the line to the console; the
 * input stays the UART's while no port has it. The early device writes no
 * more once a port of the boot line carries the log. Under the big lock
 * with interrupts off. */
static void vcon_attach_line(void)
{
    struct vcon_port *p = vcon_attaching;
    uint32_t flags = spin_lock_irqsave(&vcon_tx_lock);

    p->tx_cur = -1;
    p->tx_fill = 0;
    p->tx_stuck = 0;
    p->out = 1;
    if (vcon_nouts < VCON_WANT_MAX) {
        vcon_outs[vcon_nouts++] = p;
    }
    if (vcon_names(vcon_want_in, p)) {
        vcon_in = p;
    }
    vcon_early = 0;
    spin_unlock_irqrestore(&vcon_tx_lock, flags);
    vcon_line_update();
}

static void vcon_settle(struct vcon_port *p);

/**
 * Make `p` carry the console: its receive buffers given to the device, the
 * device told the port is open, and the port given the console's output,
 * and its input when the boot line names it last (`vcon_attach_line`). On a
 * machine without a UART a port that has not had it is then given every
 * record of the log's ring, as Linux's register_console gives a console
 * that no boot console preceded the buffer, whenever the port comes;
 * `klog_replay` attaches it inside its replay, so that each record reaches
 * the port once and in order, and meanwhile output goes to that port
 * alone. With a UART the records so far went there, and the port takes the
 * log from here on. Its nodes then follow what it is (`vcon_settle`).
 * Under the big lock. Returns 0, or -EIO once the device is closed.
 */
static int vcon_attach(struct vcon_port *p)
{
    unsigned int names = vcon_wanted(p);
    uint32_t irq, flags;
    int input = vcon_names(vcon_want_in, p);

    if (p->dev->closed) {
        return -EIO;
    }
    irq = kirq_save();
    klog(KLOG_INFO, "console", "enabled: %s, port %d of %s%s", vcon_node_name(p), (int)p->id,
         p->dev->vdev->name, input ? "" : ", for output alone");
    klog_flush();
    vcon_attaching = p;
    if (!(vcon_replayed & names) && !serial_uart_found()) {
        vcon_replayed |= names;
        flags = spin_lock_irqsave(&vcon_tx_lock);
        vcon_only = p;
        spin_unlock_irqrestore(&vcon_tx_lock, flags);
        klog_replay(vcon_attach_line);
        flags = spin_lock_irqsave(&vcon_tx_lock);
        vcon_only = 0;
        spin_unlock_irqrestore(&vcon_tx_lock, flags);
    } else {
        vcon_replayed |= names;
        vcon_attach_line();
    }
    vcon_attaching = 0;
    kirq_restore(irq);
    vcon_ctl_reply(p, VCON_DUE_OPEN);
    p->resize_due = 1;
    vcon_settle(p);
    return 0;
}

/**
 * Take `p` off the console, since it has gone, its device is being removed
 * or the system stops. When it had the console's input, the bytes it gave
 * the ring and those its buffers hold are dropped first, and with `hang`
 * the console terminal is hung up, as Linux's hvc_remove hangs its tty up,
 * so that a reader of its node is woken and its session told; the input
 * goes back to the UART. The line is detached when no port carries the
 * output. Under the big lock.
 */
static void vcon_detach(struct vcon_port *p, const char *why, int hang)
{
    uint32_t irq, flags;
    int i, k, was_in = p == vcon_in;

    if (!p->out) {
        return;
    }
    irq = kirq_save();
    if (was_in) {
        serial_rx_flush();
    }
    flags = spin_lock_irqsave(&vcon_tx_lock);
    p->out = 0;
    p->tx_cur = -1;
    p->tx_fill = 0;
    for (i = k = 0; i < vcon_nouts; i++) {
        if (vcon_outs[i] != p) {
            vcon_outs[k++] = vcon_outs[i];
        }
    }
    vcon_nouts = k;
    if (was_in) {
        vcon_in = 0;
    }
    spin_unlock_irqrestore(&vcon_tx_lock, flags);
    vcon_line_update();
    kirq_restore(irq);
    vcon_rx_drop(p);
    klog(KLOG_INFO, "console", "disabled: %s: %s", vcon_node_name(p), why);
    if (was_in && hang) {
        tty_console_hangup_later();
    }
}

/** Make each port the boot line names carry the console, when its device
 * has it and its kind is known. */
static void vcon_ports_try(void)
{
    struct vcon *d;
    uint32_t k;

    for (d = vcon_list; d; d = d->next) {
        for (k = 0; !d->closed && k < d->nports; k++) {
            struct vcon_port *p = &d->port[k];

            if (p->present && p->rxq && p->nrx && !p->out && vcon_wanted(p)) {
                (void)vcon_attach(p);
            }
        }
    }
}

/* ---- the nodes ---- */

/* A vport descriptor's record: its port, and the port's `life` at the
 * open. */
struct vcon_open {
    struct vcon_port *p;
    uint32_t life;
};

/** Whether `h` still names its port: the port has not gone since the
 * open. */
static int vcon_live(const struct vcon_open *h)
{
    return h->p->present && h->p->life == h->life && !h->p->dev->closed;
}

/** The wait condition of a vport's read: bytes came, the host's end
 * closed, or the port went. */
static int vcon_readable(void *arg)
{
    const struct vcon_open *h = (const struct vcon_open *)arg;

    return !vcon_live(h) || h->p->rx_n > 0 || !h->p->host_open;
}

/** The wait condition of a vport's write: the host's end is open and a page
 * is free, or the port went. */
static int vcon_writable(void *arg)
{
    const struct vcon_open *h = (const struct vcon_open *)arg;
    const struct vcon_port *p = h->p;

    return !vcon_live(h) || (p->host_open && (vcon_tx_busy(p) < VCON_TX_BUFS ||
                                              (p->tx_cur >= 0 && p->tx_fill < VCON_BUF)));
}

/** Whether `e` was opened for no wait. */
static int vcon_nonblock(const fd_entry_t *e)
{
    return e->ofd && (e->ofd->status_flags & OFD_NONBLOCK);
}

/**
 * A vport's read, as Linux's port_fops_read: the bytes the host wrote, in
 * the order the device returned its buffers; 0, as an end of file, while
 * none wait and the host's end is closed; else a wait for them, or -EAGAIN
 * without one. -ENODEV once the port has gone.
 */
static int32_t vcon_vport_read(fd_entry_t *e, char __user *buf, uint32_t count)
{
    struct vcon_open *h = (struct vcon_open *)e->priv;
    struct vcon_port *p = h->p;
    unsigned char tmp[512];
    uint32_t done = 0, n;

    for (;;) {
        if (!vcon_live(h)) {
            return -ENODEV;
        }
        if (p->rx_n > 0 || count == 0) {
            break;
        }
        if (!p->host_open) {
            return 0;
        }
        if (vcon_nonblock(e)) {
            return -EAGAIN;
        }
        if (wq_wait_event(&p->wq, vcon_readable, h) < 0) {
            return -EINTR;
        }
    }
    while (done < count) {
        n = vcon_rx_peek(p, tmp, count - done < sizeof(tmp) ? count - done : sizeof(tmp));
        if (n == 0) {
            break;
        }
        if (copy_to_user(buf + done, tmp, n) != 0) {
            return done ? (int32_t)done : -EFAULT;
        }
        vcon_rx_consume(p, n);
        vcon_n.rx_bytes += n;
        done += n;
    }
    return (int32_t)done;
}

/**
 * A vport's write, as Linux's port_fops_write: the bytes go to the port's
 * pages and the device at once. It waits while the host's end is closed or
 * every page is the device's, or fails with -EAGAIN without a wait, and
 * returns short when the pages fill after it put some. -ENODEV once the
 * port has gone.
 */
static int32_t vcon_vport_write(fd_entry_t *e, const char __user *buf, uint32_t count)
{
    struct vcon_open *h = (struct vcon_open *)e->priv;
    struct vcon_port *p = h->p;
    unsigned char tmp[512];
    uint32_t done = 0, n, put, flags;

    while (done < count) {
        if (!vcon_live(h)) {
            return done ? (int32_t)done : -ENODEV;
        }
        if (!vcon_writable(h)) {
            if (done) {
                break;
            }
            if (vcon_nonblock(e)) {
                return -EAGAIN;
            }
            if (wq_wait_event(&p->wq, vcon_writable, h) < 0) {
                return -EINTR;
            }
            continue;
        }
        n = count - done < sizeof(tmp) ? count - done : sizeof(tmp);
        if (copy_from_user(tmp, buf + done, n) != 0) {
            return done ? (int32_t)done : -EFAULT;
        }
        put = 0;
        if (vcon_tx_enter(&flags)) {
            put = vcon_tx_write(p, tmp, n);
            vcon_tx_leave(flags);
        }
        done += put;
        if (put < n) {
            break;
        }
    }
    return (int32_t)done;
}

/** A vport's poll, as Linux's port_fops_poll: readable with bytes waiting
 * or the host's end closed, writable with that end open and a page free,
 * POLLHUP while that end is closed or once the port has gone. */
static short vcon_vport_poll(fd_entry_t *e, short events, struct wait_queue **wq)
{
    struct vcon_open *h = (struct vcon_open *)e->priv;
    short ready = 0;

    *wq = &h->p->wq;
    if (!vcon_live(h)) {
        return POLLHUP;
    }
    if (h->p->rx_n > 0 || !h->p->host_open) {
        ready |= POLLIN | POLLRDNORM;
    }
    if (vcon_writable(h)) {
        ready |= POLLOUT | POLLWRNORM;
    }
    return (short)((events & ready) | (h->p->host_open ? 0 : POLLHUP));
}

static void vcon_free(struct vcon *d);

/** The last close of a vport's description, as Linux's port_fops_release:
 * the device is told the guest's end closed, unless the console keeps it,
 * and what came for the reader is dropped. */
static void vcon_vport_close(fd_entry_t *e)
{
    struct vcon_open *h = (struct vcon_open *)e->priv;
    struct vcon *d;

    if (!h) {
        return;
    }
    d = h->p->dev;
    if (vcon_live(h)) {
        h->p->guest_open = 0;
        vcon_rx_drop(h->p);
        vcon_ctl_reply(h->p, VCON_DUE_OPEN);
        vcon_settle(h->p);
    }
    e->priv = NULL;
    kfree(h);
    if (--d->opens == 0 && !d->vdev) {
        vcon_free(d);
    }
}

static const struct file_ops vcon_vport_ops = {
    vcon_vport_read, vcon_vport_write, vcon_vport_poll, NULL, vcon_vport_close, NULL, NULL, 0};

/**
 * The open of a vport's node, as Linux's port_fops_open: -ENXIO for a
 * console port, whose terminal is its hvc node, and for a port that has
 * gone; -EBUSY while another description has it open; else the device is
 * told the guest's end opened, which QEMU waits for before it gives the
 * port input. -ENOMEM without a record.
 */
static int32_t vcon_vport_open(struct devfs_node *node, struct fd_entry *e, int flags)
{
    struct vcon_port *p = (struct vcon_port *)node->priv;
    struct vcon_open *h;

    (void)flags;
    if (!p->present || p->dev->closed || p->console) {
        return -ENXIO;
    }
    if (p->guest_open) {
        return -EBUSY;
    }
    h = (struct vcon_open *)kmalloc(sizeof(*h));
    if (!h) {
        return -ENOMEM;
    }
    h->p = p;
    h->life = p->life;
    p->guest_open = 1;
    p->dev->opens++;
    e->priv = h;
    vcon_settle(p);
    vcon_ctl_reply(p, VCON_DUE_OPEN);
    return 0;
}

/** The open of an hvc node: the slave of the port's terminal, -ENXIO once
 * the terminal has gone. */
static int32_t vcon_hvc_open(struct devfs_node *node, struct fd_entry *e, int flags)
{
    struct vcon_port *p = (struct vcon_port *)node->priv;

    return p->tty ? pty_kernel_slave_open(p->tty, e, flags) : -ENXIO;
}

/** The record of a wake of an hvc terminal's master queue: its slave has
 * output, or room for input again, and the thread moves it. Under the
 * queue's lock. */
static int vcon_tty_wake(struct wq_waiter *w, uint32_t kinds, unsigned int nest, uint64_t *cpus)
{
    (void)w;
    if (kinds & (WQ_DATA | WQ_ROOM)) {
        vcon_work = 1;
        wq_wake_nested(&vcon_wq, WQ_DATA, nest + 1u, cpus);
    }
    return 0;
}

/** Make `node` of `p` registered as `want` says, or not when its name is
 * NULL; a node registered with another name, minor or operations is
 * registered again. A refusal is logged. */
static void vcon_node_set(struct vcon_port *p, struct vcon_node *node,
                          const struct devfs_node *want)
{
    int32_t r;

    if (node->on && (!want->name || !vcon_same(want->name, node->name) ||
                     node->n.ops != want->ops || node->n.minor != want->minor)) {
        devfs_node_unregister(&node->n);
        node->on = 0;
    }
    if (!want->name || node->on) {
        return;
    }
    node->n = *want;
    k_strncpy(node->name, want->name, sizeof(node->name) - 1);
    node->name[sizeof(node->name) - 1] = '\0';
    node->n.name = node->name;
    node->n.open = want->ops ? want->open : NULL;
    node->n.priv = p;
    r = devfs_node_register(&node->n);
    if (r != 0) {
        klog(KLOG_WARN, "virtio_console", "port: %s: /dev/%s not made: error %d",
             p->dev->vdev ? p->dev->vdev->name : "virtio", want->name, (int)r);
        return;
    }
    node->on = 1;
}

/** Make `p`'s hvc terminal, the pair whose master the driver holds. */
static void vcon_tty_open(struct vcon_port *p)
{
    uint32_t dev = ((uint32_t)VIRTIO_CONSOLE_HVC_MAJOR << 8) | (uint32_t)p->hvc;

    p->hold = (unsigned char *)kmalloc(VCON_HOLD);
    p->tty = p->hold ? pty_kernel_make(dev, vcon_hvc_names[p->hvc]) : NULL;
    if (!p->tty) {
        kfree(p->hold);
        p->hold = 0;
        klog(KLOG_WARN, "virtio_console", "port: %s: no memory for %s's terminal",
             p->dev->vdev->name, vcon_hvc_names[p->hvc]);
        return;
    }
    p->hold_n = 0;
    k_memset(&p->tty_wait, 0, sizeof(p->tty_wait));
    p->tty_wait.func = vcon_tty_wake;
    wq_entry_add(&p->tty->m_read_wq, &p->tty_wait);
    p->resize_due = 1;
    vcon_kick();
}

/** Let go of `p`'s hvc terminal, as Linux's hvc_remove: its session is
 * hung up, and the pair goes once its last slave description closes. */
static void vcon_tty_close(struct vcon_port *p)
{
    struct pty *t = p->tty;

    wq_entry_remove(&p->tty_wait);
    p->tty = 0;
    vcon_rx_drop(p);
    pty_kernel_close(t);
    kfree(p->hold);
    p->hold = 0;
    p->hold_n = 0;
}

/** Write `p`'s name below /dev/virtio-ports into `link`, which has
 * DEVFS_NODE_NAME_MAX bytes; NULL when it has none or one too long. */
static const char *vcon_link_name(const struct vcon_port *p, char *link)
{
    if (!p->name || (int)k_strlen(p->name) > VCON_NAME_MAX) {
        return NULL;
    }
    k_memcpy(link, VCON_LINK_DIR, sizeof(VCON_LINK_DIR) - 1u);
    k_memcpy(link + sizeof(VCON_LINK_DIR) - 1u, p->name, k_strlen(p->name) + 1u);
    return link;
}

/**
 * Make `p`'s nodes follow what it is now, as Linux's add_port and
 * unplug_port make and remove them: while it is present, /dev/vportNpM,
 * which opens the console terminal while a generic port is the console's
 * input and is the byte device otherwise, and with a name
 * /dev/virtio-ports/NAME, a second name of it; for a console port given N,
 * /dev/hvcN, the console terminal while the port is the console's input
 * and the port's own hvc terminal otherwise. `live` and `term` say whether
 * the port is present and whether it is the console's input.
 */
static void vcon_settle_nodes(struct vcon_port *p, int live, int term)
{
    char link[DEVFS_NODE_NAME_MAX];
    struct devfs_node w;

    k_memset(&w, 0, sizeof(w));
    w.name = live && p->minor >= 0 ? p->vname : NULL;
    w.major = VIRTIO_CONSOLE_VPORT_MAJOR;
    w.minor = (uint16_t)(p->minor < 0 ? 0 : p->minor);
    w.ops = term && !p->console ? NULL : &vcon_vport_ops;
    w.mode = w.ops ? 0600 : 0620;
    w.open = vcon_vport_open;
    vcon_node_set(p, &p->vnode, &w);
    w.name = w.name ? vcon_link_name(p, link) : NULL;
    w.alias = 1;
    vcon_node_set(p, &p->lnode, &w);
    w.name = live && p->console && p->hvc >= 0 && (term || p->tty) ? vcon_hvc_names[p->hvc] : NULL;
    w.major = VIRTIO_CONSOLE_HVC_MAJOR;
    w.minor = (uint16_t)(p->hvc < 0 ? 0 : p->hvc);
    w.mode = 0620;
    w.ops = term ? NULL : pty_slave_ops();
    w.open = vcon_hvc_open;
    w.alias = 0;
    vcon_node_set(p, &p->hnode, &w);
}

/**
 * Make `p`'s nodes, its terminal and its transmit interrupt follow what it
 * is now (`vcon_settle_nodes`): a console port that is not the console's
 * input has an hvc terminal of its own, as Linux's init_port_console gives
 * it one, and the transmit queue asks for an interrupt while a writer may
 * wait for it. Under the big lock, in process context or the boot's wait.
 */
static void vcon_settle(struct vcon_port *p)
{
    struct devfs_node none;
    int term = p == vcon_in, live = p->present && !p->dev->closed;
    int want_tty = live && p->console && p->hvc >= 0 && !term;
    int irq;

    if (p->tty && !want_tty) {
        k_memset(&none, 0, sizeof(none));
        vcon_node_set(p, &p->hnode, &none);
        vcon_tty_close(p);
    } else if (!p->tty && want_tty) {
        vcon_tty_open(p);
    }
    vcon_settle_nodes(p, live, term);
    irq = live && (p->tty != 0 || p->guest_open);
    if (p->txq && irq != p->tx_irq) {
        (void)virtqueue_irq(p->txq, irq);
        p->tx_irq = irq;
    }
}

/* ---- the hvc terminals ---- */

/**
 * Move `p`'s bytes between its port and its hvc terminal, as Linux's
 * khvcd polls an hvc: what the host wrote is fed to the slave's discipline
 * as far as it takes it, the rest waiting for the slave's reader; and the
 * slave's output and echo go to the port's pages as far as they take them,
 * the rest waiting for the device to return a page. The thread's.
 */
static void vcon_hvc_pump(struct vcon_port *p)
{
    unsigned char tmp[256];
    uint32_t n, fed, put, flags;

    if (p->resize_due) {
        vcon_resize(p);
    }
    for (;;) {
        n = vcon_rx_peek(p, tmp, sizeof(tmp));
        if (n == 0) {
            break;
        }
        fed = pty_kernel_feed(p->tty, tmp, n);
        vcon_rx_consume(p, fed);
        vcon_n.rx_bytes += fed;
        if (fed < n) {
            vcon_n.rx_held++;
            break;
        }
    }
    for (;;) {
        if (p->hold_n == 0) {
            p->hold_n = pty_kernel_take(p->tty, p->hold, VCON_HOLD);
        }
        if (p->hold_n == 0 || !vcon_tx_enter(&flags)) {
            break;
        }
        put = vcon_tx_write(p, p->hold, p->hold_n);
        vcon_tx_leave(flags);
        if (put == 0) {
            break;
        }
        k_memmove(p->hold, p->hold + put, p->hold_n - put);
        p->hold_n -= put;
    }
}

/* ---- the ports ---- */

/** Make `p`'s pages: its receive buffers, given to the device, its
 * transmit buffers and its panic buffer, once for the device's life.
 * -ENOMEM, with what was made kept for the device's reset. */
static int vcon_port_bufs(struct vcon_port *p)
{
    struct virtio_dev *v = p->dev->vdev;
    uint32_t flags;
    int i, want = VCON_RX_BUFS < (int)p->rxq->size ? VCON_RX_BUFS : (int)p->rxq->size;

    for (i = 0; i < VCON_TX_BUFS; i++) {
        if (!p->tx[i].data) {
            p->tx[i].data = (unsigned char *)virtio_dma_alloc(v, VCON_BUF, &p->tx[i].addr);
            if (!p->tx[i].data) {
                return -ENOMEM;
            }
        }
    }
    if (!p->ptx.data) {
        p->ptx.data = (unsigned char *)virtio_dma_alloc(v, VCON_BUF, &p->ptx.addr);
        if (!p->ptx.data) {
            return -ENOMEM;
        }
    }
    while (p->nrx < want) {
        struct vcon_buf *b = &p->rx[p->nrx];

        b->data = (unsigned char *)virtio_dma_alloc(v, VCON_BUF, &b->addr);
        if (!b->data) {
            return p->nrx ? 0 : -ENOMEM;
        }
        flags = console_lock();
        vcon_rx_queue(p, b);
        p->nrx++;
        console_unlock(flags);
        virtqueue_kick(p->rxq);
    }
    return 0;
}

/** The port the device announced, as Linux's add_port: its pages made, a
 * minor taken for its vport node, and the device answered with PORT_READY,
 * or PORT_READY 0 when the pages cannot be had. */
static void vcon_port_add(struct vcon_port *p)
{
    int m;

    if (vcon_port_bufs(p) != 0) {
        klog(KLOG_WARN, "virtio_console", "port: %s: port %u refused: no memory for its buffers",
             p->dev->vdev->name, (unsigned)p->id);
        (void)vcon_ctl_send(p->dev, p->id, VIRTIO_CONSOLE_PORT_READY, 0);
        return;
    }
    p->present = 1;
    p->sent_open = -1;
    if (p->minor < 0) {
        for (m = 0; m < VCON_MINORS && vcon_minor[m]; m++) {
        }
        if (m < VCON_MINORS) {
            vcon_minor[m] = p;
            p->minor = m;
        } else {
            klog(KLOG_WARN, "virtio_console", "port: %s: %s has no node: %d are taken",
                 p->dev->vdev->name, p->vname, VCON_MINORS);
        }
    }
    vcon_ctl_reply(p, VCON_DUE_READY);
}

/** The port has gone, or its device is going, as Linux's unplug_port: off
 * the console, its terminal hung up and its nodes gone, a descriptor of its
 * vport told, and its hvc number and minor given up. */
static void vcon_port_remove(struct vcon_port *p, const char *why)
{
    vcon_detach(p, why, 1);
    p->present = 0;
    p->life++;
    vcon_settle(p);
    if (p->hvc >= 0) {
        vcon_hvc[p->hvc] = 0;
    }
    if (p->minor >= 0) {
        vcon_minor[p->minor] = 0;
    }
    p->console = p->host_open = p->guest_open = 0;
    p->hvc = p->minor = -1;
    if (p->ctl_due) {
        p->dev->owed--;
    }
    p->ctl_due = 0;
    kfree(p->name);
    p->name = 0;
    vcon_rx_drop(p);
    wq_wake(&p->wq, WQ_ALL);
}

/**
 * Act on one control message, `m`, of `len` bytes with what follows it, as
 * Linux's handle_control_message: a port added is set up and answered
 * with PORT_READY, a console port is given its hvc number and answered with
 * PORT_OPEN, a port removed is taken down, and a port's size, name and the
 * host's open are recorded. A size is rows and then columns, as Linux reads
 * it. A port past the device's is logged and left.
 */
static void vcon_ctl_message(struct vcon *d, const struct virtio_console_control *m, uint32_t len)
{
    const unsigned char *more = (const unsigned char *)(m + 1);
    uint32_t extra = len - (uint32_t)sizeof(*m), i;
    struct vcon_port *p;

    if (m->id >= d->nports) {
        if (m->event == VIRTIO_CONSOLE_PORT_ADD) {
            klog(KLOG_INFO, "virtio_console", "port: %s: port %u left: the device has %u",
                 d->vdev->name, (unsigned)m->id, (unsigned)d->nports);
        }
        return;
    }
    p = &d->port[m->id];
    if (!p->rxq || (!p->present && m->event != VIRTIO_CONSOLE_PORT_ADD)) {
        return;
    }
    switch (m->event) {
    case VIRTIO_CONSOLE_PORT_ADD:
        if (!p->present) {
            vcon_port_add(p);
        }
        break;
    case VIRTIO_CONSOLE_PORT_REMOVE:
        vcon_port_remove(p, "its port has gone");
        break;
    case VIRTIO_CONSOLE_CONSOLE_PORT:
        if (m->value && !p->console) {
            p->console = 1;
            vcon_hvc_take(p);
            vcon_ctl_reply(p, VCON_DUE_OPEN);
        }
        break;
    case VIRTIO_CONSOLE_RESIZE:
        if (p->console && extra >= 4u) {
            p->rows = (uint16_t)(more[0] | (more[1] << 8));
            p->cols = (uint16_t)(more[2] | (more[3] << 8));
            vcon_resize(p);
        }
        break;
    case VIRTIO_CONSOLE_PORT_OPEN:
        p->host_open = m->value != 0;
        wq_wake(&p->wq, WQ_ALL);
        break;
    case VIRTIO_CONSOLE_PORT_NAME:
        for (i = 0; i < extra && more[i] != '\0'; i++) {
        }
        kfree(p->name);
        p->name = (char *)kmalloc(i + 1u);
        if (p->name) {
            k_memcpy(p->name, more, i);
            p->name[i] = '\0';
            klog(KLOG_INFO, "virtio_console", "port: %s: port %u is named %s", d->vdev->name,
                 (unsigned)p->id, p->name);
        }
        if (i > (uint32_t)VCON_NAME_MAX) {
            klog(KLOG_WARN, "virtio_console", "port: %s: port %u's name is longer than %d bytes",
                 d->vdev->name, (unsigned)p->id, VCON_NAME_MAX);
        }
        break;
    default:
        break;
    }
    vcon_ports_try();
    vcon_settle(p);
}

/** The device returned a control receive buffer with `len` bytes: keep it
 * for the thread, which acts on it, as Linux's control_intr queues its
 * work. From the control queue's interrupt or the boot's wait. */
static void vcon_ctl_done(struct virtqueue *vq, void *token, uint32_t len)
{
    struct vcon *d = (struct vcon *)vq->priv;
    int i = (int)((struct vcon_buf *)token - d->ctl_rx);
    uint32_t flags;

    if (d->closed) {
        return;
    }
    flags = spin_lock_irqsave(&vcon_ctl_lock);
    d->ctl_len[i] = len;
    d->ctl_pend[(d->ctl_head + d->ctl_n) % d->nctl] = (uint16_t)i;
    d->ctl_n++;
    spin_unlock_irqrestore(&vcon_ctl_lock, flags);
    vcon_kick();
}

/** Give control receive buffer `i` of `d` to the device. */
static void vcon_ctl_give(struct vcon *d, int i)
{
    struct virtio_sg sg;

    sg.addr = d->ctl_rx[i].addr;
    sg.len = VCON_BUF;
    (void)virtqueue_add(d->ctl_rxq, &sg, 0, 1, &d->ctl_rx[i]);
}

/** Act on the control messages `d` returned, oldest first: each port is
 * first given the replies it is still owed, and each buffer goes back to
 * the device after its message, as Linux's control_work_handler. */
static void vcon_ctl_run(struct vcon *d)
{
    uint32_t flags, len, k;
    int i;

    while (!d->closed) {
        flags = spin_lock_irqsave(&vcon_ctl_lock);
        if (d->ctl_n == 0) {
            spin_unlock_irqrestore(&vcon_ctl_lock, flags);
            break;
        }
        i = d->ctl_pend[d->ctl_head];
        len = d->ctl_len[i];
        d->ctl_head = (d->ctl_head + 1) % d->nctl;
        d->ctl_n--;
        spin_unlock_irqrestore(&vcon_ctl_lock, flags);
        vcon_n.control_in++;
        for (k = 0; d->owed > 0 && k < d->nports; k++) {
            if (d->port[k].ctl_due) {
                vcon_ctl_owed(&d->port[k]);
            }
        }
        if (len >= sizeof(struct virtio_console_control)) {
            if (len > VCON_BUF) {
                len = VCON_BUF;
            }
            vcon_ctl_message(d, (const struct virtio_console_control *)(void *)d->ctl_rx[i].data,
                             len);
        }
        if (!d->closed) {
            vcon_ctl_give(d, i);
            virtqueue_kick(d->ctl_rxq);
        }
    }
}

/** The device's configuration changed: without F_MULTIPORT, port 0's size,
 * when the device has F_SIZE, as Linux's config_work_handler reads it; the
 * thread gives it to the terminal. From the interrupt. */
static void vcon_config_changed(struct virtio_dev *v)
{
    struct vcon *d = (struct vcon *)v->priv;
    struct vcon_port *p;

    if (!d || d->closed || d->multiport || !d->size) {
        return;
    }
    p = &d->port[0];
    virtio_config_read(v, VIRTIO_CONSOLE_CFG_COLS, &p->cols, 2);
    virtio_config_read(v, VIRTIO_CONSOLE_CFG_ROWS, &p->rows, 2);
    p->resize_due = 1;
    vcon_kick();
}

/**
 * The thread's work, also run by the boot's wait: each device's control
 * messages, then each hvc terminal's bytes and a size owed to the console
 * terminal. Under the big lock.
 */
static void vcon_service(void)
{
    struct vcon *d;
    int h;

    vcon_work = 0;
    for (d = vcon_list; d; d = d->next) {
        vcon_ctl_run(d);
    }
    for (h = 0; h < VCON_HVC_MAX; h++) {
        struct vcon_port *p = vcon_hvc[h];

        if (p && p->tty && !p->dev->closed) {
            vcon_hvc_pump(p);
        } else if (p && p == vcon_in && p->resize_due) {
            vcon_resize(p);
        }
    }
}

/* ---- the device ---- */

/** Give back `d`'s pages, which the device no longer reaches. */
static void vcon_free_pages(struct vcon *d)
{
    struct virtio_dev *v = d->vdev;
    uint32_t k;
    int i;

    for (k = 0; d->port && k < d->nports; k++) {
        struct vcon_port *p = &d->port[k];

        for (i = 0; i < p->nrx; i++) {
            virtio_dma_free(v, p->rx[i].data, VCON_BUF);
        }
        for (i = 0; i < VCON_TX_BUFS; i++) {
            virtio_dma_free(v, p->tx[i].data, VCON_BUF);
        }
        virtio_dma_free(v, p->ptx.data, VCON_BUF);
        k_memset(p->rx, 0, sizeof(p->rx));
        k_memset(p->tx, 0, sizeof(p->tx));
        k_memset(&p->ptx, 0, sizeof(p->ptx));
        p->nrx = 0;
    }
    for (i = 0; d->ctl_rx && i < d->nctl; i++) {
        virtio_dma_free(v, d->ctl_rx[i].data, VCON_BUF);
    }
    virtio_dma_free(v, d->ctl_tx, VCON_BUF);
    d->ctl_tx = 0;
}

/** Free `d`'s record, its ports' and its lists, off the list of devices:
 * once its device is gone and no vport descriptor names it. */
static void vcon_free(struct vcon *d)
{
    struct vcon **link = &vcon_list;
    uint32_t k;

    while (*link && *link != d) {
        link = &(*link)->next;
    }
    if (*link) {
        *link = d->next;
    }
    for (k = 0; d->port && k < d->nports; k++) {
        kfree(d->port[k].name);
    }
    kfree(d->port);
    kfree(d->ctl_rx);
    kfree(d->ctl_pend);
    kfree(d->ctl_len);
    kfree(d);
}

/**
 * Stop `d`'s device and let go of it, as Linux's virtcons_remove: the
 * device closed first, so that no handler acts and no port carries the
 * console again, then each port taken down (`vcon_port_remove`), the
 * device reset, its queues and the driver's pages freed, and the device
 * shown unbound. The record goes when no vport descriptor names it. Under
 * the big lock, in process context. The handlers that may still run until
 * the reset are the device's own, which find the device closed; the
 * transmit side, which runs without the big lock, leaves each port before
 * `vcon_detach` takes it away, since that waits for its lock.
 */
static void vcon_remove(struct vcon *d)
{
    struct virtio_dev *v = d->vdev;
    uint32_t k, flags;

    d->closed = 1;
    flags = spin_lock_irqsave(&vcon_tx_lock);
    if (vcon_early == d) {
        vcon_early = 0;
    }
    spin_unlock_irqrestore(&vcon_tx_lock, flags);
    for (k = 0; d->port && k < d->nports; k++) {
        if (d->port[k].present) {
            vcon_port_remove(&d->port[k], "its device is removed");
        }
    }
    flags = kirq_save();
    vcon_line_update();
    v->config_changed = 0;
    v->removed = 0;
    kirq_restore(flags);
    virtio_teardown(v);
    vcon_free_pages(d);
    v->priv = 0;
    d->vdev = 0;
    virtio_driver_unbind(v);
    if (d->opens == 0) {
        vcon_free(d);
    }
}

/** The transport's word that the device is unbound, as Linux's
 * virtcons_remove: the driver stops and gives the device back. */
static void vcon_removed(struct virtio_dev *v)
{
    struct vcon *d = (struct vcon *)v->priv;

    if (!d) {
        return;
    }
    klog(KLOG_INFO, "virtio_console", "removed: %s", v->name);
    vcon_remove(d);
    virtio_put(v);
}

/** Set up port `id`'s two queues, `rx` and `rx + 1`, with no interrupt
 * asked for yet on the transmit queue, which the driver polls while no
 * writer waits. 0 or -errno. */
static int vcon_port_queues(struct vcon *d, uint32_t id, uint16_t rx)
{
    struct vcon_port *p = &d->port[id];
    int r;

    r = virtio_queue_setup(d->vdev, rx, vcon_rx_done, p, &p->rxq);
    if (r == 0) {
        r = virtio_queue_setup(d->vdev, (uint16_t)(rx + 1u), vcon_tx_done, p, &p->txq);
    }
    if (r != 0) {
        return r;
    }
    p->txq->batch = vcon_tx_batch;
    (void)virtqueue_irq(p->txq, 0);
    return 0;
}

/** Set up the queues of the device, as Linux's init_vqs for every port it
 * may have, and with F_MULTIPORT the control queues and their pages. A
 * port whose queues the device lacks is left without them. 0 or -errno;
 * the caller tears down on an error. */
static int vcon_setup(struct vcon *d)
{
    struct virtio_dev *v = d->vdev;
    uint32_t k;
    int r, i;

    r = vcon_port_queues(d, 0, 0);
    if (r != 0 || !d->multiport) {
        return r;
    }
    r = virtio_queue_setup(v, 2, vcon_ctl_done, d, &d->ctl_rxq);
    if (r == 0) {
        r = virtio_queue_setup(v, 3, vcon_ctl_sent, d, &d->ctl_txq);
    }
    if (r != 0) {
        return r;
    }
    (void)virtqueue_irq(d->ctl_txq, 0);
    d->nctl = d->ctl_rxq->size;
    d->ctl_rx = (struct vcon_buf *)kzalloc((uint32_t)d->nctl * sizeof(*d->ctl_rx));
    d->ctl_pend = (uint16_t *)kzalloc((uint32_t)d->nctl * sizeof(*d->ctl_pend));
    d->ctl_len = (uint32_t *)kzalloc((uint32_t)d->nctl * sizeof(*d->ctl_len));
    if (!d->ctl_rx || !d->ctl_pend || !d->ctl_len) {
        return -ENOMEM;
    }
    for (i = 0; i < d->nctl; i++) {
        d->ctl_rx[i].data = (unsigned char *)virtio_dma_alloc(v, VCON_BUF, &d->ctl_rx[i].addr);
        if (!d->ctl_rx[i].data) {
            return -ENOMEM;
        }
    }
    d->ctl_tx = (struct virtio_console_control *)virtio_dma_alloc(v, VCON_BUF, &d->ctl_tx_addr);
    if (!d->ctl_tx) {
        return -ENOMEM;
    }
    for (k = 1; k < d->nports; k++) {
        r = vcon_port_queues(d, k, (uint16_t)(2u * k + 2u));
        if (r == -ENOENT && !d->port[k].txq) {
            /* The device has fewer queues than its ports need. */
            d->port[k].rxq = 0;
            r = 0;
        }
        if (r != 0) {
            return r;
        }
    }
    return 0;
}

/** Whether a port that the boot line's name `want` names carries the
 * console. */
static int vcon_want_met(const char *want)
{
    struct vcon *d;
    uint32_t k;

    for (d = vcon_list; d; d = d->next) {
        for (k = 0; k < d->nports; k++) {
            if (d->port[k].out && vcon_names(want, &d->port[k])) {
                return 1;
            }
        }
    }
    return 0;
}

/** The first port the boot line names that carries no console yet, or
 * NULL when each does. */
static const char *vcon_want_unmet(void)
{
    int i;

    for (i = 0; i < vcon_want_n; i++) {
        if (!vcon_want_met(vcon_want[i])) {
            return vcon_want[i];
        }
    }
    return 0;
}

/** Give the control queue its receive buffers and tell the device the
 * driver is ready, on which it announces its ports. */
static void vcon_ports_start(struct vcon *d)
{
    int i;

    for (i = 0; i < d->nctl; i++) {
        vcon_ctl_give(d, i);
    }
    virtqueue_kick(d->ctl_rxq);
    (void)vcon_ctl_send(d, VIRTIO_CONSOLE_BAD_ID, VIRTIO_CONSOLE_DEVICE_READY, 1);
}

/** Wait, at most VCON_BOOT_WAIT_MS, until each port the boot line names
 * carries the console, reaping the control queues with interrupts off and
 * acting on their messages, as the thread would. */
static void vcon_boot_wait(void)
{
    struct vcon *d;
    uint64_t start = arch_cycles(), limit = vcon_cycles(VCON_BOOT_WAIT_MS);
    uint32_t flags = kirq_save();

    while (vcon_want_n > 0) {
        for (d = vcon_list; d; d = d->next) {
            if (d->ctl_rxq && !d->closed) {
                (void)virtqueue_reap(d->ctl_rxq);
            }
        }
        /* A port is given the log's ring here, which may itself take long. */
        vcon_service();
        if (!vcon_want_unmet()) {
            break;
        }
        if (arch_cycles() - start >= limit) {
            klog(KLOG_WARN, "console", "waiting: %s: no device has announced it in %d ms",
                 vcon_want_unmet(), (int)VCON_BOOT_WAIT_MS);
            break;
        }
        touch_nmi_watchdog();
        arch_relax();
    }
    kirq_restore(flags);
}

/** The lowest device number no bound device has. */
static int vcon_index(void)
{
    struct vcon *d;
    int n = 0, used;

    for (;; n++) {
        for (used = 0, d = vcon_list; d; d = d->next) {
            used |= d->vdev && d->index == n;
        }
        if (!used) {
            return n;
        }
    }
}

/** Name port `id` of `d` `vportNpM`. */
static void vcon_vname(struct vcon *d, struct vcon_port *p)
{
    int n = 5;

    k_memcpy(p->vname, "vport", 5);
    n += vcon_decimal(p->vname + n, (uint32_t)d->index);
    p->vname[n++] = 'p';
    n += vcon_decimal(p->vname + n, p->id);
    p->vname[n] = '\0';
}

/** Whether `vdev` offers F_MULTIPORT, read before the negotiation. */
static int vcon_multi(struct virtio_dev *vdev)
{
    return (vdev->ops->get_features(vdev) & VIRTIO_BIT(VIRTIO_CONSOLE_F_MULTIPORT)) != 0;
}

/** The ports `vdev` may have: its `max_nr_ports` with F_MULTIPORT, at
 * most VIRTIO_CONSOLE_PORTS_MAX, which the log says it takes, and 1
 * without; read before the negotiation, as the queues depend on it. */
static uint32_t vcon_ports_max(struct virtio_dev *vdev)
{
    uint32_t nports = 1;

    if (!vcon_multi(vdev)) {
        return 1;
    }
    virtio_config_read(vdev, VIRTIO_CONSOLE_CFG_MAX_PORTS, &nports, 4);
    if (nports == 0) {
        nports = 1;
    }
    if (nports > VIRTIO_CONSOLE_PORTS_MAX) {
        klog(KLOG_WARN, "virtio_console", "ports: %s: %u of its %u are taken", vdev->name,
             (unsigned)VIRTIO_CONSOLE_PORTS_MAX, (unsigned)nports);
        nports = VIRTIO_CONSOLE_PORTS_MAX;
    }
    return nports;
}

/** A record of `nports` ports for `vdev`, whose features are negotiated,
 * numbered and put last on the list of devices; NULL without memory. */
static struct vcon *vcon_new(struct virtio_dev *vdev, uint32_t nports)
{
    struct vcon *d = (struct vcon *)kzalloc(sizeof(*d)), **link;
    uint32_t k;

    if (d) {
        d->port = (struct vcon_port *)kzalloc(nports * sizeof(*d->port));
    }
    if (!d || !d->port) {
        kfree(d);
        return NULL;
    }
    d->vdev = vdev;
    d->index = vcon_index();
    d->nports = nports;
    d->multiport = virtio_has(vdev, VIRTIO_CONSOLE_F_MULTIPORT);
    d->emerg = virtio_has(vdev, VIRTIO_CONSOLE_F_EMERG_WRITE);
    d->size = virtio_has(vdev, VIRTIO_CONSOLE_F_SIZE);
    for (k = 0; k < nports; k++) {
        struct vcon_port *p = &d->port[k];

        p->dev = d;
        p->id = k;
        p->hvc = p->minor = p->tx_cur = p->sent_open = -1;
        wait_queue_init_named(&p->wq, "virtio_console_port");
        vcon_vname(d, p);
    }
    vdev->priv = d;
    for (link = &vcon_list; *link; link = &(*link)->next) {
    }
    *link = d;
    return d;
}

/** Make `d`'s `emerg_wr` carry the console's output until hvc0 comes, when
 * the boot line names hvc0 first, the device has F_EMERG_WRITE, no other
 * does so already and the machine has a UART, which has had the log so
 * far: QEMU gives the bytes its first console port. */
static void vcon_early_start(struct vcon *d)
{
    uint32_t flags;

    if (!d->emerg || vcon_early || vcon_want_n == 0 || !vcon_same(vcon_want[0], "hvc0") ||
        !serial_uart_found() || vcon_hvc[0]) {
        return;
    }
    flags = spin_lock_irqsave(&vcon_tx_lock);
    vcon_early = d;
    spin_unlock_irqrestore(&vcon_tx_lock, flags);
    flags = kirq_save();
    vcon_line_update();
    kirq_restore(flags);
}

/** Start a device of one port: port 0, a console port with the size of the
 * configuration, is there from the start. */
static void vcon_one_port_start(struct vcon *d)
{
    struct vcon_port *p = &d->port[0];

    p->console = 1;
    vcon_hvc_take(p);
    if (d->size) {
        virtio_config_read(d->vdev, VIRTIO_CONSOLE_CFG_COLS, &p->cols, 2);
        virtio_config_read(d->vdev, VIRTIO_CONSOLE_CFG_ROWS, &p->rows, 2);
    }
    vcon_port_add(p);
    vcon_ports_try();
    vcon_settle(p);
}

/**
 * Bind `vdev`, as `virtio_console_bind` says. The device is told the driver
 * is ready, but the boot waits for the ports it names itself
 * (`vcon_boot_wait`), after binding every device.
 */
int32_t virtio_console_bind(struct virtio_dev *vdev)
{
    uint64_t wanted = VIRTIO_BIT(VIRTIO_CONSOLE_F_SIZE) | VIRTIO_BIT(VIRTIO_CONSOLE_F_MULTIPORT) |
                      VIRTIO_BIT(VIRTIO_CONSOLE_F_EMERG_WRITE);
    uint32_t nports = vcon_ports_max(vdev);
    struct vcon *d;
    int r;

    /* Two queues for a device of one port, two more for the control and
     * two for each other port. */
    r = virtio_init(vdev, wanted, (uint16_t)(vcon_multi(vdev) ? 2u * nports + 2u : 2u));
    if (r == 0 && vdev->polled) {
        /* Linux's virtio_pci needs an interrupt as well; input and the
         * control messages after the boot come by nothing else. */
        klog(KLOG_WARN, "virtio_console", "refused: %s: no interrupt, which its input needs",
             vdev->name);
        virtio_teardown(vdev);
        r = -EIO;
    }
    d = r == 0 ? vcon_new(vdev, nports) : NULL;
    if (r == 0 && !d) {
        virtio_teardown(vdev);
        r = -ENOMEM;
    }
    if (r != 0) {
        virtio_put(vdev);
        return r;
    }
    r = virtio_driver_bind(vdev, "virtio_console");
    if (r == 0) {
        r = vcon_setup(d);
    }
    if (r == 0) {
        vdev->config_changed = vcon_config_changed;
        vdev->removed = vcon_removed;
        r = virtio_ready(vdev);
    }
    if (r != 0) {
        vcon_remove(d);
        virtio_put(vdev);
        return r;
    }
    klog(KLOG_INFO, "virtio_console", "bound: %s: %s %u%s%s", vdev->name,
         d->multiport ? "ports with the control queues, at most" : "one port", (unsigned)nports,
         d->size ? ", size" : "", d->emerg ? ", emergency write" : "");
    vcon_early_start(d);
    if (d->multiport) {
        vcon_ports_start(d);
    } else {
        vcon_one_port_start(d);
    }
    virtio_driver_bound(vdev);
    return 0;
}

/** Keep `name` among the ports the boot line names, once. */
static void vcon_want_add(const char *name)
{
    int i;

    for (i = 0; i < vcon_want_n; i++) {
        if (vcon_same(vcon_want[i], name)) {
            return;
        }
    }
    if (vcon_want_n < VCON_WANT_MAX) {
        k_memcpy(vcon_want[vcon_want_n++], name, k_strlen(name) + 1u);
    }
}

/** Whether `value` is the name of a port: `hvcN` with N below
 * VCON_HVC_MAX, or `vportNpM`. */
static int vcon_port_word(const char *value)
{
    const char *s = value;
    int n;

    if (vcon_begins(s, "hvc")) {
        s += 3;
        n = vcon_number(&s);
        return n >= 0 && n < VCON_HVC_MAX && *s == '\0';
    }
    if (!vcon_begins(s, "vport")) {
        return 0;
    }
    s += 5;
    if (vcon_number(&s) < 0 || *s++ != 'p') {
        return 0;
    }
    n = vcon_number(&s);
    return n >= 0 && n < VIRTIO_CONSOLE_PORTS_MAX && *s == '\0';
}

/**
 * Read the boot line's `console=` words, each in turn, as Linux's console
 * list: every port named carries the console's output; the console's
 * input comes from the last word that names a line, a port or the UART;
 * and the UART stays a console when a word names it. A value's options
 * after a comma are not read, and a word that names neither a port nor
 * the UART, such as `tty0`, changes nothing.
 */
static void vcon_read_boot_line(void)
{
    char value[32];
    int i, n = kcmdline_count("console");

    vcon_want_n = 0;
    vcon_want_in[0] = '\0';
    vcon_want_uart = 0;
    for (i = 0; i < n; i++) {
        unsigned int k;

        if (!kcmdline_value_nth("console", i, value, sizeof(value))) {
            continue;
        }
        for (k = 0; value[k] != '\0' && value[k] != ','; k++) {
        }
        value[k] = '\0';
        if (k < sizeof(vcon_want_in) && vcon_port_word(value)) {
            vcon_want_add(value);
            k_memcpy(vcon_want_in, value, k + 1u);
        } else if (vcon_begins(value, "ttyS")) {
            vcon_want_uart = 1;
            vcon_want_in[0] = '\0';
        }
    }
}

int32_t virtio_console_start(void)
{
    struct virtio_dev *vdev;
    int32_t rc, first = 0;
    int nth, bound = 0;

    vcon_read_boot_line();
    wait_queue_init_named(&vcon_wq, "khvcd");
    if (virtio_driver_register(VIRTIO_ID_CONSOLE, virtio_console_bind) != 0) {
        klog(KLOG_WARN, "virtio_console",
             "not registered: a console bound later through /sys is left");
    }
    for (nth = 0; virtio_pci_find(VIRTIO_ID_CONSOLE, nth, &vdev) == 0; nth++) {
        rc = virtio_console_bind(vdev);
        if (rc == 0) {
            bound++;
        } else {
            klog(KLOG_WARN, "virtio_console", "refused: error %d", (int)rc);
            if (!first) {
                first = rc;
            }
        }
    }
    vcon_boot_wait();
    for (nth = 0; nth < vcon_want_n; nth++) {
        if (!vcon_want_met(vcon_want[nth])) {
            klog(KLOG_WARN, "console",
                 "none: console=%s names no port now; the console stays on COM1", vcon_want[nth]);
        }
    }
    if (bound == 0) {
        return first;
    }
    if (kcmdline_has("rach2.vconpanic")) {
        virtio_console_panic_test();
    }
    return bound;
}

/** The thread's wait condition: it has work. */
static int vcon_due(void *arg)
{
    (void)arg;
    return vcon_work;
}

#ifndef RACH2_HOSTTEST
/** khvcd: sleep until a device returns a control message, an hvc
 * terminal has bytes to move or a size is owed, then do it, holding the
 * big lock as kernel code does. */
static void vcon_thread(void)
{
    arch_irq_enable();
    for (;;) {
        (void)wq_wait_event_idle(&vcon_wq, vcon_due, NULL);
        vcon_service();
    }
}

void virtio_console_thread_start(void)
{
    static process_t *thread;
    process_t *p;
    uintptr_t base;
    uint32_t flags;

    if (thread) {
        return;
    }
    flags = kirq_save();
    base = kstack_alloc();
    p = base ? alloc_process() : NULL;
    if (!p) {
        if (base) {
            kstack_free(base + KERNEL_STACK_SIZE);
        }
        kirq_restore(flags);
        klog(KLOG_ERR, "virtio_console", "refused: no record or stack for khvcd");
        return;
    }
    p->kernel_stack = base + KERNEL_STACK_SIZE;
    p->space_root = 0;
    p->page_dir = NULL;
    p->parent_pid = 0;
    proc_set_comm(p, "khvcd");
    setup_process_stack(p, vcon_thread);
    thread = p;
    sched_make_ready(p);
    kirq_restore(flags);
}
#endif

void virtio_console_shutdown(void)
{
    struct vcon *d;
    uint32_t k, flags;

    for (d = vcon_list; d && (!d->vdev || d->closed); d = d->next) {
    }
    if (!d) {
        return;
    }
    serial_flush();
    flags = kirq_save();
    for (d = vcon_list; d; d = d->next) {
        if (!d->vdev || d->closed) {
            continue;
        }
        d->closed = 1;
        for (k = 0; k < d->nports; k++) {
            vcon_detach(&d->port[k], "the system stops", 0);
        }
        d->vdev->config_changed = 0;
    }
    if (vcon_early) {
        vcon_early = 0;
        vcon_line_update();
    }
    kirq_restore(flags);
    for (d = vcon_list; d; d = d->next) {
        for (k = 0; d->vdev && k < d->nports; k++) {
            vcon_settle(&d->port[k]);
        }
    }
    for (d = vcon_list; d; d = d->next) {
        if (!d->vdev) {
            continue;
        }
        if (virtio_halt(d->vdev) == 0) {
            klog(KLOG_INFO, "virtio_console", "shutdown: %s: reset", d->vdev->name);
        } else {
            klog(KLOG_WARN, "virtio_console", "shutdown: %s: the reset did not end", d->vdev->name);
        }
    }
}

void virtio_console_panic_test(void)
{
    uint32_t flags = kirq_save();

    if (vcon_nouts == 0) {
        kirq_restore(flags);
        klog(KLOG_WARN, "virtio_console", "panic test: left: no port carries the console");
        return;
    }
    /* Held as a processor the panic stops would hold it: not this one's. */
    spin_lock(&vcon_tx_lock);
    k_panic("virtio_console: panic test: the transmit side is held");
}

void virtio_console_counters(struct virtio_console_counters *out)
{
    *out = vcon_n;
}
