/*
 * netconf_hosttest.c: parse and judge the address the boot command line
 * gives, and the setters of netconf.c that change it while the machine runs.
 *
 * host-deps: rach2/sys/network/netconf.c rach2/sys/cmdline.c
 * host-deps: rach2/sys/network/route.c
 * host-deps: rach2/sys/tick.c rach2/sys/lib/kstd.c
 * host-deps: rach2/sys/network/ip6/addr6.c
 * host-deps: rach2/sys/lib/kstring.c rach2/sys/lib/kctypes.c
 *
 * A wrong address here is not a crash; it is a machine that comes up on a
 * network nobody asked for, which on a remote board is indistinguishable from
 * a machine that did not come up at all. So the refusals matter more than the
 * acceptances, and most of what follows is text that must not be taken as an
 * address: the octal-looking, the out-of-range, the trailing-garbage, the
 * subnet's own network and broadcast, and a gateway that no host on the given
 * subnet could reach.
 *
 * The whole `netconf_apply` path is driven too, through the real command-line
 * capture, because the tuple rule (no options means the SLIRP tuple, an
 * address without a gateway means no gateway at all) is the part that decides
 * whether every existing test still boots into the network it expects.
 */

extern int printf(const char *fmt, ...);
extern int memcmp(const void *a, const void *b, __SIZE_TYPE__ n);

#include "../../rach2/sys/network/netconf.h"
#include "../../rach2/sys/network/net.h"
#include "../../rach2/sys/network/driver/netdev.h"
#include "../../rach2/sys/network/ip6/nd6.h"
#include "../../rach2/sys/network/ip6/route6.h"
#include "../../rach2/sys/cmdline.h"
#include "../../rach2/sys/lib/kerrno.h"
#include "../../rach2/sys/lib/kstring.h"
#include "../../rach2/sys/klog.h"

/* What an address change is supposed to cost, counted here. The kernel's own
   ARP cache is not linked into a host test, so this stands in for it and the
   cases below require it to have been called. */
static int arp_flushes;

void arp_flush(void);

/* The critical section the kernel takes against its receive interrupt. A host
   test has neither, so these stand in and count that the section was entered
   and left in balance, which is the property that matters here. */
static int lock_depth;
static int lock_unbalanced;

uint32_t net_lock(void);
void net_unlock(uint32_t flags);

/** Take the network lock: count it and return 0 as the saved flags. */
uint32_t net_lock(void)
{
    lock_depth++;
    return 0;
}

/** Release the network lock, noting a release without a lock. */
void net_unlock(uint32_t flags)
{
    (void)flags;
    lock_depth--;
    if (lock_depth < 0) {
        lock_unbalanced = 1;
    }
}

/** Count a flush of the whole ARP cache. */
void arp_flush(void)
{
    arp_flushes++;
}

static int fails;

#define CHECK(cond, what)                   \
    do {                                    \
        if (!(cond)) {                      \
            fails++;                        \
            printf("  FAIL: %s\n", (what)); \
        }                                   \
    } while (0)

/** The dotted quad `a.b.c.d` as the host-order number the stack has. */
#define IP4(a, b, c, d) \
    (((uint32_t)(a) << 24) | ((uint32_t)(b) << 16) | ((uint32_t)(c) << 8) | (uint32_t)(d))

/**
 * Return 1 when netconf_parse_cidr rejects `text` and leaves the caller's
 * values alone, since a caller that ignored the return would otherwise
 * configure whatever was half parsed; return 0 otherwise.
 */
static int cidr_refused(const char *text)
{
    uint32_t ip = 0xDEADBEEFu, mask = 0xDEADBEEFu;

    if (netconf_parse_cidr(text, &ip, &mask)) {
        return 0;
    }
    return ip == 0xDEADBEEFu && mask == 0xDEADBEEFu;
}

/**
 * Return 1 when netconf_parse_addr rejects `text` and leaves the caller's value
 * alone, and 0 otherwise.
 */
static int addr_refused(const char *text)
{
    uint32_t ip = 0xDEADBEEFu;

    if (netconf_parse_addr(text, &ip)) {
        return 0;
    }
    return ip == 0xDEADBEEFu;
}

/* The interfaces the configuration is written to. This test drives the
 * parsing and the setters, not the cards, so two records stand in for a
 * machine with two: the first, whose configuration is also the machine's,
 * and a second, which a call about it must not reach past. */
static struct netdev the_iface;
static struct netdev other_iface;

/** Report whether `ip` is the address of either interface. */
int netdev_owns(uint32_t ip)
{
    return netdev_holds(&the_iface, ip) || netdev_holds(&other_iface, ip);
}

/** Report whether `ip` is one of `nd`'s addresses, its first or another; 0
 * for an `ip` of 0. */
int netdev_holds(const struct netdev *nd, uint32_t ip)
{
    int i;

    for (i = 0; ip != 0 && i < nd->naddr2; i++) {
        if (nd->ip2[i] == ip) {
            return 1;
        }
    }
    return ip != 0 && ip == nd->ip;
}

/** Report a configured link prefix that covers `ip`, as netdev.c does. */
int netdev_on_link(const struct netdev *nd, uint32_t ip)
{
    int i;

    for (i = -1; i < nd->naddr2; i++) {
        uint32_t local = i < 0 ? nd->ip : nd->ip2[i];
        uint32_t mask = i < 0 ? nd->mask : nd->mask2[i];
        uint32_t dst = netdev_addr_dst(nd, i);

        if (netdev_addr_has_route(local, dst, mask) && (ip & mask) == (dst & mask)) {
            return 1;
        }
    }
    return 0;
}

/** Use the first address as source for the model interfaces. */
uint32_t netdev_source(const struct netdev *nd, uint32_t hop)
{
    (void)hop;
    return nd->ip;
}

/** Report whether `ip` is the broadcast address of `nd`'s subnet, which a
 * /31 and a /32 do not have. */
static int is_bcast_of(const struct netdev *nd, uint32_t ip)
{
    return nd->ip != 0 && NETDEV_MASK_HAS_BROADCAST(nd->mask) && ip == (nd->ip | ~nd->mask);
}

/** Report the all-ones address and either interface's broadcast address. */
int netdev_is_broadcast(uint32_t ip)
{
    if (ip == 0xFFFFFFFFu) {
        return 1;
    }
    return is_bcast_of(&the_iface, ip) || is_bcast_of(&other_iface, ip);
}

/** Return the first interface. */
struct netdev *netdev_first(void)
{
    return &the_iface;
}

/** Report the model's two interfaces. */
int netdev_count(void)
{
    return 2;
}

/** Return interface `n`, counted from 0, or NULL past the two. */
struct netdev *netdev_at(int n)
{
    if (n == 0) {
        return &the_iface;
    }
    return n == 1 ? &other_iface : 0;
}

/* The tick an IPv6 address's lifetimes count from. */
volatile uint64_t kernel_ticks;

/** The whole tick count, which no row moves. */
uint64_t tick_count(void)
{
    return kernel_ticks;
}

/** No IPv4 test adds IPv6 routes. */
void route6_address_update(struct netdev *nd, const struct netdev_addr6 *old,
                           const struct netdev_addr6 *a)
{
    (void)nd;
    (void)old;
    (void)a;
}

