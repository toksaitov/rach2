/*
 * packet.c: AF_PACKET sockets, as packet.h describes them: the list of bound
 * sockets with their receive queues, and the send.
 */

#include "../../klog.h"
#include "packet.h"
#include "socket.h"
#include "../net.h"
#include "../ethernet/ethernet.h"
#include "../driver/netdev.h"
#include "../../lib/kerrno.h"
#include "../../lib/kstring.h"
#include "../../mm/kheap.h"
#include "../../serial.h"
#include "../../proc/wait_queue.h"

/* What a binding keeps beside its socket's queue (`sock_dgq`): whether the
   budget has dropped a frame, which is logged once, and the link address of
   the datagram most recently taken or peeked at, since a receive reports
   the sender after the datagram it belongs to has gone; `have_last` is 0
   until there is one. */
struct packet_last {
    int dropped;
    int reported;
    uint8_t last_src[6];
    uint8_t last_pkttype;
    uint16_t last_proto;
    int last_ifindex;
    int have_last;
};

/* A bound socket and what it took last, one record from the heap per
   binding, on a list the receive path walks. */
struct packet_binding {
    struct packet_binding *next;
    struct socket *sock;
    /* The interface this socket is bound to, or 0 for every one of them. A
       machine with two cards is on two networks, and a client watching for a
       lease on one must not be handed the other's traffic. */
    int ifindex;
    struct packet_last q;
};

static struct packet_binding *packets;

/** The binding of `sock`, or NULL if it has none. */
static struct packet_binding *binding_of(struct socket *sock)
{
    struct packet_binding *b;

    for (b = packets; b; b = b->next) {
        if (b->sock == sock) {
            return b;
        }
    }
    return NULL;
}

/** What the binding of `sock` keeps, or NULL if it is not bound. */
static struct packet_last *queue_of(struct socket *sock)
{
    struct packet_binding *b = binding_of(sock);

    return b ? &b->q : NULL;
}

int packet_ifindex(struct socket *sock)
{
    uint32_t flags = net_lock();
    struct packet_binding *b = binding_of(sock);
    int index = b ? b->ifindex : 0;

    net_unlock(flags);
    return index;
}

int packet_bind(struct socket *sock, int ifindex)
{
    uint32_t flags;
    struct packet_binding *b;

    if (ifindex != 0 && (!net_ns_seen() || !netdev_by_index(ifindex))) {
        return -ENODEV;
    }

    flags = net_lock();
    b = binding_of(sock);
    if (b) {
        if (b->ifindex != ifindex) {
            sock_dgq_flush(&sock->rq);
            b->q.have_last = 0;
        }
        b->ifindex = ifindex;
        net_unlock(flags);
        return 0;
    }
    b = kzalloc(sizeof(*b));
    if (!b) {
        net_unlock(flags);
        return -ENOBUFS;
    }
    b->sock = sock;
    b->ifindex = ifindex;
    b->next = packets;
    packets = b;
    net_unlock(flags);
    return 0;
}

void packet_unbind(struct socket *sock)
{
    uint32_t flags;
    struct packet_binding **link;

    flags = net_lock();
    for (link = &packets; *link; link = &(*link)->next) {
        if ((*link)->sock == sock) {
            struct packet_binding *b = *link;

            *link = b->next;
            sock_dgq_flush(&sock->rq);
            kfree(b);
            break;
        }
    }
    net_unlock(flags);
}

void packet_dev_gone(int ifindex)
{
    struct packet_binding *b;
    uint32_t flags = net_lock();

    for (b = packets; b; b = b->next) {
        if (ifindex > 0 && b->ifindex == ifindex) {
            b->ifindex = -1;
            b->sock->so_error = ENETDOWN;
            wq_wake(&b->sock->waiters, WQ_DATA);
        }
    }
    net_unlock(flags);
}

