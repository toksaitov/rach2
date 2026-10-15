#ifndef _UDP_H_
#define _UDP_H_

#include "../../lib/kint.h"

/*
 * udp: the User Datagram Protocol. A datagram sent gets an 8-byte header and
 * goes to `ip_tx_from`; one received comes from `ip_rx` and is given to the
 * socket bound to its address and destination port. The checksum is sent as
 * 0, which IPv4 allows to mean none, and a received one is not checked.
 */

#define UDP_HDR_SIZE 8

struct socket;

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
 * Returns what `ip_tx_from` returned: 0 when the datagram left, went onto
 * the loopback ring or waits for ARP, and a negative errno for what the
 * caller must hear about. A payload past 1472 bytes is -EMSGSIZE: one past
 * 1500, this layer's buffer, is refused here, and a smaller one by IP. Takes
 * `net_lock` itself.
 */
int udp_tx(const struct socket *sock, uint32_t dst_ip, uint16_t dst_port, const uint8_t *payload,
           uint16_t plen);

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

#endif /* _UDP_H_ */
