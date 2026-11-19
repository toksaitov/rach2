/* tap.c: /dev/net/tun, Linux's TAP interface, as tap.h describes it: the
 * interfaces, their rings of frames for the reader, and the descriptor's
 * operations. */
#include "tap.h"
#include "netdev.h"
#include "../ethernet/ethernet.h"
#include "../net.h"
#include "../netconf.h"
#include "../arp/arp.h"
#include "../ip6/route6.h"
#include "../ip6/nd6.h"
#include "../../fs/devfs.h"
#include "../../fs/file.h"
#include "../../io/poll.h"
#include "../../klog.h"
#include "../../lib/kerrno.h"
#include "../../lib/krandom.h"
#include "../../lib/kstring.h"
#include "../../lib/spinlock.h"
#include "../../mm/kheap.h"
#include "../../mm/usercopy.h"
#include "../../proc/wait_queue.h"
#include "../../fs/debugfs.h"
#include "../tcp/tcp.h"

/* The longest frame an interface carries, without the frame check
 * sequence, and the room each ring slot takes. */
#define TAP_FRAME  (ETH_HDR_SIZE + ETH_MTU)
#define TAP_STRIDE 1536u
#define TAP_PAGES  ((TAP_RING * TAP_STRIDE + 4095u) / 4096u)

/* The bytes of Linux's `struct ifreq` this file reads and writes: the
 * name, and the flags, a short, right after it. */
#define TAP_IFNAME   16
#define TAP_IFR_SIZE (TAP_IFNAME + 2)

/**
 * One interface. The record is in the kernel's image and is never freed,
 * since the ARP cache keeps a pointer to an interface's name; an interface
 * that is closed gives its slot to the next TUNSETIFF.
 */
struct tap {
    struct netdev nd;
    int used;     /* the interface is registered */
    int attached; /* a descriptor holds it, so it has a carrier */
    int persist;  /* TUNSETPERSIST: it outlives its descriptor */
    /* The ring: TAP_RING slots of TAP_STRIDE bytes, taken at the first use
     * and kept; `count` frames from slot `head`, each of `len` bytes. */
    uint8_t *frames;
    uint16_t len[TAP_RING];
    uint32_t head, count;
    wait_queue_t wq; /* the reader and the pollers wait here */
    /* Two frame buffers kept between calls, one for a read and one for a
     * write, so that neither allocates for each frame. */
    uint8_t *spare[2];
    struct tap_backend *backend; /* takes the frames in place of the reader */
};

static struct tap taps[TAP_MAX];

/* Guards every ring and spare buffer; taken with interrupts off, since a
 * frame is sent from interrupt handlers too. */
static struct spinlock tap_lock = SPINLOCK_INIT_NAMED("tap_lock");

static uint64_t tap_read_frames, tap_written_frames, tap_dropped_frames, tap_thinned_frames;

/* 1 to let a bare acknowledgement replace the one before it in the ring
 * that it makes redundant (`tap_ack_covers`). Set through debugfs
 * net/tap_thin_acks. */
static int tap_thin_acks;

void tap_counts(uint64_t *read, uint64_t *written, uint64_t *dropped, uint64_t *thinned)
{
    uint32_t flags = spin_lock_irqsave(&tap_lock);

    *read = tap_read_frames;
    *written = tap_written_frames;
    *dropped = tap_dropped_frames;
    *thinned = tap_thinned_frames;
    spin_unlock_irqrestore(&tap_lock, flags);
}

/** A frame buffer of TAP_FRAME bytes: a spare of `t`, or a new one; NULL
 * when the heap has none. */
static uint8_t *buf_take(struct tap *t)
{
    uint32_t flags = spin_lock_irqsave(&tap_lock);
    uint8_t *b = 0;
    int i;

    for (i = 0; i < 2 && !b; i++) {
        b = t->spare[i];
        t->spare[i] = 0;
    }
    spin_unlock_irqrestore(&tap_lock, flags);
    return b ? b : (uint8_t *)kmalloc(TAP_FRAME);
}

/** Give `b` back: kept as a spare of `t` when one is missing, else freed. */
static void buf_give(struct tap *t, uint8_t *b)
{
    uint32_t flags = spin_lock_irqsave(&tap_lock);
    int i;

    for (i = 0; i < 2 && b; i++) {
        if (!t->spare[i]) {
            t->spare[i] = b;
            b = 0;
        }
    }
    spin_unlock_irqrestore(&tap_lock, flags);
    kfree(b);
}

