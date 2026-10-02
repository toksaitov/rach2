/*
 * netconf_hosttest.c: the boot command line's address, parsed and judged.
 *
 * host-deps: rach2/sys/network/netconf.c rach2/sys/cmdline.c
 * host-deps: rach2/sys/lib/kstring.c rach2/sys/lib/kctypes.c
 *
 * A wrong address here is not a crash; it is a machine that comes up on a
 * network nobody asked for, which on a remote board is indistinguishable from
 * a machine that did not come up at all. So the refusals matter more than the
 * acceptances, and most of what follows is text that must NOT be taken as an
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
#include "../../rach2/sys/cmdline.h"
#include "../../rach2/sys/lib/kerrno.h"
#include "../../rach2/sys/lib/kstring.h"
#include "../../rach2/sys/klog.h"

/* What an address change is supposed to cost, counted here. The kernel's own
   ARP cache and TCB table are not linked into a host test, so these stand in
   for them and the cases below require them to have been called. */
static int arp_flushes;
static int tcp_told;
static uint32_t tcp_told_of;
static int tcp_told_locked;

void arp_flush(void);
void tcp_local_addr_changed(uint32_t new_ip);

/* The critical section the kernel takes against its receive interrupt. A host
   test has neither, so these stand in and count that the section was entered
   and left in balance, which is the property that matters here. */
static int lock_depth;
static int lock_unbalanced;

uint32_t net_lock(void);
void net_unlock(uint32_t flags);

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
        lock_unbalanced = 1;
    }
}

void arp_flush(void)
{
    arp_flushes++;
}