/** The IPv4 fixture has no IPv6 prefixes to retain. */
void route6_address_removed(struct netdev *nd, int slot)
{
    (void)nd;
    (void)slot;
}

/** No row gives an IPv6 address. */
struct netdev_addr6 *netdev_addr6_of(struct netdev *nd, const uint8_t *addr)
{
    (void)nd;
    (void)addr;
    CHECK(0, "no row gives an IPv6 address");
    return 0;
}

/** No row gives an IPv6 address, which joins a group. */
int netdev_join6(struct netdev *nd, const uint8_t *group)
{
    (void)nd;
    (void)group;
    CHECK(0, "no row joins an IPv6 group");
    return 0;
}

/** No row takes an IPv6 address, which leaves a group. */
void netdev_leave6(struct netdev *nd, const uint8_t *group)
{
    (void)nd;
    (void)group;
    CHECK(0, "no row leaves an IPv6 group");
}

/** A change of an interface's state forgets its carrier: nothing to do. */
void netdev_carrier_changed(struct netdev *nd)
{
    (void)nd;
}

/** No row brings an interface up, which joins the all-nodes group. */
int netdev_member6(const struct netdev *nd, const uint8_t *group)
{
    (void)nd;
    (void)group;
    CHECK(0, "no row brings an interface up");
    return 0;
}

/** No row brings an interface up or down, nor gives an IPv6 address, which
 * would reach neighbour discovery; IPv6 is never disabled here. */
int nd6_disabled(const struct netdev *nd)
{
    (void)nd;
    return 0;
}

void nd6_dad_start(struct netdev_addr6 *a)
{
    (void)a;
    CHECK(0, "no row starts duplicate address detection");
}

void nd6_if_up(struct netdev *nd)
{
    (void)nd;
    CHECK(0, "no row starts neighbour discovery");
}

void nd6_random_link_local(struct netdev *nd, uint8_t *ll)
{
    (void)nd;
    (void)ll;
    CHECK(0, "no row brings up an interface of IP datagrams");
}

void nd6_if_down(struct netdev *nd)
{
    (void)nd;
}

/* The ARP cache is not linked in here either, so a per-interface forget is
   counted the way the whole flush is. */
void arp_forget_dev(struct netdev *nd);

static int arp_forgets;
static struct netdev *arp_forgot; /* the interface of the last forget */
static int arp_forgot_locked;     /* the last forget was under the network lock */

/** Count a forget of the mappings learned on `nd`, and record `nd` and
 * whether the network lock was held. */
void arp_forget_dev(struct netdev *nd)
{
    arp_forgets++;
    arp_forgot = nd;
    arp_forgot_locked = lock_depth > 0;
}

/** Put one interface where a fresh boot leaves it. */
static void iface_reset(struct netdev *nd, uint32_t ip, uint32_t mask, uint32_t gw)
{
    nd->up = 1;
    nd->index = nd == &the_iface ? 1 : 2;
    nd->naddr2 = 0;
    nd->peer = 0;
    nd->ip = ip;
    nd->mask = mask;
    nd->gateway = gw;
    k_memset(nd->addr4, 0, sizeof(nd->addr4));
}

/** Put the configuration back where a fresh boot starts. */
static void conf_reset(void)
{
    static const uint8_t primary_mac[6] = {0x52, 0x54, 0, 0x12, 0x34, 0x56};
    static const uint8_t secondary_mac[6] = {0x52, 0x54, 0, 0x12, 0x34, 0x57};

    net_ip_addr = NET_IP_ADDR_DEFAULT;
    net_ip_mask = NET_IP_MASK_DEFAULT;
    net_ip_gateway = NET_IP_GATEWAY_DEFAULT;
    iface_reset(&the_iface, NET_IP_ADDR_DEFAULT, NET_IP_MASK_DEFAULT, NET_IP_GATEWAY_DEFAULT);
    iface_reset(&other_iface, 0, 0, 0);
    k_memcpy(the_iface.mac, primary_mac, 6);
    k_memcpy(other_iface.mac, secondary_mac, 6);
}

/**
 * Reset the configuration, take `cmdline` as the boot command line, and return
 * what netconf_apply returns, with its reason in `*why`.
 */
static int apply(const char *cmdline, const char **why)
{
    conf_reset();
    kcmdline_capture(cmdline);
    return netconf_apply(why);
}

static int warnings;           /* the warnings logged */
static int oversized_warnings; /* those for an oversized rach2.ifaddr value */

/** Count rejected tokens and the diagnostic for an oversized value. */
void klog(int level, const char *subsystem, const char *format, ...)
{
    (void)subsystem;
    if (level == KLOG_WARN) {
        static const char oversized[] = "ifaddr: value too long";

        warnings++;
        if (k_strlen(format) == sizeof(oversized) - 1 &&
            memcmp(format, oversized, sizeof(oversized)) == 0) {
            oversized_warnings++;
        }
    }
}

/**
 * Check `rach2.ifaddr`, which configures an interface named by its station
 * address: each interface gets its own address, mask and gateway, the first's
 * also being the machine's; one with no gateway clears it; and a value that is
 * malformed, too long, empty or of an unknown station address is reported once
 * and passed over, with the valid options after it applied.
 */
static void configure_by_mac(void)
{
    char line[256];
    int i;

    conf_reset();
    netconf_publish();
    kcmdline_capture("rach2.ifaddr=52:54:00:12:34:56,192.168.8.9/16,192.168.8.1 "
                     "rach2.ifaddr=52:54:00:12:34:57,10.0.3.7/24,10.0.3.1");
    netconf_apply_by_mac();
    CHECK(the_iface.ip == IP4(192, 168, 8, 9) && the_iface.mask == IP4(255, 255, 0, 0) &&
              the_iface.gateway == IP4(192, 168, 8, 1),
          "MAC option configures the first interface");
    CHECK(net_ip_addr == the_iface.ip && net_ip_mask == the_iface.mask &&
              net_ip_gateway == the_iface.gateway,
          "MAC configuration updates all primary globals");
    CHECK(other_iface.ip == IP4(10, 0, 3, 7) && other_iface.gateway == IP4(10, 0, 3, 1),
          "MAC option configures the secondary interface independently");

    kcmdline_capture("rach2.ifaddr=52:54:00:12:34:56,172.16.5.9/24");
    netconf_apply_by_mac();
    CHECK(net_ip_addr == IP4(172, 16, 5, 9) && net_ip_mask == IP4(255, 255, 255, 0) &&
              net_ip_gateway == 0 && the_iface.gateway == 0,
          "MAC configuration without a gateway clears the primary gateway");

    for (i = 79; i <= 80; i++) {
        static const char next[] = " rach2.ifaddr=52:54:00:12:34:57,10.0.3.8/24";

        conf_reset();
        warnings = oversized_warnings = 0;
        k_memcpy(line, "rach2.ifaddr=", 13);
        k_memset(line + 13, 'x', (size_t)i);
        k_memcpy(line + 13 + i, next, sizeof(next));
        kcmdline_capture(line);
        netconf_apply_by_mac();
        CHECK(warnings == 1, "invalid MAC option is reported once");
        CHECK(oversized_warnings == (i == 80), "oversized MAC option is diagnosed");
        CHECK(other_iface.ip == IP4(10, 0, 3, 8),
              "valid MAC option after invalid value is applied");
        CHECK(the_iface.ip == NET_IP_ADDR_DEFAULT && net_ip_addr == the_iface.ip,
              "invalid MAC option leaves the primary address unchanged");
    }

    conf_reset();
    warnings = oversized_warnings = 0;
    kcmdline_capture("rach2.ifaddr=52:54:00:12:34:58,10.0.4.7/24 "
                     "rach2.ifaddr= rach2.ifaddr=52:54:00:12:34:57,10.0.3.9/24");
    netconf_apply_by_mac();
    CHECK(warnings == 2 && oversized_warnings == 0,
          "unknown hardware address and empty value are reported");
    CHECK(other_iface.ip == IP4(10, 0, 3, 9), "valid MAC option after rejected tokens is applied");
}