/** The TCP header of the IPv4 frame `f` of `len` bytes when it is a bare
 * acknowledgement: no IP options or fragment, ACK alone set, no payload, and
 * no TCP option but a timestamp. NULL otherwise. */
static const uint8_t *tap_bare_ack(const uint8_t *f, uint16_t len)
{
    const uint8_t *tcp = f + ETH_HDR_SIZE + 20;
    uint32_t total, doff;

    if (len < ETH_HDR_SIZE + 40 || f[12] != 0x08 || f[13] != 0x00 || f[14] != 0x45 || f[23] != 6 ||
        (f[20] & 0x3f) != 0 || f[21] != 0) {
        return 0;
    }
    total = ((uint32_t)f[16] << 8) | f[17];
    doff = (uint32_t)(tcp[12] >> 4) * 4u;
    if (tcp[13] != 0x10 || total != 20u + doff || ETH_HDR_SIZE + total > len) {
        return 0;
    }
    if (doff == 20u) {
        return tcp;
    }
    if (doff == 32u && tcp[20] == 1 && tcp[21] == 1 && tcp[22] == 8 && tcp[23] == 10) {
        return tcp;
    }
    return 0;
}

/** Whether the `n` bytes at `a` and at `b` are equal. */
static int tap_same(const uint8_t *a, const uint8_t *b, uint32_t n)
{
    while (n-- != 0) {
        if (*a++ != *b++) {
            return 0;
        }
    }
    return 1;
}

/**
 * Whether the bare acknowledgement `b`, unread in the ring, is made
 * redundant by the frame `a`, as sch_cake's ACK filter judges (RFC 3449,
 * 5.2.1): `a` is a bare acknowledgement of the same connection, with the
 * same options and sequence number, that acknowledges more. A duplicate
 * acknowledgement, which tells the peer of a loss, is never replaced.
 */
static int tap_ack_covers(const uint8_t *a, uint16_t alen, const uint8_t *b, uint16_t blen)
{
    const uint8_t *ta = tap_bare_ack(a, alen), *tb;
    uint32_t acka, ackb;

    if (!ta || (tb = tap_bare_ack(b, blen)) == 0 || ta[12] != tb[12] ||
        !tap_same(a + 26, b + 26, 12) || !tap_same(ta + 4, tb + 4, 4)) {
        return 0;
    }
    acka = ((uint32_t)ta[8] << 24) | ((uint32_t)ta[9] << 16) | ((uint32_t)ta[10] << 8) | ta[11];
    ackb = ((uint32_t)tb[8] << 24) | ((uint32_t)tb[9] << 16) | ((uint32_t)tb[10] << 8) | tb[11];
    return (int32_t)(acka - ackb) > 0;
}

/**
 * The driver table's `tx`: put the frame in the ring for the reader.
 *
 * A frame sent while no descriptor holds the interface, or while the ring
 * is full, is dropped, which the caller counts. Called from system calls
 * and interrupt handlers; it does not sleep.
 */
static int tap_tx(void *dev, const uint8_t *data, uint16_t len)
{
    struct tap *t = (struct tap *)dev;
    uint32_t flags;
    int queued = 0;

    if (len > TAP_FRAME) {
        return NETDEV_TX_DROPPED;
    }
    flags = spin_lock_irqsave(&tap_lock);
    if (t->backend && t->attached && t->count == 0 && t->backend->rx(t->backend, data, len)) {
        tap_read_frames++;
        spin_unlock_irqrestore(&tap_lock, flags);
        return NETDEV_TX_OK;
    }
    if (tap_thin_acks && t->attached && t->frames && t->count != 0) {
        uint32_t last = (t->head + t->count - 1u) % TAP_RING;
        uint8_t *slot = t->frames + last * TAP_STRIDE;

        if (tap_ack_covers(data, len, slot, t->len[last])) {
            k_memcpy(slot, data, len);
            t->len[last] = len;
            tap_thinned_frames++;
            spin_unlock_irqrestore(&tap_lock, flags);
            return NETDEV_TX_OK;
        }
    }
    if (t->attached && t->frames && t->count < TAP_RING) {
        uint32_t slot = (t->head + t->count) % TAP_RING;

        k_memcpy(t->frames + slot * TAP_STRIDE, data, len);
        t->len[slot] = len;
        t->count++;
        queued = 1;
    } else {
        tap_dropped_frames++;
    }
    spin_unlock_irqrestore(&tap_lock, flags);
    if (queued) {
        wq_wake(&t->wq, WQ_DATA);
    }
    return queued ? NETDEV_TX_OK : NETDEV_TX_DROPPED;
}

