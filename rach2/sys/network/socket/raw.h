#ifndef _RAW_H_
#define _RAW_H_

#include "../../lib/kint.h"

struct socket;

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
 */

/**
 * Register `sock` to receive protocol `proto` of IPv4, or of IPv6 when
 * `sock` is an AF_INET6 socket, with a queue from the heap, or change the
 * protocol of a registration it has. Returns 0, or -ENOBUFS with no memory.
 * Called when the socket is created, since a raw socket's protocol is
 * chosen there and not by a later bind. Takes `net_lock` itself.
 */
int raw_register(struct socket *sock, uint8_t proto);

/** Remove a socket's registration and free its queue, when its last
 * reference goes (`socket_unref`). Takes `net_lock` itself. */
void raw_unregister(struct socket *sock);

/**
 * Offer one received datagram to every raw socket of its protocol.
 *
 * `data` is the whole datagram, header included, and `total_len` its length;
 * one shorter than an IP header is ignored, and one past RAW_SLOT_SIZE, 1500
 * bytes, is cut to that size. A socket whose queue of eight is full drops
 * it, the newest, and the first such drop is logged. Called with interrupts
 * off, from `ip_rx`, after the datagram has been accepted for this machine
 * and before the kernel's own handler for the protocol.
 */
void raw_rx(uint8_t proto, const uint8_t *data, uint16_t total_len);

/**
 * Offer the message of `len` bytes at `msg`, the payload of an IPv6
 * datagram of next header `nh` from `src` that came in on the interface of
 * index `ifindex`, to every AF_INET6 raw socket of that protocol, under the
 * rules of `raw_rx`. Called with interrupts off, from `ip6_rx`, before the
 * kernel's own handler.
 */
void raw6_rx(int ifindex, const uint8_t *src, uint8_t nh, const uint8_t *msg, uint16_t len);

/* The receive queue, as the three steps `socket_dgram_*` are made of, the same
 * shape the packet socket uses. */

/**
 * Whether a datagram is waiting; the wait predicate.
 *
 * Read without the network lock, as the packet socket's is: `wq_wait_event`
 * evaluates a predicate with interrupts already off, which is what serializes
 * this against the receive interrupt that fills the queue.
 */
int raw_ready(struct socket *sock);

/** Bytes in the waiting datagram, or -1 when the queue is empty. */
int raw_pending(struct socket *sock);

/** Copy up to `len` bytes of the waiting datagram, from `offset` bytes into
 * it, leaving it in place: the count copied, 0 at or past its end or for a
 * negative `len`, or -1 when the queue is empty. */
int raw_peek(struct socket *sock, uint32_t offset, uint8_t *buf, int len);

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

#endif /* _RAW_H_ */
