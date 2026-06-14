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
#include "../../serial.h"
#include "../../lib/kstring.h"
#include "../../mm/heap.h"

/* Socket pool, allocated from kernel memory at init (kept out of BSS). */
static socket_t *socket_pool;

void socket_init(void)
{
    socket_pool = (socket_t *)mem_reserve(sizeof(socket_t) * MAX_SOCKETS, true);
    if (socket_pool) {
        k_memset(socket_pool, 0, sizeof(socket_t) * MAX_SOCKETS);
    }
}

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
            socket_pool[i].waiting_proc  = NULL;
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

    /* Last reference: tear down protocol state and return the slot. */
    if (sock->type == SOCK_DGRAM && sock->local_port != 0) {
        udp_unbind(sock->local_port);
    } else if (sock->type == SOCK_STREAM && sock->tcb) {
        /* tcp_close detaches the TCB so it can finish the FIN handshake on
         * its own after the socket slot is freed. */
        tcp_close(sock);
    }

    sock->in_use = 0;
    net_unlock(flags);
}

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

    if (sock->waiting_proc) {
        sock->waiting_proc->state = PROC_READY;
        sock->waiting_proc = NULL;
    }
}

int socket_recv(struct socket *sock, uint8_t *buf, int len,
                uint32_t *src_ip, uint16_t *src_port)
{
    int copy_len;

    /* Register as waiter and block. If a datagram already arrived
     * (recv_len > 0), the loop body never runs. */
    current_proc->state = PROC_BLOCKED;
    sock->waiting_proc  = current_proc;

    while (sock->recv_len == 0) {
        __asm__ __volatile__("sti; hlt");
    }

    sock->waiting_proc  = NULL;
    current_proc->state = PROC_RUNNING;

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
    return copy_len;
}

int socket_try_recv(struct socket *sock, uint8_t *buf, int len,
                    uint32_t *src_ip, uint16_t *src_port)
{
    int copy_len;

    if (sock->recv_len == 0) {
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
    return copy_len;
}

/*
 * TCP-side helpers, all called from tcp_rx in IRQ context except
 * socket_tcp_recv, which is the blocking syscall path.
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

    if (listen_sock->waiting_proc) {
        listen_sock->waiting_proc->state = PROC_READY;
        listen_sock->waiting_proc = NULL;
    }
}

void socket_tcp_connected(struct socket *sock)
{
    if (!sock) return;

    if (sock->waiting_proc) {
        sock->waiting_proc->state = PROC_READY;
        sock->waiting_proc = NULL;
    }
}

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

    if (sock->waiting_proc) {
        sock->waiting_proc->state = PROC_READY;
        sock->waiting_proc = NULL;
    }
}

int socket_tcp_recv(struct socket *sock, uint8_t *buf, int len)
{
    struct tcb *tcb;
    uint32_t avail, copy_len, i;

    if (!sock || !sock->tcb) return -1;
    tcb = sock->tcb;

    current_proc->state = PROC_BLOCKED;
    sock->waiting_proc  = current_proc;

    /* Block until data arrives, a FIN signals EOF, or the connection resets. */
    while ((tcb->recv_tail - tcb->recv_head) == 0 && !tcb->fin_received) {
        if (tcb->state == TCP_CLOSED) {
            sock->waiting_proc  = NULL;
            current_proc->state = PROC_RUNNING;
            return -1;
        }
        __asm__ __volatile__("sti; hlt");
    }

    sock->waiting_proc  = NULL;
    current_proc->state = PROC_RUNNING;

    avail = tcb->recv_tail - tcb->recv_head;
    if (avail == 0) return 0;   /* EOF */

    copy_len = avail;
    if (copy_len > (uint32_t)len) copy_len = (uint32_t)len;

    for (i = 0; i < copy_len; i++) {
        buf[i] = tcb->recv_buf[(tcb->recv_head + i) & (TCP_RING_SIZE - 1)];
    }
    tcb->recv_head += copy_len;
    return (int)copy_len;
}

int socket_tcp_try_recv(struct socket *sock, uint8_t *buf, int len)
{
    struct tcb *tcb;
    uint32_t avail, copy_len, i;

    if (!sock || !sock->tcb) return -1;
    tcb = sock->tcb;

    /* Non-blocking: with no buffered data, report EOF on a half-closed peer, an
     * error on a reset/closed connection, else EAGAIN -- never block. */
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

    if (sock->waiting_proc) {
        sock->waiting_proc->state = PROC_READY;
        sock->waiting_proc = NULL;
    }
}
