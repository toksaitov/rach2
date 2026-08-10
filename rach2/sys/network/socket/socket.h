#ifndef _SOCKET_H_
#define _SOCKET_H_

#include "../../lib/kint.h"
#include "../../proc/process.h"
#include "../../proc/wait_queue.h"

/*
 * Socket abstraction. A socket is the kernel object behind one (or more) user
 * file descriptors of type FD_TYPE_SOCKET. SOCK_DGRAM (UDP) and SOCK_STREAM
 * (TCP) are both supported.
 *
 * The pool is a fixed array; socket_alloc/socket_ref/socket_unref manage it
 * with a reference count, so a socket shared by fork() or dup2() is only torn
 * down when its last descriptor closes.
 */

#define SOCK_STREAM          1      /* TCP */
#define SOCK_DGRAM           2      /* UDP */
#define AF_INET              2      /* IPv4 address family */
#define MAX_SOCKETS          16    /* static pool size */
#define SOCK_RECV_BUF_SIZE   1500  /* one max-size UDP datagram */
#define SOCK_ACCEPT_BACKLOG  16     /* queued connections (power of two) */

/* Returned by socket_try_recv when no datagram is waiting (Linux EAGAIN). */
#define SOCK_EAGAIN  (-11)

struct tcb; /* defined in tcp/tcp.h */

typedef struct socket {
    int      in_use;
    int      refcount;        /* descriptors referencing this socket */
    int      type;            /* SOCK_DGRAM or SOCK_STREAM */
    uint16_t local_port;

    /* Remote endpoint (set for SOCK_STREAM connections). */
    uint32_t remote_ip;
    uint16_t remote_port;

    /* TCP control block for SOCK_STREAM sockets (NULL for SOCK_DGRAM). */
    struct tcb *tcb;

    /* Passive-open accept queue (circular, SOCK_STREAM listeners). */
    struct socket *accept_queue[SOCK_ACCEPT_BACKLOG];
    int accept_head, accept_tail;
    int listening;

    /* UDP: sender of the most recent datagram, returned via recvfrom. */
    uint32_t last_src_ip;
    uint16_t last_src_port;

    /* UDP single-datagram receive buffer. Occupancy is `recv_full` and not the
     * length, because a datagram of no bytes is a datagram: UDP carries an
     * empty payload perfectly well, and a slot that read its own emptiness as
     * "nothing here" would build one, send it, accept it, deliver it to the
     * right socket, and then drop it at the last step, leaving a blocked
     * `recvfrom` blocked. So a read of this socket may return 0, which is a
     * datagram and not end of input. */
    uint8_t  recv_buf[SOCK_RECV_BUF_SIZE];
    uint16_t recv_len;
    int      recv_full;

    /* Processes blocked waiting for data, a connection, or an accept. The
     * IRQ-side deliver/accept/connect/fin routines wake every waiter; each
     * re-checks its own condition. */
    wait_queue_t waiters;
} socket_t;

/** Reserve the socket pool; called once from init_network. */
void socket_init(void);

/** Allocate a socket with refcount 1; NULL if the pool is full. */
struct socket *socket_alloc(int type);

/** Add a reference (fork/dup2 sharing). */
void socket_ref(struct socket *sock);

/** Drop a reference; tear the socket down at the last one. */
void socket_unref(struct socket *sock);

/** Copy an incoming UDP payload into sock and wake any blocked recvfrom. */
void socket_udp_deliver(struct socket *sock,
                        uint32_t src_ip, uint16_t src_port,
                        const uint8_t *data, uint16_t len);

/** Blocking UDP receive; returns the byte count or a negative errno. */
int socket_recv(struct socket *sock, uint8_t *buf, int len,
                uint32_t *src_ip, uint16_t *src_port);

/** Non-blocking variant; SOCK_EAGAIN if no datagram is waiting. */
int socket_try_recv(struct socket *sock, uint8_t *buf, int len,
                    uint32_t *src_ip, uint16_t *src_port);

/* TCP-side helpers, implemented with the TCP layer. */
void socket_tcp_accept(struct socket *listen_sock, struct socket *conn_sock);
void socket_tcp_connected(struct socket *sock);
void socket_tcp_writable(struct socket *sock);
/** Blocking stream send; SOCK_EAGAIN when nonblock and the send ring is full. */
int  socket_tcp_send(struct socket *sock, const uint8_t *buf, int len, int nonblock);
/** Copy in-order stream bytes into the recv ring; returns the count accepted
 * (less than `len` if the ring was nearly full, so the caller advances rcv_nxt
 * by only what fit and the peer retransmits the rest). */
uint16_t socket_tcp_deliver(struct socket *sock, const uint8_t *data, uint16_t len);
int  socket_tcp_recv(struct socket *sock, uint8_t *buf, int len);
/** Non-blocking variant; SOCK_EAGAIN if no data is buffered (MSG_DONTWAIT). */
int  socket_tcp_try_recv(struct socket *sock, uint8_t *buf, int len);
void socket_tcp_fin(struct socket *sock);

/** Map socket state to poll revents; sets *wq to the wait queue if non-NULL. */
short socket_poll(struct socket *sock, short events, struct wait_queue **wq);

#endif /* _SOCKET_H_ */
