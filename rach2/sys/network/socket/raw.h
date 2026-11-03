#ifndef _RAW_H_
#define _RAW_H_

#include "../../lib/kint.h"
#include "address.h"

struct socket;
struct net_rxinfo;

/* struct icmp6_filter: 256 bits, one for each ICMPv6 type, a set bit
 * blocking the type (RFC 3542, 3.2). */
#define RAW_FILTER6_LEN 32

/*
 * raw: AF_INET SOCK_RAW sockets of IPPROTO_ICMP, what `ping` and
 * `traceroute` are built on, and AF_INET6 ones of IPPROTO_ICMPV6, which
 * IPv6's `ping` is.
 *
 * An AF_INET raw socket receives whole IPv4 datagrams of one protocol, the
 * header included, because that is what the programs parse: busybox `ping`
 * reads the IHL out of the first byte to find its own ICMP message, and
 * `traceroute` reads the source address out of the header of a
 * time-exceeded message to name the hop. `ip_rx` offers each datagram it has
 * accepted to the matching raw sockets on the way past, before its own
 * handler for that protocol runs, and that handler still runs: a program
 * watching ICMP does not stop the machine answering an echo request.
 *
 * Only datagrams this machine is addressed by are offered, which is what a raw
 * socket sees on Linux too without a promiscuous device: `ip_rx` has already
 * dropped everything else.
 *
 * A send takes an ICMP message the caller built, checksum included:
 * `sendto` (syscall_net.c) gives it to `ip_tx`, which supplies the IPv4
 * header. The checksum is not recomputed: it is the caller's, and a program
 * that sends a wrong one on purpose is testing something.
 *
 * An AF_INET6 raw socket receives the ICMPv6 message alone, without the
 * IPv6 header, as on Linux, with the source address and the interface it
 * came in on beside it (`raw_last_src6`). Its sends are summed by the
 * kernel: Linux computes an ICMPv6 checksum whatever the caller wrote,
 * since its pseudo-header has the source address the kernel chooses.
 *
 * Opening one is restricted to a privileged process (`proc_privileged`), as
 * on Linux, because it reads every datagram of its protocol the machine
 * receives and sends datagrams no transport layer checked.
 *
 * An ICMP datagram socket, Linux's "ping socket", is socket(AF_INET,
 * SOCK_DGRAM, IPPROTO_ICMP) or socket(AF_INET6, SOCK_DGRAM,
 * IPPROTO_ICMPV6), and any process may open one, as any may on Linux
 * whose net.ipv4.ping_group_range admits every group. It is a raw socket
 * inside (SOCK_RAW, which SO_TYPE reports as SOCK_DGRAM) with an echo
 * identifier of its own, which no other such socket and no ping of the
 * kernel's holds (`icmp_echo_id`). It sends only echo requests, each with
 * its identifier written over the caller's and summed by the kernel, and it
 * receives only the echo replies that carry that identifier, the ICMP
 * message alone over either family, an IPv4 one only when its ICMP
 * checksum holds. Any number of them run at once, and each sees only its
 * own replies.
 *
 * As on Linux (net/ipv4/ping.c), the identifier is the socket's port: bind
 * takes a nonzero one from `sin_port` or `sin6_port`, or keeps the one the
 * socket has for 0, and the first bind, send or connect fixes it, after
 * which getsockname reports it and a second bind is EINVAL. Before then
 * getsockname reports port 0. A connected one sends to its peer without an
 * address and reads with recv.
 */

/**
 * Register `sock` to receive protocol `proto` of IPv4, or of IPv6 when
 * `sock` is an AF_INET6 socket, as an ICMP datagram socket with an echo
 * identifier of its own when `echo` is set, or change the protocol of a
 * registration it has. Returns 0, or -ENOBUFS with no memory. Called when
 * the socket is created, since a raw socket's protocol is chosen there and
 * not by a later bind. Takes `net_lock` itself.
 */
int raw_register(struct socket *sock, uint8_t proto, int echo);

/** The echo identifier of the ICMP datagram socket `sock`, or -1 for any
 * other socket. Takes `net_lock` itself. */
int raw_echo_id(struct socket *sock);

/** Whether an ICMP datagram socket holds the echo identifier `id`. Called
 * under `net_lock`. */
int raw_echo_id_used(uint16_t id);

/**
 * bind(2)'s part for the ICMP datagram socket `sock`: give it the echo
 * identifier `id`, or for 0 keep the one it has, and fix it. Returns 0,
 * -EINVAL for a socket whose identifier a bind, send or connect has fixed
 * already, or that is no ICMP datagram socket, or -EADDRINUSE for an
 * identifier another such socket or a ping of the kernel's holds. Takes
 * `net_lock` itself.
 */
int raw_echo_bind(struct socket *sock, uint16_t id);

/** Fix the echo identifier of the ICMP datagram socket `sock`, as a connect
 * does on Linux; any other socket is left alone. Takes `net_lock` itself. */
void raw_echo_hold(struct socket *sock);

