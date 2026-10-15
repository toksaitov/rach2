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
 *
 * The send rows run `ip_tx` against the same recorders: the MTU on every
 * path, a driver's refusal, the loopback ring filled past its last slot, and
 * the interface a broadcast leaves by on a machine with two cards.
 */

extern int printf(const char *fmt, ...);
extern int dprintf(int fd, const char *fmt, ...);
extern void *malloc(unsigned long n);
extern void free(void *p);
extern void *memcpy(void *d, const void *s, unsigned long n);

#include "../../rach2/sys/network/ethernet/ethernet.h"
#include "../../rach2/sys/network/driver/netdev.h"
#include "../../rach2/sys/network/ip/ip.h"

/* ---- what the layers under the parsers record ---- */

/* The frames or datagrams each layer above was given. */
static unsigned to_arp, to_icmp, to_udp, to_tcp, to_raw, to_packet;

uint8_t net_mac[6] = {0x52, 0x54, 0x00, 0x12, 0x34, 0x56};
uint32_t net_ip_addr = 0x0A00020Fu; /* 10.0.2.15 */
uint32_t net_ip_mask = 0xFFFFFF00u;

/** Count an ARP packet received. */
void arp_rx(struct netdev *nd, const uint8_t *data, uint16_t len)
{
    (void)nd;
    (void)data;
    (void)len;
    to_arp++;
}

/** Count a frame offered to the packet sockets. */
void packet_rx(struct netdev *nd, uint16_t etype, const uint8_t *data, uint16_t len)
{
    (void)nd;
    (void)etype;
    (void)data;
    (void)len;
    to_packet++;
}

/** Count an ICMP message received. */
void icmp_rx(uint32_t src, uint32_t dst, uint8_t ttl, const uint8_t *data, uint16_t len)
{
    (void)ttl;
    (void)dst;
    (void)src;
    (void)data;
    (void)len;
    to_icmp++;
}

/** Send no ICMP destination-unreachable message. */
void icmp_unreachable(const uint8_t *dgram, uint16_t len, uint8_t code)
{
    (void)dgram;
    (void)len;
    (void)code;
}

/* The first two bytes and the length of each UDP payload, in the order they
 * were delivered; the loopback row writes a datagram's number in those
 * bytes. */
#define UDP_SEEN_MAX 256u
static struct {
    uint16_t number, len;
} udp_seen[UDP_SEEN_MAX];

/** Record and count a UDP datagram received; return 0. */
int udp_rx(uint32_t src, uint32_t dst, const uint8_t *data, uint16_t len)
{
    (void)src;
    (void)dst;
    if (to_udp < UDP_SEEN_MAX) {
        udp_seen[to_udp].number = len >= 2 ? (uint16_t)((data[0] << 8) | data[1]) : 0;
        udp_seen[to_udp].len = len;
    }
    to_udp++;
    return 0;
}

/** Count a TCP segment received. */
void tcp_rx(uint32_t src, uint32_t dst, const uint8_t *data, uint16_t len)
{
    (void)src;
    (void)dst;
    (void)data;
    (void)len;
    to_tcp++;
}

/** Count a datagram offered to the raw sockets. */
void raw_rx(uint8_t proto, const uint8_t *data, uint16_t len)
{
    (void)proto;
    (void)data;
    (void)len;
    to_raw++;
}

/* ARP as the send rows see it: every address resolves while `arp_known` is
   set, and each lookup and request is counted. `ethernet_tx` is `ethernet.c`'s
   own and is compiled in below. */
static int arp_known;
static unsigned arp_queries, arp_requests;

/** Count a lookup; while `arp_known` is set, give 10:11:12:13:14:15 for any
 * address and return 1, else return 0. */
int arp_lookup(struct netdev *nd, uint32_t ip, uint8_t *mac)
{
    unsigned i;

    (void)nd;
    (void)ip;
    arp_queries++;
    if (!arp_known) {
        return 0;
    }
    for (i = 0; i < 6; i++) {
        mac[i] = (uint8_t)(0x10 + i);
    }
    return 1;
}

struct arp_stats arp_stats;

/** Count an ARP request sent. */
void arp_request(struct netdev *nd, uint32_t ip)
{
    (void)nd;
    (void)ip;
    arp_requests++;
}

