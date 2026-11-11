/*
 * socket.c: the socket layer between the system calls and the protocols, as
 * socket.h describes it: the list of live sockets, their lifetime, and the
 * datagram and stream paths. A blocking receive sleeps on the socket's wait
 * queue, and the receive path, which runs with interrupts off, fills the
 * queue and wakes it.
 */

#include "../../klog.h"
#include "socket.h"
#include "netlink.h"
#include "packet.h"
#include "raw.h"
#include "../net.h"
#include "../ip/ip.h"
#include "../ip6/ip6.h"
#include "../udp/udp.h"
#include "../tcp/tcp.h"
#include "../../io/poll.h"
#include "../../serial.h"
#include "../../lib/kstring.h"
#include "../../lib/kerrno.h"
#include "../../mm/kheap.h"
#include "../../mm/paging.h"
#include "../../ipc/unix.h"
#include "../../proc/ns.h"
#include "../driver/netdev.h"

/* The live sockets, each allocated from the heap when opened and freed
 * when its last reference goes. An incoming SYN allocates one from the
 * interrupt side, which the heap allows: it masks interrupts and never
 * sleeps. */
static socket_t *sockets;

void socket_init(void)
{
    sockets = NULL;
}

static uint32_t next_socket_inode = 1;

/**
 * The next inode to give a socket: rising, nonzero, and not one in use.
 *
 * The number rises, so a socket made later never reads as one that is gone.
 * The counter is 32 bits and wraps, so the live sockets of both kinds, these
 * and the local ones, are checked and a number one of them has is stepped
 * over. Zero is skipped because it is what an unset field reads as. The
 * caller holds `net_lock`.
 */
static uint32_t socket_next_inode(void)
{
    for (;;) {
        uint32_t candidate = next_socket_inode++;
        const socket_t *s;

        if (candidate == 0) {
            continue;
        }
        for (s = sockets; s && s->inode != candidate; s = s->next) {
        }
        if (!s && !unix_inode_used(candidate)) {
            return candidate;
        }
    }
}

uint32_t socket_inode_next(void)
{
    uint32_t flags = net_lock();
    uint32_t ino = socket_next_inode();

    net_unlock(flags);
    return ino;
}

/** The list and the inode counter are shared with the receive path, so
 * they change under `net_lock`. */
struct socket *socket_alloc(int type)
{
    socket_t *sock = kzalloc(sizeof(*sock));
    uint32_t flags;

    if (!sock) {
        klog(KLOG_WARN, "socket", "open: no memory for a socket");
        return NULL;
    }
    sock->in_use = 1;
    sock->refcount = 1;
    sock->type = type;
    sock->family = AF_INET;
    sock->ip_ttl = IP_TTL_DEFAULT;
    sock->hops6 = -1;
    sock->mhops6 = 1;
    sock->mloop6 = 1;
    sock->rcvbuf = SOCK_RCVBUF_DEFAULT;
    sock->pmtudisc = IP_PMTUDISC_WANT;
    sock->local_ip = sock->remote_ip = net_addr4(0);
    wait_queue_init(&sock->waiters);

    flags = net_lock();
    sock->inode = socket_next_inode();
    sock->next = sockets;
    sockets = sock;
    net_unlock(flags);
    return sock;
}

/* The namespace of the sockets whose network namespace has ended. */
static struct netns net_gone;

int socket_outside(const socket_t *s)
{
    return s->ns != NULL;
}

int socket_ns_reaches(const socket_t *s, net_addr_t a, int up)
{
    const struct netdev *lo = s->ns ? s->ns->lo : NULL;

    if (!s->ns) {
        return 1;
    }
    return lo && lo->ip && (lo->up || !up) && a.family == AF_INET && IP_IS_LOOPBACK(a.v4);
}

struct netdev *socket_iface6(const socket_t *s, int ifindex)
{
    return ifindex > 0 && !s->ns ? netdev_by_index(ifindex) : NULL;
}

void net_ns_gone(struct netns *ns)
{
    socket_t *s;
    uint32_t flags = net_lock();

    for (s = sockets; s; s = s->next) {
        if (s->ns == ns) {
            s->ns = &net_gone;
        }
    }
    tcp_ns_gone(ns, &net_gone);
    ip_loop_ns_gone(ns);
    net_unlock(flags);
    unix_ns_gone(ns);
}

int socket_port_bound(uint16_t port)
{
    const socket_t *s;

    for (s = sockets; s; s = s->next) {
        if (s->type == SOCK_STREAM && (s->family == AF_INET || s->family == AF_INET6) &&
            s->local_port == port) {
            return 1;
        }
    }
    return 0;
}

/** Whether `s` is one of the sockets of `family` and `type` asked for, or
 * with `type` 0 a socket of any family and type. */
static int socket_counted(const socket_t *s, int family, int type)
{
    return type == 0 || (s->type == type && s->family == family);
}

/** How many sockets of `family` and `type` there are, as `socket_counted`
 * takes them. */
static int socket_count_in(int family, int type)
{
    const socket_t *s;
    uint32_t flags = net_lock();
    int n = 0;

    for (s = sockets; s; s = s->next) {
        n += socket_counted(s, family, type);
    }
    net_unlock(flags);
    return n;
}

int socket_count(int type)
{
    return socket_count_in(AF_INET, type);
}

