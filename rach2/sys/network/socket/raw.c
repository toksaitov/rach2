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

/* Policy: each socket's queue is eight datagrams of one MTU, twelve
 * kilobytes from the heap for the life of the registration, enough to hold
 * a reply behind a burst of other people's datagrams while the reader is
 * being scheduled. */
#define RAW_QUEUE_SLOTS 8
#define RAW_SLOT_SIZE   1500

/* Each entry has a canonical source and its receiving interface;
 * `last_` describes the entry most recently taken. */
struct raw_queue {
    uint8_t buf[RAW_QUEUE_SLOTS][RAW_SLOT_SIZE];
    uint16_t len[RAW_QUEUE_SLOTS];
    net_addr_t src[RAW_QUEUE_SLOTS];
    uint32_t ifindex[RAW_QUEUE_SLOTS]; /* ingress, including global addresses */
    /* The rest of what the network layer saw, for recvmsg's ancillary
     * data: the ingress is `ifindex` above. */
    struct net_rxinfo info[RAW_QUEUE_SLOTS];
    int head;  /* the next datagram to be read */
    int count; /* how many are waiting */
    int dropped;
    int reported; /* whether a drop has been logged */
    net_addr_t last_src;
    uint32_t last_ifindex;
    struct net_rxinfo last_info;
};

/* A registered socket: the socket, its registered protocol, whether it is
 * an AF_INET6 one, its ICMPv6 filter, and its queue, one record from the
 * heap per registration, on a list the receive path walks. */
struct raw_binding {
    struct raw_binding *next;
    struct socket *sock;
    uint8_t proto;
    int v6;
    uint8_t filter6[RAW_FILTER6_LEN]; /* bit t of byte t / 8 blocks type t */
    struct raw_queue q;
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

/** The queue belonging to `sock`, or NULL if it is not registered. */
static struct raw_queue *queue_of(struct socket *sock)
{
    struct raw_binding *b = binding_of(sock);

    return b ? &b->q : NULL;
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
            kfree(b);
            break;
        }
    }
    net_unlock(flags);
}

/**
 * The slot of `b`'s queue the next datagram goes in, or -1 when the queue
 * is full, which drops the newest and keeps the unread ones in order; the
 * first drop is logged.
 */
static int raw_slot(struct raw_binding *b)
{
    struct raw_queue *q = &b->q;

    if (q->count == RAW_QUEUE_SLOTS) {
        q->dropped++;
        if (!q->reported) {
            q->reported = 1;
            klog(KLOG_WARN, "raw", "rx: the queue is full, dropping datagrams");
        }
        return -1;
    }
    return (q->head + q->count) % RAW_QUEUE_SLOTS;
}

/** `ip_rx` runs with interrupts off, so the list and the queues are touched
 * without taking `net_lock`. */
