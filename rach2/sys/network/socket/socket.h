#ifndef _SOCKET_H_
#define _SOCKET_H_

#include "../../lib/kint.h"
#include "../../proc/process.h"
#include "../../proc/wait_queue.h"

/*
 * socket: the kernel object behind a descriptor of type FD_TYPE_SOCKET, for
 * AF_INET stream, datagram and raw sockets and for AF_PACKET and AF_NETLINK
 * ones. A socket is allocated from the heap and kept on a list of the live
 * sockets. Its reference count has one for each descriptor that refers to it,
 * which fork, dup and a descriptor passed over a local socket add to, and
 * the socket is torn down when the last goes.
 */

#define SOCK_STREAM         1 /* TCP */
#define SOCK_DGRAM          2 /* UDP */
/* An AF_INET raw socket of IPPROTO_ICMP (raw.h). One opened with
 * IPPROTO_RAW has protocol 0 and accepts the interface ioctls and nothing
 * else: a DHCP client opens one to ask this machine about itself (busybox's
 * `udhcp_read_interface`), and every other operation on it is refused. */
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

/* What a receive or a stream send that does not wait returns when it would
 * have to wait: EAGAIN, negated. */
#define SOCK_EAGAIN (-11)

struct tcb; /* defined in tcp/tcp.h */

typedef struct socket {
    struct socket *next; /* the list of live sockets */
    int in_use;
    int refcount; /* descriptors, and a listener's queue before accept */
    int type;     /* SOCK_DGRAM, SOCK_STREAM or SOCK_RAW */
    /* AF_INET, AF_PACKET for the tap above the Ethernet layer (packet.h), or
     * AF_NETLINK for the kernel's routing answers (netlink.h). The three
     * families share this record, and the code of the other two branches on
     * it. */
    uint16_t family;
    uint16_t local_port;

    /* The address `bind` gave: one of this machine's, 127/8 among them, or 0
     * for any. A stream or datagram socket bound to one is reached at that
     * address alone, so a server bound to 127.0.0.1 answers this machine and
     * nobody on the wire. */
    uint32_t local_ip;

    /* For AF_PACKET, the EtherType this socket sends and receives, in host
     * order, or 0 for none. Linux takes it in network order at `socket` and
     * again in the `sockaddr_ll` at `bind`, where a nonzero one replaces it; a
     * socket whose protocol is 0 receives nothing, which is what a client
     * that asked for nothing asked for. For an AF_INET raw socket,
     * IPPROTO_ICMP, or 0 for the descriptor that takes only the interface
     * ioctls. */
    uint16_t protocol;

    /* The hop limit of this socket's datagrams, IP_TTL_DEFAULT until the
     * IP_TTL option sets it, as traceroute does. A UDP send and a raw send
     * both use it. */
    uint8_t ip_ttl;

    /* Remote endpoint: a stream's peer, or the address a datagram socket was
     * connected to. A connected datagram socket sends there without being told
     * again, and receives from nobody else, which is what `connect` means for
     * UDP and what a DHCP client's renewal uses. */
    uint32_t remote_ip;
    uint16_t remote_port;
    int connected;

    /* The error an ICMP message reported for a connected datagram socket's
     * peer, ECONNREFUSED for a port unreachable, or 0: the next receive,
     * send or SO_ERROR reports it once, and poll reports POLLERR while it
     * waits, as Linux does. */
    int so_error;

    /* Three socket options. `broadcast`, SO_BROADCAST, is required before a
     * UDP datagram may go to a broadcast address, because a program that
     * broadcasts by accident reaches every machine on the wire. `reuseaddr`,
     * SO_REUSEADDR, lets UDP sockets share a port (`udp_bind`).
     * `bound_device`, SO_BINDTODEVICE, is an interface's index, or 0 for
     * none, and is only reported back by getsockopt. */
    int broadcast;
    int reuseaddr;
    int bound_device;

    /* The TCP control block of a stream socket. NULL for the other types,
     * and for a stream before `connect` or `listen` or once the block has
     * been detached from it (tcp.h). */
    struct tcb *tcb;

    /* A listener's connections not yet accepted: a ring with `accept_head`
     * the oldest and `accept_tail` the next free place, empty when the two
     * are equal. */
    struct socket *accept_queue[SOCK_ACCEPT_BACKLOG];
    int accept_head, accept_tail;
    int listening;

    /* Set by shutdown(SHUT_RDWR), which detaches the block: a read of the
     * socket afterwards is the end of input, where a stream that never
     * connected has no block either and answers ENOTCONN. */
    int shut_down;

    /*
     * Identity, for the files that report sockets. `inode` is this socket's
     * number in the one namespace `/proc/net/tcp`, `/proc/<pid>/fd/N` and
     * `fstat` all report it by (`socket_inode_next`). `uid` is the owner: the
     * effective uid of the process that made the socket, or for a passive
     * connection, which is allocated in receive context with no process to
     * ask, the listener's.
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

    /* The processes and pollers waiting for data, room, a connection or an
     * accept. A wake from the receive path wakes every one; each checks its
     * own condition again. */
    wait_queue_t waiters;

    /* AF_NETLINK only: the socket's port id and its queue of answers
     * (netlink.c). */
    void *netlink;
    /* The network namespace the socket was made in (proc/ns.h): NULL for
     * the first, whose interfaces alone a socket reaches, and for every
     * netlink socket; `net_gone` in socket.c once its namespace has ended. */
    const struct netns *ns;
} socket_t;

