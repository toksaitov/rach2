#ifndef _PACKET_H_
#define _PACKET_H_

#include "../../lib/kint.h"

struct socket;

/*
 * packet: AF_PACKET sockets, the tap a DHCP client cannot work without.
 *
 * `ip_rx` drops every datagram not addressed to this machine, and every
 * datagram a DHCP client waits for is one of those: an offer arrives before
 * the machine owns the address it is being offered. So the client opens a
 * socket above the Ethernet layer instead, and this is that socket.
 *
 * `SOCK_DGRAM` on `AF_PACKET` means the link header is not the caller's
 * business in either direction. A receive delivers the IP packet, the same
 * bytes `ip_rx` would have been given. A send takes an IP packet and a
 * destination MAC in the `sockaddr_ll` and this layer prepends the header.
 * Only IPv4 frames are offered, which is what `ETH_P_IP` asks for and all a
 * DHCP client wants; a socket asking for anything else is refused rather than
 * bound to a filter this does not implement.
 *
 * Opening one is restricted to effective uid 0. It reads every IPv4 frame the
 * machine receives, including other people's TCP, and it sends frames the IP
 * layer never saw.
 */

#define AF_PACKET 17

/* The protocol argument of `socket(AF_PACKET, ...)` is in network byte order,
 * as Linux specifies, so these are the byte-swapped EtherTypes a little-endian
 * caller passes after `htons`. */
#define PACKET_PROTO_IP 0x0008u

/**
 * Bind a packet socket to an interface index.
 *
 * Binding is what puts the socket in the delivery table; an unbound one
 * receives nothing. Rebinding moves it. There is one interface, so the only
 * index that binds is `NETDEV_IFINDEX`, and 0 (any) binds to it too.
 *
 * @return 0, or a negative errno.
 */
int packet_bind(struct socket *sock, int ifindex);

/** Remove a socket from the delivery table; called when the last fd closes. */
void packet_unbind(struct socket *sock);

/**
 * Offer one received IPv4 packet to every bound socket.
 *
 * Called from `ethernet_rx` with the frame's payload, before `ip_rx` judges
 * the destination address.
 *
 * Each socket has a queue rather than the one slot the rest of the datagram
 * path has, and the difference is the point of the socket. Everything the
 * machine receives arrives here, not just what is addressed to it, so on a
 * real network the reply a client is waiting for is followed by other
 * people's traffic within the same batch of frames one interrupt drains. A
 * single slot would hand that client whichever frame arrived last. A full
 * queue drops the newest and counts it.
 */
void packet_rx(const uint8_t *payload, uint16_t len);

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

/** Drop the waiting datagram. */
void packet_consume(struct socket *sock);

/**
 * Send one IP packet to `dst_mac`.
 *
 * @return the bytes sent, or a negative errno.
 */
int packet_send(struct socket *sock, const uint8_t *dst_mac, const uint8_t *data, uint16_t len);

#endif /* _PACKET_H_ */