void tcp_local_addr_changed(uint32_t new_ip)
{
    tcp_told++;
    tcp_told_of = new_ip;
    tcp_told_locked = lock_depth > 0;
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

/** Whether `text` is refused as an address and prefix. */
static int cidr_refused(const char *text)
{
    uint32_t ip = 0xDEADBEEFu, mask = 0xDEADBEEFu;

    if (netconf_parse_cidr(text, &ip, &mask)) {
        return 0;
    }
    /* A refusal must leave the caller's values alone, since a caller that
       ignored the return would otherwise configure whatever was half-parsed. */
    return ip == 0xDEADBEEFu && mask == 0xDEADBEEFu;
}

/** Whether `text` is refused as a bare address. */
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

/* Which addresses this machine's interfaces hold, as this test presents
 * them. */
int netdev_owns(uint32_t ip)
{
    return ip != 0 && (ip == the_iface.ip || ip == other_iface.ip);
}

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

/* The added routes, which these cases do not make. */
void netdev_route_prune(struct netdev *nd)
{
    (void)nd;
}

static int is_bcast_of(const struct netdev *nd, uint32_t ip)
{
    return nd->ip != 0 && nd->mask != 0 && ip == (nd->ip | ~nd->mask);
}

int netdev_is_broadcast(uint32_t ip)
{
    if (ip == 0xFFFFFFFFu) {
        return 1;
    }
    return is_bcast_of(&the_iface, ip) || is_bcast_of(&other_iface, ip);
}

struct netdev *netdev_first(void)
{
    return &the_iface;
}

int netdev_count(void)
{
    return 2;
}

struct netdev *netdev_at(int n)
{
    if (n == 0) {
        return &the_iface;
    }
    return n == 1 ? &other_iface : 0;
}

/* The ARP cache is not linked in here either, so a per-interface forget is
   counted the way the whole flush is. */
void arp_forget_dev(struct netdev *nd);

static int arp_forgets;
static struct netdev *arp_forgot;

void arp_forget_dev(struct netdev *nd)
{
    arp_forgets++;
    arp_forgot = nd;
}

/** Put one interface where a fresh boot leaves it. */
static void iface_reset(struct netdev *nd, uint32_t ip, uint32_t mask, uint32_t gw)
{
    nd->ip = ip;
    nd->mask = mask;
    nd->gateway = gw;
    nd->pending_ip = 0;
    nd->mask_provisional = 0;
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

static int apply(const char *cmdline, const char **why)
{
    conf_reset();
    kcmdline_capture(cmdline);
    return netconf_apply(why);
}

static int warnings;
static int oversized_warnings;

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

/** Check MAC-based configuration and recovery after rejected tokens. */
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

/** Check secondary address notifications without changing primary state. */
static void secondary_address_change(void)
{
    conf_reset();
    iface_reset(&other_iface, IP4(10, 0, 3, 7), IP4(255, 255, 255, 0), IP4(10, 0, 3, 1));
    tcp_told = arp_forgets = arp_flushes = 0;
    CHECK(netconf_set_address(&other_iface, IP4(10, 0, 4, 7), IP4(255, 255, 255, 0)) == 0,
          "secondary address changes");
    CHECK(tcp_told == 1 && tcp_told_of == IP4(10, 0, 4, 7) && tcp_told_locked,
          "secondary address change notifies TCP under the network lock");
    CHECK(arp_forgets == 1 && arp_forgot == &other_iface && arp_flushes == 0,
          "secondary address change invalidates only its ARP mappings");
    CHECK(other_iface.gateway == 0 && net_ip_addr == NET_IP_ADDR_DEFAULT &&
              net_ip_gateway == NET_IP_GATEWAY_DEFAULT,
          "secondary address change clears its gateway and preserves primary state");
    CHECK(netconf_set_address(&other_iface, other_iface.ip, other_iface.mask) == 0 && tcp_told == 1,
          "unchanged secondary address does not notify TCP");
    CHECK(netconf_set_address(&other_iface, 0, 0) == 0 && tcp_told == 2 && tcp_told_of == 0,
          "removing the secondary address notifies TCP");
}

int main(void)
{
    uint32_t ip, mask;
    const char *why;
    char text[16];

    configure_by_mac();
    secondary_address_change();
    /* ---- addresses that are addresses ---- */
    ip = 0;
    mask = 0;
    CHECK(netconf_parse_cidr("192.168.1.50/24", &ip, &mask), "a plain LAN address parses");
    CHECK(ip == IP4(192, 168, 1, 50), "the address is the one written");
    CHECK(mask == 0xFFFFFF00u, "a /24 is 255.255.255.0");

    CHECK(netconf_parse_cidr("10.0.2.15/8", &ip, &mask) && mask == 0xFF000000u,
          "a /8 is 255.0.0.0");
    CHECK(netconf_parse_cidr("172.16.5.9/30", &ip, &mask) && mask == 0xFFFFFFFCu,
          "a /30, the narrowest subnet with a host in it, is accepted");
    CHECK(netconf_parse_cidr("1.2.3.4/1", &ip, &mask) && mask == 0x80000000u,
          "a /1, the widest, is accepted");
    CHECK(netconf_parse_cidr("255.255.255.254/31", &ip, &mask) == 0,
          "a /31 leaves no host address and is refused");
    CHECK(netconf_parse_cidr("192.168.1.50/32", &ip, &mask) == 0, "a /32 is refused");
    CHECK(netconf_parse_cidr("192.168.1.50/0", &ip, &mask) == 0, "a /0 is refused");
    CHECK(cidr_refused("192.168.1.50/31"), "the /31 refusal leaves the caller's values alone");

    /* ---- text that must not become an address ---- */
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

    /* ---- addresses that exist but are not hosts ---- */
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

    /* ---- bare addresses, for the gateway ---- */
    ip = 0;
    CHECK(netconf_parse_addr("10.0.2.2", &ip) && ip == IP4(10, 0, 2, 2), "a bare address parses");
    CHECK(addr_refused("10.0.2.2/24"), "a prefix on a bare address is refused");
    CHECK(addr_refused("10.0.2"), "a short bare address is refused");
    CHECK(addr_refused("0.0.0.0"), "the all-zero gateway is refused");
    CHECK(addr_refused("127.0.0.1"), "a loopback gateway is refused");

    /* ---- a gateway has to be reachable ---- */
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

    /* ---- the tuple rule ---- */
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

    /* ---- what the boot line prints ---- */
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

    /* ---- an address set while the machine is running ---- */
    conf_reset();
    net_ip_addr = IP4(10, 0, 2, 15);
    net_ip_mask = IP4(255, 255, 255, 0);
    net_ip_gateway = IP4(10, 0, 2, 2);
    iface_reset(&the_iface, net_ip_addr, net_ip_mask, net_ip_gateway);
    arp_flushes = 0;
    tcp_told = 0;

    CHECK(netconf_set_address(&the_iface, IP4(192, 168, 90, 20), IP4(255, 255, 255, 0)) == 0,
          "a host address on a real subnet is taken");
    CHECK(net_ip_addr == IP4(192, 168, 90, 20) && net_ip_mask == IP4(255, 255, 255, 0),
          "and is what the stack now has");
    CHECK(arp_flushes == 1, "the ARP cache is forgotten, since every entry was learned "
                            "by a machine at the old address");
    CHECK(tcp_told == 1 && tcp_told_of == IP4(192, 168, 90, 20),
          "and every connection opened from the old address is told");
    CHECK(net_ip_gateway == 0, "a gateway that is no longer on this subnet is not a gateway");

    /* The refusals. Each leaves the machine on the address it had. */
    arp_flushes = 0;
    tcp_told = 0;
    CHECK(netconf_set_address(&the_iface, IP4(192, 168, 90, 0), IP4(255, 255, 255, 0)) == -EINVAL,
          "a network address is not a host");
    CHECK(netconf_set_address(&the_iface, IP4(192, 168, 90, 255), IP4(255, 255, 255, 0)) == -EINVAL,
          "nor is a broadcast address");
    CHECK(netconf_set_address(&the_iface, IP4(127, 0, 0, 1), IP4(255, 0, 0, 0)) == -EINVAL,
          "nor is loopback");
    CHECK(netconf_set_address(&the_iface, IP4(224, 0, 0, 1), IP4(255, 255, 255, 0)) == -EINVAL,
          "nor is a multicast address");
    CHECK(netconf_set_address(&the_iface, IP4(192, 168, 90, 20), IP4(255, 0, 255, 0)) == -EINVAL,
          "a mask that is not a run of ones is refused");
    CHECK(net_ip_addr == IP4(192, 168, 90, 20), "the address is unchanged by all of that");
    CHECK(arp_flushes == 0 && tcp_told == 0, "and nothing was told about a change "
                                             "that did not happen");

    /* An address with no mask yet. `ifconfig` sets the two in separate calls,
       so this state exists between them, and the address takes the classful
       mask its first byte implies until the real one arrives. */
    arp_flushes = 0;
    CHECK(netconf_set_address(&the_iface, IP4(10, 1, 2, 3), 0) == 0,
          "an address with no mask is taken");
    CHECK(net_ip_mask == IP4(255, 0, 0, 0), "with the classful mask its first byte implies");
    CHECK(netconf_set_netmask(&the_iface, IP4(255, 255, 255, 0)) == 0, "and the real mask follows");
    CHECK(net_ip_mask == IP4(255, 255, 255, 0) && net_ip_addr == IP4(10, 1, 2, 3),
          "leaving the address where it was");
    CHECK(netconf_set_netmask(&the_iface, IP4(255, 255, 255, 252)) == -EINVAL,
          "a mask that would make this address a broadcast address is refused");
    CHECK(netconf_set_netmask(&the_iface, IP4(255, 0, 255, 0)) == -EINVAL,
          "and so is one that is not a run of ones");
    CHECK(net_ip_mask == IP4(255, 255, 255, 0), "neither of which moved the one that works");

    /* And the other order, which is the one a machine booted with `none` is
       configured in: nothing set at all, then an address, then a mask. */
    CHECK(netconf_set_address(&the_iface, 0, 0) == 0, "unconfiguring first");
    CHECK(net_ip_addr == 0 && net_ip_mask == 0, "leaves nothing set");
    CHECK(netconf_set_address(&the_iface, IP4(192, 168, 90, 20), 0) == 0,
          "an address can be set with nothing configured");
    CHECK(netconf_set_netmask(&the_iface, IP4(255, 255, 255, 0)) == 0, "and then a mask");
    CHECK(net_ip_addr == IP4(192, 168, 90, 20) && net_ip_mask == IP4(255, 255, 255, 0),
          "which is a configured machine");

    /* An address that is a host only under a wider mask, and the mask that
       justifies it. The pair is not judged while the mask is the classful one
       standing in for the real one. */
    CHECK(netconf_set_address(&the_iface, IP4(192, 168, 1, 0), 0) == 0,
          "an address that is a network address under its classful mask is taken");
    CHECK(net_ip_addr == 0 && net_ip_mask == 0,
          "and waits out of force, so a client killed here leaves no machine "
          "answering for a network address");
    CHECK(netconf_set_netmask(&the_iface, IP4(255, 255, 0, 0)) == 0,
          "and the wider mask makes it a host");
    CHECK(net_ip_addr == IP4(192, 168, 1, 0) && net_ip_mask == IP4(255, 255, 0, 0),
          "which is the pair the machine ends up on");
    CHECK(netconf_set_netmask(&the_iface, IP4(255, 255, 255, 0)) == -EINVAL,
          "and narrowing it back to where the address is not a host is refused");
    CHECK(net_ip_addr == IP4(192, 168, 1, 0) && net_ip_mask == IP4(255, 255, 0, 0),
          "leaving the pair that works");

    /* Reapplying the pair the machine already has must change nothing. A DHCP
       client runs the same `ifconfig` on a renewal as on a lease, and a machine
       that tore its connections down every renewal would drop every session on
       it at the lease interval. */
    arp_flushes = 0;
    tcp_told = 0;
    CHECK(netconf_set_address(&the_iface, IP4(192, 168, 1, 0), 0) == 0, "the same address again");
    CHECK(net_ip_addr == IP4(192, 168, 1, 0) && net_ip_mask == IP4(255, 255, 0, 0),
          "leaves the pair in force");
    CHECK(arp_flushes == 0 && tcp_told == 0, "and tells nothing, because nothing changed");
    CHECK(netconf_set_netmask(&the_iface, IP4(255, 255, 0, 0)) == 0, "and so does the same mask");
    CHECK(arp_flushes == 0 && tcp_told == 0, "which is also nothing to tell");

    arp_flushes = 0;
    tcp_told = 0;
    CHECK(netconf_set_address(&the_iface, IP4(10, 0, 2, 15), IP4(255, 255, 255, 0)) == 0,
          "an ordinary address, settled");
    CHECK(netconf_set_address(&the_iface, IP4(10, 0, 2, 15), 0) == 0, "reapplied with no mask");
    CHECK(net_ip_mask == IP4(255, 255, 255, 0), "keeps the mask it had, not the classful one");
    CHECK(tcp_told == 1, "and told nothing beyond the one real change");

    /* But a refusal while the mask is still the provisional one unconfigures
     * instead of leaving the machine on a pair nobody judged. */
    CHECK(netconf_set_address(&the_iface, IP4(192, 168, 1, 0), 0) == 0,
          "the waiting address again");
    CHECK(netconf_set_netmask(&the_iface, IP4(255, 255, 255, 0)) == -EINVAL,
          "and a mask that refuses it");
    CHECK(net_ip_addr == 0 && net_ip_mask == 0,
          "leaves the machine unconfigured, not answering for a network address");

    /* Unconfiguring, which is what a client does when a lease ends. */
    arp_flushes = 0;
    tcp_told = 0;
    CHECK(netconf_set_address(&the_iface, IP4(192, 168, 90, 20), IP4(255, 255, 255, 0)) == 0,
          "a machine to unconfigure");
    arp_flushes = 0;
    tcp_told = 0;
    CHECK(netconf_set_address(&the_iface, 0, 0) == 0, "an address of zero unconfigures");
    CHECK(net_ip_addr == 0 && net_ip_mask == 0,
          "taking the mask with it, since half a configuration is not one");
    CHECK(arp_flushes == 1 && tcp_told == 1, "and that is a change like any other");
    CHECK(netconf_broadcast() == 0xFFFFFFFFu,
          "an unconfigured machine broadcasts to everyone or not at all");

    /* ---- the gateway ---- */
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

    /* ---- a second interface ---- */
    /* A machine with two cards is on two networks, and configuring one of
       them is a statement about that one. Until the setters took an interface
       every one of these calls wrote the first interface, so `ifconfig eth1`
       moved eth0 and aborted its connections. */
    conf_reset();
    CHECK(netconf_set_address(&the_iface, IP4(192, 168, 90, 20), IP4(255, 255, 255, 0)) == 0 &&
              netconf_set_gateway(&the_iface, IP4(192, 168, 90, 1)) == 0,
          "the first interface is configured");
    arp_flushes = 0;
    tcp_told = 0;
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
    CHECK(tcp_told == 1 && tcp_told_of == other_iface.ip && arp_flushes == 0,
          "secondary configuration notifies TCP without flushing all ARP mappings");
    CHECK(arp_forgets == 1 && arp_forgot == &other_iface,
          "and only the mappings learned on that interface are forgotten");

    CHECK(netconf_set_gateway(&other_iface, IP4(10, 0, 3, 1)) == 0 &&
              other_iface.gateway == IP4(10, 0, 3, 1),
          "the second interface takes a gateway on its own subnet");
    CHECK(netconf_set_gateway(&other_iface, IP4(192, 168, 90, 1)) == -EINVAL,
          "and refuses one that is a host only on the first interface's");
    CHECK(the_iface.gateway == IP4(192, 168, 90, 1) && net_ip_gateway == IP4(192, 168, 90, 1),
          "neither of which moved the first interface's way out");

    /* The address-then-mask pair is the interface's own, so one interface
       waiting for its mask cannot put the other out of force. */
    CHECK(netconf_set_address(&other_iface, IP4(192, 168, 5, 0), 0) == 0,
          "an address that waits for a wider mask, on the second interface");
    CHECK(other_iface.ip == 0 && the_iface.ip == IP4(192, 168, 90, 20),
          "waits there, and the first interface is still in force");
    CHECK(netconf_set_netmask(&other_iface, IP4(255, 255, 0, 0)) == 0 &&
              other_iface.ip == IP4(192, 168, 5, 0),
          "and the mask that justifies it puts it in force");
    CHECK(net_ip_addr == IP4(192, 168, 90, 20), "with the machine's address unmoved");

    CHECK(netconf_set_address(&other_iface, 0, 0) == 0 && other_iface.ip == 0 &&
              other_iface.gateway == 0,
          "unconfiguring the second takes its gateway with it");
    CHECK(the_iface.ip == IP4(192, 168, 90, 20) && net_ip_addr == IP4(192, 168, 90, 20),
          "and leaves the machine on the first");

    /* No interface is not the first interface. A caller with no device is a
       caller whose device is gone, and writing the machine's configuration
       for it would be answering a question about nothing. */
    CHECK(netconf_set_address(0, IP4(10, 0, 4, 5), 0) == -ENODEV &&
              netconf_set_netmask(0, IP4(255, 255, 255, 0)) == -ENODEV &&
              netconf_set_gateway(0, IP4(10, 0, 4, 1)) == -ENODEV,
          "a call about no interface configures nothing");
    CHECK(net_ip_addr == IP4(192, 168, 90, 20) && net_ip_gateway == IP4(192, 168, 90, 1),
          "and leaves the machine as it was");

    /* ---- rach2.ip=none ---- */
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
