#ifndef _ICMP_H_
#define _ICMP_H_

#include "../../lib/kint.h"

/*
 * ICMP echo (ping). The host sends an echo request (type 8); we reply with an
 * echo reply (type 0), copying the id, sequence, and payload back. icmp_ping
 * sends our own request and waits for the matching reply.
 *
 * Note: QEMU SLIRP does not forward host-to-guest ICMP, but it does answer
 * guest-originated echo requests, so pings are tested from the guest outward
 * (e.g. to the gateway 10.0.2.2).
 */

#define ICMP_ECHO_REQUEST 8
#define ICMP_ECHO_REPLY   0
/* Destination unreachable, and its code for a datagram too big for a link
 * on the way that had DF set, whose bytes 6 and 7 say the link's MTU
 * (RFC 1191). */
#define ICMP_UNREACHABLE  3
#define ICMP_FRAG_NEEDED  4

/** Handle one received ICMP datagram (IP header already stripped). */
void icmp_rx(uint32_t src_ip, const uint8_t *data, uint16_t len);

/* The most data an echo request carries: an unfragmented datagram of the
 * 1500-byte MTU less the IP and ICMP headers. */
#define ICMP_PING_DATA_MAX 1472u

/** Send an echo request to `dst_ip` and wait; the round-trip time in ticks,
 * or `-ETIMEDOUT`, `-EBUSY` or `-EINTR`. */
int icmp_ping(uint32_t dst_ip);

/** `icmp_ping` with `size` bytes of data after the header, at most
 * `ICMP_PING_DATA_MAX`, and the hop limit `ttl`; the reply's ICMP length
 * goes to `*got` unless `got` is NULL. */
int icmp_ping_data(uint32_t dst_ip, uint32_t size, uint8_t ttl, uint16_t *got);

#endif /* _ICMP_H_ */
