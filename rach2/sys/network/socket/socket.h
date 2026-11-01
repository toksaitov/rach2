#ifndef _SOCKET_H_
#define _SOCKET_H_

#include "../../lib/kint.h"
#include "address.h"
#include "../../proc/process.h"
#include "../../proc/wait_queue.h"

/*
 * socket: the kernel object behind a descriptor of type FD_TYPE_SOCKET, for
 * AF_INET and AF_INET6 stream, datagram and raw sockets and for AF_PACKET and AF_NETLINK
 * ones. A socket is allocated from the heap and kept on a list of the live
 * sockets. Its reference count has one for each descriptor that refers to it,
 * which fork, dup and a descriptor passed over a local socket add to, and
 * the socket is torn down when the last goes.
 */

#define SOCK_STREAM         1 /* TCP */
#define SOCK_DGRAM          2 /* UDP */
/* An AF_INET raw socket of IPPROTO_ICMP, or an AF_INET6 one of
 * IPPROTO_ICMPV6 (raw.h). An AF_INET one opened with
 * IPPROTO_RAW has protocol 0 and accepts the interface ioctls and nothing
 * else: a DHCP client opens one to ask this machine about itself (busybox's
 * `udhcp_read_interface`), and every other operation on it is refused. */
#define SOCK_RAW            3
#define AF_INET             2  /* IPv4 address family */
#define AF_INET6            10 /* IPv6 address family */
/* Linux's default and largest receive budget of a datagram socket, its
 * `net.core.rmem_default` and `rmem_max`, and the least SO_RCVBUF leaves
 * it, SOCK_MIN_RCVBUF. SO_RCVBUF doubles what it is given, as Linux does, to
 * cover each datagram's record; a socket's budget is `rcvbuf`. */
#define SOCK_RCVBUF_DEFAULT 212992
#define SOCK_RCVBUF_MAX     212992
#define SOCK_RCVBUF_MIN     2304
/* Policy: a listener holds fifteen accepted connections not yet taken by
 * `accept`, a ring of sixteen in the socket record with one place left
 * empty to tell full from empty; a power of two, since the ring's indexes
 * wrap by a mask. A SYN that arrives with the ring full is answered with a
 * reset before anything is allocated for it. */
#define SOCK_ACCEPT_BACKLOG 16

/* What a receive or a stream send that does not wait returns when it would
 * have to wait: EAGAIN, negated. */
#define SOCK_EAGAIN (-11)

/* Policy: an AF_INET6 socket holds at most eight multicast memberships
 * (IPV6_JOIN_GROUP), each a group on one interface. */
#define SOCK_GROUPS6 8

/* The ancillary data an AF_INET6 socket asks a receive for, the bits of
 * `rxopt6`: IPV6_RECVPKTINFO, IPV6_RECVHOPLIMIT and IPV6_RECVTCLASS. */
#define SOCK_RX_PKTINFO  0x1
#define SOCK_RX_HOPLIMIT 0x2
#define SOCK_RX_TCLASS   0x4

/*
 * What the network layer saw of a datagram it delivered to a UDP or raw
 * socket, kept beside the datagram for the ancillary data of `recvmsg`.
 * UDP over IPv4 keeps the destination alone: `udp_rx` is not given the
 * interface, the TTL or the type of service, so they are 0, -1 and -1.
 */
struct net_rxinfo {
    net_addr_t dst;   /* the destination the datagram was sent to */
    uint32_t ifindex; /* the interface it came in on, 0 when not known */
    int16_t hlim;     /* its hop limit, or -1 when not known */
    int16_t tclass;   /* its traffic class, or -1 when not known */
};

/*
 * What one send asks beyond the socket's options: the IPV6_PKTINFO,
 * IPV6_HOPLIMIT and IPV6_TCLASS control messages of `sendmsg`.
 */
struct net_txopt {
    net_addr_t src; /* the source to send from; unspecified for the usual */
    int ifindex;    /* the interface to leave by, or 0 */
    int16_t hlim;   /* the hop limit, or -1 for the socket's */
    int16_t tclass; /* the traffic class, or -1 for the socket's */
};

/*
 * One received datagram, from the heap, with what a receive reports of it;
 * its `len` bytes follow the record. A UDP datagram keeps its sender in
 * `src` and `port`; a raw one its IP source in `src`; a packet socket's its
 * link address in `lladdr`, `pkttype` and `llproto`, and its interface in
 * `info.ifindex`. A datagram of no bytes is a datagram.
 */
struct sock_dgram {
    struct sock_dgram *next;
    uint32_t len;
    net_addr_t src;
    uint16_t port;
    struct net_rxinfo info;
    uint8_t lladdr[6];
    uint8_t pkttype;
    uint16_t llproto;
};

/*
 * A queue of received datagrams in arrival order, whole, within a budget
 * of bytes: a datagram is taken while the bytes held, each datagram's
 * record counted with it, are within the socket's `rcvbuf`, and dropped
 * otherwise, as Linux drops one that finds the receive buffer full. So an
 * empty queue takes any datagram, however long.
 */
