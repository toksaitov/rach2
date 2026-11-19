/**
 * Test /dev/net/tun of sys/network/driver/tap.c: TUNSETIFF and the other
 * requests, the ring of frames for the reader, writes, the backend and the
 * close, and TUNSETIFF on a duplicated descriptor while another call's copy
 * sleeps, with the stack around the device stood in for.
 *
 * host-deps: rach2/sys/lib/kstring.c
 * host-deps: rach2/sys/lib/kctypes.c
 * host-deps: tests/host/compat/kheap_host.c tests/host/compat/spinlock_host.c
 * host-deps: tests/host/compat/kirq_none.c
 *
 * The interface's `tx` is called as the stack calls it, and the frames it
 * takes are read back through the descriptor's `read`; a descriptor is an
 * fd_entry_t of the test's with the open's record that the node's `open`
 * gives it. A second descriptor of the same open is a copy of the entry,
 * as dup makes one. User copies are plain copies, since every buffer is the
 * test's.
 *
 * The test exits with the number of failed checks, at most 255.
 */

#include "lib/kint.h"
#include "lib/kerrno.h"
#include "fs/fd.h"
#include "fs/file.h"
#include "fs/devfs.h"
#include "io/poll.h"
#include "network/driver/netdev.h"
#include "network/driver/tap.h"
#include "network/driver/tap.c"

extern int printf(const char *fmt, ...);

static int fails;

#define CHECK(cond, what)                   \
    do {                                    \
        if (!(cond)) {                      \
            fails++;                        \
            printf("  FAIL: %s\n", (what)); \
        }                                   \
    } while (0)

/** 0 when the strings `a` and `b` are equal. */
static int same_str(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a != *b;
}

/* ------------------------------------------------------------------ */
/* The kernel around the device.                                       */
/* ------------------------------------------------------------------ */

/* What the device did to the kernel around it, which the rows read. */
static struct {
    struct devfs_node *node;   /* the node registered */
    int admin;                 /* whether the caller may change interfaces */
    int other_ns;              /* the caller is in a network namespace not the first */
    int ip6_forgets;           /* interfaces whose IPv6 state was forgotten */
    void (*during_copy)(void); /* run once inside the next copy from the user */
    void (*during_wait)(void); /* run once inside the next wait, before a signal */
    int register_error;        /* what netdev_register answers, when set */
    struct netdev *iface[4];   /* the interfaces registered */
    int registered, unregistered;
    int rx, rx_len; /* frames given to ethernet_rx, and the last one's length */
    int wakes, carrier, set_ups, last_up, locks;
} k;

/** Keep the node, so that a row opens it. */
int32_t devfs_node_register(struct devfs_node *node)
{
    k.node = node;
    return 0;
}

/** Whether the caller has CAP_NET_ADMIN, as the row set it. */
int net_admin(void)
{
    return k.admin;
}

/** The interface named `name` among those registered, or NULL. */
struct netdev *netdev_by_name(const char *name)
{
    int i;

    for (i = 0; i < 4; i++) {
        if (k.iface[i] && same_str(k.iface[i]->name, name) == 0) {
            return k.iface[i];
        }
    }
    return 0;
}

/** Register `nd` in a free slot: 0, or -ENOSPC. */
int netdev_register(struct netdev *nd)
{
    int i;

    if (k.register_error) {
        return k.register_error;
    }
    for (i = 0; i < 4; i++) {
        if (!k.iface[i]) {
            k.iface[i] = nd;
            nd->index = 10 + k.registered++;
            return 0;
        }
    }
    return -ENOSPC;
}

/** Forget `nd`. */
void netdev_unregister(struct netdev *nd)
{
    int i;

    for (i = 0; i < 4; i++) {
        if (k.iface[i] == nd) {
            k.iface[i] = 0;
            k.unregistered++;
        }
    }
}

/** Count a carrier change. */
void netdev_carrier_changed(struct netdev *nd)
{
    (void)nd;
    k.carrier++;
}

/** Count a frame received and keep its length. */
void ethernet_rx(struct netdev *nd, const uint8_t *data, uint16_t len)
{
    (void)nd;
    (void)data;
    k.rx++;
    k.rx_len = len;
}

/** Open a bracket of the stack's lock, counted. */
uint32_t net_lock(void)
{
    k.locks++;
    return 0;
}

