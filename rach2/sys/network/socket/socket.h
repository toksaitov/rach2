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

#define SOCK_STREAM         1 /* TCP */
#define SOCK_DGRAM          2 /* UDP */
/* A descriptor that accepts the interface ioctls and nothing else. A DHCP
 * client opens one to ask this machine about itself (busybox's
 * `udhcp_read_interface`); this stack has no raw IP path, so every other
 * operation on it is refused, not answered with silence. */
#define SOCK_RAW            3
#define AF_INET             2 /* IPv4 address family */
/* Format: one datagram of the Ethernet MTU is the most UDP can carry here. */
#define SOCK_RECV_BUF_SIZE  1500
/* Policy: a UDP socket queues eight datagrams, twelve kilobytes of its
 * record: room for the answers to the two questions a resolver asks at
 * once, A and AAAA, which arrive together, and for a burst of others while
 * the reader is being scheduled. */
#define SOCK_DGRAM_SLOTS    8
/* Policy: a listener holds fifteen accepted connections not yet taken by
 * `accept`, a ring of sixteen in the socket record with one place left
 * empty to tell full from empty; a power of two, since the ring's indexes
 * wrap by a mask. A SYN that arrives with the ring full is answered with a
 * reset before anything is allocated for it. */
#define SOCK_ACCEPT_BACKLOG 16

/* Returned by socket_try_recv when no datagram is waiting (Linux EAGAIN). */
#define SOCK_EAGAIN (-11)

struct tcb; /* defined in tcp/tcp.h */

typedef struct socket {
    struct socket *next; /* the list of live sockets */
    int in_use;
    int refcount; /* descriptors referencing this socket */
    int type;     /* SOCK_DGRAM, SOCK_STREAM or SOCK_RAW */
    /* AF_INET, or AF_PACKET for the tap above the Ethernet layer. Nearly every
     * path below is AF_INET's; the family is what the few that are not branch
     * on, not a second socket pool. */
    uint16_t family;
    uint16_t local_port;

    /* AF_PACKET only: the EtherType this socket sends and receives, in HOST
     * order, or 0 for none. Linux takes it in network order at `socket` and
     * again in the `sockaddr_ll` at `bind`, and the later one wins; a socket
     * whose protocol is 0 receives nothing, which is what a client that asked
     * for nothing asked for. */
    uint16_t protocol;

    /* The hop limit this socket's datagrams leave with. IP_TTL sets it, which
     * is the whole of what traceroute does to a socket, and it applies to a
     * UDP send and a raw send alike. */
    uint8_t ip_ttl;

    /* Remote endpoint: a stream's peer, or the address a datagram socket was
     * connected to. A connected datagram socket sends there without being told
     * again, and receives from nobody else, which is what `connect` means for
     * UDP and what a DHCP client's renewal uses. */
    uint32_t remote_ip;
    uint16_t remote_port;
    int connected;

    /* The three options a program really sets on a socket here. `broadcast` is
     * required before a datagram may go to a broadcast address, because a
     * program that broadcasts by accident wakes every machine on the wire.
     * `reuseaddr` is recorded and reported and overrides nothing, since this
     * stack never refuses a bind for the reason the flag exists to override.
     * `bound_device` is one interface or none. */
    int broadcast;
    int reuseaddr;
    int bound_device;

    /* TCP control block for SOCK_STREAM sockets (NULL for SOCK_DGRAM). */
    struct tcb *tcb;

    /* Passive-open accept queue (circular, SOCK_STREAM listeners). */
    struct socket *accept_queue[SOCK_ACCEPT_BACKLOG];
    int accept_head, accept_tail;
    int listening;

    /* Set by shutdown(2), which detaches the block: a read of the socket
     * afterwards is the end of input, where a stream that never connected
     * has no block either and answers ENOTCONN. */
    int shut_down;

    /*
     * Identity, for the files that report sockets.
     *
     * `inode` is this socket's number in the one namespace `/proc/net/tcp`,
     * `/proc/<pid>/fd/N` and `fstat` all name it by; before this they printed a
     * constant, so every socket on the machine looked like the same one. `uid`
     * is who owns it, taken in syscall context where there is a caller to ask.
     * A passive connection is allocated in receive context, where there is not,
     * and inherits the listener's uid: that is whose socket it is.
     */
    uint32_t inode;
    uint32_t uid;

    /* UDP receive queue: the datagrams in arrival order, each with its
     * length and its sender, which recvfrom returns; `recv_head` is the
     * oldest and `recv_count` how many wait. A datagram that finds the queue
     * full is dropped, as Linux drops one that finds the receive buffer full.
     * Occupancy is the count and not a length, because a datagram of no bytes
     * is a datagram: a read of this socket may return 0, which is a datagram
     * and not end of input. */
    uint8_t recv_buf[SOCK_DGRAM_SLOTS][SOCK_RECV_BUF_SIZE];
    uint16_t recv_len[SOCK_DGRAM_SLOTS];
    uint32_t recv_src_ip[SOCK_DGRAM_SLOTS];
    uint16_t recv_src_port[SOCK_DGRAM_SLOTS];
    int recv_head;
    int recv_count;

    /* Processes blocked waiting for data, a connection, or an accept. The
     * IRQ-side deliver/accept/connect/fin routines wake every waiter; each
     * re-checks its own condition. */
    wait_queue_t waiters;
} socket_t;

