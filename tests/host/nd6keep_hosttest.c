/* Check keep_addr_on_down through the real address and group code: the
 * addresses a card keeps, its solicited-node groups and the heap it holds.
 * host-deps: rach2/sys/network/netconf.c rach2/sys/network/route.c
 * host-deps: rach2/sys/network/driver/netdev_room.c
 * host-deps: rach2/sys/network/ip6/route6.c rach2/sys/network/ip6/addr6.c
 * host-deps: rach2/sys/cmdline.c rach2/sys/tick.c rach2/sys/lib/kstd.c
 * host-deps: rach2/sys/lib/kstring.c rach2/sys/lib/kctypes.c
 * host-deps: rach2/sys/lib/ksha256.c
 * host-deps: tests/host/compat/kheap_host.c
 *
 * netconf.c inserts and deletes the addresses and netdev_room.c holds them
 * and the groups, as on the machine, so that a down, an up and a removal
 * that left an address block, a group's reference or a route behind shows
 * here as a count that does not return.
 */
#include "../../rach2/sys/network/ip6/nd6.c"

extern int printf(const char *format, ...);

extern long kheap_host_blocks;

volatile uint64_t kernel_ticks;
struct net_mib6 net_mib6;
static struct netdev lo, eth0;
static int failures, lock_depth;

