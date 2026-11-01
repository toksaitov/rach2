#ifndef _ICMP6_H_
#define _ICMP6_H_

#include "../../lib/kint.h"

/*
 * icmp6: ICMPv6 (RFC 4443). An echo request is answered with an echo reply
 * of the same identifier, sequence and data, and an echo reply may end the
 * kernel's own ping (`icmp_ping6_data` in icmp.c). This machine sends a
 * parameter problem for a datagram whose headers it refuses, a time
 * exceeded for one whose reassembly ran out of time, and an address
 * unreachable for its own datagrams whose next hop did not answer. A Packet
 * Too Big about a datagram this machine sent lowers its destination's path
 * MTU; the other errors that come in are counted. The messages of neighbour
 * discovery go to nd6.c and those of multicast listener discovery to
 * mld6.c.
 *
 * A message is, big-endian, type (1 byte), code (1), checksum (2) and a
 * body; the checksum covers the IPv6 pseudo-header (`ip6_checksum`).
 */

#define ICMP6_DST_UNREACH    1
#define ICMP6_PACKET_TOO_BIG 2
#define ICMP6_TIME_EXCEEDED  3
#define ICMP6_PARAM_PROB     4
#define ICMP6_ECHO_REQUEST   128
#define ICMP6_ECHO_REPLY     129

/* The codes of the errors this machine sends. Destination unreachable: no
 * route, a neighbour that did not answer, the port. Time exceeded: a
 * reassembly out of time. Parameter problem: an erroneous header field, a
 * next header it does not recognize, an option it does not recognize, and
 * a first fragment without the whole header chain (RFC 7112). */
#define ICMP6_DST_NOROUTE      0
#define ICMP6_DST_ADDR         3
#define ICMP6_DST_PORT         4
#define ICMP6_TIME_REASM       1
#define ICMP6_PARAM_HEADER     0
#define ICMP6_PARAM_NEXTHEADER 1
#define ICMP6_PARAM_OPTION     2
#define ICMP6_PARAM_CHAIN      3

/* The types of multicast listener discovery (RFC 2710 and 3810): a query,
 * an MLDv1 report and done, and an MLDv2 report. */
#define ICMP6_MLD_QUERY     130
#define ICMP6_MLD_REPORT    131
#define ICMP6_MLD_DONE      132
#define ICMP6_MLD_V2_REPORT 143

/* The most data an echo request carries: the largest payload a datagram
 * has, less the ICMPv6 header. Past one 1500-byte datagram it leaves in
 * fragments. */
#define ICMP6_PING_DATA_MAX 65527u

struct netdev;

/**
 * Take the ICMPv6 message of `len` bytes at `data`, the payload of a
 * datagram from `src` to `dst` that arrived on `nd`, or on the loopback
 * ring for a NULL `nd`, with the hop limit `hlim`.
 *
 * A message shorter than its 4-byte header or with a bad checksum is
 * dropped and counted as an error. An echo request is answered from `dst`,
 * or, when `dst` is a group, from the address `ip6_source` chooses on `nd`;
 * an echo reply is given to `icmp_ping_reply6`, neighbour discovery to
 * `nd6_rx` and MLD to `mld6_rx`. A Packet Too Big that quotes a datagram
 * from this machine gives its MTU to `ip6_pmtu_lower` for the quoted
 * destination, unless it came from the wire about a destination of this
 * machine's own, which no link between could have refused. Called with
 * interrupts off, from `ip6_walk`.
 */
void icmp6_rx(struct netdev *nd, const uint8_t *src, const uint8_t *dst, uint8_t hlim,
              const uint8_t *data, uint16_t len);

/**
 * Send the ICMPv6 message of `len` bytes at `msg` from `src` to `dst` by
 * the interface of index `ifindex`, or the routed one for 0, with the hop
 * limit `hlim`, filling in its checksum, which is why `msg` is writable. A
 * NULL or unspecified `src` takes the address `ip6_source` chooses. Counts
 * the message by its type. Returns `ip6_tx`'s answer, or the error of
 * `ip6_route` and `ip6_source` when no source can be chosen. Called with
 * interrupts off.
 */
int icmp6_send(const uint8_t *src, const uint8_t *dst, int ifindex, uint8_t *msg, uint16_t len,
               uint8_t hlim);

/** `icmp6_send` with the IP6_TXF_* of `opts`, which `ip6_tx_opts` takes;
 * with IP6_TXF_UNSPEC the checksum is summed over a source of ::. */
int icmp6_send_opts(const uint8_t *src, const uint8_t *dst, int ifindex, uint8_t *msg, uint16_t len,
                    uint8_t hlim, int opts);

/**
 * Tell the sender of `dgram`, `len` bytes this machine sent from its own
 * address, that its next hop did not answer, with an address unreachable
 * (RFC 4861, 7.2.2) that goes round the loopback ring; nothing for an
 * ICMPv6 error, which would answer itself. Called with interrupts off.
 */
void icmp6_unreach_local(const uint8_t *dgram, uint16_t len);

/**
 * Answer the datagram `dgram` of `len` bytes, received on `nd` or from the
 * loopback ring for NULL, with an error of `type` and `code` whose 32-bit
 * field is `param`, quoting as much of the datagram as fits the 1280-byte
 * minimum MTU (RFC 4443, 2.4).
 *
 * No error answers a datagram from :: or a group, or an ICMPv6 error, and
 * none but a parameter problem of code 2 answers one sent to a group (RFC
 * 4443, 2.4 (e)); one to another host is held to one a second after a
 * burst of six, the machine's limit, as `icmp_unreachable` holds IPv4's.
 * Called with interrupts off, from the IPv6 layer.
 */
void icmp6_error(struct netdev *nd, const uint8_t *dgram, uint16_t len, uint8_t type, uint8_t code,
                 uint32_t param);

#endif /* _ICMP6_H_ */
