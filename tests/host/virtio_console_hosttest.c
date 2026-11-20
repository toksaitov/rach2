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
 * The model is the device as QEMU's hw/char/virtio-serial-bus.c makes it,
 * twice over for a machine of two: port 0, a console (`virtconsole`), port
 * 1, a generic port (`virtserialport`) with a name, and a third port a row
 * may add, each with a host's end that reads what the port's transmit
 * queue carries and writes into its receive queue; the control queues, on
 * which the device announces its ports once the driver is ready and tells
 * each port's kind, name and open end once the driver has it; a port whose
 * end the driver has not opened is given no input, what waited going once
 * it opens, and one whose host's end is closed has its output discarded;
 * the size and the emergency write of the configuration. A row may make
 * the host's end stop taking for a number of the driver's reads of the
 * clock or for ever, make the device take a buffer only some reads after
 * the kick, keep the device silent after DEVICE_READY, leave the control
 * transmit queue unserved, give no interrupt, take the interrupts at a
 * restore of the interrupt flag, and fail the Nth allocation. The driver's
 * clock, `arch_cycles`, advances 200 ms a read, and the model's time with
 * it. The UART is a transmitter that is always ready and whose bytes and
 * interrupt enable the test keeps. Around the driver are devfs's registry,
 * the pairs whose master the driver holds, each keeping what its slave's
 * discipline took and the output a row gives it, and the wait queues, on
 * which a wait whose condition is not met ends as a signal ends it. A
 * row's `irq` takes the interrupts and then runs khvcd's work.
 *
 * The rows, each on a device with and without VIRTIO_F_EVENT_IDX: the boot
 * on `console=vport0p1`, with the handshake's messages in order, the six
 * queues, the log's ring replayed to the port on a machine without a UART
 * and not on one with it, and the nodes; `console=hvc0` on the same device
 * and on one of a single port, with the size of its configuration and a
 * change of it, and with no memory for its port's pages, which leaves no
 * hvc number after the removal; the `console=` words, each in turn, with
 * a line for output alone beside the UART, whose port is hvc0's own
 * terminal; output of
 * 100,000 bytes to a device that takes late and to a host that stops
 * reading for a while, with no byte lost and no interrupt asked for on the
 * transmit queue; a host that stops for ever, which costs one wait of 30
 * s, drops what the four buffers cannot hold and waits 2 ms a byte after,
 * until the host reads again; input, in order, with a full ring holding
 * the port's buffers back and the host's end with them, a take that makes
 * room, and a flush that drops what is held; a console port's RESIZE; a
 * port removed and added again; the device removed through the transport
 * and bound again through the driver's probe, with the terminal hung up; a
 * panic that finds the transmit side held, whose bytes reach the host's
 * port through the port's panic buffer, and one that finds the queue's
 * lock held, whose bytes go to `emerg_wr`, which the model gives port 0 as
 * QEMU does; two devices, both bound, the line on the second; each
 * allocation failing in turn, and every one from it on; a third port added
 * and removed through the control queue, with its nodes; a generic port's
 * vport node read, written and polled with the host's end open and closed,
 * and opened again after its pages came back with no interrupt;
 * a second console port's hvc terminal; a console port added late on a
 * machine without a UART, given the log's ring then; the emergency write
 * of early output and of a panic on the second device; a device that
 * announces its port late; one without port 1; a boot with no device; a
 * removal met by a pending control message's interrupt; the shutdown; a
 * device that takes its control messages late; a transmit queue of one
 * descriptor; a device with no interrupt; the input port removed with
 * input in the ring; a port that comes late on a machine without a UART,
 * given the log's ring then; a console port found after another took
 * hvc0; and two ports named, both given the log.
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

#define ARENA_PAGES 512
#define DEV_BASE    0x40000000ull
#define NQ          8 /* ports 0, 1 and 2 with the control queues */
#define NPORTS      3
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
static int flushes, replays, hangups;
static int restore_irqs; /* kirq_restores until one takes the interrupts */
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

/** Count a replay, attach the line as the replay does, and write a word
 * through the console as the ring's records go, so that a row sees which
 * ports took it. */
void klog_replay(void (*attach)(void))
{
    const char *s = "REPLAY\r\n";

    replays++;
    attach();
    while (*s) {
        serial_out(*s++);
    }
    serial_flush();
}