#define CHECK(x)                                                                  \
    do {                                                                          \
        if (!(x)) {                                                               \
            printf("nd6-keep-test: fail: line %u: %s\n", (unsigned)__LINE__, #x); \
            ++failures;                                                           \
        }                                                                         \
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

/** The model's one card, eth0. */
int netdev_count(void)
{
    return 1;
}

/** The cards the namespace lists. */
int netdev_listed(void)
{
    return 1;
}

/** The card `n`, or NULL past the one. */
struct netdev *netdev_at(int n)
{
    return n == 0 ? &eth0 : 0;
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

/** The address `addr` of `nd`, as netdev.c finds it. */
struct netdev_addr6 *netdev_addr6_of(struct netdev *nd, const uint8_t *addr)
{
    int i;

    for (i = 0; nd && i < nd->naddr6; i++) {
        if (ip6_equal(nd->addr6[i].addr, addr)) {
            return &nd->addr6[i];
        }
    }
    return 0;
}

/** Whether `nd` holds `addr` ready, as netdev.c answers it. */
int netdev_holds6(struct netdev *nd, const uint8_t *addr)
{
    struct netdev_addr6 *a = netdev_addr6_of(nd, addr);

    return a && !(a->flags & (NETDEV_A6_TENTATIVE | NETDEV_A6_DADFAILED));
}

/** No datagram is addressed to the machine in this model. */
struct netdev *netdev_owner6(const uint8_t *addr)
{
    (void)addr;
    return 0;
}

/** No IPv4 address is the machine's in this model. */
int netdev_owns(uint32_t ip)
{
    (void)ip;
    return 0;
}

/** No IPv4 address is a broadcast in this model. */
int netdev_is_broadcast(uint32_t ip)
{
    (void)ip;
    return 0;
}

/** No IPv4 source is chosen in this model. */
uint32_t netdev_source(const struct netdev *nd, uint32_t hop, int scope)
{
    (void)nd;
    (void)hop;
    (void)scope;
    return 0;
}

/** A change of a card's state forgets its carrier: nothing to do. */
void netdev_carrier_changed(struct netdev *nd)
{
    (void)nd;
}

/** No IPv4 neighbour is known in this model. */
void arp_flush(void)
{}

/** No IPv4 neighbour is known in this model. */
void arp_forget_dev(struct netdev *nd)
{
    (void)nd;
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

/** Make the card `nd` of index `index` named `name`, down and with no
 * address, as a driver registers it. */
static void card(struct netdev *nd, int index, const char *name)
{
    k_memset(nd, 0, sizeof(*nd));
    k_memcpy(nd->name, name, k_strlen(name) + 1);
    nd->index = index;
    nd->mtu = 1500;
    nd->mac[0] = 0x52;
    nd->mac[5] = (uint8_t)index;
}

/** The joins that hold the solicited-node group of `addr` on eth0, 0 when
 * eth0 is not a member. */
static uint32_t solicited_refs(const uint8_t *addr)
{
    uint8_t group[IP6_ADDR_LEN];
    int i;

    ip6_solicited_node(addr, group);
    for (i = 0; i < eth0.ngroup6; i++) {
        if (ip6_equal(eth0.group6[i].addr, group)) {
            return eth0.group6[i].refs;
        }
    }
    return 0;
}

/** Count the kernel's routes on eth0 of `plen` bits that cover `dst`, in
 * the local table when `local` is set. */
static int count_prefix(const uint8_t *dst, unsigned plen, int local)
{
    struct route6_row row;
    int i, count = 0;

    for (i = 0; route6_at(i, &row); i++) {
        if (row.ifindex == eth0.index && row.local == local && row.proto == ROUTE6_PROTO_KERNEL &&
            row.plen == plen && ip6_prefix_match(dst, row.dst, (int)plen)) {
            count++;
        }
    }
    return count;
}

/** Give eth0 `addr`/64 with `flags` through netconf, as `ip addr add`
 * does; 0 or its error. */
static int add(const uint8_t *addr, int flags)
{
    return netconf_add_address6(&eth0, addr, 64, flags, NETDEV_LIFETIME_FOREVER,
                                NETDEV_LIFETIME_FOREVER);
}

/** Run eth0's duplicate detection to its end: eight seconds of its timers,
 * past the first probe's delay and the probe's wait. */
static void settle(void)
{
    int i;

    for (i = 0; i < 8; i++) {
        kernel_ticks += TICK_HZ;
        dad_timers(&eth0);
    }
}

/** Take eth0 away as netdev_withdraw does: down, then neighbour discovery's
 * removal, then the arrays freed. Checks that no address and no
 * solicited-node group outlived the removal. */
static void withdraw(void)
{
    CHECK(netconf_set_up(&eth0, 0) == 0);
    nd6_if_remove(&eth0);
    CHECK(eth0.naddr6 == 0 && eth0.addr6 == 0 && eth0.room6 == 0);
    CHECK(eth0.ngroup6 == 1 && netdev_member6(&eth0, ip6_all_nodes));
    netdev_addr_free(&eth0);
    CHECK(eth0.ngroup6 == 0 && eth0.group6 == 0);
}

/** Join or leave, with `join`, the solicited-node group of `addr` on eth0
 * as a socket's IPV6_JOIN_GROUP does, apart from the address's own join. */
static void socket_join(const uint8_t *addr, int join)
{
    uint8_t group[IP6_ADDR_LEN];

    ip6_solicited_node(addr, group);
    if (join) {
        CHECK(netdev_join6(&eth0, group) == 0);
    } else {
        netdev_leave6(&eth0, group);
    }
}

/** Take eth0, up with its link-local `ll` and kept `global`, down and up
 * three times while a socket holds a join of global's solicited-node
 * group: each down leaves the socket's join alone and each up adds the
 * address's one join back, and no cycle holds another heap block. */
static void cycle_kept(const uint8_t *global, const uint8_t *ll)
{
    struct netdev_addr6 *a;
    long held;
    int i;

    socket_join(global, 1);
    held = kheap_host_blocks;
    for (i = 0; i < 3; i++) {
        a = netdev_addr6_of(&eth0, global);
        CHECK(eth0.naddr6 == 2 && a && !(a->flags & NETDEV_A6_TENTATIVE));
        CHECK(solicited_refs(global) == 2 && solicited_refs(ll) == 1 && eth0.ngroup6 == 3);
        CHECK(count_prefix(global, 64, 0) == 1 && count_prefix(global, 128, 1) == 1);
        CHECK(netconf_set_up(&eth0, 0) == 0 && eth0.naddr6 == 1);
        CHECK(solicited_refs(global) == 1 && solicited_refs(ll) == 0 && eth0.ngroup6 == 2);
        CHECK(netconf_set_up(&eth0, 1) == 0);
        settle();
        CHECK(kheap_host_blocks == held);
    }
    socket_join(global, 0);
    CHECK(solicited_refs(global) == 1);
}

/**
 * keep_addr_on_down, as Linux's addrconf_ifdown reads it: with it above 0
 * on the card, and `all` 0, a card taken down keeps its permanent global
 * address, tentative again and out of its solicited-node group, and
 * deletes its link-local and autoconfigured ones and every route; up joins
 * the group again, once, and gives the kept one its routes back. A
 * socket's join of the same group holds across both. A negative `all`
 * wins over the card's; an address given to a card that is down joins
 * once and no more at up; and the card's removal frees every block it
 * took.
 */
static void keep_addr_on_down(void)
{
    static const uint8_t global[16] = {0x20, 0x01, 0x0d, 0xb8, 0, 7, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1};
    static const uint8_t auto_[16] = {0x20, 0x01, 0x0d, 0xb8, 0, 8, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2};
    long blocks = kheap_host_blocks;
    struct netdev_addr6 *a;
    uint8_t ll[16];
    int32_t v = 0;

    CHECK(nd6_conf_get(eth0.index, ND6_KEEP_ADDR_ON_DOWN, &v) == 0 && v == 0);
    CHECK(nd6_conf_set(eth0.index, ND6_KEEP_ADDR_ON_DOWN, 1) == 0);
    CHECK(netconf_set_up(&eth0, 1) == 0);
    CHECK(eth0.naddr6 == 1 && ip6_is_linklocal(eth0.addr6[0].addr));
    k_memcpy(ll, eth0.addr6[0].addr, sizeof(ll));
    CHECK(add(global, NETDEV_A6_PERMANENT) == 0 && add(auto_, 0) == 0);
    CHECK(add(global, NETDEV_A6_PERMANENT) == -EEXIST);
    settle();
    /* The link-local ...:2 and auto_ ::2 share ff02::1:ff00:2. */
    CHECK(eth0.naddr6 == 3 && eth0.ngroup6 == 3 && netdev_member6(&eth0, ip6_all_nodes));
    CHECK(solicited_refs(ll) == 2 && solicited_refs(global) == 1);
    CHECK(count_prefix(global, 64, 0) == 1 && count_prefix(global, 128, 1) == 1);

    CHECK(netconf_set_up(&eth0, 0) == 0);
    a = netdev_addr6_of(&eth0, global);
    CHECK(eth0.naddr6 == 1 && a == &eth0.addr6[0] && (a->flags & NETDEV_A6_TENTATIVE));
    CHECK(solicited_refs(ll) == 0 && solicited_refs(global) == 0 && eth0.ngroup6 == 1);
    CHECK(count_prefix(global, 64, 0) == 0 && count_prefix(global, 128, 1) == 0);

    CHECK(netconf_set_up(&eth0, 1) == 0);
    CHECK(solicited_refs(global) == 1);
    settle();
    cycle_kept(global, ll);

    CHECK(nd6_conf_set(ND6_CONF_ALL, ND6_KEEP_ADDR_ON_DOWN, -1) == 0);
    CHECK(netconf_set_up(&eth0, 0) == 0);
    CHECK(eth0.naddr6 == 0 && eth0.addr6 == 0 && eth0.room6 == 0);
    CHECK(solicited_refs(global) == 0 && eth0.ngroup6 == 1);
    CHECK(nd6_conf_set(ND6_CONF_ALL, ND6_KEEP_ADDR_ON_DOWN, 0) == 0);

    CHECK(add(global, NETDEV_A6_PERMANENT) == 0 && solicited_refs(global) == 1);
    CHECK(netconf_set_up(&eth0, 1) == 0);
    CHECK(solicited_refs(global) == 1 && solicited_refs(ll) == 1 && eth0.ngroup6 == 3);
    withdraw();
    CHECK(count_prefix(global, 64, 0) == 0 && kheap_host_blocks == blocks);
    CHECK(nd6_conf_set(eth0.index, ND6_KEEP_ADDR_ON_DOWN, 0) == 0);
}

/** A retained address is ready at once on TUN (`l3`) or with NODAD. An
 * Ethernet address that failed DAD probes again and wins its host route
 * back. On Ethernet the address holds its solicited-node group while the
 * card is up and not while it is down; on TUN it never does, and the card
 * holds only ff02::1. Each case ends with the card's removal and its
 * blocks freed. */
static void retained_dad(int l3, int nodad)
{
    static const uint8_t global[16] = {0x20, 0x01, 0x0d, 0xb8, 0, 9, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1};
    long blocks = kheap_host_blocks;
    struct netdev_addr6 *a;
    struct route6_row r = {0};

    eth0.l3 = l3;
    CHECK(nd6_conf_set(eth0.index, ND6_KEEP_ADDR_ON_DOWN, 1) == 0);
    CHECK(netconf_set_up(&eth0, 1) == 0);
    CHECK(add(global, NETDEV_A6_PERMANENT | (nodad ? NETDEV_A6_NODAD : 0)) == 0);
    settle();
    a = netdev_addr6_of(&eth0, global);
    CHECK(a && !(a->flags & NETDEV_A6_TENTATIVE) && count_prefix(global, 128, 1) == 1);
    CHECK(solicited_refs(global) == (l3 ? 0u : 1u) && eth0.ngroup6 == (l3 ? 1 : 3));
    if (nodad) {
        r.plen = 128;
        r.local = 1;
        k_memcpy(r.dst, global, sizeof(r.dst));
        CHECK(route6_change_row(&eth0, &r, 1, 0) == 0 && a->local_route_deleted);
    } else if (!l3) {
        a->flags |= NETDEV_A6_TENTATIVE;
        dad_failed(&eth0, a);
        CHECK(a->flags & NETDEV_A6_DADFAILED);
    }

    CHECK(netconf_set_up(&eth0, 0) == 0);
    CHECK(eth0.naddr6 == 1 && solicited_refs(global) == 0 && eth0.ngroup6 == 1);
    CHECK(netconf_set_up(&eth0, 1) == 0);
    a = netdev_addr6_of(&eth0, global);
    CHECK(a != 0);
    if (a && !l3 && !nodad) {
        CHECK((a->flags & NETDEV_A6_TENTATIVE) && a->dad_left == 1);
        kernel_ticks = a->dad_at;
        dad_timers(&eth0);
        CHECK(a->dad_left == 0 && (a->flags & NETDEV_A6_TENTATIVE));
        kernel_ticks = a->dad_at;
        dad_timers(&eth0);
    }
    CHECK(a && !(a->flags & (NETDEV_A6_TENTATIVE | NETDEV_A6_DADFAILED)) &&
          count_prefix(global, 128, 1) == 1);
    CHECK(solicited_refs(global) == (l3 ? 0u : 1u) && eth0.ngroup6 == (l3 ? 1 : 3));
    withdraw();
    CHECK(count_prefix(global, 128, 1) == 0 && kheap_host_blocks == blocks);
    eth0.l3 = 0;
    CHECK(nd6_conf_set(eth0.index, ND6_KEEP_ADDR_ON_DOWN, 0) == 0);
}

int main(void)
{
    card(&lo, 1, "lo");
    lo.loopback = 1;
    card(&eth0, 2, "eth0");
    nd6_init();

    keep_addr_on_down();
    retained_dad(1, 0);
    retained_dad(0, 0);
    retained_dad(0, 1);
    CHECK(lock_depth == 0);

    if (failures) {
        printf("nd6-keep-test: result: %d failed\n", failures);
        return failures > 255 ? 255 : failures;
    }
    printf("nd6-keep-test: result: passed\n");
    return 0;
}