int socket_snapshot(int family, int type, struct socket_row **rows)
{
    const socket_t *s;
    struct socket_row *out;
    uint32_t flags;
    int n, room;

    *rows = NULL;
    room = socket_count_in(family, type);
    if (room == 0) {
        return 0;
    }
    out = kmalloc(sizeof(*out) * (size_t)room);
    if (!out) {
        return -1;
    }
    n = 0;
    flags = net_lock();
    for (s = sockets; s && n < room; s = s->next) {
        if (!socket_counted(s, family, type) ||
            s->ns != (net_ns_seen() ? NULL : (const struct netns *)proc_netns(current_proc))) {
            continue;
        }
        out[n].local_port = type == SOCK_RAW ? s->protocol : s->local_port;
        out[n].local_ip = s->local_ip;
        out[n].remote_ip = s->remote_ip;
        out[n].remote_port = s->remote_port;
        out[n].connected = s->connected;
        out[n].rxq = s->rq.bytes;
        out[n].uid = s->uid;
        out[n].inode = s->inode;
        n++;
    }
    net_unlock(flags);
    *rows = out;
    return n;
}

void socket_ref(struct socket *sock)
{
    uint32_t flags;

    if (sock) {
        flags = net_lock();
        sock->refcount++;
        net_unlock(flags);
    }
}

/**
 * The reference count and the list are shared with the receive path, so
 * they change under `net_lock`. A listener's queued connections are released
 * with the same call; each is a socket that is not listening, so the
 * recursion is one level deep, and `net_lock` nests.
 */
void socket_unref(struct socket *sock)
{
    uint32_t flags;

    if (!sock) {
        return;
    }

    flags = net_lock();

    if (sock->refcount > 0) {
        sock->refcount--;
    }
    if (sock->refcount > 0) {
        net_unlock(flags);
        return;
    }

    socket_leave_groups6(sock);
    if (sock->family == AF_NETLINK) {
        netlink_close(sock);
    } else if (sock->family == AF_PACKET) {
        packet_unbind(sock);
    } else if (sock->type == SOCK_RAW) {
        raw_unregister(sock);
    } else if (sock->type == SOCK_DGRAM && sock->local_port != 0) {
        udp_unbind(sock);
    } else if (sock->type == SOCK_STREAM && sock->tcb) {
        tcp_close(sock);
    }

    if (sock->type == SOCK_STREAM && sock->listening) {
        while (sock->accept_head != sock->accept_tail) {
            struct socket *conn = sock->accept_queue[sock->accept_head];
            sock->accept_head = (sock->accept_head + 1) & (SOCK_ACCEPT_BACKLOG - 1);
            socket_unref(conn);
        }
    }

    {
        socket_t **link = &sockets;

        while (*link && *link != sock) {
            link = &(*link)->next;
        }
        if (*link) {
            *link = sock->next;
        }
    }
    sock->in_use = 0;
    sock_dgq_flush(&sock->rq);
    net_unlock(flags);
    kfree(sock);
}

void socket_leave_groups6(struct socket *sock)
{
    int i;

    for (i = 0; i < sock->ngroups6; i++) {
        struct netdev *nd = socket_iface6(sock, sock->groups6[i].ifindex);

        if (nd) {
            netdev_leave6(nd, sock->groups6[i].group);
        }
    }
    sock->ngroups6 = 0;
}

void socket_udp_deliver(struct socket *sock, net_addr_t src_ip, uint16_t src_port,
                        const uint8_t *data, uint16_t len)
{
    socket_udp_deliver_info(sock, src_ip, src_port, data, len, NULL);
}

/** A connected socket takes datagrams from its peer and from nobody else,
 * which is what `connect` means for UDP. */
void socket_udp_deliver_info(struct socket *sock, net_addr_t src_ip, uint16_t src_port,
                             const uint8_t *data, uint16_t len, const struct net_rxinfo *info)
{
    struct sock_dgram meta;

    if (sock->connected &&
        (!net_addr_equal(src_ip, sock->remote_ip) || src_port != sock->remote_port)) {
        return;
    }
    k_memset(&meta, 0, sizeof(meta));
    meta.src = src_ip;
    meta.port = src_port;
    if (info) {
        meta.info = *info;
    } else {
        meta.info.dst = net_addr4(0);
        meta.info.hlim = -1;
        meta.info.tclass = -1;
    }
    if (!sock_dgq_put(&sock->rq, sock->rcvbuf, &meta, data, len)) {
        if (src_ip.family == AF_INET6) {
            net_mib6.udp6_in_errors++;
        } else {
            net_mib.udp_in_errors++;
            net_mib.udp_rcvbuf_errors++;
        }
        return;
    }
    wq_wake(&sock->waiters, WQ_DATA);
}

/** The wait predicate of a datagram receive: a datagram waits, whatever the
 * family, or a UDP socket has an error pending. */
static int udp_recv_ready(void *arg)
{
    socket_t *sock = (socket_t *)arg;

    if (sock->family == AF_NETLINK) {
        return netlink_ready(sock) || sock->so_error != 0;
    }
    return sock->rq.count != 0 || sock->so_error != 0;
}

/** The wait predicate of a stream receive: bytes are buffered, the FIN
 * came, the connection is closed, the socket was shut for receiving, or it
 * has no block, which a sharer's shutdown in SYN_SENT freed during the
 * wait. */
