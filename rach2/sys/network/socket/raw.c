/*
 * raw.c: AF_INET and AF_INET6 SOCK_RAW sockets, as raw.h describes them:
 * the list of registered sockets and their receive queues. `sendto` in
 * syscall_net.c makes a raw send.
 */

#include "../../klog.h"
#include "raw.h"
#include "socket.h"
#include "../net.h"
#include "../ip/ip.h"
#include "../ip6/ip6.h"
#include "../../lib/kerrno.h"
#include "../../lib/kstring.h"
#include "../../mm/kheap.h"
#include "../../serial.h"
#include "../../proc/wait_queue.h"

/* What a registration keeps of the datagrams its socket took: whether the
 * queue's budget has dropped one, which is logged once, and what the
 * network layer saw of the datagram most recently taken. The queue itself is
 * the socket's (`sock_dgq`). */
struct raw_last {
    int dropped;
    int reported; /* whether a drop has been logged */
    net_addr_t src;
    uint32_t ifindex; /* ingress, including global addresses */
    struct net_rxinfo info;
};

/* A registered socket: the socket, its registered protocol, whether it is
 * an AF_INET6 one, its ICMPv6 filter, and what it took last, one record
 * from the heap per registration, on a list the receive path walks. */
struct raw_binding {
    struct raw_binding *next;
    struct socket *sock;
    uint8_t proto;
    int v6;
    uint8_t filter6[RAW_FILTER6_LEN]; /* bit t of byte t / 8 blocks type t */
    struct raw_last last;
};

static struct raw_binding *raws;

/** The registration of `sock`, or NULL if it has none. */
static struct raw_binding *binding_of(struct socket *sock)
{
    struct raw_binding *b;

    for (b = raws; b; b = b->next) {
        if (b->sock == sock) {
            return b;
        }
    }
    return NULL;
}

/** What `sock` took last, or NULL if it is not registered. */
static struct raw_last *last_of(struct socket *sock)
{
    struct raw_binding *b = binding_of(sock);

    return b ? &b->last : NULL;
}

int raw_register(struct socket *sock, uint8_t proto)
{
    uint32_t flags;
    struct raw_binding *b;

    flags = net_lock();
    b = binding_of(sock);
    if (b) {
        b->proto = proto;
        net_unlock(flags);
        return 0;
    }
    b = kzalloc(sizeof(*b));
    if (!b) {
        net_unlock(flags);
        return -ENOBUFS;
    }
    b->sock = sock;
    b->proto = proto;
    b->v6 = sock->family == AF_INET6;
    b->next = raws;
    raws = b;
    net_unlock(flags);
    return 0;
}

void raw_unregister(struct socket *sock)
{
    uint32_t flags;
    struct raw_binding **link;

    flags = net_lock();
    for (link = &raws; *link; link = &(*link)->next) {
        if ((*link)->sock == sock) {
            struct raw_binding *b = *link;

            *link = b->next;
            sock_dgq_flush(&sock->rq);
            kfree(b);
            break;
        }
    }
    net_unlock(flags);
}

/**
 * Queue the `len` bytes at `data` on `b`'s socket, whole, with `meta`, and
 * wake its readers. A datagram past the queue's budget is dropped, the
 * newest, keeping the unread ones in order; the first drop is logged.
 */
static void raw_queue(struct raw_binding *b, const struct sock_dgram *meta, const uint8_t *data,
                      uint32_t len)
{
    if (!sock_dgq_put(&b->sock->rq, b->sock->rcvbuf, meta, data, len)) {
        b->last.dropped++;
        if (!b->last.reported) {
            b->last.reported = 1;
            klog(KLOG_WARN, "raw", "rx: the queue is full, dropping datagrams");
        }
        return;
    }
    wq_wake(&b->sock->waiters, WQ_DATA);
}

/** `ip_rx` runs with interrupts off, so the list and the queues are touched
 * without taking `net_lock`. */
void raw_rx(uint8_t proto, const uint8_t *data, uint16_t total_len)
{
    struct sock_dgram meta;
    struct raw_binding *b;

    if (total_len < IP_HDR_SIZE) {
        return;
    }
    k_memset(&meta, 0, sizeof(meta));
    meta.src = net_addr4(((uint32_t)data[12] << 24) | ((uint32_t)data[13] << 16) |
                         ((uint32_t)data[14] << 8) | (uint32_t)data[15]);
    meta.info.dst = net_addr4(((uint32_t)data[16] << 24) | ((uint32_t)data[17] << 16) |
                              ((uint32_t)data[18] << 8) | (uint32_t)data[19]);
    meta.info.hlim = (int16_t)data[8];
    meta.info.tclass = (int16_t)data[1];

    for (b = raws; b; b = b->next) {
        if (!b->v6 && b->proto == proto) {
            raw_queue(b, &meta, data, total_len);
        }
    }
}

/** Whether `b`'s ICMPv6 filter blocks the message of `len` bytes at `msg`:
 * a set bit for its type, or no type at all. Other protocols pass. */