/** Count a hangup of the console asked for. */
void tty_console_hangup_later(void)
{
    hangups++;
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

static void irq_only(void);

/** Nothing to put back; when a row asks, the interrupts the model raised
 * are taken at the row's Nth restore, as a pending one is when the
 * driver turns interrupts on again. */
void kirq_restore(uint32_t flags)
{
    (void)flags;
    if (restore_irqs > 0 && --restore_irqs == 0) {
        irq_only();
    }
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

static void *panic_jump[5];

/** A panic: its record through the console, flushed, as the panic writes
 * it, and back to the row that set `panic_jump`. */
void k_panic(const char *str)
{
    panicking = 1;
    while (*str) {
        serial_out(*str++);
    }
    serial_flush();
    __builtin_longjmp(panic_jump, 1);
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

/* ---- the nodes, the terminals and the queues of the kernel ---- */

#define NODES_MAX 64

static struct devfs_node *nodes[NODES_MAX];

/** Keep `node` as devfs does: -EEXIST for a name taken. */
int32_t devfs_node_register(struct devfs_node *node)
{
    int i, free = -1;

    for (i = 0; i < NODES_MAX; i++) {
        if (nodes[i] && vcon_same(nodes[i]->name, node->name)) {
            return -EEXIST;
        }
        if (!nodes[i] && free < 0) {
            free = i;
        }
    }
    check(free >= 0, "the model's devfs has room for the driver's nodes");
    if (free >= 0) {
        nodes[free] = node;
    }
    return free >= 0 ? 0 : -ENOSPC;
}

/** Forget `node`, which must be registered. */
void devfs_node_unregister(struct devfs_node *node)
{
    int i;

    for (i = 0; i < NODES_MAX; i++) {
        if (nodes[i] == node) {
            nodes[i] = 0;
            return;
        }
    }
    check(0, "a node unregistered was registered");
}

/** The node registered as `name`, or NULL. */
static struct devfs_node *node(const char *name)
{
    int i;

    for (i = 0; i < NODES_MAX; i++) {
        if (nodes[i] && vcon_same(nodes[i]->name, name)) {
            return nodes[i];
        }
    }
    return 0;
}

/** Whether `name` is registered and opens the console terminal. */
static int term_node(const char *name)
{
    struct devfs_node *n = node(name);

    return n && n->ops == 0 && n->open == 0;
}

/** Whether `name` is registered with the vport's operations. */
static int vport_node(const char *name)
{
    struct devfs_node *n = node(name);

    return n && n->ops == &vcon_vport_ops && n->major == VIRTIO_CONSOLE_VPORT_MAJOR;
}

/* A pair whose master the driver holds: what its slave's discipline took
 * and the room it has, and the output the slave has for the master. */
struct model_pty {
    struct pty p;
    int made, closed;
    uint32_t dev;
    const char *name;
    unsigned char fed[256];
    uint32_t nfed, room;
    unsigned char out[256];
    uint32_t nout, at;
    int opens;
};

static struct model_pty ptys[4];
static const struct file_ops slave_ops;

/** A pair, in the first free slot. */
struct pty *pty_kernel_make(uint32_t dev, const char *name)
{
    int i;

    for (i = 0; i < 4; i++) {
        if (!ptys[i].made) {
            k_memset(&ptys[i], 0, sizeof(ptys[i]));
            ptys[i].made = 1;
            ptys[i].dev = dev;
            ptys[i].name = name;
            ptys[i].room = sizeof(ptys[i].fed);
            return &ptys[i].p;
        }
    }
    check(0, "the model has room for the driver's terminals");
    return 0;
}

/** The record of `p`. */
static struct model_pty *mpty(struct pty *p)
{
    return (struct model_pty *)(void *)p;
}

/** Let go of a pair: its slot is free again. */
void pty_kernel_close(struct pty *p)
{
    mpty(p)->closed++;
    mpty(p)->made = 0;
}

/** The discipline takes what it has room for. */
uint32_t pty_kernel_feed(struct pty *p, const unsigned char *bytes, uint32_t n)
{
    struct model_pty *t = mpty(p);

    if (n > t->room - t->nfed) {
        n = t->room - t->nfed;
    }
    k_memcpy(t->fed + t->nfed, bytes, n);
    t->nfed += n;
    return n;
}

/** The slave's output, as far as `n` takes it. */
uint32_t pty_kernel_take(struct pty *p, unsigned char *out, uint32_t n)
{
    struct model_pty *t = mpty(p);

    if (n > t->nout - t->at) {
        n = t->nout - t->at;
    }
    k_memcpy(out, t->out + t->at, n);
    t->at += n;
    return n;
}

/** Count an open of the slave. */
int32_t pty_kernel_slave_open(struct pty *p, struct fd_entry *e, int flags)
{
    (void)e;
    (void)flags;
    mpty(p)->opens++;
    return 0;
}

/** The slave's operations, which no row calls. */
const struct file_ops *pty_slave_ops(void)
{
    return &slave_ops;
}

/* The records on the kernel's queues, which a wake of the queue calls. */
static struct wq_waiter *waiters[8];
static int wakes, waits;

/** Nothing to set up. */
void wait_queue_init_key(wait_queue_t *wq, const char *name, const struct lockdep_key *key)
{
    (void)wq;
    (void)name;
    (void)key;
}

/** Keep `w` on `wq`. */
void wq_entry_add(wait_queue_t *wq, struct wq_waiter *w)
{
    int i;

    w->wq = wq;
    for (i = 0; i < 8 && waiters[i]; i++) {
    }
    check(i < 8, "the model keeps every record on a queue");
    if (i < 8) {
        waiters[i] = w;
    }
}

/** Take `w` off its queue. */
void wq_entry_remove(struct wq_waiter *w)
{
    int i;

    for (i = 0; i < 8; i++) {
        if (waiters[i] == w) {
            waiters[i] = 0;
        }
    }
    w->wq = 0;
}

/** Call each record on `wq`. */
static void wake_records(wait_queue_t *wq, uint32_t kinds)
{
    int i;

    for (i = 0; i < 8; i++) {
        if (waiters[i] && waiters[i]->wq == wq) {
            (void)waiters[i]->func(waiters[i], kinds, 0, 0);
        }
    }
}

/** Count a wake, and call the records on the queue. */
void wq_wake(wait_queue_t *wq, uint32_t kinds)
{
    wakes++;
    wake_records(wq, kinds);
}

/** A wake from inside another. */
void wq_wake_nested(wait_queue_t *wq, uint32_t kinds, unsigned int nest, uint64_t *cpus)
{
    (void)nest;
    (void)cpus;
    wq_wake(wq, kinds);
}

/** One processor that cannot sleep: a wait whose condition is not met is
 * a signal's, and counted. */
int wq_wait_event(wait_queue_t *wq, wq_cond_fn cond, void *arg)
{
    (void)wq;
    if (cond(arg)) {
        return 0;
    }
    waits++;
    return -EINTR;
}

/** A copy from the row's memory. */
int copy_from_user(void *dst, const void __user *src, size_t n)
{
    if (!src) {
        return -EFAULT;
    }
    k_memcpy(dst, (const void *)src, n);
    return 0;
}

/** A copy to the row's memory. */
int copy_to_user(void __user *dst, const void *src, size_t n)
{
    if (!dst) {
        return -EFAULT;
    }
    k_memcpy((void *)dst, src, n);
    return 0;
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

struct model {
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
    struct model_port port[NPORTS];
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
    int shorts;   /* buffers the device found that were not one descriptor */
    int polled;   /* the transport gives no interrupt */
    int ctl_hold; /* the control transmit queue is not served on a kick */
};

/* Two devices, the second plugged in only by the rows that ask, and the
 * one the rows' `m` and the model's own steps act on. */
static struct model models[2];
static struct model *cur = &models[0];
#define m (*cur)

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

static void model_rx(void);

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
            for (i = 0; i < NPORTS && c.value && !m.silent; i++) {
                if (m.port[i].exists) {
                    announce(i);
                }
            }
            continue;
        }
        if (c.id >= (uint32_t)NPORTS || !m.port[c.id].exists) {
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
            /* QEMU's chardev offers what waited once the guest's end opens. */
            port->guest_open = c.value;
            if (c.value) {
                model_rx();
            }
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

    for (id = 0; id < NPORTS; id++) {
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

    for (id = 0; id < NPORTS; id++) {
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

/** The driver's clock: 200 ms a read, with each model's time. */
uint64_t arch_cycles(void)
{
    struct model *was = cur;
    int k;

    cycles += STEP_CYCLES;
    for (k = 0; k < 2; k++) {
        cur = &models[k];
        if (m.resume_in > 0 && --m.resume_in == 0) {
            m.port[0].throttled = m.port[1].throttled = m.port[2].throttled = 0;
            model_tx();
        }
        if (m.tx_due > 0 && --m.tx_due == 0) {
            model_tx();
        }
    }
    cur = was;
    return cycles;
}

/** The record behind `d`. */
static struct model *mod(struct virtio_dev *d)
{
    return (struct model *)d->transport;
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
        for (i = 0; i < NPORTS; i++) {
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
    /* QEMU gives the byte to port 0 when it is a console. */
    if (x->port[0].exists && x->port[0].console && x->port[0].host_open &&
        x->port[0].nout < OUT_MAX) {
        x->port[0].out[x->port[0].nout++] = *(const unsigned char *)buf;
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
    struct model *x = mod(d), *was = cur;

    cur = x;
    x->q[vq->index].notified++;
    model_mark(vq->index);
    if (vq->index == 3 && x->ctl_hold) {
    } else if (vq->index == 2 || vq->index == 3) {
        model_ctl_take();
    } else if (vq->index % 2u == 0) {
        model_rx();
    } else if (x->tx_lag > 0) {
        x->tx_due = x->tx_lag;
    } else {
        model_tx();
    }
    cur = was;
}

/** Keep the count of queues the driver asked interrupts for, and give
 * none when the row says so. */
static int m_irq_setup(struct virtio_dev *d, uint16_t nqueues)
{
    mod(d)->irq_nqueues = nqueues;
    d->polled = mod(d)->polled;
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
    mod(d)->puts++;
    mod(d)->last_put = d;
}

static const struct virtio_ops model_ops = {
    "model",      m_get_status,  m_set_status,   m_get_features, m_set_features,
    m_generation, m_config_read, m_config_write, m_queue_max,    m_queue_enable,
    m_notify,     m_irq_setup,   m_irq_start,    m_irq_release,  m_dma_alloc,
    m_dma_free,   m_shm_region,  NULL,           NULL,           NULL,
    NULL,         m_put,         NULL,           NULL,           NULL,
    NULL};

/* ---- the transport's and the bus's calls the driver makes ---- */

/** The `nth` model, when the row plugged it in; the first unplugged ends
 * the list, as a bus has no gaps. */
int virtio_pci_find(uint16_t id, int nth, struct virtio_dev **out)
{
    if (id != VIRTIO_ID_CONSOLE || nth < 0 || nth > 1 || !models[nth].plugged ||
        (nth == 1 && !models[0].plugged)) {
        return -ENODEV;
    }
    *out = &models[nth].dev;
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
    mod(d)->binds++;
    mod(d)->bound_driver = driver;
    return mod(d)->bind_result;
}

/** Count the bind's uevent. */
void virtio_driver_bound(struct virtio_dev *d)
{
    mod(d)->bounds++;
}

/** Count an unbind. */
void virtio_driver_unbind(struct virtio_dev *d)
{
    mod(d)->unbinds++;
    mod(d)->bound_driver = 0;
}

/* ---- the rows' tools ---- */

#define MULTI 1 /* the device offers F_MULTIPORT, with ports 0 and 1 */
#define SIZE  2 /* and F_SIZE */
#define EMERG 4 /* and F_EMERG_WRITE */
#define UART  8 /* the machine has a UART */

/**
 * Free the queue records of the devices the last row left bound, before
 * the models under them are cleared, and the driver's records of them: a
 * machine keeps a bound device for good, so the driver never frees them,
 * while the rings are the model's arena's; a device halted at shutdown
 * keeps them too. A queue record on any other device that no longer runs
 * is one the driver failed to free.
 */
static void drop_bound(void)
{
    struct vcon *d;
    uint32_t i;
    int k;

    for (k = 0; k < 2; k++) {
        d = (struct vcon *)models[k].dev.priv;
        for (i = 0; i < VIRTIO_MAX_QUEUES; i++) {
            if (models[k].dev.queues[i]) {
                check((models[k].status & VIRTIO_STATUS_DRIVER_OK) != 0 || (d && d->closed),
                      "a queue is left only on a running device or one halted at shutdown");
                kfree(models[k].dev.queues[i]);
                models[k].dev.queues[i] = 0;
            }
        }
    }
    while (vcon_list) {
        d = vcon_list;
        for (i = 0; i < d->nports; i++) {
            kfree(d->port[i].hold);
        }
        vcon_free(d);
    }
}

/** Make model `k` a fresh device of `kind`, named `name`. */
static void model_fresh(int k, const char *name, int kind)
{
    struct model *x = &models[k];
    int i;

    k_memset(x, 0, sizeof(*x));
    x->dev.ops = &model_ops;
    x->dev.transport = x;
    x->dev.name = name;
    x->dev.id = VIRTIO_ID_CONSOLE;
    x->dma_fail_at = x->dma_fail_from = -1;
    x->offered = VIRTIO_BIT(VIRTIO_F_VERSION_1);
    if (event_idx) {
        x->offered |= VIRTIO_BIT(VIRTIO_F_EVENT_IDX);
    }
    if (kind & MULTI) {
        x->offered |= VIRTIO_BIT(VIRTIO_CONSOLE_F_MULTIPORT);
    }
    if (kind & SIZE) {
        x->offered |= VIRTIO_BIT(VIRTIO_CONSOLE_F_SIZE);
    }
    if (kind & EMERG) {
        x->offered |= VIRTIO_BIT(VIRTIO_CONSOLE_F_EMERG_WRITE);
    }
    /* QEMU's sizes: 128 for a port's queues and 32 for the control's. */
    for (i = 0; i < NQ; i++) {
        x->qmax[i] = (uint16_t)((i == 2 || i == 3) ? 32 : 128);
    }
    if (!(kind & MULTI)) {
        for (i = 2; i < NQ; i++) {
            x->qmax[i] = 0;
        }
    }
    x->port[0].exists = 1;
    x->port[0].console = 1;
    x->port[0].host_open = 1;
    if (kind & MULTI) {
        x->port[1].exists = 1;
        x->port[1].name = "etude";
        x->port[1].host_open = 1;
        x->config[VIRTIO_CONSOLE_CFG_MAX_PORTS] = 2;
    }
}

/** A machine with a fresh model of `kind` plugged in, the boot line
 * `line`, and the driver, the serial console and the counters as a boot
 * finds them. A second model, of the same kind, waits for a row to plug it
 * in. */
static void fresh(const char *line, int kind)
{
    drop_bound();
    k_memset(arena_taken, 0, sizeof(arena_taken));
    k_memset(arena_given, 0, sizeof(arena_given));
    model_fresh(0, "virtio0", kind);
    model_fresh(1, "virtio1", kind);
    models[0].plugged = 1;
    cur = &models[0];
    vcon_tx_lock.locked = 0;
    vcon_tx_owner = 0;
    vcon_replayed = 0;
    vcon_want_n = 0;
    vcon_want_in[0] = '\0';
    vcon_attaching = 0;
    vcon_nouts = 0;
    vcon_in = vcon_only = 0;
    vcon_early = 0;
    vcon_work = 0;
    k_memset(&vcon_n, 0, sizeof(vcon_n));
    k_memset(vcon_hvc, 0, sizeof(vcon_hvc));
    k_memset(vcon_minor, 0, sizeof(vcon_minor));
    k_memset(nodes, 0, sizeof(nodes));
    k_memset(ptys, 0, sizeof(ptys));
    k_memset(waiters, 0, sizeof(waiters));
    wakes = waits = 0;
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
    flushes = replays = hangups = panicking = resize_calls = notified = 0;
    restore_irqs = 0;
    nuart = 0;
    uart_ier = -1;
    registered = 0;
    (void)kcmdline_capture(line);
}

/** Take the interrupts the models raised, as the transport's handler does,
 * until they raise no more. */
static void irq_only(void)
{
    uint32_t seen = 0, now;
    int i, k, pass;

    for (pass = 0; pass < 64; pass++) {
        now = 0;
        for (k = 0; k < 2; k++) {
            for (i = 0; i < NQ; i++) {
                now += models[k].q[i].raised;
            }
        }
        if (now == seen) {
            return;
        }
        seen = now;
        for (k = 0; k < 2; k++) {
            if (models[k].dev.priv) {
                virtio_interrupt_queues(&models[k].dev);
            }
        }
    }
}

/** The interrupts, and then khvcd, as it runs once the interrupts woke it,
 * until neither has more to do. */
static void irq(void)
{
    int pass;

    for (pass = 0; pass < 64; pass++) {
        irq_only();
        if (!vcon_work) {
            return;
        }
        vcon_service();
    }
}

/** The slave of the terminal that is `/dev/NAME`, or NULL. */
static struct model_pty *tty_of(const char *name)
{
    int i;

    for (i = 0; i < 4; i++) {
        if (ptys[i].made && vcon_same(ptys[i].name, name)) {
            return &ptys[i];
        }
    }
    return 0;
}

/** The slave of `t` writes `text`, and its master's queue is woken. */
static void tty_says(struct model_pty *t, const char *text)
{
    uint32_t n = (uint32_t)strlen(text);

    k_memcpy(t->out + t->nout, text, n);
    t->nout += n;
    wake_records(&t->p.m_read_wq, WQ_DATA);
}

/** Port `k` of the first device as the driver keeps it. */
static struct vcon_port *P(int k)
{
    struct vcon *d;

    for (d = vcon_list; d && d->vdev != &models[0].dev; d = d->next) {
    }
    return d ? &d->port[k] : 0;
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

    check(term_node("vport0p1") && !term_node("hvc0") && !term_node("vport0p0") &&
              !term_node("console"),
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
    check(term_node("vport0p1") && replays == 1, "the line stays as the boot made it");
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
    check(term_node("hvc0") && !term_node("vport0p1"), "the line's node is hvc0");
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
    check(term_node("hvc0") && replays == 1 && host_read(0, "REPLAY\r\n"),
          "port 0 is the console's line and is given the log");
    check(resize_calls == 1 && resized.ws_col == 132 && resized.ws_row == 43,
          "the console terminal takes the size of the configuration");
    v = 100;
    k_memcpy(m.config + VIRTIO_CONSOLE_CFG_COLS, &v, 2);
    v = 30;
    k_memcpy(m.config + VIRTIO_CONSOLE_CFG_ROWS, &v, 2);
    virtio_interrupt_config(&m.dev);
    irq();
    check(resize_calls == 2 && resized.ws_col == 100 && resized.ws_row == 30,
          "a change of the configuration is a new size");
    host_write(0, "in\n", 3);
    irq();
    check(serial_rx_pending() == 3 && serial_rx_take() == 'i',
          "a port without the control queues needs no open to give input");
}

/**
 * A device of one port whose pages cannot be had: the port is refused
 * before it takes hvc0, as Linux's add_port fills the receive queue before
 * it makes the hvc console, so that the device's removal, which takes down
 * only the ports present, leaves no hvc number naming the port's freed
 * record, which the console's service reads.
 */
static void one_port_no_memory(void)
{
    int calls;

    fresh("console=hvc0", UART);
    check(virtio_console_start() == 1 && vcon_hvc[0] == P(0), "the boot binds the console");
    calls = m.dma_calls;
    m.dev.removed(&m.dev);
    fresh("console=hvc0", UART);
    /* Every page of the port fails: four to send, the panic's, eight to
     * receive. */
    m.dma_fail_from = calls - (VCON_TX_BUFS + 1 + VCON_RX_BUFS);
    check(virtio_console_start() == 1 && P(0) && !P(0)->present,
          "a port without its pages is refused");
    check(vcon_hvc[0] == 0 && P(0)->hvc < 0, "and takes no hvc number");
    m.dev.removed(&m.dev);
    check(vcon_hvc[0] == 0 && m.dma_out == 0,
          "the device's removal leaves no hvc number and no page");
    vcon_service();
    say("uart", 4);
    serial_flush();
    check(uart_sent("uart"), "and the console stays on the UART");
}

/** What `console=` words leave: the ports asked for, in order and each
 * once, the one the input is to come from, and whether the UART stays a
 * console. */
static void words(void)
{
    static const struct {
        const char *line, *want, *input;
        int uart;
    } rows[] = {
        {"quiet", "", "", 0},
        {"console=hvc0", "hvc0", "hvc0", 0},
        {"console=ttyS0 console=hvc0", "hvc0", "hvc0", 1},
        {"console=hvc0 console=ttyS0,115200n8", "hvc0", "", 1},
        {"console=tty0 console=hvc0,9600", "hvc0", "hvc0", 0},
        {"console=vport0p1", "vport0p1", "vport0p1", 0},
        {"console=vport0p0", "vport0p0", "vport0p0", 0},
        {"console=vport0p1 console=hvc0", "vport0p1 hvc0", "hvc0", 0},
        {"console=hvc0 console=vport0p1 console=hvc0", "hvc0 vport0p1", "hvc0", 0},
        {"console=vport0p1 console=ttyS0", "vport0p1", "", 1},
        {"console=vport0p2", "vport0p2", "vport0p2", 0},
        {"console=vport1p1", "vport1p1", "vport1p1", 0},
        {"console=hvc1", "hvc1", "hvc1", 0},
        {"console=hvc15 console=ttyS0", "hvc15", "", 1},
        {"console=hvc16", "", "", 0},
        {"console=hvc", "", "", 0},
        {"console=vport0", "", "", 0},
        {"console=vport0p511", "", "", 0},
        {"console=ttyS0", "", "", 1},
    };
    char list[64];
    unsigned int i, n;
    int k;

    for (i = 0; i < sizeof(rows) / sizeof(rows[0]); i++) {
        fresh(rows[i].line, MULTI);
        vcon_read_boot_line();
        list[0] = '\0';
        for (k = 0, n = 0; k < vcon_want_n; k++) {
            n += (unsigned int)snprintf(list + n, sizeof(list) - n, "%s%s", k ? " " : "",
                                        vcon_want[k]);
        }
        if (!vcon_same(list, rows[i].want) || !vcon_same(vcon_want_in, rows[i].input) ||
            vcon_want_uart != rows[i].uart) {
            printf("virtio-console-hosttest: words: \"%s\" gave \"%s\" input \"%s\" uart %d\n",
                   rows[i].line, list, vcon_want_in, vcon_want_uart);
            check(0, "a boot line's console= words choose the lines");
        }
    }

    /* The UART named last: the port carries output alone. */
    fresh("console=hvc0 console=ttyS0", MULTI | UART);
    _rx_irq_wanted = 1;
    check(virtio_console_start() == 1, "the boot binds the console");
    check(strstr(logged, "console: enabled: hvc0, port 0 of virtio0, for output alone\n") != 0,
          "the log says the line carries output alone");
    check(!term_node("hvc0") && node("hvc0") && node("hvc0")->ops == &slave_ops && tty_of("hvc0"),
          "a line for output alone is not the console terminal; hvc0 is its own terminal");
    check(uart_ier != 0x00, "the UART keeps the console's input");
    say("both", 4);
    serial_flush();
    check(host_read(0, "both") && uart_sent("both"), "the log goes to the port and the UART");
    host_write(0, "mine", 4);
    irq();
    check(serial_rx_pending() == 0 && counters().rx_dropped == 0 && counters().rx_bytes == 4 &&
              tty_of("hvc0")->nfed == 4 && memcmp(tty_of("hvc0")->fed, "mine", 4) == 0,
          "input on a line for output alone goes to hvc0's terminal, as Linux's hvc tty");
    tty_says(tty_of("hvc0"), "login: ");
    irq();
    check(host_read(0, "bothlogin: ") && m.port[0].in_at == m.port[0].nin,
          "and the terminal's output goes to the port beside the log");

    /* The UART named first: both carry the log, the port has the input. */
    fresh("console=ttyS0 console=hvc0", MULTI | UART);
    check(virtio_console_start() == 1 && term_node("hvc0") && replays == 0,
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
    check(m.port[1].in_at == SERIAL_RX_SIZE + 8u * VCON_BUF && P(1)->rx_n == VCON_RX_BUFS,
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
    check(P(1)->rx_n == 0 && counters().rx_bytes == 40006 && counters().rx_dropped == 0,
          "and every buffer is back with the device");

    /* A flush of the input reaches the bytes the buffers hold. */
    fill(0, 40000);
    host_write(1, big, 40000);
    irq();
    check(P(1)->rx_n == VCON_RX_BUFS, "the buffers are held again");
    serial_rx_flush();
    check(serial_rx_pending() == 0 && P(1)->rx_n == 0, "a flush drops the ring and them");
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
    check(serial_rx_pending() == 0 && tty_of("hvc0") && tty_of("hvc0")->nfed == 1,
          "the console port that is not the line gives its input to its own terminal");
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
    check(resize_calls == 1 && term_node("hvc0"),
          "a short size, an unknown event and a port past the driver's change nothing");
    check(strstr(logged, "port 2 left: the device has 2\n") != 0,
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
    check(!term_node("vport0p1") &&
              strstr(logged, "console: disabled: vport0p1: its port has gone\n") != 0,
          "the line's port gone, the console leaves it");
    check(uart_ier == SERIAL_IER_RDA, "the UART's receive interrupt is on again");
    say("uart", 4);
    serial_flush();
    check(uart_sent("uart") && m.port[1].nout == 0, "the console's bytes go to the UART");

    announce(1);
    model_ctl_give();
    irq();
    check(term_node("vport0p1") && m.port[1].guest_open && replays == 0,
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
    check(!term_node("vport0p1") &&
              strstr(logged, "console: disabled: vport0p1: its device is removed\n") != 0,
          "the console leaves the port");
    check(hangups == 1, "and its terminal is hung up");
    say("uart", 4);
    serial_flush();
    check(uart_sent("uart"), "and goes on at the UART");

    check(registered(&m.dev) == 0, "the probe binds the device again");
    check(!term_node("vport0p1"), "a bind after the boot waits for no port");
    irq();
    check(term_node("vport0p1") && replays == 0,
          "the port, announced by interrupt, is the line again, with no replay");
    say("port", 4);
    serial_flush();
    check(host_read(1, "port"), "and carries the console");
    check(m.binds == 2 && m.bounds == 2, "the device is shown bound again");
}

/**
 * A panic that finds the transmit side held by a processor it stopped,
 * with a byte of the holder's buffer filled: the bytes reach the port the
 * host reads, a virtserialport, through the port's panic buffer, whether
 * or not the device has `emerg_wr`, which QEMU would give a virtconsole
 * alone; with the side free they go as ever. Then the panic of
 * `rach2.vconpanic`, whose record reaches the port the same way.
 */
static void panic(void)
{
    fresh("console=vport0p1", MULTI | EMERG);
    check(virtio_console_start() == 1, "the boot binds the console");
    m.port[1].nout = 0;
    panicking = 1;
    say("free", 4);
    serial_flush();
    check(host_read(1, "free") && m.nemerg == 0, "a panic with the transmit side free uses it");
    say("h", 1);
    vcon_tx_lock.locked = 1;
    say("PANIC", 5);
    serial_flush();
    check(host_read(1, "freePANIC") && m.nemerg == 0 && counters().tx_panic == 5,
          "with the side held, the bytes reach the host's port by the panic's buffer");
    check(vcon_tx_lock.locked == 1 && P(1)->tx_fill == 1, "and the held side is left alone");
    vcon_tx_lock.locked = 0;
    panicking = 0;

    fresh("console=vport0p1", MULTI);
    check(virtio_console_start() == 1, "the boot binds the console");
    m.port[1].nout = 0;
    panicking = 1;
    vcon_tx_lock.locked = 1;
    say("PANIC", 5);
    serial_flush();
    check(host_read(1, "PANIC") && m.nemerg == 0 && counters().emerg == 0,
          "a device without emerg_wr gives the host the bytes too");
    vcon_tx_lock.locked = 0;
    panicking = 0;

    fresh("console=vport0p1", MULTI);
    check(virtio_console_start() == 1, "the boot binds the console");
    m.port[1].nout = 0;
    if (__builtin_setjmp(panic_jump) == 0) {
        virtio_console_panic_test();
        check(0, "the panic test panics");
    }
    check(host_read(1, "virtio_console: panic test: the transmit side is held") &&
              vcon_tx_lock.locked == 1,
          "rach2.vconpanic's panic reaches the host's port with the side held");
    vcon_tx_lock.locked = 0;
    panicking = 0;
}

/**
 * A panic whose processor interrupted a reap of the port's transmit queue,
 * as a shared interrupt reaps it, with the queue's lock held: no queue
 * operation may wait for that lock, and the bytes go to `emerg_wr`, which
 * QEMU gives port 0, the virtconsole; the queue is left alone.
 */
static void panic_queue_held(void)
{
    fresh("console=hvc0", MULTI | EMERG);
    check(virtio_console_start() == 1, "the boot binds the console");
    irq();
    m.port[0].nout = 0;
    panicking = 1;
    P(0)->txq->lock.locked = 1;
    say("PANIC", 5);
    serial_flush();
    check(m.nemerg == 5 && host_read(0, "PANIC") && counters().emerg == 5,
          "with the queue's lock held, each byte goes to emerg_wr and reaches port 0");
    check(P(0)->txq->lock.locked == 1 && P(0)->tx_cur < 0, "and the queue is not touched");
    P(0)->txq->lock.locked = 0;
    vcon_tx_lock.locked = 1;
    P(0)->txq->lock.locked = 1;
    say("X", 1);
    serial_flush();
    check(m.nemerg == 6 && P(0)->ptx_fill == 0,
          "with both held the byte still goes to emerg_wr alone");
    P(0)->txq->lock.locked = 0;
    vcon_tx_lock.locked = 0;
    panicking = 0;
}

/** Whether the driver told the device `event` with `value` for port `id`
 * at any time. */
static int sent(uint32_t id, uint16_t event, uint16_t value)
{
    int i;

    for (i = 0; i < m.ngot; i++) {
        if (got(i, id, event, value)) {
            return 1;
        }
    }
    return 0;
}

/**
 * Two devices, each bound with its ports and numbered in the order found,
 * as Linux binds every virtio console: `console=vport1p1` names the
 * second's port 1, which carries the console, while the first's ports and
 * console port have their nodes; the first removed, the second goes on;
 * and a bind the bus refuses.
 */
static void second(void)
{
    fresh("console=vport1p1", MULTI | UART);
    models[1].plugged = 1;
    models[1].port[1].name = "other";
    check(virtio_console_start() == 2, "the boot binds both consoles");
    irq();
    check(models[0].bounds == 1 && models[1].bounds == 1 &&
              (models[0].status & VIRTIO_STATUS_DRIVER_OK) &&
              (models[1].status & VIRTIO_STATUS_DRIVER_OK),
          "each is driven and shown bound");
    check(term_node("vport1p1") && vport_node("vport0p1") && vport_node("vport0p0") &&
              vport_node("vport1p0") && vport_node("virtio-ports/etude") &&
              term_node("virtio-ports/other"),
          "the line is the second's port 1, by its number and its name, and every other port "
          "is a byte device");
    check(node("hvc0") && node("hvc0")->ops == &slave_ops && node("hvc1") && tty_of("hvc0") &&
              tty_of("hvc1") && P(0)->hvc == 0,
          "each console port is an hvc terminal, numbered in the order the devices were found");
    check(strstr(logged, "console: enabled: vport1p1, port 1 of virtio1\n") != 0,
          "the log names the line");
    say("two", 3);
    serial_flush();
    cur = &models[1];
    check(host_read(1, "two"), "the console's bytes go to the second device's port");
    cur = &models[0];
    check(m.port[1].nout == 0, "and not to the first's");

    m.dev.removed(&m.dev);
    check(m.status == 0 && !node("vport0p1") && !node("vport0p0") && !node("hvc0") &&
              !node("virtio-ports/etude"),
          "the first removed, its nodes and its terminal go");
    check(term_node("vport1p1") && node("hvc1"), "and the second's stay");
    say("on", 2);
    serial_flush();
    cur = &models[1];
    check(host_read(1, "twoon"), "the second goes on carrying the console");
    cur = &models[0];
    check(registered(&m.dev) == 0, "the probe binds the first again");
    irq();
    check(vport_node("vport0p1") && node("hvc0") && P(0)->hvc == 0 && P(0)->dev->index == 0,
          "with its numbers back");

    fresh("console=vport0p1", MULTI | UART);
    m.bind_result = -ENOMEM;
    check(virtio_console_start() == -ENOMEM, "a bind the bus refuses fails the boot's bind");
    check(m.status == 0 && m.dma_out == 0 && m.puts == 1 && !node("vport0p1"),
          "with the device reset, nothing kept and the device given back");
    check(strstr(logged, "console: none: console=vport0p1 names no port now") != 0,
          "and the log says the console stays on the UART");
}

/**
 * The boot short of memory. With one allocation failing, each in turn, or
 * every allocation from one on: the bring-up fails with nothing kept; or a
 * port whose pages cannot be had is refused with PORT_READY 0, as Linux's
 * add_port refuses it, and the console stays on the UART when it was the
 * line; or a port with fewer receive pages than it asks for works with
 * those. Nothing is kept by half, and a removal gives every page back.
 */
static void no_memory(void)
{
    int k, all, calls, setup;
    int failed[2] = {0, 0}, refused[2] = {0, 0}, lined[2] = {0, 0};

    fresh("console=vport0p1", MULTI | UART);
    check(virtio_console_start() == 1 && term_node("vport0p1"), "the boot has memory");
    calls = m.dma_calls;
    /* Each port's pages: four to send, the panic's and eight to receive. */
    setup = calls - 2 * (VCON_TX_BUFS + 1 + VCON_RX_BUFS);
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
                          vcon_list == 0,
                      "a bring-up that fails keeps nothing");
                continue;
            }
            check(rc == 1 && (m.status & VIRTIO_STATUS_DRIVER_OK), "the device stays driven");
            if (term_node("vport0p1")) {
                lined[all]++;
                say("port", 4);
                serial_flush();
                check(host_read(1, "port") && nuart == 0, "the line carries the console");
            } else {
                refused[all]++;
                check(
                    strstr(logged, "port: virtio0: port 1 refused: no memory for its buffers\n") &&
                        sent(1, VIRTIO_CONSOLE_PORT_READY, 0) && !m.port[1].ready,
                    "a port without its pages is refused, and the device told");
                say("uart", 4);
                serial_flush();
                check(uart_sent("uart"), "and the console stays on the UART");
            }
            m.dev.removed(&m.dev);
            check(m.dma_out == 0, "the removal gives every page back");
        }
    }
    check(failed[0] == setup && failed[1] == setup,
          "every allocation of the bring-up fails it, and none of a port's");
    check(refused[0] == VCON_TX_BUFS + 2 && lined[0] == calls - setup - refused[0],
          "one failure refuses the line's port only at its send pages or its first receive page");
    check(refused[1] == 2 * (VCON_TX_BUFS + 1 + VCON_RX_BUFS) - (VCON_RX_BUFS - 1) &&
              lined[1] == VCON_RX_BUFS - 1,
          "every allocation from one on failing leaves the line only with some receive pages");
}

/**
 * A third port, past the two of QEMU's harness: `max_nr_ports` 3 gives it
 * queues 6 and 7, and the device adds it after the boot, names it and
 * removes it, as `device_add virtserialport` and `device_del` do. It is
 * answered with PORT_READY, gets /dev/vport0p2 and
 * /dev/virtio-ports/NAME, which go when it goes, and comes back with the
 * pages it had.
 */
static void third_port(void)
{
    int pages;

    fresh("console=hvc0", MULTI | UART);
    m.config[VIRTIO_CONSOLE_CFG_MAX_PORTS] = 3;
    check(virtio_console_start() == 1, "the boot binds the console");
    irq();
    check(m.irq_nqueues == 8 && m.q[6].enabled && m.q[7].enabled,
          "a device of three ports has its third port's queues set up");
    check(!node("vport0p2") && P(2) && !P(2)->present, "a port not yet added has no node");
    pages = m.dma_out;
    m.port[2].exists = 1;
    m.port[2].name = "org.rach2.third";
    announce(2);
    model_ctl_give();
    irq();
    check(m.port[2].ready && P(2)->present && !P(2)->console,
          "the port added is answered with PORT_READY");
    check(vport_node("vport0p2") && node("vport0p2")->minor == 2 &&
              vport_node("virtio-ports/org.rach2.third") &&
              node("virtio-ports/org.rach2.third")->minor == 2 &&
              node("virtio-ports/org.rach2.third")->alias,
          "it is /dev/vport0p2, 245:2, also named by its name");
    check(strstr(logged, "virtio_console: port: virtio0: port 2 is named org.rach2.third\n") != 0,
          "the log keeps its name");
    check(m.dma_out == pages + VCON_TX_BUFS + 1 + VCON_RX_BUFS, "with pages of its own");
    host_write(2, "nobody", 6);
    irq();
    check(m.port[2].in_at == 0, "the device gives a port nobody opened no input");

    post(2, VIRTIO_CONSOLE_PORT_REMOVE, 1, 0, 0);
    model_ctl_give();
    irq();
    check(!P(2)->present && !node("vport0p2") && !node("virtio-ports/org.rach2.third"),
          "the port removed loses its nodes");
    check(term_node("hvc0") && node("vport0p1"), "and the others keep theirs");
    announce(2);
    model_ctl_give();
    irq();
    check(vport_node("vport0p2") && m.dma_out == pages + VCON_TX_BUFS + 1 + VCON_RX_BUFS,
          "added again, it is the same node with the pages it had");
}

/**
 * A generic port as a byte device, as Linux's port_fops: the open tells
 * the device PORT_OPEN 1 and a second open is refused; a write reaches the
 * host's end; a read takes what the host wrote, gives the end of a file
 * once the host's end has closed, and waits otherwise; poll says which;
 * a write waits while the host's end is closed; the last close tells the
 * device PORT_OPEN 0; and a port removed while open fails the descriptor's
 * calls with ENODEV. A console port's vport node refuses an open.
 */
static void vport(void)
{
    struct devfs_node *n;
    struct open_file ofd;
    struct wait_queue *wq = 0;
    fd_entry_t e, e2;
    char buf[16];

    fresh("console=hvc0", MULTI | UART);
    m.port[1].host_open = 0;
    check(virtio_console_start() == 1, "the boot binds the console");
    irq();
    n = node("vport0p1");
    check(vport_node("vport0p1") && n->minor == 1 && n->mode == 0600 &&
              vport_node("virtio-ports/etude"),
          "a generic port is a byte device, by its number and by its name");
    k_memset(&ofd, 0, sizeof(ofd));
    k_memset(&e, 0, sizeof(e));
    k_memset(&e2, 0, sizeof(e2));
    e.ofd = e2.ofd = &ofd;
    check(node("vport0p0")->open(node("vport0p0"), &e2, 0) == -ENXIO,
          "a console port's vport node refuses an open");
    check(n->open(n, &e, 0) == 0 && m.port[1].guest_open == 1 &&
              sent(1, VIRTIO_CONSOLE_PORT_OPEN, 1),
          "an open tells the device the guest's end opened");
    check(n->open(n, &e2, 0) == -EBUSY, "a second open is refused while the first is open");

    ofd.status_flags = OFD_NONBLOCK;
    check(vcon_vport_ops.read(&e, buf, sizeof(buf)) == 0,
          "with the host's end closed and nothing waiting a read is the end of a file");
    check(vcon_vport_ops.write(&e, "ping", 4) == -EAGAIN,
          "a write without a wait fails while the host's end is closed");
    check(vcon_vport_ops.poll(&e, POLLIN | POLLOUT, &wq) == (POLLIN | POLLHUP) && wq == &P(1)->wq,
          "poll says readable and hung up, and gives the port's queue");
    ofd.status_flags = 0;
    check(vcon_vport_ops.write(&e, "ping", 4) == -EINTR && waits == 1,
          "a write waits for the host's end");

    m.port[1].host_open = 1;
    post(1, VIRTIO_CONSOLE_PORT_OPEN, 1, 0, 0);
    model_ctl_give();
    irq();
    check(vcon_vport_ops.poll(&e, POLLIN | POLLOUT, &wq) == POLLOUT,
          "the host's end open, writable");
    check(vcon_vport_ops.write(&e, "ping", 4) == 4 && host_read(1, "ping"),
          "a write reaches the host's end");
    check(vcon_vport_ops.read(&e, buf, sizeof(buf)) == -EINTR && waits == 2,
          "a read with nothing waiting waits");
    ofd.status_flags = OFD_NONBLOCK;
    check(vcon_vport_ops.read(&e, buf, sizeof(buf)) == -EAGAIN, "or fails without a wait");
    host_write(1, "pong!", 5);
    irq();
    check(vcon_vport_ops.poll(&e, POLLIN | POLLOUT, &wq) == (POLLIN | POLLOUT),
          "what the host wrote makes it readable");
    check(vcon_vport_ops.read(&e, buf, 3) == 3 && memcmp(buf, "pon", 3) == 0 &&
              vcon_vport_ops.read(&e, buf, sizeof(buf)) == 2 && memcmp(buf, "g!", 2) == 0,
          "a read takes the host's bytes in order, as many as asked");
    check(counters().rx_bytes == 5 && serial_rx_pending() == 0, "and they are not the console's");

    vcon_vport_ops.close(&e);
    check(m.port[1].guest_open == 0 && sent(1, VIRTIO_CONSOLE_PORT_OPEN, 0),
          "the last close tells the device the guest's end closed");
    host_write(1, "lost", 4);
    irq();
    check(m.port[1].in_at == 0, "and the device keeps what the host writes then");

    ofd.status_flags = 0;
    check(n->open(n, &e, 0) == 0, "the port opens again");
    irq();
    check(vcon_vport_ops.read(&e, buf, sizeof(buf)) == 4 && memcmp(buf, "lost", 4) == 0,
          "and takes what waited");
    post(1, VIRTIO_CONSOLE_PORT_REMOVE, 1, 0, 0);
    model_ctl_give();
    irq();
    check(!node("vport0p1") && vcon_vport_ops.read(&e, buf, sizeof(buf)) == -ENODEV &&
              vcon_vport_ops.write(&e, "x", 1) == -ENODEV &&
              vcon_vport_ops.poll(&e, POLLIN, &wq) == POLLHUP,
          "a port removed while open fails its descriptor's calls");
    announce(1);
    model_ctl_give();
    irq();
    check(n->open(n, &e2, 0) == 0,
          "the port added again opens, with the old descriptor still open");
    vcon_vport_ops.close(&e);
    check(m.port[1].guest_open == 1, "whose close leaves the new open alone");
    vcon_vport_ops.close(&e2);
    check(m.port[1].guest_open == 0, "and the new close tells the device");
}

/**
 * A vport closed while the device holds every page, which the host's end
 * returns while the closed port's transmit queue asks for no interrupt:
 * the open again takes the pages back, as Linux's port_fops_open reclaims
 * them, so that poll finds room and a write goes out, though no transmit
 * interrupt comes. The row raises no interrupt after the host's taking,
 * as a dedicated vector would give none.
 */
static void vport_reopen(void)
{
    struct devfs_node *n;
    struct open_file ofd;
    struct wait_queue *wq = 0;
    fd_entry_t e;

    fresh("console=hvc0", MULTI | UART);
    check(virtio_console_start() == 1, "the boot binds the console");
    irq();
    n = node("vport0p1");
    k_memset(&ofd, 0, sizeof(ofd));
    k_memset(&e, 0, sizeof(e));
    e.ofd = &ofd;
    ofd.status_flags = OFD_NONBLOCK;
    check(n && n->open(n, &e, 0) == 0, "the generic port opens");
    irq();
    m.port[1].throttled = 1;
    fill(0, 4096);
    check(vcon_vport_ops.write(&e, (const char *)big, 4096) == 2048 &&
              vcon_tx_busy(P(1)) == VCON_TX_BUFS && vcon_vport_ops.poll(&e, POLLOUT, &wq) == 0,
          "a host's end that takes nothing leaves the device every page, and no room");
    vcon_vport_ops.close(&e);
    irq();
    check(P(1)->tx_irq == 0, "the close asks the transmit queue for no interrupt");
    m.port[1].throttled = 0;
    model_tx();
    check(m.port[1].nout == 2048 && vcon_tx_busy(P(1)) == VCON_TX_BUFS,
          "the host's end takes the pages, and no interrupt tells the driver");
    check(n->open(n, &e, 0) == 0 && vcon_tx_busy(P(1)) == 0,
          "the open again takes back the pages the device returned");
    check(vcon_vport_ops.poll(&e, POLLOUT, &wq) == POLLOUT,
          "poll finds room on the port opened again");
    m.port[1].nout = 0;
    check(vcon_vport_ops.write(&e, "again", 5) == 5 && host_read(1, "again"),
          "and a write goes out");
    vcon_vport_ops.close(&e);
}

/**
 * A console port that is not the console's line is an hvc terminal of its
 * own, as Linux's hvc tty: /dev/hvc1, 229:1, opens the pair's slave; what
 * the host writes goes to the slave's discipline, held while it is full;
 * the slave's output goes to the host; RESIZE sizes the slave; and a port
 * removed hangs its terminal up and gives its number back.
 */
static void hvc_tty(void)
{
    static const unsigned char size[4] = {24, 0, 80, 0};
    struct model_pty *t;
    struct devfs_node *n;
    fd_entry_t e;

    fresh("console=hvc0", MULTI | UART);
    m.config[VIRTIO_CONSOLE_CFG_MAX_PORTS] = 3;
    m.port[2].exists = 1;
    m.port[2].console = 1;
    m.port[2].host_open = 1;
    check(virtio_console_start() == 1, "the boot binds the console");
    irq();
    t = tty_of("hvc1");
    n = node("hvc1");
    check(term_node("hvc0") && t && t->dev == ((229u << 8) | 1u) && n && n->major == 229 &&
              n->minor == 1 && n->ops == &slave_ops,
          "the second console port is hvc1, a terminal of its own");
    check(m.port[2].guest_open && sent(2, VIRTIO_CONSOLE_PORT_OPEN, 1),
          "whose port is opened, as Linux opens a console port");
    k_memset(&e, 0, sizeof(e));
    check(n->open(n, &e, 0) == 0 && t->opens == 1, "its node opens the pair's slave");

    host_write(2, "ls\r", 3);
    irq();
    check(t->nfed == 3 && memcmp(t->fed, "ls\r", 3) == 0 && serial_rx_pending() == 0,
          "what the host writes goes to the slave's discipline");
    tty_says(t, "$ ");
    irq();
    check(host_read(2, "$ ") && m.port[0].nout == 0, "and the slave's output to the host");

    t->room = t->nfed + 2u;
    host_write(2, "abcd", 4);
    irq();
    check(t->nfed == 5 && counters().rx_held > 0, "a full discipline holds the rest back");
    t->room = sizeof(t->fed);
    wake_records(&t->p.m_read_wq, WQ_ROOM);
    irq();
    check(t->nfed == 7 && memcmp(t->fed + 3, "abcd", 4) == 0, "and takes it once it has room");

    post(2, VIRTIO_CONSOLE_RESIZE, 0, size, 4);
    model_ctl_give();
    irq();
    check(resize_calls >= 1 && resized.ws_col == 80 && resized.ws_row == 24 &&
              t->p.slave.winsize.ws_col == 80,
          "a size for the port sizes its terminal");

    post(2, VIRTIO_CONSOLE_PORT_REMOVE, 1, 0, 0);
    model_ctl_give();
    irq();
    check(t->closed == 1 && !node("hvc1") && vcon_hvc[1] == 0 && term_node("hvc0"),
          "the port removed lets its terminal go, with its node and number");
    announce(2);
    model_ctl_give();
    irq();
    check(tty_of("hvc1") && node("hvc1"), "and added again it is hvc1 again");
}

/**
 * A console port that comes after the boot on a machine without a UART:
 * the device has only a generic port at the boot, and its third port, a
 * virtconsole, is added later; it becomes hvc0, the line the boot line
 * names, and is given the log's ring then, as Linux gives a console
 * registered late the buffer.
 */
static void late_console(void)
{
    fresh("console=hvc0", MULTI);
    m.config[VIRTIO_CONSOLE_CFG_MAX_PORTS] = 3;
    m.port[0].exists = 0;
    check(virtio_console_start() == 1, "the boot binds the console");
    irq();
    check(replays == 0 && !node("hvc0") &&
              strstr(logged, "console: none: console=hvc0 names no port now"),
          "with no console port the log has no line");
    m.port[2].exists = 1;
    m.port[2].console = 1;
    m.port[2].host_open = 1;
    announce(2);
    model_ctl_give();
    irq();
    check(term_node("hvc0") && P(2)->hvc == 0 && vcon_in == P(2) && replays == 1 &&
              host_read(2, "REPLAY\r\n"),
          "the console port added late is hvc0, the line, and is given the log's ring");
    check(strstr(logged, "console: enabled: hvc0, port 2 of virtio0\n") != 0, "the log names it");
    host_write(2, "k", 1);
    irq();
    check(serial_rx_pending() == 1 && serial_rx_take() == 'k', "its input is the console's");
}

/**
 * The emergency write of the configuration, for any device that offers
 * it: until the port `console=hvc0` names comes, the console's output goes
 * to the first such device's `emerg_wr` beside the UART, which QEMU gives
 * its first console port; it stops once the port carries the log; and a
 * panic that finds the queue of a port of the second device held writes
 * that device's `emerg_wr`.
 */
static void emergency(void)
{
    fresh("console=hvc0", MULTI | EMERG | UART);
    m.silent = 1;
    check(virtio_console_start() == 1, "the boot binds a device that announces nothing yet");
    m.nemerg = 0;
    m.port[0].nout = 0;
    say("early", 5);
    serial_flush();
    check(m.nemerg == 5 && memcmp(m.emerg, "early", 5) == 0 && host_read(0, "early") &&
              uart_sent("early") && counters().emerg == 5,
          "the console's output goes to emerg_wr and the UART until the port comes");
    m.silent = 0;
    announce(0);
    announce(1);
    model_ctl_give();
    irq();
    check(term_node("hvc0"), "the port comes");
    say("port", 4);
    serial_flush();
    check(m.nemerg == 5 && host_read(0, "earlyport"), "and the log goes by its queue alone");

    fresh("console=vport1p1", MULTI | EMERG);
    models[1].plugged = 1;
    models[1].port[1].name = "other";
    check(virtio_console_start() == 2, "the boot binds both consoles");
    irq();
    cur = &models[1];
    m.port[0].nout = 0;
    panicking = 1;
    P(0)->dev->next->port[1].txq->lock.locked = 1;
    say("PANIC", 5);
    serial_flush();
    check(m.nemerg == 5 && memcmp(m.emerg, "PANIC", 5) == 0 && host_read(0, "PANIC"),
          "a panic that finds the second device's port held writes its emerg_wr");
    P(0)->dev->next->port[1].txq->lock.locked = 0;
    panicking = 0;
    cur = &models[0];
    check(m.nemerg == 0, "and not the first's");
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
    check(strstr(logged, "console: waiting: vport0p1: no device has announced it in 2000 ms") &&
              strstr(logged, "console: none: console=vport0p1 names no port now"),
          "which the log records, with the console left on the UART");
    for (i = 0; i < 2; i++) {
        announce(i);
    }
    model_ctl_give();
    irq();
    check(term_node("vport0p1") && replays == 0,
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
    check(!term_node("vport0p1") && !term_node("hvc0") &&
              strstr(logged, "console: none: console=vport0p1 names no port now"),
          "whose port 0 is not the port asked for");
    post(1, VIRTIO_CONSOLE_PORT_ADD, 1, 0, 0);
    model_ctl_give();
    irq();
    check(!term_node("vport0p1"), "a port without queues is left");

    fresh("console=hvc0", UART);
    m.plugged = 0;
    check(virtio_console_start() == 0, "a machine with no console binds none");
    check(strstr(logged, "console: none: console=hvc0 names no port now") != 0,
          "and the log says where the console stays");
    say("uart", 4);
    serial_flush();
    check(uart_sent("uart") && !term_node("hvc0"), "which is the UART");

    fresh("quiet", MULTI | UART);
    check(virtio_console_start() == 1 && !term_node("hvc0") && nlogged > 0 &&
              !strstr(logged, "console: enabled"),
          "with no console= the device is bound and the console untouched");
    irq();
    say("uart", 4);
    serial_flush();
    check(uart_sent("uart") && m.port[0].nout == 0 && m.port[1].nout == 0, "and stays on the UART");
}

/** A removal that a pending control message's interrupt meets as the
 * driver turns interrupts on between leaving the port and the reset: the
 * port is not taken again, and nothing of the driver's is freed while the
 * line or the device still has it. */
static void unbind_pending(void)
{
    fresh("console=vport0p1", MULTI | UART);
    _rx_irq_wanted = 1;
    check(virtio_console_start() == 1, "the boot binds the console");
    irq();
    post(1, VIRTIO_CONSOLE_PORT_OPEN, 1, 0, 0);
    model_ctl_give();
    host_write(1, "late", 4);
    restore_irqs = 1;
    m.dev.removed(&m.dev);
    restore_irqs = 0;
    check(_line == 0 && !term_node("vport0p1"),
          "the pending message does not give the port the console again");
    check(m.status == 0 && m.dma_out == 0 && m.puts == 1,
          "the device is reset and every page given back");
    say("uart", 4);
    serial_flush();
    check(uart_sent("uart") && serial_rx_pending() == 0,
          "the console goes on at the UART, with none of the port's input");
}

/** The shutdown: what the port was given leaves, the device is reset and
 * no message after it gives a port the console again. */
static void shutdown(void)
{
    fresh("console=vport0p1", MULTI);
    check(virtio_console_start() == 1, "the boot binds the console");
    irq();
    m.port[1].nout = 0;
    say("last words", 10);
    virtio_console_shutdown();
    check(host_read(1, "last words"), "the shutdown sends what the port was given");
    check(m.status == 0 && strstr(logged, "virtio_console: shutdown: virtio0: reset\n") != 0,
          "and resets the device, which the log says");
    check(_line == 0 && !term_node("vport0p1") &&
              strstr(logged, "console: disabled: vport0p1: the system stops\n") != 0 &&
              hangups == 0,
          "the console leaves the port with no hangup");
    announce(1);
    model_ctl_give();
    irq();
    say("after", 5);
    serial_flush();
    check(_line == 0 && m.port[1].nout == 10 && counters().control_out == 5,
          "a message after the reset changes nothing and nothing more is sent");
    virtio_console_shutdown();
    check(m.dma_out > 0 && counters().control_out == 5,
          "a second shutdown does nothing, and the pages stay with the halted device");
}

/** A device that takes no control message for a while, as one whose
 * control transmit queue has an MSI-X vector of its own and stays quiet:
 * DEVICE_READY's wait ends, and the messages after it still go. */
static void control_late(void)
{
    fresh("console=vport0p1", MULTI | UART);
    m.ctl_hold = 1;
    check(virtio_console_start() == 1, "the boot binds the console");
    check(strstr(logged, "control: virtio0: event 0 not taken in 1000 ms") != 0,
          "DEVICE_READY's wait ends");
    m.ctl_hold = 0;
    model_ctl_take();
    irq();
    check(m.ready && m.port[0].ready && m.port[1].ready && m.port[1].guest_open,
          "the device takes it later and every port is answered and opened");
    check(term_node("vport0p1"), "and the port carries the console");
    host_write(1, "in", 2);
    irq();
    check(serial_rx_pending() == 2, "with its input");
}

/** A port whose transmit queue has one descriptor, of a device that takes
 * each buffer late: every page waits for the descriptor, and none of
 * 20,000 bytes is lost. */
static void small_queue(void)
{
    fresh("console=vport0p1", MULTI | UART);
    m.qmax[5] = 1;
    check(virtio_console_start() == 1, "the boot binds the console");
    m.tx_lag = 2;
    m.port[1].nout = 0;
    fill(0, 20000);
    say(big, 20000);
    serial_flush();
    check(m.port[1].nout == 20000 && memcmp(m.port[1].out, big, 20000) == 0,
          "a queue of one descriptor loses nothing");
    check(counters().tx_dropped == 0 && counters().tx_waits == 0, "and no wait reaches its bound");
}

/** A device the transport can give no interrupt: refused before it takes
 * the console, and given back reset. */
static void polled(void)
{
    fresh("console=vport0p1", MULTI | UART);
    m.polled = 1;
    check(virtio_console_start() == -EIO, "a device with no interrupt is refused");
    check(!term_node("vport0p1") && _line == 0 && m.status == 0 && m.puts == 1 && m.dma_out == 0,
          "with no line, the device reset and given back");
    check(strstr(logged, "virtio_console: refused: virtio0: no interrupt") != 0,
          "and the log says why");
}

/** The input port removed with input in the ring: the input goes, the
 * terminal is hung up, and the console goes back to the UART. */
static void port_hangup(void)
{
    fresh("console=vport0p1", MULTI | UART);
    check(virtio_console_start() == 1, "the boot binds the console");
    irq();
    host_write(1, "abc", 3);
    irq();
    check(serial_rx_pending() == 3, "the port's input is in the ring");
    post(1, VIRTIO_CONSOLE_PORT_REMOVE, 1, 0, 0);
    model_ctl_give();
    irq();
    check(hangups == 1 && serial_rx_pending() == 0 && !term_node("vport0p1"),
          "its removal drops its input and hangs the terminal up");
}

/** A machine without a UART whose port comes after the boot's wait: the
 * port is given the log's ring when it first comes, and not again when it
 * comes back. */
static void late_replay(void)
{
    fresh("console=vport0p1", MULTI);
    m.silent = 1;
    check(virtio_console_start() == 1, "the boot binds a device that announces nothing");
    check(replays == 0, "no port, no replay");
    announce(0);
    announce(1);
    model_ctl_give();
    irq();
    check(term_node("vport0p1") && replays == 1 && host_read(1, "REPLAY\r\n"),
          "the port that comes late is given the log's ring");
    post(1, VIRTIO_CONSOLE_PORT_REMOVE, 1, 0, 0);
    announce(1);
    model_ctl_give();
    irq();
    check(term_node("vport0p1") && replays == 1, "and not again when it comes back");
}

/** A console port found after another took hvc0: each keeps its number,
 * and the console stays where the boot line put it. */
static void hvc_numbers(void)
{
    fresh("console=hvc0", MULTI | UART);
    m.port[0].exists = 0;
    m.port[1].console = 1;
    check(virtio_console_start() == 1, "the boot binds the console");
    irq();
    check(term_node("hvc0") && vcon_in == P(1), "the first console port, port 1, is hvc0");
    m.port[0].exists = 1;
    announce(0);
    model_ctl_give();
    irq();
    check(P(0)->console && P(0)->hvc == 1 && P(1)->hvc == 0,
          "port 0, a console port found later, is hvc1");
    check(term_node("hvc0") && !term_node("hvc1"), "and /dev/hvc0 stays the console's node");
    m.port[1].nout = 0;
    say("one", 3);
    serial_flush();
    check(host_read(1, "one") && m.port[0].nout == 0, "on port 1");
}

/** `console=vport0p1 console=hvc0` on a machine without a UART: both
 * ports carry the log, each given the ring once, and the input is hvc0's;
 * one gone, the other goes on. */
static void two_outputs(void)
{
    fresh("console=vport0p1 console=hvc0", MULTI);
    check(virtio_console_start() == 1, "the boot binds the console");
    irq();
    check(term_node("hvc0") && !term_node("vport0p1"), "the input is the port named last");
    check(strstr(logged, "console: enabled: vport0p1, port 1 of virtio0, for output alone\n") &&
              strstr(logged, "console: enabled: hvc0, port 0 of virtio0\n"),
          "the log names both");
    say("both", 4);
    serial_flush();
    check(replays == 2 && host_read(0, "REPLAY\r\nboth") && host_read(1, "REPLAY\r\nboth"),
          "each port is given the ring once and then the log");
    host_write(0, "k", 1);
    host_write(1, "j", 1);
    irq();
    check(serial_rx_pending() == 1 && serial_rx_take() == 'k' && counters().rx_dropped == 1,
          "the input is hvc0's, and the other port's is dropped");
    post(1, VIRTIO_CONSOLE_PORT_REMOVE, 1, 0, 0);
    model_ctl_give();
    irq();
    check(hangups == 0 && term_node("hvc0"), "the output port goes without a hangup");
    say("one", 3);
    serial_flush();
    check(host_read(0, "REPLAY\r\nbothone"), "and the other carries the log on");
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
        one_port_no_memory();
        words();
        output();
        stuck();
        input();
        resize();
        replug();
        removal();
        panic();
        panic_queue_held();
        second();
        no_memory();
        third_port();
        vport();
        vport_reopen();
        hvc_tty();
        late_console();
        emergency();
        late_and_absent();
        unbind_pending();
        shutdown();
        control_late();
        small_queue();
        polled();
        port_hangup();
        late_replay();
        hvc_numbers();
        two_outputs();
    }
    drop_bound();
    if (failures) {
        printf("virtio-console-hosttest: FAIL: %d\n", failures);
        return 1;
    }
    printf("virtio-console-hosttest: PASS\n");
    return 0;
}
