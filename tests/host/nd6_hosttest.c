/* Check the metric of the IPv6 default routes routers advertise, per card.
 * host-deps: rach2/sys/network/ip6/route6.c rach2/sys/network/ip6/addr6.c
 * host-deps: rach2/sys/lib/kstring.c rach2/sys/lib/kctypes.c
 * host-deps: rach2/sys/lib/ksha256.c
 * host-deps: tests/host/compat/kheap_host.c
 */
#include "../../rach2/sys/network/ip6/nd6.c"

extern int printf(const char *format, ...);

volatile uint64_t kernel_ticks;
struct net_mib6 net_mib6;
static struct netdev lo, eth0, wlan0;
static int failures, lock_depth;

#define CHECK(x)                                                             \
    do {                                                                     \
        if (!(x)) {                                                          \
            printf("nd6-test: fail: line %u: %s\n", (unsigned)__LINE__, #x); \
            ++failures;                                                      \
        }                                                                    \
    } while (0)

/** Count nested network exclusion. */
uint32_t net_lock(void)
{
    return (uint32_t)lock_depth++;
}

/** Restore the preceding exclusion depth. */
void net_unlock(uint32_t flags)
{
    CHECK(lock_depth == (int)flags + 1);
    lock_depth = (int)flags;
}

/** The model's two cards, eth0 and wlan0, in that order. */
int netdev_count(void)
{
    return 2;
}

/** The cards the namespace lists. */
int netdev_listed(void)
{
    return 2;
}

/** The card `n`, or NULL past the two. */
struct netdev *netdev_at(int n)
{
    return n == 0 ? &eth0 : n == 1 ? &wlan0 : 0;
}

/** The first card. */
struct netdev *netdev_first(void)
{
    return &eth0;
}

/** The model's `lo`. */
struct netdev *netdev_loopback(void)
{
    return &lo;
}

/** Every card of the model has its link. */
int netdev_running(const struct netdev *nd)
{
    (void)nd;
    return 1;
}

/** No card holds an address in this model. */
struct netdev_addr6 *netdev_addr6_of(struct netdev *nd, const uint8_t *addr)
{
    (void)nd;
    (void)addr;
    return 0;
}

/** No card holds an address in this model. */
int netdev_holds6(struct netdev *nd, const uint8_t *addr)
{
    (void)nd;
    (void)addr;
    return 0;
}

/** No interface holds an address in this model. */
struct netdev *netdev_owner6(const uint8_t *addr)
{
    (void)addr;
    return 0;
}

/** Groups are taken as joined. */
int netdev_join6(struct netdev *nd, const uint8_t *group)
{
    (void)nd;
    (void)group;
    return 0;
}

/** Groups are taken as left. */
void netdev_leave6(struct netdev *nd, const uint8_t *group)
{
    (void)nd;
    (void)group;
}

/** Every card is in every group. */
int netdev_member6(const struct netdev *nd, const uint8_t *group)
{
    (void)nd;
    (void)group;
    return 1;
}

/** Addresses are not made in this model. */
int netconf_add_address6(struct netdev *nd, const uint8_t *addr, int prefix, int flags,
                         uint32_t valid, uint32_t preferred)
{
    (void)nd;
    (void)addr;
    (void)prefix;
    (void)flags;
    (void)valid;
    (void)preferred;
    return 0;
}

/** Remove the address whose slot the ND state machine is visiting. */
int netconf_del_address6(struct netdev *nd, const uint8_t *addr)
{
    int i;
    for (i = 0; i < nd->naddr6; i++) {
        if (ip6_equal(nd->addr6[i].addr, addr)) {
            route6_address_removed(nd, i);
            for (; i + 1 < nd->naddr6; i++)
                nd->addr6[i] = nd->addr6[i + 1];
            nd->naddr6--;
            return 0;
        }
    }
    return -EADDRNOTAVAIL;
}

/** Cards are not brought up again in this model. */
int netconf_set_up(struct netdev *nd, int up)
{
    (void)nd;
    (void)up;
    return 0;
}

/** Frames are dropped. */
int ethernet_tx(struct netdev *out, const uint8_t *dst_mac, uint16_t ethertype,
                const uint8_t *payload, uint16_t len)
{
    (void)out;
    (void)dst_mac;
    (void)ethertype;
    (void)payload;
    (void)len;
    return 0;
}

/** Messages are dropped. */
int icmp6_send_opts(const uint8_t *src, const uint8_t *dst, int ifindex, uint8_t *msg, uint16_t len,
                    uint8_t hlim, int opts)
{
    (void)src;
    (void)dst;
    (void)ifindex;
    (void)msg;
    (void)len;
    (void)hlim;
    (void)opts;
    return 0;
}

/** Fragments are dropped: no row sends past the path MTU. */
int frag6_output(struct netdev *out, const uint8_t *via, const uint8_t *src, const uint8_t *dst,
                 uint8_t nh, const uint8_t *payload, uint16_t plen, uint8_t hlim, uint8_t tclass,
                 uint32_t mtu)
{
    (void)out;
    (void)via;
    (void)src;
    (void)dst;
    (void)nh;
    (void)payload;
    (void)plen;
    (void)hlim;
    (void)tclass;
    (void)mtu;
    return 0;
}

/** Nothing waits on an answer in this model. */
void icmp6_unreach_local(const uint8_t *dgram, uint16_t len)
{
    (void)dgram;
    (void)len;
}

/** No source is chosen in this model. */
int ip6_source(const struct netdev *out, const uint8_t *dst, uint8_t *src)
{
    (void)out;
    (void)dst;
    (void)src;
    return -1;
}

/** MLD has nothing to send. */
void mld6_tick(void)
{}

/** The options of advertisements are not reported. */
void netlink_nd_useropt(const struct netdev *nd, const uint8_t *src, const uint8_t *opt,
                        uint32_t len)
{
    (void)nd;
    (void)src;
    (void)opt;
    (void)len;
}

/** The log is dropped. */
void klog(int level, const char *subsystem, const char *format, ...)
{
    (void)level;
    (void)subsystem;
    (void)format;
}

/** A fixed pattern stands for random bytes. */
void krandom_bytes(void *buf, uint32_t n)
{
    uint8_t *p = (uint8_t *)buf;
    uint32_t i;

    for (i = 0; i < n; i++) {
        p[i] = (uint8_t)(i * 37u + 11u);
    }
}

/** The tick count is the model's clock. */
uint64_t tick_count(void)
{
    return kernel_ticks;
}

/** Divide as the kernel does. */
uint64_t tick_div(uint64_t n, uint32_t d, uint32_t *rem)
{
    if (rem) {
        *rem = (uint32_t)(n % d);
    }
    return n / d;
}

/** Read a big-endian 16-bit field. */
uint16_t read16be(const uint8_t *p)
{
    return (uint16_t)(p[0] << 8 | p[1]);
}

/** Read a big-endian 32-bit field. */
uint32_t read32be(const uint8_t *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

/** Make a card of index `index` named `name`, up. */
static void card(struct netdev *nd, int index, const char *name)
{
    k_memset(nd, 0, sizeof(*nd));
    k_memcpy(nd->name, name, k_strlen(name) + 1);
    nd->index = index;
    nd->up = 1;
    nd->mtu = 1500;
    nd->mac[0] = 0x52;
    nd->mac[5] = (uint8_t)index;
}

/** A router `last` on the link, fe80::LAST. */
static void router(uint8_t *addr, uint8_t last)
{
    k_memset(addr, 0, IP6_ADDR_LEN);
    addr[0] = 0xfe;
    addr[1] = 0x80;
    addr[15] = last;
}

/** Give `nd` an advertisement with no options from `src`, of router
 * lifetime `lifetime` seconds. */
static void advertise(struct netdev *nd, const uint8_t *src, uint16_t lifetime)
{
    uint8_t msg[16];

    k_memset(msg, 0, sizeof(msg));
    msg[0] = ND6_ROUTER_ADVERT;
    msg[4] = 64;
    msg[6] = (uint8_t)(lifetime >> 8);
    msg[7] = (uint8_t)lifetime;
    rx_ra(nd, if_of(nd), src, msg, sizeof(msg));
}

/** The count of default routes routers gave `nd`; the metric, gateway and
 * seconds left of the last in `*metric`, `gw` and `*expires`. */
static int defaults(const struct netdev *nd, uint32_t *metric, uint8_t *gw, uint32_t *expires)
{
    struct route6_row r;
    int i, n = 0;

    for (i = 0; route6_at(i, &r); i++) {
        if (r.ifindex == nd->index && r.proto == ROUTE6_PROTO_RA && r.plen == 0) {
            *metric = r.metric;
            k_memcpy(gw, r.gw, IP6_ADDR_LEN);
            *expires = r.expires;
            n++;
        }
    }
    return n;
}

/** Count exact address-generated routes in the selected table. */
static int count_prefix(const uint8_t *dst, unsigned plen, int local, unsigned metric)
{
    struct route6_row row;
    int i, count = 0;
    for (i = 0; route6_at(i, &row); i++)
        if (row.local == local && row.proto == ROUTE6_PROTO_KERNEL && row.plen == plen &&
            row.metric == metric && ip6_equal(dst, row.dst))
            count++;
    return count;
}

/** Check routes after address removal, carrier loss and device shutdown. */
static void route_lifetime(void)
{
    struct route6_row r = {0};
    struct netdev_addr6 *a = &eth0.addr6[0];
    uint8_t via[16], dst[16] = {0x20, 0x01}, other[16] = {0x30, 0x01};
    a->addr[0] = 0x40;
    a->peer[0] = 0x50;
    a->prefix = 128;
    a->metric = 80;
    a->valid = NETDEV_LIFETIME_FOREVER;
    a->flags = NETDEV_A6_TENTATIVE;
    eth0.naddr6 = 1;
    CHECK(count_prefix(a->addr, 128, 0, 80) == 1 && count_prefix(a->addr, 128, 1, 0) == 0);
    CHECK(count_prefix(a->peer, 128, 0, 80) == 0);
    a->flags = NETDEV_A6_NODAD | NETDEV_A6_PERMANENT;
    CHECK(count_prefix(a->addr, 128, 1, 0) == 1 && count_prefix(a->peer, 128, 0, 80) == 1);
    a->noprefix = 1;
    CHECK(count_prefix(a->addr, 128, 0, 80) == 0 && count_prefix(a->peer, 128, 0, 80) == 1);
    a->noprefix = 0;
    r.plen = 128;
    k_memcpy(r.dst, a->addr, 16);
    r.proto = 9;
    CHECK(route6_change_row(&eth0, &r, 1, 0) == -ESRCH);
    r.proto = 2;
    r.metric = 81;
    CHECK(route6_change_row(&eth0, &r, 1, 0) == -ESRCH);
    r.metric = 80;
    CHECK(route6_change_row(&eth0, &r, 1, 0) == 0 && count_prefix(a->addr, 128, 0, 80) == 0);
    CHECK(count_prefix(a->addr, 128, 1, 0) == 1);
    k_memcpy(r.dst, dst, 16);
    r.proto = 99;
    k_memcpy(r.prefsrc, a->addr, 16);
    CHECK(route6_change_row(&eth0, &r, 0, 0) == 0);
    CHECK(route6_preferred_source(&eth0, dst, via) && ip6_equal(via, a->addr));
    r.gw[0] = 0xff;
    CHECK(route6_change_row(&eth0, &r, 0, 1) == -EINVAL);
    CHECK(route6_lookup(dst, eth0.index, via) == &eth0 && ip6_equal(dst, via));
    r.gw[0] = 0;
    k_memcpy(r.dst, other, 16);
    CHECK(route6_change_row(&wlan0, &r, 0, 0) == 0);
    CHECK(netconf_del_address6(&eth0, a->addr) == 0 && count_prefix(a->peer, 128, 0, 80) == 0);
    CHECK(route6_lookup(dst, eth0.index, via) == &eth0);
    {
        struct route6_row got;
        CHECK(route6_resolve(dst, eth0.index, &got) == &eth0 && ip6_is_unspecified(got.prefsrc));
    }
    eth0.naddr6 = 1;
    forget_if(&eth0, 0);
    CHECK(eth0.naddr6 == 1 && route6_lookup(dst, eth0.index, via) == &eth0);
    nd6_if_down(&eth0);
    CHECK(eth0.naddr6 == 0 && route6_lookup(dst, eth0.index, via) == 0);
    CHECK(route6_lookup(other, wlan0.index, via) == &wlan0);
    CHECK(route6_change_row(&wlan0, &r, 1, 0) == 0);
}

/** A finite manual prefix keeps its expiry after its address is removed. */
static void finite_prefix(void)
{
    struct netdev_addr6 *a = &eth0.addr6[0];
    struct route6_row r = {0};
    uint8_t via[16], dst[16] = {0x20, 0x04};
    k_memset(a, 0, sizeof(*a));
    k_memcpy(a->addr, dst, 16);
    a->addr[15] = 1;
    a->prefix = 64;
    a->valid = 10;
    a->since = kernel_ticks;
    {
        struct route6_row used = {0};
        int i;
        used.dst[0] = 0x60;
        used.plen = 128;
        for (i = 0; i < ROUTE6_MAX; i++) {
            used.dst[15] = (uint8_t)i;
            CHECK(route6_change_row(&wlan0, &used, 0, 0) == 0);
        }
    }
    eth0.naddr6 = 1;
    CHECK(netconf_del_address6(&eth0, a->addr) == 0);
    CHECK(route6_lookup(dst, eth0.index, via) == &eth0);
    kernel_ticks += 10 * TICK_HZ;
    route6_tick();
    CHECK(!route6_lookup(dst, eth0.index, via));
    nd6_if_down(&wlan0);
    r.local = 1;
    r.proto = 99;
    r.plen = 64;
    k_memcpy(r.dst, dst, 16);
    CHECK(route6_change_row(&eth0, &r, 0, 0) == 0 && route6_added_local(dst) == &eth0);
    CHECK(!route6_lookup(dst, eth0.index, via));
    nd6_if_down(&eth0);
    CHECK(!route6_added_local(dst));
}

/** Changing a ready /64 peer updates its prefix and keeps the old /128. */
static void change_peer(void)
{
    struct netdev_addr6 *a = &eth0.addr6[0];
    uint8_t old[16] = {0x20, 1}, peer[16] = {0x30, 1};
    k_memset(a, 0, sizeof(*a));
    a->addr[0] = 0x40;
    a->prefix = 64;
    a->flags = NETDEV_A6_PERMANENT;
    a->valid = NETDEV_LIFETIME_FOREVER;
    k_memcpy(a->peer, old, 16);
    eth0.naddr6 = 1;
    route6_peer_change(&eth0, a, peer);
    a->peer_prefix = 1;
    CHECK(count_prefix(old, 128, 0, 256) == 1 && count_prefix(peer, 64, 0, 256) == 1);
    CHECK(count_prefix(peer, 128, 0, 256) == 0);
    nd6_if_down(&eth0);
    CHECK(count_prefix(old, 128, 0, 256) == 0);
}

int main(void)
{
    static const uint8_t dst[IP6_ADDR_LEN] = {0x20, 0x01, 0x0d, 0xb8, 0, 0, 0, 0,
                                              0,    0,    0,    0,    0, 0, 0, 1};
    uint8_t ra[IP6_ADDR_LEN], rb[IP6_ADDR_LEN], gw[IP6_ADDR_LEN], via[IP6_ADDR_LEN];
    uint32_t metric = 0, expires = 0;
    int32_t v = 0;

    card(&lo, 1, "lo");
    lo.loopback = 1;
    card(&eth0, 2, "eth0");
    card(&wlan0, 3, "wlan0");
    router(ra, 1);
    router(rb, 2);
    nd6_init();

    /* Linux's default and range. */
    CHECK(nd6_conf_get(ND6_CONF_DEFAULT, ND6_RA_DEFRTR_METRIC, &v) == 0 && v == 1024);
    CHECK(nd6_conf_get(eth0.index, ND6_RA_DEFRTR_METRIC, &v) == 0 && v == 1024);
    CHECK(nd6_conf_set(eth0.index, ND6_RA_DEFRTR_METRIC, 0) == -EINVAL);
    /* The internal int32_t carries an unsigned metric in its bits. */
    CHECK(nd6_conf_set(eth0.index, ND6_RA_DEFRTR_METRIC, -5) == 0);
    CHECK(nd6_conf_get(eth0.index, ND6_RA_DEFRTR_METRIC, &v) == 0 && (uint32_t)v == 0xFFFFFFFBu);
    CHECK(nd6_conf_set(eth0.index, ND6_RA_DEFRTR_METRIC, -1) == 0);
    CHECK(nd6_conf_get(eth0.index, ND6_RA_DEFRTR_METRIC, &v) == 0 && (uint32_t)v == 0xFFFFFFFFu);
    CHECK(nd6_conf_set(eth0.index, ND6_RA_DEFRTR_METRIC, 1) == 0);
    CHECK(nd6_conf_get(eth0.index, ND6_RA_DEFRTR_METRIC, &v) == 0 && v == 1);
    CHECK(nd6_conf_set(9, ND6_RA_DEFRTR_METRIC, 100) == -ENODEV);

    /* The cable at 100 and Wi-Fi at 600: each router's default takes its
     * card's metric, and the cable's is chosen. */
    CHECK(nd6_conf_set(eth0.index, ND6_RA_DEFRTR_METRIC, 100) == 0);
    CHECK(nd6_conf_set(wlan0.index, ND6_RA_DEFRTR_METRIC, 600) == 0);
    CHECK(nd6_conf_get(wlan0.index, ND6_RA_DEFRTR_METRIC, &v) == 0 && v == 600);
    advertise(&eth0, ra, 1800);
    advertise(&wlan0, rb, 1800);
    CHECK(defaults(&eth0, &metric, gw, &expires) == 1 && metric == 100 && ip6_equal(gw, ra));
    CHECK(defaults(&wlan0, &metric, gw, &expires) == 1 && metric == 600 && ip6_equal(gw, rb));
    CHECK(route6_lookup(dst, 0, via) == &eth0 && ip6_equal(via, ra));

    /* A new metric moves the route already learned, keeping its time, and
     * the next advertisement refreshes it there. */
    kernel_ticks = 10 * TICK_HZ;
    CHECK(nd6_conf_set(wlan0.index, ND6_RA_DEFRTR_METRIC, 50) == 0);
    CHECK(defaults(&wlan0, &metric, gw, &expires) == 1 && metric == 50 && expires == 1790);
    CHECK(route6_lookup(dst, 0, via) == &wlan0 && ip6_equal(via, rb));
    advertise(&wlan0, rb, 1800);
    CHECK(defaults(&wlan0, &metric, gw, &expires) == 1 && metric == 50 && expires == 1800);

    /* A cable unplugged in the advertisement's terms, lifetime 0, leaves
     * Wi-Fi's default; one back at 600 loses to the cable again. */
    CHECK(nd6_conf_set(wlan0.index, ND6_RA_DEFRTR_METRIC, 600) == 0);
    advertise(&eth0, ra, 0);
    CHECK(defaults(&eth0, &metric, gw, &expires) == 0);
    CHECK(route6_lookup(dst, 0, via) == &wlan0);
    advertise(&eth0, ra, 1800);
    CHECK(route6_lookup(dst, 0, via) == &eth0);
    advertise(&wlan0, rb, 0);
    CHECK(defaults(&wlan0, &metric, gw, &expires) == 0);

    /* A route of the same router already at the new metric absorbs the
     * moved one and keeps the later end. */
    route6_learn(&eth0, ip6_any, 0, ra, 1024, 60);
    CHECK(nd6_conf_set(eth0.index, ND6_RA_DEFRTR_METRIC, 1024) == 0);
    CHECK(defaults(&eth0, &metric, gw, &expires) == 1 && metric == 1024 && expires == 1800);

    /* The default is what a new card starts with; another card's setting
     * is left alone. */
    CHECK(nd6_conf_set(ND6_CONF_DEFAULT, ND6_RA_DEFRTR_METRIC, 700) == 0);
    CHECK(nd6_conf_get(wlan0.index, ND6_RA_DEFRTR_METRIC, &v) == 0 && v == 600);
    route_lifetime();
    finite_prefix();
    change_peer();
    CHECK(lock_depth == 0);

    if (failures) {
        printf("nd6-test: result: %d failed\n", failures);
        return 1;
    }
    printf("nd6-test: result: passed\n");
    return 0;
}
