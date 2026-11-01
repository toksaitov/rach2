#ifndef _ICMP_H_
#define _ICMP_H_

#include "../../lib/kint.h"

/*
 * icmp: the Internet Control Message Protocol. An echo request (type 8) is
 * answered with an echo reply (type 0) that has the request's identifier,
 * sequence and data; `icmp_ping` sends the kernel's own request and waits
 * for the matching reply. A UDP datagram for a port no socket has is
 * answered with a port unreachable. Of the errors that come in, a port
 * unreachable is given to UDP, and a report that a datagram was too big for
 * a link lowers its destination's path MTU and is given to TCP.
 *
 * QEMU's SLIRP does not pass ICMP from the host to the guest, but it answers
 * the guest's echo requests, so pings are tested from the guest outward (to
 * the gateway 10.0.2.2, for one).
 */

#define ICMP_ECHO_REQUEST  8
#define ICMP_ECHO_REPLY    0
/* Destination unreachable, and two of its codes: port unreachable, and
 * fragmentation needed, for a datagram with DF set too big for a link on the
 * way, whose bytes 6 and 7 give the link's MTU (RFC 1191). */
#define ICMP_UNREACHABLE   3
#define ICMP_PORT_UNREACH  3
#define ICMP_FRAG_NEEDED   4
/* Time exceeded, and its code for a datagram whose fragments did not all
 * come before the reassembly's time ran out. */
#define ICMP_TIME_EXCEEDED 11
#define ICMP_TIME_REASM    1

/**
 * Take the ICMP message of `len` bytes at `data`, the payload of a datagram
 * from `src_ip` to `dst_ip` that arrived with the hop limit `ttl`.
 *
 * An echo request is answered from `dst_ip` with the same identifier,
 * sequence and data. An echo reply from the peer of the ping in flight, with
 * its identifier and sequence, ends the ping's wait. A fragmentation-needed
 * report lowers the quoted destination's path MTU (`ip_pmtu_lower`), about a
 * TCP segment only when it is in flight (`tcp_in_flight`), and then also goes
 * to `tcp_path_mtu`; a port unreachable about a UDP datagram goes to
 * `udp_refused`. A message shorter than its 8-byte
 * header is dropped, and anything else is counted and ignored. Called with
 * interrupts off, from `ip_rx`.
 */
void icmp_rx(uint32_t src_ip, uint32_t dst_ip, uint8_t ttl, const uint8_t *data, uint16_t len);

/** Count an ICMP message of type `type` sent, here or by a raw socket, in
 * `net_mib`. */
void icmp_count_out(uint8_t type);

/**
 * Answer the datagram `dgram` of `len` bytes, which came to this machine,
 * not by broadcast, and which nothing here takes, with a destination
 * unreachable of code `code` to its source.
 *
 * The error quotes the datagram's header and as much of the rest as fits a
 * datagram of 576 bytes (RFC 1812, 4.3.2.3), which `traceroute` reads to
 * match the answer to its probe and a UDP client to its socket. No error
 * goes to a source of 0.0.0.0, a broadcast or a multicast group, which is no
 * single host. An error to another host is held to a limit of one a second
 * after a burst of six, Linux's figures, though the limit here is the
 * machine's and not each peer's; one to this machine's own address is not,
 * as Linux does not limit the errors it sends over its loopback. Called with
 * interrupts off, from `ip_rx`.
 */
void icmp_unreachable(const uint8_t *dgram, uint16_t len, uint8_t code);

/** Answer the datagram `dgram` of `len` bytes with a time exceeded of code
 * `code` to its source, under the rules of `icmp_unreachable`: the
 * quote's header and first 8 bytes of a datagram whose fragments did not
 * all come (`ip_frag_tick`). Called with interrupts off. */
void icmp_time_exceeded(const uint8_t *dgram, uint16_t len, uint8_t code);

/* The most data an echo request carries: the largest datagram less the IP
 * and ICMP headers, sent in fragments past one frame. */
#define ICMP_PING_DATA_MAX (65535u - 20u - 8u)

/** `icmp_ping_data` with no data and the hop limit `IP_TTL_DEFAULT`. */
int icmp_ping(uint32_t dst_ip);

/**
 * Send echo requests to `dst_ip` and wait for an answer: the round trip in
 * ticks, or -ETIMEDOUT when no request was answered, -EBUSY when another
 * ping is in flight, -EMSGSIZE for a `size` past `ICMP_PING_DATA_MAX`,
 * -ENOMEM when no block holds a request past one frame, or -EINTR when a
 * signal ended the wait.
 *
 * Up to four requests go, a second apart, each with its own sequence number.
 * Deadlines are measured from the current tick, including ticks whose
 * interrupts have not yet been taken. The handler's count ends each wait.
 * Each request has `size` bytes of data, each byte its offset modulo 256 as
 * iputils fills them, and the hop limit `ttl`; only an answer to the latest
 * ends the wait.
 * The answer's ICMP length, which a peer may make shorter or longer than the
 * request, goes to `*got`, and the hop limit it arrived with to `*got_ttl`,
 * unless `got` is NULL.
 *
 * The machine has one ping in flight at a time, and a caller that finds it
 * taken sends nothing. A process in a system call sleeps for the answer; a
 * caller outside one, the debug shell, polls with interrupts on. Takes
 * `net_lock` itself, and may sleep.
 */
int icmp_ping_data(uint32_t dst_ip, uint32_t size, uint8_t ttl, uint16_t *got, uint8_t *got_ttl);

/**
 * `icmp_ping_data` for IPv6: echo requests to `dst`, in the scope of the
 * interface of index `ifindex`, 0 for none, each with `size` bytes of data,
 * at most `ICMP6_PING_DATA_MAX`, and the hop limit `hlim`; a request past
 * the path MTU leaves in fragments. A reply from `dst`, or from any host
 * when `dst` is a group, ends the wait. Returns as `icmp_ping_data` does,
 * and also the error of `icmp6_send` when the first request cannot be sent,
 * or -ENOMEM when no block holds a request past one frame.
 * Shares the one ping in flight with IPv4's.
 */
int icmp_ping6_data(const uint8_t *dst, int ifindex, uint32_t size, uint8_t hlim, uint16_t *got,
                    uint8_t *got_hlim);

/** Give the ping in flight an ICMPv6 echo reply from `src` with the
 * identifier `id` and sequence `seq`, `len` bytes of ICMPv6 that arrived
 * with the hop limit `hlim`. Called with interrupts off, from `icmp6_rx`. */
void icmp_ping_reply6(const uint8_t *src, uint16_t id, uint16_t seq, uint16_t len, uint8_t hlim);

#endif /* _ICMP_H_ */
