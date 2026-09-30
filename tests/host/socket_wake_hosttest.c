/**
 * Wake sockets from the receive side with the kernel's socket.c, and check
 * the kind of change each wake names.
 *
 * host-deps: rach2/sys/lib/kstring.c rach2/sys/lib/kctypes.c
 * host-deps: tests/host/compat/kheap_host.c
 *
 * A datagram delivered to a UDP socket, and data delivered to a TCP
 * connection, wake the socket's queue for data alone, and data a full TCP
 * receive ring takes none of wakes nothing; an acknowledgement that
 * frees send-ring space wakes it for room alone; a FIN, a connection that
 * completes or ends, and a connection queued on a listener wake it for every
 * kind. So an edge-triggered epoll registration of a socket for output alone
 * is not told of data that comes, nor one for input alone of room made. The
 * protocol layers are substitutes that no row calls.
 */
#include "network/socket/socket.c"

extern int printf(const char *format, ...);

static wait_queue_t *woken;
static uint32_t woken_kinds;
static int failures;

#define CHECK(condition, detail)                                  \
    do {                                                          \
        if (!(condition)) {                                       \
            printf("socket-wake-hosttest: failed: %s\n", detail); \
            failures++;                                           \
        }                                                         \
    } while (0)

/** Record the queue a wake names and the kinds of change; no process waits
 * in this model. */
void wq_wake(wait_queue_t *wq, uint32_t kinds)
{
    woken = wq;
    woken_kinds |= kinds;
}

void wq_wake_all(wait_queue_t *wq)
{
    wq_wake(wq, WQ_ALL);
}

void wait_queue_init(wait_queue_t *wq)
{
    wq->head = NULL;
}

int wq_wait_event(wait_queue_t *wq, wq_cond_fn cond, void *arg)
{
    (void)wq;
    (void)cond;
    (void)arg;
    CHECK(0, "no row waits");
    return -EINTR;
}

uint32_t net_lock(void)
{
    return 0;
}

void net_unlock(uint32_t flags)
{
    (void)flags;
}

void klog(int level, const char *subsystem, const char *format, ...)
{
    (void)level;
    (void)subsystem;
    (void)format;
}

void packet_unbind(struct socket *sock)
{
    (void)sock;
}

int packet_ready(struct socket *sock)
{
    (void)sock;
    return 0;
}

int packet_pending(struct socket *sock)
{
    (void)sock;
    return -1;
}

int packet_peek(struct socket *sock, uint32_t offset, uint8_t *buf, int len)
{
    (void)sock;
    (void)offset;
    (void)buf;
    (void)len;
    return -1;
}

void packet_consume(struct socket *sock)
{
    (void)sock;
}

void raw_unregister(struct socket *sock)
{
    (void)sock;
}

int raw_ready(struct socket *sock)
{
    (void)sock;
    return 0;
}

int raw_pending(struct socket *sock)
{
    (void)sock;
    return -1;
}

int raw_peek(struct socket *sock, uint32_t offset, uint8_t *buf, int len)
{
    (void)sock;
    (void)offset;
    (void)buf;
    (void)len;
    return -1;
}

void raw_consume(struct socket *sock)
{
    (void)sock;
}

uint32_t raw_last_src(struct socket *sock)
{
    (void)sock;
    return 0;
}

int tcp_send(struct socket *sock, const uint8_t *buf, int len)
{
    (void)sock;
    (void)buf;
    (void)len;
    CHECK(0, "no row sends");
    return 0;
}

void tcp_close(struct socket *sock)
{
    (void)sock;
}

void tcp_window_update(struct socket *sock)
{
    (void)sock;
}

void udp_unbind(struct socket *sock)
{
    (void)sock;
}

int unix_inode_used(uint32_t ino)
{
    (void)ino;
    return 0;
}

static void reset(void)
{
    woken = NULL;
    woken_kinds = 0;
}

/** No row makes a netlink socket, whose calls socket.c makes. */
void netlink_close(struct socket *sock)
{
    (void)sock;
}

int netlink_pending(struct socket *sock)
{
    (void)sock;
    return -1;
}

int netlink_peek(struct socket *sock, uint32_t offset, uint8_t *buf, int len)
{
    (void)sock;
    (void)offset;
    (void)buf;
    (void)len;
    return -1;
}

void netlink_consume(struct socket *sock)
{
    (void)sock;
}

int netlink_ready(struct socket *sock)
{
    (void)sock;
    return 0;
}

int main(void)
{
    static struct socket udp, tcp, listener, conn;
    static struct tcb block;
    static uint8_t ring[TCP_RECV_RING_SMALL];
    static const uint8_t bytes[4] = {1, 2, 3, 4};

    reset();
    socket_udp_deliver(&udp, 0x0A000002u, 7, bytes, sizeof(bytes));
    CHECK(udp.recv_count == 1 && woken == &udp.waiters && woken_kinds == WQ_DATA,
          "a datagram wakes a UDP socket for data alone");

    tcp.tcb = &block;
    block.recv_buf = ring;
    block.recv_size = sizeof(ring);
    reset();
    CHECK(socket_tcp_deliver(&tcp, bytes, sizeof(bytes)) == sizeof(bytes),
          "a TCP connection takes the data delivered");
    CHECK(woken == &tcp.waiters && woken_kinds == WQ_DATA,
          "data wakes a TCP connection for data alone");
    block.recv_tail = block.recv_head + block.recv_size;
    reset();
    CHECK(socket_tcp_deliver(&tcp, bytes, sizeof(bytes)) == 0 && woken == NULL && woken_kinds == 0,
          "data a full TCP receive ring takes none of wakes nothing");
    reset();
    socket_tcp_writable(&tcp);
    CHECK(woken == &tcp.waiters && woken_kinds == WQ_ROOM,
          "freed send-ring space wakes a TCP connection for room alone");
    reset();
    socket_tcp_fin(&tcp);
    CHECK(woken == &tcp.waiters && woken_kinds == WQ_ALL,
          "a FIN wakes a TCP connection for every kind");
    reset();
    socket_tcp_connected(&tcp);
    CHECK(woken == &tcp.waiters && woken_kinds == WQ_ALL,
          "a connection that completes or ends wakes it for every kind");
    reset();
    socket_tcp_accept(&listener, &conn);
    CHECK(woken == &listener.waiters && woken_kinds == WQ_ALL,
          "a connection queued on a listener wakes it for every kind");
    printf("socket-wake-hosttest: result: %s: %d failures\n", failures ? "fail" : "pass", failures);
    return failures ? 1 : 0;
}