struct sock_dgq {
    struct sock_dgram *head, *tail;
    uint32_t count; /* datagrams queued */
    uint32_t bytes; /* their payloads' bytes */
    uint32_t held;  /* the heap they hold, records included */
};

/**
 * Append a copy of the `len` bytes at `data` to `q`, with the record
 * `meta`'s fields beside it, under the budget `rcvbuf`. Returns the record
 * queued, or NULL when the budget is spent or there is no memory, which
 * drops the datagram. Called with interrupts off.
 */
struct sock_dgram *sock_dgq_put(struct sock_dgq *q, int rcvbuf, const struct sock_dgram *meta,
                                const uint8_t *data, uint32_t len);

/** The bytes of the datagram `d`. */
#define SOCK_DGRAM_DATA(d) ((uint8_t *)((d) + 1))

/** Unlink the oldest datagram of `q` and return it for the caller to free
 * with `kfree`, or NULL when `q` is empty. Called under `net_lock`. */
struct sock_dgram *sock_dgq_pop(struct sock_dgq *q);

/** Free every datagram of `q` and leave it empty. Called under
 * `net_lock`. */
void sock_dgq_flush(struct sock_dgq *q);

/** Copy up to `len` bytes of the oldest datagram of `q`, from `offset`
 * bytes into it: the count copied, 0 at or past its end or for a negative
 * `len`, or -1 when `q` is empty. Called under `net_lock`. */
int sock_dgq_peek(const struct sock_dgq *q, uint32_t offset, uint8_t *buf, int len);

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

    /* The canonical address bind gave, including an IPv6 link's scope.
     * Unspecified means any address in the binding's family; :: also
     * accepts IPv4 unless v6only is set. */
    net_addr_t local_ip;

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
    net_addr_t remote_ip;
    uint16_t remote_port;
    int connected;
    int v6only; /* IPV6_V6ONLY; zero permits mapped IPv4 peers */

    /*
     * The IPv6 options of an AF_INET6 socket, which its UDP and raw sends
     * and its receives honour (syscall_net.c). `hops6` is IPV6_UNICAST_HOPS,
     * -1 for the default of 64; `mhops6` IPV6_MULTICAST_HOPS, 1 unless set;
     * `tclass6` IPV6_TCLASS, 0 to 255; `mif6` IPV6_MULTICAST_IF, the index of
     * the interface a datagram to a group leaves by when its address names
     * none, or 0; `mloop6` IPV6_MULTICAST_LOOP, whether such a datagram also
     * comes back to this machine's members of the group; `rxopt6` the
     * SOCK_RX_* the receives answer with. `groups6` are the `ngroups6`
     * groups IPV6_JOIN_GROUP made the socket a member of, each holding one
     * join of the interface, which the socket's end gives back.
     */
    int16_t hops6;
    int16_t mhops6;
    int16_t tclass6;
    uint8_t mloop6;
    uint8_t rxopt6;
    int mif6;
    struct {
        uint8_t group[16];
        int ifindex;
    } groups6[SOCK_GROUPS6];
    int ngroups6;

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
     * none, also used as the scope of an IPv6 endpoint with no scope id. */
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

    /* The UDP receive queue (`sock_dgq`); a raw or packet socket keeps its
     * own beside its registration, under the same budget. `rcvbuf` is
     * SO_RCVBUF, the bytes a datagram socket's queue may hold. */
    struct sock_dgq rq;
    int rcvbuf;

    /* IP_MTU_DISCOVER, an IP_PMTUDISC_* value: whether an IPv4 datagram
     * past its route's MTU leaves in fragments or is refused, and whether
     * one within it carries DF (`ip_tx_opts`). */
    uint8_t pmtudisc;

    /* What the network layer saw of the UDP or raw datagram most recently
     * taken or peeked (`socket_dgram_consume`, `socket_dgram_sender`), for
     * recvmsg's ancillary data. */
    struct net_rxinfo rx_last;

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

/** Whether a bound socket accepts a datagram at the canonical address. */
int socket_address_matches(const struct socket *sock, net_addr_t dst);
/** Whether the bindings of two sockets cover any common local address. */
int socket_bind_conflict(const struct socket *a, const struct socket *b);
/** Whether another bound stream socket conflicts with sock at port.
 * The caller holds net_lock; sock is excluded. */
int socket_stream_conflict(const struct socket *sock, uint16_t port);

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

/* One datagram or raw socket, copied out for /proc/net/udp, raw and their
 * IPv6 files and for inet_diag. An AF_INET6 socket's addresses may be IPv4,
 * which it shows mapped. A raw socket's `local_port` is its protocol, 0 for the
 * descriptor that only takes configuration ioctls. */
struct socket_row {
    net_addr_t local_ip;
    uint16_t local_port;
    net_addr_t remote_ip;
    uint16_t remote_port;
    int connected;
    uint32_t rxq; /* bytes queued to read */
    uint32_t uid;
    uint32_t inode;
};

