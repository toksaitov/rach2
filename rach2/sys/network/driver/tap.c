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
#include "../ip6/ip6.h"
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
#include "../../fs/krffs.h"
#include "../../syscall.h"
#include "../../syscall_internal.h"

/* The longest frame an interface carries, without the frame check
 * sequence, and the room each ring slot takes. */
#define TAP_FRAME  (ETH_HDR_SIZE + ETH_MTU)
#define TAP_STRIDE 1536u
#define TAP_PAGES  ((TAP_RING * TAP_STRIDE + 4095u) / 4096u)

/* Linux's `struct ifreq`: the name, then a union whose widest member, a
 * `struct ifmap` of two longs, makes it 32 bytes on i686 and 40 on x86-64;
 * an i386 caller's on x86-64 is 32, Linux's compat_ifreq. The flags are a
 * short at the union's start. */
struct tap_ifreq_shape {
    char name[16];
    union {
        uint8_t addr[16];
        struct {
            unsigned long mem_start, mem_end;
            unsigned short base_addr;
            unsigned char irq, dma, port;
        } map;
        uintptr_t data;
    } u;
};
#define TAP_IFNAME     16
#define TAP_IFR_MAX    ((uint32_t)sizeof(struct tap_ifreq_shape))
#define TAP_IFR_COMPAT 32u

/** The bytes of the caller's `struct ifreq`, as Linux's tun copies them
 * (`ifreq_len` in __tun_chr_ioctl). */
static uint32_t tap_ifr_size(void)
{
    uint32_t n = TAP_IFR_MAX;

    if (in_compat_syscall()) {
        n = TAP_IFR_COMPAT; /* equal to the native size on i686 */
    }
    return n;
}

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

/* One open of /dev/net/tun, Linux's `struct tun_file`: every descriptor of
 * the open file description has it as its `priv`, so that a descriptor
 * duplicated before TUNSETIFF shares the interface the call gives. */
struct tap_file {
    struct tap *t; /* the interface; NULL before TUNSETIFF */
};

/** The interface of the descriptor `e`, an open of /dev/net/tun, or NULL
 * before TUNSETIFF. */
static struct tap *tap_file_of(const fd_entry_t *e)
{
    return e->priv ? ((const struct tap_file *)e->priv)->t : 0;
}

/* Guards every ring and spare buffer; taken with interrupts off, since a
 * frame is sent from interrupt handlers too. */
