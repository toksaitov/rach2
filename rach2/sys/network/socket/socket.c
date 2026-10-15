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

void net_ns_gone(struct netns *ns)
{
    socket_t *s;
    uint32_t flags = net_lock();

    for (s = sockets; s; s = s->next) {
        if (s->ns == ns) {
            s->ns = &net_gone;
        }
    }
    net_unlock(flags);
    unix_ns_gone(ns);
}

int socket_port_bound(uint16_t port)
{
    const socket_t *s;

    for (s = sockets; s; s = s->next) {
        if (s->type == SOCK_STREAM && s->family == AF_INET && s->local_port == port) {
            return 1;
        }
    }
    return 0;
}

/** Whether `s` is one of the sockets `type` asks for, as `socket_count` takes
 * it. */
static int socket_counted(const socket_t *s, int type)
{
    return type == 0 || (s->type == type && s->family == AF_INET);
}

int socket_count(int type)
{
    const socket_t *s;
    uint32_t flags = net_lock();
    int n = 0;

    for (s = sockets; s; s = s->next) {
        n += socket_counted(s, type);
    }
    net_unlock(flags);
    return n;
}

int socket_snapshot(int type, struct socket_row **rows)
{
    const socket_t *s;
    struct socket_row *out;
    uint32_t flags;
    int n, room, i;

    *rows = NULL;
    room = socket_count(type);
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
        if (!socket_counted(s, type) ||
            s->ns != (net_ns_seen() ? NULL : (const struct netns *)proc_netns(current_proc))) {
            continue;
        }
        out[n].local_port = type == SOCK_RAW ? s->protocol : s->local_port;
        out[n].local_ip = s->local_ip;
        out[n].remote_ip = s->remote_ip;
        out[n].remote_port = s->remote_port;
        out[n].connected = s->connected;
        out[n].rxq = 0;
        for (i = 0; i < s->recv_count; i++) {
            out[n].rxq += s->recv_len[(s->recv_head + i) % SOCK_DGRAM_SLOTS];
        }
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
    net_unlock(flags);
    kfree(sock);
}

/** A connected socket takes datagrams from its peer and from nobody else,
 * which is what `connect` means for UDP. */
void socket_udp_deliver(struct socket *sock, uint32_t src_ip, uint16_t src_port,
                        const uint8_t *data, uint16_t len)
{
    uint16_t copy_len = len;
    int slot;

    if (sock->connected && (src_ip != sock->remote_ip || src_port != sock->remote_port)) {
        return;
    }
    if (sock->recv_count == SOCK_DGRAM_SLOTS) {
        return;
    }

    if (copy_len > SOCK_RECV_BUF_SIZE) {
        copy_len = SOCK_RECV_BUF_SIZE;
    }

    slot = (sock->recv_head + sock->recv_count) % SOCK_DGRAM_SLOTS;
    k_memcpy(sock->recv_buf[slot], data, copy_len);
    sock->recv_len[slot] = copy_len;
    sock->recv_src_ip[slot] = src_ip;
    sock->recv_src_port[slot] = src_port;
    sock->recv_count++;

    wq_wake(&sock->waiters, WQ_DATA);
}

/** The wait predicate of a datagram receive: a datagram waits, whatever the
 * family, or a UDP socket has an error pending. */
static int udp_recv_ready(void *arg)
{
    socket_t *sock = (socket_t *)arg;

    if (sock->family == AF_NETLINK) {
        return netlink_ready(sock);
    }
    if (sock->family == AF_PACKET) {
        return packet_ready(sock);
    }
    if (sock->type == SOCK_RAW) {
        return raw_ready(sock);
    }
    return sock->recv_count != 0 || sock->so_error != 0;
}

/** The wait predicate of a stream receive: bytes are buffered, the FIN
 * came, the connection is closed, or the socket has no block, which a
 * sharer's shutdown detached during the wait. */