/**
 * Copy the sockets of `family`, AF_INET or AF_INET6, and `type`, SOCK_DGRAM
 * or SOCK_RAW, in the calling process's network namespace into an array
 * from the heap at `*rows`, and return the row count. With no socket of the
 * kind, `*rows` is NULL and the count 0; otherwise the caller frees the
 * array, even with no rows. Returns -1 when the array cannot be allocated.
 * Takes `net_lock` itself.
 */
int socket_snapshot(int family, int type, struct socket_row **rows);

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
 * The hop limit of a datagram from the AF_INET6 socket `sock` to `dst`,
 * leaving in the scope `ifindex`: IPV6_MULTICAST_HOPS for a group, else
 * IPV6_UNICAST_HOPS, or with that at -1 the hop limit routers advertised
 * for the interface the route takes (`nd6_hop_limit`), 64 without one. The
 * caller holds `net_lock`.
 */
uint8_t socket_hops6(const struct socket *sock, const uint8_t *dst, int ifindex);

/** Give back every multicast membership `sock` holds (IPV6_JOIN_GROUP),
 * one join of its interface each, as the socket's end does. The caller
 * holds `net_lock`. */
void socket_leave_groups6(struct socket *sock);

/**
 * Drop a reference, and at the last tear the socket down: give back its
 * multicast memberships, free a netlink
 * socket's state, remove a packet, raw or UDP socket's binding, or close a
 * stream's connection (`tcp_close`, after which the block finishes the close
 * on its own); release a listener's connections not yet accepted; and take
 * the socket off the list and free it. NULL is ignored. Takes `net_lock`
 * itself, and may be called from the receive path.
 */
void socket_unref(struct socket *sock);

/**
 * Queue the UDP payload of `len` bytes at `data`, from `src_ip`:`src_port`,
 * whole, on `sock` and wake its readers. A datagram that finds the queue's
 * budget spent (`sock_dgq`), or that comes from other than the peer of a
 * connected socket, is dropped. Called with interrupts off, from `udp_rx`.
 */
void socket_udp_deliver(struct socket *sock, net_addr_t src_ip, uint16_t src_port,
                        const uint8_t *data, uint16_t len);

/** `socket_udp_deliver` with what the network layer saw of the datagram,
 * kept for the ancillary data of a receive; `info` NULL knows nothing. */
void socket_udp_deliver_info(struct socket *sock, net_addr_t src_ip, uint16_t src_port,
                             const uint8_t *data, uint16_t len, const struct net_rxinfo *info);

/* What a datagram receive is asked for, the bits of `socket_recv`'s
 * `how`: leave the datagram queued (MSG_PEEK), and answer its whole length
 * rather than the bytes copied (MSG_TRUNC). */
#define SOCK_RECV_PEEK  0x1
#define SOCK_RECV_TRUNC 0x2

/**
 * Wait for a datagram on a UDP, raw, packet or netlink socket, copy up to
 * `len` bytes of it to `buf`, and report its sender through `src_ip` and
 * `src_port` when they are not NULL, as `socket_dgram_consume` reports it.
 * The rest of a longer datagram is discarded, since the datagram is the
 * unit; with SOCK_RECV_PEEK in `how` the datagram stays queued, whole, for
 * the next receive. Returns the bytes copied, or with SOCK_RECV_TRUNC the
 * datagram's whole length, as Linux answers MSG_TRUNC; -EINTR, -EFAULT, or
 * the socket's pending error, which this takes. The copy runs with
 * interrupts off, so `buf`, a user buffer, is brought in again after the
 * wait, and the call is -EFAULT, with the datagram left queued, when it no
 * longer can be.
 */
int socket_recv(struct socket *sock, uint8_t *buf, int len, net_addr_t *src_ip, uint16_t *src_port,
                int how);

/** `socket_recv` without the wait: the pending error first, then
 * SOCK_EAGAIN when no datagram is waiting. */
int socket_try_recv(struct socket *sock, uint8_t *buf, int len, net_addr_t *src_ip,
                    uint16_t *src_port, int how);

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
 * UDP socket reports nothing. A UDP or raw socket keeps in `rx_last` what
 * the network layer saw of the datagram.
 */
void socket_dgram_consume(struct socket *sock, net_addr_t *src_ip, uint16_t *src_port);

/** Report the oldest waiting datagram's sender as `socket_dgram_consume`
 * does, 0 for both with none waiting, and leave the datagram in place. A
 * packet socket keeps its link address for `packet_last_addr`, and a UDP or
 * raw socket what the network layer saw in `rx_last`. */
void socket_dgram_sender(struct socket *sock, net_addr_t *src_ip, uint16_t *src_port);

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
 * the error recorded for it or -EPIPE. `buf` is a user buffer the caller
 * has brought in (`user_buffer_ok`); after a wait this brings it in again,
 * and returns -EFAULT, with nothing queued, when it no longer can.
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
 * it or -ECONNRESET; -ENOTCONN for a stream that never connected; -EINTR;
 * or -EFAULT, with the bytes left in the ring, when `buf`, a user buffer,
 * can no longer be brought in after the wait. With `peek` the bytes stay in
 * the ring for the next receive (MSG_PEEK), and the window does not change.
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
