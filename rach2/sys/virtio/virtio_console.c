/*
 * virtio_console.c: the virtio console (virtio 1.2, 5.3) over the shared
 * virtio core, as Linux's drivers/char/virtio_console.c with its hvc
 * console: its ports, the control queues, and the ports that carry the
 * kernel's console as a line of serial.h.
 */

#include "virtio_console.h"

#include "../arch/arch.h"
#include "../cmdline.h"
#include "../kio.h"
#include "../klog.h"
#include "../panic.h"
#include "../serial.h"
#include "../watchdog.h"
#include "../lib/kerrno.h"
#include "../lib/kirq.h"
#include "../lib/kstring.h"
#include "../tty/tty.h"

/* A port's buffers: VCON_RX_BUFS that the device fills, as many as the
 * receive ring of serial.h holds twice over, VCON_TX_BUFS that the driver
 * fills, and one more that only a panic fills, each a page. */
#define VCON_BUF     4096u
#define VCON_RX_BUFS 8
#define VCON_TX_BUFS 4

/* The control queue's receive buffers, cut from one page: a message is 8
 * bytes, and a port's name follows PORT_NAME in the same buffer. */
#define VCON_CTL_BUFS 8
#define VCON_CTL_BUF  (VCON_BUF / VCON_CTL_BUFS)

/* The control messages the driver may have given the device at once, each
 * 8 bytes of one page: a message the device has not taken in its wait
 * keeps its slot until a reap finds it returned. */
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
 * waits VCON_BOOT_WAIT_MS for the device to announce the ports the boot
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

/* The ports the boot line may name: hvc0 and vport0pN. */
#define VCON_WANT_MAX (VIRTIO_CONSOLE_PORTS + 1)

/* The control replies a port is owed (`ctl_due` of a port). */
#define VCON_DUE_READY 1u /* PORT_READY */
#define VCON_DUE_OPEN  2u /* PORT_OPEN */

/* A buffer of a port: a page the device reads or writes. */
struct vcon_buf {
    unsigned char *data;
    uint64_t addr;              /* as the device reads it */
    uint32_t len;               /* receive: the bytes the device wrote */
    uint32_t off;               /* receive: those the console has taken */
    volatile unsigned int busy; /* transmit: the device has it */
};

/* A port. Its receive buffers are made when it first carries the console
 * and stay in its queue until the device is reset; its transmit buffers
 * are made with the device's queues. */
struct vcon_port {
    uint32_t id;
    int present;          /* the device has the port */
    int console;          /* a console port: port 0 without F_MULTIPORT, or one
                           * the device marked with CONSOLE_PORT */
    int hvc;              /* the N of the console port's name hvcN, given when it
                           * became a console port and kept until it goes; -1 */
    int host_open;        /* the device's last PORT_OPEN */
    unsigned int ctl_due; /* VCON_DUE_* replies not yet given the device */
    uint16_t cols, rows;
    char name[32]; /* PORT_NAME's, or empty */
    struct virtqueue *rxq, *txq;
    struct vcon_buf rx[VCON_RX_BUFS];
    int rx_made;
    /* The buffers the device returned whose bytes the console has not all
     * taken, oldest first: indexes into `rx`. */
    uint8_t rx_fifo[VCON_RX_BUFS];
    unsigned int rx_head, rx_n;
    /* The transmit side, under `vcon_tx_lock`: whether the port carries
     * the console's output, its buffers, the one `vcon_out` fills or -1,
     * the bytes in it, and whether the last wait reached its bound. */
    int out;
    struct vcon_buf tx[VCON_TX_BUFS];
    int tx_cur;
    uint32_t tx_fill;
    int tx_stuck;
    /* The panic's own buffer and the bytes in it: a panic that finds the
     * transmit side held fills this one, which the holder never touches. */
    struct vcon_buf ptx;
    uint32_t ptx_fill;
};

/*
 * The driver's one device. The ports' and the control queues' state is
 * written under the big lock with interrupts off, by the boot, the
 * interrupt handlers, the removal and the shutdown; the handlers run under
 * the big lock too, since the driver does not ask for unlocked ones
 * (`irq_unlocked`). `vcon_tx_lock` guards the transmit side, each port's
 * `out`, `tx`, `tx_cur`, `tx_fill` and `tx_stuck`, `in` and `only`, which a
 * processor without the big lock also uses when it writes a report, and is
 * taken with interrupts off. The console's lock (`console_lock`), which
 * guards the receive ring the line feeds, also guards each port's `rx` and
 * its list of returned buffers. A queue's lock is taken inside either, and
 * nothing else is.
 */
static struct vcon {
    struct virtio_dev *vdev; /* NULL while no device is bound */
    int multiport, emerg, size;
    /* The removal or the shutdown has begun: no port carries the console
     * again, no handler acts and no control message is sent. */
    int closed;
    struct vcon_port port[VIRTIO_CONSOLE_PORTS];
    struct virtqueue *ctl_rxq, *ctl_txq;
    unsigned char *ctl_rx;
    struct virtio_console_control *ctl_tx; /* VCON_CTL_TX of them */
    uint64_t ctl_rx_addr, ctl_tx_addr;
    volatile unsigned char ctl_tx_busy[VCON_CTL_TX];
    struct vcon_port *in;   /* the port the console's input comes from */
    struct vcon_port *only; /* while a port is given the log's ring, the
                             * one port output goes to; NULL */
    struct virtio_console_counters n;
} vcon;

