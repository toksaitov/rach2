/*
 * arp_hosttest.c: the address cache, driven on the host.
 *
 * host-deps: rach2/sys/lib/kstring.c rach2/sys/lib/kctypes.c
 *
 * The cache is eight entries and this machine now lives on a real network,
 * where it hears every neighbour rather than the three peers QEMU provides.
 * What that changes cannot be provoked from inside a guest: a test there can
 * neither invent nine neighbours nor make one of them send the ARP a DHCP
 * client sends while it is probing, which carries a sender address of 0.0.0.0.
 *
 * So `arp.c` is compiled natively and fed packets one at a time, with the
 * Ethernet layer replaced by a recorder. The cases are the ones a real network
 * produced or would produce:
 *
 *   * an address no neighbour can have is not cached (0.0.0.0, which was found
 *     in the cache of the machine on the desk, and the broadcast and multicast
 *     ranges, which name many machines or none);
 *   * a ninth neighbour replaces the entry gone longest without use, rather
 *     than being dropped: dropping it is how a machine that filled its cache
 *     with strangers before learning its gateway loses every route off its own
 *     subnet, permanently;
 *   * a lookup counts as use, so a mapping the machine keeps reaching for is
 *     among the last to be replaced rather than the first;
 *   * a request for our address is answered, and the reply carries our own
 *     hardware address;
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

void ethernet_tx(struct netdev *out, const uint8_t *dst, uint16_t type, const uint8_t *payload,
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

int netconf_is_broadcast(uint32_t ip)
{
    return ip == 0xFFFFFFFFu || ip == ((net_ip_addr & net_ip_mask) | ~net_ip_mask);
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

/** Feed one ARP packet: `op`, from `sender_ip` and a MAC ending in
 *  `mac_last`, about `target_ip`. */
static void feed(int op, uint32_t sender_ip, uint8_t mac_last, uint32_t target_ip)
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
    arp_rx(&the_iface, pkt, 28);
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

int main(void)
{
    uint8_t mac[6];
    int i;

    printf("arp_hosttest: the address cache\n");

    /* An address no neighbour can have is not cached. 0.0.0.0 is what a DHCP
       client sends while it is probing, and it was found in the cache of the
       machine on the desk. */
    arp_flush();
    frame_count = 0;
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
    feed(ARP_OP_REPLY, 0xC0A80001u, 0x05, 0xC0A80005u);
    CHECK(cached() == 1, "an ordinary neighbour was not cached");
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

    /* A ninth neighbour replaces the entry gone longest without use. The
       gateway is looked up in between, which is what keeps it. */
    arp_flush();
    for (i = 0; i < ARP_CACHE_SIZE; i++) {
        feed(ARP_OP_REPLY, 0xC0A80010u + (uint32_t)i, (unsigned char)(0x10 + i), net_ip_addr);
    }
    CHECK(cached() == ARP_CACHE_SIZE, "the cache did not fill");
    CHECK(holds(0xC0A80010u), "the first neighbour is missing before the ninth arrives");
    /* That lookup was use: the second entry is now the oldest. */
    feed(ARP_OP_REPLY, 0xC0A80099u, 0x99, net_ip_addr);
    CHECK(cached() == ARP_CACHE_SIZE, "the cache grew past its size");
    CHECK(holds(0xC0A80099u), "the ninth neighbour was dropped instead of cached");
    CHECK(holds(0xC0A80010u), "the entry that was just used was the one replaced");
    CHECK(!holds(0xC0A80011u), "the entry gone longest without use is still there");

    /* A mapping that changes is followed, not duplicated. */
    feed(ARP_OP_REPLY, 0xC0A80099u, 0x77, net_ip_addr);
    CHECK(cached() == ARP_CACHE_SIZE, "a changed mapping added an entry");
    CHECK(arp_lookup(&the_iface, 0xC0A80099u, mac) && mac[5] == 0x77,
          "a changed mapping was not followed");

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
