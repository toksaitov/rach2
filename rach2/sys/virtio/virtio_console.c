/*
 * virtio_console.c: the virtio console (virtio 1.2, 5.3) over the shared
 * virtio core, as Linux's drivers/char/virtio_console.c with its hvc
 * console: its ports, the control queues, and the port that carries the
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

/* The line's buffers: VCON_RX_BUFS that the device fills, as many as the
 * receive ring of serial.h holds twice over, and VCON_TX_BUFS that the
 * driver fills, each a page. */
#define VCON_BUF     4096u
#define VCON_RX_BUFS 8
#define VCON_TX_BUFS 4

/* The control queue's receive buffers, cut from one page: a message is 8
 * bytes, and a port's name follows PORT_NAME in the same buffer. */
#define VCON_CTL_BUFS 8
#define VCON_CTL_BUF  (VCON_BUF / VCON_CTL_BUFS)

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
 * waits VCON_BOOT_WAIT_MS for the device to announce the port the boot
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

/* A buffer of the line: a page the device reads or writes. */
struct vcon_buf {
    unsigned char *data;
    uint64_t addr;              /* as the device reads it */
    uint32_t len;               /* receive: the bytes the device wrote */
    uint32_t off;               /* receive: those the console has taken */
    volatile unsigned int busy; /* transmit: the device has it */
};

/* A port. Its receive buffers are made when it first becomes the line and
 * stay in its queue until the device is reset. */
struct vcon_port {
    uint32_t id;
    int present;   /* the device has the port */
    int console;   /* a console port: port 0 without F_MULTIPORT, or one
                    * the device marked with CONSOLE_PORT */
    int host_open; /* the device's last PORT_OPEN */
    uint16_t cols, rows;
    char name[32]; /* PORT_NAME's, or empty */
    struct virtqueue *rxq, *txq;
    struct vcon_buf rx[VCON_RX_BUFS];
    int rx_made;
    /* The buffers the device returned whose bytes the console has not all
     * taken, oldest first: indexes into `rx`. */
    uint8_t rx_fifo[VCON_RX_BUFS];
    unsigned int rx_head, rx_n;
};

/*
 * The driver's one device. The ports' and the control queues' state is
 * written under the big lock with interrupts off, by the boot, the
 * interrupt handlers and the removal. `vcon_tx_lock` guards the line's
 * transmit side, `line`, `tx`, `tx_cur` and `tx_fill`, which a processor
 * without the big lock also uses when it writes a report, and is taken
 * with interrupts off. The console's lock (`console_lock`), which guards
 * the receive ring the line feeds, also guards each port's `rx` and its
 * list of returned buffers. A queue's lock is taken inside either, and
 * nothing else is.
 */
static struct vcon {
    struct virtio_dev *vdev; /* NULL while no device is bound */
    int multiport, emerg, size;
    struct vcon_port port[VIRTIO_CONSOLE_PORTS];
    struct virtqueue *ctl_rxq, *ctl_txq;
    unsigned char *ctl_rx;
    struct virtio_console_control *ctl_tx;
    uint64_t ctl_rx_addr, ctl_tx_addr;
    volatile unsigned int ctl_tx_busy;
    struct vcon_port *line; /* the port that is the console's line */
    int line_input;         /* and the console's input comes from it */
    struct vcon_buf tx[VCON_TX_BUFS];
    int tx_cur; /* the buffer `vcon_out` fills, or -1 */
    uint32_t tx_fill;
    int tx_stuck; /* the last wait reached its bound */
    struct virtio_console_counters n;
} vcon;

static struct spinlock vcon_tx_lock = SPINLOCK_INIT_NAMED("virtio_console");

/* The number, plus one, of the processor inside the transmit side, or 0: a
 * record that the transmit itself raises, as the core logs a device's
 * fault, finds it and stays in the log's ring. Only that processor writes
 * its own number. */
static volatile unsigned int vcon_tx_owner;

/* What the boot line asked for: the node's name of the port that is to be
 * the console's line, or empty; whether the console's input is to come
 * from it; and whether the UART stays a console beside it. */
static char vcon_want[16];
static int vcon_want_input, vcon_want_uart;