static struct spinlock vcon_tx_lock = SPINLOCK_INIT_NAMED("virtio_console");

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

/* The ports, by number, that have been given the log's ring, once each for
 * the machine's life, whatever device carries them. */
static unsigned int vcon_replayed;

/* The port `vcon_attach_line` attaches, for `klog_replay`. */
static struct vcon_port *vcon_attaching;

static void vcon_out(char c);
static void vcon_flush(void);
static void vcon_rx_room(void);
static void vcon_rx_discard(void);

static const struct serial_line vcon_serial_line = {vcon_out, vcon_flush, vcon_rx_room,
                                                    vcon_rx_discard};

/** The name of `p`'s node in /dev, in `out` of 16 bytes: `hvcN` for a
 * console port, N the number it was given, and `vport0pN` for port N of
 * another kind, as Linux names them. */
static void vcon_port_name(const struct vcon_port *p, char *out)
{
    static const char hvc[] = "hvc0", vport[] = "vport0p0";

    if (p->console && p->hvc >= 0) {
        k_memcpy(out, hvc, sizeof(hvc));
        out[3] = (char)('0' + p->hvc);
        return;
    }
    k_memcpy(out, vport, sizeof(vport));
    out[7] = (char)('0' + p->id);
}

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

/** Whether the boot line's name `want` names `p`: its node's name, or
 * `vport0pN` with N its number whatever its kind. */
static int vcon_names(const char *want, const struct vcon_port *p)
{
    char name[16];

    if (want[0] == '\0') {
        return 0;
    }
    vcon_port_name(p, name);
    return vcon_same(name, want) || (want[0] == 'v' && want[7] == (char)('0' + p->id));
}

/** Whether the boot line names `p` as a port of the console's output. */
static int vcon_wanted(const struct vcon_port *p)
{
    int i;

    for (i = 0; i < vcon_want_n; i++) {
        if (vcon_names(vcon_want[i], p)) {
            return 1;
        }
    }
    return 0;
}

/** Give console port `p` the lowest hvc number no other port has, as
 * Linux's hvc_alloc gives a console port its index once; it keeps it until
 * the port goes, so that a console port found later names no other. */
static void vcon_hvc_take(struct vcon_port *p)
{
    int n, k, used;

    if (p->hvc >= 0) {
        return;
    }
    for (n = 0; n < VIRTIO_CONSOLE_PORTS; n++) {
        used = 0;
        for (k = 0; k < VIRTIO_CONSOLE_PORTS; k++) {
            used |= &vcon.port[k] != p && vcon.port[k].hvc == n;
        }
        if (!used) {
            p->hvc = n;
            return;
        }
    }
}

/** The number of cycles `ms` milliseconds take at most. */
static uint64_t vcon_cycles(unsigned long ms)
{
    return (uint64_t)ms * VCON_CYCLES_PER_MS;
}

/* ---- the transmit side ---- */

/** A transmit buffer came back: it is free again. From a reap, in the
 * transmit side or in an interrupt the queue shares. */
static void vcon_tx_done(struct virtqueue *vq, void *token, uint32_t len)
{
    (void)vq;
    (void)len;
    ((struct vcon_buf *)token)->busy = 0;
}

/** How many of `p`'s transmit buffers the device has; `vcon_tx_lock`
 * held. */
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
            vcon.n.tx_waits++;
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
        vcon.n.tx_dropped += len;
        return 0;
    }
    sg.addr = b->addr;
    sg.len = len;
    b->busy = 1;
    if (virtqueue_add(p->txq, &sg, 1, 0, b) != 0) {
        b->busy = 0;
        vcon.n.tx_dropped += len;
        return 0;
    }
    vcon.n.tx_bytes += len;
    vcon.n.tx_buffers++;
    virtqueue_kick(p->txq);
    return 1;
}

/** Give the buffer `vcon_out` filled for `p` to the device;
 * `vcon_tx_lock` held. */
static void vcon_tx_submit(struct vcon_port *p)
{
    (void)vcon_tx_give(p, &p->tx[p->tx_cur], p->tx_fill);
    p->tx_cur = -1;
    p->tx_fill = 0;
}

/** Choose a free transmit buffer of `p` for `vcon_out` to fill, after a
 * reap, and a wait for the device when it has them all; `vcon_tx_lock`
 * held. Returns 0 when none came back. */
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
        vcon.n.tx_dropped++;
        return;
    }
    p->tx[p->tx_cur].data[p->tx_fill++] = (unsigned char)c;
    if (p->tx_fill == VCON_BUF) {
        vcon_tx_submit(p);
    }
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

/** Write `c` to the device's `emerg_wr`, which needs no queue (virtio 1.2,
 * 5.3.5) and which QEMU gives its console port, port 0; during a panic
 * that can reach no port's queue. */
