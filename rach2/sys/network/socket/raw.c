/*
 * raw.c: AF_INET SOCK_RAW sockets, as raw.h describes them: the list of
 * registered sockets and their receive queues. `sendto` in syscall_net.c
 * makes a raw send.
 */

#include "../../klog.h"
#include "raw.h"
#include "socket.h"
#include "../net.h"
#include "../ip/ip.h"
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

struct raw_queue {
    uint8_t buf[RAW_QUEUE_SLOTS][RAW_SLOT_SIZE];
    uint16_t len[RAW_QUEUE_SLOTS];
    uint32_t src[RAW_QUEUE_SLOTS];
    int head;  /* the next datagram to be read */
    int count; /* how many are waiting */
    int dropped;
    int reported; /* whether a drop has been logged */
    uint32_t last_src;
};

/* A registered socket: the socket, its registered protocol, and its
 * queue, one record from the heap per registration, on a list the receive
 * path walks. */
struct raw_binding {
    struct raw_binding *next;
    struct socket *sock;
    uint8_t proto;
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

/** `ip_rx` runs with interrupts off, so the list and the queues are touched
 * without taking `net_lock`. A full queue drops the newest datagram and
 * keeps the unread ones in order. */
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

        if (b->proto != proto) {
            continue;
        }
        if (q->count == RAW_QUEUE_SLOTS) {
            q->dropped++;
            if (!q->reported) {
                q->reported = 1;
                klog(KLOG_WARN, "raw", "rx: the queue is full, dropping datagrams");
            }
            continue;
        }
        slot = (q->head + q->count) % RAW_QUEUE_SLOTS;
        k_memcpy(q->buf[slot], data, copy_len);
        q->len[slot] = copy_len;
        q->src[slot] = src;
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
    src = q && q->count > 0 ? q->src[q->head] : 0u;
    net_unlock(flags);
    return src;
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
