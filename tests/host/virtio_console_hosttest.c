/*
 * virtio_console_hosttest.c: the virtio console driver,
 * rach2/sys/virtio/virtio_console.c, with the serial console's line of
 * rach2/sys/arch/x86/serial.c and the virtio core, against a model of
 * QEMU's virtio-serial device reached only through the transport's table.
 *
 * host-deps: rach2/sys/virtio/virtqueue.c
 * host-deps: rach2/sys/cmdline.c rach2/sys/lib/kstd.c
 * host-deps: rach2/sys/lib/kstring.c rach2/sys/lib/kctypes.c
 * host-deps: tests/host/compat/kheap_host.c tests/host/compat/spinlock_host.c
 *
 * The model is the device as QEMU's hw/char/virtio-serial-bus.c makes it:
 * port 0, a console (`virtconsole`), and port 1, a generic port
 * (`virtserialport`) with a name, each with a host's end that reads what the
 * port's transmit queue carries and writes into its receive queue; the
 * control queues, on which the device announces its ports once the driver
 * is ready and tells each port's kind, name and open end once the driver
 * has it; a port whose end the driver has not opened is given no input, and
 * one whose host's end is closed has its output discarded; the size and the
 * emergency write of the configuration. A row may make the host's end stop
 * taking for a number of the driver's reads of the clock or for ever, make
 * the device take a buffer only some reads after the kick, keep the device
 * silent after DEVICE_READY, and fail the Nth allocation. The driver's
 * clock, `arch_cycles`, advances 200 ms a read, and the model's time with
 * it. The UART is a transmitter that is always ready and whose bytes and
 * interrupt enable the test keeps.
 *
 * The rows, each on a device with and without VIRTIO_F_EVENT_IDX: the boot
 * on `console=vport0p1`, with the handshake's messages in order, the six
 * queues, the log's ring replayed to the port on a machine without a UART
 * and not on one with it, and the nodes; `console=hvc0` on the same device
 * and on one of a single port, with the size of its configuration and a
 * change of it; the `console=` words, each in turn, with a line for output
 * alone beside the UART; output of 100,000 bytes to a device that takes
 * late and to a host that stops reading for a while, with no byte lost and
 * no interrupt asked for on the transmit queue; a host that stops for ever,
 * which costs one wait of 30 s, drops what the four buffers cannot hold and
 * waits 2 ms a byte after, until the host reads again; input, in order,
 * with a full ring holding the port's buffers back and the host's end with
 * them, a take that makes room, and a flush that drops what is held; a
 * console port's RESIZE; a port removed and added again; the device removed
 * through the transport and bound again through the driver's probe; a
 * panic that finds the transmit side held, whose bytes go to `emerg_wr`; a
 * second device, refused; each allocation failing in turn; a device that
 * announces its port late; one without port 1; and a boot with no device.
 *
 * Returns 0 when every row passes, 1 otherwise.
 */

#include <stdarg.h>

extern int printf(const char *format, ...);
extern int vsnprintf(char *out, unsigned long size, const char *fmt, va_list ap);
extern int snprintf(char *out, unsigned long size, const char *fmt, ...);
extern char *strstr(const char *haystack, const char *needle);
extern int memcmp(const void *a, const void *b, unsigned long n);
extern unsigned long strlen(const char *s);

#include "virtio/virtio_console.c"
#include "arch/x86/serial.c"
#include "mm/kheap.h"

#define ARENA_PAGES 64
#define DEV_BASE    0x40000000ull
#define NQ          6
#define OUT_MAX     (256u * 1024u)
#define IN_MAX      (64u * 1024u)
#define STEP_CYCLES 1000000000ull /* 200 ms of VCON_CYCLES_PER_MS */

static int failures;
static const char *mode = "";

/** Record a failed invariant. */
static void check(int ok, const char *what)
{
    if (!ok) {
        printf("virtio-console-hosttest: fail: %s%s\n", what, mode);
        failures++;
    }
}

/* ---- what surrounds the driver ---- */

static char logged[16384];
static unsigned int nlogged;
static int flushes, replays;
static int panicking;
static uint64_t cycles;
static int resize_calls;
static struct tty_winsize resized;
static unsigned char uart_out[4096];
static unsigned int nuart;
static int uart_ier = -1;
static int notified;

struct tty console_tty;

/** Keep each record as `subsystem: text` and a newline. */
void klog(int level, const char *subsystem, const char *format, ...)
{
    va_list ap;
    int n;

    (void)level;
    n = snprintf(logged + nlogged, sizeof(logged) - nlogged, "%s: ", subsystem);
    nlogged += (unsigned int)n;
    va_start(ap, format);
    n = vsnprintf(logged + nlogged, sizeof(logged) - nlogged - 2u, format, ap);
    va_end(ap);
    nlogged += (unsigned int)n;
    logged[nlogged++] = '\n';
    logged[nlogged] = '\0';
    if (nlogged > sizeof(logged) - 512u) {
        nlogged = 0;
    }
}

/** Count a flush of the log to the console. */
void klog_flush(void)
{
    flushes++;
}

/** Count a replay, and write a word through the console as the ring's
 * records go, so that a row sees which line took it. */
void klog_replay(void)
{
    const char *s = "REPLAY\r\n";

    replays++;
    while (*s) {
        serial_out(*s++);
    }
    serial_flush();
}

/** No delay. */
void k_udelay(uint32_t usec)
{
    (void)usec;
}

/** Nothing to fence. */
void arch_dma_fence(void)
{}

/** One processor, its interrupts as the rows leave them. */
uint32_t kirq_save(void)
{
    return 0;
}

/** Nothing to put back. */
void kirq_restore(uint32_t flags)
{
    (void)flags;
}

/** The console's lock, which one processor always has. */
uint32_t console_lock(void)
{
    return 0;
}

/** Nothing, as `console_lock` took nothing. */
void console_unlock(uint32_t flags)
{
    (void)flags;
}

/** Nothing: the clock's reads move the model's time. */
void arch_relax(void)
{}

/** Nothing to touch. */
void touch_nmi_watchdog(void)
{}

/** Whether the row runs a panic. */
int k_panicking(void)
{
    return panicking;
}

/** Keep the size the driver set. */
void tty_resize(struct tty *t, const struct tty_winsize *ws)
{
    resize_calls++;
    resized = *ws;
    t->winsize = *ws;
}

/** The UART's registers written: its bytes and its interrupt enable. */
void outb(uint16_t port, uint8_t val)
{
    if (port == SERIAL_COM1 + SERIAL_DATA && nuart < sizeof(uart_out)) {
        uart_out[nuart++] = val;
    } else if (port == SERIAL_COM1 + SERIAL_INTR_ENABLE) {
        uart_ier = val;
    }
}

/** A transmitter that is always ready and a receiver with nothing. */
uint8_t inb(uint16_t port)
{
    return port == SERIAL_COM1 + SERIAL_LINE_STATUS ? SERIAL_LSR_THRE : 0;
}

/** Nothing: no row raises the UART's interrupt. */
void pic_unmask_irq(uint8_t irq)
{
    (void)irq;
}

/** Nothing: no row installs the UART's handler. */
void set_isr_handler(uint8_t vector, isr_handler_t handler, const char *name)
{
    (void)vector;
    (void)handler;
    (void)name;
}

/** The console's service, which here only counts: a reader that is not
 * yet running. */
static void rx_notify(void)
{
    notified++;
}

/* ---- the model device ---- */

struct model_queue {
    int enabled;
    uint16_t size;
    uint64_t desc, avail, used;
    uint16_t last_avail, used_idx;
    uint32_t notified, raised;
};