static int tcp_recv_ready(void *arg)
{
    socket_t *sock = (socket_t *)arg;
    struct tcb *tcb = sock->tcb;
    if (!tcb || (sock->shutdown & SOCK_RCV_SHUTDOWN)) {
        return 1;
    }
    return (tcb->recv_tail - tcb->recv_head) != 0 || tcb->fin_received || tcb->state == TCP_CLOSED;
}

int socket_dgram_error(struct socket *sock)
{
    uint32_t flags = net_lock();
    int err = sock->so_error;

    sock->so_error = 0;
    net_unlock(flags);
    return -err;
}

int socket_dgram_nowait(struct socket *sock)
{
    int rc = socket_dgram_error(sock);

    if (rc == 0 && socket_dgram_pending(sock) < 0) {
        rc = -EAGAIN;
    }
    return rc;
}

/** An error pending goes before any datagram, as Linux's receive reports
 * it first, and is looked for again after the wait. */
int socket_dgram_wait(struct socket *sock)
{
    int rc = socket_dgram_error(sock);

    if (rc == 0) {
        rc = wq_wait_event(&sock->waiters, udp_recv_ready, sock);
    }
    if (rc == 0) {
        rc = socket_dgram_error(sock);
    }
    return rc;
}

/** Occupancy is the queue's count and not a length, since a datagram of no
 * bytes is a datagram; a caller that read 0 as emptiness would block forever
 * on one. */
int socket_dgram_pending(struct socket *sock)
{
    int pending;
    uint32_t flags;

    if (sock->family == AF_NETLINK) {
        return netlink_pending(sock);
    }
    flags = net_lock();
    pending = sock->rq.head ? (int)sock->rq.head->len : -1;
    net_unlock(flags);

    return pending;
}

/** A copy that starts at or past the end returns 0, which ends the walk of
 * a caller that fills several destinations in order from the one message.
 * The queue is read under `net_lock`, so the receive interrupt does not
 * change it during the copy. */
int socket_dgram_peek(struct socket *sock, uint32_t offset, uint8_t *buf, int len)
{
    int copy_len;
    uint32_t flags;

    if (len < 0) {
        return 0;
    }
    if (sock->family == AF_NETLINK) {
        return netlink_peek(sock, offset, buf, len);
    }
    flags = net_lock();
    copy_len = sock_dgq_peek(&sock->rq, offset, buf, len);
    net_unlock(flags);

    return copy_len;
}

void socket_dgram_consume(struct socket *sock, net_addr_t *src_ip, uint16_t *src_port)
{
    struct sock_dgram *d;
    uint32_t flags;

    if (sock->family == AF_PACKET || sock->family == AF_NETLINK) {
        if (src_ip) {
            *src_ip = net_addr4(0);
        }
        if (src_port) {
            *src_port = 0;
        }
        if (sock->family == AF_NETLINK) {
            netlink_consume(sock);
        } else {
            packet_consume(sock);
            sock->rx_any = 1;
        }
        return;
    }
    if (sock->type == SOCK_RAW) {
        raw_consume(sock);
        raw_last_info(sock, &sock->rx_last);
        sock->rx_any = 1;
        if (src_ip) {
            raw_last_address(sock, src_ip);
        }
        if (src_port) {
            *src_port = 0;
        }
        return;
    }

    flags = net_lock();
    d = sock_dgq_pop(&sock->rq);
    if (!d) {
        net_unlock(flags);
        return;
    }
    if (src_ip) {
        *src_ip = d->src;
    }
    if (src_port) {
        *src_port = d->port;
    }
    sock->rx_last = d->info;
    sock->rx_any = 1;
    net_unlock(flags);
    kfree(d);
}

void socket_dgram_sender(struct socket *sock, net_addr_t *src_ip, uint16_t *src_port)
{
    net_addr_t ip = net_addr4(0);
    uint32_t flags;
    uint16_t port = 0;

    if (sock->family == AF_PACKET) {
        packet_note_head(sock);
        sock->rx_any = 1;
    } else if (sock->family != AF_NETLINK) {
        if (sock->type == SOCK_RAW) {
            raw_head_address(sock, &ip);
            raw_head_info(sock, &sock->rx_last);
            sock->rx_any = 1;
        } else {
            flags = net_lock();
            if (sock->rq.head) {
                ip = sock->rq.head->src;
                port = sock->rq.head->port;
                sock->rx_last = sock->rq.head->info;
                sock->rx_any = 1;
            }
            net_unlock(flags);
        }
    }
    if (src_ip) {
        *src_ip = ip;
    }
    if (src_port) {
        *src_port = port;
    }
}

/** Take the datagram `socket_dgram_peek` copied, or with SOCK_RECV_PEEK in
 * `how` leave it waiting, and report its sender either way. Returns what
 * the receive answers: `copied`, or with SOCK_RECV_TRUNC the datagram's
 * whole length. The caller holds `net_lock`. */
static int socket_dgram_take(struct socket *sock, net_addr_t *src_ip, uint16_t *src_port, int how,
                             int copied)
{
    if ((how & SOCK_RECV_TRUNC) && sock->family == AF_NETLINK) {
        copied = netlink_pending(sock);
    } else if (how & SOCK_RECV_TRUNC) {
        copied = sock->rq.head ? (int)sock->rq.head->len : copied;
    }
    if (how & SOCK_RECV_PEEK) {
        socket_dgram_sender(sock, src_ip, src_port);
    } else {
        socket_dgram_consume(sock, src_ip, src_port);
    }
    return copied;
}

