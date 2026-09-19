/*
 * socket.c - Socket abstraction layer (UDP paths and lifecycle).
 *
 * Sits between the syscall layer and the protocol layers. A blocking receive
 * registers the caller as the socket's waiter and halts (sti;hlt); the IRQ-side
 * deliver routine fills the buffer and wakes it. The TCP-side helpers live with
 * the TCP layer.
 *
 * Lifecycle: socket_alloc starts the refcount at 1, socket_ref bumps it (when
 * fork or dup2 share a descriptor), and socket_unref drops it, tearing the
 * socket down and returning it to the pool at zero.
 */

#include "../../klog.h"
#include "socket.h"
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
 * The counter is 32 bits and wraps, so the live sockets are checked and a
 * number one of them has is stepped over. Zero is skipped because it is
 * what an unset field reads as. Called with net_lock held.
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
        if (!s) {
            return candidate;
        }
    }
}

/**
 * The socket carrying `inode`, or NULL if no live socket has it.
 *
 * The identity /proc, an fd symlink and fstat all report is this number, so a
 * program that reads one of them can find the socket behind the other two.
 */
struct socket *socket_by_inode(uint32_t inode)
{
    socket_t *s;

    if (inode == 0) {
        return NULL;
    }
    for (s = sockets; s; s = s->next) {
        if (s->inode == inode) {
            return s;
        }
    }
    return NULL;
}

/**
 * A socket from the heap, on the list and initialised for the given type.
 *
 * Returns a socket with refcount 1 and clean protocol state, or NULL when
 * the heap has no room. The list is changed under net_lock because it is
 * shared with the receive path (an incoming SYN makes a socket too).
 * The slot gets an inode of its own here; the owning uid is set by the caller,
 * which is the only context that knows whether there is a caller to ask.
 *
 * @param type SOCK_DGRAM or SOCK_STREAM.
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
 * At the last reference, unbind the UDP port or close the TCP connection and
 * return the slot to the pool. tcp_close detaches the TCB so it can finish the
 * FIN handshake on its own after the slot is freed.
 */