int packet_snapshot(struct packet_row **rows)
{
    const struct packet_binding *b;
    struct packet_row *out;
    uint32_t flags;
    int n, room;

    *rows = NULL;
    if (!net_ns_seen()) {
        return 0;
    }
    room = 0;
    flags = net_lock();
    for (b = packets; b; b = b->next) {
        room++;
    }
    net_unlock(flags);
    if (room == 0) {
        return 0;
    }
    out = kmalloc(sizeof(*out) * (size_t)room);
    if (!out) {
        return -1;
    }
    n = 0;
    flags = net_lock();
    for (b = packets; b && n < room; b = b->next) {
        out[n].refs = b->sock->refcount;
        out[n].protocol = b->sock->protocol;
        out[n].ifindex = b->ifindex;
        out[n].rmem = b->sock->rq.bytes;
        out[n].uid = b->sock->uid;
        out[n].inode = b->sock->inode;
        n++;
    }
    net_unlock(flags);
    *rows = out;
    return n;
}

/**
 * Which of PACKET_HOST/BROADCAST/MULTICAST/OTHERHOST a destination MAC is.
 *
 * Addressed to this machine means addressed to the card it arrived on. Two
 * cards have two station addresses, and judging both against the first card's
 * reports everything the second one is sent as somebody else's.
 */
static uint8_t packet_type_of(const struct netdev *nd, const uint8_t *dst)
{
    const uint8_t *mine = nd ? nd->mac : net_mac;
    int i;

    for (i = 0; i < 6; i++) {
        if (dst[i] != 0xff) {
            break;
        }
    }
    if (i == 6) {
        return PACKET_BROADCAST;
    }
    for (i = 0; i < 6; i++) {
        if (dst[i] != mine[i]) {
            break;
        }
    }
    if (i == 6) {
        return PACKET_HOST;
    }
    return (dst[0] & 1) ? PACKET_MULTICAST : PACKET_OTHERHOST;
}

void packet_flush(struct socket *sock)
{
    struct packet_last *q;
    uint32_t flags;

    flags = net_lock();
    q = queue_of(sock);
    if (q) {
        sock_dgq_flush(&sock->rq);
        q->have_last = 0;
    }
    net_unlock(flags);
}

/** The receive path runs with interrupts off, so the list and the queues
 * are touched without taking `net_lock`. A frame past the queue's budget is
 * dropped, the newest, keeping the unread ones in order. */
void packet_rx(struct netdev *nd, uint16_t ethertype, const uint8_t *frame, uint16_t frame_len)
{
    struct sock_dgram meta;
    struct packet_binding *b;
    int arrived_on = nd ? nd->index : 0;

    if (frame_len < ETH_HDR_SIZE) {
        return;
    }
    k_memset(&meta, 0, sizeof(meta));
    meta.src = net_addr4(0);
    meta.info.dst = net_addr4(0);
    meta.info.ifindex = (uint32_t)arrived_on;
    meta.info.hlim = -1;
    meta.info.tclass = -1;
    k_memcpy(meta.lladdr, frame + 6, 6);
    meta.pkttype = packet_type_of(nd, frame);
    meta.llproto = ethertype;

    for (b = packets; b; b = b->next) {
        if (b->sock->protocol != ethertype) {
            continue;
        }
        if (b->ifindex != 0 && b->ifindex != arrived_on) {
            continue;
        }
        if (!sock_dgq_put(&b->sock->rq, b->sock->rcvbuf, &meta, frame + ETH_HDR_SIZE,
                          (uint32_t)(frame_len - ETH_HDR_SIZE))) {
            b->q.dropped++;
            if (!b->q.reported) {
                b->q.reported = 1;
                klog(KLOG_WARN, "packet",
                     "rx: the queue is full, dropping frames on a bound socket");
            }
            continue;
        }
        wq_wake(&b->sock->waiters, WQ_DATA);
    }
}

/** Keep the datagram `d`'s address as the one `packet_last_addr` reports;
 * the caller holds `net_lock`. */
static void keep_addr(struct packet_last *q, const struct sock_dgram *d)
{
    k_memcpy(q->last_src, d->lladdr, 6);
    q->last_pkttype = d->pkttype;
    q->last_proto = d->llproto;
    q->last_ifindex = d->info.ifindex;
    q->have_last = 1;
}