/** The copy and the take run under one hold of `net_lock`, as socket.h
 * requires of the three steps; the two calls take the lock again inside,
 * which nests. The buffer is brought in again after the wait, which another
 * thread's munmap may have outlasted, and before the lock, since that can
 * sleep too; a datagram another reader took meanwhile sends this one back to
 * wait for the next. */
int socket_recv(struct socket *sock, uint8_t *buf, int len, net_addr_t *src_ip, uint16_t *src_port,
                int how)
{
    int copy_len;
    int rc;
    uint32_t flags;

    for (;;) {
        rc = socket_dgram_wait(sock);
        if (rc < 0) {
            return rc;
        }
        if (!vm_prefault_range((uintptr_t)buf, (uintptr_t)len, 1)) {
            return -EFAULT;
        }
        flags = net_lock();
        copy_len = socket_dgram_peek(sock, 0, buf, len);
        if (copy_len >= 0) {
            break;
        }
        net_unlock(flags);
    }
    if (sock->family == AF_NETLINK) {
        netlink_note_recv(sock, (uint32_t)len);
    }
    copy_len = socket_dgram_take(sock, src_ip, src_port, how, copy_len);
    net_unlock(flags);

    return copy_len;
}

int socket_try_recv(struct socket *sock, uint8_t *buf, int len, net_addr_t *src_ip,
                    uint16_t *src_port, int how)
{
    int copy_len;
    uint32_t flags;

    copy_len = socket_dgram_error(sock);
    if (copy_len < 0) {
        return copy_len;
    }
    flags = net_lock();
    copy_len = socket_dgram_peek(sock, 0, buf, len);
    if (copy_len < 0) {
        net_unlock(flags);

        return SOCK_EAGAIN;
    }
    if (sock->family == AF_NETLINK) {
        netlink_note_recv(sock, (uint32_t)len);
    }
    copy_len = socket_dgram_take(sock, src_ip, src_port, how, copy_len);
    net_unlock(flags);

    return copy_len;
}

/**
 * The accept queue is a ring whose size is a power of two, and its indexes
 * wrap by a mask. `tcp_rx` answers a SYN with a reset while the queue is
 * full, so a full queue here meets a connection that completed after the
 * queue filled; `socket_unref` closes it with a FIN.
 */
void socket_tcp_accept(struct socket *listen_sock, struct socket *conn_sock)
{
    int next_tail;

    if (!listen_sock || !conn_sock)
        return;

    next_tail = (listen_sock->accept_tail + 1) & (SOCK_ACCEPT_BACKLOG - 1);
    if (next_tail == listen_sock->accept_head) {
        klog(KLOG_WARN, "socket", "accept: the queue is full, refused");
        socket_unref(conn_sock);
        return;
    }

    listen_sock->accept_queue[listen_sock->accept_tail] = conn_sock;
    listen_sock->accept_tail = next_tail;

    wq_wake_all(&listen_sock->waiters);
}

void socket_tcp_connected(struct socket *sock)
{
    if (!sock)
        return;

    wq_wake_all(&sock->waiters);
}

void socket_tcp_writable(struct socket *sock)
{
    if (!sock)
        return;

    wq_wake(&sock->waiters, WQ_ROOM);
}

/** The wait predicate of a stream send: the send ring has room, or the
 * connection has left a sending state or was shut down for writing, so
 * that a blocked sender stops waiting and reports it, as Linux's
 * sk_stream_wait_memory stops at SEND_SHUTDOWN. */
static int send_space_ready(void *arg)
{
    struct socket *sock = (struct socket *)arg;
    struct tcb *tcb = sock->tcb;

    if (!tcb || tcb->fin_pending ||
        (tcb->state != TCP_ESTABLISHED && tcb->state != TCP_CLOSE_WAIT)) {
        return 1;
    }
    return (TCP_RING_SIZE - (tcb->send_tail - tcb->send_ack)) > 0;
}

/** The wait predicate of a send on a connection still in its handshake:
 * the connection has left SYN_SENT and SYN_RCVD, or has an error, as
 * Linux's sk_stream_wait_connect (net/core/stream.c:75-78) waits. */
static int send_connect_ready(void *arg)
{
    struct socket *sock = (struct socket *)arg;
    struct tcb *tcb = sock->tcb;

    return !tcb || tcb->so_error != 0 || (tcb->state != TCP_SYN_SENT && tcb->state != TCP_SYN_RCVD);
}

/**
 * What a read of the stream socket `sock`, which has no block, answers:
 * the error a shutdown in SYN_SENT recorded, ECONNRESET, once, and then
 * -ENOTCONN, as Linux's tcp_recvmsg gives sk_err before TCP_CLOSE's
 * ENOTCONN (net/ipv4/tcp.c:2687-2700).
 */
static int tcp_unconnected_error(struct socket *sock)
{
    int err = socket_dgram_error(sock);

    return err != 0 ? err : -ENOTCONN;
}

/**
 * The reason a connection ended, as a negative errno, or `fallback` when
 * none was recorded.
 *
 * Whatever ended the connection put the reason in the block's `so_error`,
 * and a reader or a writer of a connection that is gone is given it; a bare
 * -1 would reach the program as `EPERM`. The reason is cleared once given. A
 * block with no reason answers `fallback`, the errno the caller gives for
 * its own case: `-EPIPE` for a sender, `-ECONNRESET` for a reader.
 */
