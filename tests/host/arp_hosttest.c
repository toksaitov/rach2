/*
 * arp_hosttest.c: the address cache, driven on the host.
 *
 * host-deps: rach2/sys/lib/kstring.c rach2/sys/lib/kctypes.c
 *
 * The cache is sixteen entries and this machine now lives on a real network,
 * where it hears every neighbour rather than the three peers QEMU provides.
 * What that changes cannot be provoked from inside a guest: a test there can
 * neither invent seventeen neighbours nor make one of them send the ARP a DHCP
 * client sends while it is probing, which carries a sender address of 0.0.0.0.
 *
 * So `arp.c` is compiled natively and fed packets one at a time, with the
 * Ethernet layer replaced by a recorder. The cases are the ones a real network
 * produced or would produce:
 *
 *   * an address no neighbour can have is not cached (0.0.0.0, which was found
 *     in the cache of the machine on the desk, and the broadcast and multicast
 *     ranges, which name many machines or none);
 *   * a seventeenth neighbour replaces the entry gone longest without use, rather
 *     than being dropped: dropping it is how a machine that filled its cache
 *     with strangers before learning its gateway loses every route off its own
 *     subnet, permanently;
 *   * a lookup counts as use, so a mapping the machine keeps reaching for is
 *     among the last to be replaced rather than the first;
 *   * a request for our address is answered, and the reply carries our own
 *     hardware address;
 *   * a mapping set by hand as permanent is neither changed by a packet nor
 *     replaced when the cache is full, nor flushed;
 *   * the ordering survives the counter wrapping, which no run of this machine
 *     will reach and which a reader cannot check by reading.
 *
 * The file is included, not linked, so the test can put its statics back
 * between cases; the kernel has one boot per run and needs no such thing.
 */

extern int printf(const char *fmt, ...);

#include "../../rach2/sys/network/arp/arp.h"
#include "../../rach2/sys/network/driver/netdev.h"

/* ------------------------------------------------------------------ */
/* The stubs arp.c links against.                                      */
/* ------------------------------------------------------------------ */

#define MAX_FRAMES 32

static uint8_t frame_data[MAX_FRAMES][64];
static uint16_t frame_len[MAX_FRAMES];
static uint16_t frame_type[MAX_FRAMES];
static uint8_t frame_dst[MAX_FRAMES][6];
static unsigned frame_count;

uint8_t net_mac[6] = {0x52, 0x54, 0x00, 0x12, 0x34, 0x56};
uint32_t net_ip_addr = 0xC0A80005u; /* 192.168.0.5 */
uint32_t net_ip_mask = 0xFFFFFF00u;

/* The interface a frame leaves by, which this model does not look
 * inside. */
struct netdev;

/* The interface this test's stack sends through. It never looks inside one,
 * so a record of its own is all that is needed. */
/* The interface these cases run on, with the machine's own address, since
 * that is what a reply answers for and what a neighbour is judged against. */
static struct netdev the_iface = {0,
                                  0,
                                  {'e', 't', 'h', '0', 0, 0, 0, 0},
                                  1,
                                  {0x52, 0x54, 0x00, 0x12, 0x34, 0x56},
                                  0xC0A80005u,
                                  0xFFFFFF00u,
                                  0};

struct netdev *netdev_first(void)
{
    return &the_iface;
}

int netdev_holds(const struct netdev *nd, uint32_t ip)
{
    return nd && ip != 0 && ip == nd->ip;
}

uint32_t netdev_source(const struct netdev *nd, uint32_t hop)
{
    (void)hop;
    return nd->ip;
}

int ethernet_tx(struct netdev *out, const uint8_t *dst, uint16_t type, const uint8_t *payload,
                uint16_t plen)
{
    (void)out;
    unsigned i;

    if (frame_count < MAX_FRAMES) {
        frame_type[frame_count] = type;
        frame_len[frame_count] = plen;
        for (i = 0; i < 6; i++) {
            frame_dst[frame_count][i] = dst[i];
        }
        for (i = 0; i < plen && i < sizeof(frame_data[0]); i++) {
            frame_data[frame_count][i] = payload[i];
        }
    }
    frame_count++;
    return 0;
}