/** Close a bracket of the stack's lock, counted. */
void net_unlock(uint32_t flags)
{
    (void)flags;
    k.locks--;
}

/** Record an interface set up or down. */
int netconf_set_up(struct netdev *nd, int up)
{
    (void)nd;
    k.set_ups++;
    k.last_up = up;
    return 0;
}

/** The address forgotten: nothing to do here. */
int netconf_set_address(struct netdev *nd, uint32_t ip, uint32_t mask)
{
    (void)nd;
    (void)ip;
    (void)mask;
    return 0;
}

/** No route was added to a tap. */
int netdev_route_at(int n, struct netdev_rt *out)
{
    (void)n;
    (void)out;
    return 0;
}

/** Not reached: no route is listed. */
int netdev_route_del(struct netdev *nd, uint32_t dst, uint32_t mask, uint32_t gw)
{
    (void)nd;
    (void)dst;
    (void)mask;
    (void)gw;
    return 0;
}

/** No neighbour is kept. */
void arp_forget_dev(struct netdev *nd)
{
    (void)nd;
}

/** No neighbour is kept. */
int arp_snapshot(struct arp_row *out, int max)
{
    (void)out;
    (void)max;
    return 0;
}

/** Not reached: no neighbour is listed. */
int arp_forget(struct netdev *nd, uint32_t ip)
{
    (void)nd;
    (void)ip;
    return 0;
}

/** No IPv6 state is kept. */
void nd6_if_down(struct netdev *nd)
{
    (void)nd;
}

/** No IPv6 neighbour is kept. */
int nd6_snapshot(struct nd6_row *out, int max)
{
    (void)out;
    (void)max;
    return 0;
}

/** Not reached: no IPv6 neighbour is listed. */
int nd6_forget(struct netdev *nd, const uint8_t *addr)
{
    (void)nd;
    (void)addr;
    return 0;
}

/** No IPv6 route is kept. */
void route6_forget(struct netdev *nd, int proto)
{
    (void)nd;
    (void)proto;
}

/** Bytes that are not random, which the address's bits are checked on. */
void krandom_bytes(void *buf, uint32_t n)
{
    k_memset(buf, 0xff, n);
}

/** Count a wake. */
void wq_wake(wait_queue_t *wq, uint32_t kinds)
{
    (void)wq;
    (void)kinds;
    k.wakes++;
}

/** Count a wake. */
void wq_wake_all(wait_queue_t *wq)
{
    (void)wq;
    k.wakes++;
}

/** Not reached: every read of the test that would wait does not block. */
int wq_wait_event(wait_queue_t *wq, wq_cond_fn cond, void *arg)
{
    void (*hook)(void) = k.during_wait;

    (void)wq;
    k.during_wait = 0;
    if (hook) {
        hook();
    }
    return cond(arg) ? 0 : -EINTR;
}

/** A plain copy: every buffer is the test's. A hook a row set runs first,
 * once, as another thread runs while a copy sleeps on a fault. */
int copy_from_user(void *dst, const void __user *src, size_t n)
{
    void (*hook)(void) = k.during_copy;

    k.during_copy = 0;
    if (hook) {
        hook();
    }
    k_memcpy(dst, (const void *)src, n);
    return 0;
}

/** Whether the caller is in the first network namespace. */
int net_ns_seen(void)
{
    return !k.other_ns;
}

/** Every call of the test is native. */
int in_compat_syscall(void)
{
    return 0;
}

/** Count an interface whose IPv6 state is forgotten. */
void ip6_forget_dev(struct netdev *nd)
{
    (void)nd;
    k.ip6_forgets++;
}

/** A plain copy: every buffer is the test's. */
int copy_to_user(void __user *dst, const void *src, size_t n)
{
    k_memcpy((void *)dst, src, n);
    return 0;
}

/** Discard a kernel log record. */
void klog(int level, const char *subsystem, const char *format, ...)
{
    (void)level;
    (void)subsystem;
    (void)format;
}

/* ------------------------------------------------------------------ */
/* Descriptors and frames.                                             */
/* ------------------------------------------------------------------ */

/* The descriptors and their descriptions: [0] and [1] share one, as dup
   makes them; [2] and [3] are opens of their own. */
static struct open_file ofd[3];
static fd_entry_t fds[4];