/** Start with no sockets; called once from init_network. */
void socket_init(void);

/** Allocate a socket with refcount 1; NULL if the pool is full. */
struct socket *socket_alloc(int type);

/** An inode number for a socket of either family: rising, nonzero, and not
 * one a live socket has. */
uint32_t socket_inode_next(void);

/** Add a reference (fork/dup2 sharing). */
void socket_ref(struct socket *sock);

/** Drop a reference; tear the socket down at the last one. */
void socket_unref(struct socket *sock);

/** Copy an incoming UDP payload into sock and wake any blocked recvfrom. */
void socket_udp_deliver(struct socket *sock, uint32_t src_ip, uint16_t src_port,
                        const uint8_t *data, uint16_t len);

/** Blocking UDP receive; returns the byte count or a negative errno. */
int socket_recv(struct socket *sock, uint8_t *buf, int len, uint32_t *src_ip, uint16_t *src_port);

/** Non-blocking variant; SOCK_EAGAIN if no datagram is waiting. */
int socket_try_recv(struct socket *sock, uint8_t *buf, int len, uint32_t *src_ip,
                    uint16_t *src_port);

/*
 * The same receive taken in three steps, for a caller with more than one
 * destination to fill. `readv` is the caller: a datagram is one message, so
 * reading twice would take a second one and copying it out once would drop
 * whatever did not fit the first buffer. Waiting, copying piece by piece, and
 * consuming are therefore separable, and `socket_recv` above is the three of
 * them in a row over a single buffer.
 *
 * A caller must hold `net_lock` across every copy it makes AND the consume,
 * whether it takes the message in one piece or several. A syscall body runs
 * with interrupts enabled here (see net.h), so the receive interrupt would
 * otherwise put the next datagram in the slot in between: a several-piece
 * caller would take a prefix of one message followed by bytes of another, and
 * a one-piece caller would return one message's bytes with the other one's
 * sender while clearing a datagram nobody read. Each of these takes the lock
 * itself as well, which nests safely.
 */

/** Wait for a datagram. Returns 0, or a negative errno (-EINTR on a signal). */
int socket_dgram_wait(struct socket *sock);

/** Bytes in the waiting datagram, or -1 when the slot is empty. */
int socket_dgram_pending(struct socket *sock);

/**
 * Copy up to `len` bytes of the waiting datagram from `offset` bytes into it,
 * leaving it in place. Returns the bytes copied, or -1 with no datagram.
 */
int socket_dgram_peek(struct socket *sock, uint32_t offset, uint8_t *buf, int len);

/** Drop the waiting datagram, reporting its sender through the two pointers. */
void socket_dgram_consume(struct socket *sock, uint32_t *src_ip, uint16_t *src_port);

/* TCP-side helpers, implemented with the TCP layer. */
void socket_tcp_accept(struct socket *listen_sock, struct socket *conn_sock);
void socket_tcp_connected(struct socket *sock);
void socket_tcp_writable(struct socket *sock);
/** Blocking stream send; SOCK_EAGAIN when nonblock and the send ring is
 * full. */
int socket_tcp_send(struct socket *sock, const uint8_t *buf, int len, int nonblock);
/** Copy in-order stream bytes into the recv ring; returns the count accepted
 * (less than `len` if the ring was nearly full, so the caller advances rcv_nxt
 * by only what fit and the peer retransmits the rest). */
uint16_t socket_tcp_deliver(struct socket *sock, const uint8_t *data, uint16_t len);
/** Keep stream bytes that arrived past a hole `offset` bytes past the recv
 * ring's tail, unreadable; returns how many fit in the ring's free space. */
uint16_t socket_tcp_hold(struct socket *sock, uint32_t offset, const uint8_t *data, uint16_t len);
/** Make `n` bytes kept past the recv ring's tail readable, and wake a
 * reader. */
void socket_tcp_release(struct socket *sock, uint32_t n);
int socket_tcp_recv(struct socket *sock, uint8_t *buf, int len);
/** Non-blocking variant; SOCK_EAGAIN if no data is buffered (MSG_DONTWAIT). */
int socket_tcp_try_recv(struct socket *sock, uint8_t *buf, int len);
void socket_tcp_fin(struct socket *sock);

/** Map socket state to poll revents; sets *wq to the wait queue if non-NULL. */
short socket_poll(struct socket *sock, short events, struct wait_queue **wq);

#endif /* _SOCKET_H_ */