void socket_unref(struct socket *sock)
{
    uint32_t flags;

    if (!sock) {
        return;
    }

    /* Refcount and pool slot are shared with the IRQ-context paths. */
    flags = net_lock();

    if (sock->refcount > 0) {
        sock->refcount--;
    }
    if (sock->refcount > 0) {
        net_unlock(flags);
        return;
    }

    if (sock->family == AF_PACKET) {
        packet_unbind(sock);
    } else if (sock->type == SOCK_RAW) {
        raw_unregister(sock);
    } else if (sock->type == SOCK_DGRAM && sock->local_port != 0) {
        udp_unbind(sock);
    } else if (sock->type == SOCK_STREAM && sock->tcb) {
        tcp_close(sock);
    }

    /* A listener still owns every connection queued for accept but never picked
     * up; release each so its socket slot and TCB are not leaked for the life
     * of the system. Each queued conn_sock is an established (non-listening)
     * socket, so this recurses at most one level, and net_lock is save/restore
     * reentrant. */
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
 * Deliver a received datagram into a UDP socket and wake its waiter.
 *
 * Called from the IRQ-side receive path. The single-datagram buffer has at
 * most SOCK_RECV_BUF_SIZE bytes; a longer payload is truncated. The source
 * address is recorded for a later recvfrom.
 */
void socket_udp_deliver(struct socket *sock, uint32_t src_ip, uint16_t src_port,
                        const uint8_t *data, uint16_t len)
{
    uint16_t copy_len = len;

    /* A connected datagram socket hears from its peer and from nobody else.
       That is what `connect` means for UDP, and a client that connected to its
       server must not be answered by whoever else is on the wire. */
    if (sock->connected && (src_ip != sock->remote_ip || src_port != sock->remote_port)) {
        return;
    }

    if (copy_len > SOCK_RECV_BUF_SIZE) {
        copy_len = SOCK_RECV_BUF_SIZE;
    }

    k_memcpy(sock->recv_buf, data, copy_len);
    sock->recv_len = copy_len;
    sock->recv_full = 1;
    sock->last_src_ip = src_ip;
    sock->last_src_port = src_port;

    wq_wake_all(&sock->waiters);
}

/* Wait predicate: a UDP datagram is buffered. */
static int udp_recv_ready(void *arg)
{
    socket_t *sock = (socket_t *)arg;

    if (sock->family == AF_PACKET) {
        return packet_ready(sock);
    }
    if (sock->type == SOCK_RAW) {
        return raw_ready(sock);
    }
    return sock->recv_full;
}

/* Wait predicate: a stream socket has buffered data, saw a FIN (EOF), or its
 * connection has closed/reset; any of which ends a blocking recv. */
static int tcp_recv_ready(void *arg)
{
    socket_t *sock = (socket_t *)arg;
    struct tcb *tcb = sock->tcb;
    if (!tcb) {
        return 1; /* no control block: do not block, the caller reports the error */
    }
    return (tcb->recv_tail - tcb->recv_head) != 0 || tcb->fin_received || tcb->state == TCP_CLOSED;
}

/**
 * Wait for a datagram to be buffered. Returns 0, or -errno if a signal came
 * first.
 */
int socket_dgram_wait(struct socket *sock)
{
    return wq_wait_event(&sock->waiters, udp_recv_ready, sock);
}

/**
 * Bytes in the waiting datagram, or -1 when the slot is empty.
 *
 * Occupancy is `recv_full` and not the length, since a datagram of no bytes is
 * a datagram; a caller that read 0 as emptiness would block forever on one.
 */
int socket_dgram_pending(struct socket *sock)
{
    int pending;
    uint32_t flags;

    if (sock->family == AF_PACKET) {
        return packet_pending(sock);
    }
    if (sock->type == SOCK_RAW) {
        return raw_pending(sock);
    }

    flags = net_lock();
    pending = sock->recv_full ? (int)sock->recv_len : -1;
    net_unlock(flags);

    return pending;
}

/**
 * Copy part of the waiting datagram out without consuming it.
 *
 * `offset` is how far into the datagram to start, so a caller with several
 * destinations fills them in order from the one message. A copy that starts at
 * or past the end returns 0, which is what ends such a walk. Returns -1 when
 * no datagram is waiting.
 *
 * The slot is read under `net_lock` so the receive interrupt cannot overwrite
 * it mid-copy, which would tear the payload. The caller prefaulted the user
 * buffer, so the copy cannot fault with interrupts off.
 */
int socket_dgram_peek(struct socket *sock, uint32_t offset, uint8_t *buf, int len)
{
    int copy_len;
    uint32_t flags;

    if (len < 0) {
        return 0;
    }
    if (sock->family == AF_PACKET) {
        return packet_peek(sock, offset, buf, len);
    }
    if (sock->type == SOCK_RAW) {
        return raw_peek(sock, offset, buf, len);
    }

    flags = net_lock();
    if (!sock->recv_full) {
        net_unlock(flags);

        return -1;
    }
    if (offset >= (uint32_t)sock->recv_len) {
        net_unlock(flags);

        return 0;
    }
    copy_len = (int)((uint32_t)sock->recv_len - offset);
    if (copy_len > len) {
        copy_len = len;
    }
    k_memcpy(buf, sock->recv_buf + offset, (uint32_t)copy_len);
    net_unlock(flags);

    return copy_len;
}

/**
 * Drop the waiting datagram, reporting its sender.
 *
 * Whatever of it a caller did not take is discarded here, which is what a
 * datagram read does: the message is the unit, and the next one is next.
 */
void socket_dgram_consume(struct socket *sock, uint32_t *src_ip, uint16_t *src_port)
{
    uint32_t flags;

    /* A packet socket has a queue instead of the slot, and no sender to
       report: what it received is a link-layer frame's payload, and the link
       address it came from is not kept. */
    if (sock->family == AF_PACKET) {
        if (src_ip) {
            *src_ip = 0;
        }
        if (src_port) {
            *src_port = 0;
        }
        packet_consume(sock);
        return;
    }
    /* A raw socket reports the address the datagram came from, which is in
       the header it was given; there is no port, since a raw socket is not a
       transport. */
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
    if (src_ip) {
        *src_ip = sock->last_src_ip;
    }
    if (src_port) {
        *src_port = sock->last_src_port;
    }
    sock->recv_len = 0;
    sock->recv_full = 0;
    net_unlock(flags);
}

/**
 * Blocking UDP receive: wait for a datagram, then copy it out.
 *
 * Sleeps until a datagram is buffered; a pending signal unwinds the wait and
 * returns -EINTR. A payload longer than `len` is truncated to `len` (the
 * remainder of the single-datagram buffer is discarded, as for SOCK_DGRAM).
 *
 * @param src_ip   if non-NULL, receives the sender's address.
 * @param src_port if non-NULL, receives the sender's port.
 * @return bytes copied, or a negative errno.
 */
int socket_recv(struct socket *sock, uint8_t *buf, int len, uint32_t *src_ip, uint16_t *src_port)
{
    int copy_len;
    int rc;
    uint32_t flags;

    rc = socket_dgram_wait(sock);
    if (rc < 0) {
        return rc;
    }

    /* One hold across the copy and the clear. Between them the receive
     * interrupt could otherwise put the next datagram in the slot, and this
     * would return one message's bytes with the other one's sender, clearing a
     * datagram nobody read. The two calls take the lock again inside, which
     * nests. */
    flags = net_lock();
    copy_len = socket_dgram_peek(sock, 0, buf, len);
    if (copy_len < 0) {
        copy_len = 0;
    }
    socket_dgram_consume(sock, src_ip, src_port);
    net_unlock(flags);

    return copy_len;
}

/**
 * Non-blocking UDP receive: copy a buffered datagram, else SOCK_EAGAIN.
 *
 * Same copy and source-address contract as socket_recv but never sleeps.
 */
int socket_try_recv(struct socket *sock, uint8_t *buf, int len, uint32_t *src_ip,
                    uint16_t *src_port)
{
    int copy_len;
    uint32_t flags;

    flags = net_lock();
    copy_len = socket_dgram_peek(sock, 0, buf, len);
    if (copy_len < 0) {
        net_unlock(flags);

        return SOCK_EAGAIN;
    }
    socket_dgram_consume(sock, src_ip, src_port);
    net_unlock(flags);

    return copy_len;
}

/*
 * TCP-side helpers, all called from tcp_rx in IRQ context except
 * socket_tcp_recv, which is the blocking syscall path.
 */

/**
 * Queue an established connection on a listener and wake an accept waiter.
 *
 * Called from tcp_rx in IRQ context. The accept queue is a power-of-two ring;
 * a full queue refuses the connection (see the body), not overrun.
 */
void socket_tcp_accept(struct socket *listen_sock, struct socket *conn_sock)
{
    int next_tail;

    if (!listen_sock || !conn_sock)
        return;

    next_tail = (listen_sock->accept_tail + 1) & (SOCK_ACCEPT_BACKLOG - 1);
    if (next_tail == listen_sock->accept_head) {
        /* Queue full: tear the new connection down (it sends a FIN and frees
         * the socket and TCB) instead of orphaning it in the pool. */
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

/* Wake a writer or POLLOUT poller after an ACK frees send-ring space. */
void socket_tcp_writable(struct socket *sock)
{
    if (!sock)
        return;

    wq_wake_all(&sock->waiters);
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
 * The reason is in the block, put there by whatever ended the connection,
 * and a reader or a writer of a connection that is gone is given it (a
 * bare -1, which the C library reads as `EPERM`, told a program woken
 * because this machine's address changed that it lacked permission). A
 * block that holds no reason answers `fallback`, the errno the caller
 * names for its own case: `-EPIPE` for a sender, `-ECONNRESET` for a
 * reader. A block a shutdown detached never reaches here; the entry checks
 * answer for it.
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
            /* The bare refusal a closed block gives, turned into the reason it
               closed. Anything else negative already names itself. */
            return tcp_dead_error(sock->tcb, -EPIPE);
        }
        if (sent != 0 || len == 0) {
            return sent; /* queued some, an error (< 0), or a 0-length request */
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
 * end is two straight runs. A byte at a time cost a load, a store and a mask
 * for every byte of every segment. */
static void recv_ring_put(struct tcb *tcb, uint32_t at, const uint8_t *src, uint32_t n)
{
    uint32_t head = at & (TCP_RING_SIZE - 1);
    uint32_t first = TCP_RING_SIZE - head;

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
    uint32_t head = at & (TCP_RING_SIZE - 1);
    uint32_t first = TCP_RING_SIZE - head;

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
 * Called from tcp_rx in IRQ context. Copies at most the ring's free space
 * and returns the count accepted; the caller advances rcv_nxt by that, so
 * surplus bytes are neither stored nor acknowledged and the peer retransmits
 * them once the window reopens. recv_head/recv_tail are free-running indices
 * masked to size.
 */
uint16_t socket_tcp_deliver(struct socket *sock, const uint8_t *data, uint16_t len)
{
    struct tcb *tcb;
    uint32_t free_space, copy_len;

    if (!sock || !sock->tcb)
        return 0;
    tcb = sock->tcb;

    free_space = TCP_RING_SIZE - (tcb->recv_tail - tcb->recv_head);
    copy_len = (uint32_t)len;
    if (copy_len > free_space)
        copy_len = free_space;

    recv_ring_put(tcb, tcb->recv_tail, data, copy_len);
    tcb->recv_tail += copy_len;

    wq_wake_all(&sock->waiters);
    return (uint16_t)copy_len;
}

/**
 * Blocking stream receive from the TCB ring.
 *
 * Sleeps until data arrives, a FIN signals EOF, or the connection resets; a
 * pending signal unwinds with -EINTR. Returns the byte count on data, 0 at EOF
 * (FIN with no buffered data, or a block a shutdown detached), and on a
 * reset or closed connection with no data its recorded error or
 * -ECONNRESET; a stream that never connected is -ENOTCONN.
 */
int socket_tcp_recv(struct socket *sock, uint8_t *buf, int len)
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
    /* Read again after the sleep: a sharer of the socket may have shut it
     * down meanwhile and the peer's answer freed the block. */
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
    tcb->recv_head += copy_len;

    tcp_window_update(sock);
    return (int)copy_len;
}

/**
 * Non-blocking stream receive: drain the ring, else report status.
 *
 * Never sleeps. With no buffered data: 0 on a half-closed peer (FIN) or a
 * block a shutdown detached, the recorded error or -ECONNRESET on a reset or
 * closed connection, -ENOTCONN on a stream that never connected, and
 * SOCK_EAGAIN otherwise.
 */
int socket_tcp_try_recv(struct socket *sock, uint8_t *buf, int len)
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
    tcb->recv_head += copy_len;

    tcp_window_update(sock);
    return (int)copy_len;
}

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
 * unconditional condition (POLLHUP, POLLNVAL). When `wq` is non-NULL it is set
 * to the socket's wait queue so the poller can sleep on it. The readiness rules
 * differ by socket kind and are noted at each site.
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

    /* The query descriptor is never readable and never writable: it has
       ioctls. POLLERR says so at once, so a caller that polls it waits on
       nothing, not forever. */
    /* The query descriptor is never readable and never writable; a raw socket
       with a protocol of its own is a real one and polls like a datagram
       socket, below. */
    if (sock->type == SOCK_RAW && sock->protocol == 0) {
        return (short)POLLERR;
    }

    if (sock->type == SOCK_DGRAM || sock->type == SOCK_RAW) {
        /* A packet socket and a raw socket each have a queue of their own and
           never touch `recv_full`, and this is the readiness their clients
           wait on: a DHCP client polls its socket and only then reads it, and
           so does `ping`. */
        if (sock->family == AF_PACKET ? packet_ready(sock)
            : sock->type == SOCK_RAW  ? raw_ready(sock)
                                      : sock->recv_full) {
            revents |= (short)(events & POLLIN);
        }
        revents |= (short)(events & POLLOUT); /* it can always send */
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
    if (sock->tcb) {
        struct tcb *tcb = sock->tcb;
        if ((tcb->recv_tail - tcb->recv_head) != 0 || tcb->fin_received) {
            revents |= (short)(events & POLLIN); /* buffered data, or EOF (FIN) */
        }
        /* Writable only in a sending state AND with send-ring space free, so a
         * poll-then-send loop blocks instead of spinning when the ring is full.
         */
        if ((tcb->state == TCP_ESTABLISHED || tcb->state == TCP_CLOSE_WAIT) &&
            (TCP_RING_SIZE - (tcb->send_tail - tcb->send_ack)) > 0) {
            revents |= (short)(events & POLLOUT);
        }
        if (tcb->so_error != 0) {
            /* A failed connection: report the error, and POLLOUT so a
             * poll(POLLOUT) on a non-blocking connect wakes and the caller then
             * reads SO_ERROR. */
            revents |= POLLERR;
            revents |= (short)(events & POLLOUT);
        }
        if (tcb->state == TCP_CLOSED) {
            revents |= POLLHUP;                  /* reset / fully closed */
            revents |= (short)(events & POLLIN); /* a recv will return 0/-1 at once */
        }
    }
    return revents;
}