/** Start with no sockets; called once from init_network. */
void socket_init(void);

/**
 * Allocate a socket of `type`, SOCK_STREAM, SOCK_DGRAM or SOCK_RAW, from the
 * heap and put it on the list: AF_INET, one reference, the default hop limit
 * and a new inode number. Returns NULL with no memory. The caller sets the
 * family, the protocol and the owner's uid, since whether there is a process
 * to take the uid from depends on the caller's context. Takes `net_lock`
 * itself, and may be called from the receive path, where an incoming SYN
 * allocates one.
 */
struct socket *socket_alloc(int type);

/** Whether `s` was made in a network namespace other than the first: it
 * reaches no interface, binds to nothing a packet is delivered to, and
 * neither connects nor sends (`do_bind` and the rest in syscall_net.c). */
int socket_outside(const struct socket *s);

/**
 * Whether a live stream socket has `port` as its local port, bound or
 * connected, which an ephemeral port is never chosen over. The caller holds
 * `net_lock`, which the list of sockets is shared under.
 */
int socket_port_bound(uint16_t port);

/* One datagram or raw socket, copied out for /proc/net/udp and
 * /proc/net/raw. A raw socket's `local_port` is its protocol, 0 for the
 * descriptor that only takes configuration ioctls. */
struct socket_row {
    uint32_t local_ip;
    uint16_t local_port;
    uint32_t remote_ip;
    uint16_t remote_port;
    int connected;
    uint32_t rxq; /* bytes queued to read */
    uint32_t uid;
    uint32_t inode;
};

/**
 * Copy the AF_INET sockets of `type`, SOCK_DGRAM or SOCK_RAW, in the calling
 * process's network namespace into an array from the heap at `*rows`, and
 * return the row count. With no socket of the type, `*rows` is NULL and the
 * count 0; otherwise the caller frees the array, even with no rows. Returns
 * -1 when the array cannot be allocated. Takes `net_lock` itself.
 */
int socket_snapshot(int type, struct socket_row **rows);

/** How many live sockets there are of `type` in AF_INET, or of every type
 * and family with `type` 0, in every network namespace. */
int socket_count(int type);

/** A new inode number for a socket of this list or a local one
 * (ipc/unix.c): rising, nonzero, and not one a live socket of either kind
 * has. Takes `net_lock` itself. */
uint32_t socket_inode_next(void);

/** Add a reference, for a descriptor that comes to share the socket. NULL
 * is ignored. */
void socket_ref(struct socket *sock);