/** Open the device as descriptor `n` on description `d`. */
static int open_tap(int n, int d)
{
    k_memset(&fds[n], 0, sizeof(fds[n]));
    k_memset(&ofd[d], 0, sizeof(ofd[d]));
    fds[n].ops = k.node->ops;
    fds[n].ofd = &ofd[d];
    return k.node->open(k.node, &fds[n], 0);
}

/** TUNSETIFF on descriptor `n` with `name` and `flags`; the name the call
 * gave is left in `out` when it is not NULL. */
static int32_t set_iff(int n, const char *name, uint16_t flags, char *out)
{
    uint8_t ifr[TAP_IFR_MAX];
    int32_t r;

    k_memset(ifr, 0, sizeof(ifr));
    k_strncpy((char *)ifr, name, TAP_IFNAME - 1);
    k_memcpy(ifr + TAP_IFNAME, &flags, 2);
    r = k.node->ops->ioctl(&fds[n], TUNSETIFF, (uintptr_t)ifr);
    if (out) {
        k_memcpy(out, ifr, TAP_IFNAME);
    }
    return r;
}

/** A frame of `len` bytes whose bytes are `seed` upward. */
static uint8_t *frame_of(uint16_t len, uint8_t seed)
{
    static uint8_t f[2048];
    uint16_t i;

    for (i = 0; i < len; i++) {
        f[i] = (uint8_t)(seed + i);
    }
    return f;
}

/* A backend that takes frames while `room` lasts. */
static struct test_backend {
    struct tap_backend b;
    int room, took, gone;
    uint8_t first; /* the first byte of the last frame taken */
} be;

/** Take the frame while there is room. */
static int be_rx(struct tap_backend *b, const uint8_t *frame, uint16_t len)
{
    struct test_backend *t = (struct test_backend *)b;

    (void)len;
    if (t->room == 0) {
        return 0;
    }
    t->room--;
    t->took++;
    t->first = frame[0];
    return 1;
}

/** Count the interface's close. */
static void be_gone(struct tap_backend *b)
{
    ((struct test_backend *)b)->gone++;
}

/* ------------------------------------------------------------------ */
/* The rows.                                                           */
/* ------------------------------------------------------------------ */

/** Before TUNSETIFF: the requests that need no interface, the refusals of
 * TUNSETIFF, and EBADFD for the rest. */
static void check_before(void)
{
    struct wait_queue *wq;
    uint32_t word = 0;
    char buf[64];

    CHECK(open_tap(0, 0) == 0 && fds[0].priv, "an open has its record");
    CHECK(k.node->ops->read(&fds[0], buf, sizeof(buf)) == -EBADFD, "a read before TUNSETIFF");
    CHECK(k.node->ops->write(&fds[0], buf, sizeof(buf)) == -EBADFD, "a write before TUNSETIFF");
    CHECK(k.node->ops->poll(&fds[0], POLLIN, &wq) == POLLERR, "a poll before TUNSETIFF");
    CHECK(k.node->ops->ioctl(&fds[0], TUNGETIFF, (uintptr_t)buf) == -EBADFD,
          "TUNGETIFF before TUNSETIFF");
    CHECK(k.node->ops->ioctl(&fds[0], TUNGETFEATURES, (uintptr_t)&word) == 0 &&
              word == (TAP_IFF_TAP | TAP_IFF_NO_PI),
          "TUNGETFEATURES offers IFF_TAP | IFF_NO_PI");
    CHECK(k.node->ops->ioctl(&fds[0], 0x1234u, 0) == -ENOTTY, "an unknown request");
    CHECK(k.node->ops->ioctl(&fds[0], TUNSETVNETHDRSZ, (uintptr_t)&word) == -EINVAL &&
              k.node->ops->ioctl(&fds[0], TUNSETQUEUE, (uintptr_t)&word) == -EINVAL,
          "no virtio header and no second queue");

    k.admin = 0;
    CHECK(set_iff(0, "", TAP_IFF_TAP | TAP_IFF_NO_PI, 0) == -EPERM, "TUNSETIFF needs NET_ADMIN");
    k.admin = 1;
    CHECK(set_iff(0, "", TAP_IFF_TUN | TAP_IFF_NO_PI, 0) == -EINVAL, "no TUN interface");
    CHECK(set_iff(0, "", TAP_IFF_TAP, 0) == -EINVAL, "no packet information header");
    CHECK(set_iff(0, "", TAP_IFF_TAP | TAP_IFF_NO_PI | TAP_IFF_VNET_HDR, 0) == -EINVAL,
          "no virtio header");
    CHECK(set_iff(0, "", TAP_IFF_TAP | TAP_IFF_NO_PI | TAP_IFF_MULTI_QUEUE, 0) == -EINVAL,
          "one queue");
    CHECK(set_iff(0, "a/b", TAP_IFF_TAP | TAP_IFF_NO_PI, 0) == -EINVAL &&
              set_iff(0, "..", TAP_IFF_TAP | TAP_IFF_NO_PI, 0) == -EINVAL &&
              set_iff(0, "a b", TAP_IFF_TAP | TAP_IFF_NO_PI, 0) == -EINVAL &&
              set_iff(0, "a:%d", TAP_IFF_TAP | TAP_IFF_NO_PI, 0) == -EINVAL,
          "a name dev_valid_name refuses is refused");
    CHECK(set_iff(0, "a%d%d", TAP_IFF_TAP | TAP_IFF_NO_PI, 0) == -EINVAL &&
              set_iff(0, "a%x", TAP_IFF_TAP | TAP_IFF_NO_PI, 0) == -EINVAL,
          "and a template with a second %d or another conversion, as dev_alloc_name");
    k.register_error = -ENOSPC;
    CHECK(set_iff(0, "", TAP_IFF_TAP | TAP_IFF_NO_PI, 0) == -ENOSPC,
          "a registration that fails is the call's error");
    k.register_error = 0;
    k.other_ns = 1;
    CHECK(set_iff(0, "", TAP_IFF_TAP | TAP_IFF_NO_PI, 0) == -EOPNOTSUPP,
          "a network namespace other than the first has no tap");
    k.other_ns = 0;
    CHECK(k.registered == 0 && !taps[0].frames && !taps[1].frames,
          "and no interface or ring is made by a refusal");
}