static int tcp_dead_error(struct tcb *tcb, int fallback)
{
    if (tcb && tcb->so_error != 0) {
        int err = (int)tcb->so_error;

        tcb->so_error = 0;
        return -err;
    }
    return fallback;
}

/**
 * `tcp_send` copies what fits without waiting: 0 for a full ring,
 * TCP_SEND_CONNECTING for a connection still in its handshake, and -1 for a
 * block that is gone or not sending. A blocking socket sleeps for room,
 * which `socket_tcp_writable` wakes it for when an ACK frees some, so a
 * nonzero request never returns 0, which a caller would take for the end of
 * input. A send on a connection still in its handshake fails with its
 * pending error if it has one, and otherwise with EAGAIN when nonblocking,
 * or sleeps until the handshake ends and sends then, as Linux's
 * tcp_sendmsg_locked (net/ipv4/tcp.c:1106-1111) has sk_stream_wait_connect
 * (net/core/stream.c:62-82) do; EPIPE comes only from the other states. The
 * buffer is brought in again after each sleep, since `tcp_send` reads it
 * with interrupts off and another thread may have unmapped it.
 */
int socket_tcp_send(struct socket *sock, const uint8_t *buf, int len, int nonblock)
{
    if (!sock) {
        return -1;
    }
    for (;;) {
        int rc, sent = tcp_send(sock, buf, len);
        if (sent == TCP_SEND_CONNECTING) {
            if (sock->tcb && sock->tcb->so_error != 0) {
                return tcp_dead_error(sock->tcb, -EPIPE);
            }
            if (nonblock) {
                return SOCK_EAGAIN;
            }
            rc = wq_wait_event(&sock->waiters, send_connect_ready, sock);
            if (rc < 0) {
                return rc;
            }
            if (!vm_prefault_range((uintptr_t)buf, (uintptr_t)len, 0)) {
                return -EFAULT;
            }
            continue;
        }
        if (sent == -1) {
            return tcp_dead_error(sock->tcb, -EPIPE);
        }
        if (sent != 0 || len == 0) {
            return sent;
        }
        if (!sock->tcb ||
            (sock->tcb->state != TCP_ESTABLISHED && sock->tcb->state != TCP_CLOSE_WAIT)) {
            return tcp_dead_error(sock->tcb, -EPIPE);
        }
        if (nonblock) {
            return SOCK_EAGAIN;
        }
        rc = wq_wait_event(&sock->waiters, send_space_ready, sock);
        if (rc < 0) {
            return rc;
        }
        if (!vm_prefault_range((uintptr_t)buf, (uintptr_t)len, 0)) {
            return -EFAULT;
        }
    }
}

/* The receive ring's copies, in at most two calls each: the ring's size is a
 * power of two and its index wraps through a mask, so a run that crosses the
 * end is two straight runs. */
static void recv_ring_put(struct tcb *tcb, uint32_t at, const uint8_t *src, uint32_t n)
{
    uint32_t head = at & (tcb->recv_size - 1);
    uint32_t first = tcb->recv_size - head;

    if (first > n) {
        first = n;
    }
    k_memcpy(tcb->recv_buf + head, src, first);
    if (n > first) {
        k_memcpy(tcb->recv_buf, src + first, n - first);
    }
}

static void recv_ring_get(uint8_t *dst, struct tcb *tcb, uint32_t at, uint32_t n)
{
    uint32_t head = at & (tcb->recv_size - 1);
    uint32_t first = tcb->recv_size - head;

    if (first > n) {
        first = n;
    }
    k_memcpy(dst, tcb->recv_buf + head, first);
    if (n > first) {
        k_memcpy(dst + first, tcb->recv_buf, n - first);
    }
}

/** The table of receive-buffer ends of `tcb`, the heap's or the block's
 * own, with its entries in `*cap`. */
static uint32_t *recv_seg_table(struct tcb *tcb, uint32_t *cap)
{
    if (tcb->recv_seg_heap) {
        *cap = tcb->recv_seg_cap;
        return tcb->recv_seg_heap;
    }
    *cap = TCP_RECV_SEGS;
    return tcb->recv_seg_end;
}

/** The end of the `i`th oldest receive buffer of `tcb`. */
static uint32_t recv_seg_at(struct tcb *tcb, uint32_t i)
{
    uint32_t cap;
    const uint32_t *table = recv_seg_table(tcb, &cap);

    return table[(tcb->recv_seg_first + i) % cap];
}

/** Drop the ends of the receive buffers of `tcb` that `recv_head` has
 * passed, and give back the heap's table once none is left. With
 * interrupts off. */
static void recv_seg_trim(struct tcb *tcb)
{
    uint32_t cap;

    (void)recv_seg_table(tcb, &cap);
    while (tcb->recv_seg_count > 0 && (int32_t)(recv_seg_at(tcb, 0) - tcb->recv_head) <= 0) {
        tcb->recv_seg_first = (tcb->recv_seg_first + 1u) % cap;
        tcb->recv_seg_count--;
    }
    if (tcb->recv_seg_count == 0 && tcb->recv_seg_heap) {
        kfree(tcb->recv_seg_heap);
        tcb->recv_seg_heap = NULL;
        tcb->recv_seg_cap = 0;
        tcb->recv_seg_first = 0;
    }
}

/** Double the table of receive-buffer ends of `tcb` on the heap, its
 * entries kept in order from the first; refused when the table would take
 * more bytes than the receive ring, or the heap has no room. Returns 1, or
 * 0 when refused. With interrupts off, as the heap allows. */
