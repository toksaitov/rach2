#ifndef _IP6_H_
#define _IP6_H_

#include "../../lib/kint.h"

/*
 * ip6: the IPv6 layer (RFC 8200). A received datagram is checked, its
 * extension headers are processed in the order they come, its fragments
 * are reassembled (frag6.c), and what follows them is given to ICMPv6. A
 * datagram sent has a 40-byte header and no extension header unless it is
 * past its destination's path MTU (pmtu6.c), when it leaves in fragments,
 * or is MLD's, which carries a router alert. It leaves round the loopback
 * ring when it is addressed to this machine, to a multicast group's
 * station address, or to its next hop by the neighbour cache (nd6.h). An
 * address is 16 bytes in network order, as on the wire, and is passed as a
 * pointer to its first byte; a scope is an interface index, 0 for none.
 */

/* The fixed header: version, class and flow label (4), payload length (2),
 * next header (1), hop limit (1), source (16) and destination (16). */
#define IP6_HDR_SIZE 40
#define IP6_ADDR_LEN 16

/* The next-header numbers this layer knows: the extension headers, the
 * upper layers, and the end of the chain. */
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
#define IP6_NH_MOBILITY 135
#define IP6_NH_HIP      139
#define IP6_NH_SHIM6    140

/* The fragment header (RFC 8200, 4.5): next header (1), reserved (1), the
 * offset in 8-byte units in the top 13 bits of a 16-bit word whose lowest
 * bit is the more-fragments flag, and a 32-bit identification. */
#define IP6_FRAG_HDR_SIZE 8
#define IP6_FRAG_MORE     0x0001u
#define IP6_FRAG_OFFSET   0xfff8u

/* The largest payload a datagram's 16-bit length can describe. */
#define IP6_PAYLOAD_MAX 65535u

/* The hop limit a unicast datagram leaves with unless its sender asked for
 * another, and the one a multicast datagram leaves with, which keeps it on
 * the link, as Linux's IPV6_MULTICAST_HOPS defaults. */
#define IP6_HOP_LIMIT_DEFAULT 64
#define IP6_HOP_LIMIT_MCAST   1

/* The smallest MTU a link carrying IPv6 may have (RFC 8200, section 5). */
#define IP6_MTU_MIN 1280

/* The largest datagram, header included, that leaves a card whole: one
 * Ethernet payload, a card's largest MTU. The link's own MTU, at most this,
 * is `nd6_link_mtu`, and a larger datagram leaves in fragments. The
 * loopback ring's MTU is `lo`'s, NETDEV_LOOPBACK_MTU. */
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
    uint32_t ip6_in_discards; /* a fragment with no memory to hold it */
    uint32_t ip6_in_delivers;
    uint32_t ip6_in_mcast_pkts;
    uint32_t ip6_out_requests;
    uint32_t ip6_out_discards; /* refused by a full card or loopback ring */
    uint32_t ip6_out_no_routes;
    uint32_t ip6_out_mcast_pkts;
    uint32_t ip6_reasm_timeout; /* datagrams whose fragments did not all come in time */
    uint32_t ip6_reasm_reqds;   /* fragments received, an atomic one among them */
    uint32_t ip6_reasm_oks;
    uint32_t ip6_reasm_fails; /* datagrams given up: overlap, timeout, limits */
    uint32_t ip6_frag_oks;    /* datagrams sent in fragments */
    uint32_t ip6_frag_fails;  /* datagrams one of whose fragments could not leave */
    uint32_t ip6_frag_creates;
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
    uint32_t icmp6_out_time_excds;
    uint32_t icmp6_out_parm_problems;
    uint32_t icmp6_out_echos;
    uint32_t icmp6_out_echo_replies;
    /* Neighbour discovery's and MLD's messages, in and out. */
    uint32_t icmp6_in_router_solicits;
    uint32_t icmp6_in_router_advertisements;
    uint32_t icmp6_in_neighbor_solicits;
    uint32_t icmp6_in_neighbor_advertisements;
    uint32_t icmp6_in_redirects;
    uint32_t icmp6_in_group_memb_queries;
    uint32_t icmp6_in_group_memb_responses;
    uint32_t icmp6_in_group_memb_reductions;
    uint32_t icmp6_in_mldv2_reports;
    uint32_t icmp6_out_router_solicits;
    uint32_t icmp6_out_neighbor_solicits;
    uint32_t icmp6_out_neighbor_advertisements;
    uint32_t icmp6_out_group_memb_responses;
    uint32_t icmp6_out_group_memb_reductions;
    uint32_t icmp6_out_mldv2_reports;
    /* UDP over IPv6, which net_mib's UDP counts leave out. */
    uint32_t udp6_in_datagrams;
    uint32_t udp6_no_ports;  /* no socket has the port */
    uint32_t udp6_in_errors; /* a bad length or checksum */
    uint32_t udp6_out_datagrams;
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

