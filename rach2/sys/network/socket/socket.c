/*
 * socket.c - Socket abstraction layer (lifecycle, datagram and stream paths).
 *
 * Sits between the syscall layer and the protocol layers. A blocking receive
 * sleeps on the socket's wait queue; the receive path, which runs with
 * interrupts off, fills the buffer and wakes it.
 *
 * Lifecycle: socket_alloc starts the refcount at 1, socket_ref bumps it (when
 * fork or a dup call shares a descriptor), and socket_unref drops it, tearing
 * the socket down and freeing it at zero.
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
#include "../../arch/x86/serial.h"
#include "../../lib/kstring.h"
#include "../../lib/kerrno.h"
#include "../../mm/kheap.h"
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
 * The counter is 32 bits and wraps, so the live sockets of both families
 * are checked and a number one of them has is stepped over. Zero is skipped
 * because it is what an unset field reads as. Called with net_lock held.
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

/**
 * A socket from the heap, on the list and initialised for the given type.
 *
 * Returns a socket with refcount 1 and clean protocol state, or NULL when
 * the heap has no room. The list is changed under net_lock because it is
 * shared with the receive path (an incoming SYN makes a socket too).
 * The socket gets its own inode here; the owning uid is set by the caller,
 * which is the only context that knows whether there is a caller to ask.
 *
 * @param type SOCK_DGRAM, SOCK_STREAM or SOCK_RAW.
 */
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

    /* The list and the inode are shared with the interrupt side. */
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
 * Drop a reference and tear the socket down at zero.
 *
 * At the last reference, unbind the packet, raw or UDP socket or close the
 * TCP connection, then take the socket off the list and free it. tcp_close
 * detaches the TCB so it can finish the FIN handshake on its own after the
 * socket is freed. A listener also releases every connection still queued
 * for accept, so their sockets and TCBs are not leaked; each is a
 * non-listening socket, so the recursion is one level deep, and net_lock
 * nests.
 */
