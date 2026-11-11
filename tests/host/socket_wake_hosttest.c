/**
 * Wake sockets from the receive side with the kernel's socket.c, and check
 * the kind of change each wake names.
 *
 * host-deps: rach2/sys/lib/kstring.c rach2/sys/lib/kctypes.c
 * host-deps: rach2/sys/network/socket/address.c rach2/sys/network/ip6/addr6.c
 * host-deps: tests/host/compat/kheap_host.c
 * host-deps: rach2/sys/network/socket/dgq.c
 *
 * A datagram delivered to a UDP socket, and data delivered to a TCP
 * connection, wake the socket's queue for data alone, and data a full TCP
 * receive ring takes none of wakes nothing; an acknowledgement that
 * frees send-ring space wakes it for room alone; a FIN, a connection that
 * completes or ends, and a connection queued on a listener wake it for every
 * kind. So an edge-triggered epoll registration of a socket for output alone
 * is not told of data that comes, nor one for input alone of room made.
 *
 * The answers after shutdown(2) and a listener's close are checked with the
 * same file: a stream shut for receiving whose connection was then reset
 * reads ECONNRESET once and then 0, as Linux's tcp_recvmsg_locked tests
 * sk_err before RCV_SHUTDOWN; a send on a socket a shutdown in SYN_SENT left
 * with no block fails with ECONNRESET once and then EPIPE, as
 * sk_stream_wait_connect gives sock_error first; a UDP socket shut for
 * receiving ends a blocking receive with 0 once its queue is empty and polls
 * POLLIN and POLLRDHUP, and POLLHUP shut both ways, as datagram_poll; and a
 * closing listener resets each queued connection before releasing it, as
 * inet_csk_listen_stop. The protocol layers are substitutes.
 */
#include "network/socket/socket.c"
#include "ns_first.h"

extern int printf(const char *format, ...);

/** Report that the caller sees the devices of the model's one network
 * namespace, the first. */
int net_ns_seen(void)
{
    return 1;
}

/** Do nothing: no row ends the namespace. */
void unix_ns_gone(const struct netns *ns)
{
    (void)ns;
}

/** Do nothing: no row ends the namespace. */
void tcp_ns_gone(const struct netns *ns, const struct netns *to)
{
    (void)ns;
    (void)to;
}

/** Do nothing: no row ends the namespace. */
void ip_loop_ns_gone(const struct netns *ns)
{
    (void)ns;
}

/* The namespace of the datagram being delivered: always the first, the
 * model's only one. */
const struct netns *ip_ns;

static wait_queue_t *woken;  /* the queue wq_wake last named; NULL before */
static uint32_t woken_kinds; /* the WQ_ kinds of every wake, or-ed together */
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

/** Record a wake of `wq` for every kind of change. */
void wq_wake_all(wait_queue_t *wq)
{
    wq_wake(wq, WQ_ALL);
}

/** Make `wq` an empty queue. */
void wait_queue_init(wait_queue_t *wq)
{
    wq->head = NULL;
}

/** Return 0 when `cond` holds already; otherwise fail the row and return
 * -EINTR, since no process could end the wait in this model. */
int wq_wait_event(wait_queue_t *wq, wq_cond_fn cond, void *arg)
{
    (void)wq;
    if (cond(arg)) {
        return 0;
    }
    CHECK(0, "no row waits for a condition that does not hold");
    return -EINTR;
}

/** Make `l` free: no row reads a stream, which alone takes it. */
void sleep_lock_init(struct sleep_lock *l, struct sleep_lock_record *record, int flags)
{
    (void)record;
    l->holder = -1;
    l->depth = 0;
    l->flags = flags;
}

/** The wall clock `sock_dgq_put` stamps a datagram with: the epoch, since
 * no test here reads a timestamp. */
void rtc_realtime(int64_t *seconds, int64_t *nanoseconds)
{
    *seconds = 0;
    *nanoseconds = 0;
}

/** Take the network lock: a no-op, since no interrupt runs in this model. */
uint32_t net_lock(void)
{
    return 0;
}

/** Release the network lock: a no-op. */
void net_unlock(uint32_t flags)
{
    (void)flags;
}

/** Discard a kernel log record. */
void klog(int level, const char *subsystem, const char *format, ...)
{
    (void)level;
    (void)subsystem;
    (void)format;
}

/** Unbind a packet socket: a no-op, since no row makes one. */
void packet_unbind(struct socket *sock)
{
    (void)sock;
}

