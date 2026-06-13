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

/*
 * Handle a received ICMP message (after the IP header is stripped). src_ip is
 * the sender, used as the reply destination and to match echo replies.
 */
void icmp_rx(uint32_t src_ip, const uint8_t *data, uint16_t len);

/*
 * Send an ICMP echo request to dst_ip and wait for the matching reply.
 * Returns the round-trip time in kernel ticks (100 Hz), or -1 on timeout.
 * Only one ping may be in flight at a time.
 */
int icmp_ping(uint32_t dst_ip);

#endif /* _ICMP_H_ */
