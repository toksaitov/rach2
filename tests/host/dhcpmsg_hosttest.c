/**
 * Host test for the DHCP client's message half (utils/dhcpmsg.c).
 *
 * host-deps: utils/dhcpmsg.c utils/ipcheck.c
 *
 * Everything this file judges arrives on a packet socket, where most of the
 * traffic is somebody else's and any of it may be built to hurt the machine
 * reading it. A server will not produce a truncated option or a wrong checksum
 * on request, so the malformed messages are written here, one field wrong at a
 * time, beside the well-formed one they are made from. The frames are built by
 * this file's own checksum routines, not the library's, so two
 * implementations of the same arithmetic meet over every packet.
 *
 * Returns the failed-check count (0 == pass).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dhcpmsg.h"

static int fails;

#define CHECK(cond, what)                          \
    do {                                           \
        if (!(cond)) {                             \
            fails++;                               \
            fprintf(stderr, "  FAIL: %s\n", what); \
        }                                          \
    } while (0)

#define XID 0x12345678UL

/*
 * Every message goes through the library on a heap copy of its exact length,
 * so a read one byte past what the sender sent is an AddressSanitizer report,
 * not a byte of whatever the client had in its buffer. That is what a
 * declared option length longer than the message is: the checks that refuse it
 * cannot be shown to matter on a 576-byte buffer with slack after it.
 */
static int frame_ok_exact(const unsigned char *f, int n, unsigned int *ihl, unsigned int *udplen)
{
    unsigned char *copy = malloc((size_t)(n > 0 ? n : 1));
    int rc;

    memcpy(copy, f, (size_t)(n > 0 ? n : 0));
    rc = dhcp_frame_ok(copy, n, ihl, udplen);
    free(copy);
    return rc;
}

static int parse_exact(const unsigned char *msg, unsigned int len, unsigned long xid,
                       const unsigned char *mac, struct dhcp_lease *l)
{
    unsigned char *copy = malloc(len ? len : 1);
    int rc;

    memcpy(copy, msg, len);
    rc = dhcp_parse(copy, len, xid, mac, l);
    free(copy);
    return rc;
}

static const unsigned char MAC[6] = {0x52, 0x54, 0x00, 0x11, 0x22, 0x33};

/* This file's own one's-complement sum, deliberately not the library's. */
static unsigned int sum16(const unsigned char *p, unsigned int len, unsigned long start)
{
    unsigned long s = start;
    unsigned int i;

    for (i = 0; i + 1 < len; i += 2) {
        s += ((unsigned long)p[i] << 8) | p[i + 1];
    }
    if (i < len) {
        s += (unsigned long)p[i] << 8;
    }
    while (s >> 16) {
        s = (s & 0xffff) + (s >> 16);
    }
    return (unsigned int)(~s & 0xffff);
}

static void w16(unsigned char *p, unsigned int v)
{
    p[0] = (unsigned char)(v >> 8);
    p[1] = (unsigned char)(v & 0xff);
}

static void w32(unsigned char *p, unsigned long v)
{
    p[0] = (unsigned char)((v >> 24) & 0xff);
    p[1] = (unsigned char)((v >> 16) & 0xff);
    p[2] = (unsigned char)((v >> 8) & 0xff);
    p[3] = (unsigned char)(v & 0xff);
}

static unsigned int r16(const unsigned char *p)
{
    return ((unsigned int)p[0] << 8) | p[1];
}

/* Fill in the two checksums of a frame whose other fields are already set. */
static void seal(unsigned char *f, unsigned int total, int udp_checksum)
{
    unsigned long ps;

    w16(f + 10, 0);
    w16(f + 10, sum16(f, 20, 0));
    w16(f + 26, 0);
    if (!udp_checksum) {
        return;
    }
    ps = 0;
    ps += ((unsigned long)f[12] << 8) | f[13];
    ps += ((unsigned long)f[14] << 8) | f[15];
    ps += ((unsigned long)f[16] << 8) | f[17];
    ps += ((unsigned long)f[18] << 8) | f[19];
    ps += 17;
    ps += total - 20;
    w16(f + 26, sum16(f + 20, total - 20, ps));
}