/** The driver table's `link`: the carrier is the descriptor. The speed is
 * the 10 Mbit/s Linux's tun driver reports, which limits nothing. */
static int tap_link(void *dev, struct netdev_link *out)
{
    struct tap *t = (struct tap *)dev;

    out->up = t->attached;
    out->mbps = 10;
    out->full_duplex = 1;
    return 1;
}

static const struct netdev_ops tap_ops = {"tap", 0, tap_tx, 0, 0, tap_link, 0, 0, 0, 0};

/** The interface of that name among this file's, or NULL. */
static struct tap *tap_named(const char *name)
{
    struct netdev *nd = netdev_by_name(name);

    return nd && nd->ops == &tap_ops ? (struct tap *)nd->dev : 0;
}

/**
 * Make the interface `name` in a free record and register it: the record,
 * or NULL with `*err` set to -ENOSPC when every record is used, -ENOMEM
 * without room for the ring, or `netdev_register`'s error. The interface
 * starts down, with a random station address that has the locally
 * administered bit, as Linux gives a tap.
 */
static struct tap *tap_make(const char *name, int32_t *err)
{
    struct tap *t = 0;
    int i, rc;

    for (i = 0; i < TAP_MAX && !t; i++) {
        if (!taps[i].used) {
            t = &taps[i];
        }
    }
    if (!t) {
        *err = -ENOSPC;
        return 0;
    }
    if (!t->frames) {
        t->frames = (uint8_t *)kalloc_pages(TAP_PAGES, 0);
        if (!t->frames) {
            *err = -ENOMEM;
            return 0;
        }
    }
    k_memset(&t->nd, 0, sizeof(t->nd));
    t->nd.ops = &tap_ops;
    t->nd.dev = t;
    t->nd.pci_key = -1;
    t->nd.mtu = NETDEV_ETH_MTU_MAX;
    k_strncpy(t->nd.name, name, sizeof(t->nd.name) - 1);
    krandom_bytes(t->nd.mac, 6);
    t->nd.mac[0] = (uint8_t)((t->nd.mac[0] & 0xfeu) | 0x02u);
    t->head = 0;
    t->count = 0;
    t->attached = 0;
    t->persist = 0;
    rc = netdev_register(&t->nd);
    if (rc < 0) {
        *err = rc;
        return 0;
    }
    t->used = 1;
    return t;
}

/**
 * Withdraw the interface of `t`: down, its addresses, routes and
 * neighbours forgotten, as the radio layer withdraws its interface
 * (wireless/wiphy.c), and the record free for the next TUNSETIFF.
 */
static void tap_remove(struct tap *t)
{
    struct netdev_rt r;
    struct arp_row neighbors[ARP_CACHE_SIZE];
    struct nd6_row neighbors6[ND6_CACHE_SIZE];
    uint32_t flags;
    int n, count;

    flags = net_lock();
    netconf_set_up(&t->nd, 0);
    nd6_if_down(&t->nd);
    route6_forget(&t->nd, -1);
    for (n = 0; netdev_route_at(n, &r);) {
        if (r.ifindex == t->nd.index && r.kind == NETDEV_RT_ADDED) {
            netdev_route_del(&t->nd, r.dst, r.mask, r.gw);
        } else {
            ++n;
        }
    }
    netconf_set_address(&t->nd, 0, 0);
    arp_forget_dev(&t->nd);
    count = arp_snapshot(neighbors, ARP_CACHE_SIZE);
    for (n = 0; n < count; ++n) {
        if (neighbors[n].iface == t->nd.name) {
            arp_forget(&t->nd, neighbors[n].ip);
        }
    }
    count = nd6_snapshot(neighbors6, ND6_CACHE_SIZE);
    for (n = 0; n < count; ++n) {
        if (neighbors6[n].ifindex == t->nd.index) {
            nd6_forget(&t->nd, neighbors6[n].addr);
        }
    }
    netdev_unregister(&t->nd);
    net_unlock(flags);
    klog(KLOG_INFO, "tap", "%s: removed", t->nd.name);
    t->used = 0;
}

/** Whether TUNSETIFF's `flags` ask for what this device is: Ethernet
 * frames with no header before them, on one queue. */