struct model_port {
    int exists;
    int console;      /* a virtconsole; otherwise a virtserialport */
    const char *name; /* told with PORT_NAME, or NULL */
    int host_open;    /* the host's end is connected */
    int guest_open;   /* the driver's last PORT_OPEN */
    int ready;        /* the driver's PORT_READY */
    int throttled;    /* the host's end takes nothing */
    unsigned char out[OUT_MAX];
    uint32_t nout;      /* what the host's end read */
    uint32_t discarded; /* output with the host's end closed */
    unsigned char in[IN_MAX];
    uint32_t nin, in_at; /* what the host's end wrote, and how far the
                          * device has given it to the driver */
};

struct model_msg {
    struct virtio_console_control c;
    unsigned char extra[32];
    uint32_t nextra;
};

static struct model {
    struct virtio_dev dev;
    int plugged;
    uint8_t status;
    uint64_t offered, driver;
    uint8_t config[12];
    unsigned char emerg[64];
    uint32_t nemerg;
    uint16_t qmax[NQ];
    struct model_queue q[NQ];
    uint16_t irq_nqueues;
    int irq_starts, irq_releases;
    int dma_out, dma_calls;
    int dma_fail_at;   /* the allocation, from 0, that fails; -1 none */
    int dma_fail_from; /* the first of the allocations that all fail; -1 none */
    int puts;
    struct virtio_dev *last_put;
    struct model_port port[3];
    struct model_msg pending[32]; /* control messages not yet given */
    int npending;
    struct virtio_console_control got[64]; /* those the driver sent */
    int ngot;
    int ready;     /* DEVICE_READY came */
    int silent;    /* and is not answered */
    int tx_lag;    /* clock reads from a kick to the buffer's taking */
    int tx_due;    /* reads left of them */
    int resume_in; /* clock reads until every host's end takes again */
    const char *bound_driver;
    int binds, bounds, unbinds;
    int bind_result;
    int shorts; /* buffers the device found that were not one descriptor */
} m;

static uint8_t arena[ARENA_PAGES][4096] __attribute__((aligned(4096)));
static uint8_t arena_taken[ARENA_PAGES];
static uint8_t arena_given[ARENA_PAGES]; /* a queue's memory, or a buffer put in one */
static virtio_probe_t registered;
static uint16_t registered_id;
static int event_idx;

/** The host byte at device address `addr`, `len` bytes within one page the
 * driver was given; anything else fails the row and gives NULL. */
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

/** Mark the page of device address `addr` as one the device was given. */
static void given(uint64_t addr)
{
    uint64_t page = (addr - DEV_BASE) / 4096u;

    if (addr >= DEV_BASE && page < ARENA_PAGES) {
        arena_given[page] = 1;
    }
}

/** Mark every buffer the driver has put in queue `qi` and the device has
 * not yet taken. */
static void model_mark(int qi)
{
    struct model_queue *q = &m.q[qi];
    uint16_t *avail, at;

    if (!q->enabled) {
        return;
    }
    avail = (uint16_t *)(void *)dev_ptr(q->avail, 4u + 2u * q->size);
    for (at = q->last_avail; avail && at != avail[1]; at++) {
        uint8_t *p = dev_ptr(q->desc + 16u * avail[2 + (at & (q->size - 1u))], 16);
        struct virtq_desc dsc;

        if (p) {
            k_memcpy(&dsc, p, sizeof(dsc));
            given(dsc.addr);
        }
    }
}

/** Take the next buffer of queue `qi`: its head in `*head`, its address
 * and length. 0 when the driver has given none. */
static int model_pop(int qi, uint16_t *head, uint64_t *addr, uint32_t *len)
{
    struct model_queue *q = &m.q[qi];
    uint16_t *avail;
    struct virtq_desc dsc;
    uint8_t *p, *used;

    if (!q->enabled) {
        return 0;
    }
    avail = (uint16_t *)(void *)dev_ptr(q->avail, 4u + 2u * q->size);
    if (!avail || avail[1] == q->last_avail) {
        return 0;
    }
    *head = avail[2 + (q->last_avail & (q->size - 1u))];
    q->last_avail++;
    if (m.driver & VIRTIO_BIT(VIRTIO_F_EVENT_IDX)) {
        used = dev_ptr(q->used, 4u + 8u * q->size);
        if (used) {
            k_memcpy(used + 4u + 8u * q->size, &q->last_avail, 2);
        }
    }
    p = dev_ptr(q->desc + 16u * *head, 16);
    k_memset(&dsc, 0, sizeof(dsc));
    if (p) {
        k_memcpy(&dsc, p, sizeof(dsc));
    }
    if (dsc.flags & (VIRTQ_DESC_F_NEXT | VIRTQ_DESC_F_INDIRECT)) {
        m.shorts++;
    }
    *addr = dsc.addr;
    *len = dsc.len;
    return 1;
}

/** Return the buffer `head` of queue `qi` with `len` bytes written, and
 * raise the queue's interrupt as the driver's ring asks. */
static void model_push(int qi, uint16_t head, uint32_t len)
{
    struct model_queue *q = &m.q[qi];
    uint16_t *avail = (uint16_t *)(void *)dev_ptr(q->avail, 4u + 2u * q->size);
    uint8_t *used = dev_ptr(q->used, 4u + 8u * q->size);
    uint32_t slot = q->used_idx & (q->size - 1u), id = head;

    if (!avail || !used) {
        return;
    }
    k_memcpy(used + 4u + 8u * slot, &id, 4);
    k_memcpy(used + 8u + 8u * slot, &len, 4);
    q->used_idx++;
    k_memcpy(used + 2u, &q->used_idx, 2);
    if (m.driver & VIRTIO_BIT(VIRTIO_F_EVENT_IDX)) {
        q->raised += (uint16_t)(q->used_idx - avail[2u + q->size] - 1u) < 1u;
    } else {
        q->raised += !(avail[0] & VIRTQ_AVAIL_F_NO_INTERRUPT);
    }
}

/** The receive queue of port `id`; its transmit queue is the next. */
static int port_rxq(int id)
{
    return id == 0 ? 0 : 2 * id + 2;
}

/** Add a control message for the driver, with `extra` after it. */
static void post(uint32_t id, uint16_t event, uint16_t value, const void *extra, uint32_t nextra)
{
    struct model_msg *x = &m.pending[m.npending++];

    x->c.id = id;
    x->c.event = event;
    x->c.value = value;
    x->nextra = nextra;
    if (nextra) {
        k_memcpy(x->extra, extra, nextra);
    }
}

/** Give the driver the control messages it has buffers for. */
static void model_ctl_give(void)
{
    uint16_t head;
    uint64_t addr;
    uint32_t len;
    int i = 0, k;

    while (i < m.npending && model_pop(2, &head, &addr, &len)) {
        struct model_msg *x = &m.pending[i++];
        uint8_t *p = dev_ptr(addr, len);

        check(len >= 8u + x->nextra, "a control buffer holds a message and what follows it");
        if (p) {
            k_memcpy(p, &x->c, 8);
            k_memcpy(p + 8, x->extra, x->nextra);
        }
        model_push(2, head, 8u + x->nextra);
    }
    for (k = i; k < m.npending; k++) {
        m.pending[k - i] = m.pending[k];
    }
    m.npending -= i;
}

/** Announce port `id`, as the device does at DEVICE_READY and at a hot
 * plug. */