/**
 * Build one server reply into `f` and return its length.
 *
 * `opts` is the option block after the cookie, and it is copied whole, so a
 * caller may hand in a block that is malformed on purpose.
 */
static unsigned int reply(unsigned char *f, unsigned long xid, const unsigned char *mac,
                          unsigned long yiaddr, const unsigned char *opts, unsigned int optlen,
                          int udp_checksum)
{
    unsigned char *bp = f + 28;
    unsigned int total = 28 + OPTIONS_OFF + optlen;

    memset(f, 0, total);
    f[0] = 0x45;
    w16(f + 2, total);
    f[8] = 64;
    f[9] = 17;
    w32(f + 12, 0xC0A85A02UL); /* 192.168.90.2 */
    w32(f + 16, 0xFFFFFFFFUL);
    w16(f + 20, DHCP_SERVER_PORT);
    w16(f + 22, DHCP_CLIENT_PORT);
    w16(f + 24, total - 20);

    bp[0] = 2; /* BOOTREPLY */
    bp[1] = 1;
    bp[2] = 6;
    w32(bp + 4, xid);
    w32(bp + 16, yiaddr);
    memcpy(bp + 28, mac, 6);
    w32(bp + BOOTP_LEN, 0x63825363UL);
    memcpy(bp + OPTIONS_OFF, opts, optlen);
    seal(f, total, udp_checksum);
    return total;
}

/* An offer of 192.168.90.20/24, router .2, one nameserver, a 600 second
 * lease. */
static const unsigned char GOOD_OPTS[] = {
    53, 1,  DHCP_OFFER, 1, 4, 255, 255, 255, 0, 3,   4,   192, 168, 90, 2, 6,   4,   192, 168, 90,
    3,  51, 4,          0, 0, 2,   88,  54,  4, 192, 168, 90,  2,   15, 4, 'l', 'a', 'n', '.', 255};

static void check_build(void)
{
    unsigned char out[DHCP_MAX];
    struct dhcp_request r;
    unsigned int n;
    unsigned int ihl = 0, udplen = 0;
    struct dhcp_lease l;

    memset(&r, 0, sizeof(r));
    r.type = DHCP_DISCOVER;
    r.xid = XID;
    r.hwaddr = MAC;
    r.dst = 0xFFFFFFFFUL;
    n = dhcp_build(out, sizeof(out), &r);
    CHECK(n >= 28 + 300, "a discover is padded to RFC 1542's 300 BOOTP octets");
    CHECK(n <= DHCP_MAX, "a discover fits the buffer");
    CHECK(r16(out + 2) == n, "the IP total length is the frame length");
    CHECK(sum16(out, 20, 0) == 0, "the IP checksum verifies");
    CHECK(out[9] == 17, "the protocol is UDP");
    CHECK(r16(out + 20) == DHCP_CLIENT_PORT && r16(out + 22) == DHCP_SERVER_PORT,
          "the ports are 68 to 67");
    CHECK(r16(out + 24) == n - 20, "the UDP length is the datagram's");
    CHECK(r16(out + 26) != 0, "a zero checksum is sent as all ones");
    CHECK(out[28] == 1, "the message is a BOOTREQUEST");
    CHECK(memcmp(out + 28 + 28, MAC, 6) == 0, "the hardware address is in chaddr");
    CHECK(out[28 + 10] == 0x80, "the broadcast flag is set");
    CHECK(out[28 + OPTIONS_OFF] == 53 && out[28 + OPTIONS_OFF + 2] == DHCP_DISCOVER,
          "the first option is the message type");

    /* The same bytes, judged by the reader that will see them on the wire. */
    CHECK(frame_ok_exact(out, (int)n, &ihl, &udplen) == 0,
          "a request is not a reply and the reader drops it");

    n = dhcp_build(out, DHCP_MAX - 1, &r);
    CHECK(n == 0, "a buffer smaller than the maximum message is refused");

    /* A UDP checksum of zero means "not computed", so a message whose checksum
       comes out zero must be sent as all ones instead. It comes out zero for
       one transaction id in about 65536, which is a machine that cannot be
       reached one boot in 65536: found here by trying them until one does. */
    {
        unsigned long w;
        int found = 0;

        for (w = 0; w < 0x10000UL && !found; w++) {
            unsigned long ps;

            memset(&r, 0, sizeof(r));
            r.type = DHCP_DISCOVER;
            r.xid = w << 16;
            r.hwaddr = MAC;
            r.dst = 0xFFFFFFFFUL;
            n = dhcp_build(out, sizeof(out), &r);
            w16(out + 26, 0);
            ps = 0;
            ps += r16(out + 12);
            ps += r16(out + 14);
            ps += r16(out + 16);
            ps += r16(out + 18);
            ps += 17;
            ps += n - 20;
            if (sum16(out + 20, n - 20, ps) != 0) {
                continue;
            }
            found = 1;
            n = dhcp_build(out, sizeof(out), &r);
            CHECK(r16(out + 26) == 0xffff, "a checksum that comes out zero is sent as all ones");
        }
        CHECK(found, "a transaction id whose checksum comes out zero was found");
    }

    /* A request has the two options a discover does not. */
    memset(&r, 0, sizeof(r));
    r.type = DHCP_REQUEST;
    r.xid = XID;
    r.hwaddr = MAC;
    r.requested = 0xC0A85A14UL;
    r.server = 0xC0A85A02UL;
    r.dst = 0xFFFFFFFFUL;
    n = dhcp_build(out, sizeof(out), &r);
    CHECK(n > 0, "a request builds");
    {
        const unsigned char *o = out + 28 + OPTIONS_OFF;
        int saw_requested = 0, saw_server = 0;
        unsigned int i;

        for (i = 0; i + 1 < n - 28 - OPTIONS_OFF && o[i] != 255;) {
            if (o[i] == 0) {
                i++;
                continue;
            }
            if (o[i] == 50 && o[i + 1] == 4) {
                saw_requested = (o[i + 2] == 192 && o[i + 5] == 20);
            }
            if (o[i] == 54 && o[i + 1] == 4) {
                saw_server = (o[i + 5] == 2);
            }
            i += 2 + o[i + 1];
        }
        CHECK(saw_requested, "the requested address option is the address asked for");
        CHECK(saw_server, "the server identifier option is the server answered");
    }
    (void)l;
}

