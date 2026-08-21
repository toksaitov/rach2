/*
 * raw.c: AF_INET SOCK_RAW sockets, what `ping` and `traceroute` are built on.
 *
 * The contract and the reasoning are in raw.h. What is here is the table of
 * registered sockets and the two directions.
 */

#include "raw.h"
#include "socket.h"
#include "../net.h"
#include "../ip/ip.h"
#include "../../lib/kerrno.h"
#include "../../lib/kstring.h"
#include "../../serial.h"
#include "../../proc/wait_queue.h"

#define RAW_QUEUE_SLOTS 8
#define RAW_SLOT_SIZE   1500

static struct socket *bound[RAW_MAX_BOUND];
static uint8_t bound_proto[RAW_MAX_BOUND];

struct raw_queue {
    uint8_t buf[RAW_QUEUE_SLOTS][RAW_SLOT_SIZE];
    uint16_t len[RAW_QUEUE_SLOTS];
    uint32_t src[RAW_QUEUE_SLOTS];
    int head;  /* the next datagram to be read */
    int count; /* how many are waiting */
    int dropped;
    int reported;
    uint32_t last_src;
};

static struct raw_queue queues[RAW_MAX_BOUND];

/** The queue belonging to `sock`, or NULL if it is not registered. */
static struct raw_queue *queue_of(struct socket *sock)
{
    int i;

    for (i = 0; i < RAW_MAX_BOUND; i++) {
        if (bound[i] == sock) {
            return &queues[i];
        }
    }
    return NULL;
}

int raw_register(struct socket *sock, uint8_t proto)
{
    uint32_t flags;
    int i, free_slot;

    flags = net_lock();
    free_slot = -1;
    for (i = 0; i < RAW_MAX_BOUND; i++) {
        if (bound[i] == sock) {
            bound_proto[i] = proto;
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
    bound_proto[free_slot] = proto;
    queues[free_slot].head = 0;
    queues[free_slot].count = 0;
    queues[free_slot].dropped = 0;
    queues[free_slot].reported = 0;
    queues[free_slot].last_src = 0;
    net_unlock(flags);
    return 0;
}

void raw_unregister(struct socket *sock)
{
    uint32_t flags;
    int i;

    flags = net_lock();
    for (i = 0; i < RAW_MAX_BOUND; i++) {
        if (bound[i] == sock) {
            bound[i] = NULL;
            bound_proto[i] = 0;
            queues[i].count = 0;
            queues[i].head = 0;
            queues[i].last_src = 0;
        }
    }
    net_unlock(flags);
}

void raw_rx(uint8_t proto, const uint8_t *data, uint16_t total_len)
{
    uint16_t copy_len = total_len > RAW_SLOT_SIZE ? RAW_SLOT_SIZE : total_len;
    uint32_t src;
    int i;

    if (total_len < IP_HDR_SIZE) {
        return;
    }
    src = ((uint32_t)data[12] << 24) | ((uint32_t)data[13] << 16) |
          ((uint32_t)data[14] << 8) | (uint32_t)data[15];

    /* Runs in the receive interrupt, where interrupts are already off; the
       table and the queues are touched without taking net_lock again. Every
       matching socket gets its own copy: two programs watching ICMP each see
       the whole exchange, which is what a tap means. */
    for (i = 0; i < RAW_MAX_BOUND; i++) {
        struct raw_queue *q = &queues[i];
        int slot;

        if (!bound[i] || bound_proto[i] != proto) {
            continue;
        }
        if (q->count == RAW_QUEUE_SLOTS) {
            /* The newest is dropped, so a client that is behind still finds
               the datagrams it has not read yet in the order they arrived.
               Said once per socket: a flood must not become a flood of
               logging. */
            q->dropped++;
            if (!q->reported) {
                q->reported = 1;
                serial_printf("raw: queue full: dropping datagrams on a raw socket\n");
            }
            continue;
        }
        slot = (q->head + q->count) % RAW_QUEUE_SLOTS;
        k_memcpy(q->buf[slot], data, copy_len);
        q->len[slot] = copy_len;
        q->src[slot] = src;
        q->count++;
        wq_wake_all(&bound[i]->waiters);
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
        q->head = (q->head + 1) % RAW_QUEUE_SLOTS;
        q->count--;
    }
    net_unlock(flags);
}

uint32_t raw_last_src(struct socket *sock)
{
    struct raw_queue *q;
    uint32_t flags, src;

    flags = net_lock();
    q = queue_of(sock);
    src = q ? q->last_src : 0u;
    net_unlock(flags);
    return src;
}
