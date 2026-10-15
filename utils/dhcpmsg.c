/**
 * The DHCP messages of `dhcpc`. See dhcpmsg.h for what is here and why.
 */

#include <string.h>

#include "dhcpmsg.h"
#include "ipcheck.h"

#define OP_REQUEST 1
#define OP_REPLY   2

/* The hardware type BOOTP gives Ethernet, from the ARP table. */
#define ARPHRD_ETHER 1

#define OPT_SUBNET    1
#define OPT_ROUTER    3
#define OPT_DNS       6
#define OPT_DOMAIN    15
#define OPT_REQUESTED 50
#define OPT_LEASE     51
#define OPT_T1        58
#define OPT_T2        59
#define OPT_MSGTYPE   53
#define OPT_SERVERID  54
#define OPT_PARAMS    55
#define OPT_END       255

/** Read a big-endian 16-bit field. */
static unsigned int be16(const unsigned char *p)
{
    return ((unsigned int)p[0] << 8) | (unsigned int)p[1];
}

/** Read a big-endian 32-bit field as a host-order number. */
static unsigned long be32(const unsigned char *p)
{
    return ((unsigned long)p[0] << 24) | ((unsigned long)p[1] << 16) | ((unsigned long)p[2] << 8) |
           (unsigned long)p[3];
}

/** Write the low 16 bits of `v` at `p`, big-endian. */
static void put_be16(unsigned char *p, unsigned int v)
{
    p[0] = (unsigned char)((v >> 8) & 0xff);
    p[1] = (unsigned char)(v & 0xff);
}

/** Write the low 32 bits of `v` at `p`, big-endian. */
static void put_be32(unsigned char *p, unsigned long v)
{
    p[0] = (unsigned char)((v >> 24) & 0xff);
    p[1] = (unsigned char)((v >> 16) & 0xff);
    p[2] = (unsigned char)((v >> 8) & 0xff);
    p[3] = (unsigned char)(v & 0xff);
}

/**
 * The one's-complement sum of the `len` bytes at `p`, as big-endian 16-bit
 * words with a last odd byte padded by a zero, added to `start` and not yet
 * folded. Every IP checksum is made of it; `start` lets a UDP checksum add
 * its pseudo-header before its own bytes.
 */
static unsigned long ones_sum(const unsigned char *p, unsigned int len, unsigned long start)
{
    unsigned long sum = start;
    unsigned int i;

    for (i = 0; i + 1 < len; i += 2) {
        sum += ((unsigned long)p[i] << 8) | p[i + 1];
    }
    if (i < len) {
        sum += (unsigned long)p[i] << 8;
    }
    return sum;
}

/** Fold a one's-complement sum and take its complement. */
static unsigned int fold(unsigned long sum)
{
    while (sum >> 16) {
        sum = (sum & 0xffff) + (sum >> 16);
    }
    return (unsigned int)(~sum & 0xffff);
}

/**
 * The IP source is `r->ciaddr`, 0 before a lease. The UDP checksum covers a
 * pseudo-header of the two addresses, the protocol and the length, then the
 * datagram. The BOOTP part is padded to the 300 bytes of RFC 1542: a shorter
 * message is legal DHCP, and some servers and relays drop it.
 */
unsigned int dhcp_build(unsigned char *out, unsigned int outlen, const struct dhcp_request *r)
{
    unsigned char *ip = out;
    unsigned char *udp = out + 20;
    unsigned char *bp = out + 28;
    unsigned int optlen = 0;
    unsigned char *o = bp + OPTIONS_OFF;
    unsigned int total;
    unsigned long sum;

    if (outlen < DHCP_MAX) {
        return 0;
    }
    memset(out, 0, DHCP_MAX);

    bp[0] = OP_REQUEST;
    bp[1] = ARPHRD_ETHER;
    bp[2] = 6;
    put_be32(bp + 4, r->xid);
    put_be16(bp + 10, 0x8000); /* the broadcast flag, for a machine with no address yet */
    put_be32(bp + 12, r->ciaddr);
    memcpy(bp + 28, r->hwaddr, 6);
    put_be32(bp + BOOTP_LEN, 0x63825363UL);

    o[optlen++] = OPT_MSGTYPE;
    o[optlen++] = 1;
    o[optlen++] = (unsigned char)r->type;
    if (r->requested) {
        o[optlen++] = OPT_REQUESTED;
        o[optlen++] = 4;
        put_be32(o + optlen, r->requested);
        optlen += 4;
    }
    if (r->server) {
        o[optlen++] = OPT_SERVERID;
        o[optlen++] = 4;
        put_be32(o + optlen, r->server);
        optlen += 4;
    }
    o[optlen++] = OPT_PARAMS;
    o[optlen++] = 4;
    o[optlen++] = OPT_SUBNET;
    o[optlen++] = OPT_ROUTER;
    o[optlen++] = OPT_DNS;
    o[optlen++] = OPT_DOMAIN;
    o[optlen++] = OPT_END;

    while (OPTIONS_OFF + optlen < 300) {
        o[optlen++] = 0;
    }

    total = 28 + OPTIONS_OFF + optlen;

    put_be16(udp + 0, DHCP_CLIENT_PORT);
    put_be16(udp + 2, DHCP_SERVER_PORT);
    put_be16(udp + 4, total - 20);

    ip[0] = 0x45;
    put_be16(ip + 2, total);
    ip[8] = 64;
    ip[9] = 17;
    put_be32(ip + 12, r->ciaddr);
    put_be32(ip + 16, r->dst);
    put_be16(ip + 10, fold(ones_sum(ip, 20, 0)));

    sum = ones_sum(ip + 12, 8, 0);
    sum += 17;
    sum += total - 20;
    put_be16(udp + 6, fold(ones_sum(udp, total - 20, sum)));
    if (be16(udp + 6) == 0) {
        put_be16(udp + 6, 0xffff); /* zero means "not computed", so send all ones */
    }
    return total;
}

