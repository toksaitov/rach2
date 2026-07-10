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

/** Validate one received IPv4 datagram and dispatch it by protocol.
 *  `from_loopback` is nonzero only for the loopback ring; a wire packet with a
 *  martian source (our own address or 127/8) is dropped. */
void ip_rx(const uint8_t *data, uint16_t len, int from_loopback);

/** Build an IPv4 datagram around the payload and transmit it; dst_ip is host order. */
void ip_tx(uint32_t dst_ip, uint8_t proto,
           const uint8_t *payload, uint16_t plen);

/** Deliver queued loopback datagrams; called from the timer IRQ each tick. */
void ip_loop_drain(void);

/** 16-bit one's complement checksum over `len` bytes of `buf`. */
uint16_t ip_checksum(const uint8_t *buf, uint16_t len);

#endif /* _IP_H_ */