static void check_good_reply(void)
{
    unsigned char f[DHCP_MAX];
    unsigned int n, ihl = 0, udplen = 0;
    struct dhcp_lease l;
    int type;

    n = reply(f, XID, MAC, 0xC0A85A14UL, GOOD_OPTS, sizeof(GOOD_OPTS), 1);
    CHECK(frame_ok_exact(f, (int)n, &ihl, &udplen), "a well-formed reply is accepted");
    CHECK(ihl == 20 && udplen == n - 20, "the header lengths come back");
    type = parse_exact(f + ihl + 8, udplen - 8, XID, MAC, &l);
    CHECK(type == DHCP_OFFER, "the message type is the offer's");
    CHECK(l.address == 0xC0A85A14UL, "the offered address is yiaddr");
    CHECK(l.netmask == 0xFFFFFF00UL, "the netmask is option 1");
    CHECK(l.router == 0xC0A85A02UL, "the router is option 3");
    CHECK(l.server == 0xC0A85A02UL, "the server identifier is option 54");
    CHECK(l.seconds == 600 && l.has_seconds, "the lease time is option 51");
    CHECK(l.dns_count == 1 && l.dns[0] == 0xC0A85A03UL, "the nameserver is option 6");
    CHECK(strcmp(l.domain, "lan.") == 0, "the domain is option 15");
    CHECK(l.t1 == 0 && l.t2 == 0, "a server that named no timers leaves them unset");

    /* IPv4 allows a UDP checksum of zero, and SLIRP sends one. */
    n = reply(f, XID, MAC, 0xC0A85A14UL, GOOD_OPTS, sizeof(GOOD_OPTS), 0);
    CHECK(frame_ok_exact(f, (int)n, &ihl, &udplen), "a zero UDP checksum is allowed");
}