/** TUNSETIFF names and makes interfaces, and a descriptor of the same open
 * shares the interface. */
static void check_setiff(void)
{
    char name[TAP_IFNAME];
    struct netdev_link link;
    struct tap *t;

    fds[1] = fds[0];
    CHECK(set_iff(0, "", TAP_IFF_TAP | TAP_IFF_NO_PI, name) == 0 && same_str(name, "tap0") == 0,
          "an empty name takes tap0 and is written back");
    t = tap_of(&fds[0]);
    CHECK(t && k.registered == 1 && netdev_by_name("tap0") == &t->nd, "tap0 is registered");
    CHECK((t->nd.mac[0] & 3u) == 2u, "with a locally administered unicast address");
    CHECK(t->nd.mtu == NETDEV_ETH_MTU_MAX && t->nd.pci_key == -1, "an MTU of 1500, no function");
    CHECK(tap_ops.link(t, &link) == 1 && link.up == 1,
          "it has a carrier while a descriptor holds it");
    CHECK(tap_of(&fds[1]) == t,
          "a descriptor duplicated before TUNSETIFF has the interface, as the open's");
    CHECK(set_iff(1, "", TAP_IFF_TAP | TAP_IFF_NO_PI, 0) == -EEXIST,
          "and a second TUNSETIFF on the open is EEXIST, as Linux's");
    {
        uint8_t ifr[TAP_IFR_MAX + 4];

        k_memset(ifr, 0x5a, sizeof(ifr));
        CHECK(k.node->ops->ioctl(&fds[1], TUNGETIFF, (uintptr_t)ifr) == 0 &&
                  same_str((const char *)ifr, "tap0") == 0 &&
                  ifr[TAP_IFNAME] == ((TAP_IFF_TAP | TAP_IFF_NO_PI) & 0xff) &&
                  ifr[TAP_IFNAME + 1] == (TAP_IFF_NO_PI >> 8) && ifr[TAP_IFR_MAX - 1] == 0 &&
                  ifr[TAP_IFR_MAX] == 0x5a,
              "TUNGETIFF writes the name and flags in a whole struct ifreq, and no more");
    }

    CHECK(open_tap(2, 1) == 0, "a second open");
    CHECK(set_iff(2, "tap0", TAP_IFF_TAP | TAP_IFF_NO_PI, 0) == -EBUSY,
          "an interface another descriptor holds is busy");
    CHECK(set_iff(2, "eth0", TAP_IFF_TAP | TAP_IFF_NO_PI, 0) == -EINVAL,
          "the name of an interface that is no tap is refused");
    CHECK(set_iff(2, "eth0", TAP_IFF_TAP | TAP_IFF_NO_PI | TAP_IFF_TUN_EXCL, 0) == -EBUSY,
          "but is busy under IFF_TUN_EXCL, as Linux checks that first");
    CHECK(set_iff(2, "vm%d", TAP_IFF_TAP | TAP_IFF_NO_PI, name) == 0 && same_str(name, "vm0") == 0,
          "a stem with %d takes its first free digit");
    CHECK(open_tap(3, 2) == 0 && set_iff(3, "", TAP_IFF_TAP | TAP_IFF_NO_PI, 0) == -ENOSPC,
          "a third interface finds both records taken");
    CHECK(set_iff(3, ".%d", TAP_IFF_TAP | TAP_IFF_NO_PI, 0) == -ENOSPC &&
              set_iff(3, "%d", TAP_IFF_TAP | TAP_IFF_NO_PI, 0) == -ENOSPC,
          "the templates .%d and %d pass the checks of a name, as dev_valid_name allows them");
}