static struct spinlock tap_lock = SPINLOCK_INIT_NAMED("tap_lock");

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
        spin_unlock_irqrestore(&tap_lock, flags);
        return NETDEV_TX_OK;
    }
    if (t->attached && t->frames && t->count < TAP_RING) {
        uint32_t slot = (t->head + t->count) % TAP_RING;

        k_memcpy(t->frames + slot * TAP_STRIDE, data, len);
        t->len[slot] = len;
        t->count++;
        queued = 1;
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
 * for a record whose ring `tap_rings_ready` could not take, or
 * `netdev_register`'s error. It does not sleep. The interface
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
        *err = -ENOMEM;
        return 0;
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

/** Give each free record its ring, which it keeps: 0, or -ENOMEM. It may
 * sleep, so TUNSETIFF calls it before the checks it must make in one hold
 * of the big lock. */
static int32_t tap_rings_ready(void)
{
    int i;

    for (i = 0; i < TAP_MAX; i++) {
        if (!taps[i].used && !taps[i].frames) {
            taps[i].frames = (uint8_t *)kalloc_pages(TAP_PAGES, 0);
            if (!taps[i].frames) {
                return -ENOMEM;
            }
        }
    }
    return 0;
}

/**
 * Withdraw the interface of `t`: down, its addresses, routes and
 * neighbours and IPv6 reassemblies forgotten, as the radio layer
 * withdraws its interface (wireless/wiphy.c), and the record free for the
 * next TUNSETIFF.
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
    ip6_forget_dev(&t->nd);
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

/** Whether `name` may name an interface, as Linux's dev_valid_name judges:
 * not "." or "..", and no '/', ':' or white space. */
static int tap_name_ok(const char *name)
{
    if (name[0] == '.' && (name[1] == '\0' || (name[1] == '.' && name[2] == '\0'))) {
        return 0;
    }
    for (; *name; name++) {
        if (*name == '/' || *name == ':' || *name == ' ' || (*name >= '\t' && *name <= '\r')) {
            return 0;
        }
    }
    return 1;
}

/**
 * The name TUNSETIFF's `name` asks for, in `out`: an empty name, or one
 * that ends in `%d`, takes the first free `tapN` or stem and digit. 0;
 * -EINVAL for a name the kernel's interfaces cannot hold; -ENFILE when no
 * digit is free. `*fresh` is 1 for a name made from a stem.
 */
static int32_t tap_pick_name(const char *name, char *out, int *fresh)
{
    char stem[TAP_IFNAME];
    int i, len = (int)k_strlen(name);

    *fresh = len == 0 || (len >= 2 && name[len - 2] == '%' && name[len - 1] == 'd');
    if (!*fresh) {
        if (len > (int)sizeof(taps[0].nd.name) - 1 || !tap_name_ok(name)) {
            return -EINVAL;
        }
        k_strncpy(out, name, TAP_IFNAME - 1);
        out[TAP_IFNAME - 1] = '\0';
        return 0;
    }
    k_strncpy(stem, len == 0 ? "tap" : name, sizeof(stem) - 1);
    stem[sizeof(stem) - 1] = '\0';
    if (len != 0) {
        stem[len - 2] = '\0';
    }
    len = (int)k_strlen(stem);
    if (len == 0 || len > (int)sizeof(taps[0].nd.name) - 2 || !tap_name_ok(stem)) {
        return -EINVAL;
    }
    for (i = 0; i < 10; i++) {
        k_memcpy(out, stem, (size_t)len);
        out[len] = (char)('0' + i);
        out[len + 1] = '\0';
        if (!netdev_by_name(out)) {
            return 0;
        }
    }
    return -ENFILE;
}

/**
 * TUNSETIFF: give the descriptor `e` the interface the `ifreq` at `arg`
 * names, made when it is not there (`tap_pick_name`), and write its name
 * back, with the flags as the caller gave them, as Linux's tun_set_iff.
 * -EEXIST for an open that has an interface, as Linux's; -EINVAL for a
 * request that is not IFF_TAP with IFF_NO_PI, or a name the kernel's
 * interfaces cannot hold or that another kind of interface has; -EPERM
 * without the privilege to change the interfaces; -EOPNOTSUPP in a network
 * namespace other than the first, which has no tap; -EBUSY for an
 * interface another descriptor holds, or any that exists under
 * IFF_TUN_EXCL; else `tap_make`'s error.
 *
 * The request is copied in and the rings made first, since either may
 * sleep and let another call on a descriptor of the same open run; from
 * the checks to the record's interface nothing sleeps.
 */
static int32_t tap_set_iff(fd_entry_t *e, uintptr_t arg)
{
    uint8_t ifr[TAP_IFR_MAX];
    char name[TAP_IFNAME];
    struct tap *t;
    uint16_t flags;
    uint32_t irq, size = tap_ifr_size();
    int32_t err = 0;
    int fresh;

    k_memset(ifr, 0, sizeof(ifr));
    if (copy_from_user(ifr, (const void __user *)arg, size) != 0) {
        return -EFAULT;
    }
    k_memcpy(&flags, ifr + TAP_IFNAME, 2);
    ifr[TAP_IFNAME - 1] = 0;
    err = tap_rings_ready();
    if (err != 0) {
        return err;
    }
    if (tap_file_of(e)) {
        return -EEXIST;
    }
    if (!net_ns_seen()) {
        return -EOPNOTSUPP;
    }
    if (!net_admin()) {
        return -EPERM;
    }
    if (!tap_flags_ok(flags)) {
        return -EINVAL;
    }
    err = tap_pick_name((const char *)ifr, name, &fresh);
    if (err != 0) {
        return err;
    }
    t = fresh ? 0 : tap_named(name);
    if (!t && netdev_by_name(name)) {
        return -EINVAL;
    }
    if (t && (t->attached || (flags & TAP_IFF_TUN_EXCL))) {
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
    ((struct tap_file *)e->priv)->t = t;
    klog(KLOG_INFO, "tap", "%s: attached", t->nd.name);
    k_memset(ifr, 0, TAP_IFNAME);
    k_strncpy((char *)ifr, t->nd.name, TAP_IFNAME - 1);
    return copy_to_user((void __user *)arg, ifr, size) != 0 ? -EFAULT : 0;
}

/** The wait condition of a read: a frame is queued, or the interface left
 * its descriptor. */
static int tap_readable(void *arg)
{
    struct tap *t = (struct tap *)arg;

    return t->count != 0 || !t->attached;
}

/**
 * Take the next frame of the descriptor `e`'s interface into a buffer of
 * `buf_take`'s, waiting for one unless the descriptor does not block: 0
 * with `*tp`, `*bp` and `*np` set, the caller to give the buffer back with
 * `buf_give`; -EBADFD before TUNSETIFF or once the interface left the
 * descriptor; -EAGAIN with no frame on a descriptor that does not block;
 * -EINTR when a signal ends the wait; -ENOMEM.
 */
static int32_t tap_take(fd_entry_t *e, struct tap **tp, uint8_t **bp, uint32_t *np)
{
    struct tap *t = tap_file_of(e);
    uint8_t *b;
    uint32_t flags;

    if (!t) {
        return -EBADFD;
    }
    b = buf_take(t);
    if (!b) {
        return -ENOMEM;
    }
    for (;;) {
        int got = 0, gone;

        flags = spin_lock_irqsave(&tap_lock);
        if (t->count) {
            *np = t->len[t->head];
            k_memcpy(b, t->frames + t->head * TAP_STRIDE, *np);
            t->head = (t->head + 1) % TAP_RING;
            t->count--;
            got = 1;
        }
        gone = !t->attached;
        spin_unlock_irqrestore(&tap_lock, flags);
        if (got) {
            *tp = t;
            *bp = b;
            return 0;
        }
        if (gone || (e->ofd->status_flags & OFD_NONBLOCK) ||
            wq_wait_event(&t->wq, tap_readable, t) < 0) {
            buf_give(t, b);
            return gone ? -EBADFD : (e->ofd->status_flags & OFD_NONBLOCK) ? -EAGAIN : -EINTR;
        }
    }
}

/**
 * Read one frame, whole, or its first `count` bytes when the buffer is
 * shorter, as Linux's tun does; errors as `tap_take`, or -EFAULT, which
 * loses the frame, as Linux's does.
 */
static int32_t tap_read(fd_entry_t *e, char __user *buf, uint32_t count)
{
    struct tap *t;
    uint8_t *b;
    uint32_t n;
    int32_t r = tap_take(e, &t, &b, &n);

    if (r != 0) {
        return r;
    }
    if (n > count) {
        n = count;
    }
    r = copy_to_user(buf, b, n) != 0 ? -EFAULT : (int32_t)n;
    buf_give(t, b);
    return r;
}

int32_t tap_readv(struct fd_entry *e, const struct kernel_iovec *iov, int count)
{
    struct tap *t;
    uint8_t *b;
    uint32_t n, at = 0;
    int32_t r = tap_take(e, &t, &b, &n);
    int i;

    if (r != 0) {
        return r;
    }
    for (i = 0; i < count && at < n; i++) {
        uint32_t len = iov[i].iov_len < n - at ? (uint32_t)iov[i].iov_len : n - at;

        if (len != 0 && copy_to_user((void __user *)iov[i].iov_base, b + at, len) != 0) {
            r = -EFAULT;
            break;
        }
        at += len;
    }
    buf_give(t, b);
    return r != 0 ? r : (int32_t)at;
}

int32_t tap_writev(struct fd_entry *e, const struct kernel_iovec *iov, int count, uint32_t total)
{
    struct tap *t = tap_file_of(e);
    uint8_t *b;
    uint32_t at = 0;
    int32_t r;
    int i;

    if (!t) {
        return -EBADFD;
    }
    if (total < ETH_HDR_SIZE || total > TAP_FRAME) {
        return tap_inject(t, 0, total);
    }
    b = buf_take(t);
    if (!b) {
        return -ENOMEM;
    }
    for (i = 0; i < count; i++) {
        if (iov[i].iov_len != 0 &&
            copy_from_user(b + at, (const void __user *)iov[i].iov_base, iov[i].iov_len) != 0) {
            buf_give(t, b);
            return -EFAULT;
        }
        at += (uint32_t)iov[i].iov_len;
    }
    r = tap_inject(t, b, total);
    buf_give(t, b);
    return r;
}

struct tap *tap_of(const struct fd_entry *e)
{
    return tap_is(e) ? tap_file_of(e) : 0;
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
    }
    spin_unlock_irqrestore(&tap_lock, flags);
}

/** Give the interface of the descriptor `e` the frame of `len` bytes at
 * `frame` (`tap_inject`); -EBADFD before TUNSETIFF. */
static int32_t tap_write_frame(fd_entry_t *e, const uint8_t *frame, uint32_t len)
{
    struct tap *t = tap_file_of(e);

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
    return (int32_t)len;
}

/**
 * Write one frame, which the interface receives (`tap_write_frame`). A
 * frame longer than the interface carries is not copied in: it is counted
 * as dropped and taken, so that the writer goes on.
 */
static int32_t tap_write(fd_entry_t *e, const char __user *buf, uint32_t count)
{
    struct tap *t = tap_file_of(e);
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
    struct tap *t = tap_file_of(e);

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
    struct tap *t = tap_file_of(e);
    uint8_t ifr[TAP_IFR_MAX];
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
        return copy_to_user((void __user *)arg, ifr, tap_ifr_size()) != 0 ? -EFAULT : 0;
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
 * The last descriptor of the open closes: the open's record is freed, and
 * the interface, if TUNSETIFF gave it one, loses its carrier
 * and its queued frames, a reader that waits is woken, and unless
 * TUNSETPERSIST keeps it the interface is withdrawn.
 */
static void tap_close(fd_entry_t *e)
{
    struct tap *t = tap_file_of(e);
    struct tap_backend *b;
    uint32_t flags;

    kfree(e->priv);
    e->priv = 0;
    if (!t) {
        return;
    }
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

/** The node's `open`: give the descriptor its open's record, with no
 * interface yet. 0, or -ENOMEM. */
static int32_t tap_open(struct devfs_node *node, struct fd_entry *e, int flags)
{
    (void)node;
    (void)flags;
    e->priv = kzalloc(sizeof(struct tap_file));
    return e->priv ? 0 : -ENOMEM;
}

/* Linux's numbers for the device, 10:200. */
static struct devfs_node tap_node = {"net/tun", 10, 200, 0666, &tap_file_ops, tap_open, NULL};

void tap_start(void)
{
    int32_t r = devfs_node_register(&tap_node);

    if (r != 0) {
        klog(KLOG_WARN, "tap", "/dev/net/tun not made: error %d", r);
    }
}
