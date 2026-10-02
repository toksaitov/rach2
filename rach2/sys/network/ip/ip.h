#ifndef _IP_H_
#define _IP_H_

#include "../../lib/kint.h"

/*
 * IPv4 layer. Every ICMP, UDP, and TCP message rides inside an IP packet.
 * We handle a fixed 20-byte header (no options on transmit) and reject
 * fragments, since reassembly is not implemented.
 */

/* Protocol numbers (the IP header's protocol field). */
#define IP_PROTO_ICMP 1
#define IP_PROTO_TCP  6
#define IP_PROTO_UDP  17

/* Fixed IPv4 header size (IHL = 5 words). */
#define IP_HDR_SIZE 20

/* The loopback network, 127/8, and the address a datagram to it leaves
 * from. Its datagrams never reach a card: they go round the loopback ring,
 * and one that arrives from the wire is dropped. */
#define IP_IS_LOOPBACK(ip) (((ip) & 0xFF000000u) == 0x7F000000u)
#define IP_LOOPBACK_ADDR   0x7F000001u

/** Whether `ip` is an address of this machine: an interface's, or one of
 * 127/8. */
int ip_is_local(uint32_t ip);

/** The address a datagram to `dst` leaves from: 127.0.0.1 for 127/8, else
 * the address of the interface the route to `dst` leaves by, or the primary
 * address when no route reaches it. */
uint32_t ip_source(uint32_t dst);

/** Validate one received IPv4 datagram and dispatch it by protocol.
 *  `from_loopback` is nonzero only for the loopback ring; a wire packet with a
 *  martian source (our own address or 127/8), or addressed to 127/8, is
 *  dropped. */
void ip_rx(const uint8_t *data, uint16_t len, int from_loopback);

/* The hop limit a datagram leaves with unless the sender asked for another.
 * A socket may set its own with IP_TTL, which is what traceroute is. */
#define IP_TTL_DEFAULT 64

/**
 * Build an IPv4 datagram around the payload and transmit it; dst_ip is host
 * order, and `ttl` is the hop limit.
 *
 * Returns 0 when the datagram left, or was queued for loopback, or was dropped
 * for want of a hardware address the request for which has just gone out; a
 * datagram send promises no delivery, and the next one goes out resolved.
 * Returns a negative errno for the conditions the caller must hear about:
 * -EMSGSIZE for a payload above 1480 bytes (1500 less the IPv4 header),
 * -ENETUNREACH for an address with no route, -ENOBUFS when the loopback ring
 * is full or the driver did not take the frame, -EOPNOTSUPP for a broadcast
 * of anything but UDP, and -EINVAL for a datagram from 127/8 to an address
 * that is not this machine's, which no card may carry.
 */
int ip_tx(uint32_t dst_ip, uint8_t proto, const uint8_t *payload, uint16_t plen, uint8_t ttl);

/** The same from the source `src`, one of this machine's addresses, which a
 * connection or a bound socket keeps; with 0, or an address the machine no
 * longer holds, the one `ip_source` chooses. */
int ip_tx_from(uint32_t src, uint32_t dst_ip, uint8_t proto, const uint8_t *payload, uint16_t plen,
               uint8_t ttl);

/**
 * Send the datagrams held for the next hop `hop` on `nd`, whose hardware
 * address `mac` ARP has just learned. Call with interrupts off.
 */
struct netdev;
void ip_hold_resolved(struct netdev *nd, uint32_t hop, const uint8_t *mac);

/** Drop the held datagrams that leave by `nd`, or all of them for NULL, when
 * an address changes and their source with it. */
void ip_hold_forget(struct netdev *nd);

/** Deliver queued loopback datagrams; called from the timer IRQ each tick. */
void ip_loop_drain(void);

/* Who drains the loopback: the tick, the idle wait, or a system call's
   exit; `ip_loop_delivered` counts the datagrams each delivered. */
#define IP_LOOP_FROM_TICK 0
#define IP_LOOP_FROM_IDLE 1
#define IP_LOOP_FROM_CALL 2
void ip_loop_drain_from(int from);
uint32_t ip_loop_delivered(int from);

/** Nonzero while the loopback ring holds anything at all. */
int ip_loop_pending(void);

/** Datagrams the loopback ring had no room for since boot. */
uint32_t ip_loop_dropped(void);

/** The most datagrams the loopback ring has held at once, which says how much
 * room the ring has over what one system call can put in it. */
uint32_t ip_loop_peak(void);

/**
 * Nonzero when the loopback ring holds more than a pair of datagrams.
 *
 * A caller uses this to drain before the ring fills and starts refusing. Two
 * conditions belong to the caller and not to `ip_loop_drain`: interrupts are
 * off, and the caller has finished whatever it was doing to the stack, since
 * a delivery runs the receive path and may transmit. A pair is left to the
 * timer, since a simultaneous open is a pair of crossing SYNs and is only a
 * pair while neither has been delivered.
 */
int ip_loop_crowded(void);

/** 16-bit one's complement checksum over `len` bytes of `buf`. */
uint16_t ip_checksum(const uint8_t *buf, uint16_t len);

#endif /* _IP_H_ */