static void check_frame_refusals(void)
{
    unsigned char f[DHCP_MAX];
    unsigned int n, ihl, udplen;

#define REBUILD()     (n = reply(f, XID, MAC, 0xC0A85A14UL, GOOD_OPTS, sizeof(GOOD_OPTS), 1))
#define REFUSED(what) CHECK(!frame_ok_exact(f, (int)n, &ihl, &udplen), what)

    REBUILD();
    CHECK(!frame_ok_exact(f, 27, &ihl, &udplen), "a frame shorter than the headers");
    CHECK(!frame_ok_exact(f, 3, &ihl, &udplen), "a frame of three bytes");
    CHECK(!frame_ok_exact(f, 0, &ihl, &udplen), "a frame of nothing");
    REBUILD();
    f[0] = 0x4f;
    seal(f, n, 1);
    REFUSED("a header length past the frame");
    REBUILD();
    f[0] = 0x65;
    seal(f, n, 1);
    REFUSED("a version that is not 4");
    REBUILD();
    f[0] = 0x43;
    seal(f, n, 1);
    REFUSED("a header length below the minimum");
    REBUILD();
    w16(f + 2, n + 1);
    seal(f, n, 1);
    REFUSED("a total length past the bytes received");
    REBUILD();
    w16(f + 6, 0x0021);
    seal(f, n, 1);
    REFUSED("a fragment");
    REBUILD();
    f[10] ^= 0x01;
    REFUSED("an IP checksum that does not verify");
    REBUILD();
    f[9] = 6;
    seal(f, n, 1);
    REFUSED("a protocol that is not UDP");
    REBUILD();
    w16(f + 20, 68);
    seal(f, n, 1);
    REFUSED("a source port that is not the server's");
    REBUILD();
    w16(f + 22, 67);
    seal(f, n, 1);
    REFUSED("a destination port that is not the client's");
    REBUILD();
    w16(f + 24, 8 + OPTIONS_OFF - 1);
    seal(f, n, 1);
    REFUSED("a UDP length too short to hold a BOOTP message");
    REBUILD();
    w16(f + 24, n - 20 + 2);
    seal(f, n, 1);
    REFUSED("a UDP length past the IP total length");
    REBUILD();
    f[26] ^= 0x01;
    if (r16(f + 26) == 0) {
        f[27] ^= 0x02;
    }
    REFUSED("a UDP checksum that does not verify");

#undef REBUILD
#undef REFUSED
}

