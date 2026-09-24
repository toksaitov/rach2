/**
 * DHCP message building and judging, with no system call in it.
 *
 * The client is two things: the exchange, which needs sockets and a clock, and
 * the framing and option parsing, which need nothing. This is the second half.
 * It is separate because everything it reads comes off the wire, where a
 * malformed message is not an accident that has to be waited for: it is what
 * anything on the segment can send. `tests/host/dhcpmsg_hosttest.c` drives it
 * with the messages a server will not produce on request.
 */
#ifndef DHCPMSG_H
#define DHCPMSG_H

#include <stddef.h>
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

/* The shortest lease this client will act on. Below it the renewal, the
 * rebinding and the expiry fall a few seconds apart at most, and at two
 * seconds or less the renewal and the rebinding are the same moment; a server
 * offering less than this is one to refuse, not to keep up with. */
#define DHCP_MIN_LEASE 10

#define DHCP_DISCOVER 1
#define DHCP_OFFER    2
#define DHCP_REQUEST  3
#define DHCP_ACK      5
#define DHCP_NAK      6
#define DHCP_RELEASE  7

/** What a server said, once a reply has been judged. */
struct dhcp_lease {
    unsigned long address;
    unsigned long netmask;
    unsigned long router;
    unsigned long server;
    unsigned long seconds;
    unsigned long t1; /* the server's own renewal time, 0 when it named none */
    unsigned long t2; /* and its rebinding time */
    int has_seconds;
    unsigned long dns[3];
    int dns_count;
    char domain[64];
};

/** What one message is asking for. */
struct dhcp_request {
    int type;
    unsigned long xid;
    const unsigned char *hwaddr; /* six bytes */
    unsigned long ciaddr;        /* the address in hand, 0 unless renewing */
    unsigned long server;        /* the server identifier option, 0 for none */
    unsigned long requested;     /* the requested address option, 0 for none */
    unsigned long dst;           /* where the packet is addressed */
};

unsigned int dhcp_build(unsigned char *out, unsigned int outlen, const struct dhcp_request *r);
int dhcp_frame_ok(const unsigned char *f, int n, unsigned int *ihl, unsigned int *udplen);
int dhcp_parse(const unsigned char *msg, unsigned int len, unsigned long xid,
               const unsigned char *hwaddr, struct dhcp_lease *l);
int dhcp_lease_usable(const struct dhcp_lease *l, char *err, size_t errlen);
unsigned long dhcp_sane_lease(unsigned long seconds, unsigned long max);
void dhcp_boundaries(const struct dhcp_lease *l, time_t from, time_t *t1, time_t *t2, time_t *end);

#endif /* DHCPMSG_H */