static int recv_seg_grow(struct tcb *tcb)
{
    uint32_t cap, i, *bigger;
    uint32_t *table = recv_seg_table(tcb, &cap);

    if (cap > tcb->recv_size / (2u * (uint32_t)sizeof(uint32_t))) {
        return 0;
    }
    bigger = kmalloc((size_t)cap * 2u * sizeof(uint32_t));
    if (!bigger) {
        return 0;
    }
    for (i = 0; i < tcb->recv_seg_count; i++) {
        bigger[i] = table[(tcb->recv_seg_first + i) % cap];
    }
    if (tcb->recv_seg_heap) {
        kfree(tcb->recv_seg_heap);
    }
    tcb->recv_seg_heap = bigger;
    tcb->recv_seg_cap = cap * 2u;
    tcb->recv_seg_first = 0;
    return 1;
}

/** End a receive buffer of `tcb` at `recv_tail`, in a table grown when it
 * is full, or move the last one's end there when it cannot grow. With
 * interrupts off. */
static void recv_seg_add(struct tcb *tcb)
{
    uint32_t cap, *table;

    recv_seg_trim(tcb);
    table = recv_seg_table(tcb, &cap);
    if (tcb->recv_seg_count == cap) {
        if (!recv_seg_grow(tcb)) {
            table[(tcb->recv_seg_first + cap - 1u) % cap] = tcb->recv_tail;
            return;
        }
        table = recv_seg_table(tcb, &cap);
    }
    table[(tcb->recv_seg_first + tcb->recv_seg_count) % cap] = tcb->recv_tail;
    tcb->recv_seg_count++;
}

/**
 * `recv_head` and `recv_tail` are free-running counts, masked to the ring's
 * size where they index it. The wake is for data, and comes only when bytes
 * were taken: a full ring takes none, and an edge-triggered registration for
 * input would be told of data with nothing new to read.
 */
uint16_t socket_tcp_deliver(struct socket *sock, const uint8_t *data, uint16_t len)
{
    struct tcb *tcb;
    uint32_t free_space, copy_len;

    if (!sock || !sock->tcb)
        return 0;
    tcb = sock->tcb;

    free_space = tcb->recv_size - (tcb->recv_tail - tcb->recv_head);
    copy_len = (uint32_t)len;
    if (copy_len > free_space)
        copy_len = free_space;

    recv_ring_put(tcb, tcb->recv_tail, data, copy_len);
    tcb->recv_tail += copy_len;

    if (copy_len > 0) {
        recv_seg_add(tcb);
        wq_wake(&sock->waiters, WQ_DATA);
    }
    return (uint16_t)copy_len;
}

/** The bytes go where the stream puts them once the hole is filled. Data
 * that fills the hole later writes over them only with the same bytes, since
 * a byte of a stream has one value. */
uint16_t socket_tcp_hold(struct socket *sock, uint32_t offset, const uint8_t *data, uint16_t len)
{
    struct tcb *tcb;
    uint32_t free_space, copy_len;

    if (!sock || !sock->tcb)
        return 0;
    tcb = sock->tcb;

    free_space = tcb->recv_size - (tcb->recv_tail - tcb->recv_head);
    if (offset >= free_space)
        return 0;
    copy_len = (uint32_t)len;
    if (copy_len > free_space - offset)
        copy_len = free_space - offset;

    recv_ring_put(tcb, tcb->recv_tail + offset, data, copy_len);
    return (uint16_t)copy_len;
}

void socket_tcp_release(struct socket *sock, uint32_t n)
{
    if (!sock || !sock->tcb || n == 0)
        return;
    sock->tcb->recv_tail += n;
    recv_seg_add(sock->tcb);
    wq_wake(&sock->waiters, WQ_DATA);
}

/** A read of an empty stream shut for receiving is the end of input at
 * once, as Linux's tcp_recvmsg ends at RCV_SHUTDOWN (net/ipv4/tcp.c:2692),
 * while bytes buffered before or after the shutdown are still read. A
 * sharer's shutdown in SYN_SENT may free the block during the wait
 * (`tcp_unconnected_error`). The buffer is brought in again after the
 * wait, which another thread's munmap may have outlasted; that can sleep
 * too, so the ring is judged again after it, and bytes another reader took
 * meanwhile send this one back to the wait instead of reading as the end
 * of input. */
int socket_tcp_recv(struct socket *sock, uint8_t *buf, int len, int peek)
{
    struct tcb *tcb;
    uint32_t avail, copy_len;
    int rc;

    if (!sock)
        return -1;
    if (!sock->tcb)
        return tcp_unconnected_error(sock);
    if (sock->tcb->state == TCP_LISTEN)
        return -ENOTCONN;

    for (;;) {
        rc = wq_wait_event(&sock->waiters, tcp_recv_ready, sock);
        if (rc < 0) {
            return rc;
        }
        if (!vm_prefault_range((uintptr_t)buf, (uintptr_t)len, 1)) {
            return -EFAULT;
        }
        if (tcp_recv_ready(sock)) {
            break;
        }
    }
    tcb = sock->tcb;
    if (!tcb) {
        return tcp_unconnected_error(sock);
    }

    avail = tcb->recv_tail - tcb->recv_head;
    if (avail == 0 && !tcb->fin_received && tcb->state == TCP_CLOSED) {
        return tcp_dead_error(tcb, -ECONNRESET);
    }
    if (avail == 0)
        return 0;

    copy_len = avail;
    if (copy_len > (uint32_t)len)
        copy_len = (uint32_t)len;

    recv_ring_get(buf, tcb, tcb->recv_head, copy_len);
    if (!peek) {
        tcb->recv_head += copy_len;
        tcp_window_update(sock);
    }
    return (int)copy_len;
}