static void check_parse_refusals(void)
{
    unsigned char f[DHCP_MAX];
    unsigned char opts[64];
    unsigned int n, ihl = 0, udplen = 0;
    struct dhcp_lease l;
    unsigned char other[6];

    n = reply(f, XID + 1, MAC, 0xC0A85A14UL, GOOD_OPTS, sizeof(GOOD_OPTS), 1);
    CHECK(frame_ok_exact(f, (int)n, &ihl, &udplen), "the wrong-xid frame is well formed");
    CHECK(parse_exact(f + ihl + 8, udplen - 8, XID, MAC, &l) == 0,
          "a reply to another machine's transaction is dropped");

    memcpy(other, MAC, 6);
    other[5] ^= 0xff;
    n = reply(f, XID, other, 0xC0A85A14UL, GOOD_OPTS, sizeof(GOOD_OPTS), 1);
    (void)frame_ok_exact(f, (int)n, &ihl, &udplen);
    CHECK(parse_exact(f + ihl + 8, udplen - 8, XID, MAC, &l) == 0,
          "a reply to another machine's hardware address is dropped");

    n = reply(f, XID, MAC, 0xC0A85A14UL, GOOD_OPTS, sizeof(GOOD_OPTS), 1);
    (void)frame_ok_exact(f, (int)n, &ihl, &udplen);
    f[ihl + 8] = 1; /* BOOTREQUEST */
    CHECK(parse_exact(f + ihl + 8, udplen - 8, XID, MAC, &l) == 0,
          "a request read off the wire is not an answer");

    n = reply(f, XID, MAC, 0xC0A85A14UL, GOOD_OPTS, sizeof(GOOD_OPTS), 1);
    (void)frame_ok_exact(f, (int)n, &ihl, &udplen);
    f[ihl + 8 + BOOTP_LEN] ^= 0xff;
    CHECK(parse_exact(f + ihl + 8, udplen - 8, XID, MAC, &l) == 0,
          "a message with no magic cookie is not DHCP");

    n = reply(f, XID, MAC, 0xC0A85A14UL, GOOD_OPTS, sizeof(GOOD_OPTS), 1);
    (void)frame_ok_exact(f, (int)n, &ihl, &udplen);
    CHECK(parse_exact(f + ihl + 8, OPTIONS_OFF - 1, XID, MAC, &l) == 0,
          "a message shorter than the fixed part is dropped");

    /* An option whose length runs past the message. */
    memset(opts, 0, sizeof(opts));
    opts[0] = 53;
    opts[1] = 1;
    opts[2] = DHCP_OFFER;
    opts[3] = 1;
    opts[4] = 40; /* a netmask forty bytes long, in a block of eight */
    opts[5] = 255;
    n = reply(f, XID, MAC, 0xC0A85A14UL, opts, 8, 1);
    (void)frame_ok_exact(f, (int)n, &ihl, &udplen);
    CHECK(parse_exact(f + ihl + 8, udplen - 8, XID, MAC, &l) == 0,
          "an option that claims more than the message holds is dropped");

    /* A code with no length byte after it. */
    memset(opts, 0, sizeof(opts));
    opts[0] = 53;
    opts[1] = 1;
    opts[2] = DHCP_OFFER;
    opts[3] = 1;
    n = reply(f, XID, MAC, 0xC0A85A14UL, opts, 4, 1);
    (void)frame_ok_exact(f, (int)n, &ihl, &udplen);
    CHECK(parse_exact(f + ihl + 8, udplen - 8, XID, MAC, &l) == 0,
          "an option code with no length is dropped");

    /* Well formed but with no end marker: the message was cut short. */
    memset(opts, 0, sizeof(opts));
    opts[0] = 53;
    opts[1] = 1;
    opts[2] = DHCP_ACK;
    opts[3] = 51;
    opts[4] = 4;
    opts[8] = 60;
    n = reply(f, XID, MAC, 0xC0A85A14UL, opts, 9, 1);
    (void)frame_ok_exact(f, (int)n, &ihl, &udplen);
    CHECK(parse_exact(f + ihl + 8, udplen - 8, XID, MAC, &l) == 0,
          "a message with no end marker is dropped");
}

static void check_hostile_options(void)
{
    unsigned char f[DHCP_MAX];
    unsigned char opts[256];
    unsigned int n, ihl = 0, udplen = 0;
    unsigned int i;
    struct dhcp_lease l;

    /* A domain with a newline in it would be a second line in resolv.conf,
       and the second line is what the resolver would use first. */
    memset(opts, 0, sizeof(opts));
    i = 0;
    opts[i++] = 53;
    opts[i++] = 1;
    opts[i++] = DHCP_ACK;
    opts[i++] = 15;
    opts[i++] = 21;
    memcpy(opts + i, "lan\nnameserver 6.6.6", 20);
    i += 21;
    opts[i++] = 51;
    opts[i++] = 4;
    opts[i++] = 0;
    opts[i++] = 0;
    opts[i++] = 1;
    opts[i++] = 44;
    opts[i++] = 255;
    n = reply(f, XID, MAC, 0xC0A85A14UL, opts, i, 1);
    (void)frame_ok_exact(f, (int)n, &ihl, &udplen);
    CHECK(parse_exact(f + ihl + 8, udplen - 8, XID, MAC, &l) == DHCP_ACK,
          "the message itself is still an acknowledgement");
    CHECK(l.domain[0] == '\0', "a domain with anything but a hostname in it is dropped");

    /* More nameservers than the lease can hold, and a fifth that is a byte
       short of an address. */
    memset(opts, 0, sizeof(opts));
    i = 0;
    opts[i++] = 53;
    opts[i++] = 1;
    opts[i++] = DHCP_ACK;
    opts[i++] = 6;
    opts[i++] = 19;
    for (n = 0; n < 19; n++) {
        opts[i++] = (unsigned char)(n + 1);
    }
    opts[i++] = 255;
    n = reply(f, XID, MAC, 0xC0A85A14UL, opts, i, 1);
    (void)frame_ok_exact(f, (int)n, &ihl, &udplen);
    CHECK(parse_exact(f + ihl + 8, udplen - 8, XID, MAC, &l) == DHCP_ACK,
          "an over-long nameserver list does not spoil the message");
    CHECK(l.dns_count == 3, "no more nameservers are kept than there is room for");

    /* A domain name longer than the field it is kept in. */
    memset(opts, 0, sizeof(opts));
    i = 0;
    opts[i++] = 53;
    opts[i++] = 1;
    opts[i++] = DHCP_ACK;
    opts[i++] = 15;
    opts[i++] = 100;
    for (n = 0; n < 100; n++) {
        opts[i++] = 'a';
    }
    opts[i++] = 255;
    n = reply(f, XID, MAC, 0xC0A85A14UL, opts, i, 1);
    (void)frame_ok_exact(f, (int)n, &ihl, &udplen);
    CHECK(parse_exact(f + ihl + 8, udplen - 8, XID, MAC, &l) == DHCP_ACK,
          "an over-long domain does not spoil the message");
    CHECK(strlen(l.domain) == sizeof(l.domain) - 1, "the domain is cut to the field");

    /* A pad byte between options, and an option this client does not know. */
    memset(opts, 0, sizeof(opts));
    i = 0;
    opts[i++] = 0;
    opts[i++] = 53;
    opts[i++] = 1;
    opts[i++] = DHCP_NAK;
    opts[i++] = 200;
    opts[i++] = 2;
    opts[i++] = 9;
    opts[i++] = 9;
    opts[i++] = 255;
    n = reply(f, XID, MAC, 0, opts, i, 1);
    (void)frame_ok_exact(f, (int)n, &ihl, &udplen);
    CHECK(parse_exact(f + ihl + 8, udplen - 8, XID, MAC, &l) == DHCP_NAK,
          "padding and unknown options are stepped over");
}

