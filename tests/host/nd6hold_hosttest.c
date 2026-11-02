/*
 * nd6hold_hosttest.c: a datagram past the path MTU sent to a neighbour whose
 * station address is not yet known, held by nd6.c and fragmented by frag6.c.
 *
 * host-deps: rach2/sys/network/ip6/frag6.c rach2/sys/network/ip6/addr6.c
 * host-deps: rach2/sys/lib/kstring.c rach2/sys/lib/kctypes.c
 * host-deps: rach2/sys/lib/ksha256.c
 * host-deps: tests/host/compat/kheap_host.c
 *
 * The neighbour cache holds a few datagrams while it solicits a hop's
 * address. A datagram of 6000 bytes leaves a card of MTU 1500 as five
 * fragments; held one fragment to a slot, the fifth would put the first out
 * of a queue of four, and the datagram would never be put back together at
 * the far end, while the sender was told it was sent. A guest cannot show
 * this: QEMU's user network answers a solicitation before a second fragment
 * is sent. So nd6.c is compiled whole with the layers around it replaced by
 * recorders, the hop's answer is given by hand, and the frames the card is
 * handed are put back together here.
 */
#include "network/ip6/nd6.c"
#include "network/ip6/frag6.h"

extern int printf(const char *format, ...);
extern int memcmp(const void *a, const void *b, unsigned long n);

volatile uint64_t kernel_ticks;
struct net_mib6 net_mib6;

static int failures;

#define CHECK(condition, detail)                              \
    do {                                                      \
        if (!(condition)) {                                   \
            printf("nd6hold_hosttest: failed: %s\n", detail); \
            failures++;                                       \
        }                                                     \
    } while (0)

/* The one card, of MTU 1500 and index 1. */
static struct netdev card;

/* The frames the card was handed and the solicitations sent; the payload
 * of the datagram the fragments make up, put back together by offset, the
 * bytes they carried, whether every fragment had the first's identification
 * and whether one came without M; and the address-unreachable errors. */
static unsigned frames, solicits, unreachables;
static uint8_t whole[65536];
static uint32_t whole_bytes, whole_id;
static int ids_agree, saw_last;