/**
 * Check that changing the second interface's address forgets only that
 * interface's ARP mappings, under the network lock, clears its gateway and
 * leaves the first interface and the machine's address alone; and that
 * setting the address it has forgets nothing.
 */
static void secondary_address_change(void)
{
    conf_reset();
    iface_reset(&other_iface, IP4(10, 0, 3, 7), IP4(255, 255, 255, 0), IP4(10, 0, 3, 1));
    arp_forgets = arp_flushes = 0;
    CHECK(netconf_set_address(&other_iface, IP4(10, 0, 4, 7), IP4(255, 255, 255, 0)) == 0,
          "secondary address changes");
    CHECK(arp_forgets == 1 && arp_forgot == &other_iface && arp_flushes == 0,
          "secondary address change invalidates only its ARP mappings");
    CHECK(arp_forgot_locked, "and invalidates them under the network lock");
    CHECK(other_iface.gateway == 0 && net_ip_addr == NET_IP_ADDR_DEFAULT &&
              net_ip_gateway == NET_IP_GATEWAY_DEFAULT,
          "secondary address change clears its gateway and preserves primary state");
    CHECK(netconf_set_address(&other_iface, other_iface.ip, other_iface.mask) == 0 &&
              arp_forgets == 1,
          "unchanged secondary address forgets nothing");
    CHECK(netconf_set_address(&other_iface, 0, 0) == 0 && arp_forgets == 2,
          "removing the secondary address forgets its mappings");
}

/**
 * Run the rows of narrow_subnets for an address added to an interface
 * beside its first.
 */
static void narrow_subnets_added(void)
{
    iface_reset(&other_iface, 0, 0, 0);
    other_iface.naddr2 = 0;
    CHECK(netconf_add_address(&other_iface, IP4(192, 168, 0, 105), 0xFFFFFFFFu, 0, 0) == 0 &&
              other_iface.ip == IP4(192, 168, 0, 105) && other_iface.mask == 0xFFFFFFFFu,
          "a /32 is added as an interface's first address");
    CHECK(netconf_add_address(&other_iface, IP4(10, 0, 0, 0), 0xFFFFFFFEu, 0, 0) == 0 &&
              netconf_add_address(&other_iface, IP4(10, 0, 0, 1), 0xFFFFFFFEu, 0, 0) == 0 &&
              other_iface.naddr2 == 2,
          "both addresses of a /31 are added beside it");
    CHECK(!netconf_is_broadcast(IP4(10, 0, 0, 0)) && !netconf_is_broadcast(IP4(10, 0, 0, 1)),
          "neither of which is a broadcast address");
    CHECK(netconf_add_address(&other_iface, IP4(10, 0, 1, 0), 0xFFFFFFFCu, 0, 0) == 0 &&
              netconf_add_address(&other_iface, IP4(10, 0, 1, 3), 0xFFFFFFFCu, 0, 0) == 0,
          "both endpoint addresses can be assigned");
    CHECK(netconf_add_address(&other_iface, IP4(10, 0, 1, 2), 0xFFFFFFFCu, 0, 0) == 0 &&
              other_iface.naddr2 == 5,
          "and so can another host");
    other_iface.naddr2 = 0;
}

/**
 * Judge the subnets of a /32, a /31 and a /30, as Linux does: every address
 * of a /32 or a /31 is a host (RFC 3021) and neither has a broadcast address,
 * so a /32 has no other host for a gateway and a /31 has one; a /30 keeps its
 * network and broadcast addresses out. The rules are the same for the boot
 * line, an address set, a mask set and an address added.
 */
static void narrow_subnets(void)
{
    uint32_t ip = 0, mask = 0;

    CHECK(netconf_parse_cidr("192.168.0.105/32", &ip, &mask) && ip == IP4(192, 168, 0, 105) &&
              mask == 0xFFFFFFFFu,
          "a /32 parses");
    CHECK(netconf_gateway_ok(IP4(192, 168, 0, 1), ip, mask) == 0,
          "a /32 has no other host to be its gateway");
    CHECK(netconf_parse_cidr("10.0.0.0/31", &ip, &mask) && ip == IP4(10, 0, 0, 0) &&
              mask == 0xFFFFFFFEu,
          "the lower address of a /31 parses");
    CHECK(netconf_gateway_ok(IP4(10, 0, 0, 1), ip, mask), "and the upper is its gateway");
    CHECK(netconf_parse_cidr("10.0.0.1/31", &ip, &mask) && ip == IP4(10, 0, 0, 1),
          "the upper address of a /31 parses");
    CHECK(netconf_gateway_ok(IP4(10, 0, 0, 0), ip, mask), "and the lower is its gateway");
    CHECK(netconf_gateway_ok(IP4(10, 0, 0, 2), ip, mask) == 0,
          "an address past the /31 is no gateway of it");
    CHECK(cidr_refused("10.0.0.0/30") && cidr_refused("10.0.0.3/30"),
          "a /30 refuses its network and broadcast addresses");
    CHECK(netconf_gateway_ok(IP4(10, 0, 0, 3), IP4(10, 0, 0, 1), 0xFFFFFFFCu) == 0,
          "and they are no gateway of it");
    CHECK(netconf_gateway_ok(IP4(10, 0, 0, 2), IP4(10, 0, 0, 1), 0xFFFFFFFCu),
          "but its other host is");
    CHECK(cidr_refused("192.168.0.105/33"), "a prefix past 32 is refused");

    conf_reset();
    CHECK(netconf_set_address(&the_iface, IP4(192, 168, 0, 105), 0xFFFFFFFFu) == 0 &&
              net_ip_mask == 0xFFFFFFFFu,
          "an interface takes a /32");
    CHECK(netconf_broadcast() == 0xFFFFFFFFu && !netconf_is_broadcast(IP4(192, 168, 0, 105)),
          "which has no broadcast address, its own address least of all");
    CHECK(netconf_set_gateway(&the_iface, IP4(192, 168, 0, 1)) == -EINVAL,
          "nor a gateway on its subnet");
    CHECK(netconf_set_address(&the_iface, IP4(10, 0, 0, 1), 0xFFFFFFFEu) == 0,
          "an interface takes the upper address of a /31");
    CHECK(netconf_broadcast() == 0xFFFFFFFFu && !netconf_is_broadcast(IP4(10, 0, 0, 0)),
          "whose other address is a host, not a broadcast address");
    CHECK(netconf_set_gateway(&the_iface, IP4(10, 0, 0, 0)) == 0 &&
              net_ip_gateway == IP4(10, 0, 0, 0),
          "and is its gateway");
    CHECK(netconf_set_address(&the_iface, IP4(10, 0, 0, 0), 0xFFFFFFFEu) == 0 &&
              net_ip_gateway == 0,
          "the lower address of a /31 is taken too, which drops that gateway");
    CHECK(netconf_set_address(&the_iface, IP4(10, 0, 0, 0), 0xFFFFFFFCu) == 0 &&
              netconf_set_address(&the_iface, IP4(10, 0, 0, 3), 0xFFFFFFFCu) == 0,
          "a /30 accepts local addresses at both ends");
    CHECK(netconf_set_address(&the_iface, IP4(10, 0, 0, 1), 0xFFFFFFFCu) == 0 &&
              netconf_broadcast() == IP4(10, 0, 0, 3) && netconf_is_broadcast(IP4(10, 0, 0, 3)),
          "and a host of it has the broadcast address");

    CHECK(netconf_set_address(&the_iface, IP4(192, 168, 1, 0), 0) == 0 &&
              net_ip_addr == IP4(192, 168, 1, 0),
          "the address takes effect with its classful mask");
    CHECK(netconf_set_netmask(&the_iface, 0xFFFFFFFFu) == 0 && net_ip_addr == IP4(192, 168, 1, 0) &&
              net_ip_mask == 0xFFFFFFFFu,
          "is a host under a /32 mask");
    CHECK(netconf_set_netmask(&the_iface, 0xFFFFFFFEu) == 0 && net_ip_mask == 0xFFFFFFFEu,
          "and under a /31 mask");
    CHECK(netconf_set_netmask(&the_iface, 0xFFFFFFFCu) == 0,
          "a /30 also accepts its network address");

    narrow_subnets_added();
    conf_reset();
}

