/*
 * packet_hosttest.c: the receive parsers against frames no peer would send.
 *
 * host-deps: rach2/sys/lib/kstring.c rach2/sys/lib/kctypes.c
 *
 * A machine on a real network is handed whatever arrives, and what arrives is
 * not always well formed: a header cut short by a bad cable, a length field
 * that claims more than the frame holds, an options field that runs past the
 * end, a fragment. The kernel's answer to all of those is to drop the frame,
 * and the way to be sure it does is to hand it one and see that it neither
 * reads past the buffer nor passes the frame on.
 *
 * That cannot be provoked from inside a guest: QEMU's network will not send a
 * malformed frame, and a probe cannot make one arrive. So `ethernet.c` and
 * `ip.c` are compiled natively with the layers under them replaced by
 * recorders, the frames are built here, and the whole thing runs under
 * AddressSanitizer, which is what turns a read past the end of a frame from a
 * value nobody notices into a failure.
 *
 * Every frame is placed at the end of its own allocation, so a read one byte
 * past the length is a read past the allocation and the sanitizer says so.
 * That is the point of the test: the checks below say what was delivered, and
 * the sanitizer says what was touched.
 */

extern int printf(const char *fmt, ...);
extern void *malloc(unsigned long n);
extern void free(void *p);
extern void *memcpy(void *d, const void *s, unsigned long n);

#include "../../rach2/sys/network/ethernet/ethernet.h"
#include "../../rach2/sys/network/ip/ip.h"

/* ---- what the layers under the parsers record ---- */

static unsigned to_arp, to_icmp, to_udp, to_tcp, to_raw, to_packet;
static unsigned up = 1;

uint8_t net_mac[6] = {0x52, 0x54, 0x00, 0x12, 0x34, 0x56};
uint32_t net_ip_addr = 0x0A00020Fu; /* 10.0.2.15 */
uint32_t net_ip_mask = 0xFFFFFF00u;

int netif_is_up(void)
{
    return (int)up;
}

void arp_rx(const uint8_t *data, uint16_t len)
{
    (void)data;
    (void)len;
    to_arp++;
}

void packet_rx(uint16_t etype, const uint8_t *data, uint16_t len)
{
    (void)etype;
    (void)data;
    (void)len;
    to_packet++;
}

void icmp_rx(uint32_t src, const uint8_t *data, uint16_t len)
{
    (void)src;
    (void)data;
    (void)len;
    to_icmp++;
}

void udp_rx(uint32_t src, const uint8_t *data, uint16_t len)
{
    (void)src;
    (void)data;
    (void)len;
    to_udp++;
}

void tcp_rx(uint32_t src, uint32_t dst, const uint8_t *data, uint16_t len)
{
    (void)src;
    (void)dst;
    (void)data;
    (void)len;
    to_tcp++;
}

void raw_rx(uint8_t proto, const uint8_t *data, uint16_t len)
{
    (void)proto;
    (void)data;
    (void)len;
    to_raw++;
}

/* The rest of what the two files reach for. None of it is exercised here;
   `ethernet_tx` is `ethernet.c`'s own and is compiled in below. */
int arp_lookup(uint32_t ip, uint8_t *mac)
{
    (void)ip;
    (void)mac;
    return -1;
}

void arp_request(uint32_t ip)
{
    (void)ip;
}

uint32_t net_ip_gateway = 0x0A000202u;

uint32_t netconf_broadcast(void)
{
    return (net_ip_addr & net_ip_mask) | ~net_ip_mask;
}

/* The four byte-order helpers and the counters, which live in net_core.c with
   the boot report and its whole dependency chain; they are three lines each
   and are written out here rather than dragging that in. */