static void vcon_emerg(char c)
{
    uint32_t v = (unsigned char)c;

    if (vcon.emerg && vcon.vdev && !vcon.closed) {
        virtio_config_write(vcon.vdev, VIRTIO_CONSOLE_CFG_EMERG_WR, &v, 4);
        vcon.n.emerg++;
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
            vcon.n.tx_waits++;
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
    vcon.n.tx_panic += p->ptx_fill;
    (void)vcon_tx_give(p, &p->ptx, p->ptx_fill);
    p->ptx_fill = 0;
    (void)vcon_ptx_wait(p);
}

/**
 * A panic's byte that found the transmit side held: it goes into each
 * output port's panic buffer, which no holder of the side uses, where the
 * port's queue is free (`vcon_vq_free`), and a full buffer to the device;
 * a byte that reaches no port's queue goes to `emerg_wr`.
 */
static void vcon_panic_out(char c)
{
    int i, lost = 0, sent = 0;

    for (i = 0; i < VIRTIO_CONSOLE_PORTS; i++) {
        struct vcon_port *p = &vcon.port[i];

        if (!p->out || (vcon.only && p != vcon.only) || !p->ptx.data) {
            continue;
        }
        if (!vcon_vq_free(p->txq)) {
            lost = 1;
            continue;
        }
        if (p->ptx.busy && !vcon_ptx_wait(p)) {
            vcon.n.tx_dropped++;
            continue;
        }
        p->ptx.data[p->ptx_fill++] = (unsigned char)c;
        sent = 1;
        if (p->ptx_fill == VCON_BUF) {
            vcon_ptx_flush(p);
        }
    }
    if (lost || !sent) {
        vcon_emerg(c);
    }
}

/**
 * The line's `out`: put `c` in the buffer being filled for each port that
 * carries the console's output, or the one being given the log's ring, and
 * give a buffer to the device when it is full. A byte that finds no port,
 * or no buffer after the wait for one, is dropped and counted. During a
 * panic, a port whose queue's lock is held is not touched; a byte that so
 * reaches no port, or that finds the transmit side itself held, goes the
 * panic's way (`vcon_panic_out`).
 */
static void vcon_out(char c)
{
    uint32_t flags;
    int i, sent = 0, lost = 0;

    if (!vcon_tx_enter(&flags)) {
        if (k_panicking()) {
            vcon_panic_out(c);
        }
        return;
    }
    for (i = 0; i < VIRTIO_CONSOLE_PORTS; i++) {
        struct vcon_port *p = &vcon.port[i];

        if (!p->out || (vcon.only && p != vcon.only)) {
            continue;
        }
        if (k_panicking() && !vcon_vq_free(p->txq)) {
            lost = 1;
            continue;
        }
        vcon_tx_put(p, c);
        sent = 1;
    }
    if (!sent && !lost) {
        vcon.n.tx_dropped++;
    }
    vcon_tx_leave(flags);
    if (lost) {
        vcon_emerg(c);
    }
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
    for (i = 0; i < VIRTIO_CONSOLE_PORTS; i++) {
        struct vcon_port *p = &vcon.port[i];

        if (!p->out || (k_panicking() && !vcon_vq_free(p->txq))) {
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
        struct vcon_buf *b = &p->rx[p->rx_fifo[p->rx_head]];
        int n = serial_rx_feed(b->data + b->off, (int)(b->len - b->off));

        b->off += (uint32_t)n;
        moved += (uint32_t)n;
        if (b->off < b->len) {
            vcon.n.rx_held++;
            break;
        }
        p->rx_head = (p->rx_head + 1u) % VCON_RX_BUFS;
        p->rx_n--;
        vcon_rx_queue(p, b);
        kick = 1;
    }
    if (kick) {
        virtqueue_kick(p->rxq);
    }
    vcon.n.rx_bytes += moved;
    return moved;
}

/**
 * The device returned a receive buffer of a port with `len` bytes, from
 * the port's interrupt, under the big lock. The input port's go to the
 * console's ring, and the console is then served as the UART's interrupt
 * serves it: a parked reader is woken, or the ring is moved into the
 * discipline, in passes while a pass takes bytes. Another port has its
 * bytes dropped and counted. Once the device is closed the buffer stays
 * with the driver, which frees it after the reset.
 */
static void vcon_rx_done(struct virtqueue *vq, void *token, uint32_t len)
{
    struct vcon_port *p = (struct vcon_port *)vq->priv;
    struct vcon_buf *b = (struct vcon_buf *)token;
    uint32_t flags, moved = 0;
    int pass;

    if (vcon.closed) {
        return;
    }
    flags = console_lock();
    if (p != vcon.in) {
        vcon.n.rx_dropped += len;
        vcon_rx_queue(p, b);
        virtqueue_kick(p->rxq);
    } else {
        b->len = len;
        b->off = 0;
        p->rx_fifo[(p->rx_head + p->rx_n) % VCON_RX_BUFS] = (uint8_t)(b - p->rx);
        p->rx_n++;
        moved = vcon_rx_drain(p);
    }
    console_unlock(flags);
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

    if (vcon.in) {
        (void)vcon_rx_drain(vcon.in);
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
    if (vcon.in) {
        vcon_rx_drop(vcon.in);
    }
}

/* ---- the control queues ---- */

/** A control message came back: its slot is free again. */
static void vcon_ctl_sent(struct virtqueue *vq, void *token, uint32_t len)
{
    (void)vq;
    (void)len;
    vcon.ctl_tx_busy[(struct virtio_console_control *)token - vcon.ctl_tx] = 0;
}

/**
 * Send the control message `event` with `value` for port `id`, and wait
 * until the device has taken it, at most VCON_CTL_WAIT_MS, as Linux's
 * __send_control_msg waits; under the big lock with interrupts off. The
 * queue is reaped first, so that a message the device took after its own
 * wait ended frees its slot, since the transmit queue asks for no
 * interrupt; a message not taken in its wait stays in the queue, in its
 * slot. Returns 0, at once without F_MULTIPORT, or -EBUSY when every slot
 * is still the device's, or the queue's error, with nothing sent.
 */
static int vcon_ctl_send(uint32_t id, uint16_t event, uint16_t value)
{
    struct virtio_console_control *m;
    struct virtio_sg sg;
    uint64_t start, limit = vcon_cycles(VCON_CTL_WAIT_MS);
    int i, r;

    if (!vcon.multiport || !vcon.ctl_txq) {
        return 0;
    }
    if (vcon.closed) {
        return -EIO;
    }
    (void)virtqueue_reap(vcon.ctl_txq);
    for (i = 0; i < VCON_CTL_TX && vcon.ctl_tx_busy[i]; i++) {
    }
    if (i == VCON_CTL_TX) {
        klog(KLOG_WARN, "virtio_console", "control: %s: event %d not sent: the device holds %d",
             vcon.vdev->name, (int)event, VCON_CTL_TX);
        return -EBUSY;
    }
    m = &vcon.ctl_tx[i];
    m->id = id;
    m->event = event;
    m->value = value;
    sg.addr = vcon.ctl_tx_addr + (uint64_t)i * sizeof(*m);
    sg.len = (uint32_t)sizeof(*m);
    vcon.ctl_tx_busy[i] = 1;
    r = virtqueue_add(vcon.ctl_txq, &sg, 1, 0, m);
    if (r != 0) {
        vcon.ctl_tx_busy[i] = 0;
        return r;
    }
    vcon.n.control_out++;
    virtqueue_kick(vcon.ctl_txq);
    start = arch_cycles();
    while (vcon.ctl_tx_busy[i]) {
        (void)virtqueue_reap(vcon.ctl_txq);
        if (!vcon.ctl_tx_busy[i]) {
            break;
        }
        if (arch_cycles() - start >= limit) {
            klog(KLOG_WARN, "virtio_console", "control: %s: event %d not taken in %d ms",
                 vcon.vdev->name, (int)event, (int)VCON_CTL_WAIT_MS);
            break;
        }
        arch_relax();
    }
    return 0;
}

/** Give the device the replies `p` is owed (`ctl_due`), PORT_READY before
 * PORT_OPEN; one that cannot be sent stays owed, for the next control
 * message the device sends. */
static void vcon_ctl_owed(struct vcon_port *p)
{
    if ((p->ctl_due & VCON_DUE_READY) && vcon_ctl_send(p->id, VIRTIO_CONSOLE_PORT_READY, 1) == 0) {
        p->ctl_due &= ~VCON_DUE_READY;
    }
    if ((p->ctl_due & VCON_DUE_OPEN) && !(p->ctl_due & VCON_DUE_READY) &&
        vcon_ctl_send(p->id, VIRTIO_CONSOLE_PORT_OPEN, 1) == 0) {
        p->ctl_due &= ~VCON_DUE_OPEN;
    }
}

/** Owe `p` the reply `due`, VCON_DUE_*, and give it now if the device
 * takes it. */
static void vcon_ctl_reply(struct vcon_port *p, unsigned int due)
{
    p->ctl_due |= due;
    vcon_ctl_owed(p);
}

/** Set the console terminal's size to `p`'s when `p` is the console's
 * input and the device gave a size, as Linux's hvc_resize. */
static void vcon_resize(const struct vcon_port *p)
{
    struct tty_winsize ws;

    if (p != vcon.in || p->cols == 0 || p->rows == 0) {
        return;
    }
    ws = console_tty.winsize;
    ws.ws_col = p->cols;
    ws.ws_row = p->rows;
    tty_resize(&console_tty, &ws);
}

/** Whether a port carries the console's output now. */
static int vcon_any_out(void)
{
    int i;

    for (i = 0; i < VIRTIO_CONSOLE_PORTS; i++) {
        if (vcon.port[i].out) {
            return 1;
        }
    }
    return 0;
}

/** Make `vcon_attaching` carry the console's output, and its input when
 * the boot line names it last, and attach the line to the console
 * (`serial_line_attach`); the input stays the UART's while no port has
 * it. Under the big lock with interrupts off. */
static void vcon_attach_line(void)
{
    struct vcon_port *p = vcon_attaching;
    uint32_t flags = spin_lock_irqsave(&vcon_tx_lock);

    p->tx_cur = -1;
    p->tx_fill = 0;
    p->tx_stuck = 0;
    p->out = 1;
    if (vcon_names(vcon_want_in, p)) {
        vcon.in = p;
    }
    spin_unlock_irqrestore(&vcon_tx_lock, flags);
    serial_line_attach(&vcon_serial_line, vcon.in != 0, vcon_want_uart);
}

/**
 * Make `p` carry the console: its receive buffers made and given to the
 * device, the device told the port is open, and the port given the
 * console's output, and its input when the boot line names it last
 * (`vcon_attach_line`). On a machine without a UART a port that has not
 * had it is then given every record of the log's ring, as Linux's
 * register_console gives a console that no boot console preceded the
 * buffer, whenever the port comes; `klog_replay` attaches it inside its
 * replay, so that each record reaches the port once and in order, and
 * meanwhile output goes to that port alone. With a UART the records so far
 * went there, and the port takes the log from here on. Under the big lock
 * with interrupts off. Returns 0, -EIO once the device is closed, or
 * -ENOMEM with nothing changed.
 */
static int vcon_attach(struct vcon_port *p)
{
    char name[16];
    uint32_t flags;
    int i, input = vcon_names(vcon_want_in, p);

    if (vcon.closed) {
        return -EIO;
    }
    if (!p->rx_made) {
        for (i = 0; i < VCON_RX_BUFS; i++) {
            p->rx[i].data = (unsigned char *)virtio_dma_alloc(vcon.vdev, VCON_BUF, &p->rx[i].addr);
            if (!p->rx[i].data) {
                while (i-- > 0) {
                    virtio_dma_free(vcon.vdev, p->rx[i].data, VCON_BUF);
                    p->rx[i].data = 0;
                }
                return -ENOMEM;
            }
        }
        flags = console_lock();
        for (i = 0; i < VCON_RX_BUFS && i < (int)p->rxq->size; i++) {
            vcon_rx_queue(p, &p->rx[i]);
        }
        p->rx_made = 1;
        console_unlock(flags);
    }
    vcon_port_name(p, name);
    klog(KLOG_INFO, "console", "enabled: %s, port %d of %s%s", name, (int)p->id, vcon.vdev->name,
         input ? "" : ", for output alone");
    klog_flush();
    virtqueue_kick(p->rxq);
    if (!p->console) {
        vcon_ctl_reply(p, VCON_DUE_OPEN);
    }
    vcon_attaching = p;
    if (!(vcon_replayed & (1u << p->id)) && !serial_uart_found()) {
        vcon_replayed |= 1u << p->id;
        flags = spin_lock_irqsave(&vcon_tx_lock);
        vcon.only = p;
        spin_unlock_irqrestore(&vcon_tx_lock, flags);
        klog_replay(vcon_attach_line);
        flags = spin_lock_irqsave(&vcon_tx_lock);
        vcon.only = 0;
        spin_unlock_irqrestore(&vcon_tx_lock, flags);
    } else {
        vcon_attach_line();
    }
    vcon_attaching = 0;
    vcon_resize(p);
    return 0;
}

/**
 * Take `p` off the console, since it has gone, its device is being removed
 * or the system stops. When it had the console's input, the bytes it gave
 * the ring and those its buffers hold are dropped first, and with `hang`
 * the console terminal is hung up, as Linux's hvc_remove hangs its tty up,
 * so that a reader of its node is woken and its session told; the input
 * goes back to the UART. The line is detached when no port carries the
 * output. Under the big lock with interrupts off.
 */
static void vcon_detach(struct vcon_port *p, const char *why, int hang)
{
    char name[16];
    uint32_t flags;
    int was_in = p == vcon.in;

    if (!p->out) {
        return;
    }
    vcon_port_name(p, name);
    if (was_in) {
        serial_rx_flush();
    }
    flags = spin_lock_irqsave(&vcon_tx_lock);
    p->out = 0;
    p->tx_cur = -1;
    p->tx_fill = 0;
    if (was_in) {
        vcon.in = 0;
    }
    spin_unlock_irqrestore(&vcon_tx_lock, flags);
    if (vcon_any_out()) {
        serial_line_attach(&vcon_serial_line, vcon.in != 0, vcon_want_uart);
    } else {
        serial_line_detach(&vcon_serial_line);
    }
    vcon_rx_drop(p);
    klog(KLOG_INFO, "console", "disabled: %s: %s", name, why);
    if (was_in && hang) {
        tty_console_hangup_later();
    }
}

/** Make each port the boot line names carry the console, when the device
 * has it and its kind is known. */
static void vcon_ports_try(void)
{
    char name[16];
    int i;

    if (vcon.closed) {
        return;
    }
    for (i = 0; i < VIRTIO_CONSOLE_PORTS; i++) {
        struct vcon_port *p = &vcon.port[i];

        if (!p->present || !p->rxq || !p->txq || p->out || !vcon_wanted(p)) {
            continue;
        }
        if (vcon_attach(p) != 0) {
            vcon_port_name(p, name);
            klog(KLOG_WARN, "console", "refused: %s: no memory for its buffers", name);
        }
    }
}

/**
 * Act on one control message, `m`, of `len` bytes with what follows it, as
 * Linux's handle_control_message: a port added is answered with
 * PORT_READY, a console port is given its hvc number and answered with
 * PORT_OPEN, and a port's removal, size, name and the host's open are
 * recorded. A size is rows and then columns, as Linux reads it. A port
 * past the driver's is logged and left.
 */
static void vcon_ctl_message(const struct virtio_console_control *m, uint32_t len)
{
    const unsigned char *more = (const unsigned char *)(m + 1);
    uint32_t extra = len - (uint32_t)sizeof(*m), i;
    struct vcon_port *p;

    if (m->id >= VIRTIO_CONSOLE_PORTS) {
        if (m->event == VIRTIO_CONSOLE_PORT_ADD) {
            klog(KLOG_INFO, "virtio_console",
                 "port: %s: port %u left: the driver takes ports 0 to %d", vcon.vdev->name,
                 (unsigned)m->id, VIRTIO_CONSOLE_PORTS - 1);
        }
        return;
    }
    p = &vcon.port[m->id];
    if (!p->rxq || (!p->present && m->event != VIRTIO_CONSOLE_PORT_ADD)) {
        return;
    }
    switch (m->event) {
    case VIRTIO_CONSOLE_PORT_ADD:
        p->present = 1;
        vcon_ctl_reply(p, VCON_DUE_READY);
        break;
    case VIRTIO_CONSOLE_PORT_REMOVE:
        vcon_detach(p, "its port has gone", 1);
        p->present = p->console = p->host_open = 0;
        p->hvc = -1;
        p->ctl_due = 0;
        p->name[0] = '\0';
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
        break;
    case VIRTIO_CONSOLE_PORT_NAME:
        for (i = 0; i < extra && i < sizeof(p->name) - 1u && more[i] != '\0'; i++) {
            p->name[i] = (char)more[i];
        }
        p->name[i] = '\0';
        klog(KLOG_INFO, "virtio_console", "port: %s: port %u is named %s", vcon.vdev->name,
             (unsigned)p->id, p->name);
        break;
    default:
        break;
    }
    vcon_ports_try();
}

/** The device returned a control receive buffer with `len` bytes: give
 * each port the replies it is still owed, act on the message and give the
 * buffer back. From the control queue's interrupt, or the boot's wait,
 * under the big lock with interrupts off; nothing once the device is
 * closed. */
static void vcon_ctl_done(struct virtqueue *vq, void *token, uint32_t len)
{
    struct virtio_sg sg;
    int i;

    if (vcon.closed) {
        return;
    }
    vcon.n.control_in++;
    for (i = 0; i < VIRTIO_CONSOLE_PORTS; i++) {
        if (vcon.port[i].ctl_due) {
            vcon_ctl_owed(&vcon.port[i]);
        }
    }
    if (len >= sizeof(struct virtio_console_control)) {
        vcon_ctl_message((const struct virtio_console_control *)token, len);
    }
    sg.addr = vcon.ctl_rx_addr + (uint64_t)((unsigned char *)token - vcon.ctl_rx);
    sg.len = VCON_CTL_BUF;
    (void)virtqueue_add(vq, &sg, 0, 1, token);
    virtqueue_kick(vq);
}

/** The device's configuration changed: without F_MULTIPORT, port 0's size,
 * when the device has F_SIZE, as Linux's config_work_handler reads it. */
static void vcon_config_changed(struct virtio_dev *d)
{
    struct vcon_port *p = &vcon.port[0];

    if (d != vcon.vdev || vcon.closed || vcon.multiport || !vcon.size) {
        return;
    }
    virtio_config_read(d, VIRTIO_CONSOLE_CFG_COLS, &p->cols, 2);
    virtio_config_read(d, VIRTIO_CONSOLE_CFG_ROWS, &p->rows, 2);
    vcon_resize(p);
}

/* ---- the device ---- */

/** Free the driver's buffers and forget the device, which is reset or can
 * no longer reach memory. */
static void vcon_free(void)
{
    struct virtio_dev *d = vcon.vdev;
    int i, k;

    for (k = 0; k < VIRTIO_CONSOLE_PORTS; k++) {
        struct vcon_port *p = &vcon.port[k];

        for (i = 0; i < VCON_RX_BUFS; i++) {
            virtio_dma_free(d, p->rx[i].data, VCON_BUF);
        }
        for (i = 0; i < VCON_TX_BUFS; i++) {
            virtio_dma_free(d, p->tx[i].data, VCON_BUF);
        }
        virtio_dma_free(d, p->ptx.data, VCON_BUF);
    }
    virtio_dma_free(d, vcon.ctl_rx, VCON_BUF);
    virtio_dma_free(d, vcon.ctl_tx, VCON_BUF);
    k_memset(&vcon, 0, sizeof(vcon));
}

/**
 * Stop the device and let go of it, as Linux's virtcons_remove: the device
 * closed first, so that no handler acts and no port carries the console
 * again, then the console back on the UART, the device reset, its queues
 * and the driver's buffers freed, and the device shown unbound. Under the
 * big lock, in process context. The handlers that may still run until the
 * reset are the device's own, under the big lock too, which find the
 * device closed; the transmit side, which runs without it, leaves each
 * port before `vcon_detach` takes it away, since that waits for its lock.
 */
static void vcon_remove(struct virtio_dev *d)
{
    uint32_t flags = kirq_save();
    int i;

    vcon.closed = 1;
    for (i = 0; i < VIRTIO_CONSOLE_PORTS; i++) {
        vcon_detach(&vcon.port[i], "its device is removed", 1);
    }
    d->config_changed = 0;
    d->removed = 0;
    kirq_restore(flags);
    virtio_teardown(d);
    vcon_free();
    virtio_driver_unbind(d);
}

/** The transport's word that the device is unbound, as Linux's
 * virtcons_remove: the driver stops and gives the device back. */
static void vcon_removed(struct virtio_dev *d)
{
    if (d != vcon.vdev) {
        return;
    }
    klog(KLOG_INFO, "virtio_console", "removed: %s", d->name);
    vcon_remove(d);
    virtio_put(d);
}

/** Set up port `id`'s two queues, `rx` and `rx + 1`, with no interrupt
 * asked for on the transmit queue, which the driver polls, and make its
 * transmit buffers and its panic buffer. 0 or -errno. */
static int vcon_port_queues(uint32_t id, uint16_t rx)
{
    struct vcon_port *p = &vcon.port[id];
    int i, r;

    p->id = id;
    r = virtio_queue_setup(vcon.vdev, rx, vcon_rx_done, p, &p->rxq);
    if (r == 0) {
        r = virtio_queue_setup(vcon.vdev, (uint16_t)(rx + 1u), vcon_tx_done, p, &p->txq);
    }
    if (r != 0) {
        return r;
    }
    (void)virtqueue_irq(p->txq, 0);
    for (i = 0; i < VCON_TX_BUFS; i++) {
        p->tx[i].data = (unsigned char *)virtio_dma_alloc(vcon.vdev, VCON_BUF, &p->tx[i].addr);
        if (!p->tx[i].data) {
            return -ENOMEM;
        }
    }
    p->ptx.data = (unsigned char *)virtio_dma_alloc(vcon.vdev, VCON_BUF, &p->ptx.addr);
    return p->ptx.data ? 0 : -ENOMEM;
}

/** Set up the queues of the device and make the buffers every use of it
 * needs: each port's transmit buffers and, with F_MULTIPORT, the control
 * queues' pages. 0 or -errno; the caller tears down on an error. */
static int vcon_setup(void)
{
    struct virtio_dev *d = vcon.vdev;
    int r;

    r = vcon_port_queues(0, 0);
    if (r != 0 || !vcon.multiport) {
        return r;
    }
    r = virtio_queue_setup(d, 2, vcon_ctl_done, 0, &vcon.ctl_rxq);
    if (r == 0) {
        r = virtio_queue_setup(d, 3, vcon_ctl_sent, 0, &vcon.ctl_txq);
    }
    if (r != 0) {
        return r;
    }
    (void)virtqueue_irq(vcon.ctl_txq, 0);
    vcon.ctl_rx = (unsigned char *)virtio_dma_alloc(d, VCON_BUF, &vcon.ctl_rx_addr);
    vcon.ctl_tx = (struct virtio_console_control *)virtio_dma_alloc(d, VCON_BUF, &vcon.ctl_tx_addr);
    if (!vcon.ctl_rx || !vcon.ctl_tx) {
        return -ENOMEM;
    }
    r = vcon_port_queues(1, 4);
    if (r == -ENOENT && !vcon.port[1].txq) {
        /* A device of one port: its port 1 has no queues. */
        vcon.port[1].rxq = 0;
        r = 0;
    }
    return r;
}

/** The first port the boot line names that carries no console yet, or
 * NULL when each does. */
static const char *vcon_want_unmet(void)
{
    int i, k, met;

    for (i = 0; i < vcon_want_n; i++) {
        met = 0;
        for (k = 0; k < VIRTIO_CONSOLE_PORTS; k++) {
            met |= vcon.port[k].out && vcon_names(vcon_want[i], &vcon.port[k]);
        }
        if (!met) {
            return vcon_want[i];
        }
    }
    return 0;
}

/** Give the control queue its receive buffers, tell the device the driver
 * is ready, and with `boot` wait, at most VCON_BOOT_WAIT_MS, until each
 * port the boot line names carries the console, reaping the control queue
 * with interrupts off. */
static void vcon_ports_start(int boot)
{
    struct virtio_sg sg;
    uint64_t start, limit = vcon_cycles(VCON_BOOT_WAIT_MS);
    uint32_t flags = kirq_save();
    int i;

    for (i = 0; i < VCON_CTL_BUFS && i < (int)vcon.ctl_rxq->size; i++) {
        sg.addr = vcon.ctl_rx_addr + (uint64_t)i * VCON_CTL_BUF;
        sg.len = VCON_CTL_BUF;
        (void)virtqueue_add(vcon.ctl_rxq, &sg, 0, 1, vcon.ctl_rx + (uint32_t)i * VCON_CTL_BUF);
    }
    virtqueue_kick(vcon.ctl_rxq);
    (void)vcon_ctl_send(VIRTIO_CONSOLE_BAD_ID, VIRTIO_CONSOLE_DEVICE_READY, 1);
    start = arch_cycles();
    while (boot && vcon_want_n > 0) {
        /* The reap that makes a port the console's may itself take long: a
         * port is given the log's ring in it. */
        (void)virtqueue_reap(vcon.ctl_rxq);
        if (!vcon_want_unmet()) {
            break;
        }
        if (arch_cycles() - start >= limit) {
            klog(KLOG_WARN, "console", "waiting: %s: %s has not announced it in %d ms",
                 vcon_want_unmet(), vcon.vdev->name, (int)VCON_BOOT_WAIT_MS);
            break;
        }
        touch_nmi_watchdog();
        arch_relax();
    }
    kirq_restore(flags);
}

/** Bind `vdev`, as `virtio_console_bind` says, waiting for the ports the
 * boot line names when `boot` is set. */
static int32_t vcon_bind(struct virtio_dev *vdev, int boot)
{
    uint64_t wanted = VIRTIO_BIT(VIRTIO_CONSOLE_F_SIZE) | VIRTIO_BIT(VIRTIO_CONSOLE_F_MULTIPORT) |
                      VIRTIO_BIT(VIRTIO_CONSOLE_F_EMERG_WRITE);
    uint32_t flags;
    int r, i;

    if (vcon.vdev) {
        klog(KLOG_INFO, "virtio_console", "left: %s: the driver has %s", vdev->name,
             vcon.vdev->name);
        virtio_put(vdev);
        return -EBUSY;
    }
    /* The queues to ask interrupts for depend on a feature read before the
     * negotiation: two for a device of one port, six for ports 0 and 1. */
    r = virtio_init(vdev, wanted,
                    (vdev->ops->get_features(vdev) & VIRTIO_BIT(VIRTIO_CONSOLE_F_MULTIPORT)) ? 6
                                                                                             : 2);
    if (r == 0 && vdev->polled) {
        /* Linux's virtio_pci needs an interrupt as well; input and the
         * control messages after the boot come by nothing else. */
        klog(KLOG_WARN, "virtio_console", "refused: %s: no interrupt, which its input needs",
             vdev->name);
        virtio_teardown(vdev);
        r = -EIO;
    }
    if (r != 0) {
        virtio_put(vdev);
        return r;
    }
    k_memset(&vcon, 0, sizeof(vcon));
    for (i = 0; i < VIRTIO_CONSOLE_PORTS; i++) {
        vcon.port[i].hvc = -1;
        vcon.port[i].tx_cur = -1;
    }
    vcon.vdev = vdev;
    vcon.multiport = virtio_has(vdev, VIRTIO_CONSOLE_F_MULTIPORT);
    vcon.emerg = virtio_has(vdev, VIRTIO_CONSOLE_F_EMERG_WRITE);
    vcon.size = virtio_has(vdev, VIRTIO_CONSOLE_F_SIZE);
    r = virtio_driver_bind(vdev, "virtio_console");
    if (r == 0) {
        r = vcon_setup();
    }
    if (r == 0) {
        vdev->config_changed = vcon_config_changed;
        vdev->removed = vcon_removed;
        r = virtio_ready(vdev);
    }
    if (r != 0) {
        vcon_remove(vdev);
        virtio_put(vdev);
        return r;
    }
    klog(KLOG_INFO, "virtio_console", "bound: %s: %s%s%s", vdev->name,
         vcon.multiport ? "ports 0 and 1 with the control queues" : "one port",
         vcon.size ? ", size" : "", vcon.emerg ? ", emergency write" : "");
    if (vcon.multiport) {
        vcon_ports_start(boot);
    } else {
        flags = kirq_save();
        vcon.port[0].present = 1;
        vcon.port[0].console = 1;
        vcon_hvc_take(&vcon.port[0]);
        if (vcon.size) {
            virtio_config_read(vdev, VIRTIO_CONSOLE_CFG_COLS, &vcon.port[0].cols, 2);
            virtio_config_read(vdev, VIRTIO_CONSOLE_CFG_ROWS, &vcon.port[0].rows, 2);
        }
        vcon_ports_try();
        kirq_restore(flags);
    }
    virtio_driver_bound(vdev);
    return 0;
}

int32_t virtio_console_bind(struct virtio_dev *vdev)
{
    return vcon_bind(vdev, 0);
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
        if (vcon_same(value, "hvc0") ||
            (k == 8 && vcon_begins(value, "vport0p") && value[7] >= '0' &&
             value[7] < '0' + VIRTIO_CONSOLE_PORTS)) {
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
    int32_t rc;
    int i, k, met;

    vcon_read_boot_line();
    if (virtio_driver_register(VIRTIO_ID_CONSOLE, virtio_console_bind) != 0) {
        klog(KLOG_WARN, "virtio_console",
             "not registered: a console bound later through /sys is left");
    }
    rc = virtio_pci_find(VIRTIO_ID_CONSOLE, 0, &vdev);
    if (rc == 0) {
        rc = vcon_bind(vdev, 1);
    }
    for (i = 0; i < vcon_want_n; i++) {
        met = 0;
        for (k = 0; k < VIRTIO_CONSOLE_PORTS; k++) {
            met |= vcon.port[k].out && vcon_names(vcon_want[i], &vcon.port[k]);
        }
        if (!met) {
            klog(KLOG_WARN, "console",
                 "none: console=%s names no port now; the console stays on COM1", vcon_want[i]);
        }
    }
    if (rc == -ENODEV) {
        return 0;
    }
    if (rc != 0) {
        klog(KLOG_WARN, "virtio_console", "refused: error %d", (int)rc);
        return rc;
    }
    if (kcmdline_has("rach2.vconpanic")) {
        virtio_console_panic_test();
    }
    return 1;
}

void virtio_console_shutdown(void)
{
    struct virtio_dev *d = vcon.vdev;
    uint32_t flags;
    int i;

    if (!d || vcon.closed) {
        return;
    }
    serial_flush();
    flags = kirq_save();
    vcon.closed = 1;
    for (i = 0; i < VIRTIO_CONSOLE_PORTS; i++) {
        vcon_detach(&vcon.port[i], "the system stops", 0);
    }
    d->config_changed = 0;
    kirq_restore(flags);
    if (virtio_halt(d) == 0) {
        klog(KLOG_INFO, "virtio_console", "shutdown: %s: reset", d->name);
    } else {
        klog(KLOG_WARN, "virtio_console", "shutdown: %s: the reset did not end", d->name);
    }
}

void virtio_console_panic_test(void)
{
    uint32_t flags = kirq_save();

    if (!vcon_any_out()) {
        kirq_restore(flags);
        klog(KLOG_WARN, "virtio_console", "panic test: left: no port carries the console");
        return;
    }
    /* Held as a processor the panic stops would hold it: not this one's. */
    spin_lock(&vcon_tx_lock);
    k_panic("virtio_console: panic test: the transmit side is held");
}

int virtio_console_node(const char *name)
{
    char mine[16];

    if (!vcon.in) {
        return 0;
    }
    vcon_port_name(vcon.in, mine);
    return vcon_same(name, mine);
}

void virtio_console_counters(struct virtio_console_counters *out)
{
    *out = vcon.n;
}