/* Set while the boot binds the device: a port that becomes the line then
 * is given the log's ring, once (`vcon_replayed`). */
static int vcon_boot, vcon_replayed;

static void vcon_out(char c);
static void vcon_flush(void);
static void vcon_rx_room(void);
static void vcon_rx_discard(void);

static const struct serial_line vcon_serial_line = {vcon_out, vcon_flush, vcon_rx_room,
                                                    vcon_rx_discard};

/** The name of `p`'s node in /dev, in `out` of 16 bytes: `hvc0` for the
 * device's first console port, `hvc1` for a second, and `vport0pN` for
 * port N of another kind, as Linux names them. */
static void vcon_port_name(const struct vcon_port *p, char *out)
{
    static const char hvc[] = "hvc0", vport[] = "vport0p0";

    if (p->console) {
        k_memcpy(out, hvc, sizeof(hvc));
        if (p->id != 0 && vcon.port[0].present && vcon.port[0].console) {
            out[3] = '1';
        }
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

/** How many transmit buffers the device has; `vcon_tx_lock` held. */
static int vcon_tx_busy(void)
{
    int i, n = 0;

    for (i = 0; i < VCON_TX_BUFS; i++) {
        n += vcon.tx[i].busy != 0;
    }
    return n;
}

/**
 * Reap the line's transmit queue until the device has at most `most` of
 * the buffers, waiting as VCON_TX_WAIT_MS describes; `vcon_tx_lock` held.
 * Returns 1 when it has, 0 when the wait reached its bound, which
 * `tx_waits` counts.
 */
static int vcon_tx_wait(int most)
{
    uint64_t start = 0, limit = 0;
    int waiting = 0;

    for (;;) {
        (void)virtqueue_reap(vcon.line->txq);
        if (vcon_tx_busy() <= most) {
            vcon.tx_stuck = 0;
            return 1;
        }
        if (!waiting) {
            waiting = 1;
            limit = vcon_cycles(vcon.tx_stuck ? VCON_TX_STUCK_MS : VCON_TX_WAIT_MS);
            start = arch_cycles();
        } else if (arch_cycles() - start >= limit) {
            vcon.tx_stuck = 1;
            vcon.n.tx_waits++;
            return 0;
        }
        touch_nmi_watchdog();
        arch_relax();
    }
}

/** Give the buffer `vcon_out` filled to the device and tell it;
 * `vcon_tx_lock` held. */
static void vcon_tx_submit(void)
{
    struct vcon_buf *b = &vcon.tx[vcon.tx_cur];
    struct virtio_sg sg;

    sg.addr = b->addr;
    sg.len = vcon.tx_fill;
    b->busy = 1;
    if (virtqueue_add(vcon.line->txq, &sg, 1, 0, b) != 0) {
        b->busy = 0;
        vcon.n.tx_dropped += vcon.tx_fill;
    } else {
        vcon.n.tx_bytes += vcon.tx_fill;
        vcon.n.tx_buffers++;
        virtqueue_kick(vcon.line->txq);
    }
    vcon.tx_cur = -1;
    vcon.tx_fill = 0;
}

/** Choose a free transmit buffer for `vcon_out` to fill, after a wait for
 * the device when it has them all; `vcon_tx_lock` held. Returns 0 when
 * none came back. */
static int vcon_tx_take(void)
{
    int i;

    if (vcon_tx_busy() == VCON_TX_BUFS && !vcon_tx_wait(VCON_TX_BUFS - 1)) {
        return 0;
    }
    for (i = 0; i < VCON_TX_BUFS; i++) {
        if (!vcon.tx[i].busy) {
            vcon.tx_cur = i;
            vcon.tx_fill = 0;
            return 1;
        }
    }
    return 0;
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

/**
 * The line's `out`: put `c` in the buffer being filled, and give the buffer
 * to the device when it is full. A byte that finds no line, or no buffer
 * after the wait for one, is dropped and counted. During a panic that
 * finds the transmit side held, the byte goes to the device's `emerg_wr`
 * where it has one, which needs no queue (virtio 1.2, 5.3.5).
 */
static void vcon_out(char c)
{
    uint32_t flags;

    if (!vcon_tx_enter(&flags)) {
        if (k_panicking() && vcon.emerg && vcon.vdev) {
            uint32_t v = (unsigned char)c;

            virtio_config_write(vcon.vdev, VIRTIO_CONSOLE_CFG_EMERG_WR, &v, 4);
            vcon.n.emerg++;
        }
        return;
    }
    if (!vcon.line || (vcon.tx_cur < 0 && !vcon_tx_take())) {
        vcon.n.tx_dropped++;
    } else {
        vcon.tx[vcon.tx_cur].data[vcon.tx_fill++] = (unsigned char)c;
        if (vcon.tx_fill == VCON_BUF) {
            vcon_tx_submit();
        }
    }
    vcon_tx_leave(flags);
}

/**
 * The line's `flush`: give the device what `vcon_out` took and wait until
 * it has returned every buffer, as Linux's put_chars waits for the host
 * before it returns (`__send_to_port`), so that a reset or a power off
 * that follows the last byte loses none.
 */
static void vcon_flush(void)
{
    uint32_t flags;

    if (!vcon_tx_enter(&flags)) {
        return;
    }
    if (vcon.line) {
        if (vcon.tx_cur >= 0 && vcon.tx_fill > 0) {
            vcon_tx_submit();
        }
        if (vcon_tx_busy() > 0) {
            (void)vcon_tx_wait(0);
        }
    }
    vcon_tx_leave(flags);
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
 * the port's interrupt, under the big lock. The line's go to the console's
 * ring, and the console is then served as the UART's interrupt serves it:
 * a parked reader is woken, or the ring is moved into the discipline, in
 * passes while a pass takes bytes. A port that is not the console's input
 * has its bytes dropped and counted.
 */
static void vcon_rx_done(struct virtqueue *vq, void *token, uint32_t len)
{
    struct vcon_port *p = (struct vcon_port *)vq->priv;
    struct vcon_buf *b = (struct vcon_buf *)token;
    uint32_t flags = console_lock();
    uint32_t moved = 0;
    int pass;

    if (p != vcon.line || !vcon.line_input) {
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
 * port's returned buffers still hold. */
static void vcon_rx_room(void)
{
    uint32_t flags = console_lock();

    if (vcon.line && vcon.line_input) {
        (void)vcon_rx_drain(vcon.line);
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
 * bytes the port's returned buffers still hold. */
static void vcon_rx_discard(void)
{
    if (vcon.line) {
        vcon_rx_drop(vcon.line);
    }
}

/* ---- the control queues ---- */

/** The control transmit buffer came back. */
static void vcon_ctl_sent(struct virtqueue *vq, void *token, uint32_t len)
{
    (void)vq;
    (void)token;
    (void)len;
    vcon.ctl_tx_busy = 0;
}

/**
 * Send the control message `event` with `value` for port `id`, and wait
 * until the device has taken it, at most VCON_CTL_WAIT_MS, as Linux's
 * __send_control_msg waits. Under the big lock with interrupts off.
 * Nothing without F_MULTIPORT.
 */
static void vcon_ctl_send(uint32_t id, uint16_t event, uint16_t value)
{
    struct virtio_console_control *m = vcon.ctl_tx;
    struct virtio_sg sg;
    uint64_t start, limit = vcon_cycles(VCON_CTL_WAIT_MS);

    if (!vcon.multiport || !vcon.ctl_txq || vcon.ctl_tx_busy) {
        return;
    }
    m->id = id;
    m->event = event;
    m->value = value;
    sg.addr = vcon.ctl_tx_addr;
    sg.len = (uint32_t)sizeof(*m);
    vcon.ctl_tx_busy = 1;
    if (virtqueue_add(vcon.ctl_txq, &sg, 1, 0, vcon.ctl_tx) != 0) {
        vcon.ctl_tx_busy = 0;
        return;
    }
    vcon.n.control_out++;
    virtqueue_kick(vcon.ctl_txq);
    start = arch_cycles();
    while (vcon.ctl_tx_busy) {
        (void)virtqueue_reap(vcon.ctl_txq);
        if (arch_cycles() - start >= limit) {
            klog(KLOG_WARN, "virtio_console", "control: %s: event %d not taken in %d ms",
                 vcon.vdev->name, (int)event, (int)VCON_CTL_WAIT_MS);
            break;
        }
        arch_relax();
    }
}

/** Set the console terminal's size to `p`'s when `p` is the console's
 * input and the device gave a size, as Linux's hvc_resize. */
static void vcon_resize(const struct vcon_port *p)
{
    struct tty_winsize ws;

    if (p != vcon.line || !vcon.line_input || p->cols == 0 || p->rows == 0) {
        return;
    }
    ws = console_tty.winsize;
    ws.ws_col = p->cols;
    ws.ws_row = p->rows;
    tty_resize(&console_tty, &ws);
}

/**
 * Make `p` the console's line: its transmit side opened, its receive
 * buffers made and given to the device, the device told the port is open,
 * and the line attached to the console (`serial_line_attach`). While the
 * boot binds the device, on a machine without a UART, the line is then
 * given every record of the log's ring, as Linux's register_console gives
 * a console that no boot console preceded the buffer; with a UART the
 * records so far went there, and the line takes the log from here on.
 * Under the big lock with interrupts off. Returns 0, or -ENOMEM with
 * nothing changed.
 */
static int vcon_attach(struct vcon_port *p)
{
    char name[16];
    uint32_t flags;
    int i;

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
         vcon_want_input ? "" : ", for output alone");
    if (vcon_boot) {
        klog_flush();
    }
    flags = spin_lock_irqsave(&vcon_tx_lock);
    vcon.tx_cur = -1;
    vcon.tx_fill = 0;
    vcon.tx_stuck = 0;
    vcon.line_input = vcon_want_input;
    vcon.line = p;
    spin_unlock_irqrestore(&vcon_tx_lock, flags);
    virtqueue_kick(p->rxq);
    if (!p->console) {
        vcon_ctl_send(p->id, VIRTIO_CONSOLE_PORT_OPEN, 1);
    }
    serial_line_attach(&vcon_serial_line, vcon_want_input, vcon_want_uart);
    if (vcon_boot && !vcon_replayed && !serial_uart_found()) {
        klog_replay();
    }
    vcon_replayed = 1;
    vcon_resize(p);
    return 0;
}

/** Take the console's line away from its port, which has gone or whose
 * device is being removed: the console goes back to the UART. Under the
 * big lock with interrupts off. */
static void vcon_detach(const char *why)
{
    struct vcon_port *p = vcon.line;
    char name[16];
    uint32_t flags;

    if (!p) {
        return;
    }
    vcon_port_name(p, name);
    serial_line_detach(&vcon_serial_line);
    flags = spin_lock_irqsave(&vcon_tx_lock);
    vcon.line = 0;
    vcon.line_input = 0;
    spin_unlock_irqrestore(&vcon_tx_lock, flags);
    vcon_rx_drop(p);
    klog(KLOG_INFO, "console", "disabled: %s: %s", name, why);
}

/** Make the port the boot line names the console's line, when the device
 * has it, its kind is known and no port is the line yet. */
static void vcon_line_try(void)
{
    char name[16];
    int i;

    if (vcon.line || vcon_want[0] == '\0') {
        return;
    }
    for (i = 0; i < VIRTIO_CONSOLE_PORTS; i++) {
        struct vcon_port *p = &vcon.port[i];

        if (!p->present || !p->rxq || !p->txq) {
            continue;
        }
        vcon_port_name(p, name);
        if (vcon_same(name, vcon_want) ||
            (vcon_want[0] == 'v' && vcon_want[7] == (char)('0' + p->id))) {
            if (vcon_attach(p) != 0) {
                klog(KLOG_WARN, "console", "refused: %s: no memory for its buffers", name);
            }
            return;
        }
    }
}

/**
 * Act on one control message, `m`, of `len` bytes with what follows it, as
 * Linux's handle_control_message: a port added is answered with
 * PORT_READY, a console port with PORT_OPEN, and a port's removal, size,
 * name and the host's open are recorded. A size is rows and then columns,
 * as Linux reads it. A port past the driver's is logged and left.
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
        vcon_ctl_send(p->id, VIRTIO_CONSOLE_PORT_READY, 1);
        break;
    case VIRTIO_CONSOLE_PORT_REMOVE:
        if (p == vcon.line) {
            vcon_detach("its port has gone");
        }
        p->present = p->console = p->host_open = 0;
        p->name[0] = '\0';
        break;
    case VIRTIO_CONSOLE_CONSOLE_PORT:
        if (m->value && !p->console) {
            p->console = 1;
            vcon_ctl_send(p->id, VIRTIO_CONSOLE_PORT_OPEN, 1);
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
    vcon_line_try();
}

/** The device returned a control receive buffer with `len` bytes: act on
 * its message and give the buffer back. From the control queue's
 * interrupt, or the boot's wait, under the big lock with interrupts off. */
static void vcon_ctl_done(struct virtqueue *vq, void *token, uint32_t len)
{
    struct virtio_sg sg;

    vcon.n.control_in++;
    if (len >= sizeof(struct virtio_console_control)) {
        vcon_ctl_message((const struct virtio_console_control *)token, len);
    }
    if (!vcon.vdev) {
        return;
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

    if (d != vcon.vdev || vcon.multiport || !vcon.size) {
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

    for (i = 0; i < VCON_TX_BUFS; i++) {
        if (vcon.tx[i].data) {
            virtio_dma_free(d, vcon.tx[i].data, VCON_BUF);
        }
    }
    for (k = 0; k < VIRTIO_CONSOLE_PORTS; k++) {
        for (i = 0; i < VCON_RX_BUFS; i++) {
            if (vcon.port[k].rx[i].data) {
                virtio_dma_free(d, vcon.port[k].rx[i].data, VCON_BUF);
            }
        }
    }
    if (vcon.ctl_rx) {
        virtio_dma_free(d, vcon.ctl_rx, VCON_BUF);
    }
    if (vcon.ctl_tx) {
        virtio_dma_free(d, vcon.ctl_tx, VCON_BUF);
    }
    k_memset(&vcon, 0, sizeof(vcon));
    vcon.tx_cur = -1;
}

/** Stop the device and let go of it: the console back on the UART, the
 * device reset, its queues and the driver's buffers freed, and the device
 * shown unbound. Under the big lock, in process context. */
static void vcon_remove(struct virtio_dev *d)
{
    uint32_t flags = kirq_save();

    vcon_detach("its device is removed");
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
 * asked for on the transmit queue, which the driver polls. 0 or -errno. */
static int vcon_port_queues(uint32_t id, uint16_t rx)
{
    struct vcon_port *p = &vcon.port[id];
    int r;

    p->id = id;
    r = virtio_queue_setup(vcon.vdev, rx, vcon_rx_done, p, &p->rxq);
    if (r == 0) {
        r = virtio_queue_setup(vcon.vdev, (uint16_t)(rx + 1u), vcon_tx_done, p, &p->txq);
    }
    if (r == 0) {
        (void)virtqueue_irq(p->txq, 0);
    }
    return r;
}

/** Set up the queues of the device and make the buffers every use of it
 * needs: the line's transmit buffers and, with F_MULTIPORT, the control
 * queues' pages. 0 or -errno; the caller tears down on an error. */
static int vcon_setup(void)
{
    struct virtio_dev *d = vcon.vdev;
    int i, r;

    r = vcon_port_queues(0, 0);
    if (r != 0) {
        return r;
    }
    for (i = 0; i < VCON_TX_BUFS; i++) {
        vcon.tx[i].data = (unsigned char *)virtio_dma_alloc(d, VCON_BUF, &vcon.tx[i].addr);
        if (!vcon.tx[i].data) {
            return -ENOMEM;
        }
    }
    if (!vcon.multiport) {
        return 0;
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
    if (r == -ENOENT) {
        /* A device of one port: its port 1 has no queues. */
        vcon.port[1].rxq = vcon.port[1].txq = 0;
        r = 0;
    }
    return r;
}

/** Give the control queue its receive buffers, tell the device the driver
 * is ready, and at the boot wait, at most VCON_BOOT_WAIT_MS, until the
 * port the boot line names is the console's line, reaping the control
 * queue with interrupts off. */
static void vcon_ports_start(void)
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
    vcon_ctl_send(VIRTIO_CONSOLE_BAD_ID, VIRTIO_CONSOLE_DEVICE_READY, 1);
    start = arch_cycles();
    while (vcon_boot && vcon_want[0] != '\0') {
        /* The reap that makes the port the line may itself take long: the
         * line is given the log's ring in it. */
        (void)virtqueue_reap(vcon.ctl_rxq);
        if (vcon.line) {
            break;
        }
        if (arch_cycles() - start >= limit) {
            klog(KLOG_WARN, "console", "waiting: %s: %s has not announced it in %d ms", vcon_want,
                 vcon.vdev->name, (int)VCON_BOOT_WAIT_MS);
            break;
        }
        touch_nmi_watchdog();
        arch_relax();
    }
    kirq_restore(flags);
}

int32_t virtio_console_bind(struct virtio_dev *vdev)
{
    uint64_t wanted = VIRTIO_BIT(VIRTIO_CONSOLE_F_SIZE) | VIRTIO_BIT(VIRTIO_CONSOLE_F_MULTIPORT) |
                      VIRTIO_BIT(VIRTIO_CONSOLE_F_EMERG_WRITE);
    uint32_t flags;
    int r;

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
    if (r != 0) {
        virtio_put(vdev);
        return r;
    }
    k_memset(&vcon, 0, sizeof(vcon));
    vcon.tx_cur = -1;
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
        vcon_ports_start();
    } else {
        flags = kirq_save();
        vcon.port[0].present = 1;
        vcon.port[0].console = 1;
        if (vcon.size) {
            virtio_config_read(vdev, VIRTIO_CONSOLE_CFG_COLS, &vcon.port[0].cols, 2);
            virtio_config_read(vdev, VIRTIO_CONSOLE_CFG_ROWS, &vcon.port[0].rows, 2);
        }
        vcon_line_try();
        kirq_restore(flags);
    }
    virtio_driver_bound(vdev);
    return 0;
}

/**
 * Read the boot line's `console=` words, each in turn: the port the
 * console's line is to be, whether its input is the console's, which the
 * last word that names a line decides, and whether the UART stays a
 * console. A value's options after a comma are not read, and a word that
 * names neither a port nor the UART, such as `tty0`, changes nothing.
 */
static void vcon_read_boot_line(void)
{
    char value[32];
    int i, n = kcmdline_count("console");

    vcon_want[0] = '\0';
    vcon_want_input = vcon_want_uart = 0;
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
            k_memcpy(vcon_want, value, k + 1u);
            vcon_want_input = 1;
        } else if (vcon_begins(value, "ttyS")) {
            vcon_want_uart = 1;
            vcon_want_input = 0;
        }
    }
}

int32_t virtio_console_start(void)
{
    struct virtio_dev *vdev;
    int32_t rc;

    vcon.tx_cur = -1;
    vcon_read_boot_line();
    if (virtio_driver_register(VIRTIO_ID_CONSOLE, virtio_console_bind) != 0) {
        klog(KLOG_WARN, "virtio_console",
             "not registered: a console bound later through /sys is left");
    }
    rc = virtio_pci_find(VIRTIO_ID_CONSOLE, 0, &vdev);
    if (rc == 0) {
        vcon_boot = 1;
        rc = virtio_console_bind(vdev);
        vcon_boot = 0;
    }
    if (vcon_want[0] != '\0' && !vcon.line) {
        klog(KLOG_WARN, "console", "none: console=%s names no port now; the console stays on COM1",
             vcon_want);
    }
    if (rc == -ENODEV) {
        return 0;
    }
    if (rc != 0) {
        klog(KLOG_WARN, "virtio_console", "refused: error %d", (int)rc);
        return rc;
    }
    return 1;
}

int virtio_console_node(const char *name)
{
    char mine[16];

    if (!vcon.line || !vcon.line_input) {
        return 0;
    }
    vcon_port_name(vcon.line, mine);
    return vcon_same(name, mine);
}

void virtio_console_counters(struct virtio_console_counters *out)
{
    *out = vcon.n;
}