static int tap_flags_ok(uint16_t flags)
{
    return (flags & TAP_IFF_TAP) && (flags & TAP_IFF_NO_PI) &&
           !(flags & (TAP_IFF_TUN | TAP_IFF_VNET_HDR | TAP_IFF_MULTI_QUEUE));
}

/**
 * TUNSETIFF: give the descriptor `e` the interface the `ifreq` at `arg`
 * names, made when it is not there. An empty name, or one with `%d`, takes
 * the first free `tapN`. -EINVAL for a descriptor that has an interface,
 * a request that is not IFF_TAP alone with IFF_NO_PI, a name the kernel's
 * interfaces cannot hold or that another kind of interface has; -EPERM
 * without the privilege to change the interfaces; -EBUSY for an interface
 * another descriptor holds; else `tap_make`'s error.
 */
static int32_t tap_set_iff(fd_entry_t *e, uintptr_t arg)
{
    uint8_t ifr[TAP_IFR_SIZE];
    char name[TAP_IFNAME];
    struct tap *t;
    uint16_t flags;
    uint32_t irq;
    int32_t err = 0;
    int i, len;

    if (e->priv) {
        return -EINVAL;
    }
    if (!net_admin()) {
        return -EPERM;
    }
    if (copy_from_user(ifr, (const void __user *)arg, sizeof(ifr)) != 0) {
        return -EFAULT;
    }
    k_memcpy(&flags, ifr + TAP_IFNAME, 2);
    if (!tap_flags_ok(flags)) {
        return -EINVAL;
    }
    k_memcpy(name, ifr, TAP_IFNAME);
    name[TAP_IFNAME - 1] = '\0';
    len = (int)k_strlen(name);
    if (len == 0 || (len >= 2 && name[len - 2] == '%' && name[len - 1] == 'd')) {
        char stem[TAP_IFNAME];

        k_strncpy(stem, len == 0 ? "tap" : name, sizeof(stem) - 1);
        stem[sizeof(stem) - 1] = '\0';
        if (len != 0) {
            stem[len - 2] = '\0';
        }
        len = (int)k_strlen(stem);
        if (len == 0 || len > (int)sizeof(taps[0].nd.name) - 2) {
            return -EINVAL;
        }
        for (i = 0; i < 10; i++) {
            k_memcpy(name, stem, (size_t)len);
            name[len] = (char)('0' + i);
            name[len + 1] = '\0';
            if (!netdev_by_name(name)) {
                break;
            }
        }
        if (i == 10) {
            return -ENFILE;
        }
        t = 0;
    } else {
        if (len > (int)sizeof(taps[0].nd.name) - 1) {
            return -EINVAL;
        }
        t = tap_named(name);
        if (!t && netdev_by_name(name)) {
            return -EINVAL;
        }
    }
    if (t && t->attached) {
        return -EBUSY;
    }
    if (!t) {
        t = tap_make(name, &err);
        if (!t) {
            return err;
        }
    }
    irq = spin_lock_irqsave(&tap_lock);
    t->head = 0;
    t->count = 0;
    t->attached = 1;
    spin_unlock_irqrestore(&tap_lock, irq);
    netdev_carrier_changed(&t->nd);
    e->priv = t;
    k_memset(ifr, 0, TAP_IFNAME);
    k_strncpy((char *)ifr, t->nd.name, TAP_IFNAME - 1);
    flags = (uint16_t)(TAP_IFF_TAP | TAP_IFF_NO_PI);
    k_memcpy(ifr + TAP_IFNAME, &flags, 2);
    klog(KLOG_INFO, "tap", "%s: attached", t->nd.name);
    return copy_to_user((void __user *)arg, ifr, sizeof(ifr)) != 0 ? -EFAULT : 0;
}

/** The wait condition of a read: a frame is queued, or the interface left
 * its descriptor. */
static int tap_readable(void *arg)
{
    struct tap *t = (struct tap *)arg;

    return t->count != 0 || !t->attached;
}

/**
 * Read one frame, whole, or its first `count` bytes when the buffer is
 * shorter, as Linux's tun does. -EBADFD before TUNSETIFF; -EAGAIN on a
 * descriptor that does not block with no frame queued; -EINTR when a
 * signal ends the wait.
 */