static int raw_filtered6(const struct raw_binding *b, const uint8_t *msg, uint16_t len)
{
    if (b->proto != IP6_NH_ICMP6) {
        return 0;
    }
    return len < 1 || (b->filter6[msg[0] >> 3] & (1u << (msg[0] & 7))) != 0;
}

/** As `raw_rx`, from `ip6_rx`; what is queued is the message alone, as an
 * AF_INET6 raw socket receives it on Linux. The filter is judged before the
 * message is queued, so that a message blocked costs no room and no drop. */
void raw6_rx(int ifindex, const uint8_t *ip6h, uint8_t nh, const uint8_t *msg, uint16_t len)
{
    struct sock_dgram meta;
    struct raw_binding *b;

    k_memset(&meta, 0, sizeof(meta));
    meta.src = net_addr6(ip6h + 8, (uint32_t)ifindex);
    meta.info.dst = net_addr6(ip6h + 24, (uint32_t)ifindex);
    meta.info.ifindex = (uint32_t)ifindex;
    meta.info.hlim = (int16_t)ip6h[7];
    meta.info.tclass = (int16_t)(((ip6h[0] & 0x0f) << 4) | (ip6h[1] >> 4));

    for (b = raws; b; b = b->next) {
        if (b->v6 && b->proto == nh && !raw_filtered6(b, msg, len)) {
            raw_queue(b, &meta, msg, len);
        }
    }
}

void raw_consume(struct socket *sock)
{
    struct raw_last *l;
    struct sock_dgram *d;
    uint32_t flags;

    flags = net_lock();
    l = last_of(sock);
    d = l ? sock_dgq_pop(&sock->rq) : NULL;
    if (d) {
        l->src = d->src;
        l->ifindex = d->info.ifindex;
        l->info = d->info;
    }
    net_unlock(flags);
    kfree(d);
}

uint32_t raw_head_src(struct socket *sock)
{
    uint32_t flags, src;

    flags = net_lock();
    src = sock->rq.head ? sock->rq.head->src.v4 : 0u;
    net_unlock(flags);
    return src;
}

uint32_t raw_last_src(struct socket *sock)
{
    struct raw_last *l;
    uint32_t flags, src;

    flags = net_lock();
    l = last_of(sock);
    src = l ? l->src.v4 : 0u;
    net_unlock(flags);
    return src;
}

int raw_head_src6(struct socket *sock, uint8_t *src)
{
    uint32_t flags;
    int ifindex = -1;

    flags = net_lock();
    if (sock->rq.head) {
        net_addr_to6(sock->rq.head->src, src);
        ifindex = (int)sock->rq.head->info.ifindex;
    }
    net_unlock(flags);
    return ifindex;
}

int raw_last_src6(struct socket *sock, uint8_t *src)
{
    struct raw_last *l;
    uint32_t flags;
    int ifindex = -1;

    flags = net_lock();
    l = last_of(sock);
    if (l) {
        if (l->src.family == AF_INET6)
            net_addr_to6(l->src, src);
        else
            k_memset(src, 0, 16);
        ifindex = (int)l->ifindex;
    }
    net_unlock(flags);
    return ifindex;
}

/** Copy the canonical address of the oldest message, or unspecified. */
void raw_head_address(struct socket *sock, net_addr_t *src)
{
    *src = sock->rq.head ? sock->rq.head->src : net_addr4(0);
}

/** Copy the canonical address of the message most recently consumed. */
void raw_last_address(struct socket *sock, net_addr_t *src)
{
    struct raw_last *l = last_of(sock);
    *src = l ? l->src : net_addr4(0);
}

int raw_set_filter6(struct socket *sock, const uint8_t *filter, uint32_t len)
{
    struct raw_binding *b;
    uint32_t flags = net_lock();

    b = binding_of(sock);
    if (b) {
        k_memcpy(b->filter6, filter, len < RAW_FILTER6_LEN ? len : RAW_FILTER6_LEN);
    }
    net_unlock(flags);
    return b ? 0 : -1;
}

int raw_get_filter6(struct socket *sock, uint8_t *filter)
{
    struct raw_binding *b;
    uint32_t flags = net_lock();

    b = binding_of(sock);
    if (b) {
        k_memcpy(filter, b->filter6, RAW_FILTER6_LEN);
    }
    net_unlock(flags);
    return b ? 0 : -1;
}

void raw_head_info(struct socket *sock, struct net_rxinfo *info)
{
    if (sock->rq.head) {
        *info = sock->rq.head->info;
        return;
    }
    k_memset(info, 0, sizeof(*info));
    info->dst = net_addr4(0);
    info->hlim = -1;
    info->tclass = -1;
}

void raw_last_info(struct socket *sock, struct net_rxinfo *info)
{
    struct raw_last *l = last_of(sock);

    if (l) {
        *info = l->info;
        return;
    }
    raw_head_info(sock, info);
}
