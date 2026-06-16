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

#define ICMP_ECHO_REQUEST   8
#define ICMP_ECHO_REPLY     0

/** Handle one received ICMP datagram (IP header already stripped). */
void icmp_rx(uint32_t src_ip, const uint8_t *data, uint16_t len);

/** Send an echo request to `dst_ip` and wait; round-trip ticks, or -1. */
int icmp_ping(uint32_t dst_ip);

#endif /* _ICMP_H_ */