/**
 * Read queued stream bytes without waiting, leaving them queued for a peek.
 * Return SOCK_EAGAIN without input, 0 at the end of input or when shut for
 * receiving, or -ENOTCONN on a listener.
 */
int socket_tcp_try_recv(struct socket *sock, uint8_t *buf, int len, int peek)
{
    struct tcb *tcb;
    uint32_t avail, copy_len;

    if (!sock)
        return -1;
    if (!sock->tcb)
        return tcp_unconnected_error(sock);
    tcb = sock->tcb;
    if (tcb->state == TCP_LISTEN)
        return -ENOTCONN;

    avail = tcb->recv_tail - tcb->recv_head;
    if (avail == 0) {
        if (tcb->fin_received || (sock->shutdown & SOCK_RCV_SHUTDOWN))
            return 0;
        if (tcb->state == TCP_CLOSED)
            return tcp_dead_error(tcb, -ECONNRESET);
        return SOCK_EAGAIN;
    }

    copy_len = avail;
    if (copy_len > (uint32_t)len)
        copy_len = (uint32_t)len;
    recv_ring_get(buf, tcb, tcb->recv_head, copy_len);
    if (!peek) {
        tcb->recv_head += copy_len;
        tcp_window_update(sock);
    }
    return (int)copy_len;
}

/**
 * Copy up to `len` queued stream bytes that lie `off` bytes past the next
 * unread one into `buf`, leaving every byte queued: the second and later
 * buffers of a recvmsg(2) with MSG_PEEK. Returns the bytes copied, 0 when
 * none lie past `off` or the socket has no connection. Never waits.
 */
int socket_tcp_peek_at(struct socket *sock, uint32_t off, uint8_t *buf, int len)
{
    struct tcb *tcb;
    uint32_t avail, copy_len;

    if (!sock || !sock->tcb || len <= 0)
        return 0;
    tcb = sock->tcb;
    if (tcb->state == TCP_LISTEN)
        return 0;
    avail = tcb->recv_tail - tcb->recv_head;
    if (avail <= off)
        return 0;
    copy_len = avail - off;
    if (copy_len > (uint32_t)len)
        copy_len = (uint32_t)len;
    recv_ring_get(buf, tcb, tcb->recv_head + off, copy_len);
    return (int)copy_len;
}

uint32_t socket_tcp_queued(const struct socket *sock)
{
    if (!sock || !sock->tcb || sock->tcb->state == TCP_LISTEN)
        return 0;
    return sock->tcb->recv_tail - sock->tcb->recv_head;
}

/** The receive buffers are read under `net_lock`, since the receive path
 * adds to them with interrupts off. A byte past every recorded end, which
 * the table's invariant rules out, is counted to the tail. */
uint32_t socket_tcp_buffer(struct socket *sock, uint32_t off)
{
    struct tcb *tcb;
    uint32_t flags, at, n = 0, i;

    if (!sock)
        return 0;
    flags = net_lock();
    tcb = sock->tcb;
    if (tcb && tcb->state != TCP_LISTEN && tcb->recv_tail - tcb->recv_head > off) {
        at = tcb->recv_head + off;
        n = tcb->recv_tail - at;
        recv_seg_trim(tcb);
        for (i = 0; i < tcb->recv_seg_count; i++) {
            uint32_t end = recv_seg_at(tcb, i);

            if ((int32_t)(end - at) > 0) {
                n = end - at;
                break;
            }
        }
    }
    net_unlock(flags);
    return n;
}

/* What `socket_tcp_wait_more` waits on: the socket, and the bytes queued
 * that do not count. */
struct tcp_more {
    socket_t *sock;
    uint32_t have;
};

/** The wait predicate of `socket_tcp_wait_more`: more than `have` bytes are
 * buffered, or the input ended, the socket was shut for receiving, the
 * connection closed or has an error, or the socket has no block. */
static int tcp_more_ready(void *arg)
{
    const struct tcp_more *m = (const struct tcp_more *)arg;
    struct tcb *tcb = m->sock->tcb;

    if (!tcb || (m->sock->shutdown & SOCK_RCV_SHUTDOWN)) {
        return 1;
    }
    return tcb->recv_tail - tcb->recv_head > m->have || tcb->fin_received ||
           tcb->state == TCP_CLOSED || tcb->so_error != 0;
}

int socket_tcp_wait_more(struct socket *sock, uint32_t have)
{
    struct tcp_more m;
    int rc;

    if (!sock || !sock->tcb || sock->tcb->state == TCP_LISTEN)
        return 0;
    m.sock = sock;
    m.have = have;
    rc = wq_wait_event(&sock->waiters, tcp_more_ready, &m);
    if (rc < 0) {
        return rc;
    }
    return sock->tcb && sock->tcb->recv_tail - sock->tcb->recv_head > have;
}

void socket_tcp_fin(struct socket *sock)
{
    if (!sock)
        return;

    wq_wake_all(&sock->waiters);
}

