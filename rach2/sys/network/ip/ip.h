#ifndef _IP_H_
#define _IP_H_

#include "../../lib/kint.h"

/*
 * ip: the IPv4 layer, which every ICMP, UDP and TCP message travels in. A
 * received datagram is checked, reassembled when it came in fragments
 * (ipfrag.c), and given to its protocol. A datagram sent has a 20-byte
 * header with no options, and leaves by its route, by broadcast, or round
 * the loopback ring when it is addressed to this machine. One within its
 * path MTU carries DF, as Linux's path MTU discovery sends it; a larger
 * one leaves in fragments, unless its socket asked for DF with
 * IP_MTU_DISCOVER. The path MTU is the interface's MTU, lowered for a
 * destination by the fragmentation-needed reports that come back (pmtu.c).
 * Addresses are host-order uint32_t values (10.0.2.15 is
 * 0x0A00020F), converted at the header.
 */

/* Protocol numbers (the IP header's protocol field). */
#define IP_PROTO_ICMP 1
#define IP_PROTO_TCP  6
#define IP_PROTO_UDP  17

/* Fixed IPv4 header size (IHL = 5 words). */
#define IP_HDR_SIZE 20

/* The largest payload a datagram's 16-bit total length leaves room for
 * after the header. */
#define IP_PAYLOAD_MAX (65535 - IP_HDR_SIZE)

/* The flags and offset field: DF, MF, and the offset in 8-byte units. */
#define IP_DF          0x4000u
#define IP_MF          0x2000u
#define IP_FRAG_OFFSET 0x1FFFu

/* IP_MTU_DISCOVER's values, Linux's: never set DF; set it on a datagram
 * within the path MTU and fragment a larger one, the default; refuse a
 * larger one with EMSGSIZE (DO and PROBE); and, for INTERFACE and OMIT,
 * fragment by the interface's MTU, whatever the path's, with DF never
 * set. */
#define IP_PMTUDISC_DONT      0
#define IP_PMTUDISC_WANT      1
#define IP_PMTUDISC_DO        2
#define IP_PMTUDISC_PROBE     3
#define IP_PMTUDISC_INTERFACE 4
#define IP_PMTUDISC_OMIT      5

/* The least path MTU a report may set, Linux's `ip_rt_min_pmtu`: 512
 * bytes of payload behind the IPv4 and TCP headers. A smaller one counts as
 * this. */
#define IP_PMTU_MIN 552

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

/** `ip_source` by the route `netdev_route_on` chooses for `ifindex`, the
 * card a socket is bound to, or any card for 0. */
uint32_t ip_source_on(uint32_t dst, int ifindex);

/**
 * Check the IPv4 datagram of `len` bytes at `data` and give it to its
 * protocol. `from_loopback` is nonzero only for a datagram from the loopback
 * ring.
 *
 * A datagram with a bad version, header length, total length or header
 * checksum is dropped, and so is one addressed to neither an address of an
 * interface nor a broadcast; an interface with no address holds none, so
 * 0.0.0.0 is nobody's. Options are skipped. A datagram from the wire is
 * also dropped when it is addressed to 127/8, as Linux drops it, or its
 * source is an address of this machine or in 127/8. A broadcast is given to
 * UDP alone. A fragment goes to `ip_frag_input`, which gives the datagram it
 * completes to `ip_deliver`, as an accepted whole datagram is given.
 *
 * Each datagram counts in `net_mib` as received, and a dropped one by its
 * reason; a multicast datagram, for a group no socket here can join, counts
 * only as received, as Linux counts one for a group it has not joined.
 * Called with interrupts off, from `ethernet_rx` and the loopback drain.
 */
void ip_rx(const uint8_t *data, uint16_t len, int from_loopback);