/** Set O_NONBLOCK on the first open, as another thread's fcntl does. */
static void set_nonblock(void)
{
    ofd[0].status_flags = OFD_NONBLOCK;
}

/** The ring: TAP_RING frames held in order, one more dropped, a read of
 * each, a short buffer, and EAGAIN once it is empty. */
static void check_ring(void)
{
    struct tap *t = tap_of(&fds[0]);
    struct wait_queue *wq;
    uint8_t buf[2048];
    uint32_t i;
    int ok = 1;

    CHECK(k.node->ops->poll(&fds[0], POLLIN | POLLOUT, &wq) == POLLOUT && wq == &t->wq,
          "an empty ring is writable and not readable");
    for (i = 0; i < TAP_RING; i++) {
        ok &= tap_ops.tx(t, frame_of((uint16_t)(60 + i % 1000), (uint8_t)i),
                         (uint16_t)(60 + i % 1000)) == NETDEV_TX_OK;
    }
    CHECK(ok && t->count == TAP_RING, "the ring holds 1024 frames");
    CHECK(tap_ops.tx(t, frame_of(60, 0), 60) == NETDEV_TX_DROPPED, "and drops the next");
    CHECK(tap_ops.tx(t, frame_of(1600, 0), 1515) == NETDEV_TX_DROPPED,
          "a frame longer than 1514 bytes is dropped");
    CHECK(k.node->ops->poll(&fds[0], POLLIN | POLLOUT, &wq) == (POLLIN | POLLOUT),
          "a ring with frames is readable");

    ofd[0].status_flags = OFD_NONBLOCK;
    CHECK(k.node->ops->read(&fds[1], (char *)buf, 10) == 10 && buf[0] == 0 && buf[9] == 9,
          "a short buffer takes the frame's first bytes");
    for (i = 1; i < TAP_RING && ok; i++) {
        uint16_t len = (uint16_t)(60 + i % 1000);

        ok = k.node->ops->read(&fds[0], (char *)buf, sizeof(buf)) == (int32_t)len &&
             buf[0] == (uint8_t)i && buf[len - 1] == (uint8_t)(i + len - 1);
    }
    CHECK(ok, "each read is one frame, whole and in order");
    {
        struct kernel_iovec v[2];
        uint8_t head[14], rest[20];

        tap_ops.tx(t, frame_of(60, 100), 60);
        tap_ops.tx(t, frame_of(70, 200), 70);
        v[0].iov_base = (uintptr_t)head;
        v[0].iov_len = sizeof(head);
        v[1].iov_base = (uintptr_t)rest;
        v[1].iov_len = sizeof(rest);
        CHECK(tap_readv(&fds[0], v, 2) == 34 && head[0] == 100 && head[13] == 113 &&
                  rest[0] == 114 && rest[19] == 133,
              "readv scatters one frame across its vector");
        CHECK(k.node->ops->read(&fds[0], (char *)buf, sizeof(buf)) == 70 && buf[0] == 200,
              "and the next read has the next frame whole");
    }
    CHECK(k.node->ops->read(&fds[0], (char *)buf, sizeof(buf)) == -EAGAIN,
          "an empty ring is EAGAIN without blocking");
    ofd[0].status_flags = 0;
    k.during_wait = set_nonblock;
    CHECK(k.node->ops->read(&fds[0], (char *)buf, sizeof(buf)) == -EINTR,
          "a blocking read a signal ends is EINTR, though O_NONBLOCK was set as it slept");
    ofd[0].status_flags = 0;
}

