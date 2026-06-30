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

#include "socket.h"
#include "../net.h"
#include "../udp/udp.h"
#include "../tcp/tcp.h"
#include "../../io/poll.h"
#include "../../serial.h"
#include "../../lib/kstring.h"
#include "../../mm/heap.h"

/* Socket pool, allocated from kernel memory at init (kept out of BSS). */
static socket_t *socket_pool;

/**
 * Reserve the socket pool from kernel memory.
 *
 * Allocated rather than placed in BSS so the table does not bloat the kernel
 * image. A failed reservation leaves the pool NULL; socket_alloc then fails.
 */
void socket_init(void)
{
    socket_pool = (socket_t *)mem_reserve(sizeof(socket_t) * MAX_SOCKETS, true);
    if (socket_pool) {
        k_memset(socket_pool, 0, sizeof(socket_t) * MAX_SOCKETS);
    }
}

/**
 * Claim a free socket slot and initialize it for the given type.
 *
 * Returns a socket with refcount 1 and clean protocol state, or NULL if the
 * pool is exhausted. The scan runs under net_lock because the slot table is
 * shared with the IRQ-context allocator (an incoming SYN claims a slot too).
 *
 * @param type SOCK_DGRAM or SOCK_STREAM.
 */
struct socket *socket_alloc(int type)
{
    int i;
    uint32_t flags;

    if (!socket_pool) {
        return NULL;
    }

    /* The pool scan races with the IRQ-context allocator (an incoming SYN). */
    flags = net_lock();

    for (i = 0; i < MAX_SOCKETS; i++) {
        if (!socket_pool[i].in_use) {
            socket_pool[i].in_use        = 1;
            socket_pool[i].refcount      = 1;
            socket_pool[i].type          = type;
            socket_pool[i].local_port    = 0;
            socket_pool[i].remote_ip     = 0;
            socket_pool[i].remote_port   = 0;
            socket_pool[i].tcb           = NULL;
            socket_pool[i].accept_head   = 0;
            socket_pool[i].accept_tail   = 0;
            socket_pool[i].listening     = 0;
            socket_pool[i].last_src_ip   = 0;
            socket_pool[i].last_src_port = 0;
            socket_pool[i].recv_len      = 0;
            wait_queue_init(&socket_pool[i].waiters);
            k_memset(socket_pool[i].accept_queue, 0,
                     sizeof(socket_pool[i].accept_queue));
            net_unlock(flags);
            return &socket_pool[i];
        }
    }

    net_unlock(flags);
    serial_printf("socket: pool exhausted (%d slots)\n", MAX_SOCKETS);
    return NULL;
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

    if (sock->type == SOCK_DGRAM && sock->local_port != 0) {
        udp_unbind(sock->local_port);
    } else if (sock->type == SOCK_STREAM && sock->tcb) {
        tcp_close(sock);
    }

    /* A listener still owns every connection queued for accept but never picked
     * up; release each so its socket slot and TCB are not leaked for the life of
     * the system. Each queued conn_sock is an established (non-listening) socket,
     * so this recurses at most one level, and net_lock is save/restore reentrant. */
    if (sock->type == SOCK_STREAM && sock->listening) {
        while (sock->accept_head != sock->accept_tail) {
            struct socket *conn = sock->accept_queue[sock->accept_head];
            sock->accept_head = (sock->accept_head + 1) & (SOCK_ACCEPT_BACKLOG - 1);
            socket_unref(conn);
        }
    }

    sock->in_use = 0;
    net_unlock(flags);
}

/**
 * Deliver a received datagram into a UDP socket and wake its waiter.
 *
 * Called from the IRQ-side receive path. The single-datagram buffer holds at
 * most SOCK_RECV_BUF_SIZE bytes; a longer payload is truncated. The source
 * address is recorded for a later recvfrom.
 */