static int32_t tap_read(fd_entry_t *e, char __user *buf, uint32_t count)
{
    struct tap *t = (struct tap *)e->priv;
    uint8_t *b;
    uint32_t flags, n = 0;
    int got = 0;

    if (!t) {
        return -EBADFD;
    }
    b = buf_take(t);
    if (!b) {
        return -ENOMEM;
    }
    while (!got) {
        int gone;

        flags = spin_lock_irqsave(&tap_lock);
        if (t->count) {
            n = t->len[t->head];
            k_memcpy(b, t->frames + t->head * TAP_STRIDE, n);
            t->head = (t->head + 1) % TAP_RING;
            t->count--;
            tap_read_frames++;
            got = 1;
        }
        gone = !t->attached;
        spin_unlock_irqrestore(&tap_lock, flags);
        if (got) {
            break;
        }
        if (gone) {
            buf_give(t, b);
            return -EBADFD;
        }
        if (e->ofd->status_flags & OFD_NONBLOCK) {
            buf_give(t, b);
            return -EAGAIN;
        }
        if (wq_wait_event(&t->wq, tap_readable, t) < 0) {
            buf_give(t, b);
            return -EINTR;
        }
    }
    if (n > count) {
        n = count;
    }
    if (copy_to_user(buf, b, n) != 0) {
        buf_give(t, b);
        return -EFAULT;
    }
    buf_give(t, b);
    return (int32_t)n;
}

struct tap *tap_of(const struct fd_entry *e)
{
    return tap_is(e) ? (struct tap *)e->priv : 0;
}

int tap_backend_set(struct tap *t, struct tap_backend *b)
{
    uint32_t flags = spin_lock_irqsave(&tap_lock);
    int r = 0;

    if (b && t->backend && t->backend != b) {
        r = -EBUSY;
    } else {
        t->backend = b;
    }
    spin_unlock_irqrestore(&tap_lock, flags);
    return r;
}

void tap_backend_drain(struct tap *t)
{
    uint32_t flags = spin_lock_irqsave(&tap_lock);

    while (t->backend && t->count != 0 &&
           t->backend->rx(t->backend, t->frames + t->head * TAP_STRIDE, t->len[t->head])) {
        t->head = (t->head + 1) % TAP_RING;
        t->count--;
        tap_read_frames++;
    }
    spin_unlock_irqrestore(&tap_lock, flags);
}

int32_t tap_write_frame(struct fd_entry *e, const uint8_t *frame, uint32_t len)
{
    struct tap *t = (struct tap *)e->priv;

    if (!t) {
        return -EBADFD;
    }
    return tap_inject(t, frame, len);
}

int32_t tap_inject(struct tap *t, const uint8_t *frame, uint32_t len)
{
    uint32_t flags;

    if (len < ETH_HDR_SIZE) {
        return -EINVAL;
    }
    flags = net_lock();
    if (len > TAP_FRAME) {
        t->nd.rx_dropped++;
    } else {
        ethernet_rx(&t->nd, frame, (uint16_t)len);
    }
    net_unlock(flags);
    flags = spin_lock_irqsave(&tap_lock);
    tap_written_frames++;
    spin_unlock_irqrestore(&tap_lock, flags);
    return (int32_t)len;
}

/**
 * Write one frame, which the interface receives (`tap_write_frame`). A
 * frame longer than the interface carries is not copied in: it is counted
 * as dropped and taken, so that the writer goes on.
 */
static int32_t tap_write(fd_entry_t *e, const char __user *buf, uint32_t count)
{
    struct tap *t = (struct tap *)e->priv;
    uint8_t *b;
    int32_t r;

    if (!t) {
        return -EBADFD;
    }
    if (count < ETH_HDR_SIZE || count > TAP_FRAME) {
        return tap_write_frame(e, 0, count);
    }
    b = buf_take(t);
    if (!b) {
        return -ENOMEM;
    }
    if (copy_from_user(b, buf, count) != 0) {
        buf_give(t, b);
        return -EFAULT;
    }
    r = tap_write_frame(e, b, count);
    buf_give(t, b);
    return r;
}

/** A descriptor is readable with a frame queued and always writable; one
 * with no interface reports an error, as Linux's does. */
static short tap_poll(fd_entry_t *e, short events, struct wait_queue **wq)
{
    struct tap *t = (struct tap *)e->priv;

    if (!t) {
        *wq = 0;
        return POLLERR;
    }
    *wq = &t->wq;
    return (short)(events & ((t->count ? POLLIN : 0) | POLLOUT));
}