/** Check explicit routes through a device as its peers and link change. */
static void route_lifetime(int l3)
{
    struct netdev *nd = &other_iface;
    uint32_t ip = IP4(192, 0, 2, 1), peer = IP4(198, 51, 100, 1);
    uint32_t gw = IP4(203, 0, 113, 2), dst = IP4(172, 16, 0, 0);
    uint32_t via, mask = 0xffffff00u;
    struct netdev_rt r = {0};

    conf_reset();
    nd->l3 = l3;
    CHECK(netconf_add_address(nd, ip, mask, &peer, 0) == 0 &&
              netconf_add_address(nd, ip, ~0u, &gw, 0) == 0,
          "two destination prefixes on one interface");
    CHECK(netdev_route_add(nd, ip & mask, mask, 0, 10, 0) == 0 &&
              netdev_route_add(nd, dst, mask, gw, 20, 0) == 0 &&
              netdev_route_add(nd, 0, 0, gw, 1, 0) == 0,
          "install direct, gateway and default routes");
    CHECK(netconf_del_address(nd, ip, ~0u, &gw) == 0 && netdev_route(dst, &via) == nd && via == gw,
          "a removed peer keeps the explicit route");
    CHECK(netdev_route_lookup(gw, nd->index, &r) == nd && r.gw == gw,
          "a lookup of the removed peer now reports the default route's gateway");
    r.gw = r.prefsrc = 0;
    CHECK(netdev_gateway_reachable(nd, ip + 1), "an explicit direct route resolves a gateway");
    CHECK(netconf_del_address(nd, ip, mask, &peer) == 0 && netdev_route(dst, &via) != nd &&
              netdev_route(ip + 1, &via) != nd && netdev_default_gateway(nd) == 0,
          "last address flushes direct and gateway routes");
    CHECK(netconf_add_address(nd, ip, mask, &peer, 0) == 0 && !netdev_gateway_reachable(nd, ip + 1),
          "a restored address does not restore a direct route");
    CHECK(netdev_route_del(nd, peer & mask, mask, 0) == 0 && !netdev_gateway_reachable(nd, peer),
          "a deleted kernel prefix stays deleted");
    CHECK(netconf_set_peer(nd, gw) == 0 && netdev_gateway_reachable(nd, gw),
          "changing the peer installs its new prefix");
    r.dst = dst;
    r.mask = mask;
    r.metric = 8;
    r.proto = 99;
    r.scope = 254;
    CHECK(netdev_route_change(nd, &r, 0, 0) == 0, "install a host-scope route");
    r.proto = 4;
    CHECK(netdev_route_change(nd, &r, 1, 0) == -ESRCH, "wrong protocol leaves the route intact");
    CHECK(netdev_route_add(nd, ip & mask, mask, 0, 0, 0) == 0 && netconf_set_up(nd, 0) == 0 &&
              !netdev_has_link_route(nd, gw & mask, mask),
          "taking the link down hides address prefixes");
    r.proto = 99;
    CHECK(netdev_route_change(nd, &r, 1, 0) == 0 &&
              netdev_route_del(nd, ip & mask, mask, 0) == -ESRCH,
          "down retains only host scope");
    CHECK(netconf_del_address(nd, ip, mask, &gw) == 0, "remove address from the down interface");
    nd->l3 = 0;
}

/** Reinsert each ioctl replacement behind the surviving primary. */
static void ioctl_reinsert(void)
{
    struct netdev *nd = &other_iface;
    uint32_t ip = IP4(192, 0, 2, 1), keep = IP4(198, 51, 100, 1), mask = 0xffffff00u;
    uint32_t brd = IP4(198, 51, 100, 252);
    int op;
    for (op = 0; op < 4; op++) {
        uint32_t old = op == 2 ? keep + 1 : ip;
        uint32_t next = op == 0 ? keep + 1 : old;
        conf_reset();
        CHECK(netconf_add_address(nd, old, op == 2 ? ~0u : mask, 0, 0) == 0 &&
                  netconf_add_address(nd, keep, mask, 0, 0) == 0,
              "configure two primary addresses");
        nd->addr4[0].metric = 23;
        if (op == 0)
            CHECK(netconf_set_address(nd, next, 0) == 0, "replace the first address");
        else if (op == 1)
            CHECK(netconf_set_peer(nd, keep + 2) == 0, "replace the peer");
        else if (op == 2)
            CHECK(netconf_set_netmask(nd, mask) == 0, "replace the mask");
        else
            CHECK(netconf_set_broadcast(nd, brd) == 0, "replace the broadcast");
        CHECK(nd->ip == keep && nd->naddr2 == 1 && nd->ip2[0] == next &&
                  netdev_secondary(nd, 0) == (op != 3) && nd->addr4[1].metric == 23,
              "the surviving primary remains first and each address keeps its attributes");
        CHECK(op != 3 || netdev_addr_brd(nd, 0) == brd, "the new broadcast follows its address");
        CHECK(netconf_del_address(nd, next, 0, 0) == 0 && nd->ip == keep &&
                  netdev_has_link_route(nd, keep & mask, mask),
              "deleting the replacement keeps the surviving primary and route");
    }
}

/** A duplicate replacement leaves only the existing address. */
static void ioctl_duplicate(void)
{
    struct netdev *nd = &other_iface;
    uint32_t ip = IP4(192, 0, 2, 1), keep = IP4(198, 51, 100, 1), mask = 0xffffff00u;
    conf_reset();
    CHECK(!netconf_add_address(nd, ip, mask, 0, 0) && !netconf_add_address(nd, keep, mask, 0, 0),
          "configure distinct prefixes");
    CHECK(netconf_set_address(nd, keep, 0) == -EEXIST && nd->ip == keep && !nd->naddr2,
          "the old address is gone when reinsertion finds a duplicate");
}