/**
 * The query descriptor, a raw socket with no protocol, has only ioctls: it
 * answers POLLERR at once, so a poller does not wait on it forever. A raw,
 * packet or netlink socket polls like a datagram socket, on its own queue,
 * which is what a DHCP client and `ping` poll before they read, and a
 * datagram socket can always send.
 *
 * A listener is ready for input when a connection waits to be accepted. A
 * stream with no block, never connected or disconnected by a shutdown in
 * SYN_SENT, is Linux's TCP_CLOSE: POLLHUP and POLLOUT, and POLLERR while
 * an error waits. A connected stream answers as Linux's tcp_poll does
 * (net/ipv4/tcp.c:559-596), with the halves shut counted as its
 * sk_shutdown counts them: the receiving half by SHUT_RD or the peer's
 * FIN, both by a closed or reset connection. A receiving half shut adds
 * POLLIN and POLLRDHUP, since a receive answers at once, and both halves
 * shut, or a closed connection, add POLLHUP. Bytes buffered add POLLIN. A
 * sending half shut adds POLLOUT, since a send fails at once; otherwise
 * POLLOUT comes only in a sending state with send-ring room free, so a
 * poll-then-send loop blocks instead of spinning on a full ring. A
 * recorded error adds POLLERR and POLLOUT, so a non-blocking connect
 * polled for POLLOUT wakes and reads SO_ERROR.
 */
short socket_poll(struct socket *sock, short events, struct wait_queue **wq)
{
    short revents = 0;

    if (!sock) {
        return POLLNVAL;
    }
    if (wq) {
        *wq = &sock->waiters;
    }

    if (sock->type == SOCK_RAW && sock->protocol == 0) {
        return (short)POLLERR;
    }

    if (sock->type == SOCK_DGRAM || sock->type == SOCK_RAW) {
        if (sock->family == AF_NETLINK ? netlink_ready(sock) : sock->rq.count != 0) {
            revents |= (short)(events & POLLIN);
        }
        revents |= (short)(events & POLLOUT);
        if (sock->so_error != 0) {
            revents |= POLLERR;
        }
        return revents;
    }

    if (sock->listening) {
        if (sock->accept_head != sock->accept_tail) {
            revents |= (short)(events & POLLIN);
        }
        return revents;
    }
    if (!sock->tcb) {
        revents = (short)(POLLHUP | (events & POLLOUT));
        if (sock->so_error != 0) {
            revents |= POLLERR;
        }
        return revents;
    }
    {
        struct tcb *tcb = sock->tcb;
        int shut = sock->shutdown;

        if (tcb->fin_received) {
            shut |= SOCK_RCV_SHUTDOWN;
        }
        if (tcb->state == TCP_CLOSED) {
            shut = SOCK_RCV_SHUTDOWN | SOCK_SEND_SHUTDOWN;
        }
        if (shut == (SOCK_RCV_SHUTDOWN | SOCK_SEND_SHUTDOWN)) {
            revents |= POLLHUP;
        }
        if (shut & SOCK_RCV_SHUTDOWN) {
            revents |= (short)(events & (POLLIN | POLLRDHUP));
        }
        if ((tcb->recv_tail - tcb->recv_head) != 0) {
            revents |= (short)(events & POLLIN);
        }
        if (shut & SOCK_SEND_SHUTDOWN) {
            revents |= (short)(events & POLLOUT);
        } else if ((tcb->state == TCP_ESTABLISHED || tcb->state == TCP_CLOSE_WAIT) &&
                   (TCP_RING_SIZE - (tcb->send_tail - tcb->send_ack)) > 0) {
            revents |= (short)(events & POLLOUT);
        }
        if (tcb->so_error != 0) {
            revents |= POLLERR;
            revents |= (short)(events & POLLOUT);
        }
    }
    return revents;
}

/** Match a canonical destination against a socket's bound address. */
int socket_address_matches(const struct socket *sock, net_addr_t dst)
{
    if (sock->ns != ip_ns)
        return 0;
    if (dst.family == AF_INET6 && sock->family != AF_INET6)
        return 0;
    if (dst.family == AF_INET && sock->family == AF_INET6 && sock->v6only)
        return 0;
    if (net_addr_equal(sock->local_ip, dst))
        return 1;
    if (!net_addr_any(sock->local_ip))
        return 0;
    return sock->local_ip.family == dst.family ||
           (sock->family == AF_INET6 && !sock->v6only && sock->local_ip.family == AF_INET6);
}

/** Test overlap in the address sets of two bindings, including dual stack. */
int socket_bind_conflict(const struct socket *a, const struct socket *b)
{
    if (a->ns != b->ns)
        return 0;
    if (a->local_ip.family == b->local_ip.family)
        return net_addr_any(a->local_ip) || net_addr_any(b->local_ip) ||
               net_addr_equal(a->local_ip, b->local_ip);
    return (a->family == AF_INET6 && !a->v6only && a->local_ip.family == AF_INET6 &&
            net_addr_any(a->local_ip)) ||
           (b->family == AF_INET6 && !b->v6only && b->local_ip.family == AF_INET6 &&
            net_addr_any(b->local_ip));
}

/** Check every live bound stream, including one not yet listening. */
int socket_stream_conflict(const struct socket *sock, uint16_t port)
{
    const struct socket *s;
    for (s = sockets; s; s = s->next) {
        if (s != sock && s->type == SOCK_STREAM && s->family != sock->family &&
            s->local_port == port && socket_bind_conflict(sock, s))
            return 1;
    }
    return 0;
}