/** How many leading bits `a` and `b` share, 0 to 128. */
int ip6_common_prefix(const uint8_t *a, const uint8_t *b);

/** Write to `out` the solicited-node group of `a`, ff02::1:ff00:0/104 with
 * the low 24 bits of `a` (RFC 4291, 2.7.1). */
void ip6_solicited_node(const uint8_t *a, uint8_t *out);

/** Write to `out` the station address the group `group` is sent to on
 * Ethernet, 33:33 and its low 32 bits (RFC 2464, section 7). */
void ip6_mcast_mac(const uint8_t *group, uint8_t *out);

/** Format `a` as RFC 5952 writes it, the longest run of two or more zero
 * groups as `::`, into `out`, which needs 40 bytes. */
void ip6_format(const uint8_t *a, char *out);

/** Read the `len` bytes at `s`, which need no terminator, as an IPv6
 * address in the text of RFC 4291, 2.2, into `out`: 0, or -1 for a text
 * that is not one. */
int ip6_parse(const char *s, uint32_t len, uint8_t *out);

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
 * Give the IPv6 datagram of `len` bytes at `data`, taken from the loopback
 * ring, to `ip6_rx`: as `lo`'s when `ifindex` is 0, and as received by the
 * card of index `ifindex` when it is a copy of a datagram this machine sent
 * to a group that card is a member of (IP6_TXF_LOOP), which is not judged a
 * spoof for its own source. Called with interrupts off, from the drain.
 */
void ip6_loop_rx(int ifindex, const uint8_t *data, uint32_t len);

/**
 * Check the IPv6 datagram of `len` bytes at `data`, received on `nd`, and
 * give it to its protocol. `nd` is NULL for a datagram from the loopback
 * ring, which is `lo`'s.
 *
 * A datagram is dropped when `nd` has IPv6 disabled, its version is not 6,
 * its payload length is 0 or runs past `len`, or its source is a multicast
 * group. One from the wire is also dropped when either address is ::1, or
 * its source is an address of this machine. It is this machine's when its
 * destination is an address
 * an interface holds, a link-local one only on the interface that holds
 * it, ::1 from the ring, or a group `nd` is a member of.
 *
 * Its extension headers are then processed as RFC 8200, section 4, says.
 * Hop-by-hop options are taken only right after the IPv6 header; an option
 * this layer does not know is skipped or answered by the action its type's
 * top two bits name. A routing header with segments left is refused with a
 * parameter problem, since this machine routes no datagram onward, and one
 * with none left is skipped. A fragment goes to `frag6_input`, and the
 * datagram it completes is processed from the header after the fragment
 * header. Of the upper layers, an ICMPv6 message is offered to the raw
 * sockets (`raw6_rx`) and given to `icmp6_rx`; a next header this layer
 * does not have is answered with a parameter problem pointing at the field
 * that names it, unless the datagram was sent to a group.
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
 * to any other address goes to its next hop by the neighbour cache
 * (`nd6_output`), which holds it while it asks for the hop's station
 * address; a failure to learn it is answered later with an ICMPv6 address
 * unreachable. A datagram past the destination's path MTU
 * (`ip6_path_mtu`) leaves in fragments, as `frag6_output` says.
 *
 * Returns 0 when the datagram left or went onto the loopback ring, or a
 * negative errno: -EMSGSIZE when the driver refused the frame's length,
 * -EINVAL for a link-scoped destination with no interface to leave by
 * where there is more than one card, or for a source that is not this
 * machine's, -ENETUNREACH for a destination no route reaches,
 * -EADDRNOTAVAIL when the interface has no source address to use, or
 * -ENOBUFS when the ring is full or the driver did not take a frame.
 *
 * The datagram is built in a buffer of this file's, so the caller has
 * interrupts off (`net_lock` in a system call).
 */
int ip6_tx(const uint8_t *src, const uint8_t *dst, int ifindex, uint8_t nh, const uint8_t *payload,
           uint16_t plen, uint8_t hlim);