/** Reinsertion keeps Linux's address scope order and rejects a mixed subnet. */
static void scope_reinsert(void)
{
    struct netdev *nd = &other_iface;
    uint32_t first = IP4(192, 0, 2, 1), link = IP4(198, 51, 100, 1);
    uint32_t mask = 0xffffff00u;
    conf_reset();
    CHECK(!netconf_add_address(nd, first, mask, 0, 0) &&
              !netconf_add_address(nd, first + 1, mask, 0, 0) &&
              !netconf_add_address(nd, link, mask, 0, 253) && nd->ip == link &&
              nd->ip2[0] == first && nd->ip2[1] == first + 1,
          "a link-scope primary precedes global primaries and their secondaries");
    CHECK(!netconf_set_broadcast(nd, link + 42) && nd->ip == link && nd->addr4[0].scope == 253 &&
              netdev_addr_brd(nd, -1) == link + 42,
          "reinsert the link-scope primary ahead of the surviving global addresses");
    CHECK(netconf_add_address(nd, first + 2, mask, 0, 253) == -EINVAL && nd->naddr2 == 2,
          "a secondary must have the primary's scope");
    CHECK(!netconf_set_address(nd, first + 2, 0) && nd->ip == first && nd->ip2[1] == first + 2 &&
              !nd->addr4[2].scope,
          "replacing a link-scope address resets its scope and appends it as secondary");
}

/** An explicit zero peer has its own destination prefix. */
static void zero_peer(void)
{
    struct netdev *nd = &other_iface;
    uint32_t first = IP4(192, 0, 2, 1), keep = IP4(198, 51, 100, 1);
    uint32_t mask = 0xffffff00u, zero = 0;
    conf_reset();
    CHECK(!netconf_add_address(nd, first, mask, 0, 0) &&
              !netconf_add_address(nd, keep, mask, &zero, 0),
          "configure an address with an explicit zero peer");
    CHECK(!netconf_set_peer(nd, 0) && nd->ip == keep && netdev_secondary(nd, 0) &&
              !netdev_has_link_route(nd, first & mask, mask) && !netdev_addr_dst(nd, -1),
          "a zero peer joins the existing zero subnet");
    CHECK(!netconf_del_address(nd, first, mask, &zero) && nd->ip == keep && !nd->naddr2,
          "deletion by the zero prefix preserves its primary");
    CHECK(!netconf_set_peer(nd, keep) && netdev_has_link_route(nd, keep & mask, mask),
          "resetting the peer to the local address installs its subnet route");
}

/** Count a generated or explicit local-table row with its preferred source. */
static int local_row(uint32_t dst, int kind, uint32_t src)
{
    struct netdev_rt r;
    int i;
    for (i = 0; netdev_local_route_at(i, &r); i++)
        if (r.dst == dst && r.kind == kind && r.prefsrc == src)
            return 1;
    return 0;
}

/** Check local, broadcast and preferred-source routes on a shared prefix. */
static void local_lifetime(void)
{
    struct netdev *nd = &other_iface;
    struct netdev_rt r = {0};
    uint32_t ip = 0xc0000201u, mask = 0xffffff00u, other = 0xc6336401u;
    conf_reset();
    CHECK(netconf_add_address(nd, ip, mask, 0, 0) == 0 &&
              netconf_add_address(nd, ip + 1, mask, 0, 0) == 0 &&
              netconf_add_address(nd, other, mask, 0, 0) == 0,
          "install a primary, alias and other subnet");
    CHECK(local_row(ip + 1, NETDEV_RT_LOCAL, ip), "an alias uses the primary's preferred source");
    nd->addr4[0].noprefix = 1;
    CHECK(!netdev_has_link_route(nd, ip & mask, mask) &&
              local_row(ip | ~mask, NETDEV_RT_BROADCAST, ip),
          "noprefix preserves the subnet broadcast");
    CHECK(netconf_set_broadcast(nd, other + 2) == 0 &&
              local_row(other + 2, NETDEV_RT_BROADCAST, ip) && nd->naddr2 == 1,
          "a custom broadcast is installed");
    CHECK(netconf_set_netmask(nd, 0xffff0000u) == 0 && nd->ip == ip &&
              netconf_set_netmask(nd, 0xffff0000u) == 0 && netdev_addr_brd(nd, 0) == other + 2,
          "a mask change preserves a custom broadcast");
    r.dst = 0xac100001u;
    r.mask = ~0u;
    r.prefsrc = ip;
    r.scope = 253;
    r.proto = 99;
    CHECK(netdev_route_change(&the_iface, &r, 0, 0) == 0, "another card uses this source");
    CHECK(netconf_del_address(nd, ip, 0, 0) == 0 &&
              netdev_route_change(&the_iface, &r, 1, 0) == -ESRCH && nd->ip == other,
          "losing the source flushes its dependent route and keeps the other primary");
    r.prefsrc = 0;
    r.kind = NETDEV_RT_LOCAL;
    r.scope = 254;
    CHECK(netdev_local_route_change(nd, &r, 0, 0) == 0 && netdev_added_local(r.dst, r.kind) == nd,
          "an explicit host-scope local route");
    CHECK(netconf_set_up(nd, 0) == 0 && netdev_added_local(r.dst, r.kind) == nd &&
              local_row(other, NETDEV_RT_LOCAL, other),
          "local routes survive administrative down");
    CHECK(netconf_del_address(nd, other, 0, 0) == 0 && !netdev_added_local(r.dst, r.kind),
          "last-address deletion flushes the explicit local route");
}

/** Address expiry deprecates first and flushes routes only on deletion. */
static void timed_address(void)
{
    struct netdev *nd = &other_iface;
    uint32_t dst = 0xac100000u, mask = 0xffffff00u;
    conf_reset();
    CHECK(netconf_add_address(nd, 0xc0000201u, mask, 0, 0) == 0, "add a finite IPv4 address");
    nd->addr4[0].preferred_until = 10;
    nd->addr4[0].valid_until = 20;
    CHECK(netdev_route_add(nd, dst, mask, 0, 3, 0) == 0, "add a route under its lifetime");
    netconf_expire_addresses(10);
    CHECK(nd->ip && nd->addr4[0].deprecated, "preferred lifetime ends first");
    netconf_expire_addresses(20);
    CHECK(!nd->ip && netdev_route_del(nd, dst, mask, 0) == -ESRCH,
          "valid lifetime ends with last-address cleanup");
}

