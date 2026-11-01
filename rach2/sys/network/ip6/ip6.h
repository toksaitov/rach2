#ifndef _IP6_H_
#define _IP6_H_

#include "../../lib/kint.h"

/*
 * ip6: the IPv6 layer (RFC 8200). A received datagram is checked and given
 * to ICMPv6; a datagram sent has a 40-byte header and no extension header,
 * and leaves round the loopback ring when it is addressed to this machine,
 * to a multicast group's station address, or to a neighbour whose station
 * address is known. An address is 16 bytes in network order, as on the
 * wire, and is passed as a pointer to its first byte; a scope is an
 * interface index, 0 for none.
 *
 * Neither fragments nor extension headers are handled: a datagram that
 * carries one is dropped, and none is sent. There is no neighbour cache, so
 * a unicast datagram can leave only for ::1 and this machine's addresses.
 */

/* The fixed header: version, class and flow label (4), payload length (2),
 * next header (1), hop limit (1), source (16) and destination (16). */
#define IP6_HDR_SIZE 40
#define IP6_ADDR_LEN 16

/* The next-header numbers this layer knows: the extension headers, which it
 * drops, ICMPv6, which it delivers, and the end of the chain. */
#define IP6_NH_HOPOPTS  0
#define IP6_NH_TCP      6
#define IP6_NH_UDP      17
#define IP6_NH_ROUTING  43
#define IP6_NH_FRAGMENT 44
#define IP6_NH_ESP      50
#define IP6_NH_AH       51
#define IP6_NH_ICMP6    58
#define IP6_NH_NONE     59
#define IP6_NH_DSTOPTS  60

/* The hop limit a unicast datagram leaves with unless its sender asked for
 * another, and the one a multicast datagram leaves with, which keeps it on
 * the link, as Linux's IPV6_MULTICAST_HOPS defaults. */
#define IP6_HOP_LIMIT_DEFAULT 64
#define IP6_HOP_LIMIT_MCAST   1

/* The smallest MTU a link carrying IPv6 may have (RFC 8200, section 5). */
#define IP6_MTU_MIN 1280

/* The largest datagram, header included, this layer sends or the loopback
 * ring carries: one Ethernet payload. */
#define IP6_MTU 1500u

struct netdev;

/*
 * The counters of RFC 4293 and RFC 4292 for IPv6 and ICMPv6, which Linux
 * reports in /proc/net/snmp6 under these names. Every path that changes one
 * runs with interrupts off, as net.h has it.
 */
struct net_mib6 {
    uint32_t ip6_in_receives;
    uint32_t ip6_in_hdr_errors;  /* a bad version or length, or a hop limit of 0 */
    uint32_t ip6_in_addr_errors; /* for another host, a group not joined, or a spoofed source */
    uint32_t ip6_in_unknown_protos;
    uint32_t ip6_in_discards; /* an extension header, which this layer drops */
    uint32_t ip6_in_delivers;
    uint32_t ip6_in_mcast_pkts;
    uint32_t ip6_out_requests;
    uint32_t ip6_out_discards; /* refused by a full card or loopback ring */
    uint32_t ip6_out_no_routes;
    uint32_t ip6_out_mcast_pkts;
    uint32_t icmp6_in_msgs;
    uint32_t icmp6_in_errors; /* too short or a bad checksum */
    uint32_t icmp6_in_dest_unreachs;
    uint32_t icmp6_in_pkt_too_bigs;
    uint32_t icmp6_in_time_excds;
    uint32_t icmp6_in_parm_problems;
    uint32_t icmp6_in_echos;
    uint32_t icmp6_in_echo_replies;
    uint32_t icmp6_out_msgs;
    uint32_t icmp6_out_dest_unreachs;
    uint32_t icmp6_out_parm_problems;
    uint32_t icmp6_out_echos;
    uint32_t icmp6_out_echo_replies;
};

extern struct net_mib6 net_mib6;

/* The addresses every layer names: ::, ::1, and the all-nodes group,
 * ff02::1. */
extern const uint8_t ip6_any[IP6_ADDR_LEN];
extern const uint8_t ip6_loopback[IP6_ADDR_LEN];
extern const uint8_t ip6_all_nodes[IP6_ADDR_LEN];

/** Whether the addresses at `a` and `b` are the same. */
int ip6_equal(const uint8_t *a, const uint8_t *b);

/** Whether `a` is ::, the unspecified address. */
int ip6_is_unspecified(const uint8_t *a);

/** Whether `a` is ::1. */
int ip6_is_loopback(const uint8_t *a);

/** Whether `a` is a multicast group, ff00::/8. */
int ip6_is_multicast(const uint8_t *a);

/** Whether `a` is a link-local unicast address, fe80::/10. */
int ip6_is_linklocal(const uint8_t *a);

/** Whether `a` is an IPv4-mapped address, ::ffff:0:0/96, which never
 * appears on the wire. */
int ip6_is_v4mapped(const uint8_t *a);

/**
 * Whether a datagram to `a` stays on one link, and so needs an interface to
 * leave by: a link-local address, and a multicast group of interface or
 * link scope (ff01::/16 and ff02::/16, whatever the flag bits).
 */
int ip6_link_scoped(const uint8_t *a);

/** The NETDEV_SCOPE_* of the unicast address `a`. */
int ip6_scope(const uint8_t *a);

/** Whether the first `prefix` bits of `a` and `b` agree. */
int ip6_prefix_match(const uint8_t *a, const uint8_t *b, int prefix);