/** Writes: one frame each, a frame too short refused, one too long
 * counted and taken; writev's gathered frame takes the same path. */
static void check_write(void)
{
    struct tap *t = tap_of(&fds[0]);
    uint64_t dropped = t->nd.rx_dropped;

    CHECK(k.node->ops->write(&fds[0], (const char *)frame_of(60, 1), 60) == 60 && k.rx == 1 &&
              k.rx_len == 60,
          "a write is one frame received");
    CHECK(k.node->ops->write(&fds[0], (const char *)frame_of(13, 1), 13) == -EINVAL && k.rx == 1,
          "a write shorter than an Ethernet header is EINVAL");
    CHECK(k.node->ops->write(&fds[0], (const char *)frame_of(2000, 1), 2000) == 2000 && k.rx == 1 &&
              t->nd.rx_dropped == dropped + 1,
          "a write too long for the interface is taken and counted as dropped");
    {
        struct kernel_iovec v[2];
        uint8_t *f = frame_of(1514, 1);

        v[0].iov_base = (uintptr_t)f;
        v[0].iov_len = 14;
        v[1].iov_base = (uintptr_t)(f + 14);
        v[1].iov_len = 1500;
        CHECK(tap_writev(&fds[1], v, 2, 1514) == 1514 && k.rx == 2 && k.rx_len == 1514,
              "writev gathers a frame of 1514 bytes, which is received");
        v[1].iov_base = 0; /* not read */
        v[1].iov_len = 1501;
        CHECK(tap_writev(&fds[1], v, 2, 1515) == 1515 && k.rx == 2 &&
                  t->nd.rx_dropped == dropped + 2,
              "writev of 1515 bytes is taken and counted as dropped, unread, as write's");
        v[0].iov_len = 13;
        CHECK(tap_writev(&fds[1], v, 1, 13) == -EINVAL, "writev shorter than a header is EINVAL");
    }
    CHECK(k.locks == 0, "every bracket of the stack's lock is closed");
}

/** The backend takes frames in place of the reader, only when the ring is
 * empty so that order holds, and the ring drains to it. */
static void check_backend(void)
{
    struct tap *t = tap_of(&fds[0]);
    struct test_backend other;

    be.b.rx = be_rx;
    be.b.gone = be_gone;
    other = be;
    CHECK(tap_backend_set(t, &be.b) == 0, "a backend is set");
    CHECK(tap_backend_set(t, &other.b) == -EBUSY, "a second is busy");
    be.room = 1;
    CHECK(tap_ops.tx(t, frame_of(60, 7), 60) == NETDEV_TX_OK && be.took == 1 && t->count == 0,
          "a frame goes to the backend");
    CHECK(tap_ops.tx(t, frame_of(60, 8), 60) == NETDEV_TX_OK && be.took == 1 && t->count == 1,
          "one it has no room for waits in the ring");
    be.room = 5;
    CHECK(tap_ops.tx(t, frame_of(60, 9), 60) == NETDEV_TX_OK && be.took == 1 && t->count == 2,
          "and the next waits behind it, so that order holds");
    tap_backend_drain(t);
    CHECK(be.took == 3 && be.first == 9 && t->count == 0, "a drain gives the ring to the backend");
    CHECK(tap_inject(t, frame_of(60, 1), 60) == 60 && k.rx == 3, "the backend injects a frame");
}

/** The close: the backend let go, the interface removed, its record free
 * for the next TUNSETIFF; TUNSETPERSIST keeps it. */
