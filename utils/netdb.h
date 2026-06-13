/*
 * netdb.h - name resolution for the Rach2 network utilities.
 *
 * A small userspace stub resolver: turn a host string into an IPv4 address by
 * trying, in order, a numeric dotted-quad literal, the /hosts file, then a DNS
 * "A" query to the nameserver listed in /resolv.conf. This mirrors how a libc
 * stub resolver works -- the kernel stays transport-only and naming lives here.
 * A future musl port would replace resolve_host() with getaddrinfo() and read
 * the same /resolv.conf and /hosts, so callers and config files do not change.
 *
 * All addresses are host byte order. These functions are implemented in
 * resolv.c, which is linked into each program that needs them (ping, nc, and the
 * dnstest test). Prototypes use plain C types so the header stays self-contained
 * and never collides with a program's own fixed-width typedefs.
 */
#ifndef _NETDB_H_
#define _NETDB_H_

/*
 * Parse a dotted-quad "a.b.c.d" into a host-order IPv4 address. Returns 0 and
 * stores the address in *out_ip on success; -1 if the string is not a valid
 * numeric address. ("0.0.0.0" succeeds with *out_ip == 0, so a 0 return is the
 * only success signal -- callers must not treat a zero address as an error.)
 */
int inet_aton(const char *s, unsigned int *out_ip);

/*
 * Look up name in a /hosts-format buffer: lines of "ip name [alias...]", with
 * '#' starting a comment. Returns 0 and stores the first matching address in
 * *out_ip; -1 if not found. Name matching is case-insensitive.
 */
int hosts_find(const char *buf, int len, const char *name, unsigned int *out_ip);

/*
 * Build a DNS query for the A record of name into buf (capacity cap bytes):
 * a 12-byte header (id, RD set, one question) followed by the QNAME labels and
 * QTYPE=A / QCLASS=IN. Returns the query length in bytes, or -1 on error (name
 * too long, empty label, or buffer too small).
 */
int dns_build_query(unsigned char *buf, int cap, const char *name,
                    unsigned short id);

/*
 * Parse a DNS response in resp[0..len) that answers query id. Returns 0 and
 * stores the first A record's host-order address in *out_ip on success; -1 on
 * any error (id mismatch, not a response, non-zero RCODE, no A record, or a
 * malformed/truncated message). Untrusted input: every field is bounds-checked.
 */
int dns_parse_a(const unsigned char *resp, int len, unsigned short id,
                unsigned int *out_ip);

/*
 * Resolve host (numeric literal, then /hosts, then DNS) to a host-order IPv4.
 * Returns 0 and stores the address in *out_ip; -1 on failure.
 */
int resolve_host(const char *host, unsigned int *out_ip);

#endif /* _NETDB_H_ */