void socket_unref(struct socket *sock)
{
    uint32_t flags;

    if (!sock) {
        return;
    }

    /* The refcount and the list are shared with the interrupt side. */
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

    /* Off the list and back to the heap; nothing refers to it now. */
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

/**
 * Queue a received datagram on a UDP socket and wake its waiters.
 *
 * Called from the receive path with interrupts off. A slot has at most
 * SOCK_RECV_BUF_SIZE bytes, and a longer payload is truncated; a datagram
 * that finds all SOCK_DGRAM_SLOTS slots waiting is dropped. The source
 * address is kept with the datagram for recvfrom. A connected socket takes
 * datagrams from its peer and from nobody else, which is what `connect`
 * means for UDP.
 */
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

/* Wait predicate: a datagram waits on a UDP, raw or packet socket. */
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

/* Wait predicate: a stream socket has buffered data, saw a FIN (EOF), or its
 * connection has closed/reset; any of which ends a blocking recv. */
static int tcp_recv_ready(void *arg)
{
    socket_t *sock = (socket_t *)arg;
    struct tcb *tcb = sock->tcb;
    if (!tcb) {
        return 1; /* no control block: do not block, the caller returns 0 */
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

/**
 * Wait for a datagram to be buffered. Returns 0, or -errno if a signal came
 * first or an error waits, which goes before any datagram, as Linux's
 * receive reports it first.
 */
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

/**
 * Bytes in the oldest waiting datagram, or -1 when none waits.
 *
 * Occupancy is `recv_count` and not a length, since a datagram of no bytes is
 * a datagram; a caller that read 0 as emptiness would block forever on one.
 */
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

/**
 * Copy part of the oldest waiting datagram out without consuming it.
 *
 * `offset` is how far into the datagram to start, so a caller with several
 * destinations fills them in order from the one message. A copy that starts at
 * or past the end returns 0, which is what ends such a walk. Returns -1 when
 * no datagram is waiting.
 *
 * The queue is read under `net_lock`, so the receive interrupt does not
 * change it during the copy. The caller prefaulted the user buffer, so the
 * copy cannot fault with interrupts off.
 */
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

/**
 * Drop the oldest waiting datagram, reporting its sender.
 *
 * Whatever of it a caller did not take is discarded here, which is what a
 * datagram read does: the message is the unit. A packet socket reports
 * address and port 0, since its sender is a link address, which
 * packet_consume keeps for packet_last_addr. A raw socket reports the source
 * address from the datagram's IP header and port 0, since a raw socket is
 * not a transport.
 */
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

/**
 * Report the sender of the oldest waiting datagram as `socket_dgram_consume`
 * reports it, without taking the datagram: for a raw socket the source
 * address of its IP header and port 0, and 0 for both for a packet or
 * netlink socket, and with no datagram waiting. A packet socket keeps the
 * datagram's link address for `packet_last_addr`, as a consume keeps it.
 */
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

/**
 * Blocking datagram receive: wait for a datagram, then copy it out.
 *
 * Sleeps until a datagram is buffered; a pending signal unwinds the wait and
 * returns -EINTR. A payload longer than `len` is truncated to `len` (the
 * remainder of the single-datagram buffer is discarded, as for SOCK_DGRAM).
 * With `peek` the datagram is copied and left waiting, whole, for the next
 * receive, as MSG_PEEK asks. The copy and the consume run under one net_lock
 * hold, so the receive path cannot put the next datagram in the slot between
 * them and pair one message's bytes with another's sender. The two calls
 * take the lock again inside, which nests.
 *
 * @param src_ip   if non-NULL, receives the sender's address.
 * @param src_port if non-NULL, receives the sender's port.
 * @return bytes copied, or a negative errno.
 */
int socket_recv(struct socket *sock, uint8_t *buf, int len, uint32_t *src_ip, uint16_t *src_port,
                int peek)
{
    int copy_len;
    int rc;
    uint32_t flags;

    rc = socket_dgram_wait(sock);
    if (rc < 0) {
        return rc;
    }

    flags = net_lock();
    copy_len = socket_dgram_peek(sock, 0, buf, len);
    if (copy_len < 0) {
        copy_len = 0;
    }
    socket_dgram_take(sock, src_ip, src_port, peek);
    net_unlock(flags);

    return copy_len;
}

/**
 * Non-blocking datagram receive: copy a buffered datagram, else SOCK_EAGAIN.
 *
 * Same copy and source-address contract as socket_recv but never sleeps.
 */
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

/*
 * The stream side. The TCP layer calls socket_tcp_accept,
 * socket_tcp_connected, socket_tcp_writable, socket_tcp_deliver and
 * socket_tcp_fin with interrupts off; socket_tcp_send, socket_tcp_recv and
 * socket_tcp_try_recv are the syscall paths.
 */

/**
 * Queue an established connection on a listener and wake an accept waiter.
 *
 * Called from tcp_rx with interrupts off. The accept queue is a power-of-two
 * ring. A full queue refuses the connection: socket_unref closes it, which
 * sends a FIN, and frees the socket.
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

/** Wake a socket's waiters when its connection completes or ends. */
void socket_tcp_connected(struct socket *sock)
{
    if (!sock)
        return;

    wq_wake_all(&sock->waiters);
}

/* Wake a writer or POLLOUT poller after an ACK frees send-ring space. */
void socket_tcp_writable(struct socket *sock)
{
    if (!sock)
        return;

    wq_wake(&sock->waiters, WQ_ROOM);
}

/* Wait predicate: the send ring has room, or the connection left a sending
 * state (so a blocked sender stops waiting and reports the result). */
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
 * Blocking stream send: queue bytes, waiting for send-ring space if needed.
 *
 * tcp_send is the non-blocking primitive (it copies what fits and returns 0
 * when the ring is full). This wraps it so a blocking socket sleeps on
 * send-ring space (woken by socket_tcp_writable when an ACK frees room) and
 * a non-blocking one returns -EAGAIN. A nonzero request thus never returns
 * 0, which a caller would misread as EOF. Returns the bytes queued,
 * SOCK_EAGAIN, -EINTR, or, on a connection that is no longer in a sending
 * state, its recorded error or -EPIPE.
 */
int socket_tcp_send(struct socket *sock, const uint8_t *buf, int len, int nonblock)
{
    if (!sock) {
        return -1;
    }
    for (;;) {
        int sent = tcp_send(sock, buf, len);
        if (sent == -1) {
            /* tcp_send's refusal of a block that is gone or not sending. */
            return tcp_dead_error(sock->tcb, -EPIPE);
        }
        if (sent != 0 || len == 0) {
            return sent; /* queued some, or a 0-length request */
        }
        /* The ring is full and the request is nonzero. */
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
                return rc; /* -EINTR */
            }
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
 * Copy received stream bytes into the TCB receive ring and wake a reader.
 *
 * Called from tcp_rx with interrupts off. Copies at most the ring's free space
 * and returns the count accepted; the caller advances rcv_nxt by that, so
 * surplus bytes are neither stored nor acknowledged and the peer retransmits
 * them once the window reopens. recv_head/recv_tail are free-running indices
 * masked to size. The wake is for data, and comes only when bytes were
 * taken: a full ring takes none, and an edge-triggered registration for
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

/**
 * Keep stream bytes that arrived past a hole in the TCB receive ring.
 *
 * Called from tcp_rx with interrupts off. The bytes go `offset` bytes past
 * the ring's tail, where the stream puts them once the hole is filled, and
 * stay unreadable until `socket_tcp_release`; what does not fit in the ring's
 * free space is not kept. Data that fills the hole later overwrites none of
 * them but with the same bytes, since a stream's byte has one value.
 */
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

/**
 * Make `n` bytes that `socket_tcp_hold` kept past the ring's tail readable,
 * once the hole before them is filled, and wake a reader for data.
 */
void socket_tcp_release(struct socket *sock, uint32_t n)
{
    if (!sock || !sock->tcb || n == 0)
        return;
    sock->tcb->recv_tail += n;
    wq_wake(&sock->waiters, WQ_DATA);
}

/**
 * Blocking stream receive from the TCB ring.
 *
 * Sleeps until data arrives, a FIN signals EOF, or the connection resets; a
 * pending signal unwinds with -EINTR. Returns the byte count on data, 0 at EOF
 * (FIN with no buffered data, or a block a shutdown detached), and on a
 * reset or closed connection with no data its recorded error or
 * -ECONNRESET; a stream that never connected is -ENOTCONN. With `peek` the
 * bytes are copied and left in the ring, where the next receive finds them,
 * as MSG_PEEK asks; the window does not change.
 */
int socket_tcp_recv(struct socket *sock, uint8_t *buf, int len, int peek)
{
    struct tcb *tcb;
    uint32_t avail, copy_len;
    int rc;

    if (!sock)
        return -1;
    if (!sock->tcb)
        return sock->shut_down ? 0 : -ENOTCONN;

    rc = wq_wait_event(&sock->waiters, tcp_recv_ready, sock);
    if (rc < 0) {
        return rc;
    }
    /* A sharer's shutdown may have freed the block during the sleep. */
    tcb = sock->tcb;
    if (!tcb) {
        return 0;
    }

    avail = tcb->recv_tail - tcb->recv_head;
    if (avail == 0 && !tcb->fin_received && tcb->state == TCP_CLOSED) {
        return tcp_dead_error(tcb, -ECONNRESET); /* reset or closed with no data */
    }
    if (avail == 0)
        return 0; /* EOF (FIN, no buffered data) */

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
 * Non-blocking stream receive: drain the ring, else report status.
 *
 * Never sleeps. With no buffered data: 0 on a half-closed peer (FIN) or a
 * block a shutdown detached, the recorded error or -ECONNRESET on a reset or
 * closed connection, -ENOTCONN on a stream that never connected, and
 * SOCK_EAGAIN otherwise. `peek` is as for `socket_tcp_recv`.
 */
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
            return 0; /* EOF */
        if (tcb->state == TCP_CLOSED)
            return tcp_dead_error(tcb, -ECONNRESET);
        return SOCK_EAGAIN; /* no data yet */
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

/** Wake a socket's waiters on the peer's FIN or the connection's end. */
void socket_tcp_fin(struct socket *sock)
{
    if (!sock)
        return;

    wq_wake_all(&sock->waiters);
}

/**
 * Map current socket state to poll revents.
 *
 * Returns the subset of the requested `events` that are ready now, plus any
 * unconditional condition (POLLERR, POLLHUP, POLLNVAL). When `wq` is non-NULL
 * it is set to the socket's wait queue so the poller can sleep on it.
 *
 * The query descriptor, a raw socket with no protocol, has only ioctls: it
 * answers POLLERR at once, so a poller does not wait on it forever. A raw or
 * packet socket polls like a datagram socket, on its own queue, which is
 * what a DHCP client and `ping` poll before they read. A stream a shutdown
 * detached is ready both ways, since a read of it answers end of input at
 * once. A connected stream is writable only in a sending state with
 * send-ring space free, so a poll-then-send loop blocks instead of spinning
 * on a full ring; a recorded error adds POLLERR and POLLOUT, so a
 * non-blocking connect polled for POLLOUT wakes and reads SO_ERROR.
 *
 * @return the ready revents, or POLLNVAL for a NULL socket.
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
        revents |= (short)(events & POLLOUT); /* it can always send */
        if (sock->so_error != 0) {
            revents |= POLLERR;
        }
        return revents;
    }

    /* SOCK_STREAM. */
    if (sock->listening) {
        /* A listener is "readable" when a connection is queued for accept. */
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
            revents |= (short)(events & POLLIN); /* buffered data, or EOF (FIN) */
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
            revents |= POLLHUP;                  /* reset / fully closed */
            revents |= (short)(events & POLLIN); /* a recv returns at once */
        }
    }
    return revents;
}