static void announce(int id)
{
    m.port[id].ready = 0;
    m.port[id].guest_open = 0;
    post((uint32_t)id, VIRTIO_CONSOLE_PORT_ADD, 1, 0, 0);
}

/** Take the driver's control messages and answer them as QEMU does. */
static void model_ctl_take(void)
{
    uint16_t head;
    uint64_t addr;
    uint32_t len;
    int i;

    while (model_pop(3, &head, &addr, &len)) {
        struct virtio_console_control c;
        uint8_t *p = dev_ptr(addr, len);
        struct model_port *port;

        k_memset(&c, 0, sizeof(c));
        check(len == 8u, "a control message of the driver's is 8 bytes");
        if (p && len >= 8u) {
            k_memcpy(&c, p, 8);
        }
        if (m.ngot < 64) {
            m.got[m.ngot++] = c;
        }
        model_push(3, head, 0);
        if (c.event == VIRTIO_CONSOLE_DEVICE_READY) {
            m.ready = c.value;
            for (i = 0; i < 3 && c.value && !m.silent; i++) {
                if (m.port[i].exists) {
                    announce(i);
                }
            }
            continue;
        }
        if (c.id >= 3u || !m.port[c.id].exists) {
            continue;
        }
        port = &m.port[c.id];
        if (c.event == VIRTIO_CONSOLE_PORT_READY && c.value) {
            port->ready = 1;
            if (port->console) {
                post(c.id, VIRTIO_CONSOLE_CONSOLE_PORT, 1, 0, 0);
            }
            if (port->name) {
                post(c.id, VIRTIO_CONSOLE_PORT_NAME, 1, port->name, (uint32_t)strlen(port->name));
            }
            if (port->host_open) {
                post(c.id, VIRTIO_CONSOLE_PORT_OPEN, 1, 0, 0);
            }
        } else if (c.event == VIRTIO_CONSOLE_PORT_OPEN) {
            port->guest_open = c.value;
        }
    }
    model_ctl_give();
}

/** Take what the ports' transmit queues carry to each host's end that
 * reads, and discard it where the end is closed. */
static void model_tx(void)
{
    uint16_t head;
    uint64_t addr;
    uint32_t len;
    int id;

    for (id = 0; id < 2; id++) {
        struct model_port *port = &m.port[id];
        int qi = port_rxq(id) + 1;

        while ((!port->host_open || !port->throttled) && model_pop(qi, &head, &addr, &len)) {
            uint8_t *p = dev_ptr(addr, len);

            if (!port->host_open) {
                port->discarded += len;
            } else if (p && port->nout + len <= OUT_MAX) {
                k_memcpy(port->out + port->nout, p, len);
                port->nout += len;
            } else {
                check(0, "the model's host end has room for the row's output");
            }
            model_push(qi, head, 0);
        }
    }
}

/** Give the driver what each host's end wrote, a buffer at a time, on a
 * port whose end the driver opened, or on any port without F_MULTIPORT. */
static void model_rx(void)
{
    uint16_t head;
    uint64_t addr;
    uint32_t len;
    int id;

    for (id = 0; id < 2; id++) {
        struct model_port *port = &m.port[id];
        int qi = port_rxq(id);

        if ((m.driver & VIRTIO_BIT(VIRTIO_CONSOLE_F_MULTIPORT)) && !port->guest_open) {
            continue;
        }
        while (port->in_at < port->nin && model_pop(qi, &head, &addr, &len)) {
            uint32_t n = port->nin - port->in_at;
            uint8_t *p = dev_ptr(addr, len);

            if (n > len) {
                n = len;
            }
            if (p) {
                k_memcpy(p, port->in + port->in_at, n);
            }
            port->in_at += n;
            model_push(qi, head, n);
        }
    }
}

/** The host's end of port `id` writes `n` bytes. */
static void host_write(int id, const void *buf, uint32_t n)
{
    struct model_port *port = &m.port[id];

    if (port->in_at == port->nin) {
        port->in_at = port->nin = 0;
    }
    if (port->nin + n > IN_MAX) {
        check(0, "the model's host end holds what the row writes");
        return;
    }
    k_memcpy(port->in + port->nin, buf, n);
    port->nin += n;
    model_rx();
}

/** The driver's clock: 200 ms a read, with the model's time. */
uint64_t arch_cycles(void)
{
    cycles += STEP_CYCLES;
    if (m.resume_in > 0 && --m.resume_in == 0) {
        m.port[0].throttled = m.port[1].throttled = 0;
        model_tx();
    }
    if (m.tx_due > 0 && --m.tx_due == 0) {
        model_tx();
    }
    return cycles;
}

/** The record behind `d`: the one model. */
static struct model *mod(struct virtio_dev *d)
{
    (void)d;
    return &m;
}

/** The status byte. */
static uint8_t m_get_status(struct virtio_dev *d)
{
    return mod(d)->status;
}

