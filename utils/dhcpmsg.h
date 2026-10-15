/**
 * The DHCP messages of `dhcpc`: building a request, and checking a received
 * frame and the message in it, with no system call.
 *
 * The exchange, which needs sockets and a clock, is in dhcpc.c. This half is
 * apart because everything it reads comes off the wire, where anything on
 * the segment can send a malformed message, and
 * `tests/host/dhcpmsg_hosttest.c` drives it with messages a server does not
 * send on request.
 */
#ifndef DHCPMSG_H
#define DHCPMSG_H

#include <stddef.h>
#include <stdint.h>
#include <time.h>

#define DHCP_SERVER_PORT 67
#define DHCP_CLIENT_PORT 68

/* The fixed part of a BOOTP message, the magic cookie after it, and where the
 * options begin. A DHCP message is a BOOTP message with options. */
#define BOOTP_LEN   236
#define COOKIE_LEN  4
#define OPTIONS_OFF (BOOTP_LEN + COOKIE_LEN)

/* The most this client builds or reads: RFC 2131's smallest legal maximum. */
#define DHCP_MAX 576

/* The shortest lease, in seconds, this client acts on. Below it the renewal,
 * the rebinding and the expiry fall a few seconds apart at most, and at two
 * seconds or less the renewal and the rebinding are the same moment, so a
 * shorter lease is taken as none. */
#define DHCP_MIN_LEASE 10

#define DHCP_DISCOVER 1
#define DHCP_OFFER    2
#define DHCP_REQUEST  3
#define DHCP_ACK      5
#define DHCP_NAK      6
#define DHCP_RELEASE  7

/** The lease a reply gives, as `dhcp_parse` reads it. Addresses are in host
 * byte order and times in seconds, and an option the reply lacks is 0. */
struct dhcp_lease {
    unsigned long address; /* yiaddr, the address given to this client */
    unsigned long netmask;
    unsigned long router; /* the first address of the router option */
    unsigned long server; /* the server identifier */
    unsigned long seconds;
    unsigned long t1; /* the server's own renewal time, 0 when it named none */
    unsigned long t2; /* and its rebinding time */
    int has_seconds;  /* the reply has a four-byte lease time option */
    unsigned long dns[3];
    int dns_count;
    char domain[64]; /* empty when absent or not letters, digits, dots and hyphens */
};

/** The fields of one message that `dhcp_build` makes, addresses in host byte
 * order. */
struct dhcp_request {
    int type;                    /* DHCP_DISCOVER or DHCP_REQUEST */
    uint32_t xid;                /* the transaction id, 32 bits on the wire */
    const unsigned char *hwaddr; /* six bytes */
    unsigned long ciaddr;        /* the address held, 0 before a lease */
    unsigned long server;        /* the server identifier option, 0 for none */
    unsigned long requested;     /* the requested address option, 0 for none */
    unsigned long dst;           /* the IP destination, 0xFFFFFFFF to broadcast */
};

/**
 * Build the message `r` describes into `out`, which has room for `outlen`
 * bytes: an IPv4 header, a UDP header from port 68 to port 67 with its
 * checksum, and a BOOTP message with the magic cookie, the broadcast flag
 * and the options, padded to 300 bytes.
 *
 * The IP and UDP headers are built here because a machine with no address
 * cannot have the IP layer build them. A renewal goes through the IP layer
 * and sends only the BOOTP part, which begins 28 bytes into `out`.
 *
 * Returns the length written, or 0 when `outlen` is less than `DHCP_MAX`.
 */
unsigned int dhcp_build(unsigned char *out, unsigned int outlen, const struct dhcp_request *r);

/**
 * Check the received IPv4 packet `f` of `n` bytes as a DHCP reply to this
 * machine, as the IP layer would and then as UDP: version 4, a header length
 * and a total length that fit in `n`, a header checksum that checks, no
 * fragment, UDP from port 67 to port 68 with a length that holds a BOOTP
 * message and its cookie, and a UDP checksum that checks or is 0, which says
 * the sender computed none.
 *
 * Returns 1 with the IP header's length in `*ihl` and the UDP datagram's in
 * `*udplen`, or 0, which leaves them alone.
 */
int dhcp_frame_ok(const unsigned char *f, int n, unsigned int *ihl, unsigned int *udplen);

/**
 * Check the BOOTP message `msg` of `len` bytes as a reply to the transaction
 * `xid` of the client with the six-byte hardware address `hwaddr`, and read
 * its lease into `l`.
 *
 * Returns the DHCP message type, or 0 when the message is not a well-formed
 * reply to this client: shorter than the fixed part and the cookie, not a
 * BOOTP reply, another transaction or hardware address, no magic cookie, an
 * option that runs past `len`, no end option, or no message type. `l` is
 * overwritten once the fixed fields match, even when 0 is returned.
 */
int dhcp_parse(const unsigned char *msg, unsigned int len, uint32_t xid,
               const unsigned char *hwaddr, struct dhcp_lease *l);

/**
 * Whether the lease `l` can be applied: 0 when its address, netmask and
 * router pass `ipcheck_config`, or -1 with the reason in `err`, cut to
 * `errlen - 1` bytes.
 */
int dhcp_lease_usable(const struct dhcp_lease *l, char *err, size_t errlen);

/**
 * The lease time this client acts on for a lease of `seconds`: `seconds`
 * cut to `max`, the longest lease the caller acts on, or 0 when that is
 * shorter than `DHCP_MIN_LEASE`.
 */
unsigned long dhcp_sane_lease(unsigned long seconds, unsigned long max);

/**
 * The renewal time `*t1`, the rebinding time `*t2` and the expiry `*end` of
 * the lease `l`, counted from `from`, the moment it was requested. The
 * server's own times are used when they are in order and inside the lease,
 * and half and seven eighths of the lease otherwise (RFC 2131).
 * `l->seconds` must be at least `DHCP_MIN_LEASE`, as `dhcp_sane_lease`
 * makes it.
 */
void dhcp_boundaries(const struct dhcp_lease *l, time_t from, time_t *t1, time_t *t2, time_t *end);

#endif /* DHCPMSG_H */