/** Drop a packet socket's head datagram: a no-op. */
void packet_consume(struct socket *sock)
{
    (void)sock;
}

/** Note a packet socket's head datagram: a no-op. */
void packet_note_head(struct socket *sock)
{
    (void)sock;
}

/** Unregister a raw socket: a no-op, since no row makes one. */
void raw_unregister(struct socket *sock)
{
    (void)sock;
}

/** Drop a raw socket's head datagram: a no-op. */
void raw_consume(struct socket *sock)
{
    (void)sock;
}

/** Report 0 as the source of a raw socket's last datagram. */
uint32_t raw_last_src(struct socket *sock)
{
    (void)sock;
    return 0;
}

/** Report 0 as the source of a raw socket's head datagram. */
uint32_t raw_head_src(struct socket *sock)
{
    (void)sock;
    return 0;
}

/** Return -1 for a socket with no block, as the kernel's does; fail the row
 * otherwise, since no row sends on a connection. */
int tcp_send(struct socket *sock, const uint8_t *buf, int len)
{
    (void)buf;
    (void)len;
    if (!sock->tcb) {
        return -1;
    }
    CHECK(0, "no row sends on a connection");
    return 0;
}

static struct socket *forgotten; /* the socket tcp_forget_queued last ended */

/** Record the queued connection ended, and take its block, as the kernel's
 * does. */
void tcp_forget_queued(struct socket *sock)
{
    forgotten = sock;
    sock->tcb = NULL;
}

/** Close a TCP connection: a no-op. */
void tcp_close(struct socket *sock)
{
    (void)sock;
}

/** Announce a TCP receive window: a no-op. */
void tcp_window_update(struct socket *sock)
{
    (void)sock;
}

/** Report every host range brought in: no row receives into a buffer. */
int vm_prefault_range(uintptr_t addr, uintptr_t len, int write)
{
    (void)addr;
    (void)len;
    (void)write;
    return 1;
}

/** Unbind a UDP socket: a no-op. */
void udp_unbind(struct socket *sock)
{
    (void)sock;
}

/** Report that no Unix-domain socket uses inode `ino`. */
int unix_inode_used(uint32_t ino)
{
    (void)ino;
    return 0;
}

/** Forget the wakes recorded so far. */
static void reset(void)
{
    woken = NULL;
    woken_kinds = 0;
}

/** Close a netlink socket: a no-op, since no row makes one. */
void netlink_close(struct socket *sock)
{
    (void)sock;
}

/** Report an empty netlink socket queue: -1. */
int netlink_pending(struct socket *sock)
{
    (void)sock;
    return -1;
}

/** Copy nothing from a netlink socket's queue and return -1. */
int netlink_peek(struct socket *sock, uint32_t offset, uint8_t *buf, int len)
{
    (void)sock;
    (void)offset;
    (void)buf;
    (void)len;
    return -1;
}

/** Drop a netlink socket's head message: a no-op. */
void netlink_consume(struct socket *sock)
{
    (void)sock;
}

/** Note a netlink read's size: a no-op, since no row reads one. */
void netlink_note_recv(struct socket *sock, uint32_t len)
{
    (void)sock;
    (void)len;
}

/* The counters socket.c keeps for a datagram its queue drops. */
struct net_mib net_mib;
struct net_mib6 net_mib6;

/** Report that no message is queued for a netlink socket. */
int netlink_ready(struct socket *sock)
{
    (void)sock;
    return 0;
}

/**
 * The answers of a stream, a UDP socket and a listener after shutdown(2) or
 * a close, as the file's comment lists them.
 */