static void check_close(void)
{
    struct tap *t = tap_of(&fds[2]);
    struct netdev_link link;
    char name[TAP_IFNAME];

    CHECK(k.node->ops->ioctl(&fds[2], TUNSETPERSIST, 1) == 0, "vm0 is made persistent");
    k.node->ops->close(&fds[2]);
    CHECK(open_tap(2, 1) == 0 &&
              set_iff(2, "vm0", TAP_IFF_TAP | TAP_IFF_NO_PI | TAP_IFF_TUN_EXCL, 0) == -EBUSY,
          "IFF_TUN_EXCL refuses an interface that exists");
    k.node->ops->close(&fds[2]);
    CHECK(netdev_by_name("vm0") == &t->nd && tap_ops.link(t, &link) == 1 && link.up == 0,
          "a persistent interface stays, without a carrier, after its close");
    CHECK(tap_ops.tx(t, frame_of(60, 0), 60) == NETDEV_TX_DROPPED,
          "and drops what is sent with no reader");
    CHECK(open_tap(2, 1) == 0 && set_iff(2, "vm0", TAP_IFF_TAP | TAP_IFF_NO_PI, name) == 0 &&
              tap_of(&fds[2]) == t,
          "a later TUNSETIFF of its name takes it again");
    CHECK(k.node->ops->ioctl(&fds[2], TUNSETPERSIST, 0) == 0, "and it is made transient");
    k.node->ops->close(&fds[2]);
    CHECK(netdev_by_name("vm0") == 0, "a transient interface is removed at its close");

    t = tap_of(&fds[0]);
    k.node->ops->close(&fds[0]);
    CHECK(be.gone == 1 && t->backend == 0, "the close lets the backend go");
    CHECK(netdev_by_name("tap0") == 0 && k.unregistered == 2 && k.set_ups >= 2 && k.last_up == 0,
          "tap0 is set down and removed");
    CHECK(set_iff(3, "", TAP_IFF_TAP | TAP_IFF_NO_PI, name) == 0 && same_str(name, "tap0") == 0,
          "and its record serves the next TUNSETIFF");
    k.node->ops->close(&fds[3]);
    CHECK(k.ip6_forgets == k.unregistered, "each interface removed has its IPv6 state forgotten");
    CHECK(k.locks == 0, "every bracket of the stack's lock is closed");
}

/** A TUNSETIFF on the duplicate of a descriptor, run while the first call's
 * copy of its request sleeps. */
static void race_setiff(void)
{
    CHECK(set_iff(1, "race%d", TAP_IFF_TAP | TAP_IFF_NO_PI, 0) == 0, "the second call attaches");
}

/** Two TUNSETIFF calls on one open, the second run while the first's copy
 * sleeps: the first finds the open attached and changes nothing, so that
 * no interface is left without a descriptor. */
static void check_race(void)
{
    char name[TAP_IFNAME];
    int registered = k.registered;

    CHECK(open_tap(0, 0) == 0, "an open for the race");
    fds[1] = fds[0];
    k.during_copy = race_setiff;
    CHECK(set_iff(0, "", TAP_IFF_TAP | TAP_IFF_NO_PI, 0) == -EEXIST,
          "the first call finds the open attached after its copy");
    CHECK(tap_of(&fds[0]) && same_str(tap_of(&fds[0])->nd.name, "race0") == 0 &&
              k.registered == registered + 1,
          "and the open keeps the second call's interface alone");
    k.node->ops->close(&fds[0]);
    CHECK(netdev_by_name("race0") == 0, "which the close removes");
    CHECK(open_tap(2, 1) == 0 && set_iff(2, "", TAP_IFF_TAP | TAP_IFF_NO_PI, name) == 0 &&
              open_tap(3, 2) == 0 && set_iff(3, "", TAP_IFF_TAP | TAP_IFF_NO_PI, 0) == 0,
          "and both records serve again");
    k.node->ops->close(&fds[2]);
    k.node->ops->close(&fds[3]);
    CHECK(open_tap(3, 2) == 0 && set_iff(3, "a%db", TAP_IFF_TAP | TAP_IFF_NO_PI, name) == 0 &&
              same_str(name, "a0b") == 0,
          "a template's %d may have text after it, as dev_alloc_name fills it in");
    k.node->ops->close(&fds[3]);
}

/** Run the rows; return the number of failed checks, at most 255. */
int main(void)
{
    static struct netdev eth0;

    k_strncpy(eth0.name, "eth0", sizeof(eth0.name) - 1);
    k.iface[3] = &eth0;
    k.admin = 1;
    tap_start();
    CHECK(k.node && same_str(k.node->name, "net/tun") == 0 && k.node->major == 10 &&
              k.node->minor == 200,
          "the node is /dev/net/tun, 10:200");
    check_before();
    check_setiff();
    check_ring();
    check_write();
    check_backend();
    check_close();
    check_race();
    if (!fails) {
        printf("tap-hosttest: result: pass\n");
    }
    return fails > 255 ? 255 : fails;
}