/**
 * Drop a reference, and at the last tear the socket down: free a netlink
 * socket's state, remove a packet, raw or UDP socket's binding, or close a
 * stream's connection (`tcp_close`, after which the block finishes the close
 * on its own); release a listener's connections not yet accepted; and take
 * the socket off the list and free it. NULL is ignored. Takes `net_lock`
 * itself, and may be called from the receive path.
 */
void socket_unref(struct socket *sock);

/**
 * Queue the UDP payload of `len` bytes at `data`, from `src_ip`:`src_port`,
 * on `sock` and wake its readers. A payload past SOCK_RECV_BUF_SIZE bytes is
 * cut to that size. A datagram that finds all SOCK_DGRAM_SLOTS slots full,
 * or that comes from other than the peer of a connected socket, is dropped.
 * Called with interrupts off, from `udp_rx`.
 */
void socket_udp_deliver(struct socket *sock, uint32_t src_ip, uint16_t src_port,
                        const uint8_t *data, uint16_t len);

/**
 * Wait for a datagram on a UDP, raw, packet or netlink socket, copy up to
 * `len` bytes of it to `buf`, and report its sender through `src_ip` and
 * `src_port` when they are not NULL, as `socket_dgram_consume` reports it.
 * The rest of a longer datagram is discarded, since the datagram is the
 * unit; with `peek` the datagram stays queued, whole, for the next receive
 * (MSG_PEEK). Returns the bytes copied, -EINTR, or the socket's pending
 * error, which this takes. The copy runs with interrupts off, so `buf`, a
 * user buffer, must have been faulted in (`user_buffer_ok`).
 */
int socket_recv(struct socket *sock, uint8_t *buf, int len, uint32_t *src_ip, uint16_t *src_port,
                int peek);

/** `socket_recv` without the wait: the pending error first, then
 * SOCK_EAGAIN when no datagram is waiting. */
int socket_try_recv(struct socket *sock, uint8_t *buf, int len, uint32_t *src_ip,
                    uint16_t *src_port, int peek);

/*
 * The same receive in three steps, wait, copy and consume, for a caller with
 * more than one destination to fill: `readv_dgram` in syscall_fs.c, which
 * readv and recvmsg use. A datagram is one message, so a second receive
 * would take the next one, and a copy into the first destination alone would
 * drop what did not fit. `socket_recv` is the three steps in a row over one
 * buffer. These serve every family `socket_recv` does.
 *
 * The caller holds `net_lock` across every copy it makes and the consume, so
 * that the bytes copied, the sender reported and the datagram consumed are
 * all of the one at the head of the queue. A system call runs with
 * interrupts enabled (net.h). Each function takes the lock itself as well,
 * which nests.
 */

/** Wait for a datagram. Returns 0, or a negative errno: -EINTR on a signal,
 * or the socket's pending error, which this takes. */
int socket_dgram_wait(struct socket *sock);

/** Take the socket's pending error: its negative errno, or 0 for none. */
int socket_dgram_error(struct socket *sock);

/** For a receive that does not wait: the pending error, which this takes,
 * else -EAGAIN when no datagram waits, else 0. */
int socket_dgram_nowait(struct socket *sock);

/** Bytes in the oldest waiting datagram, or -1 when none waits. A datagram
 * of no bytes is 0, not emptiness. */
int socket_dgram_pending(struct socket *sock);

/**
 * Copy up to `len` bytes of the oldest waiting datagram, from `offset` bytes
 * into it, leaving it in place. Returns the bytes copied, 0 for a copy that
 * starts at or past the datagram's end or a negative `len`, or -1 with no
 * datagram. The copy runs with interrupts off, so a user buffer must have
 * been faulted in.
 */
int socket_dgram_peek(struct socket *sock, uint32_t offset, uint8_t *buf, int len);

/**
 * Drop the oldest waiting datagram, with whatever of it the caller did not
 * copy, and report its sender through the two pointers when they are not
 * NULL: for UDP its address and port; for a raw socket the source address of
 * its IP header and port 0; and 0 for both for a packet or netlink socket,
 * whose link address `packet_last_addr` reports. With no datagram waiting, a
 * UDP socket reports nothing.
 */