/**
 * Give the accepted, whole datagram of `total_len` bytes at `data` to its
 * protocol: offered whole to the raw sockets of its protocol (`raw_rx`),
 * and its payload given to `icmp_rx`, `udp_rx` or `tcp_rx`; a unicast UDP
 * datagram for a port no socket has, one not sent to a broadcast as
 * `to_broadcast` says, is answered with a port unreachable
 * (`icmp_unreachable`). Called with interrupts off, from `ip_rx` and
 * `ip_frag_input`.
 */
void ip_deliver(const uint8_t *data, uint16_t total_len, int to_broadcast);

/*
 * Reassembly (ipfrag.c), as RFC 791 has it and Linux keeps it. A fragment
 * is kept in its datagram's queue, keyed by its source, destination,
 * protocol and identification, until every byte of the datagram has come,
 * when the datagram, its header the first fragment's with the total length
 * set and the fragment fields cleared, goes to `ip_deliver`. A datagram is
 * given up when two fragments overlap, as Linux gives one up, when its
 * fragments disagree about its end or it would pass 65535 bytes, past
 * IP_FRAG_PIECES fragments, and IP_FRAG_TIMEOUT_SECONDS after its first
 * fragment came; an exact duplicate is dropped alone. At most IP_FRAG_QUEUES
 * datagrams are reassembled at once, in at most IP_FRAG_MEM_MAX bytes of
 * the heap, Linux's `ipfrag_high_thresh`; a new one takes the place of the
 * one that would time out first. Each fragment counts in ReasmReqds, each
 * datagram made in ReasmOKs, and each given up in ReasmFails.
 */
#define IP_FRAG_QUEUES          16
#define IP_FRAG_PIECES          192
#define IP_FRAG_MEM_MAX         (4u * 1024u * 1024u)
#define IP_FRAG_TIMEOUT_SECONDS 30u

/**
 * Take the fragment of `len` bytes at `data`, whose header of `hdr_len`
 * bytes ip_rx has checked, sent to a broadcast when `to_broadcast` is set.
 * A fragment other than the last whose data is not a multiple of 8 bytes,
 * or one that reaches past 65535 bytes, is dropped as a header error.
 * Called with interrupts off, from `ip_rx`.
 */
void ip_frag_input(const uint8_t *data, uint16_t len, uint16_t hdr_len, int to_broadcast);

/**
 * Give up the reassemblies whose time is out, each counted in ReasmTimeout
 * and ReasmFails; one whose first fragment came is answered with a time
 * exceeded of code 1 (`icmp_time_exceeded`), as Linux answers it. Called
 * once a tick, with interrupts off, from the timer interrupt.
 */
void ip_frag_tick(void);

/* The hop limit a datagram leaves with unless the sender asked for another.
 * A socket may set its own with IP_TTL, which is what traceroute is. */
#define IP_TTL_DEFAULT 64

/** `ip_tx_from` from the source `ip_source` chooses. */
int ip_tx(uint32_t dst_ip, uint8_t proto, const uint8_t *payload, uint16_t plen, uint8_t ttl);

/** `ip_tx_opts` under IP_PMTUDISC_WANT, Linux's default. */
int ip_tx_from(uint32_t src, uint32_t dst_ip, uint8_t proto, const uint8_t *payload, uint16_t plen,
               uint8_t ttl);

