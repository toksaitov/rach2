/*
 * packet.c: the socket that sees Ethernet, for a DHCP client and `arping`.
 *
 * The contract and the reasoning are in packet.h. What is here is the table of
 * bound sockets and the two directions.
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

/* Policy: each socket's queue is eight datagrams of one MTU, twelve
   kilobytes from the heap for the life of the binding, enough to hold a
   reply behind a burst of other people's frames while the client is being
   scheduled. */
#define PACKET_QUEUE_SLOTS 8
#define PACKET_SLOT_SIZE   1500

struct packet_queue {
    uint8_t buf[PACKET_QUEUE_SLOTS][PACKET_SLOT_SIZE];
    uint16_t len[PACKET_QUEUE_SLOTS];
    /* The link address each datagram came from, kept beside it: the sender's
       MAC, and whether the frame was addressed to this machine, to everyone,
       or to a group. A reader is given this with the datagram, which is what
       `arping` decides on. */
    uint8_t src[PACKET_QUEUE_SLOTS][6];
    uint8_t pkttype[PACKET_QUEUE_SLOTS];
    uint16_t proto[PACKET_QUEUE_SLOTS];
    /* The interface each datagram arrived on. A socket bound to no particular
       one is offered them all, and a reader is told which. */
    uint8_t ifindex[PACKET_QUEUE_SLOTS];
    int head;  /* the next datagram to be read */
    int count; /* how many are waiting */
    int dropped;
    int reported;
    /* The address of the datagram most recently taken, since a receive reports
       the sender after the datagram it belongs to has gone. */
    uint8_t last_src[6];
    uint8_t last_pkttype;
    uint16_t last_proto;
    uint8_t last_ifindex;
    int have_last;
};

/* A bound socket and its queue, one record from the heap per binding, on
   a list the receive path walks. */
struct packet_binding {
    struct packet_binding *next;
    struct socket *sock;
    /* The interface this socket is bound to, or 0 for every one of them. A
       machine with two cards is on two networks, and a client watching for a
       lease on one must not be handed the other's traffic. */
    int ifindex;
    struct packet_queue q;
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

/** The queue belonging to `sock`, or NULL if it is not bound. */
static struct packet_queue *queue_of(struct socket *sock)
{
    struct packet_binding *b = binding_of(sock);

    return b ? &b->q : NULL;
}

/** Return the bound interface index, or zero for none or every interface. */
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

    /* 0 is every interface; any other index must be one of this machine's. */
    if (ifindex != 0 && (ifindex < 1 || ifindex > netdev_count())) {
        return -ENODEV;
    }

    flags = net_lock();
    b = binding_of(sock);
    if (b) {
        /* A move to another interface drops what the old one queued. */
        if (b->ifindex != ifindex) {
            b->q.count = 0;
            b->q.head = 0;
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
            kfree(b);
            break;
        }
    }
    net_unlock(flags);
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
    struct packet_queue *q;
    uint32_t flags;

    flags = net_lock();
    q = queue_of(sock);
    if (q) {
        q->count = 0;
        q->head = 0;
        q->have_last = 0;
    }
    net_unlock(flags);
}

void packet_rx(struct netdev *nd, uint16_t ethertype, const uint8_t *frame, uint16_t frame_len)
{
    const uint8_t *payload;
    uint16_t len, copy_len;
    struct packet_binding *b;
    int arrived_on = nd ? nd->index : 0;

    if (frame_len < ETH_HDR_SIZE) {
        return;
    }
    payload = frame + ETH_HDR_SIZE;
    len = (uint16_t)(frame_len - ETH_HDR_SIZE);
    copy_len = len > PACKET_SLOT_SIZE ? PACKET_SLOT_SIZE : len;

    /* Runs in the receive interrupt, where interrupts are already off; the
       table and the queues are touched without taking net_lock again. */
    for (b = packets; b; b = b->next) {
        struct packet_queue *q = &b->q;
        int slot;

        if (b->sock->protocol != ethertype) {
            continue; /* another EtherType, or a socket that asked for none */
        }
        if (b->ifindex != 0 && b->ifindex != arrived_on) {
            continue; /* another interface's traffic */
        }
        if (q->count == PACKET_QUEUE_SLOTS) {
            /* Drop the newest, keeping the unread ones in order; log once. */
            q->dropped++;
            if (!q->reported) {
                q->reported = 1;
                klog(KLOG_WARN, "packet",
                     "rx: the queue is full, dropping frames on a bound socket");
            }
            continue;
        }
        slot = (q->head + q->count) % PACKET_QUEUE_SLOTS;
        k_memcpy(q->buf[slot], payload, copy_len);
        q->len[slot] = copy_len;
        k_memcpy(q->src[slot], frame + 6, 6);
        q->pkttype[slot] = packet_type_of(nd, frame);
        q->proto[slot] = ethertype;
        q->ifindex[slot] = (uint8_t)arrived_on;
        q->count++;
        wq_wake(&b->sock->waiters, WQ_DATA);
    }
}

