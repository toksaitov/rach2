/**
 * The stub resolver of the Rach2 network utilities, in resolv.c.
 *
 * It turns a host string into an IPv4 address by a dotted-quad literal, then
 * the hosts file, then a DNS A query to the nameserver the resolver
 * configuration names, as a libc's stub resolver does; the kernel has no
 * name service.
 *
 * Addresses are in host byte order. ping, nc and the dnstest probe link
 * resolv.c. The prototypes use plain C types, so the header includes nothing
 * and cannot collide with a program's own fixed-width typedefs.
 */
#ifndef _NETDB_H_
#define _NETDB_H_

/**
 * Parse the dotted quad `s`, four decimal octets of 0 to 255 joined by dots
 * and nothing else, into the host-order address `*out_ip`.
 *
 * Returns 0, or -1 when `s` is not such an address. "0.0.0.0" gives 0 with
 * `*out_ip` 0, so the return, and not the address, says whether `s` parsed.
 */
int rach2_inet_aton(const char *s, unsigned int *out_ip);

/**
 * Look up `name` in the hosts-format text `buf` of `len` bytes: lines of
 * "address name [alias ...]", with '#' starting a comment.
 *
 * Names are compared without regard to case, and a line whose first word is
 * not a dotted quad is skipped. Returns 0 with the address of the first line
 * that has the name in `*out_ip`, or -1 when no line has it.
 */
int hosts_find(const char *buf, int len, const char *name, unsigned int *out_ip);

/**
 * Build a DNS query (RFC 1035) for the A record of `name`, with the
 * transaction id `id`, into `buf`, which has room for `cap` bytes: the 12-byte
 * header with RD set and one question, the name as length-prefixed labels,
 * and QTYPE A and QCLASS IN.
 *
 * Returns the length of the query, or -1 for an empty name, a label that is
 * empty or longer than 63 bytes, or a query that would not fit.
 */
int dns_build_query(unsigned char *buf, int cap, const char *name, unsigned short id);

/**
 * Read the first A record of the DNS response `resp`, of `len` bytes, to the
 * query `id`.
 *
 * Returns 0 with the record's address, in host order, in `*out_ip`; or -1
 * for another id, a message that is not a response, a nonzero RCODE, no
 * answer, no IN A record with four bytes of data, or a message that ends
 * before the record. The input comes from the network, so every field is
 * checked against `len`.
 */
int dns_parse_a(const unsigned char *resp, int len, unsigned short id, unsigned int *out_ip);

/**
 * Resolve `host` to a host-order IPv4 address in `*out_ip`.
 *
 * A dotted quad is its own address. Otherwise the first 1024 bytes of the
 * hosts file are searched, and then the first nameserver with a valid
 * address in the first 256 bytes of the resolver configuration is queried,
 * or SLIRP's forwarder, 10.0.2.3, when there is none. The files are
 * `/etc/hosts` and `/etc/resolv.conf`, or `/hosts` and `/resolv.conf` where
 * the one under `/etc` does not exist. Returns 0, or -1 when no source gives
 * an address; an unanswered query takes about six seconds to fail.
 */
int resolve_host(const char *host, unsigned int *out_ip);

#endif /* _NETDB_H_ */
