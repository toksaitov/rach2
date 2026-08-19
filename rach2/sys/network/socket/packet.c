/*
 * packet.c: the socket that sees Ethernet, for the one program that must.
 *
 * The contract and the reasoning are in packet.h. What is here is the table of
 * bound sockets and the two directions.
 */

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
    int head;  /* the next datagram to be read */
    int count; /* how many are waiting */
    int dropped;
    int reported;
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
        }
    }
    net_unlock(flags);
}

void packet_rx(const uint8_t *payload, uint16_t len)
{
    uint16_t copy_len = len > PACKET_SLOT_SIZE ? PACKET_SLOT_SIZE : len;
    int i;

    /* Runs in the receive interrupt, where interrupts are already off; the
       table and the queues are touched without taking net_lock again. */
    for (i = 0; i < PACKET_MAX_BOUND; i++) {
        struct packet_queue *q = &queues[i];
        int slot;

        if (!bound[i]) {
            continue;
        }
        if (q->count == PACKET_QUEUE_SLOTS) {
            /* The newest is dropped, so a client that is behind still finds
               the frames it has not read yet in the order they arrived. Said
               once per socket: a flood must not become a flood of logging. */
            q->dropped++;
            if (!q->reported) {
                q->reported = 1;
                serial_printf("packet: queue full: dropping frames on a bound socket\n");
            }
            continue;
        }
        slot = (q->head + q->count) % PACKET_QUEUE_SLOTS;
        k_memcpy(q->buf[slot], payload, copy_len);
        q->len[slot] = copy_len;
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
        q->head = (q->head + 1) % PACKET_QUEUE_SLOTS;
        q->count--;
    }
    net_unlock(flags);
}

int packet_send(struct socket *sock, const uint8_t *dst_mac, const uint8_t *data, uint16_t len)
{
    uint32_t flags;

    (void)sock;

    if (len == 0 || len > 1500) {
        return -EMSGSIZE;
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
    ethernet_tx(dst_mac, ETH_TYPE_IP, data, len);
    net_unlock(flags);
    return (int)len;
}