/**
 * The packet socket gives what the wire carried, before the IP layer has
 * looked at any of it, so everything the IP layer would check is checked
 * here. Without these checks a client would take its address from anything
 * on the wire that looked close enough. A zero UDP checksum, which IPv4
 * allows, says the sender computed none.
 */
int dhcp_frame_ok(const unsigned char *f, int n, unsigned int *ihl, unsigned int *udplen)
{
    unsigned int hlen, total, ulen;
    unsigned long sum;

    if (n < 20 + 8) {
        return 0;
    }
    if ((f[0] >> 4) != 4) {
        return 0;
    }
    hlen = (unsigned int)(f[0] & 0x0f) * 4;
    if (hlen < 20 || (int)hlen > n) {
        return 0;
    }
    total = be16(f + 2);
    if (total < hlen + 8 || (int)total > n) {
        return 0;
    }
    if ((be16(f + 6) & 0x3fff) != 0) {
        return 0; /* a fragment: no DHCP message is one */
    }
    if (fold(ones_sum(f, hlen, 0)) != 0) {
        return 0;
    }
    if (f[9] != 17) {
        return 0;
    }
    if (be16(f + hlen) != DHCP_SERVER_PORT || be16(f + hlen + 2) != DHCP_CLIENT_PORT) {
        return 0;
    }
    ulen = be16(f + hlen + 4);
    if (ulen < 8 + OPTIONS_OFF || hlen + ulen > total) {
        return 0;
    }
    if (be16(f + hlen + 6) != 0) {
        sum = ones_sum(f + 12, 8, 0);
        sum += 17;
        sum += ulen;
        if (fold(ones_sum(f + hlen, ulen, sum)) != 0) {
            return 0;
        }
    }
    *ihl = hlen;
    *udplen = ulen;
    return 1;
}

/**
 * `msg` is the message without the packet that carried it, so the two ways a
 * reply arrives (the packet socket while discovering or rebinding, an
 * ordinary socket for a renewal) are checked by the same code. Every bound is
 * checked against `len`, because most of what the packet socket delivers is
 * other machines' traffic, and some of it may be built to break this parser.
 *
 * A domain name with anything but letters, digits, dots and hyphens in it is
 * dropped. It is written into the resolver as a `search` line, and a newline
 * in it would start a second line: a machine on the wire that answered first
 * could put its own nameserver ahead of the leased ones and see every name
 * this machine looks up.
 */
