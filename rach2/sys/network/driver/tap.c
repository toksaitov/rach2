/* tap.c: /dev/net/tun, Linux's TUN and TAP interfaces, as tap.h describes
 * them: the interfaces, their rings of frames for the reader, and the
 * descriptor's operations. */
#include "tap.h"
#include "netdev.h"
#include "../ethernet/ethernet.h"
#include "../net.h"
#include "../netconf.h"
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
#include "../../proc/ns.h"
#include "../../proc/process.h"
#include "../../proc/wait_queue.h"
#include "../../fs/krffs.h"
#include "../../syscall.h"
#include "../../syscall_internal.h"

/* The longest frame an interface carries, without the frame check
 * sequence, and the room each ring slot takes. A TUN's frame is its
 * datagram behind the header `ethernet_tx` builds, which carries only the
 * EtherType. */
#define TAP_FRAME  (ETH_HDR_SIZE + ETH_MTU)
#define TAP_STRIDE 1536u
#define TAP_PAGES  ((TAP_RING * TAP_STRIDE + 4095u) / 4096u)

/* Linux's `struct tun_pi`, before each frame without IFF_NO_PI: a 16-bit
 * word of flags, TUN_PKT_STRIP when a read cut the frame short, and the
 * frame's protocol, big-endian. */
#define TAP_PI        4u
#define TUN_PKT_STRIP 0x0001u

/* A frame buffer: the packet information, then the longest frame. */
#define TAP_BUF (TAP_PI + TAP_FRAME)

/* The flags of TUNSETIFF an interface keeps from its last attach, as
 * Linux's TUN_FEATURES, but those this device does not offer. */
#define TAP_KEPT (TAP_IFF_NO_PI | TAP_IFF_ONE_QUEUE)

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
 * One interface. A record is taken from the heap when no record is free
 * and is never freed, since the ARP cache keeps a pointer to an interface's
 * name; an interface that is removed gives its record to the next
 * TUNSETIFF, so the interfaces at once are bounded by the stack's alone.
 */
struct tap {
    struct netdev nd;
    struct tap *next; /* on `taps`, every record registered once */
    int used;         /* the interface is registered */
    int attached;     /* a descriptor holds it, so it has a carrier */
    int persist;      /* TUNSETPERSIST: it outlives its descriptor */
    /* TAP_IFF_TUN or TAP_IFF_TAP, which the interface was made as, and the
     * TAP_KEPT flags of the TUNSETIFF that last attached it. */
    uint16_t flags;
    /* TUNSETOWNER's user and TUNSETGROUP's group, kernel ids, or -1 for
     * none: who may attach the interface without CAP_NET_ADMIN. */
    int64_t owner, group;
    /* The ring: TAP_RING slots of TAP_STRIDE bytes, taken when the record
     * makes an interface; `count` frames from slot `head`, each of `len`
     * bytes. */
    uint8_t *frames;
    int ring_kept; /* an interface was registered with the ring, which stays */
    uint16_t len[TAP_RING];
    uint32_t head, count;
    wait_queue_t wq; /* the reader and the pollers wait here */
    /* Two frame buffers kept between calls, one for a read and one for a
     * write, so that neither allocates for each frame. */
    uint8_t *spare[2];
    struct tap_backend *backend; /* takes the frames in place of the reader */
};

static struct tap *taps;

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

/** A frame buffer of TAP_BUF bytes: a spare of `t`, or a new one; NULL
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
    return b ? b : (uint8_t *)kmalloc(TAP_BUF);
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
 * Shape the frame of `len` bytes at `b + TAP_PI` as a reader is given it,
 * as Linux's tun_put_user: a TUN's datagram without the header before it,
 * and without IFF_NO_PI a `struct tun_pi` first, whose protocol is the
 * frame's EtherType and whose flags are TUN_PKT_STRIP when `room`, the
 * reader's bytes, is short of the whole. `*p` is set to the first byte; the
 * bytes shaped, or -EINVAL for room short of the packet information, as
 * Linux's. The frame of a TUN is at least a header long.
 */