/** Write to `out` the solicited-node group of `a`, ff02::1:ff00:0/104 with
 * the low 24 bits of `a` (RFC 4291, 2.7.1). */
void ip6_solicited_node(const uint8_t *a, uint8_t *out);

/** Write to `out` the station address the group `group` is sent to on
 * Ethernet, 33:33 and its low 32 bits (RFC 2464, section 7). */
void ip6_mcast_mac(const uint8_t *group, uint8_t *out);

/** Format `a` as RFC 5952 writes it, the longest run of two or more zero
 * groups as `::`, into `out`, which needs 40 bytes. */
void ip6_format(const uint8_t *a, char *out);

/**
 * The Internet checksum of an upper-layer message of `len` bytes at `data`
 * with next header `nh` from `src` to `dst`, over the pseudo-header of RFC
 * 8200, 8.1: the two addresses, the 32-bit length and the next header. A
 * message whose checksum field is right gives 0.
 */
uint16_t ip6_checksum(const uint8_t *src, const uint8_t *dst, uint8_t nh, const uint8_t *data,
                      uint32_t len);

/**
 * Choose the address of this machine a datagram to `dst` leaves from, by
 * `out`, into `src`: ::1 for ::1; else, of `out`'s addresses that are not
 * tentative, one of the same scope as `dst`, the longest prefix shared with
 * it first, and a deprecated one only when nothing else is left (RFC 6724,
 * rules 2, 3 and 8, in that order). A multicast destination takes the scope
 * of a link-local address for a group of link scope and of a global one
 * otherwise. Returns 0, or -EADDRNOTAVAIL when `out` is NULL or has no
 * address that may be used.
 */
int ip6_source(const struct netdev *out, const uint8_t *dst, uint8_t *src);

/**
 * Check the IPv6 datagram of `len` bytes at `data`, received on `nd`, and
 * give it to its protocol. `nd` is NULL for a datagram from the loopback
 * ring, which is `lo`'s.
 *
 * A datagram is dropped when its version is not 6, its payload length is 0
 * or runs past `len`, or its source is a multicast group. One from the wire
 * is also dropped when either address is ::1, or its source is an address
 * of this machine. It is this machine's when its destination is an address
 * an interface holds, a link-local one only on the interface that holds
 * it, ::1 from the ring, or a group `nd` is a member of. A datagram whose
 * next header is an extension header is dropped; one of a protocol this
 * layer does not have is answered with a parameter problem pointing at its
 * next-header field, unless it was sent to a group. An ICMPv6 message is
 * offered to the raw sockets (`raw6_rx`) and given to `icmp6_rx`.
 *
 * Counts in `net_mib6`. Called with interrupts off, from `ethernet_rx` and
 * the loopback drain.
 */
void ip6_rx(struct netdev *nd, const uint8_t *data, uint16_t len);

/**
 * Send the `plen` bytes at `payload` as a datagram of next header `nh` from
 * `src` to `dst`, with the hop limit `hlim`, by the interface of index
 * `ifindex`, or the one the route chooses when it is 0.
 *
 * `src` is NULL or :: to take the one `ip6_source` chooses, or else must be
 * an address of this machine; an upper layer that sums a pseudo-header
 * chooses the source first, with `ip6_route` and `ip6_source`. A datagram
 * to ::1 or to an address an interface holds goes onto the loopback ring.
 * One to a group goes to the group's station address by its interface. One
 * to any other address needs its next hop's station address, which no
 * neighbour cache supplies yet.
 *
 * Returns 0 when the datagram left or went onto the loopback ring, or a
 * negative errno: -EMSGSIZE when the datagram is past the path MTU,
 * -EINVAL for a link-scoped destination with no interface to leave by
 * where there is more than one card, or for a source that is not this
 * machine's, -ENETUNREACH for a destination no route reaches,
 * -EADDRNOTAVAIL when the interface has no source address to use,
 * -EHOSTUNREACH for a next hop whose station address is not known, or
 * -ENOBUFS when the ring is full or the driver did not take the frame.
 *
 * The datagram is built in a buffer of this file's, so the caller has
 * interrupts off (`net_lock` in a system call).
 */
int ip6_tx(const uint8_t *src, const uint8_t *dst, int ifindex, uint8_t nh, const uint8_t *payload,
           uint16_t plen, uint8_t hlim);

/**
 * The interface a datagram to `dst` leaves by, with the next hop in `via`,
 * for the scope `ifindex`, the interface index a caller gave with the
 * address or 0. ::1 and an address an interface holds are `lo`'s. A
 * link-scoped destination (`ip6_link_scoped`) leaves by the interface
 * `ifindex` names, or by the one card when it is 0 and there is one, and
 * its next hop is itself. A group of wider scope leaves by `ifindex`, or
 * the first card. No other destination has a route yet.
 *
 * Counts one that finds no route in `ip6_out_no_routes`.
 *
 * @return the interface, or NULL with no route; `via` is filled only when
 *         there is one.
 */
struct netdev *ip6_route(const uint8_t *dst, int ifindex, uint8_t *via);

/**
 * The most bytes, IPv6 header included, a datagram to `dst` by `out` may
 * have without being fragmented: the link's MTU, 1500 bytes for a card and
 * for the loopback ring alike, or 0 for a NULL `out`.
 */
uint32_t ip6_path_mtu(const struct netdev *out, const uint8_t *dst);

#endif /* _IP6_H_ */