int dhcp_parse(const unsigned char *msg, unsigned int len, uint32_t xid,
               const unsigned char *hwaddr, struct dhcp_lease *l)
{
    const unsigned char *o;
    unsigned int optlen;
    unsigned int i;
    int ended;
    int type = 0;

    if (len < OPTIONS_OFF) {
        return 0;
    }
    if (msg[0] != OP_REPLY || be32(msg + 4) != xid) {
        return 0;
    }
    if (memcmp(msg + 28, hwaddr, 6) != 0) {
        return 0;
    }
    if (be32(msg + BOOTP_LEN) != 0x63825363UL) {
        return 0;
    }

    memset(l, 0, sizeof(*l));
    l->address = be32(msg + 16);
    o = msg + OPTIONS_OFF;
    optlen = len - OPTIONS_OFF;
    ended = 0;
    for (i = 0; i < optlen;) {
        unsigned int code = o[i];
        unsigned int optsize;

        if (code == OPT_END) {
            ended = 1;
            break;
        }
        if (code == 0) {
            i++;
            continue;
        }
        if (i + 1 >= optlen) {
            return 0;
        }
        optsize = o[i + 1];
        if (i + 2 + optsize > optlen) {
            return 0;
        }
        switch (code) {
        case OPT_MSGTYPE:
            if (optsize >= 1) {
                type = o[i + 2];
            }
            break;
        case OPT_SUBNET:
            if (optsize >= 4) {
                l->netmask = be32(o + i + 2);
            }
            break;
        case OPT_ROUTER:
            if (optsize >= 4) {
                l->router = be32(o + i + 2);
            }
            break;
        case OPT_SERVERID:
            if (optsize >= 4) {
                l->server = be32(o + i + 2);
            }
            break;
        case OPT_LEASE:
            if (optsize == 4) {
                l->seconds = be32(o + i + 2);
                l->has_seconds = 1;
            }
            break;
        case OPT_T1:
            if (optsize == 4) {
                l->t1 = be32(o + i + 2);
            }
            break;
        case OPT_T2:
            if (optsize == 4) {
                l->t2 = be32(o + i + 2);
            }
            break;
        case OPT_DNS: {
            unsigned int k;

            for (k = 0; k + 4 <= optsize && l->dns_count < 3; k += 4) {
                l->dns[l->dns_count++] = be32(o + i + 2 + k);
            }
            break;
        }
        case OPT_DOMAIN: {
            unsigned int take = optsize;
            unsigned int k;

            if (take > sizeof(l->domain) - 1) {
                take = sizeof(l->domain) - 1;
            }
            for (k = 0; k < take; k++) {
                unsigned char c = o[i + 2 + k];

                if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                      c == '.' || c == '-')) {
                    take = 0;
                    break;
                }
            }
            memcpy(l->domain, o + i + 2, take);
            l->domain[take] = '\0';
            break;
        }
        default:
            break;
        }
        i += 2 + optsize;
    }
    if (!ended) {
        return 0;
    }
    return type;
}

/**
 * A server can acknowledge an address with a netmask that is not a run of
 * ones or a router on another network, and a client that applied such a
 * lease would change the address, have the mask fail, and be left half
 * configured. So the lease is checked whole first, by the rule `netup`
 * applies to a static stanza, and `dhcpc` takes an unusable lease as no
 * answer and goes on requesting one.
 */
int dhcp_lease_usable(const struct dhcp_lease *l, char *err, size_t errlen)
{
    return ipcheck_config(l->address, l->netmask, l->router, err, errlen);
}

/**
 * The infinite lease RFC 2131 writes as 0xFFFFFFFF is a number the timers
 * cannot hold: seven eighths of it overflows, and the client would compute a
 * rebinding time before its renewal time and never renew at all. `dhcpc`
 * passes a week as `max`, which is long enough that a machine holding a lease
 * meant to be permanent renews rarely, and short enough that the arithmetic
 * of `dhcp_boundaries` cannot overflow. Its `-l` lowers `max`, which is how
 * the renewal path is exercised on a network that hands out days.
 *
 * Below `DHCP_MIN_LEASE`, `(seconds / 2)` and `(seconds * 7) / 8` fall a few
 * seconds apart at most, and at two seconds or less they are the same moment.
 */
unsigned long dhcp_sane_lease(unsigned long seconds, unsigned long max)
{
    if (seconds > max) {
        seconds = max;
    }
    if (seconds < DHCP_MIN_LEASE) {
        return 0;
    }
    return seconds;
}

/**
 * RFC 2131 makes half and seven eighths of the lease the times only when the
 * server names none. Each defaults on its own, because a server may name one
 * and not the other. The pair is then checked together, and a pair that is
 * out of order or reaches past the lease is replaced by the two defaults.
 * Seven eighths is computed as `(seconds * 7) / 8`, multiplying first,
 * because dividing first turns every lease under eight seconds into zero.
 */
void dhcp_boundaries(const struct dhcp_lease *l, time_t from, time_t *t1, time_t *t2, time_t *end)
{
    unsigned long a = l->t1 ? l->t1 : l->seconds / 2;
    unsigned long b = l->t2 ? l->t2 : (l->seconds * 7) / 8;

    if (!(a > 0 && b > a && b < l->seconds)) {
        a = l->seconds / 2;
        b = (l->seconds * 7) / 8;
    }
    *t1 = from + (time_t)a;
    *t2 = from + (time_t)b;
    *end = from + (time_t)l->seconds;
}
