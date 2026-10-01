#ifndef _UDP_H_
#define _UDP_H_

#include "../../lib/kint.h"

/*
 * User Datagram Protocol. Outgoing datagrams get an 8-byte header and go to
 * ip_tx; incoming datagrams arrive from ip_rx via udp_rx and are dispatched to
 * the socket bound on the destination port. Outgoing checksums are sent as 0
 * (disabled, legal over IPv4); incoming checksums are not verified.
 */

#define UDP_HDR_SIZE 8

struct socket;

/** Receive and dispatch a UDP datagram to its bound socket: 0, or -1 when
 * no socket has its port, for `ip_rx` to answer. */
int udp_rx(uint32_t src_ip, const uint8_t *data, uint16_t len);

/** Give ECONNREFUSED to the socket on `local_port` connected to
 * `remote_ip`:`remote_port`, which answered with a port unreachable. */
void udp_refused(uint16_t local_port, uint32_t remote_ip, uint16_t remote_port);

/**
 * Build and send a UDP datagram with hop limit `ttl`.
 *
 * Returns what `ip_tx` returned: 0 when the datagram left (or was dropped for
 * want of an address the request for which has just gone out), and a negative
 * errno for the conditions the caller must hear about.
 */
int udp_tx(uint16_t src_port, uint32_t dst_ip, uint16_t dst_port, const uint8_t *payload,
           uint16_t plen, uint8_t ttl);

/** Bind `sock` to `port`; 0 on success, -1 on conflict, -ENOMEM on allocation
 * failure. Sharing requires SO_REUSEADDR on every socket and the caller's
 * effective uid on every binding. */
int udp_bind(uint16_t port, struct socket *sock);

/** Bind sock to a free ephemeral port; returns the port, or 0 on failure. */
uint16_t udp_bind_ephemeral(struct socket *sock);

/* Remove every binding `sock` has, called when a socket closes. */
void udp_unbind(struct socket *sock);

#endif /* _UDP_H_ */
