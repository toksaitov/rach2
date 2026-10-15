#ifndef _ICMP_H_
#define _ICMP_H_

#include "../../lib/kint.h"

/*
 * icmp: the Internet Control Message Protocol. An echo request (type 8) is
 * answered with an echo reply (type 0) that has the request's identifier,
 * sequence and data; `icmp_ping` sends the kernel's own request and waits
 * for the matching reply. A UDP datagram for a port no socket has is
 * answered with a port unreachable. Of the errors that come in, a port
 * unreachable is given to UDP, and a report that a segment was too big for a
 * link to TCP.
 *
 * QEMU's SLIRP does not pass ICMP from the host to the guest, but it answers
 * the guest's echo requests, so pings are tested from the guest outward (to
 * the gateway 10.0.2.2, for one).
 */

#define ICMP_ECHO_REQUEST 8
#define ICMP_ECHO_REPLY   0
/* Destination unreachable, and two of its codes: port unreachable, and
 * fragmentation needed, for a datagram with DF set too big for a link on the
 * way, whose bytes 6 and 7 give the link's MTU (RFC 1191). */
#define ICMP_UNREACHABLE  3
#define ICMP_PORT_UNREACH 3
#define ICMP_FRAG_NEEDED  4

/**
 * Take the ICMP message of `len` bytes at `data`, the payload of a datagram
 * from `src_ip` to `dst_ip` that arrived with the hop limit `ttl`.
 *
 * An echo request is answered from `dst_ip` with the same identifier,
 * sequence and data. An echo reply from the peer of the ping in flight, with
 * its identifier and sequence, ends the ping's wait. A fragmentation-needed
 * report about a TCP segment goes to `tcp_path_mtu`, and a port unreachable
 * about a UDP datagram to `udp_refused`. A message shorter than its 8-byte
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

/* The most data an echo request carries: an unfragmented datagram of the
 * 1500-byte MTU less the IP and ICMP headers. */
#define ICMP_PING_DATA_MAX 1472u

/** `icmp_ping_data` with no data and the hop limit `IP_TTL_DEFAULT`. */
int icmp_ping(uint32_t dst_ip);

/**
 * Send echo requests to `dst_ip` and wait for an answer: the round trip in
 * ticks, or -ETIMEDOUT when no request was answered, -EBUSY when another
 * ping is in flight, -EMSGSIZE for a `size` past `ICMP_PING_DATA_MAX`, or
 * -EINTR when a signal ended the wait.
 *
 * Up to four requests go, a second apart, each with its own sequence number,
 * `size` bytes of data, each byte its offset modulo 256 as iputils fills
 * them, and the hop limit `ttl`; only an answer to the latest ends the wait.
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

#endif /* _ICMP_H_ */