uint32_t net_ip_gateway = 0x0A000202u;

/**
 * Read the big-endian 16-bit field at `p`.
 *
 * This and the three functions after it are the byte-order helpers of
 * net_core.c, which has the stack's bring-up and its whole dependency chain
 * with them; they are written out here rather than bringing that in.
 */
uint16_t read16be(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

/** Read the big-endian 32-bit field at `p`. */
uint32_t read32be(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

/** Store `v` at `p` in network byte order. */
void write16be(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)(v & 0xFF);
}

/** Store `v` at `p` in network byte order. */
void write32be(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

/* The interface the cases run on, with the machine's own address so that a
 * datagram addressed to it is taken, and a second card on another network,
 * present only while `ifaces` is 2; `test_broadcast_iface` fills it in. */
static struct netdev the_iface = {0,
                                  0,
                                  {'e', 't', 'h', '0', 0, 0, 0, 0},
                                  1,
                                  {0, 0, 0, 0, 0, 0},
                                  0x0A00020Fu,
                                  0xFFFFFF00u,
                                  0x0A000202u,
                                  0,
                                  0,
                                  0,
                                  0,
                                  0,
                                  0,
                                  0,
                                  0,
                                  0,
                                  1};
static struct netdev second_iface;
static int ifaces = 1;

static int route_available = 1; /* netdev_route finds a route while set */
static unsigned route_queries;  /* the routes netdev_route was asked for */

/** Return the interface a datagram to `dst` leaves by, with `dst` itself as
 * the next hop in `*via`, or NULL when `route_available` is clear. */
struct netdev *netdev_route(uint32_t dst, uint32_t *via)
{
    route_queries++;
    *via = dst;
    return route_available ? &the_iface : 0;
}

/** Return `nd`'s one address as the source for any next hop. */
uint32_t netdev_source(const struct netdev *nd, uint32_t hop)
{
    (void)hop;
    return nd->ip;
}

/** Report the interfaces present: `ifaces`. */
int netdev_count(void)
{
    return ifaces;
}

/** Return interface `n`, counted from 0, or NULL past the last. */
struct netdev *netdev_at(int n)
{
    if (n == 0) {
        return &the_iface;
    }
    return n == 1 && ifaces == 2 ? &second_iface : 0;
}

/** Report no loopback interface; the loopback ring counts its traffic only
 * when there is one. */
struct netdev *netdev_loopback(void)
{
    return 0;
}

/** Report whether `ip` is the address of an interface present. */
int netdev_owns(uint32_t ip)
{
    int i;

    for (i = 0; i < ifaces; i++) {
        if (ip != 0 && ip == netdev_at(i)->ip) {
            return 1;
        }
    }
    return 0;
}

/** Report the all-ones address and the broadcast address of the subnet of
 * an interface present. */
int netdev_is_broadcast(uint32_t ip)
{
    int i;

    if (ip == 0xFFFFFFFFu) {
        return 1;
    }
    for (i = 0; i < ifaces; i++) {
        const struct netdev *nd = netdev_at(i);

        if (nd->ip != 0 && nd->mask != 0 && ip == (nd->ip | ~nd->mask)) {
            return 1;
        }
    }
    return 0;
}

/** Report the interfaces' broadcast addresses, as `netconf.c` does. */
int netconf_is_broadcast(uint32_t ip)
{
    return netdev_is_broadcast(ip);
}

/** Return the first interface. */
struct netdev *netdev_first(void)
{
    return &the_iface;
}

/* Whether the card says its link is up, as `netdev.c` judges it. */
static int link_down;

/** Report the link up unless `link_down` is set. */
int netdev_running(const struct netdev *nd)
{
    return nd && !link_down;
}

/* What the driver answers a frame with, how many it was handed, and the
 * length, the card and the first bytes of the last one. */
static int tx_answer;
static unsigned tx_frames;
static uint16_t tx_len;
static struct netdev *tx_card;
static uint8_t tx_frame[64];

/** Record a frame handed to the driver and answer it with `tx_answer`. */
int netdev_tx(struct netdev *nd, const uint8_t *frame, uint16_t len)
{
    unsigned i;

    tx_card = nd;
    for (i = 0; i < len && i < sizeof(tx_frame); i++) {
        tx_frame[i] = frame[i];
    }
    tx_len = len;
    tx_frames++;
    return tx_answer;
}

/* The lines the loopback ring's refusal writes, which the ring row counts.
 * The two files' other records are not what is under test. */
static unsigned loop_lines;

/** Count a record of subsystem "ip" whose format begins "loopback:", and
 * discard any other. */
void klog(int level, const char *subsystem, const char *format, ...)
{
    static const char prefix[] = "loopback:";
    unsigned i = 0;

    (void)level;
    while (prefix[i] != 0 && format[i] == prefix[i]) {
        i++;
    }
    if (subsystem[0] == 'i' && subsystem[1] == 'p' && subsystem[2] == 0 && prefix[i] == 0) {
        loop_lines++;
    }
}

#include "../../rach2/sys/network/ethernet/ethernet.c"
#include "../../rach2/sys/network/ip/ip.c"

/* The counters /proc/net/snmp reports, which net_core.c defines. */
struct net_mib net_mib;

/* ---- the frames ---- */

static int fails;

/* A failure goes to the unbuffered standard error, so that a sanitizer that
 * stops the test at a later row still leaves it in the log. */
#define CHECK(what, cond)                     \
    do {                                      \
        if (!(cond)) {                        \
            dprintf(2, "  FAIL: %s\n", what); \
            fails++;                          \
        }                                     \
    } while (0)

/**
 * Hand `len` bytes to `ethernet_rx` from an allocation of exactly that size, so
 * that one byte past the frame is one byte past the allocation.
 */
static void deliver(const uint8_t *bytes, unsigned len)
{
    uint8_t *exact = (uint8_t *)malloc(len ? len : 1);

    memcpy(exact, bytes, len);
    ethernet_rx(netdev_first(), exact, (uint16_t)len);
    free(exact);
}

/** Mark the interface up and zero the counts of the layers above. */
static void reset(void)
{
    the_iface.up = 1;
    to_arp = to_icmp = to_udp = to_tcp = to_raw = to_packet = 0;
}

/**
 * Write at `f` an Ethernet header to this machine's station address, from
 * AA:AB:AC:AD:AE:AF, of EtherType `etype`; return its length, 14.
 */
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

/**
 * Recompute the checksum of the IPv4 header at `ip` after a field is changed:
 * every case changes one field and must not be dropped for the checksum as
 * well.
 */
static void restamp(uint8_t *ip)
{
    uint16_t sum;

    ip[10] = 0;
    ip[11] = 0;
    sum = ip_checksum(ip, (uint16_t)((ip[0] & 0x0F) * 4));
    ip[10] = (uint8_t)(sum >> 8);
    ip[11] = (uint8_t)(sum & 0xFF);
}

/**
 * Write at `f` a well-formed IPv4 header from 10.0.2.2 to this machine,
 * 10.0.2.15, of protocol `proto` over `payload_len` bytes; return its length,
 * 20.
 */
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

/**
 * Check a datagram of `len` bytes of payload that ip_tx took, where path `path`
 * puts it: 0 the loopback ring, 1 an ARP request and no data frame, any other
 * the driver.
 */
static void check_tx_path(unsigned path, uint16_t len)
{
    if (path == 0) {
        CHECK("loopback queues the complete IP datagram",
              loop_head == 1 && loop_ring_len[0] == 20 + len &&
                  read16be(loop_ring[0] + 2) == 20 + len && read16be(loop_ring[0] + 6) == 0x4000 &&
                  ip_checksum(loop_ring[0], 20) == 0 && tx_frames == 0 && arp_queries == 0);
    } else if (path == 1) {
        CHECK("ARP miss requests resolution without a data frame",
              arp_queries == 1 && arp_requests == 1 && tx_frames == 0);
    } else {
        CHECK("wire datagram includes both headers within the MTU",
              tx_frames == 1 && tx_len == 14 + 20 + len && read16be(ip_tx_buf + 2) == 20 + len &&
                  read16be(ip_tx_buf + 6) == 0x4000 && ip_checksum(ip_tx_buf, 20) == 0 &&
                  arp_requests == 0);
    }
}

/**
 * Send `len` bytes of protocol `proto` to `dst` by path `path`, and check the
 * result. The paths are 0 this machine, 1 a next hop missing from the ARP
 * cache, 2 one in it, 3 the limited broadcast and 4 a destination with no
 * route. A payload past 1480 bytes is -EMSGSIZE before any route lookup, ARP,
 * queueing or transmission; with no route the send is -ENETUNREACH; an ICMP
 * broadcast is -EOPNOTSUPP; and anything else is taken, as check_tx_path
 * checks.
 */
static void test_tx_size(unsigned path, uint32_t dst, uint8_t proto, uint16_t len)
{
    static const uint8_t payload[65535];
    int rc, before = fails;

    route_available = path != 4;
    arp_known = path == 2;
    route_queries = arp_queries = arp_requests = tx_frames = 0;
    tx_answer = NETDEV_TX_OK;
    tx_len = 0;
    loop_head = loop_tail = loop_dropped = loop_peak = 0;
    ip_id_counter = 0;
    the_iface.tx_dropped = 0;
    rc = ip_tx(dst, proto, payload, len, IP_TTL_DEFAULT);
    if (len > 1480) {
        CHECK("oversize IP payload returns -EMSGSIZE on every path", rc == -EMSGSIZE);
        CHECK("oversize IP payload has no routing or transmit side effects",
              route_queries == 0 && arp_queries == 0 && arp_requests == 0 && tx_frames == 0 &&
                  loop_head == 0 && loop_dropped == 0 && loop_peak == 0 && ip_id_counter == 0 &&
                  the_iface.tx_dropped == 0);
    } else if (path == 4) {
        CHECK("in-range IP payload without a route returns -ENETUNREACH",
              rc == -ENETUNREACH && route_queries == 1 && tx_frames == 0);
    } else if (path == 3 && proto != IP_PROTO_UDP) {
        CHECK("in-range ICMP broadcast returns -EOPNOTSUPP", rc == -EOPNOTSUPP && tx_frames == 0);
    } else {
        CHECK("in-range IP payload is accepted", rc == 0);
        check_tx_path(path, len);
    }
    if (fails != before) {
        printf("packet_hosttest: path %u protocol %u payload %u returned %d\n", path,
               (unsigned)proto, (unsigned)len, rc);
    }
}

/** Check that a card whose link is down is handed no frame of any type. */
static void test_tx_link_down(void)
{
    static const uint8_t dst[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
    static const uint8_t payload[28];

    the_iface.up = 1;
    the_iface.tx_dropped = 0;
    the_iface.tx_packets = 0;
    tx_answer = NETDEV_TX_OK;
    tx_frames = 0;
    link_down = 1;
    CHECK("a card whose link is down is handed no IP frame",
          ethernet_tx(&the_iface, dst, ETH_TYPE_IP, payload, 28) == NETDEV_TX_DROPPED);
    CHECK("and no netconsole frame",
          ethernet_tx(&the_iface, dst, 0x88b5, payload, 28) == NETDEV_TX_DROPPED);
    CHECK("and each refusal counts as a transmit drop",
          tx_frames == 0 && the_iface.tx_dropped == 2 && the_iface.tx_packets == 0);
    link_down = 0;
    CHECK("with the link up the frame goes to the card",
          ethernet_tx(&the_iface, dst, ETH_TYPE_IP, payload, 28) == NETDEV_TX_OK &&
              tx_frames == 1 && the_iface.tx_packets == 1);
}

/**
 * Check empty, maximum and oversize payloads of ICMP and UDP on each path of
 * test_tx_size.
 */
static void test_tx_mtu(void)
{
    static const uint32_t destinations[] = {0x0A00020Fu, 0x0A000202u, 0x0A000202u, 0xFFFFFFFFu,
                                            0xC0000201u};
    static const uint16_t lengths[] = {0, 1480, 1481, 1500, 65535};
    static const uint8_t protocols[] = {IP_PROTO_ICMP, IP_PROTO_UDP};
    unsigned path, n, p;

    reset();
    for (p = 0; p < sizeof(protocols); p++) {
        for (path = 0; path < sizeof(destinations) / sizeof(destinations[0]); path++) {
            for (n = 0; n < sizeof(lengths) / sizeof(lengths[0]); n++) {
                test_tx_size(path, destinations[path], protocols[p], lengths[n]);
            }
        }
    }
    route_available = 1;
    loop_head = loop_tail = 0;
}

volatile uint32_t kernel_ticks; /* the clock, 100 ticks to a second */

/** A datagram sent while its next hop is missing from the ARP cache waits,
 * and leaves when the mapping is learned, once; one held past three seconds
 * is dropped. */
static void test_tx_hold(void)
{
    static const uint8_t mac[6] = {0x02, 0x22, 0x33, 0x44, 0x55, 0x66};
    static uint8_t payload[40];

    reset();
    ip_hold_forget(0);
    arp_known = 0;
    tx_answer = NETDEV_TX_OK;
    tx_frames = 0;
    kernel_ticks = 1000;
    CHECK("a send on an ARP miss is taken and sends no data frame",
          ip_tx(0x0A000202u, IP_PROTO_ICMP, payload, 40, IP_TTL_DEFAULT) == 0 && tx_frames == 0);
    ip_hold_resolved(&the_iface, 0x0A000202u, mac);
    CHECK("the held datagram leaves, whole, when its hop's mapping is learned",
          tx_frames == 1 && tx_len == 14 + 20 + 40 && tx_frame[0] == 0x02 && tx_frame[5] == 0x66 &&
              tx_frame[14 + 9] == IP_PROTO_ICMP);
    ip_hold_resolved(&the_iface, 0x0A000202u, mac);
    CHECK("and only once", tx_frames == 1);
    ip_tx(0x0A000202u, IP_PROTO_ICMP, payload, 40, IP_TTL_DEFAULT);
    kernel_ticks += 301;
    ip_hold_resolved(&the_iface, 0x0A000202u, mac);
    CHECK("a datagram held for more than three seconds is dropped", tx_frames == 1);
}

/**
 * Check that a send the driver takes is reported sent, that one it does not
 * take is -ENOBUFS, to be kept and sent again, and that a payload past the
 * frame's 1500 bytes is -EMSGSIZE and reaches no driver.
 */
static void test_tx_errors(void)
{
    static uint8_t big[1500];

    reset();
    arp_known = 1;
    tx_answer = NETDEV_TX_OK;
    tx_frames = 0;
    CHECK("a send the driver takes is reported sent",
          ip_tx(0x0A000202u, IP_PROTO_ICMP, big, 100, IP_TTL_DEFAULT) == 0 && tx_frames == 1);
    tx_answer = NETDEV_TX_DROPPED;
    CHECK("a send the driver did not take is -ENOBUFS, to be kept and sent again",
          ip_tx(0x0A000202u, IP_PROTO_ICMP, big, 100, IP_TTL_DEFAULT) == -ENOBUFS &&
              tx_frames == 2);
    tx_answer = NETDEV_TX_OK;
    CHECK("a payload that would pass the frame's 1500 bytes with its header is -EMSGSIZE",
          ip_tx(0x0A000202u, IP_PROTO_ICMP, big, 1490, IP_TTL_DEFAULT) == -EMSGSIZE);
    CHECK("and reaches no driver", tx_frames == 2);
    arp_known = 0;
}

/**
 * Fill the loopback ring and go on sending to this machine's own address.
 *
 * Every datagram past the last slot is refused with `-ENOBUFS`, which is what
 * lets TCP keep a refused segment and send it again, and is counted; the ring
 * keeps what it held. One line records the first refusal and no later one: a
 * line per refusal would wrap the kernel log and lose the boot record. A ring
 * drained and filled past its end again writes no second line.
 * Every datagram has its own number in its first two bytes, so the drain
 * shows that the ring delivers the datagrams it took, in the order they were
 * sent, and none of those it refused.
 */
static void test_loop_full(void)
{
    uint8_t payload[64] = {0};
    unsigned i, taken = 0, refused = 0, again = 0, in_order = 0;

    reset();
    tx_frames = arp_queries = 0;
    loop_head = loop_tail = loop_dropped = loop_peak = 0;
    loop_drop_logged = 0;
    loop_lines = 0;
    for (i = 0; i < LOOP_RING_SLOTS; i++) {
        write16be(payload, (uint16_t)i);
        taken += ip_tx(net_ip_addr, IP_PROTO_UDP, payload, sizeof(payload), IP_TTL_DEFAULT) == 0;
    }
    CHECK("the loopback ring takes as many datagrams as it has slots",
          taken == LOOP_RING_SLOTS && loop_head - loop_tail == LOOP_RING_SLOTS);
    for (i = 0; i < 3 * LOOP_RING_SLOTS; i++) {
        write16be(payload, (uint16_t)(LOOP_RING_SLOTS + i));
        refused +=
            ip_tx(net_ip_addr, IP_PROTO_UDP, payload, sizeof(payload), IP_TTL_DEFAULT) == -ENOBUFS;
    }
    CHECK("a datagram past a full loopback ring is refused with -ENOBUFS",
          refused == 3 * LOOP_RING_SLOTS);
    CHECK("each refused datagram is counted", ip_loop_dropped() == 3 * LOOP_RING_SLOTS);
    CHECK("a full ring keeps the datagrams it held",
          loop_head - loop_tail == LOOP_RING_SLOTS && ip_loop_peak() == LOOP_RING_SLOTS);
    CHECK("one line records the refusals, not one a datagram", loop_lines == 1);
    CHECK("a datagram to this machine reaches no driver and asks no ARP",
          tx_frames == 0 && arp_queries == 0);
    ip_loop_drain_from(IP_LOOP_FROM_TICK);
    CHECK("the drained ring delivers what it held",
          to_udp == LOOP_RING_SLOTS && !ip_loop_pending());
    for (i = 0; i < LOOP_RING_SLOTS && i < UDP_SEEN_MAX; i++) {
        in_order += udp_seen[i].number == i && udp_seen[i].len == sizeof(payload);
    }
    CHECK("the drain delivers the datagrams the ring took, in the order sent, and none it refused",
          to_udp == LOOP_RING_SLOTS && in_order == LOOP_RING_SLOTS);
    for (i = 0; i < LOOP_RING_SLOTS + 1; i++) {
        again +=
            ip_tx(net_ip_addr, IP_PROTO_UDP, payload, sizeof(payload), IP_TTL_DEFAULT) == -ENOBUFS;
    }
    CHECK("the drained ring takes datagrams again and refuses past its end",
          again == 1 && ip_loop_dropped() == 3 * LOOP_RING_SLOTS + 1);
    CHECK("and the later refusal writes no second line", loop_lines == 1);
    loop_head = loop_tail = 0;
}

/**
 * Send broadcasts on a machine with two cards.
 *
 * A subnet's broadcast leaves by the card on that subnet, with that card's
 * address as its source, since that is the only address its answers can come
 * back to; the limited broadcast, which names no card, leaves by the first.
 * Both go to the all-ones hardware address without asking ARP. A second card
 * masked /1 has 255.255.255.255 as its subnet's broadcast address, and the
 * limited broadcast still leaves by the first card.
 */
static void test_broadcast_iface(void)
{
    static const uint8_t payload[8];
    static const struct {
        uint32_t dst;
        struct netdev *card;
        const char *what;
    } rows[] = {
        {0xC0A807FFu, &second_iface, "the second card's subnet broadcast leaves by that card"},
        {0x0A0002FFu, &the_iface, "the first card's subnet broadcast leaves by that card"},
        {0xFFFFFFFFu, &the_iface, "the limited broadcast leaves by the first card"},
    };
    unsigned r;

    reset();
    second_iface = the_iface;
    second_iface.name[3] = '1';
    second_iface.index = 2;
    second_iface.mac[5] = 0x57;
    second_iface.ip = 0xC0A8070Au;
    second_iface.gateway = 0;
    ifaces = 2;
    tx_answer = NETDEV_TX_OK;
    for (r = 0; r < sizeof(rows) / sizeof(rows[0]); r++) {
        int rc;

        tx_frames = arp_queries = 0;
        tx_card = 0;
        rc = ip_tx(rows[r].dst, IP_PROTO_UDP, payload, sizeof(payload), IP_TTL_DEFAULT);
        CHECK(rows[r].what, rc == 0 && tx_frames == 1 && tx_card == rows[r].card);
        CHECK("with the source address of the card it leaves by",
              read32be(tx_frame + 14 + 12) == rows[r].card->ip);
        CHECK("to the all-ones hardware address, without ARP",
              tx_frame[0] == 0xFF && tx_frame[5] == 0xFF && arp_queries == 0);
    }
    second_iface.mask = 0x80000000u;
    tx_frames = arp_queries = 0;
    tx_card = 0;
    CHECK("the limited broadcast leaves by the first card beside a card masked /1",
          ip_tx(0xFFFFFFFFu, IP_PROTO_UDP, payload, sizeof(payload), IP_TTL_DEFAULT) == 0 &&
              tx_frames == 1 && tx_card == &the_iface);
    ifaces = 1;
}

/**
 * Run the send rows, then the receive rows, each frame built here:
 *
 *   * a whole UDP datagram reaches the port layer, the packet sockets and the
 *     raw layer, and nothing else;
 *   * a frame shorter than an Ethernet header is dropped, and one that is only
 *     the header reaches no IP parser but is still offered to the packet
 *     sockets, which exist to see what the IP layer drops, every reply a DHCP
 *     client is waiting for among them;
 *   * an interface that is down delivers nothing;
 *   * an IPv4 header with a version other than 4, a header length below the
 *     minimum or past the frame, a total length past the frame or below the
 *     header, or a checksum that does not sum is dropped;
 *   * a fragment is not delivered as a whole datagram, since this stack does
 *     not reassemble, and a datagram addressed elsewhere is not delivered;
 *   * ICMP and TCP reach their own parsers and the raw layer; an EtherType with
 *     no parser reaches none; and ARP reaches the ARP parser.
 *
 * Returns 0 when every check passes and 1 otherwise.
 */
int main(void)
{
    uint8_t f[256];
    unsigned n;

    test_tx_mtu();
    test_tx_link_down();
    test_tx_errors();
    test_tx_hold();
    test_loop_full();
    test_broadcast_iface();

    reset();
    n = build_eth(f, 0x0800);
    n += build_ip(f + n, 17, 8);
    deliver(f, n + 8);
    CHECK("a whole UDP datagram reaches the port layer", to_udp == 1);
    CHECK("and the packet sockets", to_packet == 1);
    CHECK("and the raw layer", to_raw == 1);
    CHECK("and nothing else", to_tcp == 0 && to_icmp == 0);

    reset();
    deliver(f, 13);
    CHECK("a frame shorter than an Ethernet header is dropped", to_packet == 0 && to_udp == 0);
    reset();
    n = build_eth(f, 0x0800);
    deliver(f, n);
    CHECK("a frame that is only an Ethernet header reaches no IP parser",
          to_udp == 0 && to_tcp == 0 && to_icmp == 0 && to_raw == 0);
    CHECK("but the packet sockets are still offered it", to_packet == 1);

    reset();
    the_iface.up = 0; /* the flag the receive path reads is the interface's */
    n = build_eth(f, 0x0800);
    n += build_ip(f + n, 17, 8);
    deliver(f, n + 8);
    CHECK("an interface that is down delivers nothing", to_udp == 0);

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

    reset();
    n = build_eth(f, 0x0800);
    build_ip(f + n, 17, 8);
    f[n + 6] = 0x20; /* more fragments */
    restamp(f + n);
    deliver(f, n + 28);
    CHECK("a fragment is not delivered as a whole datagram", to_udp == 0);

    reset();
    n = build_eth(f, 0x0800);
    build_ip(f + n, 17, 8);
    f[n + 19] = 200; /* a destination this machine does not have */
    restamp(f + n);
    deliver(f, n + 28);
    CHECK("a datagram addressed elsewhere is not delivered", to_udp == 0);

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

    reset();
    n = build_eth(f, 0x88B5);
    deliver(f, n + 20);
    CHECK("an unknown ethertype reaches no parser",
          to_udp == 0 && to_tcp == 0 && to_icmp == 0 && to_arp == 0);

    reset();
    n = build_eth(f, 0x0806);
    deliver(f, n + 28);
    CHECK("ARP reaches the ARP parser", to_arp == 1);

    if (fails == 0) {
        printf("packet_hosttest: PASS\n");
    }
    return fails != 0;
}