/**
 * Send the `plen` bytes at `payload` as a datagram of protocol `proto` from
 * `src` to `dst_ip`, with the hop limit `ttl`, under `pmtudisc`, an
 * IP_PMTUDISC_* value, by the card of index `ifindex`, that of the
 * sender's SO_BINDTODEVICE, or by any card for 0. `src` is one of this
 * machine's addresses, which a connection or a bound socket keeps; 0, or an
 * address that is not this machine's (`ip_is_local`), takes the one
 * `ip_source_on` chooses.
 *
 * A datagram to 127/8 or to an address an interface holds goes onto the
 * loopback ring, whose MTU is `lo`'s, 65536 bytes, so that it never needs
 * fragments; `ip_loop_drain_from` delivers it later. A broadcast, to
 * 255.255.255.255 or a subnet's broadcast address, goes to the all-ones
 * station address by the card on that subnet, or the first card for
 * 255.255.255.255, or by `ifindex` when it is not 0. Any other datagram
 * leaves by the route `netdev_route_on` chooses for `ifindex`, to the next
 * hop's station address from ARP; on a miss an ARP
 * request goes out and the datagram waits for the reply in one of
 * IP_HOLD_SLOTS slots, for at most three seconds (ip.c). A datagram past
 * the path MTU (`ip_path_mtu`), or the interface's under
 * IP_PMTUDISC_INTERFACE and OMIT, leaves in fragments, each a multiple of 8
 * bytes of the payload but the last, all with one identification and none
 * with DF, counted in FragOKs and FragCreates.
 *
 * Returns 0 when the datagram left, went onto the loopback ring, or waits
 * for ARP: a datagram send promises no delivery. Returns a negative errno
 * for what the caller must hear about: -EMSGSIZE for a payload past
 * IP_PAYLOAD_MAX, or past the MTU under IP_PMTUDISC_DO or PROBE, counted
 * in FragFails; -ENETUNREACH for a destination with no route, -ENOBUFS when
 * the loopback ring is full, no memory holds a datagram past one slot of
 * it, or the driver did not take a frame; -EOPNOTSUPP for a broadcast of
 * anything but UDP; and -EINVAL for a datagram from 127/8 to an address
 * that is not this machine's, which no card may carry.
 * In a network namespace other than the first (`ip_ns`), only 127/8 is
 * reached, and only while the namespace's `lo` is up with its address;
 * anything else is -ENETUNREACH.
 *
 * A datagram is built in a buffer of this file's, so the caller has
 * interrupts off (`net_lock` in a system call).
 */
int ip_tx_opts(uint32_t src, uint32_t dst_ip, uint8_t proto, const uint8_t *payload, uint16_t plen,
               uint8_t ttl, int pmtudisc, int ifindex);

/* The interface a held datagram leaves by. */
struct netdev;

/**
 * The largest datagram, header included, that may leave by `out` for `dst`
 * without being fragmented: `out`'s MTU, or less when a
 * fragmentation-needed report has lowered the path's estimate for `dst`
 * within the last ten minutes (`ip_pmtu_lower`); NETDEV_LOOPBACK_MTU for a
 * NULL `out`, the loopback ring. Called with interrupts off.
 */
uint32_t ip_path_mtu(const struct netdev *out, uint32_t dst);

/**
 * Take the MTU `mtu` a fragmentation-needed report gave for the path to
 * `dst` (RFC 1191). An MTU below IP_PMTU_MIN counts as IP_PMTU_MIN, and the
 * path's estimate only falls, never rises, until it ages out ten minutes
 * after it last fell. The table holds 16 destinations, and a new one takes
 * the place of the one nearest its end. Called with interrupts off.
 */
void ip_pmtu_lower(uint32_t dst, uint32_t mtu);

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

struct netns;

/**
 * The network namespace the datagram being sent or delivered is in: NULL
 * for the first, whose interfaces are the machine's, else another, whose
 * one interface is its own `lo` (proc/ns.h). A sender of a namespace's
 * socket or connection sets it around its call to `ip_tx_from`, and
 * `ip_loop_drain_from` around each delivery of a datagram one sent, so that
 * the demultiplexers of TCP and UDP match only that namespace's sockets. It
 * is set only with interrupts off, and NULL again before they are on.
 */
extern const struct netns *ip_ns;

/** Drop the datagrams on the loopback ring of the network namespace `ns`,
 * which has ended, so that none reaches a namespace made at its address
 * later. Called with interrupts off. */
void ip_loop_ns_gone(const struct netns *ns);