/**
 * Run the rows of configure_by_mac, secondary_address_change and
 * narrow_subnets, then:
 *
 *   * netconf_parse_cidr takes an address with a prefix from /1 to /32 and
 *     rejects /0; text that must not become an address, and an
 *     address that exists but is not a host, is rejected and leaves the
 *     caller's values alone;
 *   * netconf_parse_addr takes a bare address for the gateway, and
 *     netconf_gateway_ok takes only a host on the address's subnet other than
 *     the address itself;
 *   * the tuple rule of netconf_apply: no option keeps the SLIRP defaults, an
 *     address alone has no gateway, and a gateway alone, an option given twice
 *     or a value that is no address is rejected with a reason; a token that
 *     only starts or ends with a key is not that key;
 *   * netconf_format and netconf_prefix_of;
 *   * an address set while the machine runs forgets the ARP cache, every entry
 *     of which was learned by a machine at the old address, and tells TCP; an
 *     address that is not a host, or a mask that is not a run of ones, is
 *     rejected and tells nothing;
 *   * an address with no mask takes the classful mask its first byte implies
 *     until the real one arrives, since `ifconfig` sets the two in separate
 *     calls, and the other order, the one a machine booted with `none` is
 *     configured in, works too. An address that is a host only under a wider
 *     mask waits out of force while the classful mask stands in, and a
 *     rejection in that state unconfigures the interface rather than leave it
 *     on a pair nobody judged;
 *   * reapplying the pair the machine has changes nothing and tells nothing: a
 *     DHCP client runs the same `ifconfig` on a renewal as on a lease, and a
 *     machine that tore its connections down at every renewal would drop every
 *     session on it at the lease interval;
 *   * an address of 0 unconfigures, as a client does when a lease ends, and
 *     takes the mask with it;
 *   * a gateway needs an address first, and must be a host on its subnet;
 *   * a machine with two cards is on two networks, and configuring one of them
 *     leaves the other alone: the second interface's address, mask and gateway
 *     are its own, it can wait for its mask without putting the first out of
 *     force, and only the ARP mappings learned on it are forgotten. A call
 *     about no interface, from a caller whose device is gone, configures
 *     nothing;
 *   * `rach2.ip=none` leaves IPv4 unset for userland to set, and a gateway
 *     beside it is rejected;
 *   * every change leaves the network lock as it found it.
 *
 * Returns the number of failed checks, at most 255.
 */