void socket_dgram_consume(struct socket *sock, uint32_t *src_ip, uint16_t *src_port);

/** Report the oldest waiting datagram's sender as `socket_dgram_consume`
 * does, 0 for both with none waiting, and leave the datagram in place. A
 * packet socket keeps its link address for `packet_last_addr`. */
void socket_dgram_sender(struct socket *sock, uint32_t *src_ip, uint16_t *src_port);

/*
 * The stream side. The TCP layer calls `socket_tcp_accept`,
 * `socket_tcp_connected`, `socket_tcp_writable`, `socket_tcp_deliver`,
 * `socket_tcp_hold`, `socket_tcp_release` and `socket_tcp_fin` with
 * interrupts off, from its receive path, its timers and a change of this
 * machine's addresses; the send and the receives are for system calls. Each
 * ignores a NULL socket, and a send or a receive returns -1 for one.
 */

/** Queue the established connection `conn_sock` on the listener
 * `listen_sock` and wake its waiters. With the queue full the connection is
 * closed and freed instead (`socket_unref`). */
void socket_tcp_accept(struct socket *listen_sock, struct socket *conn_sock);
/** Wake every waiter of `sock` when its connection is established or ends. */
void socket_tcp_connected(struct socket *sock);
/** Wake a writer or a poller for output when an ACK frees send-ring room. */
void socket_tcp_writable(struct socket *sock);
/**
 * Queue the `len` bytes at `buf` on the stream, waiting for room in the send
 * ring as long as none is free, and return the count queued, never 0 for a
 * nonzero `len`. Returns SOCK_EAGAIN instead of waiting when `nonblock` is
 * set, -EINTR on a signal, and on a connection no longer in a sending state
 * the error recorded for it or -EPIPE.
 */
int socket_tcp_send(struct socket *sock, const uint8_t *buf, int len, int nonblock);
/** Copy in-order stream bytes into the receive ring and wake a reader;
 * returns the count taken, less than `len` when the ring was nearly full, so
 * that the caller advances `rcv_nxt` by only what fit and the peer sends the
 * rest again. */
uint16_t socket_tcp_deliver(struct socket *sock, const uint8_t *data, uint16_t len);
/** Keep stream bytes that arrived past a hole, `offset` bytes past the
 * receive ring's tail, unreadable until `socket_tcp_release`; returns how
 * many fit in the ring's free space. */
uint16_t socket_tcp_hold(struct socket *sock, uint32_t offset, const uint8_t *data, uint16_t len);
/** Make `n` bytes kept past the receive ring's tail readable, once the hole
 * before them is filled, and wake a reader. */
void socket_tcp_release(struct socket *sock, uint32_t n);
/**
 * Wait for stream bytes, the peer's FIN or the connection's end, and copy up
 * to `len` bytes to `buf`. Returns the count copied; 0 at the end of input,
 * the FIN with nothing buffered or a block a shutdown detached; with
 * nothing buffered on a reset or closed connection, the error recorded for
 * it or -ECONNRESET; -ENOTCONN for a stream that never connected; or -EINTR.
 * With `peek` the bytes stay in the ring for the next receive (MSG_PEEK),
 * and the window does not change.
 */
int socket_tcp_recv(struct socket *sock, uint8_t *buf, int len, int peek);
/** `socket_tcp_recv` without the wait (MSG_DONTWAIT): SOCK_EAGAIN where it
 * would wait. */
int socket_tcp_try_recv(struct socket *sock, uint8_t *buf, int len, int peek);
/** Wake every waiter of `sock` on the peer's FIN or the connection's end. */
void socket_tcp_fin(struct socket *sock);

/**
 * The events among `events` that `sock` is ready for now, with POLLERR and
 * POLLHUP whether asked for or not, and POLLNVAL for a NULL socket. Sets
 * `*wq` to the socket's wait queue when `wq` is not NULL, for the poller to
 * sleep on.
 */
short socket_poll(struct socket *sock, short events, struct wait_queue **wq);

#endif /* _SOCKET_H_ */
