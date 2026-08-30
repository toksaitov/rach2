/*
 * packet.c: the socket that sees Ethernet, for the one program that must.
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
#include "../../serial.h"
#include "../../proc/wait_queue.h"

/* One client is the case; two is room for a second. Each socket's queue is
   eight datagrams of one MTU, which is 24 KB of the kernel's image for the two
   of them, and enough to hold a reply behind a burst of other people's frames
   while the client is being scheduled. */
#define PACKET_MAX_BOUND 2
#define PACKET_QUEUE_SLOTS 8
#define PACKET_SLOT_SIZE 1500

static struct socket *bound[PACKET_MAX_BOUND];

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
    int head;  /* the next datagram to be read */
    int count; /* how many are waiting */
    int dropped;
    int reported;
    /* The address of the datagram most recently taken, since a receive reports
       the sender after the datagram it belongs to has gone. */
    uint8_t last_src[6];
    uint8_t last_pkttype;
    uint16_t last_proto;
    int have_last;
};

static struct packet_queue queues[PACKET_MAX_BOUND];

/** The queue belonging to `sock`, or NULL if it is not bound. */
static struct packet_queue *queue_of(struct socket *sock)
{
    int i;

    for (i = 0; i < PACKET_MAX_BOUND; i++) {
        if (bound[i] == sock) {
            return &queues[i];
        }
    }
    return NULL;
}

int packet_bind(struct socket *sock, int ifindex)
{
    uint32_t flags;
    int i, free_slot;

    if (ifindex != 0 && ifindex != NETDEV_IFINDEX) {
        return -ENODEV;
    }

    flags = net_lock();
    free_slot = -1;
    for (i = 0; i < PACKET_MAX_BOUND; i++) {
        if (bound[i] == sock) {
            net_unlock(flags);
            return 0;
        }
        if (!bound[i] && free_slot < 0) {
            free_slot = i;
        }
    }
    if (free_slot < 0) {
        net_unlock(flags);
        return -ENOBUFS;
    }
    bound[free_slot] = sock;
    queues[free_slot].head = 0;
    queues[free_slot].count = 0;
    queues[free_slot].dropped = 0;
    queues[free_slot].reported = 0;
    queues[free_slot].have_last = 0;
    net_unlock(flags);
    return 0;
}

void packet_unbind(struct socket *sock)
{
    uint32_t flags;
    int i;

    flags = net_lock();
    for (i = 0; i < PACKET_MAX_BOUND; i++) {
        if (bound[i] == sock) {
            bound[i] = NULL;
            queues[i].count = 0;
            queues[i].head = 0;
            queues[i].have_last = 0;
        }
    }
    net_unlock(flags);
}

/** Which of PACKET_HOST/BROADCAST/MULTICAST/OTHERHOST a destination MAC is. */
static uint8_t packet_type_of(const uint8_t *dst)
{
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
        if (dst[i] != net_mac[i]) {
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

void packet_rx(uint16_t ethertype, const uint8_t *frame, uint16_t frame_len)
{
    const uint8_t *payload;
    uint16_t len, copy_len;
    int i;

    if (frame_len < ETH_HDR_SIZE) {
        return;
    }
    payload = frame + ETH_HDR_SIZE;
    len = (uint16_t)(frame_len - ETH_HDR_SIZE);
    copy_len = len > PACKET_SLOT_SIZE ? PACKET_SLOT_SIZE : len;

    /* Runs in the receive interrupt, where interrupts are already off; the
       table and the queues are touched without taking net_lock again. */
    for (i = 0; i < PACKET_MAX_BOUND; i++) {
        struct packet_queue *q = &queues[i];
        int slot;

        if (!bound[i]) {
            continue;
        }
        if (bound[i]->protocol != ethertype) {
            continue; /* another EtherType, or a socket that asked for none */
        }
        if (q->count == PACKET_QUEUE_SLOTS) {
            /* The newest is dropped, so a client that is behind still finds
               the frames it has not read yet in the order they arrived. Said
               once per socket: a flood must not become a flood of logging. */
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
        q->pkttype[slot] = packet_type_of(frame);
        q->proto[slot] = ethertype;
        q->count++;
        wq_wake_all(&bound[i]->waiters);
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

void packet_consume(struct socket *sock)
{
    struct packet_queue *q;
    uint32_t flags;

    flags = net_lock();
    q = queue_of(sock);
    if (q && q->count > 0) {
        k_memcpy(q->last_src, q->src[q->head], 6);
        q->last_pkttype = q->pkttype[q->head];
        q->last_proto = q->proto[q->head];
        q->have_last = 1;
        q->head = (q->head + 1) % PACKET_QUEUE_SLOTS;
        q->count--;
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
    /* The frame's own EtherType, not the socket's: a socket rebound to another
       protocol must not relabel a datagram that arrived under the first. */
    sll[2] = (uint8_t)((q->last_proto >> 8) & 0xff); /* network order */
    sll[3] = (uint8_t)(q->last_proto & 0xff);
    sll[4] = (uint8_t)NETDEV_IFINDEX;
    sll[8] = (uint8_t)PACKET_HATYPE_ETHER;
    sll[10] = q->last_pkttype;
    sll[11] = 6;
    for (i = 0; i < 6; i++) {
        sll[12 + i] = q->last_src[i];
    }
    net_unlock(flags);
    return PACKET_SLL_LEN;
}

int packet_send(struct socket *sock, const uint8_t *dst_mac, uint16_t ethertype,
                const uint8_t *data, uint16_t len)
{
    uint32_t flags;

    (void)sock;

    if (len == 0 || len > 1500) {
        return -EMSGSIZE;
    }
    if (ethertype != ETH_TYPE_IP && ethertype != ETH_TYPE_ARP) {
        return -EINVAL;
    }
    /* The Ethernet transmit buffer is one buffer for the machine, and this call
       runs in syscall context with interrupts on, so the device check and the
       whole of the send are held against a receive interrupt that would send
       through the same buffer. This is the rule in net.h. */
    flags = net_lock();
    if (!netdev) {
        net_unlock(flags);
        return -ENETDOWN;
    }
    ethernet_tx(dst_mac, ethertype, data, len);
    net_unlock(flags);
    return (int)len;
}