/* The stack's held datagrams, of which the cache tells the address it learns
   and which a flush drops. */
static uint32_t resolved_ip;
static unsigned resolved_calls;

void ip_hold_resolved(struct netdev *nd, uint32_t hop, const uint8_t *mac)
{
    (void)nd;
    (void)mac;
    resolved_ip = hop;
    resolved_calls++;
}

void ip_hold_forget(struct netdev *nd)
{
    (void)nd;
}

/* The kernel's interrupt bracket. On the host there are no interrupts, so the
   flag value is a token; what the test needs is that every lock is released,
   which the balance counter checks. */
static int lock_depth;
static int lock_ever_negative;

uint32_t net_lock(void)
{
    lock_depth++;
    return 0;
}

void net_unlock(uint32_t flags)
{
    (void)flags;
    lock_depth--;
    if (lock_depth < 0) {
        lock_ever_negative = 1;
    }
}

/* The big-endian readers and writers live beside the interrupt bracket in
   net_core.c, which is x86 assembly and will not compile here, so they are
   written out. They are the whole of what arp.c needs from that file. */
uint16_t read16be(const uint8_t *p)
{
    return (uint16_t)((p[0] << 8) | p[1]);
}

uint32_t read32be(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

void write16be(uint8_t *p, uint16_t value)
{
    p[0] = (uint8_t)(value >> 8);
    p[1] = (uint8_t)(value & 0xff);
}

void write32be(uint8_t *p, uint32_t value)
{
    p[0] = (uint8_t)(value >> 24);
    p[1] = (uint8_t)((value >> 16) & 0xff);
    p[2] = (uint8_t)((value >> 8) & 0xff);
    p[3] = (uint8_t)(value & 0xff);
}

int netconf_address_is_impossible(uint32_t ip)
{
    uint32_t top = ip >> 24;

    return top == 0u || top == 127u || top >= 224u;
}

static struct netdev second_iface;

int netconf_is_broadcast(uint32_t ip)
{
    return ip == 0xFFFFFFFFu || ip == (the_iface.ip | ~the_iface.mask) ||
           (second_iface.mask != 0 && ip == (second_iface.ip | ~second_iface.mask));
}

#include "../../rach2/sys/network/arp/arp.c"

/* ------------------------------------------------------------------ */
/* The test.                                                           */
/* ------------------------------------------------------------------ */

static int fails;

#define CHECK(cond, what)                   \
    do {                                    \
        if (!(cond)) {                      \
            printf("  FAIL: %s\n", (what)); \
            fails++;                        \
        }                                   \
    } while (0)

/** Feed an Ethernet/IPv4 ARP packet to `nd`. */
static void feed_on(struct netdev *nd, int op, uint32_t sender_ip, uint8_t mac_last,
                    uint32_t target_ip)
{
    uint8_t pkt[28];
    int i;

    for (i = 0; i < 28; i++) {
        pkt[i] = 0;
    }
    pkt[1] = 1;    /* hardware type: Ethernet */
    pkt[2] = 0x08; /* protocol type: IPv4 */
    pkt[4] = 6;
    pkt[5] = 4;
    pkt[7] = (unsigned char)op;
    pkt[8] = 0xAA;
    pkt[9] = 0xBB;
    pkt[10] = 0xCC;
    pkt[11] = 0xDD;
    pkt[12] = 0xEE;
    pkt[13] = mac_last;
    pkt[14] = (unsigned char)((sender_ip >> 24) & 0xff);
    pkt[15] = (unsigned char)((sender_ip >> 16) & 0xff);
    pkt[16] = (unsigned char)((sender_ip >> 8) & 0xff);
    pkt[17] = (unsigned char)(sender_ip & 0xff);
    pkt[24] = (unsigned char)((target_ip >> 24) & 0xff);
    pkt[25] = (unsigned char)((target_ip >> 16) & 0xff);
    pkt[26] = (unsigned char)((target_ip >> 8) & 0xff);
    pkt[27] = (unsigned char)(target_ip & 0xff);
    arp_rx(nd, pkt, 28);
}

/** Feed one ARP packet to the primary interface. */
static void feed(int op, uint32_t sender_ip, uint8_t mac_last, uint32_t target_ip)
{
    feed_on(&the_iface, op, sender_ip, mac_last, target_ip);
}

/** How many entries the cache holds. */
static int cached(void)
{
    struct arp_row rows[ARP_CACHE_SIZE];

    return arp_snapshot(rows, ARP_CACHE_SIZE);
}

/** 1 if `ip` is in the cache. */
static int holds(uint32_t ip)
{
    uint8_t mac[6];

    return arp_lookup(&the_iface, ip, mac);
}

/** Require a rejected packet to leave both new and existing mappings alone. */
static void reject_packet(uint8_t *pkt, uint16_t len)
{
    uint8_t mac[6];

    arp_flush();
    frame_count = 0;
    feed(ARP_OP_REPLY, 0xC0A80009u, 1, the_iface.ip);
    write32be(pkt + 14, 0xC0A80009u);
    arp_rx(&the_iface, pkt, len);
    CHECK(arp_lookup(&the_iface, 0xC0A80009u, mac) && mac[5] == 1,
          "invalid ARP layout changed a mapping");
    write32be(pkt + 14, 0xC0A8000Au);
    arp_rx(&the_iface, pkt, len);
    CHECK(cached() == 1 && !holds(0xC0A8000Au), "invalid ARP layout added a mapping");
    CHECK(frame_count == 0, "invalid ARP layout produced a reply");
}

/** Check address lengths independently for requests and replies. */
static void test_layout(void)
{
    static const uint8_t bad_lengths[2][4] = {{0, 5, 7, 255}, {0, 3, 5, 255}};
    uint8_t pkt[29];
    unsigned field, i;
    int op;

    k_memset(pkt, 0, sizeof(pkt));
    write16be(pkt, 1);
    write16be(pkt + 2, ETH_TYPE_IP);
    pkt[4] = 6;
    pkt[5] = 4;
    pkt[13] = 2;
    write32be(pkt + 24, the_iface.ip);
    for (op = ARP_OP_REQUEST; op <= ARP_OP_REPLY; op++) {
        write16be(pkt + 6, (uint16_t)op);
        for (field = 0; field < 2; field++) {
            for (i = 0; i < 4; i++) {
                pkt[4 + field] = bad_lengths[field][i];
                reject_packet(pkt, 28);
            }
            pkt[4 + field] = field == 0 ? 6 : 4;
        }
        reject_packet(pkt, 27);
        pkt[1] = 2;
        reject_packet(pkt, 28);
        pkt[1] = 1;
        pkt[2] = 9;
        reject_packet(pkt, 28);
        pkt[2] = 8;
    }
    arp_flush();
    frame_count = 0;
    write16be(pkt + 6, ARP_OP_REQUEST);
    arp_rx(&the_iface, pkt, sizeof(pkt));
    CHECK(cached() == 1 && frame_count == 1, "valid padded ARP request was rejected");
}

/** Check subnet exclusions against the receiving interface alone. */
static void test_interfaces(void)
{
    uint8_t mac[6];

    second_iface.ip = 0x0A000005u;
    second_iface.mask = 0xFFFFFF00u;
    arp_flush();
    feed_on(&second_iface, ARP_OP_REPLY, 0x0A000000u, 1, second_iface.ip);
    CHECK(cached() == 0, "second interface network address was cached");
    arp_flush();
    feed_on(&second_iface, ARP_OP_REPLY, 0x0A0000FFu, 1, second_iface.ip);
    CHECK(cached() == 0, "second interface broadcast address was cached");
    feed_on(&second_iface, ARP_OP_REPLY, 0x0A000001u, 2, second_iface.ip);
    CHECK(arp_lookup(&second_iface, 0x0A000001u, mac) && mac[5] == 2,
          "second interface neighbour was rejected");
    CHECK(!holds(0x0A000001u), "second interface mapping matched the primary interface");

    second_iface.ip = 0xC0A80105u;
    second_iface.mask = 0xFF000000u;
    arp_flush();
    feed_on(&second_iface, ARP_OP_REPLY, 0xC0A80000u, 3, second_iface.ip);
    CHECK(arp_lookup(&second_iface, 0xC0A80000u, mac) && mac[5] == 3,
          "another interface network address was rejected");
    feed_on(&second_iface, ARP_OP_REPLY, 0xC0A800FFu, 4, second_iface.ip);
    CHECK(arp_lookup(&second_iface, 0xC0A800FFu, mac) && mac[5] == 4,
          "another interface broadcast address was rejected");

    arp_flush();
    feed_on(&second_iface, ARP_OP_REQUEST, 0xC0A80109u, 5, second_iface.ip);
    feed(ARP_OP_REPLY, 0xC0A80109u, 6, the_iface.ip);
    CHECK(cached() == 2 && arp_lookup(&second_iface, 0xC0A80109u, mac) && mac[5] == 5,
          "second interface mapping was overwritten");
    CHECK(arp_lookup(&the_iface, 0xC0A80109u, mac) && mac[5] == 6,
          "primary interface mapping was overwritten");
}

int main(void)
{
    uint8_t mac[6];
    int i;

    printf("arp_hosttest: the address cache\n");
    test_layout();
    test_interfaces();

    /* An address no neighbour can have is not cached. 0.0.0.0 is what a DHCP
       client sends while it is probing, and it was found in the cache of the
       machine on the desk. */
    arp_flush();
    frame_count = 0;
    resolved_calls = 0;
    feed(ARP_OP_REQUEST, 0x00000000u, 0x01, 0xC0A80005u);
    CHECK(cached() == 0, "0.0.0.0 was cached, and nothing can be sent to it");
    CHECK(frame_count == 1, "a probe from 0.0.0.0 for our address went unanswered");
    feed(ARP_OP_REPLY, 0xFFFFFFFFu, 0x02, 0xC0A80005u);
    CHECK(cached() == 0, "the all-ones broadcast was cached");
    feed(ARP_OP_REPLY, 0xC0A800FFu, 0x03, 0xC0A80005u);
    CHECK(cached() == 0, "this subnet's broadcast address was cached");
    feed(ARP_OP_REPLY, 0xE0000001u, 0x04, 0xC0A80005u);
    CHECK(cached() == 0, "a multicast address was cached");
    feed(ARP_OP_REPLY, 0x7F000001u, 0x06, 0xC0A80005u);
    CHECK(cached() == 0, "a loopback address was cached, which this stack is itself");
    feed(ARP_OP_REPLY, 0xC0A80000u, 0x07, 0xC0A80005u);
    CHECK(cached() == 0, "the subnet's own network address was cached");
    CHECK(resolved_calls == 0, "an address that was refused released held datagrams");
    feed(ARP_OP_REPLY, 0xC0A80001u, 0x05, 0xC0A80005u);
    CHECK(cached() == 1, "an ordinary neighbour was not cached");
    CHECK(resolved_calls == 1 && resolved_ip == 0xC0A80001u,
          "the neighbour learned did not release the datagrams held for it");
    CHECK(holds(0xC0A80001u), "the neighbour that was cached cannot be looked up");

    /* A request for our address is answered, with our own hardware address in
       the reply, and the answer goes to the machine that asked. */
    arp_flush();
    frame_count = 0;
    feed(ARP_OP_REQUEST, 0xC0A80009u, 0x09, net_ip_addr);
    CHECK(frame_count == 1, "a request for our address was not answered");
    if (frame_count == 1) {
        CHECK(frame_type[0] == 0x0806, "the answer was not an ARP frame");
        CHECK(frame_len[0] == 28, "the answer was not 28 bytes");
        CHECK(frame_data[0][7] == ARP_OP_REPLY, "the answer was not a reply");
        CHECK(frame_data[0][8] == net_mac[0] && frame_data[0][13] == net_mac[5],
              "the answer did not carry our own hardware address");
        CHECK(frame_dst[0][5] == 0x09, "the answer did not go to the asker");
    }

    /* A seventeenth neighbour replaces the entry gone longest without use.
       The gateway is looked up in between, which is what keeps it. */
    arp_flush();
    for (i = 0; i < ARP_CACHE_SIZE; i++) {
        feed(ARP_OP_REPLY, 0xC0A80010u + (uint32_t)i, (unsigned char)(0x10 + i), net_ip_addr);
    }
    CHECK(cached() == ARP_CACHE_SIZE, "the cache did not fill");
    CHECK(holds(0xC0A80010u), "the first neighbour is missing before the last arrives");
    /* That lookup was use: the second entry is now the oldest. */
    feed(ARP_OP_REPLY, 0xC0A80099u, 0x99, net_ip_addr);
    CHECK(cached() == ARP_CACHE_SIZE, "the cache grew past its size");
    CHECK(holds(0xC0A80099u), "the last neighbour was dropped instead of cached");
    CHECK(holds(0xC0A80010u), "the entry that was just used was the one replaced");
    CHECK(!holds(0xC0A80011u), "the entry gone longest without use is still there");

    /* A mapping that changes is followed, not duplicated. */
    feed(ARP_OP_REPLY, 0xC0A80099u, 0x77, net_ip_addr);
    CHECK(cached() == ARP_CACHE_SIZE, "a changed mapping added an entry");
    CHECK(arp_lookup(&the_iface, 0xC0A80099u, mac) && mac[5] == 0x77,
          "a changed mapping was not followed");

    /* A permanent mapping: a packet does not change it, a full cache does not
       replace it, a flush keeps it, and forgetting it by hand drops it. */
    arp_flush();
    {
        static const uint8_t set[6] = {0x02, 0, 0, 0, 0, 0x5e};

        CHECK(arp_set(&the_iface, 0xC0A8005Eu, set, 1) == 0, "a permanent mapping was refused");
        feed(ARP_OP_REPLY, 0xC0A8005Eu, 0x66, net_ip_addr);
        CHECK(arp_lookup(&the_iface, 0xC0A8005Eu, mac) && mac[5] == 0x5e,
              "a packet changed a permanent mapping");
        for (i = 0; i < ARP_CACHE_SIZE + 2; i++) {
            feed(ARP_OP_REPLY, 0xC0A80060u + (uint32_t)i, (unsigned char)(0x60 + i), net_ip_addr);
        }
        arp_flush();
        CHECK(holds(0xC0A8005Eu), "a full cache or a flush dropped a permanent mapping");
        CHECK(arp_forget(&the_iface, 0xC0A8005Eu) == 0 && !holds(0xC0A8005Eu),
              "a permanent mapping was not forgotten by hand");
    }

    /* And the ordering survives the counter wrapping, which no run reaches. */
    arp_flush();
    feed(ARP_OP_REPLY, 0xC0A80021u, 0x21, net_ip_addr);
    feed(ARP_OP_REPLY, 0xC0A80022u, 0x22, net_ip_addr);
    arp_clock = 0xFFFFFFFEu;
    (void)arp_lookup(&the_iface, 0xC0A80021u, mac); /* 21 is used; 22 becomes the oldest */
    for (i = 0; i < ARP_CACHE_SIZE - 2; i++) {
        feed(ARP_OP_REPLY, 0xC0A80030u + (uint32_t)i, (unsigned char)(0x30 + i), net_ip_addr);
    }
    feed(ARP_OP_REPLY, 0xC0A80040u, 0x40, net_ip_addr);
    CHECK(holds(0xC0A80021u), "the used entry was lost across the counter wrap");
    CHECK(!holds(0xC0A80022u), "the oldest entry survived the wrap and another left");

    CHECK(lock_depth == 0, "a lock was taken and not released");
    CHECK(!lock_ever_negative, "a lock was released more often than it was taken");

    if (fails == 0) {
        printf("arp_hosttest: OK\n");
        return 0;
    }
    printf("arp_hosttest: %d case(s) failed\n", fails);
    return 1;
}