uint16_t read16be(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

uint32_t read32be(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

void write16be(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)(v & 0xFF);
}

void write32be(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

/* The interrupt bracket, which on this host is a counter: what the parsers
   need from it is that it balances, not that it disables anything. */
static unsigned irq_depth;

unsigned int kirq_save(void)
{
    irq_depth++;
    return 0;
}

void kirq_restore(unsigned int flags)
{
    (void)flags;
    irq_depth--;
}

int netconf_is_broadcast(uint32_t ip)
{
    return ip == netconf_broadcast() || ip == 0xFFFFFFFFu;
}

/* The interface the stack sends through. This test never looks inside one,
 * so a record of its own is all that is needed. */
static struct netdev the_iface;

struct netdev *netdev_first(void)
{
    return &the_iface;
}

int netdev_tx(struct netdev *nd, const uint8_t *frame, uint16_t len)
{
    (void)frame;
    (void)len;
    return 0;
}

/* The records these two files raise are not what is under test here. */
void klog(int level, const char *subsystem, const char *format, ...)
{
    (void)level;
    (void)subsystem;
    (void)format;
}

#include "../../rach2/sys/network/ethernet/ethernet.c"
#include "../../rach2/sys/network/ip/ip.c"

/* ---- the frames ---- */

static int fails;

#define CHECK(what, cond)                 \
    do {                                  \
        if (!(cond)) {                    \
            printf("  FAIL: %s\n", what); \
            fails++;                      \
        }                                 \
    } while (0)

/* Hand `len` bytes to `ethernet_rx` from the end of an allocation of exactly
   that size, so one byte past the frame is one byte past the allocation. */
static void deliver(const uint8_t *bytes, unsigned len)
{
    uint8_t *exact = (uint8_t *)malloc(len ? len : 1);

    memcpy(exact, bytes, len);
    ethernet_rx(exact, (uint16_t)len);
    free(exact);
}

static void reset(void)
{
    to_arp = to_icmp = to_udp = to_tcp = to_raw = to_packet = 0;
}

/* An Ethernet header with an IPv4 payload after it. */
static unsigned build_eth(uint8_t *f, uint16_t etype)
{
    unsigned i;

    for (i = 0; i < 6; i++) {
        f[i] = net_mac[i];
        f[6 + i] = (uint8_t)(0xAA + i);
    }
    f[12] = (uint8_t)(etype >> 8);
    f[13] = (uint8_t)(etype & 0xFF);
    return 14;
}

/* The header checksum, recomputed after a field is changed: every case below
   changes one field and must not be dropped for the checksum as well. */
static void restamp(uint8_t *ip)
{
    uint16_t sum;

    ip[10] = 0;
    ip[11] = 0;
    sum = ip_checksum(ip, (uint16_t)((ip[0] & 0x0F) * 4));
    ip[10] = (uint8_t)(sum >> 8);
    ip[11] = (uint8_t)(sum & 0xFF);
}

/* A well-formed IPv4 header at `f`, `proto` over `payload_len` bytes. */
static unsigned build_ip(uint8_t *f, uint8_t proto, unsigned payload_len)
{
    unsigned i;
    uint16_t total = (uint16_t)(20 + payload_len);

    for (i = 0; i < 20; i++) {
        f[i] = 0;
    }
    f[0] = 0x45;
    f[2] = (uint8_t)(total >> 8);
    f[3] = (uint8_t)(total & 0xFF);
    f[8] = 64;
    f[9] = proto;
    f[12] = 10;
    f[13] = 0;
    f[14] = 2;
    f[15] = 2;
    f[16] = 10;
    f[17] = 0;
    f[18] = 2;
    f[19] = 15;
    f[10] = 0;
    f[11] = 0;
    {
        uint16_t sum = ip_checksum(f, 20);

        f[10] = (uint8_t)(sum >> 8);
        f[11] = (uint8_t)(sum & 0xFF);
    }
    return 20;
}

int main(void)
{
    uint8_t f[256];
    unsigned n;

    /* A whole frame, so the cases below are read against something that does
       arrive. */
    reset();
    n = build_eth(f, 0x0800);
    n += build_ip(f + n, 17, 8);
    deliver(f, n + 8);
    CHECK("a whole UDP datagram reaches the port layer", to_udp == 1);
    CHECK("and the packet sockets", to_packet == 1);
    CHECK("and the raw layer", to_raw == 1);
    CHECK("and nothing else", to_tcp == 0 && to_icmp == 0);

    /* Ethernet: shorter than its own header, and exactly it. */
    reset();
    deliver(f, 13);
    CHECK("a frame shorter than an Ethernet header is dropped", to_packet == 0 && to_udp == 0);
    reset();
    n = build_eth(f, 0x0800);
    deliver(f, n);
    CHECK("a frame that is only an Ethernet header reaches no IP parser",
          to_udp == 0 && to_tcp == 0 && to_icmp == 0 && to_raw == 0);
    /* Packet sockets are offered it, deliberately: they exist to see what the
       IP layer drops, which is every reply a DHCP client is waiting for. */
    CHECK("but the packet sockets are still offered it", to_packet == 1);

    /* An interface that is down receives nothing. */
    reset();
    up = 0;
    n = build_eth(f, 0x0800);
    n += build_ip(f + n, 17, 8);
    deliver(f, n + 8);
    CHECK("an interface that is down delivers nothing", to_udp == 0);
    up = 1;

    /* IPv4: the version, the header length, the total length, the checksum. */
    reset();
    n = build_eth(f, 0x0800);
    build_ip(f + n, 17, 8);
    f[n] = 0x65; /* version 6 */
    restamp(f + n);
    deliver(f, n + 28);
    CHECK("a version that is not 4 is dropped", to_udp == 0);

    reset();
    n = build_eth(f, 0x0800);
    build_ip(f + n, 17, 8);
    f[n] = 0x44; /* header length 4 words, below the minimum */
    restamp(f + n);
    deliver(f, n + 28);
    CHECK("a header length below the minimum is dropped", to_udp == 0);

    reset();
    n = build_eth(f, 0x0800);
    build_ip(f + n, 17, 8);
    f[n] = 0x4F; /* 15 words of header, past the end of this frame */
    f[n + 10] = 0;
    f[n + 11] = 0;
    deliver(f, n + 28);
    CHECK("a header length past the end of the frame is dropped", to_udp == 0);

    reset();
    n = build_eth(f, 0x0800);
    build_ip(f + n, 17, 8);
    f[n + 2] = 0xFF; /* a total length far past the frame */
    f[n + 3] = 0xFF;
    restamp(f + n);
    deliver(f, n + 28);
    CHECK("a total length past the frame is dropped", to_udp == 0);

    reset();
    n = build_eth(f, 0x0800);
    build_ip(f + n, 17, 8);
    f[n + 2] = 0;
    f[n + 3] = 10; /* a total length below the header */
    restamp(f + n);
    deliver(f, n + 28);
    CHECK("a total length below the header is dropped", to_udp == 0);

    reset();
    n = build_eth(f, 0x0800);
    build_ip(f + n, 17, 8);
    f[n + 10] ^= 0xFF; /* a checksum that does not sum */
    deliver(f, n + 28);
    CHECK("a header whose checksum does not sum is dropped", to_udp == 0);

    /* A fragment: this stack does not reassemble, so it must not pass one on
       as though it were whole. */
    reset();
    n = build_eth(f, 0x0800);
    build_ip(f + n, 17, 8);
    f[n + 6] = 0x20; /* more fragments */
    restamp(f + n);
    deliver(f, n + 28);
    CHECK("a fragment is not delivered as a whole datagram", to_udp == 0);

    /* A datagram for another machine. */
    reset();
    n = build_eth(f, 0x0800);
    build_ip(f + n, 17, 8);
    f[n + 19] = 200; /* a destination this machine does not have */
    restamp(f + n);
    deliver(f, n + 28);
    CHECK("a datagram addressed elsewhere is not delivered", to_udp == 0);

    /* Each protocol reaches its own parser, and the raw layer sees them all. */
    reset();
    n = build_eth(f, 0x0800);
    n += build_ip(f + n, 1, 8);
    deliver(f, n + 8);
    CHECK("ICMP reaches the ICMP parser", to_icmp == 1 && to_udp == 0);
    CHECK("and the raw layer sees it", to_raw == 1);
    reset();
    n = build_eth(f, 0x0800);
    n += build_ip(f + n, 6, 8);
    deliver(f, n + 8);
    CHECK("TCP reaches the TCP parser", to_tcp == 1 && to_udp == 0);
    CHECK("and the raw layer sees it too", to_raw == 1);

    /* An EtherType with no parser behind it. */
    reset();
    n = build_eth(f, 0x88B5);
    deliver(f, n + 20);
    CHECK("an unknown ethertype reaches no parser",
          to_udp == 0 && to_tcp == 0 && to_icmp == 0 && to_arp == 0);

    /* ARP, which is the other type the switch above knows. */
    reset();
    n = build_eth(f, 0x0806);
    deliver(f, n + 28);
    CHECK("ARP reaches the ARP parser", to_arp == 1);

    if (fails == 0) {
        printf("packet_hosttest: PASS\n");
    }
    return fails;
}