void raw_rx(uint8_t proto, const uint8_t *data, uint16_t total_len)
{
    uint16_t copy_len = total_len > RAW_SLOT_SIZE ? RAW_SLOT_SIZE : total_len;
    uint32_t src;
    struct raw_binding *b;

    if (total_len < IP_HDR_SIZE) {
        return;
    }
    src = ((uint32_t)data[12] << 24) | ((uint32_t)data[13] << 16) | ((uint32_t)data[14] << 8) |
          (uint32_t)data[15];

    for (b = raws; b; b = b->next) {
        struct raw_queue *q = &b->q;
        int slot;

        if (b->v6 || b->proto != proto || (slot = raw_slot(b)) < 0) {
            continue;
        }
        k_memcpy(q->buf[slot], data, copy_len);
        q->len[slot] = copy_len;
        q->src[slot] = net_addr4(src);
        q->ifindex[slot] = 0;
        q->info[slot].dst = net_addr4(((uint32_t)data[16] << 24) | ((uint32_t)data[17] << 16) |
                                      ((uint32_t)data[18] << 8) | (uint32_t)data[19]);
        q->info[slot].ifindex = 0;
        q->info[slot].hlim = (int16_t)data[8];
        q->info[slot].tclass = (int16_t)data[1];
        q->count++;
        wq_wake(&b->sock->waiters, WQ_DATA);
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
 * AF_INET6 raw socket receives it on Linux. The filter is judged before a
 * slot is taken, so that a message blocked costs no place and no drop. */
void raw6_rx(int ifindex, const uint8_t *ip6h, uint8_t nh, const uint8_t *msg, uint16_t len)
{
    uint16_t copy_len = len > RAW_SLOT_SIZE ? RAW_SLOT_SIZE : len;
    struct raw_binding *b;

    for (b = raws; b; b = b->next) {
        struct raw_queue *q = &b->q;
        int slot;

        if (!b->v6 || b->proto != nh || raw_filtered6(b, msg, len) || (slot = raw_slot(b)) < 0) {
            continue;
        }
        k_memcpy(q->buf[slot], msg, copy_len);
        q->len[slot] = copy_len;
        q->src[slot] = net_addr6(ip6h + 8, (uint32_t)ifindex);
        q->ifindex[slot] = (uint32_t)ifindex;
        q->info[slot].dst = net_addr6(ip6h + 24, (uint32_t)ifindex);
        q->info[slot].ifindex = (uint32_t)ifindex;
        q->info[slot].hlim = (int16_t)ip6h[7];
        q->info[slot].tclass = (int16_t)(((ip6h[0] & 0x0f) << 4) | (ip6h[1] >> 4));
        q->count++;
        wq_wake(&b->sock->waiters, WQ_DATA);
    }
}

int raw_ready(struct socket *sock)
{
    struct raw_queue *q = queue_of(sock);

    return q && q->count > 0;
}

int raw_pending(struct socket *sock)
{
    struct raw_queue *q;
    uint32_t flags;
    int pending;

    flags = net_lock();
    q = queue_of(sock);
    pending = (q && q->count > 0) ? (int)q->len[q->head] : -1;
    net_unlock(flags);
    return pending;
}

int raw_peek(struct socket *sock, uint32_t offset, uint8_t *buf, int len)
{
    struct raw_queue *q;
    uint32_t flags;
    int copy_len;

    if (len < 0) {
        return 0;
    }

    flags = net_lock();
    q = queue_of(sock);
    if (!q || q->count == 0) {
        net_unlock(flags);
        return -1;
    }
    if (offset >= (uint32_t)q->len[q->head]) {
        net_unlock(flags);
        return 0;
    }
    copy_len = (int)((uint32_t)q->len[q->head] - offset);
    if (copy_len > len) {
        copy_len = len;
    }
    k_memcpy(buf, q->buf[q->head] + offset, (uint32_t)copy_len);
    net_unlock(flags);
    return copy_len;
}

void raw_consume(struct socket *sock)
{
    struct raw_queue *q;
    uint32_t flags;

    flags = net_lock();
    q = queue_of(sock);
    if (q && q->count > 0) {
        q->last_src = q->src[q->head];
        q->last_ifindex = q->ifindex[q->head];
        q->last_info = q->info[q->head];
        q->head = (q->head + 1) % RAW_QUEUE_SLOTS;
        q->count--;
    }
    net_unlock(flags);
}

uint32_t raw_head_src(struct socket *sock)
{
    struct raw_queue *q;
    uint32_t flags, src;

    flags = net_lock();
    q = queue_of(sock);
    src = q && q->count > 0 ? q->src[q->head].v4 : 0u;
    net_unlock(flags);
    return src;
}

uint32_t raw_last_src(struct socket *sock)
{
    struct raw_queue *q;
    uint32_t flags, src;

    flags = net_lock();
    q = queue_of(sock);
    src = q ? q->last_src.v4 : 0u;
    net_unlock(flags);
    return src;
}

int raw_head_src6(struct socket *sock, uint8_t *src)
{
    struct raw_queue *q;
    uint32_t flags;
    int ifindex = -1;

    flags = net_lock();
    q = queue_of(sock);
    if (q && q->count > 0) {
        net_addr_to6(q->src[q->head], src);
        ifindex = (int)q->ifindex[q->head];
    }
    net_unlock(flags);
    return ifindex;
}

int raw_last_src6(struct socket *sock, uint8_t *src)
{
    struct raw_queue *q;
    uint32_t flags;
    int ifindex = -1;

    flags = net_lock();
    q = queue_of(sock);
    if (q) {
        if (q->last_src.family == AF_INET6)
            net_addr_to6(q->last_src, src);
        else
            k_memset(src, 0, 16);
        ifindex = (int)q->last_ifindex;
    }
    net_unlock(flags);
    return ifindex;
}

/** Copy the canonical address of the oldest message, or unspecified. */
void raw_head_address(struct socket *sock, net_addr_t *src)
{
    struct raw_queue *q = queue_of(sock);
    *src = q && q->count ? q->src[q->head] : net_addr4(0);
}

/** Copy the canonical address of the message most recently consumed. */
void raw_last_address(struct socket *sock, net_addr_t *src)
{
    struct raw_queue *q = queue_of(sock);
    *src = q ? q->last_src : net_addr4(0);
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
    struct raw_queue *q = queue_of(sock);

    if (q && q->count) {
        *info = q->info[q->head];
        return;
    }
    k_memset(info, 0, sizeof(*info));
    info->dst = net_addr4(0);
    info->hlim = -1;
    info->tclass = -1;
}

void raw_last_info(struct socket *sock, struct net_rxinfo *info)
{
    struct raw_queue *q = queue_of(sock);

    if (q) {
        *info = q->last_info;
        return;
    }
    raw_head_info(sock, info);
}