/**
 * Put the datagram of `total_len` bytes at `dgram`, IPv4 or IPv6, on the
 * loopback ring, counted as `lo`'s traffic, as Linux counts it, and marked
 * with `ip_ns`, the namespace it is delivered in. A datagram past a slot's
 * 1520 bytes is copied to the heap. Returns 0, -EMSGSIZE past `lo`'s MTU,
 * NETDEV_LOOPBACK_MTU, or -ENOBUFS when the ring is full or no memory holds
 * the datagram, which is counted in `ip_loop_dropped` and logged once.
 * Called with interrupts off.
 */
int ip_loop_put(const uint8_t *dgram, uint32_t total_len);

/** `ip_loop_put` of an IPv6 datagram that the drain gives `ip6_loop_rx` as
 * received by the card of index `ifindex`, or as `lo`'s for 0. */
int ip_loop_put_on(const uint8_t *dgram, uint32_t total_len, int ifindex);

/** `ip_loop_put_on` of the datagram whose first `hlen` bytes, its headers,
 * are at `hdr` and the rest, `plen` bytes, at `payload`, so that a sender
 * need not gather a datagram past its own buffer first. Marks the current
 * thread's work (WORK_LOOP), so that its return to user mode delivers the
 * ring. */
int ip_loop_put_parts(const uint8_t *hdr, uint32_t hlen, const uint8_t *payload, uint32_t plen,
                      int ifindex);

/** `ip_loop_drain_from` for the tick, which calls it once a tick from the
 * timer interrupt. */
void ip_loop_drain(void);

/* Who drains the loopback: the tick, the idle wait, or the return to user
   mode of a thread that put a datagram on it, or a thread's end; `ip_loop_delivered` counts the
   datagrams each delivered. */
#define IP_LOOP_FROM_TICK 0
#define IP_LOOP_FROM_IDLE 1
#define IP_LOOP_FROM_CALL 2

/**
 * Deliver the datagrams on the loopback ring through `ip_rx`, or `ip6_rx`
 * for an IPv6 datagram, at most four times the ring's slots in one call,
 * and count them under `from`, an `IP_LOOP_FROM_*` value.
 *
 * Called with interrupts off, after the caller has finished with the stack,
 * since a delivery runs the receive path and may transmit: from the tick,
 * the idle wait, the return to user mode of a thread that put a datagram
 * on the ring (`syscall_loop_drain`) and a thread's end, once its
 * descriptors are closed. A call made from inside a delivery returns at
 * once, and the drain already running delivers what that delivery put on
 * the ring.
 */
void ip_loop_drain_from(int from);

/** The datagrams the drains of `from` have delivered since boot; 0 for a
 * value that is no `IP_LOOP_FROM_*`. */
uint32_t ip_loop_delivered(int from);

/** Nonzero while the loopback ring holds anything at all. */
int ip_loop_pending(void);

/** Count the datagrams on the loopback ring as left for the timer by an idle
 * machine. The boot processor's tick calls it, before its own work can send
 * onto the ring, when every processor online is halted in an idle wait. The
 * idle wait of a blocked process (`idle_wait_irq_off`) delivers the ring
 * before it halts, so what the ring holds then is left for the timer. */
void ip_loop_count_idle(void);

/** The datagrams `ip_loop_count_idle` has counted since boot. */
uint32_t ip_loop_left_idle(void);

/** Datagrams the loopback ring had no room for since boot. */
uint32_t ip_loop_dropped(void);

/** The most datagrams the loopback ring has held at once, which says how much
 * room the ring has over what one system call can put in it. */
uint32_t ip_loop_peak(void);

/**
 * The Internet checksum of the `len` bytes at `buf`: the one's complement of
 * the one's complement sum of its 16-bit big-endian words, an odd last byte
 * padded with a zero. A header whose checksum field is right sums to zero,
 * so a receiver checks one by asking for zero.
 */
uint16_t ip_checksum(const uint8_t *buf, uint16_t len);

#endif /* _IP_H_ */