void packet_consume(struct socket *sock)
{
    struct packet_last *q;
    struct sock_dgram *d = NULL;
    uint32_t flags;

    flags = net_lock();
    q = queue_of(sock);
    if (q) {
        d = sock_dgq_pop(&sock->rq);
        if (d) {
            keep_addr(q, d);
            sock->rx_last = d->info;
        }
    }
    net_unlock(flags);
    kfree(d);
}

void packet_note_head(struct socket *sock)
{
    struct packet_last *q;
    uint32_t flags;

    flags = net_lock();
    q = queue_of(sock);
    if (q && sock->rq.head) {
        keep_addr(q, sock->rq.head);
        sock->rx_last = sock->rq.head->info;
    }
    net_unlock(flags);
}

int packet_last_addr(struct socket *sock, uint8_t *sll)
{
    struct packet_last *q;
    struct netdev *nd;
    uint32_t flags;
    int i;

    flags = net_lock();
    q = queue_of(sock);
    if (!q || !q->have_last) {
        net_unlock(flags);
        return 0;
    }
    for (i = 0; i < PACKET_SLL_LEN; i++) {
        sll[i] = 0;
    }
    sll[0] = (uint8_t)(AF_PACKET & 0xff);
    sll[1] = (uint8_t)((AF_PACKET >> 8) & 0xff);
    sll[2] = (uint8_t)((q->last_proto >> 8) & 0xff); /* network order */
    sll[3] = (uint8_t)(q->last_proto & 0xff);
    sll[4] = (uint8_t)(q->last_ifindex & 0xff); /* sll_ifindex, an int in host order */
    sll[5] = (uint8_t)((q->last_ifindex >> 8) & 0xff);
    sll[6] = (uint8_t)((q->last_ifindex >> 16) & 0xff);
    sll[7] = (uint8_t)((q->last_ifindex >> 24) & 0xff);
    nd = netdev_by_index(q->last_ifindex);
    sll[8] = (uint8_t)PACKET_HATYPE_ETHER;
    sll[10] = q->last_pkttype;
    sll[11] = 6;
    if (nd && nd->l3) {
        sll[8] = (uint8_t)(NETDEV_NONE_TYPE & 0xff); /* sll_hatype, in host order */
        sll[9] = (uint8_t)(NETDEV_NONE_TYPE >> 8);
        sll[11] = 0;
    }
    for (i = 0; i < sll[11]; i++) {
        sll[12 + i] = q->last_src[i];
    }
    net_unlock(flags);
    return PACKET_SLL_LEN;
}

int packet_send(struct socket *sock, int ifindex, const uint8_t *dst_mac, uint16_t ethertype,
                const uint8_t *data, uint16_t len)
{
    uint32_t flags;
    struct packet_binding *b;
    struct netdev *nd;

    if (len == 0 || len > ETH_MTU) {
        return -EMSGSIZE;
    }
    if (ethertype != ETH_TYPE_IP && ethertype != ETH_TYPE_ARP && ethertype != ETH_TYPE_IPV6 &&
        ethertype != ETH_TYPE_PAE) {
        return -EINVAL;
    }
    if (ifindex != 0 && (!net_ns_seen() || !netdev_by_index(ifindex))) {
        return -ENODEV;
    }
    flags = net_lock();
    b = binding_of(sock);
    if (ifindex == 0 && b) {
        ifindex = b->ifindex;
    }
    nd = ifindex != 0 ? netdev_by_index(ifindex) : netdev_first();
    if (!nd) {
        net_unlock(flags);
        return -ENETDOWN;
    }
    if (len > nd->mtu) {
        net_unlock(flags);
        return -EMSGSIZE;
    }
    if (ethernet_tx(nd, dst_mac, ethertype, data, len) != NETDEV_TX_OK) {
        net_unlock(flags);
        return -ENETDOWN;
    }
    net_unlock(flags);
    return (int)len;
}