/** Write the status byte; 0 resets the device and its queues. */
static void m_set_status(struct virtio_dev *d, uint8_t status)
{
    struct model *x = mod(d);
    int i;

    if (status == 0) {
        x->driver = 0;
        x->ready = 0;
        x->npending = 0;
        for (i = 0; i < NQ; i++) {
            k_memset(&x->q[i], 0, sizeof(x->q[i]));
        }
        for (i = 0; i < 3; i++) {
            x->port[i].ready = x->port[i].guest_open = 0;
        }
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

/** A configuration that does not change under a read. */
static uint32_t m_generation(struct virtio_dev *d)
{
    (void)d;
    return 0;
}

/** Copy out of the configuration. */
static void m_config_read(struct virtio_dev *d, uint32_t offset, void *buf, uint32_t len)
{
    k_memcpy(buf, mod(d)->config + offset, len);
}

/** A write of `emerg_wr` is a byte of output; nothing else is writable. */
static void m_config_write(struct virtio_dev *d, uint32_t offset, const void *buf, uint32_t len)
{
    struct model *x = mod(d);

    check(offset == VIRTIO_CONSOLE_CFG_EMERG_WR && len == 4u,
          "the driver writes only emerg_wr, 32 bits");
    if (x->nemerg < sizeof(x->emerg)) {
        x->emerg[x->nemerg++] = *(const unsigned char *)buf;
    }
}

/** The size the row gave queue `index`. */
static uint16_t m_queue_max(struct virtio_dev *d, uint16_t index)
{
    return index < NQ ? mod(d)->qmax[index] : 0;
}

/** Take the queue's size and addresses. */
static int m_queue_enable(struct virtio_dev *d, struct virtqueue *vq)
{
    struct model_queue *q = &mod(d)->q[vq->index];

    q->enabled = 1;
    q->size = vq->size;
    q->desc = vq->desc_addr;
    q->avail = vq->avail_addr;
    q->used = vq->used_addr;
    given(q->desc);
    given(q->desc + 16u * q->size - 1u);
    given(q->avail);
    given(q->used);
    given(q->used + 4u + 8u * q->size);
    return 0;
}

/** A kick: the control queues are served at once, the receive queues are
 * filled, and a transmit queue is served at once or `tx_lag` reads of the
 * clock later. */
static void m_notify(struct virtio_dev *d, struct virtqueue *vq)
{
    struct model *x = mod(d);

    x->q[vq->index].notified++;
    model_mark(vq->index);
    if (vq->index == 2 || vq->index == 3) {
        model_ctl_take();
    } else if (vq->index == 0 || vq->index == 4) {
        model_rx();
    } else if (x->tx_lag > 0) {
        x->tx_due = x->tx_lag;
    } else {
        model_tx();
    }
}

/** Keep the count of queues the driver asked interrupts for. */
static int m_irq_setup(struct virtio_dev *d, uint16_t nqueues)
{
    mod(d)->irq_nqueues = nqueues;
    return 0;
}

/** Count a start. */
static void m_irq_start(struct virtio_dev *d)
{
    mod(d)->irq_starts++;
}

/** Count a release. */
static void m_irq_release(struct virtio_dev *d)
{
    mod(d)->irq_releases++;
}

/** A page of the arena at its device address, unless the row fails this
 * allocation. */
static void *m_dma_alloc(struct virtio_dev *d, uint32_t bytes, uint64_t *addr)
{
    struct model *x = mod(d);
    int i;

    int call = x->dma_calls++;

    if (call == x->dma_fail_at || (x->dma_fail_from >= 0 && call >= x->dma_fail_from) ||
        bytes == 0 || bytes > 4096u) {
        return 0;
    }
    for (i = 0; i < ARENA_PAGES; i++) {
        if (!arena_taken[i]) {
            arena_taken[i] = 1;
            arena_given[i] = 0;
            k_memset(arena[i], 0, 4096);
            *addr = DEV_BASE + (uint64_t)i * 4096u;
            x->dma_out++;
            return arena[i];
        }
    }
    check(0, "the model's arena holds what the driver allocates");
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
    check(mod(d)->status == 0 || !arena_given[i],
          "no page the device was given is freed before the device is reset");
}

/** No shared memory. */
static int m_shm_region(struct virtio_dev *d, uint8_t id, uint64_t *addr, uint64_t *len)
{
    (void)d;
    (void)id;
    (void)addr;
    (void)len;
    return -ENOENT;
}

/** Count a device given back, and which. */
static void m_put(struct virtio_dev *d)
{
    m.puts++;
    m.last_put = d;
}

static const struct virtio_ops model_ops = {
    "model",      m_get_status,  m_set_status,   m_get_features, m_set_features,
    m_generation, m_config_read, m_config_write, m_queue_max,    m_queue_enable,
    m_notify,     m_irq_setup,   m_irq_start,    m_irq_release,  m_dma_alloc,
    m_dma_free,   m_shm_region,  NULL,           NULL,           NULL,
    NULL,         m_put,         NULL,           NULL,           NULL,
    NULL};

/* ---- the transport's and the bus's calls the driver makes ---- */

/** The model, when the row plugged it in and asks for the first console. */
int virtio_pci_find(uint16_t id, int nth, struct virtio_dev **out)
{
    if (!m.plugged || id != VIRTIO_ID_CONSOLE || nth != 0) {
        return -ENODEV;
    }
    *out = &m.dev;
    return 0;
}

/** Keep the driver's probe. */
int virtio_driver_register(uint16_t id, virtio_probe_t probe)
{
    registered_id = id;
    registered = probe;
    return 0;
}

/** Keep the driver's name; the row's result. */
int virtio_driver_bind(struct virtio_dev *d, const char *driver)
{
    (void)d;
    m.binds++;
    m.bound_driver = driver;
    return m.bind_result;
}

/** Count the bind's uevent. */
void virtio_driver_bound(struct virtio_dev *d)
{
    (void)d;
    m.bounds++;
}

/** Count an unbind. */
void virtio_driver_unbind(struct virtio_dev *d)
{
    (void)d;
    m.unbinds++;
    m.bound_driver = 0;
}

/* ---- the rows' tools ---- */

#define MULTI 1 /* the device offers F_MULTIPORT, with ports 0 and 1 */
#define SIZE  2 /* and F_SIZE */
#define EMERG 4 /* and F_EMERG_WRITE */
#define UART  8 /* the machine has a UART */

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

/** A machine with a fresh model of `kind` plugged in, the boot line
 * `line`, and the driver, the serial console and the counters as a boot
 * finds them. */
static void fresh(const char *line, int kind)
{
    int i;

    drop_bound();
    k_memset(&m, 0, sizeof(m));
    k_memset(arena_taken, 0, sizeof(arena_taken));
    k_memset(arena_given, 0, sizeof(arena_given));
    m.dev.ops = &model_ops;
    m.dev.transport = &m;
    m.dev.name = "virtio0";
    m.dev.id = VIRTIO_ID_CONSOLE;
    m.plugged = 1;
    m.dma_fail_at = m.dma_fail_from = -1;
    m.offered = VIRTIO_BIT(VIRTIO_F_VERSION_1);
    if (event_idx) {
        m.offered |= VIRTIO_BIT(VIRTIO_F_EVENT_IDX);
    }
    if (kind & MULTI) {
        m.offered |= VIRTIO_BIT(VIRTIO_CONSOLE_F_MULTIPORT);
    }
    if (kind & SIZE) {
        m.offered |= VIRTIO_BIT(VIRTIO_CONSOLE_F_SIZE);
    }
    if (kind & EMERG) {
        m.offered |= VIRTIO_BIT(VIRTIO_CONSOLE_F_EMERG_WRITE);
    }
    /* QEMU's sizes: 128 for a port's queues and 32 for the control's. */
    for (i = 0; i < NQ; i++) {
        m.qmax[i] = (uint16_t)((i == 2 || i == 3) ? 32 : 128);
    }
    if (!(kind & MULTI)) {
        m.qmax[2] = m.qmax[3] = m.qmax[4] = m.qmax[5] = 0;
    }
    m.port[0].exists = 1;
    m.port[0].console = 1;
    m.port[0].host_open = 1;
    if (kind & MULTI) {
        m.port[1].exists = 1;
        m.port[1].name = "etude";
        m.port[1].host_open = 1;
        m.config[VIRTIO_CONSOLE_CFG_MAX_PORTS] = 2;
    }
    k_memset(&vcon, 0, sizeof(vcon));
    vcon_tx_lock.locked = 0;
    vcon_tx_owner = 0;
    vcon_boot = vcon_replayed = 0;
    vcon_want[0] = '\0';
    _line = 0;
    _line_input = _line_uart = _line_waits = 0;
    _rx_head = _rx_tail = _rx_looked = _rx_scanned = 0;
    k_memset(&_rx, 0, sizeof(_rx));
    _rx_irq_wanted = _rx_irq_masked = 0;
    _serial_stuck = 0;
    _serial_port_exists = (kind & UART) != 0;
    serial_set_rx_notify(rx_notify);
    k_memset(&console_tty, 0, sizeof(console_tty));
    nlogged = 0;
    logged[0] = '\0';
    flushes = replays = panicking = resize_calls = notified = 0;
    nuart = 0;
    uart_ier = -1;
    registered = 0;
    (void)kcmdline_capture(line);
}

/** Take the interrupts the model raised, as the transport's handler does,
 * until it raises no more. */
static void irq(void)
{
    uint32_t seen = 0, now;
    int i, pass;

    for (pass = 0; pass < 64; pass++) {
        now = 0;
        for (i = 0; i < NQ; i++) {
            now += m.q[i].raised;
        }
        if (now == seen) {
            return;
        }
        seen = now;
        virtio_interrupt_queues(&m.dev);
    }
}

/** Write `n` bytes through the serial console, as the console's sink and
 * the log do. */
static void say(const void *buf, uint32_t n)
{
    const char *s = (const char *)buf;

    while (n-- > 0) {
        serial_out(*s++);
    }
}

/** Whether the host's end of port `id` read exactly `text`. */
static int host_read(int id, const char *text)
{
    uint32_t n = (uint32_t)strlen(text);

    return m.port[id].nout == n && memcmp(m.port[id].out, text, n) == 0;
}

/** Whether the UART sent exactly `text`. */
static int uart_sent(const char *text)
{
    unsigned int n = (unsigned int)strlen(text);

    return nuart == n && memcmp(uart_out, text, n) == 0;
}

/** Whether the driver's control message `i` is `event` with `value` for
 * port `id`. */
static int got(int i, uint32_t id, uint16_t event, uint16_t value)
{
    return i < m.ngot && m.got[i].id == id && m.got[i].event == event && m.got[i].value == value;
}

/** Byte `i` of the rows' pattern, which no power of two repeats. */
static unsigned char pattern(uint32_t i)
{
    return (unsigned char)('!' + i % 89u);
}

static unsigned char big[100000];

/** Fill `big` with the pattern from byte `from`. */
static void fill(uint32_t from, uint32_t n)
{
    uint32_t i;

    for (i = 0; i < n; i++) {
        big[i] = pattern(from + i);
    }
}

/** The driver's counters now. */
static struct virtio_console_counters counters(void)
{
    struct virtio_console_counters c;

    virtio_console_counters(&c);
    return c;
}

/* ---- the rows ---- */

/** The boot on `console=vport0p1`, the harness's line, on a machine with
 * no UART: the handshake, the queues, the replay and the nodes. */
static void boot_port(void)
{
    fresh("root=/dev/ram0 console=vport0p1 quiet", MULTI | EMERG);
    check(virtio_console_start() == 1, "the boot binds the console");
    check(registered == virtio_console_bind && registered_id == VIRTIO_ID_CONSOLE,
          "the driver registers its probe for the console's id");
    check((m.driver & VIRTIO_BIT(VIRTIO_CONSOLE_F_MULTIPORT)) &&
              (m.driver & VIRTIO_BIT(VIRTIO_CONSOLE_F_EMERG_WRITE)) &&
              !(m.driver & VIRTIO_BIT(VIRTIO_CONSOLE_F_SIZE)),
          "the driver takes the features offered among its three");
    check(m.irq_nqueues == 6 && m.q[0].enabled && m.q[1].enabled && m.q[2].enabled &&
              m.q[3].enabled && m.q[4].enabled && m.q[5].enabled,
          "a device of several ports has six queues set up");
    check((m.status & VIRTIO_STATUS_DRIVER_OK) && m.irq_starts == 1, "the device is driven");
    check(m.binds == 1 && m.bound_driver && vcon_same(m.bound_driver, "virtio_console") &&
              m.bounds == 1,
          "the device is shown bound to virtio_console, with one uevent");
    check(got(0, VIRTIO_CONSOLE_BAD_ID, VIRTIO_CONSOLE_DEVICE_READY, 1),
          "the first control message is DEVICE_READY for the device");
    check(got(1, 0, VIRTIO_CONSOLE_PORT_READY, 1) && got(2, 1, VIRTIO_CONSOLE_PORT_READY, 1),
          "each port added is answered with PORT_READY");
    check(got(3, 1, VIRTIO_CONSOLE_PORT_OPEN, 1) && m.port[1].guest_open,
          "the port that becomes the line is opened");
}

/** What the same boot leaves: the node, the log's ring replayed to the
 * port on a machine without a UART, and the messages that came after the
 * boot's wait. */
static void boot_port_line(void)
{
    struct virtio_console_counters c;

    check(virtio_console_node("vport0p1") && !virtio_console_node("hvc0") &&
              !virtio_console_node("vport0p0") && !virtio_console_node("console"),
          "the line's node is vport0p1 alone");
    check(strstr(logged, "console: enabled: vport0p1, port 1 of virtio0\n") != 0,
          "the log names the console's line");
    check(flushes == 1 && replays == 1 && host_read(1, "REPLAY\r\n") && m.port[0].nout == 0,
          "a machine without a UART gives the port the log's ring, once");
    irq();
    check(got(4, 0, VIRTIO_CONSOLE_PORT_OPEN, 1) && m.ngot == 5,
          "the console port, told after the boot's wait, is opened too");
    check(strstr(logged, "virtio_console: port: virtio0: port 1 is named etude\n") != 0,
          "the port's name is kept");
    check(virtio_console_node("vport0p1") && replays == 1, "the line stays as the boot made it");
    c = counters();
    check(c.control_out == 5 && c.control_in == 6 && c.tx_bytes == 8 && c.tx_buffers == 1 &&
              c.tx_dropped == 0 && c.tx_waits == 0,
          "the counters hold the handshake and the replay");
    check(m.shorts == 0, "each buffer is one descriptor");
}

/** `console=hvc0` on the same device with a UART, whose receive interrupt
 * the line takes the place of: no replay, and the console's bytes go to
 * port 0 alone. */
static void boot_hvc(void)
{
    fresh("console=hvc0", MULTI | UART);
    _rx_irq_wanted = 1;
    check(virtio_console_start() == 1, "the boot binds the console");
    check(virtio_console_node("hvc0") && !virtio_console_node("vport0p1"),
          "the line's node is hvc0");
    check(replays == 0 && flushes == 1, "with a UART the log so far is not replayed");
    check(uart_ier == 0x00, "the UART's receive interrupt is off while the line has the input");
    check(got(1, 0, VIRTIO_CONSOLE_PORT_READY, 1) && got(3, 0, VIRTIO_CONSOLE_PORT_OPEN, 1),
          "a console port is opened once the device says it is one");
    say("to the port\n", 12);
    serial_flush();
    check(host_read(0, "to the port\n") && nuart == 0 && m.port[1].nout == 0,
          "the console's bytes go to port 0 and not to the UART");
    check(strstr(logged, "console: enabled: hvc0, port 0 of virtio0\n") != 0,
          "the log names the console's line");
}

/** A device of one port, as QEMU's without a second: two queues, no
 * control messages, and the size of its configuration, then a change. */
static void one_port(void)
{
    uint16_t v;

    fresh("console=hvc0", SIZE);
    v = 132;
    k_memcpy(m.config + VIRTIO_CONSOLE_CFG_COLS, &v, 2);
    v = 43;
    k_memcpy(m.config + VIRTIO_CONSOLE_CFG_ROWS, &v, 2);
    check(virtio_console_start() == 1, "the boot binds the console");
    check(m.irq_nqueues == 2 && m.q[0].enabled && m.q[1].enabled && !m.q[2].enabled &&
              !m.q[4].enabled,
          "a device of one port has two queues");
    check(m.ngot == 0 && counters().control_out == 0, "and no control messages");
    check(virtio_console_node("hvc0") && replays == 1 && host_read(0, "REPLAY\r\n"),
          "port 0 is the console's line and is given the log");
    check(resize_calls == 1 && resized.ws_col == 132 && resized.ws_row == 43,
          "the console terminal takes the size of the configuration");
    v = 100;
    k_memcpy(m.config + VIRTIO_CONSOLE_CFG_COLS, &v, 2);
    v = 30;
    k_memcpy(m.config + VIRTIO_CONSOLE_CFG_ROWS, &v, 2);
    virtio_interrupt_config(&m.dev);
    check(resize_calls == 2 && resized.ws_col == 100 && resized.ws_row == 30,
          "a change of the configuration is a new size");
    host_write(0, "in\n", 3);
    irq();
    check(serial_rx_pending() == 3 && serial_rx_take() == 'i',
          "a port without the control queues needs no open to give input");
}

/** What `console=` words leave: the port asked for, whether the input is
 * its, and whether the UART stays a console. */
static void words(void)
{
    static const struct {
        const char *line, *want;
        int input, uart;
    } rows[] = {
        {"quiet", "", 0, 0},
        {"console=hvc0", "hvc0", 1, 0},
        {"console=ttyS0 console=hvc0", "hvc0", 1, 1},
        {"console=hvc0 console=ttyS0,115200n8", "hvc0", 0, 1},
        {"console=tty0 console=hvc0,9600", "hvc0", 1, 0},
        {"console=vport0p1", "vport0p1", 1, 0},
        {"console=vport0p0", "vport0p0", 1, 0},
        {"console=vport0p1 console=hvc0", "hvc0", 1, 0},
        {"console=vport0p2", "", 0, 0},
        {"console=vport1p1", "", 0, 0},
        {"console=hvc1", "", 0, 0},
        {"console=hvc", "", 0, 0},
        {"console=ttyS0", "", 0, 1},
    };
    unsigned int i;

    for (i = 0; i < sizeof(rows) / sizeof(rows[0]); i++) {
        fresh(rows[i].line, MULTI);
        vcon_read_boot_line();
        if (!vcon_same(vcon_want, rows[i].want) || vcon_want_input != rows[i].input ||
            vcon_want_uart != rows[i].uart) {
            printf("virtio-console-hosttest: words: \"%s\" gave \"%s\" input %d uart %d\n",
                   rows[i].line, vcon_want, vcon_want_input, vcon_want_uart);
            check(0, "a boot line's console= words choose the line");
        }
    }

    /* The UART named last: the port carries output alone. */
    fresh("console=hvc0 console=ttyS0", MULTI | UART);
    _rx_irq_wanted = 1;
    check(virtio_console_start() == 1, "the boot binds the console");
    check(strstr(logged, "console: enabled: hvc0, port 0 of virtio0, for output alone\n") != 0,
          "the log says the line carries output alone");
    check(!virtio_console_node("hvc0"), "a line for output alone has no node");
    check(uart_ier != 0x00, "the UART keeps the console's input");
    say("both", 4);
    serial_flush();
    check(host_read(0, "both") && uart_sent("both"), "the log goes to the port and the UART");
    host_write(0, "lost", 4);
    irq();
    check(serial_rx_pending() == 0 && counters().rx_dropped == 4 && counters().rx_bytes == 0,
          "input on a line for output alone is dropped and counted");
    host_write(0, "more", 4);
    irq();
    check(counters().rx_dropped == 8 && m.port[0].in_at == m.port[0].nin,
          "and its buffers go back to the device");

    /* The UART named first: both carry the log, the port has the input. */
    fresh("console=ttyS0 console=hvc0", MULTI | UART);
    check(virtio_console_start() == 1 && virtio_console_node("hvc0") && replays == 0,
          "the port named last is the console's input");
    say("both", 4);
    serial_flush();
    check(host_read(0, "both") && uart_sent("both"), "the log goes to the port and the UART");
    host_write(0, "k", 1);
    irq();
    check(serial_rx_pending() == 1 && serial_rx_take() == 'k', "the port's input is the console's");
}

/** Output: bytes leave at a full page and at a flush, in order, to a
 * device that takes at once, one that takes late, and a host that stops
 * reading for a while; none is lost and no interrupt is asked for. */
static void output(void)
{
    struct virtio_console_counters c;
    uint32_t i;

    fresh("console=vport0p1", MULTI | UART);
    check(virtio_console_start() == 1, "the boot binds the console");
    fill(0, 10000);
    say(big, 10000);
    check(m.port[1].nout == 8192, "each full page leaves without a flush");
    serial_flush();
    check(m.port[1].nout == 10000 && memcmp(m.port[1].out, big, 10000) == 0,
          "a flush sends the rest, in order");
    c = counters();
    check(c.tx_bytes == 10000 && c.tx_buffers == 3 && c.tx_dropped == 0 && c.tx_waits == 0,
          "the counters hold the bytes and the buffers");
    serial_flush();
    check(counters().tx_buffers == 3, "a flush with nothing to send sends nothing");

    /* A device that takes each buffer three reads of the clock late. */
    m.tx_lag = 3;
    m.port[1].nout = 0;
    fill(0, sizeof(big));
    for (i = 0; i < sizeof(big); i += 1000) {
        say(big + i, 1000);
        if (i % 7000u == 0) {
            serial_flush();
        }
    }
    serial_flush();
    check(m.port[1].nout == sizeof(big) && memcmp(m.port[1].out, big, sizeof(big)) == 0,
          "a device that takes late loses nothing");
    check(counters().tx_dropped == 0 && counters().tx_waits == 0, "and no wait reaches its bound");

    /* A host that stops reading for 8 s, as the harness's reader may. */
    m.tx_lag = 0;
    m.port[1].nout = 0;
    m.port[1].throttled = 1;
    m.resume_in = 40;
    say(big, sizeof(big));
    serial_flush();
    check(m.port[1].nout == sizeof(big) && memcmp(m.port[1].out, big, sizeof(big)) == 0,
          "a host that stops reading for a while loses nothing");
    check(counters().tx_dropped == 0 && counters().tx_waits == 0, "and no wait reaches its bound");
    check(m.q[5].raised == 0 && m.q[3].raised == 0,
          "the transmit queues, which the driver polls, raise no interrupt");
    check(m.port[0].nout == 0 && nuart == 0, "nothing goes to another port or the UART");
}

/** A host that stops reading for ever: one long wait, then short ones,
 * with what the buffers cannot hold dropped and counted, until the host
 * reads again. */
static void stuck(void)
{
    struct virtio_console_counters c;
    uint64_t before;

    fresh("console=vport0p1", MULTI | UART);
    check(virtio_console_start() == 1, "the boot binds the console");
    m.port[1].throttled = 1;
    fill(0, 5u * 4096u);
    before = cycles;
    say(big, 4u * 4096u);
    check(cycles == before && counters().tx_buffers == 4, "four pages go without a wait");
    say(big + 4u * 4096u, 1);
    check(cycles - before >= vcon_cycles(VCON_TX_WAIT_MS) &&
              cycles - before < vcon_cycles(VCON_TX_WAIT_MS) + 2u * STEP_CYCLES,
          "the fifth waits 30 s for a buffer");
    c = counters();
    check(c.tx_waits == 1 && c.tx_dropped == 1, "and its byte is dropped and counted");
    before = cycles;
    say(big + 4u * 4096u + 1u, 100);
    check(cycles - before <= 200u * STEP_CYCLES, "each later byte waits one read of the clock");
    check(counters().tx_dropped == 101 && counters().tx_waits == 101, "and is dropped");
    before = cycles;
    serial_flush();
    check(cycles - before <= 2u * STEP_CYCLES, "a flush waits no longer");
    check(m.port[1].nout == 0, "nothing reached the host's end");

    m.port[1].throttled = 0;
    model_tx();
    before = cycles;
    say("resumed", 7);
    serial_flush();
    check(cycles == before, "once the host reads again nothing waits");
    check(m.port[1].nout == 4u * 4096u + 7u && memcmp(m.port[1].out, big, 4u * 4096u) == 0 &&
              memcmp(m.port[1].out + 4u * 4096u, "resumed", 7) == 0,
          "the four pages and what follows arrive, in order");
    check(counters().tx_dropped == 101, "and nothing more is dropped");
}

/** Input: bytes reach the console's ring in order; a full ring holds the
 * port's buffers back, and the host's end with them; a take makes room;
 * and a flush of the input drops what is held. */
static void input(void)
{
    struct virtio_console_counters c;
    uint32_t taken = 0, bad = 0, i;
    int pass, ch;

    fresh("console=vport0p1", MULTI | UART);
    check(virtio_console_start() == 1, "the boot binds the console");
    host_write(1, "hello\n", 6);
    check(serial_rx_pending() == 0, "nothing arrives without the port's interrupt");
    irq();
    check(serial_rx_pending() == 6 && notified == 1, "an interrupt feeds the ring and serves it");
    for (i = 0; i < 6; i++) {
        check(serial_rx_take() == "hello\n"[i], "the bytes come in order");
    }
    check(serial_rx_take() == -1 && counters().rx_bytes == 6, "and no more");

    /* 40,000 bytes with no reader: the ring's 4,096 and eight pages. */
    fill(0, 40000);
    host_write(1, big, 40000);
    irq();
    c = counters();
    check(serial_rx_pending() == SERIAL_RX_SIZE, "the ring fills");
    check(m.port[1].in_at == SERIAL_RX_SIZE + 8u * VCON_BUF && vcon.port[1].rx_n == VCON_RX_BUFS,
          "the port's eight buffers are held back, and the host's end stops");
    check(c.rx_held > 0 && c.rx_dropped == 0 && c.rx_bytes == 6 + SERIAL_RX_SIZE,
          "the counters say the ring held them");

    for (pass = 0; pass < 64 && taken < 40000; pass++) {
        while ((ch = serial_rx_take()) >= 0) {
            bad += (unsigned char)ch != pattern(taken);
            taken++;
        }
        irq();
    }
    check(taken == 40000 && bad == 0, "a reader that takes gets every byte, in order");
    check(vcon.port[1].rx_n == 0 && counters().rx_bytes == 40006 && counters().rx_dropped == 0,
          "and every buffer is back with the device");

    /* A flush of the input reaches the bytes the buffers hold. */
    fill(0, 40000);
    host_write(1, big, 40000);
    irq();
    check(vcon.port[1].rx_n == VCON_RX_BUFS, "the buffers are held again");
    serial_rx_flush();
    check(serial_rx_pending() == 0 && vcon.port[1].rx_n == 0, "a flush drops the ring and them");
    irq();
    taken = SERIAL_RX_SIZE + 8u * VCON_BUF;
    bad = 0;
    for (pass = 0; pass < 8 && taken < 40000; pass++) {
        while ((ch = serial_rx_take()) >= 0) {
            bad += (unsigned char)ch != pattern(taken);
            taken++;
        }
        irq();
    }
    check(taken == 40000 && bad == 0, "what the host's end had not sent arrives after it");

    host_write(0, "x", 1);
    irq();
    check(m.port[0].in_at == 0 && serial_rx_pending() == 0,
          "a port that is not the line has no buffers and gives no input");
}

/** A console port's RESIZE, rows and then columns as Linux reads it, and
 * one for a port that is no console. */
static void resize(void)
{
    static const unsigned char size[4] = {50, 0, 160, 0};

    fresh("console=hvc0", MULTI | UART);
    check(virtio_console_start() == 1, "the boot binds the console");
    irq();
    post(1, VIRTIO_CONSOLE_RESIZE, 0, size, 4);
    model_ctl_give();
    irq();
    check(resize_calls == 0, "a size for a port that is no console is left");
    post(0, VIRTIO_CONSOLE_RESIZE, 0, size, 4);
    model_ctl_give();
    irq();
    check(resize_calls == 1 && resized.ws_col == 160 && resized.ws_row == 50,
          "the console terminal takes the line's size");
    post(0, VIRTIO_CONSOLE_RESIZE, 0, size, 2);
    post(0, 99, 1, 0, 0);
    post(2, VIRTIO_CONSOLE_PORT_ADD, 1, 0, 0);
    post(7, VIRTIO_CONSOLE_PORT_OPEN, 1, 0, 0);
    model_ctl_give();
    irq();
    check(resize_calls == 1 && virtio_console_node("hvc0"),
          "a short size, an unknown event and a port past the driver's change nothing");
    check(strstr(logged, "port 2 left: the driver takes ports 0 to 1\n") != 0,
          "and the log names the port left");
}

/** The line's port removed and added again: the console goes back to the
 * UART and returns to the port, with no second replay and no new buffers. */
static void replug(void)
{
    int pages;

    fresh("console=vport0p1", MULTI | UART);
    _rx_irq_wanted = 1;
    check(virtio_console_start() == 1, "the boot binds the console");
    irq();
    pages = m.dma_out;
    post(1, VIRTIO_CONSOLE_PORT_REMOVE, 1, 0, 0);
    model_ctl_give();
    irq();
    check(!virtio_console_node("vport0p1") &&
              strstr(logged, "console: disabled: vport0p1: its port has gone\n") != 0,
          "the line's port gone, the console leaves it");
    check(uart_ier == SERIAL_IER_RDA, "the UART's receive interrupt is on again");
    say("uart", 4);
    serial_flush();
    check(uart_sent("uart") && m.port[1].nout == 0, "the console's bytes go to the UART");

    announce(1);
    model_ctl_give();
    irq();
    check(virtio_console_node("vport0p1") && m.port[1].guest_open && replays == 0,
          "the port added again is the line again, with no replay");
    check(m.dma_out == pages, "and keeps the buffers it had");
    say("port", 4);
    serial_flush();
    check(host_read(1, "port") && uart_sent("uart"), "the console's bytes go to the port");
    host_write(1, "in", 2);
    irq();
    check(serial_rx_pending() == 2 && serial_rx_take() == 'i' && serial_rx_take() == 'n',
          "and its input is the console's");
}

/** The device removed through the transport, and bound again through the
 * probe the driver registered. */
static void removal(void)
{
    fresh("console=vport0p1", MULTI | UART);
    check(virtio_console_start() == 1, "the boot binds the console");
    irq();
    host_write(1, "held", 4);
    check(m.dev.removed != 0, "the driver gives the transport its removal");
    m.dev.removed(&m.dev);
    check(m.status == 0 && m.dma_out == 0 && m.irq_releases == 1,
          "the device is reset and every page and interrupt given back");
    check(m.unbinds == 1 && m.puts == 1 && m.last_put == &m.dev && m.dev.removed == 0,
          "the device is shown unbound and given back to the transport");
    check(!virtio_console_node("vport0p1") &&
              strstr(logged, "console: disabled: vport0p1: its device is removed\n") != 0,
          "the console leaves the port");
    say("uart", 4);
    serial_flush();
    check(uart_sent("uart"), "and goes on at the UART");

    check(registered(&m.dev) == 0, "the probe binds the device again");
    check(!virtio_console_node("vport0p1"), "a bind after the boot waits for no port");
    irq();
    check(virtio_console_node("vport0p1") && replays == 0,
          "the port, announced by interrupt, is the line again, with no replay");
    say("port", 4);
    serial_flush();
    check(host_read(1, "port"), "and carries the console");
    check(m.binds == 2 && m.bounds == 2, "the device is shown bound again");
}

/** A panic that finds the transmit side held by a processor it stopped:
 * the bytes go to `emerg_wr` where the device has it, and nowhere where
 * it has not; with the side free they go as ever. */
static void panic(void)
{
    fresh("console=vport0p1", MULTI | EMERG);
    check(virtio_console_start() == 1, "the boot binds the console");
    m.port[1].nout = 0;
    panicking = 1;
    say("free", 4);
    serial_flush();
    check(host_read(1, "free") && m.nemerg == 0, "a panic with the transmit side free uses it");
    vcon_tx_lock.locked = 1;
    say("PANIC", 5);
    serial_flush();
    check(m.nemerg == 5 && memcmp(m.emerg, "PANIC", 5) == 0 && counters().emerg == 5,
          "with the side held, each byte is written to emerg_wr");
    check(host_read(1, "free") && vcon_tx_lock.locked == 1, "and the held side is left alone");
    vcon_tx_lock.locked = 0;
    panicking = 0;

    fresh("console=vport0p1", MULTI);
    check(virtio_console_start() == 1, "the boot binds the console");
    panicking = 1;
    vcon_tx_lock.locked = 1;
    say("PANIC", 5);
    serial_flush();
    check(m.nemerg == 0 && counters().emerg == 0, "a device without emerg_wr is written nothing");
    vcon_tx_lock.locked = 0;
    panicking = 0;
}

/** A second console, which the driver leaves, and a bind the bus refuses. */
static void second(void)
{
    static struct virtio_dev other;

    fresh("console=vport0p1", MULTI | UART);
    check(virtio_console_start() == 1, "the boot binds the console");
    other.ops = &model_ops;
    other.transport = &m;
    other.name = "virtio1";
    other.id = VIRTIO_ID_CONSOLE;
    check(virtio_console_bind(&other) == -EBUSY && m.puts == 1 && m.last_put == &other,
          "a second console is refused and given back");
    check(virtio_console_node("vport0p1") && m.status != 0, "and the first is left as it was");

    fresh("console=vport0p1", MULTI | UART);
    m.bind_result = -ENOMEM;
    check(virtio_console_start() == -ENOMEM, "a bind the bus refuses fails the boot's bind");
    check(m.status == 0 && m.dma_out == 0 && m.puts == 1 && !virtio_console_node("vport0p1"),
          "with the device reset, nothing kept and the device given back");
    check(strstr(logged, "console: none: console=vport0p1 names no port now") != 0,
          "and the log says the console stays on the UART");
}

/**
 * The boot short of memory. With one allocation failing, each in turn,
 * the bring-up fails with nothing kept, or the line, refused once, is made
 * at the device's next message. With every allocation from one on
 * failing, the bring-up fails the same way or the boot goes on without the
 * line. Nothing is ever kept by half, and a removal gives every page back.
 */
static void no_memory(void)
{
    int k, i, all, calls, pages;
    int failed[2] = {0, 0}, retried = 0, without = 0;

    fresh("console=vport0p1", MULTI | UART);
    check(virtio_console_start() == 1 && virtio_console_node("vport0p1"), "the boot has memory");
    calls = m.dma_calls;
    pages = m.dma_out;
    for (all = 0; all < 2; all++) {
        for (k = 0; k < calls; k++) {
            int32_t rc;

            fresh("console=vport0p1", MULTI | UART);
            if (all) {
                m.dma_fail_from = k;
            } else {
                m.dma_fail_at = k;
            }
            rc = virtio_console_start();
            irq();
            if (rc < 0) {
                failed[all]++;
                check(rc == -ENOMEM && m.status == 0 && m.dma_out == 0 && m.puts == 1 &&
                          vcon.vdev == 0,
                      "a bring-up that fails keeps nothing");
                continue;
            }
            check(rc == 1 && (m.status & VIRTIO_STATUS_DRIVER_OK), "the device stays driven");
            check(strstr(logged, "console: refused: vport0p1: no memory for its buffers\n") != 0,
                  "the log says the line was refused");
            if (all) {
                without++;
                check(!virtio_console_node("vport0p1"), "the boot goes on without the line");
                for (i = 0; i < VCON_RX_BUFS; i++) {
                    check(vcon.port[1].rx[i].data == 0, "a line refused keeps no buffer");
                }
                say("uart", 4);
                serial_flush();
                check(uart_sent("uart"), "and the console stays on the UART");
            } else {
                retried++;
                check(virtio_console_node("vport0p1") && m.dma_out == pages,
                      "the device's next message makes the line, with every page it needs");
                check(!strstr(logged, "console: waiting"), "within the boot's wait");
            }
            m.dev.removed(&m.dev);
            check(m.dma_out == 0, "the removal gives every page back");
        }
    }
    check(failed[0] == calls - VCON_RX_BUFS && failed[1] == failed[0] && retried == VCON_RX_BUFS &&
              without == VCON_RX_BUFS,
          "every allocation but the line's buffers fails the bring-up");
}

/** A device that announces its ports only after the boot's wait, one
 * without port 1, and a machine with no console at all. */
static void late_and_absent(void)
{
    uint64_t before;
    int i;

    fresh("console=vport0p1", MULTI | UART);
    m.silent = 1;
    before = cycles;
    check(virtio_console_start() == 1, "the boot binds a device that announces nothing");
    check(cycles - before >= vcon_cycles(VCON_BOOT_WAIT_MS) &&
              cycles - before <= vcon_cycles(VCON_BOOT_WAIT_MS) + 4u * STEP_CYCLES,
          "after a wait of 2 s for the port");
    check(strstr(logged, "console: waiting: vport0p1: virtio0 has not announced it in 2000 ms") &&
              strstr(logged, "console: none: console=vport0p1 names no port now"),
          "which the log records, with the console left on the UART");
    for (i = 0; i < 2; i++) {
        announce(i);
    }
    model_ctl_give();
    irq();
    check(virtio_console_node("vport0p1") && replays == 0,
          "the port announced later is the line from then on");
    say("late", 4);
    serial_flush();
    check(host_read(1, "late"), "and carries the console");

    fresh("console=vport0p1", MULTI | UART);
    m.qmax[4] = m.qmax[5] = 0;
    m.port[1].exists = 0;
    before = cycles;
    check(virtio_console_start() == 1, "the boot binds a device without port 1");
    irq();
    check(!virtio_console_node("vport0p1") && !virtio_console_node("hvc0") &&
              strstr(logged, "console: none: console=vport0p1 names no port now"),
          "whose port 0 is not the port asked for");
    post(1, VIRTIO_CONSOLE_PORT_ADD, 1, 0, 0);
    model_ctl_give();
    irq();
    check(!virtio_console_node("vport0p1"), "a port without queues is left");

    fresh("console=hvc0", UART);
    m.plugged = 0;
    check(virtio_console_start() == 0, "a machine with no console binds none");
    check(strstr(logged, "console: none: console=hvc0 names no port now") != 0,
          "and the log says where the console stays");
    say("uart", 4);
    serial_flush();
    check(uart_sent("uart") && !virtio_console_node("hvc0"), "which is the UART");

    fresh("quiet", MULTI | UART);
    check(virtio_console_start() == 1 && !virtio_console_node("hvc0") && nlogged > 0 &&
              !strstr(logged, "console: enabled"),
          "with no console= the device is bound and the console untouched");
    irq();
    say("uart", 4);
    serial_flush();
    check(uart_sent("uart") && m.port[0].nout == 0 && m.port[1].nout == 0, "and stays on the UART");
}

/** Run every row on a device without and with VIRTIO_F_EVENT_IDX. */
int main(void)
{
    for (event_idx = 0; event_idx < 2; event_idx++) {
        mode = event_idx ? " (with EVENT_IDX)" : "";
        boot_port();
        boot_port_line();
        boot_hvc();
        one_port();
        words();
        output();
        stuck();
        input();
        resize();
        replug();
        removal();
        panic();
        second();
        no_memory();
        late_and_absent();
    }
    drop_bound();
    if (failures) {
        printf("virtio-console-hosttest: FAIL: %d\n", failures);
        return 1;
    }
    printf("virtio-console-hosttest: PASS\n");
    return 0;
}