static int32_t tap_out(const struct tap *t, uint8_t *b, uint32_t len, uint32_t room, uint8_t **p)
{
    uint8_t *f = b + TAP_PI;
    uint8_t proto[2];
    uint16_t pf = 0;

    proto[0] = f[12];
    proto[1] = f[13];
    if (t->flags & TAP_IFF_TUN) {
        f += ETH_HDR_SIZE;
        len -= ETH_HDR_SIZE;
    }
    *p = f;
    if (t->flags & TAP_IFF_NO_PI) {
        return (int32_t)len;
    }
    if (room < TAP_PI) {
        return -EINVAL;
    }
    if (room < TAP_PI + len) {
        pf = TUN_PKT_STRIP;
    }
    f -= TAP_PI;
    k_memcpy(f, &pf, 2); /* host order, as Linux's */
    f[2] = proto[0];
    f[3] = proto[1];
    *p = f;
    return (int32_t)(TAP_PI + len);
}

/** Offer the frame of `len` bytes at `f` to the backend of `t`, shaped as
 * a reader would be given it (`tap_out`): 1 when it took it. Called with
 * the ring's lock held, whose holder alone uses the buffer of the shape. */
static int tap_backend_give(struct tap *t, const uint8_t *f, uint16_t len)
{
    static uint8_t shaped[TAP_BUF];
    uint8_t *p = (uint8_t *)shaped;
    int32_t n = len;

    if ((t->flags & (TAP_IFF_TUN | TAP_IFF_NO_PI)) != TAP_IFF_NO_PI) {
        k_memcpy(shaped + TAP_PI, f, len);
        n = tap_out(t, shaped, len, TAP_BUF, &p);
        f = p;
    }
    return t->backend->rx(t->backend, f, (uint16_t)n);
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

    if (len > TAP_FRAME || len < ETH_HDR_SIZE) {
        return NETDEV_TX_DROPPED;
    }
    flags = spin_lock_irqsave(&tap_lock);
    if (t->backend && t->attached && t->count == 0 && tap_backend_give(t, data, len)) {
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
 * Make the interface `name`, a TUN or a TAP as `type` says, in a free
 * record, or a new one when every record is used, and register it: the
 * record, or NULL with `*err` set to -ENOMEM for a record or ring the heap
 * cannot give, or `netdev_register`'s error, -ENOSPC once the stack has as
 * many interfaces as it takes. A record keeps its ring once an interface is
 * registered with it, and gives back a ring, or a new record, it made for a
 * registration that failed. It does not sleep, since the heap takes a
 * spinlock and waits for nothing. The interface starts down with no owner
 * or group; a TAP has a random station address with the locally
 * administered bit, as Linux gives one, and a TUN, an interface of IP
 * datagrams (`l3`), none.
 */
static struct tap *tap_make(const char *name, uint16_t type, int32_t *err)
{
    struct tap *t;
    int rc, fresh = 0;

    for (t = taps; t && t->used; t = t->next) {
    }
    if (!t) {
        t = (struct tap *)kzalloc(sizeof(*t));
        fresh = 1;
    }
    if (t && !t->frames) {
        t->frames = (uint8_t *)kalloc_pages(TAP_PAGES, 0);
    }
    if (!t || !t->frames) {
        if (fresh) {
            kfree(t);
        }
        *err = -ENOMEM;
        return 0;
    }
    k_memset(&t->nd, 0, sizeof(t->nd));
    t->nd.ops = &tap_ops;
    t->nd.dev = t;
    t->nd.pci_key = -1;
    t->nd.mtu = NETDEV_ETH_MTU_MAX;
    t->nd.l3 = type == TAP_IFF_TUN;
    k_strncpy(t->nd.name, name, sizeof(t->nd.name) - 1);
    if (!t->nd.l3) {
        krandom_bytes(t->nd.mac, 6);
        t->nd.mac[0] = (uint8_t)((t->nd.mac[0] & 0xfeu) | 0x02u);
    }
    t->head = 0;
    t->count = 0;
    t->attached = 0;
    t->persist = 0;
    t->flags = type;
    t->owner = -1;
    t->group = -1;
    rc = netdev_register(&t->nd);
    if (rc < 0) {
        if (!t->ring_kept) {
            kfree_pages(t->frames, TAP_PAGES);
            t->frames = 0;
        }
        if (fresh) {
            kfree(t);
        }
        *err = rc;
        return 0;
    }
    t->used = 1;
    t->ring_kept = 1;
    if (fresh) {
        t->next = taps;
        taps = t;
    }
    return t;
}

/**
 * Withdraw the interface of `t` from the stack (`netdev_withdraw`), as the
 * radio layer withdraws its interface (wireless/wiphy.c), and free the
 * record for the next TUNSETIFF.
 */
static void tap_remove(struct tap *t)
{
    netdev_withdraw(&t->nd);
    klog(KLOG_INFO, "tap", "%s: removed", t->nd.name);
    t->used = 0;
}

/** Whether TUNSETIFF's `flags` ask for what this device offers: one queue
 * and no virtio header. */
static int tap_flags_ok(uint16_t flags)
{
    return !(flags & (TAP_IFF_VNET_HDR | TAP_IFF_MULTI_QUEUE));
}

/** Whether the caller is in the group `gid`, a kernel id, by its effective
 * group or a supplementary one, as Linux's in_egroup_p. */
static int tap_in_group(uint32_t gid)
{
    uint32_t i;

    if (current_proc->egid == gid) {
        return 1;
    }
    for (i = 0; i < current_proc->ngroups; i++) {
        if (current_proc->groups[i] == gid) {
            return 1;
        }
    }
    return 0;
}

/** Whether the caller may not attach `t`, as Linux's tun_not_capable: an
 * owner or group is set that the caller is not, and it lacks the privilege
 * to change the interfaces. */
static int tap_not_capable(const struct tap *t)
{
    return ((t->owner >= 0 && (int64_t)current_proc->euid != t->owner) ||
            (t->group >= 0 && !tap_in_group((uint32_t)t->group))) &&
           !net_admin();
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

/** The first '%' of `s`, or NULL. */
static const char *tap_percent(const char *s)
{
    for (; *s; s++) {
        if (*s == '%') {
            return s;
        }
    }
    return 0;
}

/** The number sscanf reads back, through a template, from the name the
 * template gives the digit `i`: the digit and the digits that follow the
 * template's `%d`, or -1 past two digits. */
static int tap_scan_back(int i, const char *tail)
{
    int v = i;

    for (; *tail >= '0' && *tail <= '9'; tail++) {
        v = v * 10 + (*tail - '0');
        if (v > 99) {
            return -1;
        }
    }
    return v;
}

/**
 * The name TUNSETIFF's `name` asks for, in `out`. An empty name is
 * `dflt`, "tun%d" or "tap%d" as Linux's; a name with a '%' is a template,
 * whose one `%d` takes a digit as Linux's __dev_alloc_name chooses it: a
 * number is taken
 * when the name it gives exists and reads back through the template, with
 * sscanf, as that number, and the first number not taken is the one, or
 * ENFILE when its name exists. A digit after the `%d` is read back with
 * the number, so a00 takes 0 from a%d0 and a01 takes none from a%d1.
 * 0; -EINVAL for a name, or a template once filled in, longer than the
 * kernel's interfaces hold, one dev_valid_name refuses, or a '%' that is
 * not one `%d`; -ENFILE.
 */
static int32_t tap_pick_name(const char *name, const char *dflt, char *out)
{
    const char *pct, *tail;
    uint32_t max = (uint32_t)sizeof(taps->nd.name) - 1;
    int i, head;

    if (name[0] == '\0') {
        name = dflt;
    }
    if (!tap_name_ok(name)) {
        return -EINVAL;
    }
    pct = tap_percent(name);
    if (!pct) {
        if (k_strlen(name) > max) {
            return -EINVAL;
        }
        k_strncpy(out, name, TAP_IFNAME - 1);
        out[TAP_IFNAME - 1] = '\0';
        return 0;
    }
    tail = pct + 2;
    if (pct[1] != 'd' || tap_percent(tail) || k_strlen(name) - 1 > max) {
        return -EINVAL;
    }
    head = (int)(pct - name);
    for (i = 0; i < 10; i++) {
        k_memcpy(out, name, (size_t)head);
        out[head] = (char)('0' + i);
        k_memcpy(out + head + 1, tail, k_strlen(tail) + 1);
        if (!netdev_by_name(out) || tap_scan_back(i, tail) != i) {
            break;
        }
    }
    return i == 10 || netdev_by_name(out) ? -ENFILE : 0;
}

/**
 * TUNSETIFF: give the descriptor `e` the interface the `ifreq` at `arg`
 * names, as Linux's tun_set_iff, and write its name back, with the flags as
 * the caller gave them. An interface of that name is attached when it is a
 * TUN asked for with IFF_TUN or a TAP asked for with IFF_TAP and the caller
 * is its owner, in its group or has the privilege to change the interfaces;
 * else a TUN, or a TAP for IFF_TAP alone, is made, named as
 * `tap_pick_name` says. The attach keeps IFF_NO_PI and IFF_ONE_QUEUE of
 * the call for the interface. -EEXIST for an open that has an interface,
 * as Linux's; -EOPNOTSUPP in a network namespace other than the first,
 * which has no tap; for a name an interface has, -EBUSY under
 * IFF_TUN_EXCL, -EINVAL for an interface of another kind, -EPERM for a
 * caller who may not attach it and -EBUSY for one another descriptor
 * holds; else -EPERM without the privilege to make one and -EINVAL for a
 * call with neither IFF_TUN nor IFF_TAP or a name `tap_pick_name` refuses;
 * -EINVAL for IFF_VNET_HDR or IFF_MULTI_QUEUE, which this device does not
 * offer; else `tap_make`'s error.
 *
 * The request is copied in first, since the copy may sleep and let
 * another call on a descriptor of the same open run; from the checks to
 * the record's interface nothing sleeps. A record and its ring are made
 * only for a request that passed them, so a refused one takes no memory.
 */
static int32_t tap_set_iff(fd_entry_t *e, uintptr_t arg)
{
    uint8_t ifr[TAP_IFR_MAX];
    char name[TAP_IFNAME];
    struct netdev *nd;
    struct tap *t;
    uint16_t flags, type;
    uint32_t irq, size = tap_ifr_size();
    int32_t err = 0;

    k_memset(ifr, 0, sizeof(ifr));
    if (copy_from_user(ifr, (const void __user *)arg, size) != 0) {
        return -EFAULT;
    }
    k_memcpy(&flags, ifr + TAP_IFNAME, 2);
    ifr[TAP_IFNAME - 1] = 0;
    if (tap_file_of(e)) {
        return -EEXIST;
    }
    if (!net_ns_seen()) {
        return -EOPNOTSUPP;
    }
    nd = netdev_by_name((const char *)ifr);
    if (nd) {
        if (flags & TAP_IFF_TUN_EXCL) {
            return -EBUSY;
        }
        t = tap_named((const char *)ifr);
        if (!t || !(flags & t->flags & (TAP_IFF_TUN | TAP_IFF_TAP))) {
            return -EINVAL;
        }
        if (tap_not_capable(t)) {
            return -EPERM;
        }
        if (!tap_flags_ok(flags)) {
            return -EINVAL;
        }
        if (t->attached) {
            return -EBUSY;
        }
    } else {
        if (!net_admin()) {
            return -EPERM;
        }
        type = (flags & TAP_IFF_TUN) ? TAP_IFF_TUN : (flags & TAP_IFF_TAP) ? TAP_IFF_TAP : 0;
        if (!type || !tap_flags_ok(flags)) {
            return -EINVAL;
        }
        err = tap_pick_name((const char *)ifr, type == TAP_IFF_TUN ? "tun%d" : "tap%d", name);
        if (err != 0) {
            return err;
        }
        t = tap_make(name, type, &err);
        if (!t) {
            return err;
        }
    }
    irq = spin_lock_irqsave(&tap_lock);
    t->head = 0;
    t->count = 0;
    t->attached = 1;
    t->flags = (uint16_t)((t->flags & (TAP_IFF_TUN | TAP_IFF_TAP)) | (flags & TAP_KEPT));
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
 * `buf_take`'s, at TAP_PI bytes into it so that `tap_out` can shape it,
 * waiting for one unless the descriptor does not block: 0 with `*tp`,
 * `*bp` and `*np` set, the caller to give the buffer back with
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
        int32_t r;

        flags = spin_lock_irqsave(&tap_lock);
        if (t->count) {
            *np = t->len[t->head];
            k_memcpy(b + TAP_PI, t->frames + t->head * TAP_STRIDE, *np);
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
        r = gone ? -EBADFD : (e->ofd->status_flags & OFD_NONBLOCK) ? -EAGAIN : 0;
        if (r == 0 && wq_wait_event(&t->wq, tap_readable, t) < 0) {
            r = -EINTR;
        }
        if (r != 0) {
            buf_give(t, b);
            return r;
        }
    }
}

/**
 * Read one frame, shaped as `tap_out` says, whole, or its first `count`
 * bytes when the buffer is shorter, as Linux's tun does; errors as
 * `tap_take`, or -EINVAL from `tap_out` or -EFAULT, which lose the frame,
 * as Linux's do.
 */
static int32_t tap_read(fd_entry_t *e, char __user *buf, uint32_t count)
{
    struct tap *t;
    uint8_t *b, *p;
    uint32_t n;
    int32_t r = tap_take(e, &t, &b, &n);

    if (r != 0) {
        return r;
    }
    r = tap_out(t, b, n, count, &p);
    if (r > 0) {
        n = (uint32_t)r < count ? (uint32_t)r : count;
        r = copy_to_user(buf, p, n) != 0 ? -EFAULT : (int32_t)n;
    }
    buf_give(t, b);
    return r;
}

int32_t tap_readv(struct fd_entry *e, const struct kernel_iovec *iov, int count)
{
    struct tap *t;
    uint8_t *b, *p;
    uint32_t n, room = 0, at = 0;
    int32_t r = tap_take(e, &t, &b, &n);
    int i;

    if (r != 0) {
        return r;
    }
    for (i = 0; i < count && room < TAP_BUF; i++) {
        room += iov[i].iov_len < TAP_BUF ? (uint32_t)iov[i].iov_len : TAP_BUF;
    }
    r = tap_out(t, b, n, room, &p);
    if (r < 0) {
        buf_give(t, b);
        return r;
    }
    n = (uint32_t)r;
    r = 0;
    for (i = 0; i < count && at < n; i++) {
        uint32_t len = iov[i].iov_len < n - at ? (uint32_t)iov[i].iov_len : n - at;

        if (len != 0 && copy_to_user((void __user *)iov[i].iov_base, p + at, len) != 0) {
            r = -EFAULT;
            break;
        }
        at += len;
    }
    buf_give(t, b);
    return r != 0 ? r : (int32_t)at;
}

/** The fewest bytes a write of `t` holds that `tap_inject` reads: the
 * packet information without IFF_NO_PI, then a TAP's Ethernet header or a
 * TUN's first byte, whose IP version it judges. */
static uint32_t tap_least(const struct tap *t)
{
    return ((t->flags & TAP_IFF_NO_PI) ? 0u : TAP_PI) +
           ((t->flags & TAP_IFF_TUN) ? 1u : (uint32_t)ETH_HDR_SIZE);
}

/** The most bytes a write of `t` holds that `tap_inject` reads: the packet
 * information without IFF_NO_PI, then a TAP's frame of 1514 bytes or a
 * TUN's datagram of 1500. */
static uint32_t tap_most(const struct tap *t)
{
    return ((t->flags & TAP_IFF_NO_PI) ? 0u : TAP_PI) +
           ((t->flags & TAP_IFF_TUN) ? (uint32_t)ETH_MTU : (uint32_t)TAP_FRAME);
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
    if (total < tap_least(t) || total > tap_most(t)) {
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

const char *tap_name(const struct tap *t)
{
    return t->nd.name;
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
           tap_backend_give(t, t->frames + t->head * TAP_STRIDE, t->len[t->head])) {
        t->head = (t->head + 1) % TAP_RING;
        t->count--;
    }
    spin_unlock_irqrestore(&tap_lock, flags);
}

int32_t tap_inject(struct tap *t, const uint8_t *frame, uint32_t len)
{
    static uint8_t l3[TAP_FRAME]; /* a TUN's frame, under net_lock */
    int pi = !(t->flags & TAP_IFF_NO_PI), tun = (t->flags & TAP_IFF_TUN) != 0;
    uint32_t n = len, flags;
    uint16_t proto = 0;
    int32_t r = (int32_t)len;

    if (pi && len < TAP_PI) {
        return -EINVAL;
    }
    if (pi) {
        n -= TAP_PI;
    }
    if (!tun && n < ETH_HDR_SIZE) {
        return -EINVAL;
    }
    if (frame && pi) {
        proto = (uint16_t)(frame[2] << 8 | frame[3]);
        frame += TAP_PI;
    }
    if (frame && tun && !pi && n != 0 && n <= ETH_MTU) {
        proto = (frame[0] >> 4) == 4 ? ETH_TYPE_IP : (frame[0] >> 4) == 6 ? ETH_TYPE_IPV6 : 0;
    }
    flags = net_lock();
    if (tun && !pi && (n == 0 || (frame && n <= ETH_MTU && proto == 0))) {
        t->nd.rx_dropped++; /* no IP version, as Linux's tun_get_user */
        r = -EINVAL;
    } else if (!frame || n > (tun ? ETH_MTU : TAP_FRAME)) {
        t->nd.rx_dropped++;
    } else if (tun) {
        k_memset(l3, 0, ETH_HDR_SIZE);
        l3[12] = (uint8_t)(proto >> 8);
        l3[13] = (uint8_t)proto;
        k_memcpy(l3 + ETH_HDR_SIZE, frame, n);
        ethernet_rx(&t->nd, l3, (uint16_t)(ETH_HDR_SIZE + n));
    } else {
        ethernet_rx(&t->nd, frame, (uint16_t)n);
    }
    net_unlock(flags);
    return r;
}

/**
 * Write one frame, which the interface receives (`tap_inject`): -EBADFD
 * before TUNSETIFF. A write too short for the interface's form, or longer
 * than it carries, is not copied in: `tap_inject` refuses the first and
 * counts the second as dropped and takes it, so that the writer goes on.
 */
static int32_t tap_write(fd_entry_t *e, const char __user *buf, uint32_t count)
{
    struct tap *t = tap_file_of(e);
    uint8_t *b;
    int32_t r;

    if (!t) {
        return -EBADFD;
    }
    if (count < tap_least(t) || count > tap_most(t)) {
        return tap_inject(t, 0, count);
    }
    b = buf_take(t);
    if (!b) {
        return -ENOMEM;
    }
    if (copy_from_user(b, buf, count) != 0) {
        buf_give(t, b);
        return -EFAULT;
    }
    r = tap_inject(t, b, count);
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
 * The descriptor's control. TUNGETFEATURES answers IFF_TUN, IFF_TAP,
 * IFF_NO_PI and IFF_ONE_QUEUE, so that a caller asks for no virtio header;
 * TUNSETOFFLOAD takes only 0, no offload; TUNSETOWNER and TUNSETGROUP keep
 * the user or group, -EINVAL for an id the caller's namespace does not map,
 * as Linux's; the send buffer and the debug and checksum words are taken
 * and ignored; the virtio header's size and a second queue are -EINVAL.
 * -EBADFD for a request that needs the interface before TUNSETIFF, -ENOTTY
 * for any other request.
 */
static int32_t tap_ioctl(fd_entry_t *e, uint32_t request, uintptr_t arg)
{
    struct tap *t = tap_file_of(e);
    uint8_t ifr[TAP_IFR_MAX];
    uint32_t word;
    uint16_t flags;
    int64_t id;

    switch (request) {
    case TUNGETFEATURES:
        word = TAP_IFF_TUN | TAP_IFF_TAP | TAP_IFF_NO_PI | TAP_IFF_ONE_QUEUE;
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
        flags = (uint16_t)(t->flags | (t->persist ? TAP_IFF_PERSIST : 0));
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
    case TUNSETOWNER:
    case TUNSETGROUP:
        id = (uint32_t)arg == 0xffffffffu ? -1
             : request == TUNSETOWNER     ? uid_named((uint32_t)arg)
                                          : gid_named((uint32_t)arg);
        if (id < 0) {
            return -EINVAL;
        }
        if (request == TUNSETOWNER) {
            t->owner = id;
        } else {
            t->group = id;
        }
        return 0;
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
static struct devfs_node tap_node = {"net/tun", 10, 200, 0666, &tap_file_ops, tap_open, NULL, 0};

void tap_start(void)
{
    int32_t r = devfs_node_register(&tap_node);

    if (r != 0) {
        klog(KLOG_WARN, "tap", "/dev/net/tun not made: error %d", r);
    }
}