int packet_ready(struct socket *sock)
{
    struct packet_queue *q = queue_of(sock);

    return q && q->count > 0;
}

int packet_pending(struct socket *sock)
{
    struct packet_queue *q;
    uint32_t flags;
    int pending;

    flags = net_lock();
    q = queue_of(sock);
    pending = (q && q->count > 0) ? (int)q->len[q->head] : -1;
    net_unlock(flags);
    return pending;
}

int packet_peek(struct socket *sock, uint32_t offset, uint8_t *buf, int len)
{
    struct packet_queue *q;
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

/* Keep the waiting datagram's address as the one `packet_last_addr`
 * reports; the caller holds `net_lock` and has found a datagram waiting. */
static void keep_head_addr(struct packet_queue *q)
{
    k_memcpy(q->last_src, q->src[q->head], 6);
    q->last_pkttype = q->pkttype[q->head];
    q->last_proto = q->proto[q->head];
    q->last_ifindex = q->ifindex[q->head];
    q->have_last = 1;
}

void packet_consume(struct socket *sock)
{
    struct packet_queue *q;
    uint32_t flags;

    flags = net_lock();
    q = queue_of(sock);
    if (q && q->count > 0) {
        keep_head_addr(q);
        q->head = (q->head + 1) % PACKET_QUEUE_SLOTS;
        q->count--;
    }
    net_unlock(flags);
}

void packet_note_head(struct socket *sock)
{
    struct packet_queue *q;
    uint32_t flags;

    flags = net_lock();
    q = queue_of(sock);
    if (q && q->count > 0) {
        keep_head_addr(q);
    }
    net_unlock(flags);
}

int packet_last_addr(struct socket *sock, uint8_t *sll)
{
    struct packet_queue *q;
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
    /* The frame's own EtherType, not the socket's, which a bind may change. */
    sll[2] = (uint8_t)((q->last_proto >> 8) & 0xff); /* network order */
    sll[3] = (uint8_t)(q->last_proto & 0xff);
    /* The interface the datagram arrived on, not always the first. */
    sll[4] = q->last_ifindex;
    sll[8] = (uint8_t)PACKET_HATYPE_ETHER;
    sll[10] = q->last_pkttype;
    sll[11] = 6;
    for (i = 0; i < 6; i++) {
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

    if (len == 0 || len > 1500) {
        return -EMSGSIZE;
    }
    if (ethertype != ETH_TYPE_IP && ethertype != ETH_TYPE_ARP) {
        return -EINVAL;
    }
    if (ifindex != 0 && (ifindex < 1 || ifindex > netdev_count())) {
        return -ENODEV;
    }
    /* The Ethernet transmit buffer is shared with the receive interrupt. */
    flags = net_lock();
    b = binding_of(sock);
    if (ifindex == 0 && b) {
        ifindex = b->ifindex;
    }
    nd = ifindex != 0 ? netdev_at(ifindex - 1) : netdev_first();
    if (!nd) {
        net_unlock(flags);
        return -ENETDOWN;
    }
    ethernet_tx(nd, dst_mac, ethertype, data, len);
    net_unlock(flags);
    return (int)len;
}