static void shutdown_rows(void)
{
    static struct socket tcp, udp;
    static struct tcb block;
    static uint8_t ring[TCP_RECV_RING_SMALL];
    static const uint8_t bytes[4] = {1, 2, 3, 4};
    static uint8_t buf[8];
    struct socket *listener, *conn;

    k_memset(&block, 0, sizeof(block));
    block.recv_buf = ring;
    block.recv_size = sizeof(ring);
    block.state = TCP_CLOSED;
    block.so_error = ECONNRESET;
    tcp.tcb = &block;
    tcp.shutdown = SOCK_RCV_SHUTDOWN;
    CHECK(socket_tcp_try_recv(&tcp, buf, sizeof(buf), 0) == -ECONNRESET,
          "a read after SHUT_RD of a reset connection gives ECONNRESET");
    CHECK(socket_tcp_try_recv(&tcp, buf, sizeof(buf), 0) == 0, "and then the end of input");

    tcp.tcb = NULL;
    tcp.shutdown = 0;
    tcp.so_error = ECONNRESET;
    CHECK(socket_tcp_send(&tcp, bytes, sizeof(bytes), 0) == -ECONNRESET,
          "a send after a shutdown in SYN_SENT gives the recorded ECONNRESET");
    CHECK(socket_tcp_send(&tcp, bytes, sizeof(bytes), 0) == -EPIPE, "and then EPIPE");

    udp.rcvbuf = SOCK_RCVBUF_DEFAULT;
    udp.type = SOCK_DGRAM;
    udp.shutdown = SOCK_RCV_SHUTDOWN;
    socket_udp_deliver(&udp, net_addr4(0x0A000002u), 7, bytes, sizeof(bytes));
    CHECK(socket_dgram_wait(&udp) == 0 &&
              socket_recv(&udp, buf, sizeof(buf), NULL, NULL, 0) == (int)sizeof(bytes),
          "a UDP socket shut for receiving still gives the datagram queued");
    CHECK(socket_dgram_wait(&udp) == SOCK_RECV_SHUT, "and then ends the wait at the shutdown");
    CHECK(socket_recv(&udp, buf, sizeof(buf), NULL, NULL, 0) == 0,
          "where a blocking receive returns 0");
    CHECK(socket_try_recv(&udp, buf, sizeof(buf), NULL, NULL, 0) == SOCK_EAGAIN,
          "and one that does not wait EAGAIN, as Linux's");
    CHECK(socket_poll(&udp, (short)(POLLIN | POLLOUT | POLLRDHUP), NULL) ==
              (short)(POLLIN | POLLOUT | POLLRDHUP),
          "it polls POLLIN and POLLRDHUP");
    udp.shutdown |= SOCK_SEND_SHUTDOWN;
    CHECK((socket_poll(&udp, POLLIN, NULL) & POLLHUP) != 0, "and POLLHUP shut both ways");

    listener = socket_alloc(SOCK_STREAM);
    conn = socket_alloc(SOCK_STREAM);
    CHECK(listener && conn, "a listener and a connection can be made");
    if (!listener || !conn) {
        return;
    }
    listener->listening = 1;
    conn->tcb = &block;
    forgotten = NULL;
    socket_tcp_accept(listener, conn);
    socket_unref(listener);
    CHECK(forgotten == conn, "a listener's close resets the connection nobody accepted");
}

/** Run the rows; return 0 when every one passes and 1 otherwise. */
int main(void)
{
    static struct socket udp, tcp, listener, conn;
    static struct tcb block;
    static uint8_t ring[TCP_RECV_RING_SMALL];
    static const uint8_t bytes[4] = {1, 2, 3, 4};

    udp.rcvbuf = SOCK_RCVBUF_DEFAULT;
    reset();
    socket_udp_deliver(&udp, net_addr4(0x0A000002u), 7, bytes, sizeof(bytes));
    CHECK(udp.rq.count == 1 && woken == &udp.waiters && woken_kinds == WQ_DATA,
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
    shutdown_rows();
    printf("socket-wake-hosttest: result: %s: %d failures\n", failures ? "fail" : "pass", failures);
    return failures ? 1 : 0;
}

/** No interface exists in the wake fixture. */
struct netdev *netdev_at(int n)
{
    (void)n;
    return NULL;
}

/** No socket of the wake fixture joins a group. */
void netdev_leave6(struct netdev *nd, const uint8_t *group)
{
    (void)nd;
    (void)group;
}

/** No raw message is queued in the wake fixture, so nothing is known. */
void raw_head_info(struct socket *sock, struct net_rxinfo *info)
{
    (void)sock;
    k_memset(info, 0, sizeof(*info));
    info->hlim = -1;
    info->tclass = -1;
}

/** No raw message was consumed in the wake fixture. */
void raw_last_info(struct socket *sock, struct net_rxinfo *info)
{
    raw_head_info(sock, info);
}

/** No raw message is queued in the wake fixture. */
void raw_head_address(struct socket *sock, net_addr_t *src)
{
    (void)sock;
    *src = net_addr4(0);
}

/** No raw message was consumed in the wake fixture. */
void raw_last_address(struct socket *sock, net_addr_t *src)
{
    raw_head_address(sock, src);
}
