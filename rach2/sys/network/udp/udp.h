#ifndef _UDP_H_
#define _UDP_H_

#include "../../lib/kint.h"
#include "../socket/address.h"

/*
 * udp: the User Datagram Protocol. A datagram sent gets an 8-byte header and
 * goes to `ip_tx_opts`; one received comes from `ip_rx` and is given to the
 * socket bound to its address and destination port. Both families send the
 * pseudo-header checksum; IPv4 checks one received unless it is 0, which
 * says the sender gave none, and IPv6 requires it.
 */

#define UDP_HDR_SIZE 8

/* The largest payload over each family: the largest IPv4 datagram less its
 * header and UDP's, and the largest IPv6 payload less UDP's header. */
#define UDP_PAYLOAD_MAX  (65535 - 20 - UDP_HDR_SIZE)
#define UDP6_PAYLOAD_MAX (65535 - UDP_HDR_SIZE)

struct socket;
struct net_txopt;

/**
 * Give the UDP datagram of `len` bytes at `data`, header included, sent from
 * `src_ip` to `dst_ip`, to a socket on its destination port: the one
 * connected to its sender, else the one bound to `dst_ip` most recently,
 * else the one bound to any address most recently.
 *
 * Returns -1 when no socket has the address and port, which `ip_rx` answers
 * with a port unreachable, and 0 otherwise, a malformed datagram included:
 * one shorter than the header, or whose length field is below the header's
 * size or above `len`, which is dropped. Counts in `net_mib`. Called with
 * interrupts off, from `ip_rx`.
 */
int udp_rx(uint32_t src_ip, uint32_t dst_ip, const uint8_t *data, uint16_t len);

/** Give ECONNREFUSED as the pending error to each socket on `local_port`
 * connected to `remote_ip`:`remote_port`, which answered with a port
 * unreachable, and wake its waiters. An unconnected socket is not told, as
 * Linux tells none without IP_RECVERR. Called with interrupts off, from
 * `icmp_rx`. */
void udp_refused(uint16_t local_port, uint32_t remote_ip, uint16_t remote_port);

/**
 * Send the `plen` bytes at `payload` as a datagram from `sock` to
 * `dst_ip`:`dst_port`: from its bound address, or the one `ip_source`
 * chooses for a socket bound to any, and its port, with its hop limit.
 *
 * Over IPv6 the socket's options apply (socket.h): the hop limit is
 * IPV6_MULTICAST_HOPS for a group and IPV6_UNICAST_HOPS otherwise, or with
 * that at -1 the one routers advertise; a group with no scope in its address
 * leaves by IPV6_MULTICAST_IF, and comes back to this machine as
 * IPV6_MULTICAST_LOOP says; the class is IPV6_TCLASS. `opt`, when not NULL,
 * is sendmsg's control messages, which go before the socket's: a source,
 * which must be an address of this machine, an interface, which must agree
 * with the address's scope, a hop limit and a class.
 *
 * Over IPv4 the datagram leaves under the socket's IP_MTU_DISCOVER
 * (`pmtudisc`): in fragments past the route's MTU, or refused.
 *
 * Returns what `ip_tx_opts` or `ip6_tx_opts` returned: 0 when the datagram
 * left, went onto the loopback ring or waits for ARP, and a negative errno
 * for what the caller must hear about, -EINVAL among them for a source of
 * `opt` that is not this machine's or an interface that disagrees. A
 * payload past UDP_PAYLOAD_MAX, or UDP6_PAYLOAD_MAX over IPv6, is
 * -EMSGSIZE, and -ENOMEM is one past a frame that no block holds. Takes
 * `net_lock` itself.
 */
int udp_tx(const struct socket *sock, net_addr_t dst_ip, uint16_t dst_port, const uint8_t *payload,
           uint16_t plen, const struct net_txopt *opt);

/** Bind `sock` to `port` at its `local_ip`, under the caller's effective
 * uid; 0 on success, -1 on conflict, -ENOMEM on allocation failure. Two
 * bindings conflict when one is to any address or both are to the same one;
 * sharing requires SO_REUSEADDR on every such socket and the caller's
 * effective uid on every such binding. Takes `net_lock` itself. */
int udp_bind(uint16_t port, struct socket *sock);

/** Bind `sock` to the next port from 49152 to 65535 that no binding has, at
 * any address, and record it in `sock->local_port`: the port, or 0 when every
 * one is taken or there is no memory for a binding. */
uint16_t udp_bind_ephemeral(struct socket *sock);

/** Remove every binding `sock` has, when the socket closes or binds again.
 * Takes `net_lock` itself. */
void udp_unbind(struct socket *sock);

/** Deliver the UDP message of `len` bytes at `data`, of the IPv6 datagram
 * whose fixed header is at `ip6h`, received on the interface of index
 * `ifindex`, after verifying its mandatory checksum, with the datagram's
 * destination, hop limit and class kept for the receive's ancillary data.
 * Returns -1 for no binding, zero for delivery or a malformed message.
 * The caller holds net_lock. */
int udp6_rx(int ifindex, const uint8_t *ip6h, const uint8_t *data, uint16_t len);

#endif /* _UDP_H_ */