void socket_udp_deliver(struct socket *sock,
                        uint32_t src_ip, uint16_t src_port,
                        const uint8_t *data, uint16_t len)
{
    uint16_t copy_len = len;

    if (copy_len > SOCK_RECV_BUF_SIZE) {
        copy_len = SOCK_RECV_BUF_SIZE;
    }

    k_memcpy(sock->recv_buf, data, copy_len);
    sock->recv_len      = copy_len;
    sock->last_src_ip   = src_ip;
    sock->last_src_port = src_port;

    wq_wake_all(&sock->waiters);
}

/* Wait predicate: a UDP datagram is buffered. */
static int udp_recv_ready(void *arg)
{
    socket_t *sock = (socket_t *)arg;
    return sock->recv_len != 0;
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
    return (tcb->recv_tail - tcb->recv_head) != 0 ||
           tcb->fin_received ||
           tcb->state == TCP_CLOSED;
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
int socket_recv(struct socket *sock, uint8_t *buf, int len,
                uint32_t *src_ip, uint16_t *src_port)
{
    int copy_len;
    int rc;
    uint32_t flags;

    rc = wq_wait_event(&sock->waiters, udp_recv_ready, sock);
    if (rc < 0) {
        return rc;
    }

    /* Consume the single-datagram slot under net_lock so the RX IRQ producer
     * (socket_udp_deliver) cannot overwrite it mid-readout, which would tear the
     * payload, mismatch the recorded source, or let the trailing recv_len = 0 drop
     * a datagram the IRQ just delivered. The caller prefaulted the user buffer, so
     * the copy cannot fault with interrupts off. */
    flags = net_lock();
    copy_len = (int)sock->recv_len;
    if (copy_len > len) {
        copy_len = len;
    }
    k_memcpy(buf, sock->recv_buf, (uint32_t)copy_len);
    if (src_ip) {
        *src_ip = sock->last_src_ip;
    }
    if (src_port) {
        *src_port = sock->last_src_port;
    }
    sock->recv_len = 0;
    net_unlock(flags);
    return copy_len;
}

/**
 * Non-blocking UDP receive: copy a buffered datagram, else SOCK_EAGAIN.
 *
 * Same copy and source-address contract as socket_recv but never sleeps.
 */
int socket_try_recv(struct socket *sock, uint8_t *buf, int len,
                    uint32_t *src_ip, uint16_t *src_port)
{
    int copy_len;
    uint32_t flags;

    /* Same atomic read-out as socket_recv: snapshot and clear the slot under
     * net_lock so the RX IRQ cannot interleave. */
    flags = net_lock();
    if (sock->recv_len == 0) {
        net_unlock(flags);
        return SOCK_EAGAIN;
    }
    copy_len = (int)sock->recv_len;
    if (copy_len > len) {
        copy_len = len;
    }
    k_memcpy(buf, sock->recv_buf, (uint32_t)copy_len);
    if (src_ip) {
        *src_ip = sock->last_src_ip;
    }
    if (src_port) {
        *src_port = sock->last_src_port;
    }
    sock->recv_len = 0;
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
 * a full queue refuses the connection (see the body) rather than overrun.
 */
void socket_tcp_accept(struct socket *listen_sock, struct socket *conn_sock)
{
    int next_tail;

    if (!listen_sock || !conn_sock) return;

    next_tail = (listen_sock->accept_tail + 1) & (SOCK_ACCEPT_BACKLOG - 1);
    if (next_tail == listen_sock->accept_head) {
        /* Queue full: tear the new connection down (it sends a FIN and frees
         * the socket and TCB) rather than orphaning it in the pool. */
        serial_printf("socket: accept queue full, connection refused\n");
        socket_unref(conn_sock);
        return;
    }

    listen_sock->accept_queue[listen_sock->accept_tail] = conn_sock;
    listen_sock->accept_tail = next_tail;

    wq_wake_all(&listen_sock->waiters);
}

void socket_tcp_connected(struct socket *sock)
{
    if (!sock) return;

    wq_wake_all(&sock->waiters);
}

/**
 * Copy received stream bytes into the TCB receive ring and wake a reader.
 *
 * Called from tcp_rx in IRQ context. Copies at most the ring's free space;
 * surplus bytes are dropped here, leaving the peer to retransmit them once the
 * window reopens. recv_head/recv_tail are free-running indices masked to size.
 */
void socket_tcp_deliver(struct socket *sock, const uint8_t *data, uint16_t len)
{
    struct tcb *tcb;
    uint32_t free_space, copy_len, i;

    if (!sock || !sock->tcb) return;
    tcb = sock->tcb;

    free_space = TCP_RING_SIZE - (tcb->recv_tail - tcb->recv_head);
    copy_len   = (uint32_t)len;
    if (copy_len > free_space) copy_len = free_space;

    for (i = 0; i < copy_len; i++) {
        tcb->recv_buf[(tcb->recv_tail + i) & (TCP_RING_SIZE - 1)] = data[i];
    }
    tcb->recv_tail += copy_len;

    wq_wake_all(&sock->waiters);
}

/**
 * Blocking stream receive from the TCB ring.
 *
 * Sleeps until data arrives, a FIN signals EOF, or the connection resets; a
 * pending signal unwinds with -EINTR. Returns the byte count on data, 0 at EOF
 * (FIN with no buffered data), and -1 on a reset/closed connection with no
 * data.
 */
int socket_tcp_recv(struct socket *sock, uint8_t *buf, int len)
{
    struct tcb *tcb;
    uint32_t avail, copy_len, i;
    int rc;

    if (!sock || !sock->tcb) return -1;
    tcb = sock->tcb;

    rc = wq_wait_event(&sock->waiters, tcp_recv_ready, sock);
    if (rc < 0) {
        return rc;
    }

    avail = tcb->recv_tail - tcb->recv_head;
    if (avail == 0 && !tcb->fin_received && tcb->state == TCP_CLOSED) {
        return -1;              /* connection reset/closed with no data */
    }
    if (avail == 0) return 0;   /* EOF (FIN, no buffered data) */

    copy_len = avail;
    if (copy_len > (uint32_t)len) copy_len = (uint32_t)len;

    for (i = 0; i < copy_len; i++) {
        buf[i] = tcb->recv_buf[(tcb->recv_head + i) & (TCP_RING_SIZE - 1)];
    }
    tcb->recv_head += copy_len;
    return (int)copy_len;
}

/**
 * Non-blocking stream receive: drain the ring, else report status.
 *
 * Never sleeps. With no buffered data: 0 on a half-closed peer (FIN), -1 on a
 * reset/closed connection, and SOCK_EAGAIN otherwise.
 */
int socket_tcp_try_recv(struct socket *sock, uint8_t *buf, int len)
{
    struct tcb *tcb;
    uint32_t avail, copy_len, i;

    if (!sock || !sock->tcb) return -1;
    tcb = sock->tcb;

    avail = tcb->recv_tail - tcb->recv_head;
    if (avail == 0) {
        if (tcb->fin_received)        return 0;            /* EOF */
        if (tcb->state == TCP_CLOSED) return -1;           /* reset/closed */
        return SOCK_EAGAIN;                                /* no data yet */
    }

    copy_len = avail;
    if (copy_len > (uint32_t)len) copy_len = (uint32_t)len;
    for (i = 0; i < copy_len; i++) {
        buf[i] = tcb->recv_buf[(tcb->recv_head + i) & (TCP_RING_SIZE - 1)];
    }
    tcb->recv_head += copy_len;
    return (int)copy_len;
}

void socket_tcp_fin(struct socket *sock)
{
    if (!sock) return;

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

    if (sock->type == SOCK_DGRAM) {
        if (sock->recv_len != 0) {
            revents |= (short)(events & POLLIN);
        }
        revents |= (short)(events & POLLOUT); /* a datagram socket can always send */
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
        if (tcb->state == TCP_ESTABLISHED) {
            revents |= (short)(events & POLLOUT); /* connected -> writable */
        }
        if (tcb->so_error != 0) {
            /* A failed connection: report the error, and POLLOUT so a poll(POLLOUT)
             * on a non-blocking connect wakes and the caller then reads SO_ERROR. */
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