/* A lease is judged whole before it is applied, by the same rule a static
   stanza is judged by: applying one that the kernel then refuses half way
   through leaves the machine with neither the address it had nor the one it
   was given. */
static void check_lease_usable(void)
{
    struct dhcp_lease l;
    char why[96];

    memset(&l, 0, sizeof(l));
    l.address = 0xC0A85A14UL; /* 192.168.90.20 */
    l.netmask = 0xFFFFFF00UL;
    l.router = 0xC0A85A02UL;
    CHECK(dhcp_lease_usable(&l, why, sizeof(why)) == 0, "an ordinary lease is usable");

    l.router = 0;
    CHECK(dhcp_lease_usable(&l, why, sizeof(why)) == 0, "a lease with no router is usable");

    l.netmask = 0;
    CHECK(dhcp_lease_usable(&l, why, sizeof(why)) == 0,
          "a lease with no netmask is judged under the mask the kernel will derive");

    /* 10.1.0.0 is a host under the /8 the kernel derives for a class A address
       and the network address under any shorter mask, so this says which mask
       the judgement used. */
    l.address = 0x0A010000UL;
    l.netmask = 0;
    l.router = 0;
    CHECK(dhcp_lease_usable(&l, why, sizeof(why)) == 0,
          "a maskless class A lease is judged under the eight-bit mask");
    l.address = 0xC0A85A14UL;
    l.netmask = 0xFFFFFF00UL;
    l.router = 0xC0A85A02UL;

    l.netmask = 0xFF00FF00UL;
    CHECK(dhcp_lease_usable(&l, why, sizeof(why)) != 0,
          "a netmask that is not a run of ones is refused before it is applied");

    l.netmask = 0xFFFFFF00UL;
    l.address = 0xC0A85A00UL;
    CHECK(dhcp_lease_usable(&l, why, sizeof(why)) != 0,
          "an offer of the subnet's own address is refused");

    l.address = 0xC0A85AFFUL;
    CHECK(dhcp_lease_usable(&l, why, sizeof(why)) != 0,
          "an offer of the subnet's broadcast address is refused");

    l.address = 0x7F000002UL;
    CHECK(dhcp_lease_usable(&l, why, sizeof(why)) != 0, "an offer of a loopback address");

    l.address = 0xC0A85A14UL;
    l.router = 0x0A000001UL;
    CHECK(dhcp_lease_usable(&l, why, sizeof(why)) != 0, "a router on another subnet");

    l.router = 0x7F000001UL;
    CHECK(dhcp_lease_usable(&l, why, sizeof(why)) != 0, "a loopback router");

    /* Wide enough and the loopback router is on the subnet, so only the rule
       about what a machine can hold refuses it. */
    l.address = 0x0A000005UL;
    l.netmask = 0x80000000UL;
    l.router = 0x7F000001UL;
    CHECK(dhcp_lease_usable(&l, why, sizeof(why)) != 0 &&
              strstr(why, "not an address a machine can hold") != NULL,
          "a loopback router on the same subnet is still refused");

    memset(&l, 0, sizeof(l));
    CHECK(dhcp_lease_usable(&l, why, sizeof(why)) != 0 && strstr(why, "no address") != NULL,
          "and a lease with no address, refused for being none, not for its class");
}