/** Read the big-endian 16-bit field at `p`. */
uint16_t read16be(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

/** Read the big-endian 32-bit field at `p`. */
uint32_t read32be(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

/** Store `v` big-endian at `p`. */
void write16be(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

/** Store `v` big-endian at `p`. */
void write32be(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

/** Record an IPv6 frame: a fragment's payload goes into `whole` at its
 * offset. */
int ethernet_tx(struct netdev *out, const uint8_t *dst_mac, uint16_t ethertype,
                const uint8_t *payload, uint16_t plen)
{
    (void)out;
    (void)dst_mac;
    frames++;
    if (ethertype == ETH_TYPE_IPV6 && plen > IP6_HDR_SIZE + IP6_FRAG_HDR_SIZE &&
        payload[6] == IP6_NH_FRAGMENT) {
        const uint8_t *fh = payload + IP6_HDR_SIZE;
        uint32_t off = read16be(fh + 2) & ~7u;
        uint32_t n = plen - IP6_HDR_SIZE - IP6_FRAG_HDR_SIZE;

        if (whole_bytes == 0) {
            whole_id = read32be(fh + 4);
            ids_agree = 1;
        }
        ids_agree &= read32be(fh + 4) == whole_id;
        saw_last |= !(read16be(fh + 2) & IP6_FRAG_MORE);
        if (off + n <= sizeof(whole)) {
            k_memcpy(whole + off, fh + IP6_FRAG_HDR_SIZE, n);
        }
        whole_bytes += n;
    }
    return NETDEV_TX_OK;
}

/** Give a unicast datagram to the neighbour cache, as ip6.c does. */
int ip6_emit(struct netdev *out, const uint8_t *via, const uint8_t *buf, uint32_t len)
{
    return nd6_output(out, via, buf, (uint16_t)len);
}

/** Write the fixed header, as ip6.c does. */
void ip6_header(uint8_t *buf, const uint8_t *src, const uint8_t *dst, uint8_t nh, uint16_t plen,
                uint8_t hlim)
{
    buf[0] = 0x60;
    buf[1] = buf[2] = buf[3] = 0;
    write16be(buf + 4, plen);
    buf[6] = nh;
    buf[7] = hlim;
    k_memcpy(buf + 8, src, IP6_ADDR_LEN);
    k_memcpy(buf + 24, dst, IP6_ADDR_LEN);
}

/** Set the traffic class, as ip6.c does. */
void ip6_set_class(uint8_t *buf, uint8_t tclass)
{
    buf[0] = (uint8_t)(0x60 | (tclass >> 4));
    buf[1] = (uint8_t)(((tclass & 0x0f) << 4) | (buf[1] & 0x0f));
}

/** Count a solicitation, the only message the rows send. */
int icmp6_send_opts(const uint8_t *src, const uint8_t *dst, int ifindex, uint8_t *msg, uint16_t len,
                    uint8_t hlim, int opts)
{
    (void)src;
    (void)dst;
    (void)ifindex;
    (void)len;
    (void)hlim;
    (void)opts;
    solicits += msg[0] == ND6_NEIGHBOR_SOLICIT;
    return 0;
}

/** Count an address unreachable for a held datagram. */
void icmp6_unreach_local(const uint8_t *dgram, uint16_t len)
{
    (void)dgram;
    (void)len;
    unreachables++;
}

/** No row receives a fragment, which alone sends an error. */
void icmp6_error(struct netdev *nd, const uint8_t *dgram, uint16_t len, uint8_t type, uint8_t code,
                 uint32_t param)
{
    (void)nd;
    (void)dgram;
    (void)len;
    (void)type;
    (void)code;
    (void)param;
}

/** No row reassembles. */
void ip6_walk(struct netdev *nd, const uint8_t *dg, uint32_t len, uint32_t off, uint8_t nh,
              uint32_t nhoff, int reassembled)
{
    (void)nd;
    (void)dg;
    (void)len;
    (void)off;
    (void)nh;
    (void)nhoff;
    (void)reassembled;
}

/** The card holds every source a datagram names. */
int netdev_holds6(struct netdev *nd, const uint8_t *addr)
{
    (void)nd;
    (void)addr;
    return 1;
}

/** A source is never asked for: the held datagram's is used. */
int ip6_source(const struct netdev *out, const uint8_t *dst, uint8_t *src)
{
    (void)out;
    (void)dst;
    (void)src;
    return -1;
}

/* What follows answers the calls of the parts of nd6.c no row reaches. */

void klog(int level, const char *subsystem, const char *format, ...)
{
    (void)level;
    (void)subsystem;
    (void)format;
}

void krandom_bytes(void *buf, uint32_t n)
{
    k_memset(buf, 0x5a, n);
}

uint32_t krandom_u32(void)
{
    return 0x12345678u;
}

void mld6_tick(void)
{}

uint32_t net_lock(void)
{
    return 0;
}

void net_unlock(uint32_t flags)
{
    (void)flags;
}

int netconf_add_address6(struct netdev *nd, const uint8_t *addr, int prefix, int flags,
                         uint32_t valid, uint32_t preferred)
{
    (void)nd;
    (void)addr;
    (void)prefix;
    (void)flags;
    (void)valid;
    (void)preferred;
    return -1;
}

int netconf_del_address6(struct netdev *nd, const uint8_t *addr)
{
    (void)nd;
    (void)addr;
    return -1;
}

int netconf_set_up(struct netdev *nd, int up)
{
    (void)nd;
    (void)up;
    return 0;
}

struct netdev_addr6 *netdev_addr6_of(struct netdev *nd, const uint8_t *addr)
{
    (void)nd;
    (void)addr;
    return 0;
}

struct netdev *netdev_at(int n)
{
    return n == 0 ? &card : 0;
}

int netdev_count(void)
{
    return 1;
}

int netdev_join6(struct netdev *nd, const uint8_t *group)
{
    (void)nd;
    (void)group;
    return 0;
}

void netdev_leave6(struct netdev *nd, const uint8_t *group)
{
    (void)nd;
    (void)group;
}

int netdev_listed(void)
{
    return 1;
}

int netdev_member6(const struct netdev *nd, const uint8_t *group)
{
    (void)nd;
    (void)group;
    return 0;
}

int netdev_running(const struct netdev *nd)
{
    (void)nd;
    return 1;
}

void netlink_nd_useropt(const struct netdev *nd, const uint8_t *src, const uint8_t *opt,
                        uint32_t len)
{
    (void)nd;
    (void)src;
    (void)opt;
    (void)len;
}

int route6_at(int n, struct route6_row *out)
{
    (void)n;
    (void)out;
    return -1;
}

void route6_forget(struct netdev *nd, int proto)
{
    (void)nd;
    (void)proto;
}

void route6_learn(struct netdev *nd, const uint8_t *dst, int plen, const uint8_t *gw,
                  uint32_t metric, uint32_t lifetime)
{
    (void)nd;
    (void)dst;
    (void)plen;
    (void)gw;
    (void)metric;
    (void)lifetime;
}

void route6_ra_metric(struct netdev *nd, uint32_t metric)
{
    (void)nd;
    (void)metric;
}

void route6_tick(void)
{}

uint64_t tick_count(void)
{
    return kernel_ticks;
}

uint64_t tick_div(uint64_t n, uint32_t d, uint32_t *rem)
{
    if (rem) {
        *rem = (uint32_t)(n % d);
    }
    return n / d;
}

/** Forget what the last row recorded. */
static void reset_record(void)
{
    frames = solicits = unreachables = 0;
    k_memset(whole, 0, sizeof(whole));
    whole_bytes = whole_id = 0;
    ids_agree = saw_last = 0;
}

/**
 * Send 6000 bytes of UDP from `src` to the cold neighbour `hop` and check
 * that nothing leaves but one solicitation; answer it, and check that the
 * five fragments then leave and make up the whole payload, once.
 */
static void cold_send_row(void)
{
    static const uint8_t src[16] = {0xfe, 0x80, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x0f};
    static const uint8_t hop[16] = {0xfe, 0x80, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x02};
    static const uint8_t mac[6] = {0x52, 0x55, 0x0a, 0x00, 0x02, 0x02};
    static uint8_t payload[6000];
    uint32_t i;
    int r;

    for (i = 0; i < sizeof(payload); i++) {
        payload[i] = (uint8_t)(i * 13u + i / 253u);
    }
    reset_record();
    r = frag6_output(&card, hop, src, hop, IP6_NH_UDP, payload, (uint16_t)sizeof(payload), 64, 0,
                     card.mtu);
    CHECK(r == 0, "a send of 6000 bytes to a cold neighbour is taken");
    CHECK(frames == 0 && solicits == 1, "and only a solicitation leaves");
    learn(&card, hop, mac, -1);
    CHECK(frames == 5, "the answer sends it in five fragments");
    CHECK(whole_bytes == sizeof(payload) && ids_agree && saw_last &&
              memcmp(whole, payload, sizeof(payload)) == 0,
          "which carry the whole datagram under one identification, the last without M");
    learn(&card, hop, mac, -1);
    CHECK(frames == 5, "and only once");

    reset_record();
    r = frag6_output(&card, hop, src, hop, IP6_NH_UDP, payload, (uint16_t)sizeof(payload), 64, 0,
                     card.mtu);
    CHECK(r == 0 && frames == 5 && whole_bytes == sizeof(payload) && solicits == 0,
          "a send to the neighbour now known fragments at once");
}

/**
 * Send 6000 bytes to a second cold neighbour that never answers, and check
 * that after three solicitations the one held datagram is answered with one
 * address unreachable, and its memory freed.
 */
static void failed_row(void)
{
    static const uint8_t src[16] = {0xfe, 0x80, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x0f};
    static const uint8_t hop[16] = {0xfe, 0x80, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x03};
    static uint8_t payload[6000];
    int i;

    reset_record();
    frag6_output(&card, hop, src, hop, IP6_NH_UDP, payload, (uint16_t)sizeof(payload), 64, 0,
                 card.mtu);
    for (i = 0; i < 8; i++) {
        kernel_ticks += TICK_HZ;
        nd6_tick();
    }
    CHECK(frames == 0 && unreachables == 1,
          "a neighbour that never answers gets the held datagram one address unreachable");
}

int main(void)
{
    card.index = 1;
    card.mtu = 1500;
    card.up = 1;
    kernel_ticks = 1000;
    cold_send_row();
    failed_row();
    if (failures == 0) {
        printf("nd6hold_hosttest: PASS\n");
    }
    return failures != 0;
}