static int tcp_recv_ready(void *arg)
{
    socket_t *sock = (socket_t *)arg;
    struct tcb *tcb = sock->tcb;
    if (!tcb) {
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

/** Occupancy is `recv_count` and not a length, since a datagram of no bytes
 * is a datagram; a caller that read 0 as emptiness would block forever on
 * one. */
int socket_dgram_pending(struct socket *sock)
{
    int pending;
    uint32_t flags;

    if (sock->family == AF_NETLINK) {
        return netlink_pending(sock);
    }
    if (sock->family == AF_PACKET) {
        return packet_pending(sock);
    }
    if (sock->type == SOCK_RAW) {
        return raw_pending(sock);
    }

    flags = net_lock();
    pending = sock->recv_count ? (int)sock->recv_len[sock->recv_head] : -1;
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
    int head;
    uint32_t flags;

    if (len < 0) {
        return 0;
    }
    if (sock->family == AF_NETLINK) {
        return netlink_peek(sock, offset, buf, len);
    }
    if (sock->family == AF_PACKET) {
        return packet_peek(sock, offset, buf, len);
    }
    if (sock->type == SOCK_RAW) {
        return raw_peek(sock, offset, buf, len);
    }

    flags = net_lock();
    if (!sock->recv_count) {
        net_unlock(flags);

        return -1;
    }
    head = sock->recv_head;
    if (offset >= (uint32_t)sock->recv_len[head]) {
        net_unlock(flags);

        return 0;
    }
    copy_len = (int)((uint32_t)sock->recv_len[head] - offset);
    if (copy_len > len) {
        copy_len = len;
    }
    k_memcpy(buf, sock->recv_buf[head] + offset, (uint32_t)copy_len);
    net_unlock(flags);

    return copy_len;
}

void socket_dgram_consume(struct socket *sock, uint32_t *src_ip, uint16_t *src_port)
{
    uint32_t flags;

    if (sock->family == AF_PACKET || sock->family == AF_NETLINK) {
        if (src_ip) {
            *src_ip = 0;
        }
        if (src_port) {
            *src_port = 0;
        }
        if (sock->family == AF_NETLINK) {
            netlink_consume(sock);
        } else {
            packet_consume(sock);
        }
        return;
    }
    if (sock->type == SOCK_RAW) {
        raw_consume(sock);
        if (src_ip) {
            *src_ip = raw_last_src(sock);
        }
        if (src_port) {
            *src_port = 0;
        }
        return;
    }

    flags = net_lock();
    if (sock->recv_count == 0) {
        net_unlock(flags);
        return;
    }
    if (src_ip) {
        *src_ip = sock->recv_src_ip[sock->recv_head];
    }
    if (src_port) {
        *src_port = sock->recv_src_port[sock->recv_head];
    }
    sock->recv_head = (sock->recv_head + 1) % SOCK_DGRAM_SLOTS;
    sock->recv_count--;
    net_unlock(flags);
}

void socket_dgram_sender(struct socket *sock, uint32_t *src_ip, uint16_t *src_port)
{
    uint32_t ip = 0, flags;
    uint16_t port = 0;

    if (sock->family == AF_PACKET) {
        packet_note_head(sock);
    } else if (sock->family != AF_NETLINK) {
        if (sock->type == SOCK_RAW) {
            ip = raw_head_src(sock);
        } else {
            flags = net_lock();
            if (sock->recv_count) {
                ip = sock->recv_src_ip[sock->recv_head];
                port = sock->recv_src_port[sock->recv_head];
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

/** Take the datagram `socket_dgram_peek` copied, or with `peek` leave it
 * waiting, and report its sender either way. */
static void socket_dgram_take(struct socket *sock, uint32_t *src_ip, uint16_t *src_port, int peek)
{
    if (peek) {
        socket_dgram_sender(sock, src_ip, src_port);
    } else {
        socket_dgram_consume(sock, src_ip, src_port);
    }
}

/** The copy and the take run under one hold of `net_lock`, as socket.h
 * requires of the three steps; the two calls take the lock again inside,
 * which nests. The buffer is brought in again after the wait, which another
 * thread's munmap may have outlasted, and before the lock, since that can
 * sleep too; a datagram another reader took meanwhile sends this one back to
 * wait for the next. */
int socket_recv(struct socket *sock, uint8_t *buf, int len, uint32_t *src_ip, uint16_t *src_port,
                int peek)
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
    socket_dgram_take(sock, src_ip, src_port, peek);
    net_unlock(flags);

    return copy_len;
}

int socket_try_recv(struct socket *sock, uint8_t *buf, int len, uint32_t *src_ip,
                    uint16_t *src_port, int peek)
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
    socket_dgram_take(sock, src_ip, src_port, peek);
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
 * connection has left a sending state, so that a blocked sender stops
 * waiting and reports it. */
static int send_space_ready(void *arg)
{
    struct socket *sock = (struct socket *)arg;
    struct tcb *tcb = sock->tcb;

    if (!tcb || (tcb->state != TCP_ESTABLISHED && tcb->state != TCP_CLOSE_WAIT)) {
        return 1;
    }
    return (TCP_RING_SIZE - (tcb->send_tail - tcb->send_ack)) > 0;
}

/**
 * The reason a connection ended, as a negative errno, or `fallback` when
 * none was recorded.
 *
 * Whatever ended the connection put the reason in the block's `so_error`,
 * and a reader or a writer of a connection that is gone is given it; a bare
 * -1 would reach the program as `EPERM`. The reason is cleared once given. A
 * block with no reason answers `fallback`, the errno the caller gives for
 * its own case: `-EPIPE` for a sender, `-ECONNRESET` for a reader. A block a
 * shutdown detached never reaches here; the entry checks answer for it.
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
 * `tcp_send` copies what fits without waiting: 0 for a full ring, and -1 for
 * a block that is gone or not sending. A blocking socket sleeps for room,
 * which `socket_tcp_writable` wakes it for when an ACK frees some, so a
 * nonzero request never returns 0, which a caller would take for the end of
 * input. The buffer is brought in again after each sleep, since `tcp_send`
 * reads it with interrupts off and another thread may have unmapped it.
 */
int socket_tcp_send(struct socket *sock, const uint8_t *buf, int len, int nonblock)
{
    if (!sock) {
        return -1;
    }
    for (;;) {
        int sent = tcp_send(sock, buf, len);
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
        {
            int rc = wq_wait_event(&sock->waiters, send_space_ready, sock);
            if (rc < 0) {
                return rc;
            }
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
    wq_wake(&sock->waiters, WQ_DATA);
}

/** A sharer's shutdown may detach the block during the wait, which is then
 * the end of input. The buffer is brought in again after the wait, which
 * another thread's munmap may have outlasted; that can sleep too, so the
 * ring is judged again after it, and bytes another reader took meanwhile
 * send this one back to the wait instead of reading as the end of input. */
int socket_tcp_recv(struct socket *sock, uint8_t *buf, int len, int peek)
{
    struct tcb *tcb;
    uint32_t avail, copy_len;
    int rc;

    if (!sock)
        return -1;
    if (!sock->tcb)
        return sock->shut_down ? 0 : -ENOTCONN;

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
        return 0;
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

int socket_tcp_try_recv(struct socket *sock, uint8_t *buf, int len, int peek)
{
    struct tcb *tcb;
    uint32_t avail, copy_len;

    if (!sock)
        return -1;
    if (!sock->tcb)
        return sock->shut_down ? 0 : -ENOTCONN;
    tcb = sock->tcb;

    avail = tcb->recv_tail - tcb->recv_head;
    if (avail == 0) {
        if (tcb->fin_received)
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
 * stream a shutdown detached is ready both ways, since a read of it answers
 * end of input at once. A connected stream is ready for input with bytes
 * buffered or the FIN taken, and for output only in a sending state with
 * send-ring room free, so a poll-then-send loop blocks instead of spinning on
 * a full ring. A recorded error adds POLLERR and POLLOUT, so a non-blocking
 * connect polled for POLLOUT wakes and reads SO_ERROR. A closed or reset
 * connection adds POLLHUP and is ready for input, since a receive answers at
 * once.
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
        if (sock->family == AF_NETLINK  ? netlink_ready(sock)
            : sock->family == AF_PACKET ? packet_ready(sock)
            : sock->type == SOCK_RAW    ? raw_ready(sock)
                                        : sock->recv_count != 0) {
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
    if (sock->shut_down && !sock->tcb) {
        return (short)(POLLHUP | (events & (POLLIN | POLLOUT)));
    }
    if (sock->tcb) {
        struct tcb *tcb = sock->tcb;
        if ((tcb->recv_tail - tcb->recv_head) != 0 || tcb->fin_received) {
            revents |= (short)(events & POLLIN);
        }
        if ((tcb->state == TCP_ESTABLISHED || tcb->state == TCP_CLOSE_WAIT) &&
            (TCP_RING_SIZE - (tcb->send_tail - tcb->send_ack)) > 0) {
            revents |= (short)(events & POLLOUT);
        }
        if (tcb->so_error != 0) {
            revents |= POLLERR;
            revents |= (short)(events & POLLOUT);
        }
        if (tcb->state == TCP_CLOSED) {
            revents |= POLLHUP;
            revents |= (short)(events & POLLIN);
        }
    }
    return revents;
}