int main(void)
{
    uint32_t ip, mask;
    const char *why;
    char text[16];

    configure_by_mac();
    secondary_address_change();
    route_lifetime(0);
    route_lifetime(1);
    ioctl_reinsert();
    ioctl_duplicate();
    scope_reinsert();
    zero_peer();
    local_lifetime();
    timed_address();
    narrow_subnets();
    ip = 0;
    mask = 0;
    CHECK(netconf_parse_cidr("192.168.1.50/24", &ip, &mask), "a plain LAN address parses");
    CHECK(ip == IP4(192, 168, 1, 50), "the address is the one written");
    CHECK(mask == 0xFFFFFF00u, "a /24 is 255.255.255.0");

    CHECK(netconf_parse_cidr("10.0.2.15/8", &ip, &mask) && mask == 0xFF000000u,
          "a /8 is 255.0.0.0");
    CHECK(netconf_parse_cidr("172.16.5.9/30", &ip, &mask) && mask == 0xFFFFFFFCu,
          "a /30 is accepted");
    CHECK(netconf_parse_cidr("1.2.3.4/1", &ip, &mask) && mask == 0x80000000u,
          "a /1, the widest, is accepted");
    CHECK(netconf_parse_cidr("192.168.1.50/0", &ip, &mask) == 0, "a /0 is refused");
    CHECK(cidr_refused("192.168.1.50/0"), "the /0 refusal leaves the caller's values alone");

    CHECK(cidr_refused("192.168.1.50"), "an address with no prefix is refused");
    CHECK(cidr_refused("192.168.1/24"), "three quartets are refused");
    CHECK(cidr_refused("192.168.1.50.7/24"), "five quartets are refused");
    CHECK(cidr_refused("192.168.1.256/24"), "a quartet over 255 is refused");
    CHECK(cidr_refused("192.168.1.-1/24"), "a negative quartet is refused");
    CHECK(cidr_refused("192.168.01.50/24"), "a leading zero is refused, not read as octal");
    CHECK(cidr_refused("010.0.0.1/24"), "a leading zero in the first quartet is refused too");
    CHECK(cidr_refused("192.168.1.50/24 "), "a trailing space is refused, not ignored");
    CHECK(cidr_refused("192.168.1.50/24x"), "trailing text is refused");
    CHECK(cidr_refused("192.168.1.50/2x4"), "a malformed prefix is refused");
    CHECK(cidr_refused(""), "an empty address is refused");
    CHECK(cidr_refused("/24"), "a prefix with no address is refused");
    CHECK(cidr_refused("192.168.1.50/"), "an address with an empty prefix is refused");
    CHECK(cidr_refused("hello/24"), "text that is not a number at all is refused");
    CHECK(cidr_refused("192.168.1.50/1024"), "an over-long prefix is refused, not truncated");

    CHECK(cidr_refused("192.168.1.0/24"), "the subnet's network address is refused");
    CHECK(cidr_refused("192.168.1.255/24"), "the subnet's broadcast address is refused");
    CHECK(cidr_refused("172.16.4.0/30"), "the network address of a /30 is refused");
    CHECK(cidr_refused("172.16.4.3/30"), "the broadcast address of a /30 is refused");
    CHECK(netconf_parse_cidr("172.16.4.1/30", &ip, &mask),
          "a host inside that /30 is still accepted");
    CHECK(cidr_refused("0.0.0.0/24"), "the all-zero address is refused");
    CHECK(cidr_refused("255.255.255.255/24"), "the all-ones address is refused");
    CHECK(cidr_refused("127.0.0.1/8"), "loopback is refused, since the stack aliases it");
    CHECK(cidr_refused("224.0.0.1/24"), "a multicast address is refused");
    CHECK(cidr_refused("239.255.255.250/24"), "the top of multicast is refused");

    ip = 0;
    CHECK(netconf_parse_addr("10.0.2.2", &ip) && ip == IP4(10, 0, 2, 2), "a bare address parses");
    CHECK(addr_refused("10.0.2.2/24"), "a prefix on a bare address is refused");
    CHECK(addr_refused("10.0.2"), "a short bare address is refused");
    CHECK(addr_refused("0.0.0.0"), "the all-zero gateway is refused");
    CHECK(addr_refused("127.0.0.1"), "a loopback gateway is refused");

    CHECK(netconf_gateway_ok(IP4(192, 168, 1, 1), IP4(192, 168, 1, 50), 0xFFFFFF00u),
          "a gateway on our subnet is usable");
    CHECK(netconf_gateway_ok(IP4(10, 0, 0, 1), IP4(192, 168, 1, 50), 0xFFFFFF00u) == 0,
          "a gateway on another subnet is refused: nothing could reach it");
    CHECK(netconf_gateway_ok(IP4(192, 168, 1, 50), IP4(192, 168, 1, 50), 0xFFFFFF00u) == 0,
          "our own address is not a gateway");
    CHECK(netconf_gateway_ok(IP4(192, 168, 1, 0), IP4(192, 168, 1, 50), 0xFFFFFF00u) == 0,
          "the network address is not a gateway");
    CHECK(netconf_gateway_ok(IP4(192, 168, 1, 255), IP4(192, 168, 1, 50), 0xFFFFFF00u) == 0,
          "the broadcast address is not a gateway");

    CHECK(apply("", &why) == NETCONF_DEFAULT, "an empty command line keeps the defaults");
    CHECK(net_ip_addr == NET_IP_ADDR_DEFAULT && net_ip_mask == NET_IP_MASK_DEFAULT &&
              net_ip_gateway == NET_IP_GATEWAY_DEFAULT,
          "and the defaults are the whole SLIRP tuple");

    CHECK(apply("/kernel rach2.rescue quiet", &why) == NETCONF_DEFAULT,
          "a command line with other options keeps the defaults");
    CHECK(net_ip_gateway == NET_IP_GATEWAY_DEFAULT, "and does not disturb the gateway");

    CHECK(apply("rach2.ip=192.168.1.50/24 rach2.gateway=192.168.1.1", &why) == NETCONF_APPLIED,
          "an address and a gateway are applied");
    CHECK(net_ip_addr == IP4(192, 168, 1, 50) && net_ip_mask == 0xFFFFFF00u &&
              net_ip_gateway == IP4(192, 168, 1, 1),
          "and all three numbers are the ones given");

    CHECK(apply("rach2.ip=192.168.1.50/24", &why) == NETCONF_APPLIED,
          "an address alone is applied");
    CHECK(net_ip_gateway == 0, "and leaves NO gateway, not the SLIRP one beside a real LAN");

    CHECK(apply("rach2.gateway=192.168.1.1", &why) == NETCONF_REFUSED,
          "a gateway with no address is refused");
    CHECK(net_ip_addr == NET_IP_ADDR_DEFAULT && net_ip_gateway == NET_IP_GATEWAY_DEFAULT,
          "and the refusal leaves the defaults in force");

    CHECK(apply("rach2.ip=192.168.1.50/24 rach2.ip=10.0.0.5/24", &why) == NETCONF_REFUSED,
          "the address given twice is refused, not resolved by a first-wins rule");
    CHECK(apply("rach2.ip=192.168.1.50/24 rach2.gateway=192.168.1.1 rach2.gateway=192.168.1.2",
                &why) == NETCONF_REFUSED,
          "the gateway given twice is refused");
    CHECK(apply("rach2.ip=192.168.1.0/24", &why) == NETCONF_REFUSED,
          "a network address on the command line is refused");
    CHECK(apply("rach2.ip=192.168.1.50/24 rach2.gateway=10.0.0.1", &why) == NETCONF_REFUSED,
          "a gateway off the given subnet is refused");
    CHECK(apply("rach2.ip=nonsense", &why) == NETCONF_REFUSED, "nonsense is refused");
    CHECK(apply("rach2.ip=", &why) == NETCONF_REFUSED, "an empty value is refused");
    CHECK(why[0] != '\0', "and every refusal says what was wrong");

    CHECK(apply("rach2.ip=192.168.1.50/24 rach2.ipx=1 norach2.ip=2", &why) == NETCONF_APPLIED,
          "a token that merely starts or ends with the key is not the key");
    CHECK(net_ip_addr == IP4(192, 168, 1, 50), "and the real option still took effect");

    netconf_format(IP4(192, 168, 1, 50), text);
    CHECK(text[0] == '1' && text[1] == '9' && text[2] == '2' && text[3] == '.',
          "an address formats back to dotted quad");
    netconf_format(IP4(0, 0, 0, 0), text);
    CHECK(text[0] == '0' && text[1] == '.' && text[7] == '\0', "zero formats as 0.0.0.0");
    netconf_format(IP4(255, 255, 255, 255), text);
    CHECK(text[15] == '\0', "the widest address fits the buffer the header promises");
    CHECK(netconf_prefix_of(0xFFFFFF00u) == 24 && netconf_prefix_of(0xFF000000u) == 8 &&
              netconf_prefix_of(0xFFFFFFFCu) == 30,
          "a mask reports its prefix length");

    conf_reset();
    net_ip_addr = IP4(10, 0, 2, 15);
    net_ip_mask = IP4(255, 255, 255, 0);
    net_ip_gateway = IP4(10, 0, 2, 2);
    iface_reset(&the_iface, net_ip_addr, net_ip_mask, net_ip_gateway);
    arp_flushes = 0;

    CHECK(netconf_set_address(&the_iface, IP4(192, 168, 90, 20), IP4(255, 255, 255, 0)) == 0,
          "a host address on a real subnet is taken");
    CHECK(net_ip_addr == IP4(192, 168, 90, 20) && net_ip_mask == IP4(255, 255, 255, 0),
          "and is what the stack now has");
    CHECK(arp_flushes == 1, "the ARP cache is forgotten, since every entry was learned "
                            "by a machine at the old address");
    CHECK(net_ip_gateway == 0, "a gateway that is no longer on this subnet is not a gateway");

    arp_flushes = 0;
    CHECK(netconf_set_address(&the_iface, IP4(224, 0, 0, 1), IP4(255, 255, 255, 0)) == -EINVAL,
          "nor is a multicast address");
    CHECK(netconf_set_address(&the_iface, IP4(192, 168, 90, 20), IP4(255, 0, 255, 0)) == -EINVAL,
          "a mask that is not a run of ones is refused");
    CHECK(net_ip_addr == IP4(192, 168, 90, 20), "the address is unchanged by all of that");
    CHECK(arp_flushes == 0, "and nothing was forgotten for a change that did not happen");

    arp_flushes = 0;
    CHECK(netconf_set_address(&the_iface, IP4(10, 1, 2, 3), 0) == 0,
          "an address with no mask is taken");
    CHECK(net_ip_mask == IP4(255, 0, 0, 0), "with the classful mask its first byte implies");
    CHECK(netconf_set_netmask(&the_iface, IP4(255, 255, 255, 0)) == 0, "and the real mask follows");
    CHECK(net_ip_mask == IP4(255, 255, 255, 0) && net_ip_addr == IP4(10, 1, 2, 3),
          "leaving the address where it was");
    CHECK(netconf_set_netmask(&the_iface, IP4(255, 255, 255, 252)) == 0,
          "an assigned address may equal its subnet broadcast");
    CHECK(netconf_set_netmask(&the_iface, IP4(255, 0, 255, 0)) == -EINVAL,
          "and so is one that is not a run of ones");
    CHECK(net_ip_mask == IP4(255, 255, 255, 252), "an invalid mask preserves the last good one");

    CHECK(netconf_set_address(&the_iface, 0, 0) == 0, "unconfiguring first");
    CHECK(net_ip_addr == 0 && net_ip_mask == 0, "leaves nothing set");
    CHECK(netconf_set_address(&the_iface, IP4(192, 168, 90, 20), 0) == 0,
          "an address can be set with nothing configured");
    CHECK(netconf_set_netmask(&the_iface, IP4(255, 255, 255, 0)) == 0, "and then a mask");
    CHECK(net_ip_addr == IP4(192, 168, 90, 20) && net_ip_mask == IP4(255, 255, 255, 0),
          "which is a configured machine");

    CHECK(netconf_set_address(&the_iface, IP4(192, 168, 1, 0), 0) == 0,
          "an address that is a network address under its classful mask is taken");
    CHECK(net_ip_addr == IP4(192, 168, 1, 0) && net_ip_mask == 0xffffff00u,
          "its address and local route take effect immediately");
    CHECK(netconf_set_netmask(&the_iface, IP4(255, 255, 0, 0)) == 0,
          "and the wider mask makes it a host");
    CHECK(net_ip_addr == IP4(192, 168, 1, 0) && net_ip_mask == IP4(255, 255, 0, 0),
          "which is the pair the machine ends up on");
    arp_flushes = 0;
    CHECK(netconf_set_address(&the_iface, IP4(192, 168, 1, 0), 0) == 0, "the same address again");
    CHECK(net_ip_addr == IP4(192, 168, 1, 0) && net_ip_mask == IP4(255, 255, 0, 0),
          "leaves the pair in force");
    CHECK(arp_flushes == 0, "and forgets nothing, because nothing changed");
    CHECK(netconf_set_netmask(&the_iface, IP4(255, 255, 0, 0)) == 0, "and so does the same mask");
    CHECK(arp_flushes == 0, "which is also nothing to forget");

    arp_flushes = 0;
    CHECK(netconf_set_address(&the_iface, IP4(10, 0, 2, 15), IP4(255, 255, 255, 0)) == 0,
          "an ordinary address, settled");
    CHECK(netconf_set_address(&the_iface, IP4(10, 0, 2, 15), 0) == 0, "reapplied with no mask");
    CHECK(net_ip_mask == IP4(255, 255, 255, 0), "keeps the mask it had, not the classful one");
    CHECK(arp_flushes == 1, "and forgot nothing beyond the one real change");

    CHECK(netconf_set_address(&the_iface, IP4(192, 168, 1, 0), 0) == 0, "a classful address again");
    CHECK(netconf_set_netmask(&the_iface, 0xff00ff00u) == -EINVAL &&
              net_ip_addr == IP4(192, 168, 1, 0) && net_ip_mask == 0xffffff00u,
          "an invalid separate mask preserves the address");

    arp_flushes = 0;
    CHECK(netconf_set_address(&the_iface, IP4(192, 168, 90, 20), IP4(255, 255, 255, 0)) == 0,
          "a machine to unconfigure");
    arp_flushes = 0;
    CHECK(netconf_set_address(&the_iface, 0, 0) == 0, "an address of zero unconfigures");
    CHECK(net_ip_addr == 0 && net_ip_mask == 0,
          "taking the mask with it, since half a configuration is not one");
    CHECK(arp_flushes == 1, "and that is a change like any other");
    CHECK(netconf_broadcast() == 0xFFFFFFFFu,
          "an unconfigured machine broadcasts to everyone or not at all");

    CHECK(netconf_set_gateway(&the_iface, IP4(192, 168, 90, 1)) == -EINVAL,
          "a gateway needs an address to be on the subnet of");
    CHECK(netconf_set_address(&the_iface, IP4(192, 168, 90, 20), IP4(255, 255, 255, 0)) == 0,
          "so the address comes first");
    CHECK(netconf_broadcast() == IP4(192, 168, 90, 255), "which fixes the broadcast address");
    CHECK(netconf_set_gateway(&the_iface, IP4(192, 168, 90, 1)) == 0 &&
              net_ip_gateway == IP4(192, 168, 90, 1),
          "and then the gateway is taken");
    CHECK(netconf_set_gateway(&the_iface, IP4(10, 0, 2, 2)) == -EINVAL,
          "a gateway off the subnet is refused");
    CHECK(netconf_set_gateway(&the_iface, IP4(192, 168, 90, 20)) == -EINVAL,
          "and so is this machine itself");
    CHECK(net_ip_gateway == IP4(192, 168, 90, 1), "neither of which moved the one that works");
    CHECK(netconf_set_gateway(&the_iface, 0) == 0 && net_ip_gateway == 0, "zero clears it");

    conf_reset();
    CHECK(netconf_set_address(&the_iface, IP4(192, 168, 90, 20), IP4(255, 255, 255, 0)) == 0 &&
              netconf_set_gateway(&the_iface, IP4(192, 168, 90, 1)) == 0,
          "the first interface is configured");
    arp_flushes = 0;
    arp_forgets = 0;
    arp_forgot = 0;

    CHECK(netconf_set_address(&other_iface, IP4(10, 0, 3, 7), IP4(255, 255, 255, 0)) == 0,
          "and the second is configured on its own subnet");
    CHECK(other_iface.ip == IP4(10, 0, 3, 7) && other_iface.mask == IP4(255, 255, 255, 0),
          "which is what that interface has");
    CHECK(the_iface.ip == IP4(192, 168, 90, 20) && the_iface.mask == IP4(255, 255, 255, 0) &&
              the_iface.gateway == IP4(192, 168, 90, 1),
          "leaving the first interface where it was");
    CHECK(net_ip_addr == IP4(192, 168, 90, 20) && net_ip_gateway == IP4(192, 168, 90, 1),
          "and the machine's own address, which is the first interface's, with it");
    CHECK(arp_flushes == 0, "secondary configuration does not flush all ARP mappings");
    CHECK(arp_forgets == 1 && arp_forgot == &other_iface,
          "and only the mappings learned on that interface are forgotten");

    CHECK(netconf_set_gateway(&other_iface, IP4(10, 0, 3, 1)) == 0 &&
              other_iface.gateway == IP4(10, 0, 3, 1),
          "the second interface takes a gateway on its own subnet");
    CHECK(netconf_set_gateway(&other_iface, IP4(192, 168, 90, 1)) == -EINVAL,
          "and refuses one that is a host only on the first interface's");
    CHECK(the_iface.gateway == IP4(192, 168, 90, 1) && net_ip_gateway == IP4(192, 168, 90, 1),
          "neither of which moved the first interface's way out");

    CHECK(netconf_set_address(&other_iface, IP4(192, 168, 5, 0), 0) == 0,
          "an address that waits for a wider mask, on the second interface");
    CHECK(other_iface.ip == IP4(192, 168, 5, 0) && the_iface.ip == IP4(192, 168, 90, 20),
          "both cards hold their own addresses");
    CHECK(netconf_set_netmask(&other_iface, IP4(255, 255, 0, 0)) == 0 &&
              other_iface.ip == IP4(192, 168, 5, 0),
          "and the mask that justifies it puts it in force");
    CHECK(net_ip_addr == IP4(192, 168, 90, 20), "with the machine's address unmoved");

    CHECK(netconf_set_address(&other_iface, 0, 0) == 0 && other_iface.ip == 0 &&
              other_iface.gateway == 0,
          "unconfiguring the second takes its gateway with it");
    CHECK(the_iface.ip == IP4(192, 168, 90, 20) && net_ip_addr == IP4(192, 168, 90, 20),
          "and leaves the machine on the first");

    CHECK(netconf_set_address(0, IP4(10, 0, 4, 5), 0) == -ENODEV &&
              netconf_set_netmask(0, IP4(255, 255, 255, 0)) == -ENODEV &&
              netconf_set_gateway(0, IP4(10, 0, 4, 1)) == -ENODEV,
          "a call about no interface configures nothing");
    CHECK(net_ip_addr == IP4(192, 168, 90, 20) && net_ip_gateway == IP4(192, 168, 90, 1),
          "and leaves the machine as it was");

    CHECK(apply("rach2.ip=none", &why) == NETCONF_NONE, "`none` is a configuration of its own");
    CHECK(net_ip_addr == 0 && net_ip_mask == 0 && net_ip_gateway == 0,
          "and it leaves IPv4 unset for userland to set");
    CHECK(apply("rach2.ip=none rach2.gateway=10.0.2.2", &why) == NETCONF_REFUSED,
          "a gateway beside `none` is a route from an address that does not exist");

    CHECK(lock_depth == 0 && !lock_unbalanced,
          "every configuration change left the network lock as it found it");

    conf_reset();
    if (!fails) {
        printf("netconf-hosttest: result: pass\n");
    }
    return fails > 255 ? 255 : fails;
}
