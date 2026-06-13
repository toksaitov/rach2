#ifndef _IP_H_
#define _IP_H_

#include "../../lib/kint.h"

/*
 * IPv4 layer. Every ICMP, UDP, and TCP message rides inside an IP packet.
 * We handle a fixed 20-byte header (no options on transmit) and reject
 * fragments, since reassembly is not implemented.
 */

/* Protocol numbers (the IP header's protocol field). */
#define IP_PROTO_ICMP   1
#define IP_PROTO_TCP    6
#define IP_PROTO_UDP    17

/* Fixed IPv4 header size (IHL = 5 words). */
#define IP_HDR_SIZE     20

/*
 * Handle a received IPv4 packet (header plus payload, after the Ethernet
 * header has been stripped). Dispatches by protocol to ICMP/UDP/TCP.
 */
void ip_rx(const uint8_t *data, uint16_t len);

/*
 * Build and transmit an IPv4 packet. dst_ip is in host byte order. On a local
 * /24 destination we ARP for it directly, otherwise for the gateway. On an ARP
 * miss the packet is dropped after sending the request; the caller retries.
 */
void ip_tx(uint32_t dst_ip, uint8_t proto,
           const uint8_t *payload, uint16_t plen);

/*
 * 16-bit one's complement checksum. Zero the checksum field and call this to
 * fill it; call it over a buffer with the field in place and a value of 0
 * means the checksum is valid.
 */
uint16_t ip_checksum(const uint8_t *buf, uint16_t len);

#endif /* _IP_H_ */