/* What `ip6_tx_opts` may add to a send: the source is :: as given, not
 * one chosen, for duplicate address detection (RFC 4862, 5.4.2); a
 * hop-by-hop header with the router alert of MLD (RFC 3810, 5); the
 * destination is on the link of the interface `ifindex` names, whatever
 * the routes say, for neighbour discovery's answers and probes; a datagram
 * to a group the card it leaves by is a member of also comes back to this
 * machine, as IPV6_MULTICAST_LOOP asks; and the traffic class, bits 8 to
 * 15, 0 unless given. */
#define IP6_TXF_UNSPEC         0x1
#define IP6_TXF_RTALERT        0x2
#define IP6_TXF_ONLINK         0x4
#define IP6_TXF_LOOP           0x8
#define IP6_TXF_TCLASS(c)      (((int)(c) & 0xff) << 8)
#define IP6_TXF_CLASS_OF(opts) ((uint8_t)(((opts) >> 8) & 0xff))

/**
 * `ip6_tx` with the IP6_TXF_* of `opts`. IP6_TXF_UNSPEC sends from ::
 * whatever `src` is, and only by a card; IP6_TXF_ONLINK needs a card that
 * is up in `ifindex`, -ENETUNREACH otherwise. IP6_TXF_LOOP puts a copy of a
 * datagram to a group on the loopback ring as received by the card it left
 * by, when the card is a member, unless it left in fragments.
 */
int ip6_tx_opts(const uint8_t *src, const uint8_t *dst, int ifindex, uint8_t nh,
                const uint8_t *payload, uint16_t plen, uint8_t hlim, int opts);

/**
 * Send the datagram of `len` bytes at `dg`, header included, which the
 * caller built whole, toward `dst` in the scope `ifindex`, as a raw socket
 * of IPPROTO_RAW sends it: the route is that of `dst`, and the datagram
 * leaves as it is, whatever its header says. Returns 0, -EINVAL for a
 * datagram shorter than its header, not of version 6, or whose payload
 * length disagrees with `len`, -EMSGSIZE for one past the path MTU, which
 * is not fragmented, or an error of `ip6_tx` for the route and the emit.
 * Called with interrupts off.
 */
int ip6_tx_whole(const uint8_t *dg, uint32_t len, const uint8_t *dst, int ifindex);

/**
 * The interface a datagram to `dst` leaves by, with the next hop in `via`,
 * for the scope `ifindex`, the interface index a caller gave with the
 * address or 0. ::1 and an address an interface holds are `lo`'s. A
 * link-scoped destination (`ip6_link_scoped`) leaves by the interface
 * `ifindex` names, or by the one card when it is 0 and there is one, and
 * its next hop is itself. A group of wider scope leaves by `ifindex`, or
 * the first card. Any other destination is the routing table's
 * (`route6_lookup`), only `ifindex`'s routes when it is not 0. An interface
 * that is down or has IPv6 disabled is no route.
 *
 * Counts one that finds no route in `ip6_out_no_routes`.
 *
 * @return the interface, or NULL with no route; `via` is filled only when
 *         there is one.
 */
struct netdev *ip6_route(const uint8_t *dst, int ifindex, uint8_t *via);

/** `ip6_route` without the count, for a caller that only asks which
 * interface a later send would take. */
struct netdev *ip6_route_find(const uint8_t *dst, int ifindex, uint8_t *via);

/**
 * The most bytes, IPv6 header included, a datagram to `dst` by `out` may
 * have without being fragmented: the link's MTU, `nd6_link_mtu` for a card
 * and NETDEV_LOOPBACK_MTU for the loopback ring, or less when a Packet Too Big has
 * lowered the destination's within the last ten minutes
 * (`ip6_pmtu_lower`); 0 for a NULL `out`. A link-scoped destination is
 * told apart by `out`.
 */
uint32_t ip6_path_mtu(const struct netdev *out, const uint8_t *dst);

/**
 * Take the MTU `mtu` a Packet Too Big reported for the path to `dst`, of
 * the scope `ifindex` for a link-scoped destination, as RFC 8201 says: an
 * MTU below 1280 counts as 1280, and the path's estimate only falls, never
 * rises, until it ages out ten minutes after it last fell. The table holds
 * `PMTU6_ENTRIES` destinations, and a new one takes the place of the one
 * that would age out first. Called with interrupts off, from `icmp6_rx`.
 */
void ip6_pmtu_lower(const uint8_t *dst, int ifindex, uint32_t mtu);

/** Run the layer's timers, once a tick, from the timer interrupt:
 * reassemblies that ran out of time (`frag6_tick`), and neighbour
 * discovery's and MLD's (`nd6_tick`). */
void ip6_tick(void);

#endif /* _IP6_H_ */