/**
 * The descriptor's control. TUNGETFEATURES answers IFF_TAP | IFF_NO_PI,
 * so that a caller asks for no virtio header; TUNSETOFFLOAD takes only 0,
 * no offload; the send buffer, the owner, the group and the debug and
 * checksum words are taken and ignored; the virtio header's size and a
 * second queue are -EINVAL. -EBADFD for a request that needs the interface
 * before TUNSETIFF, -ENOTTY for any other request.
 */
static int32_t tap_ioctl(fd_entry_t *e, uint32_t request, uintptr_t arg)
{
    struct tap *t = (struct tap *)e->priv;
    uint8_t ifr[TAP_IFR_SIZE];
    uint32_t word;
    uint16_t flags;

    switch (request) {
    case TUNGETFEATURES:
        word = TAP_IFF_TAP | TAP_IFF_NO_PI;
        return copy_to_user((void __user *)arg, &word, sizeof(word)) != 0 ? -EFAULT : 0;
    case TUNSETIFF:
        return tap_set_iff(e, arg);
    case TUNSETVNETHDRSZ:
    case TUNGETVNETHDRSZ:
    case TUNSETVNETLE:
    case TUNGETVNETLE:
    case TUNSETVNETBE:
    case TUNGETVNETBE:
    case TUNSETQUEUE:
        return -EINVAL;
    default:
        break;
    }
    if (request != TUNGETIFF && request != TUNSETPERSIST && request != TUNSETOFFLOAD &&
        request != TUNSETSNDBUF && request != TUNGETSNDBUF && request != TUNSETOWNER &&
        request != TUNSETGROUP && request != TUNSETNOCSUM && request != TUNSETDEBUG &&
        request != TUNSETLINK) {
        return -ENOTTY;
    }
    if (!t) {
        return -EBADFD;
    }
    switch (request) {
    case TUNGETIFF:
        k_memset(ifr, 0, sizeof(ifr));
        k_strncpy((char *)ifr, t->nd.name, TAP_IFNAME - 1);
        flags = (uint16_t)(TAP_IFF_TAP | TAP_IFF_NO_PI | (t->persist ? TAP_IFF_PERSIST : 0));
        k_memcpy(ifr + TAP_IFNAME, &flags, 2);
        return copy_to_user((void __user *)arg, ifr, sizeof(ifr)) != 0 ? -EFAULT : 0;
    case TUNSETPERSIST:
        t->persist = arg != 0;
        return 0;
    case TUNSETOFFLOAD:
        return arg == 0 ? 0 : -EINVAL;
    case TUNGETSNDBUF:
        word = 0x7fffffffu;
        return copy_to_user((void __user *)arg, &word, sizeof(word)) != 0 ? -EFAULT : 0;
    default:
        return 0;
    }
}

/**
 * The last descriptor of the open closes: the interface loses its carrier
 * and its queued frames, a reader that waits is woken, and unless
 * TUNSETPERSIST keeps it the interface is withdrawn.
 */
static void tap_close(fd_entry_t *e)
{
    struct tap *t = (struct tap *)e->priv;
    struct tap_backend *b;
    uint32_t flags;

    if (!t) {
        return;
    }
    e->priv = 0;
    flags = spin_lock_irqsave(&tap_lock);
    t->attached = 0;
    t->count = 0;
    b = t->backend;
    t->backend = 0;
    spin_unlock_irqrestore(&tap_lock, flags);
    if (b && b->gone) {
        b->gone(b);
    }
    wq_wake_all(&t->wq);
    netdev_carrier_changed(&t->nd);
    klog(KLOG_INFO, "tap", "%s: detached", t->nd.name);
    if (!t->persist) {
        tap_remove(t);
    }
}

static const struct file_ops tap_file_ops = {tap_read,  tap_write, tap_poll, tap_ioctl,
                                             tap_close, NULL,      NULL,     0};

int tap_is(const struct fd_entry *e)
{
    return e && e->ops == &tap_file_ops;
}

/* Linux's numbers for the device, 10:200. */
static struct devfs_node tap_node = {"net/tun", 10, 200, 0666, &tap_file_ops, NULL, NULL};

void tap_start(void)
{
    int32_t r = devfs_node_register(&tap_node);

    if (r != 0) {
        klog(KLOG_WARN, "tap", "/dev/net/tun not made: error %d", r);
    }
    if (debugfs_create_text("net/tap_thin_acks", 0644u, &tcp_switch_ops, &tap_thin_acks) != 0) {
        klog(KLOG_WARN, "tap", "failed: net/tap_thin_acks not registered");
    }
}