static void check_lease_arithmetic(void)
{
    struct dhcp_lease l;
    time_t t1, t2, end;

    CHECK(dhcp_sane_lease(0, 1000) == 0, "a lease of no seconds stays none");
    CHECK(dhcp_sane_lease(999, 1000) == 999, "a lease under the cap is untouched");
    CHECK(dhcp_sane_lease(0xFFFFFFFFUL, 1000) == 1000, "the infinite lease is capped");
    CHECK(dhcp_sane_lease(DHCP_MIN_LEASE - 1, 1000) == 0,
          "a lease too short to renew inside is no lease");
    CHECK(dhcp_sane_lease(DHCP_MIN_LEASE, 1000) == DHCP_MIN_LEASE,
          "the shortest lease this acts on is acted on");
    CHECK(dhcp_sane_lease(1000, DHCP_MIN_LEASE - 1) == 0,
          "a cap below the minimum leaves no lease, not a short one");

    /* Seven eighths computed the other way around is zero for every lease
       under eight seconds, which would rebind the instant it was configured. */
    memset(&l, 0, sizeof(l));
    l.seconds = 10;
    dhcp_boundaries(&l, 0, &t1, &t2, &end);
    CHECK(t1 == 5 && t2 == 8 && end == 10,
          "a ten second lease renews at five and rebinds at eight");
    l.seconds = DHCP_MIN_LEASE;
    dhcp_boundaries(&l, 0, &t1, &t2, &end);
    CHECK(t1 > 0 && t2 > t1 && end > t2, "the shortest lease has boundaries in order");

    memset(&l, 0, sizeof(l));
    l.seconds = 800;
    dhcp_boundaries(&l, 100, &t1, &t2, &end);
    CHECK(t1 == 500, "with no timers named, renewal is at half the lease");
    CHECK(t2 == 800, "with no timers named, rebinding is at seven eighths");
    CHECK(end == 900, "the lease ends where it was asked for plus its length");

    l.t1 = 100;
    l.t2 = 700;
    dhcp_boundaries(&l, 0, &t1, &t2, &end);
    CHECK(t1 == 100 && t2 == 700, "the server's own timers are taken");

    l.t1 = 700;
    l.t2 = 100;
    dhcp_boundaries(&l, 0, &t1, &t2, &end);
    CHECK(t1 == 400 && t2 == 700, "timers out of order are replaced by the defaults");

    l.t1 = 100;
    l.t2 = 900;
    dhcp_boundaries(&l, 0, &t1, &t2, &end);
    CHECK(t1 == 400 && t2 == 700, "a rebinding past the lease is replaced");

    l.t1 = 0;
    l.t2 = 700;
    dhcp_boundaries(&l, 0, &t1, &t2, &end);
    CHECK(t1 == 400 && t2 == 700, "a named rebinding stands beside the default renewal");

    l.t1 = 100;
    l.t2 = 0;
    dhcp_boundaries(&l, 0, &t1, &t2, &end);
    CHECK(t1 == 100 && t2 == 700, "a named renewal stands beside the default rebinding");
}

int main(void)
{
    check_build();
    check_good_reply();
    check_frame_refusals();
    check_parse_refusals();
    check_hostile_options();
    check_lease_usable();
    check_lease_arithmetic();
    return fails > 120 ? 120 : fails;
}
