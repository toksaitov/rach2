#ifndef _IP_H_
#define _IP_H_

#include "../../lib/kint.h"

/*
 * ip: the IPv4 layer, which every ICMP, UDP and TCP message travels in. A
 * received datagram is checked and given to its protocol. A datagram sent
 * has a 20-byte header with no options and DF set, and leaves by its
 * route, by broadcast, or round the loopback ring when it is addressed to
 * this machine. A fragment is dropped, since there is no reassembly, and
 * none is made. Addresses are host-order uint32_t values (10.0.2.15 is
 * 0x0A00020F), converted at the header.
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
 * the address of the interface the route to `dst` leaves by on the next
 * hop's subnet, or that interface's first (`netdev_source`), or the first
 * interface's address (`net_ip_addr`) when no route reaches `dst`. */
uint32_t ip_source(uint32_t dst);

/**
 * Check the IPv4 datagram of `len` bytes at `data` and give it to its
 * protocol. `from_loopback` is nonzero only for a datagram from the loopback
 * ring.
 *
 * A datagram with a bad version, header length, total length or header
 * checksum is dropped, and so is every fragment and one addressed to
 * neither an address of an interface nor a broadcast; an interface with no
 * address holds none, so 0.0.0.0 is nobody's. Options are skipped. A
 * datagram from the wire is also dropped when it is addressed to 127/8, as
 * Linux drops it, or its source is an address of this machine or in 127/8.
 * A broadcast is given to UDP alone. An accepted datagram is offered whole
 * to the raw sockets of its protocol (`raw_rx`), and its payload, to the
 * datagram's total length, given to `icmp_rx`, `udp_rx` or `tcp_rx`; a
 * unicast UDP datagram for a port no socket has is answered with a port
 * unreachable (`icmp_unreachable`).
 *
 * Each datagram counts in `net_mib` as received, and a dropped one by its
 * reason; a multicast datagram, for a group no socket here can join, counts
 * only as received, as Linux counts one for a group it has not joined.
 * Called with interrupts off, from `ethernet_rx` and the loopback drain.
 */
void ip_rx(const uint8_t *data, uint16_t len, int from_loopback);

/* The hop limit a datagram leaves with unless the sender asked for another.
 * A socket may set its own with IP_TTL, which is what traceroute is. */
#define IP_TTL_DEFAULT 64

/** `ip_tx_from` from the source `ip_source` chooses. */
int ip_tx(uint32_t dst_ip, uint8_t proto, const uint8_t *payload, uint16_t plen, uint8_t ttl);

/**
 * Send the `plen` bytes at `payload` as a datagram of protocol `proto` from
 * `src` to `dst_ip`, with the hop limit `ttl`. `src` is one of this
 * machine's addresses, which a connection or a bound socket keeps; 0, or an
 * address that is not this machine's (`ip_is_local`), takes the one
 * `ip_source` chooses.
 *
 * A datagram to 127/8 or to an address an interface holds goes onto the
 * loopback ring, which `ip_loop_drain_from` delivers later. A broadcast, to
 * 255.255.255.255 or a subnet's broadcast address, goes to the all-ones
 * station address by the card on that subnet, or the first card for
 * 255.255.255.255. Any other datagram leaves by the route `netdev_route`
 * chooses, to the next hop's station address from ARP; on a miss an ARP
 * request goes out and the datagram waits for the reply in one of four
 * slots, for at most three seconds (`IP_HOLD_SLOTS` and `IP_HOLD_TICKS` in
 * ip.c).
 *
 * Returns 0 when the datagram left, went onto the loopback ring, or waits
 * for ARP: a datagram send promises no delivery. Returns a negative errno
 * for what the caller must hear about: -EMSGSIZE for a payload above 1480
 * bytes (1500 less the IPv4 header), -ENETUNREACH for a destination with no
 * route, -ENOBUFS when the loopback ring is full or the driver did not take
 * the frame, -EOPNOTSUPP for a broadcast of anything but UDP, and -EINVAL
 * for a datagram from 127/8 to an address that is not this machine's, which
 * no card may carry.
 *
 * The datagram is built in a buffer of this file's, so the caller has
 * interrupts off (`net_lock` in a system call).
 */
int ip_tx_from(uint32_t src, uint32_t dst_ip, uint8_t proto, const uint8_t *payload, uint16_t plen,
               uint8_t ttl);

/* The interface a held datagram leaves by. */
struct netdev;

/**
 * Send the datagrams held for the next hop `hop` on `nd`, whose station
 * address `mac` ARP has just learned, and free their slots; one that has
 * waited longer than three seconds is dropped. Called with interrupts off,
 * when ARP stores a mapping.
 */
void ip_hold_resolved(struct netdev *nd, uint32_t hop, const uint8_t *mac);

/** Drop the held datagrams that leave by `nd`, or all of them for NULL,
 * when an address changes and their source with it. Called with interrupts
 * off, from `arp_flush` and `arp_forget_dev`. */
void ip_hold_forget(struct netdev *nd);

/** `ip_loop_drain_from` for the tick, which calls it once a tick from the
 * timer interrupt. */
void ip_loop_drain(void);

/* Who drains the loopback: the tick, the idle wait, or a system call's
   exit; `ip_loop_delivered` counts the datagrams each delivered. */
#define IP_LOOP_FROM_TICK 0
#define IP_LOOP_FROM_IDLE 1
#define IP_LOOP_FROM_CALL 2

/**
 * Deliver the datagrams on the loopback ring through `ip_rx`, at most four
 * times the ring's slots in one call, and count them under `from`, an
 * `IP_LOOP_FROM_*` value.
 *
 * Called with interrupts off, after the caller has finished with the stack
 * (`ip_loop_crowded`): from the tick, the idle wait and a system call's
 * exit. A call made from inside a delivery returns at once, and the drain
 * already running delivers what that delivery put on the ring.
 */
void ip_loop_drain_from(int from);

/** The datagrams the drains of `from` have delivered since boot; 0 for a
 * value that is no `IP_LOOP_FROM_*`. */
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
 * A system call's exit asks this to drain before the ring fills and starts
 * refusing. Two conditions belong to the caller of `ip_loop_drain_from`:
 * interrupts are off, and the caller has finished whatever it was doing to
 * the stack, since a delivery runs the receive path and may transmit. A
 * pair is left to the timer, since a simultaneous open is a pair of crossing
 * SYNs and is a pair only while neither has been delivered.
 */
int ip_loop_crowded(void);

/**
 * The Internet checksum of the `len` bytes at `buf`: the one's complement of
 * the one's complement sum of its 16-bit big-endian words, an odd last byte
 * padded with a zero. A header whose checksum field is right sums to zero,
 * so a receiver checks one by asking for zero.
 */
uint16_t ip_checksum(const uint8_t *buf, uint16_t len);

#endif /* _IP_H_ */