/** The port getsockname reports for the ICMP datagram socket `sock`: its
 * echo identifier once fixed, 0 before; -1 for any other socket. Takes
 * `net_lock` itself. */
int raw_echo_port(struct socket *sock);

/**
 * Make the message of `len` bytes at `msg`, which the ICMP datagram socket
 * `sock` sends, the socket's echo request: its type must be `request` and
 * its code 0, and its identifier becomes the socket's, which the send
 * fixes, and its checksum 0, for the send to sum. Any other socket's
 * message is left alone. Returns 1 for a message stamped, 0 for another
 * socket's, or -1 for a message shorter than an echo's 8-byte header or of
 * another type, which the send refuses with EINVAL, as Linux does. Takes
 * `net_lock` itself.
 */
int raw_echo_stamp(struct socket *sock, uint8_t *msg, int len, uint8_t request);

/** Remove a socket's registration and free the datagrams it has queued,
 * when its last reference goes (`socket_unref`). Takes `net_lock` itself. */
void raw_unregister(struct socket *sock);

/**
 * Offer one received datagram to every raw socket of its protocol.
 *
 * `data` is the whole datagram, header included, reassembled if it came in
 * fragments, and `total_len` its length; one shorter than an IP header is
 * ignored. Each socket queues it whole on its own queue (`sock_dgq`), an
 * ICMP datagram socket only its own echo reply, without the header and
 * only with a good ICMP checksum, and
 * one whose budget is spent drops it, the newest, the first such drop
 * logged. Called with interrupts
 * off, from `ip_rx`, after the datagram has been accepted for this machine
 * and before the kernel's own handler for the protocol.
 */
void raw_rx(uint8_t proto, const uint8_t *data, uint16_t total_len);

/**
 * Offer the message of `len` bytes at `msg`, the payload of the IPv6 datagram
 * whose fixed header is at `ip6h`, of next header `nh`, that came in on the
 * interface of index `ifindex`, to every AF_INET6 raw socket of that protocol,
 * under the rules of `raw_rx`. A socket of IPPROTO_ICMPV6 takes no message of a
 * type its filter blocks (`raw_set_filter6`), nor one too short to have a type,
 * nor one whose checksum fails, which RFC 3542, 3.1, has the kernel check; an
 * ICMP datagram socket takes only its own echo replies, whatever its filter.
 * The hop limit and source are not checked, as on Linux: a reader of neighbor
 * discovery checks them itself. Each message keeps its source, destination, hop
 * limit and traffic class. Called with interrupts off, from `ip6_rx`, before
 * the kernel's own handler.
 */
void raw6_rx(int ifindex, const uint8_t *ip6h, uint8_t nh, const uint8_t *msg, uint16_t len);

/**
 * Set the ICMPv6 filter of the raw socket `sock` from the `len` bytes at
 * `filter`, at most RAW_FILTER6_LEN; the bits past them are left as they
 * were, as Linux leaves them. A new socket passes every type. Returns 0, or
 * -1 for a socket with no registration. Takes `net_lock` itself.
 */
int raw_set_filter6(struct socket *sock, const uint8_t *filter, uint32_t len);

/** Copy the RAW_FILTER6_LEN bytes of the ICMPv6 filter of `sock` to
 * `filter`. Returns 0, or -1 for a socket with no registration. */
int raw_get_filter6(struct socket *sock, uint8_t *filter);

/** What the network layer saw of the oldest waiting datagram, into
 * `info`; a destination of 0.0.0.0 and nothing else known when none
 * waits. Called under the caller's `net_lock`. */
void raw_head_info(struct socket *sock, struct net_rxinfo *info);

/** What the network layer saw of the datagram most recently taken. */
void raw_last_info(struct socket *sock, struct net_rxinfo *info);

/* The receive queue is the socket's own (`rq`), which the three steps
 * `socket_dgram_*` read; these keep what a receive reports of a raw
 * datagram. */

/** Drop the waiting datagram, keeping the address it came from. */
void raw_consume(struct socket *sock);

/** The source address of the datagram most recently taken, or 0 when none
 * has been. */
uint32_t raw_last_src(struct socket *sock);

/** The source address of the oldest waiting datagram, or 0 with none. */
uint32_t raw_head_src(struct socket *sock);

/** For an AF_INET6 socket: copy the IPv6 source of the oldest waiting
 * datagram to `src` and return the index of the interface it came in on,
 * or return -1 with none waiting. */
int raw_head_src6(struct socket *sock, uint8_t *src);

/** For an AF_INET6 socket: copy the IPv6 source of the datagram most
 * recently taken to `src`, all zeros when none has been, and return the
 * index of the interface it came in on, 0 for none; -1 for a socket with
 * no registration. */
int raw_last_src6(struct socket *sock, uint8_t *src);

/** Copy the head source under the caller's net_lock, or unspecified. */
void raw_head_address(struct socket *sock, net_addr_t *src);
/** Copy the last consumed source under the caller's net_lock. */
void raw_last_address(struct socket *sock, net_addr_t *src);
#endif /* _RAW_H_ */
