#ifndef _PACKET_H_
#define _PACKET_H_

#include "../../lib/kint.h"

struct socket;
struct netdev;

/** Return the bound interface index, or zero for none or every interface. */
int packet_ifindex(struct socket *sock);

/*
 * packet: AF_PACKET sockets, the tap a DHCP client cannot work without.
 *
 * `ip_rx` drops every datagram not addressed to this machine, and every
 * datagram a DHCP client waits for is one of those: an offer arrives before
 * the machine owns the address it is being offered. So the client opens a
 * socket above the Ethernet layer instead, and this is that socket.
 *
 * `SOCK_DGRAM` on `AF_PACKET` means the link header is not the caller's
 * business in either direction. A receive delivers the payload, the same bytes
 * `ip_rx` or `arp_rx` would have been given. A send takes that payload and a
 * destination MAC in the `sockaddr_ll` and this layer prepends the header.
 *
 * A socket sees one EtherType, its protocol: IPv4 for a DHCP client, ARP for
 * `arping`. Anything else is refused rather than bound to a filter this does
 * not implement, and a socket whose protocol is 0 receives nothing, which is
 * Linux's rule and is what a caller that named no protocol asked for. The
 * kernel's own handlers still see every frame: a packet socket taps the path,
 * it does not stand in it, so ARP still answers and still fills its cache
 * while a client watches the same frames.
 *
 * Opening one is restricted to effective uid 0. It reads every IPv4 frame the
 * machine receives, including other people's TCP, and it sends frames the IP
 * layer never saw.
 */

#define AF_PACKET 17

/* The protocol argument of `socket(AF_PACKET, ...)` is in network byte order,
 * as Linux specifies, so these are the byte-swapped EtherTypes a little-endian
 * caller passes after `htons`. */
#define PACKET_PROTO_IP  0x0008u
#define PACKET_PROTO_ARP 0x0608u

/* The `sockaddr_ll` a receive reports, and the two fields a reader decides on:
 * `sll_hatype` says the interface is Ethernet, and `sll_pkttype` says who the
 * frame was addressed to. The numbers are Linux's. */
#define PACKET_SLL_LEN      20
#define PACKET_HATYPE_ETHER 1
#define PACKET_HOST         0
#define PACKET_BROADCAST    1
#define PACKET_MULTICAST    2
#define PACKET_OTHERHOST    3

/**
 * Bind a packet socket to an interface index.
 *
 * Binding is what puts the socket in the delivery table; an unbound one
 * receives nothing, and rebinding moves it. An index must be one this machine
 * has; 0 is every interface, which is what a socket that named none asked for.
 * A socket bound to one interface is offered that interface's frames and sends
 * through it, so a client watching for a lease on one card is not handed the
 * other card's traffic. The socket's own `protocol` decides what of that it is
 * offered.
 *
 * @return 0, or a negative errno.
 */
int packet_bind(struct socket *sock, int ifindex);

/** Remove a socket from the delivery table; called when the last fd closes. */
void packet_unbind(struct socket *sock);

/**
 * Offer one received frame's payload to every bound socket that asked for it.
 *
 * Called from `ethernet_rx` with the interface the frame arrived on, its
 * EtherType (host order) and the WHOLE frame, header included, before the
 * kernel's own handler for that type runs: before `ip_rx` judges the
 * destination address, and before `arp_rx` answers and fills the cache. What
 * is queued is the payload; the header is read for the sender's address and
 * for who the frame was addressed to, which a reader is given with it, along
 * with the interface it came in on.
 *
 * Each socket has a queue, not the one slot the rest of the datagram
 * path has, and the difference is the point of the socket. Everything the
 * machine receives arrives here, not just what is addressed to it, so on a
 * real network the reply a client is waiting for is followed by other
 * people's traffic within the same batch of frames one interrupt drains. A
 * single slot would hand that client whichever frame arrived last. A full
 * queue drops the newest and counts it.
 */
void packet_rx(struct netdev *nd, uint16_t ethertype, const uint8_t *frame, uint16_t frame_len);

/*
 * The receive queue, as the three steps `socket_dgram_*` are made of. A packet
 * socket's datagrams come from here instead of from the socket's own slot;
 * `socket.c` branches on the family and calls these, so every caller above it
 * keeps one API.
 */

/** Whether a datagram is waiting; the wait predicate. */
int packet_ready(struct socket *sock);

/** Bytes in the waiting datagram, or -1 when the queue is empty. */
int packet_pending(struct socket *sock);

/** Copy up to `len` bytes from `offset` into the waiting datagram, in place. */
int packet_peek(struct socket *sock, uint32_t offset, uint8_t *buf, int len);

/** Drop the waiting datagram, keeping the address it came from. */
void packet_consume(struct socket *sock);

/**
 * Drop everything queued for `sock`.
 *
 * Called when a bind changes the socket's protocol: what is queued arrived
 * under the old one, and a reader would be given it as though it had not.
 */
void packet_flush(struct socket *sock);

/**
 * Write the `sockaddr_ll` of the datagram most recently taken into `sll`.
 *
 * The address is reported after the datagram it belongs to has been consumed,
 * which is the order a receive runs in, so the last one is kept rather than
 * read from a queue it has left. `sll` must have room for `PACKET_SLL_LEN`
 * bytes.
 *
 * @return PACKET_SLL_LEN, or 0 if this socket has taken no datagram.
 */
int packet_last_addr(struct socket *sock, uint8_t *sll);

/**
 * Send one payload to `dst_mac` under `ethertype` (host order).
 *
 * The caller takes the EtherType from the `sockaddr_ll` it was given, falling
 * back to the socket's own protocol, so a program that names neither is told
 * so rather than having a frame sent under a type it did not choose. `ifindex`
 * is the interface the caller's `sockaddr_ll` named, or 0 for none; the frame
 * leaves by that one, or by the one the socket is bound to, or by the first.
 * There is no address to route on here, since the caller gave a MAC.
 *
 * @return the bytes sent, or a negative errno.
 */
int packet_send(struct socket *sock, int ifindex, const uint8_t *dst_mac, uint16_t ethertype,
                const uint8_t *data, uint16_t len);

#endif /* _PACKET_H_ */
