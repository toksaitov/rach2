/*
 * netconf.c: each interface's IPv4 configuration, as netconf.h describes it,
 * parsed from the boot line and changed at run time through the interface
 * ioctls and netlink.
 *
 * The parser is strict, so that the address configured is the one the
 * operator wrote: `010.0.0.1` is refused rather than read as 10 or as 8, a
 * quartet over 255 is refused rather than truncated, and a trailing
 * character is refused rather than ignored. A refused boot-line option
 * leaves the machine on the network it already had, and is logged.
 */

#include "netconf.h"
#include "net.h"
#include "driver/netdev.h"
#include "arp/arp.h"
#include "tcp/tcp.h"
#include "../cmdline.h"
#include "../klog.h"
#include "../lib/kerrno.h"
#include "../lib/kstring.h"

/* The first interface's configuration, the SLIRP network until the boot line
 * or a program gives another. */
uint32_t net_ip_addr = NET_IP_ADDR_DEFAULT;
uint32_t net_ip_mask = NET_IP_MASK_DEFAULT;
uint32_t net_ip_gateway = NET_IP_GATEWAY_DEFAULT;

/**
 * Read one two-digit hexadecimal byte, advancing `*s` past it.
 *
 * @return 1 on success, 0 on anything that is not two hexadecimal digits.
 */
static int parse_hex_byte(const char **s, uint8_t *out)
{
    const char *p = *s;
    uint32_t v = 0;
    int i;

    for (i = 0; i < 2; i++) {
        char c = p[i];
        int d;

        if (c >= '0' && c <= '9') {
            d = c - '0';
        } else if (c >= 'a' && c <= 'f') {
            d = c - 'a' + 10;
        } else if (c >= 'A' && c <= 'F') {
            d = c - 'A' + 10;
        } else {
            return 0;
        }
        v = (v << 4) | (uint32_t)d;
    }
    *out = (uint8_t)v;
    *s = p + 2;
    return 1;
}

/**
 * Read a hardware address, six hexadecimal bytes separated by colons.
 *
 * @return 1 on success with the six bytes in `out`, 0 on anything else.
 */
static int parse_mac(const char **s, uint8_t *out)
{
    int i;

    for (i = 0; i < 6; i++) {
        if (i != 0) {
            if (**s != ':') {
                return 0;
            }
            (*s)++;
        }
        if (!parse_hex_byte(s, &out[i])) {
            return 0;
        }
    }
    return 1;
}

/** Whether two hardware addresses are the same. */
static int mac_equal(const uint8_t *a, const uint8_t *b)
{
    int i;

    for (i = 0; i < 6; i++) {
        if (a[i] != b[i]) {
            return 0;
        }
    }
    return 1;
}

/**
 * `rach2.ifaddr=52:54:00:12:34:57,10.0.3.5/24` gives that card that address,
 * and a gateway may follow after another comma. A card found in another
 * order on the next boot still takes the address the operator meant for it.
 * The address and prefix are copied out to the comma before they are parsed,
 * since `netconf_parse_cidr` reads a whole string. The configuration is
 * written into the card's record directly, without what `addr_publish`
 * drops, since this runs at boot, before any program has opened a socket.
 */
void netconf_apply_by_mac(void)
{
    char value[80];
    int n;
    int count = kcmdline_count("rach2.ifaddr");

    for (n = 0; n < count; n++) {
        const char *p = value;
        uint8_t mac[6];
        uint32_t ip = 0, mask = 0, gw = 0;
        struct netdev *nd = 0;
        int i;

        if (!kcmdline_value_nth("rach2.ifaddr", n, value, (unsigned int)sizeof(value))) {
            klog(KLOG_WARN, "net", "ifaddr: value too long");
            continue;
        }
        if (!parse_mac(&p, mac) || *p != ',') {
            klog(KLOG_WARN, "net", "ifaddr: %s: not a hardware address", value);
            continue;
        }
        p++;
        {
            char cidr[24];
            int c = 0;

            while (p[c] != '\0' && p[c] != ',' && c < (int)sizeof(cidr) - 1) {
                cidr[c] = p[c];
                c++;
            }
            cidr[c] = '\0';
            p += c;
            if (!netconf_parse_cidr(cidr, &ip, &mask)) {
                klog(KLOG_WARN, "net", "ifaddr: %s: not an address and prefix", value);
                continue;
            }
        }
        if (*p == ',') {
            p++;
            if (!netconf_parse_addr(p, &gw)) {
                klog(KLOG_WARN, "net", "ifaddr: %s: not a gateway", value);
                continue;
            }
            if (!netconf_gateway_ok(gw, ip, mask)) {
                klog(KLOG_WARN, "net", "ifaddr: %s: the gateway is not on that subnet", value);
                continue;
            }
        }
        for (i = 0; i < netdev_count(); i++) {
            if (mac_equal(netdev_at(i)->mac, mac)) {
                nd = netdev_at(i);
                break;
            }
        }
        if (!nd) {
            klog(KLOG_WARN, "net", "ifaddr: %s: no card of this machine has that address", value);
            continue;
        }
        nd->ip = ip;
        nd->mask = mask;
        nd->gateway = gw;
        if (nd == netdev_first()) {
            net_ip_addr = ip;
            net_ip_mask = mask;
            net_ip_gateway = gw;
        }
        klog(KLOG_INFO, "net", "%s: configured by its station address", nd->name);
    }
}

void netconf_publish(void)
{
    struct netdev *nd = netdev_first();

    if (nd) {
        nd->ip = net_ip_addr;
        nd->mask = net_ip_mask;
        nd->gateway = net_ip_gateway;
    }
}

/**
 * Read one decimal quartet, 0 to 255, advancing `*s` past it.
 *
 * A leading zero on a multi-digit number is refused: it reads as octal in
 * enough other places that the same text would mean two different addresses to
 * two readers.
 *
 * @return 1 on success, 0 on a malformed or out-of-range quartet.
 */
static int parse_quartet(const char **s, uint32_t *out)
{
    const char *p = *s;
    uint32_t v = 0;
    int digits = 0;

    if (*p < '0' || *p > '9') {
        return 0;
    }
    if (*p == '0' && p[1] >= '0' && p[1] <= '9') {
        return 0;
    }
    while (*p >= '0' && *p <= '9') {
        v = v * 10u + (uint32_t)(*p - '0');
        if (v > 255u) {
            return 0;
        }
        p++;
        digits++;
        if (digits > 3) {
            return 0;
        }
    }
    *s = p;
    *out = v;
    return 1;
}

/**
 * Read `A.B.C.D`, advancing `*s` past it, without judging the result.
 *
 * @return 1 on a well-formed dotted quad, 0 otherwise.
 */
static int parse_quad(const char **s, uint32_t *ip)
{
    uint32_t part[4];
    int i;

    for (i = 0; i < 4; i++) {
        if (i > 0) {
            if (**s != '.') {
                return 0;
            }
            (*s)++;
        }
        if (!parse_quartet(s, &part[i])) {
            return 0;
        }
    }
    *ip = (part[0] << 24) | (part[1] << 16) | (part[2] << 8) | part[3];
    return 1;
}

int netconf_address_is_impossible(uint32_t ip)
{
    uint32_t top = ip >> 24;

    return top == 0u || top == 127u || top >= 224u;
}

int netconf_parse_addr(const char *s, uint32_t *ip)
{
    uint32_t v;

    if (!parse_quad(&s, &v) || *s != '\0') {
        return 0;
    }
    if (netconf_address_is_impossible(v)) {
        return 0;
    }
    *ip = v;
    return 1;
}

int netconf_parse_cidr(const char *s, uint32_t *ip, uint32_t *mask)
{
    uint32_t v, prefix, m;

    if (!parse_quad(&s, &v)) {
        return 0;
    }
    if (*s != '/') {
        return 0;
    }
    s++;
    if (!parse_quartet(&s, &prefix) || *s != '\0') {
        return 0;
    }
    if (prefix < 1u || prefix > 30u) {
        return 0;
    }
    m = (uint32_t)(0xFFFFFFFFu << (32u - prefix));
    if (netconf_address_is_impossible(v)) {
        return 0;
    }
    if ((v & ~m) == 0u || (v & ~m) == (~m & 0xFFFFFFFFu)) {
        return 0;
    }
    *ip = v;
    *mask = m;
    return 1;
}

int netconf_gateway_ok(uint32_t gw, uint32_t ip, uint32_t mask)
{
    if (netconf_address_is_impossible(gw)) {
        return 0;
    }
    if ((gw & mask) != (ip & mask)) {
        return 0;
    }
    if (gw == ip) {
        return 0;
    }
    if ((gw & ~mask) == 0u || (gw & ~mask) == (~mask & 0xFFFFFFFFu)) {
        return 0;
    }
    return 1;
}

int netconf_apply(const char **why)
{
    char value[40];
    uint32_t ip = 0, mask = 0, gw = 0;
    int have_ip, have_gw;

    *why = "";
    have_ip = kcmdline_count("rach2.ip") > 0;
    have_gw = kcmdline_count("rach2.gateway") > 0;

    if (!have_ip && !have_gw) {
        return NETCONF_DEFAULT;
    }
    if (kcmdline_count("rach2.ip") > 1 || kcmdline_count("rach2.gateway") > 1) {
        *why = "the address was given more than once";
        return NETCONF_REFUSED;
    }
    if (!have_ip) {
        *why = "a gateway was given with no address to reach it from";
        return NETCONF_REFUSED;
    }
    if (!kcmdline_value("rach2.ip", value, (unsigned int)sizeof(value))) {
        *why = "the address is longer than an address can be";
        return NETCONF_REFUSED;
    }
    if (value[0] == 'n' && value[1] == 'o' && value[2] == 'n' && value[3] == 'e' &&
        value[4] == '\0') {
        if (have_gw) {
            *why = "a gateway was given with no address to reach it from";
            return NETCONF_REFUSED;
        }
        net_ip_addr = 0;
        net_ip_mask = 0;
        net_ip_gateway = 0;
        netconf_publish();
        return NETCONF_NONE;
    }
    if (!netconf_parse_cidr(value, &ip, &mask)) {
        *why = "the address is not a usable A.B.C.D/prefix";
        return NETCONF_REFUSED;
    }
    if (have_gw) {
        if (!kcmdline_value("rach2.gateway", value, (unsigned int)sizeof(value))) {
            *why = "the gateway is longer than an address can be";
            return NETCONF_REFUSED;
        }
        if (!netconf_parse_addr(value, &gw)) {
            *why = "the gateway is not a usable A.B.C.D";
            return NETCONF_REFUSED;
        }
        if (!netconf_gateway_ok(gw, ip, mask)) {
            *why = "the gateway is not a host on the given subnet";
            return NETCONF_REFUSED;
        }
    }

    net_ip_addr = ip;
    net_ip_mask = mask;
    net_ip_gateway = gw;
    netconf_publish();
    return NETCONF_APPLIED;
}

/** Append `v`, 0 to 255, to `out` and return how many characters that took. */
static int put_quartet(char *out, uint32_t v)
{
    int n = 0;

    if (v >= 100u) {
        out[n++] = (char)('0' + (v / 100u));
    }
    if (v >= 10u) {
        out[n++] = (char)('0' + ((v / 10u) % 10u));
    }
    out[n++] = (char)('0' + (v % 10u));
    return n;
}

void netconf_format(uint32_t ip, char *out)
{
    int n = 0;
    int i;

    for (i = 3; i >= 0; i--) {
        n += put_quartet(out + n, (ip >> (i * 8)) & 0xFFu);
        if (i > 0) {
            out[n++] = '.';
        }
    }
    out[n] = '\0';
}

int netconf_prefix_of(uint32_t mask)
{
    int n = 0;

    while (mask & 0x80000000u) {
        n++;
        mask <<= 1;
    }
    return n;
}

int netconf_is_broadcast(uint32_t ip)
{
    return netdev_is_broadcast(ip);
}

/** Whether `mask` is a run of ones followed by a run of zeros. */
static int mask_ok(uint32_t mask)
{
    uint32_t inverted = ~mask;

    return (inverted & (inverted + 1u)) == 0u;
}

uint32_t netconf_broadcast(void)
{
    if (net_ip_addr == 0 || net_ip_mask == 0) {
        return 0xFFFFFFFFu;
    }
    return net_ip_addr | ~net_ip_mask;
}

/**
 * The classful mask an address implies, for the moment between being given an
 * address and being given a mask: 255.0.0.0 below 128, 255.255.0.0 below 192,
 * and 255.255.255.0 from 192 up.
 *
 * `ifconfig eth0 A.B.C.D` is one ioctl and `netmask M.M.M.M` is the next, so
 * there is a moment with an address and no mask, and a machine that refused to
 * hold it could never be configured at all: the mask ioctl would have no
 * address to judge against and the address ioctl no mask. Linux fills the gap
 * the same way, with the class of the first byte.
 */
static uint32_t classful_mask(uint32_t ip)
{
    uint32_t top = ip >> 24;

    if (top < 128u) {
        return 0xFF000000u;
    }
    if (top < 192u) {
        return 0xFFFF0000u;
    }
    return 0xFFFFFF00u;
}

/** Return whether `nd` is the first interface. */
static int is_machine(const struct netdev *nd)
{
    return nd != 0 && nd == netdev_first();
}

/**
 * Put `ip`/`mask` in force on `nd`, with the costs `netconf_set_address`
 * lists.
 *
 * A change, a new address or a new mask under an address, is logged, drops
 * the ARP mappings of the interface but the permanent ones (of every
 * interface, for the first) and the datagrams waiting for them, and aborts
 * the TCP connections whose local address the machine no longer has; a
 * connection on an address still held is kept. A gateway the subnet does
 * not have is cleared, change or not, and so are the added routes whose
 * gateway `nd` no longer reaches. The first interface's values are copied to
 * `net_ip_addr` and the two after it. The caller holds `net_lock`.
 */
static void addr_publish(struct netdev *nd, uint32_t ip, uint32_t mask)
{
    uint32_t old = nd->ip;
    uint32_t old_mask = nd->mask;

    nd->ip = ip;
    nd->mask = mask;
    if (is_machine(nd)) {
        net_ip_addr = ip;
        net_ip_mask = mask;
    }

    if (ip != old || (ip != 0 && mask != old_mask)) {
        char addr[16];

        if (ip != 0) {
            netconf_format(ip, addr);
            klog(KLOG_INFO, "net", "%s: IPv4 %s/%d", nd->name, addr, netconf_prefix_of(mask));
        } else {
            klog(KLOG_INFO, "net", "%s: IPv4 none", nd->name);
        }
        if (is_machine(nd)) {
            arp_flush();
        } else {
            arp_forget_dev(nd);
        }
        tcp_local_addr_changed();
    }
    if (nd->gateway != 0 &&
        (nd->ip == 0 || nd->mask == 0 || !netconf_gateway_ok(nd->gateway, nd->ip, nd->mask))) {
        nd->gateway = 0;
    }
    if (is_machine(nd)) {
        net_ip_gateway = nd->gateway;
    }
    netdev_route_prune(nd);
}

/** Whether `ip`/`mask` may be an address of an interface: a possible
 * address, a contiguous mask other than 0 and all ones, and a host of its
 * subnet; 0, or -EINVAL. */
static int addr_valid(uint32_t ip, uint32_t mask)
{
    if (netconf_address_is_impossible(ip) || !mask_ok(mask) || mask == 0 || mask == 0xFFFFFFFFu ||
        ip == (ip & mask) || ip == (ip | ~mask)) {
        return -EINVAL;
    }
    return 0;
}

int netconf_add_address(struct netdev *nd, uint32_t ip, uint32_t mask)
{
    uint32_t flags;
    int rc = addr_valid(ip, mask);

    if (rc != 0) {
        return rc;
    }
    if (nd->ip == 0) {
        return netconf_set_address(nd, ip, mask);
    }
    flags = net_lock();
    if (netdev_holds(nd, ip)) {
        rc = -EEXIST;
    } else if (nd->naddr2 == NETDEV_ADDRS) {
        rc = -ENOSPC;
    } else {
        char text[16];

        nd->ip2[nd->naddr2] = ip;
        nd->mask2[nd->naddr2] = mask;
        nd->naddr2++;
        netconf_format(ip, text);
        klog(KLOG_INFO, "net", "%s: IPv4 %s/%d added", nd->name, text, netconf_prefix_of(mask));
    }
    net_unlock(flags);
    return rc;
}

int netconf_del_address(struct netdev *nd, uint32_t ip)
{
    uint32_t flags;
    int i;

    if (ip == 0 || ip == nd->ip) {
        uint32_t next = nd->naddr2 > 0 ? nd->ip2[0] : 0;
        uint32_t next_mask = nd->naddr2 > 0 ? nd->mask2[0] : 0;

        if (nd->ip == 0) {
            return -EADDRNOTAVAIL;
        }
        flags = net_lock();
        for (i = 1; i < nd->naddr2; i++) {
            nd->ip2[i - 1] = nd->ip2[i];
            nd->mask2[i - 1] = nd->mask2[i];
        }
        nd->naddr2 -= nd->naddr2 > 0;
        addr_publish(nd, next, next_mask);
        nd->mask_provisional = 0;
        nd->pending_ip = 0;
        net_unlock(flags);
        return 0;
    }
    flags = net_lock();
    for (i = 0; i < nd->naddr2 && nd->ip2[i] != ip; i++) {
    }
    if (i == nd->naddr2) {
        net_unlock(flags);
        return -EADDRNOTAVAIL;
    }
    for (; i + 1 < nd->naddr2; i++) {
        nd->ip2[i] = nd->ip2[i + 1];
        nd->mask2[i] = nd->mask2[i + 1];
    }
    nd->naddr2--;
    tcp_local_addr_changed();
    netdev_route_prune(nd);
    net_unlock(flags);
    return 0;
}

/**
 * The whole change runs under `net_lock`, so an interrupt never finds the new
 * address in force while the control blocks still record the old one. A DHCP
 * client writes an address of 0 when a lease ends.
 *
 * With a mask of 0 the classful mask stands in, and `mask_provisional`
 * records that it was derived and not given. An address that is a host under
 * it goes into force at once. One that is not, a host only under a wider mask
 * still to come, waits in `pending_ip` with the interface unconfigured: put
 * in force, it would leave the interface answering for a network or
 * broadcast address until the next call, and for good if the program making
 * the calls were killed before it. An unconfigured interface is a state
 * anyone can see and nobody takes for a working one.
 *
 * The address in force repeated with no mask changes nothing, since a DHCP
 * client runs the same `ifconfig` at every renewal, and taking the address
 * down each time would abort every connection.
 */
int netconf_set_address(struct netdev *nd, uint32_t ip, uint32_t mask)
{
    uint32_t flags;
    int rc = 0;

    if (!nd) {
        return -ENODEV;
    }

    flags = net_lock();

    if (ip == 0) {
        addr_publish(nd, 0, 0);
        nd->mask_provisional = 0;
        nd->pending_ip = 0;
        net_unlock(flags);
        return 0;
    }

    if (netconf_address_is_impossible(ip)) {
        net_unlock(flags);
        return -EINVAL;
    }

    if (mask == 0) {
        uint32_t derived = classful_mask(ip);

        if (ip == nd->ip && nd->mask != 0 && !nd->mask_provisional && nd->pending_ip == 0) {
            net_unlock(flags);
            return 0;
        }

        if (ip == (ip & derived) || ip == (ip | ~derived)) {
            addr_publish(nd, 0, 0);
            nd->pending_ip = ip;
        } else {
            addr_publish(nd, ip, derived);
            nd->pending_ip = 0;
        }
        nd->mask_provisional = 1;
        net_unlock(flags);
        return 0;
    }

    if (!mask_ok(mask) || mask == 0xFFFFFFFFu) {
        rc = -EINVAL;
    } else if (ip == (ip & mask) || ip == (ip | ~mask)) {
        rc = -EINVAL;
    }
    if (rc == 0) {
        addr_publish(nd, ip, mask);
        nd->mask_provisional = 0;
        nd->pending_ip = 0;
    }
    net_unlock(flags);
    return rc;
}

/**
 * A mask with no address is refused, since it would give the interface a
 * subnet broadcast address derived from nothing. A refusal while the mask in
 * force is provisional unconfigures the interface: the address is a host
 * under the classful mask but may be the network or broadcast address of the
 * subnet the caller meant, and an interface answering for such an address is
 * worse than one with no address at all. A refusal while an address waits
 * for its mask unconfigures the interface too, since nothing would put that
 * address in force.
 */
int netconf_set_netmask(struct netdev *nd, uint32_t mask)
{
    uint32_t flags;
    uint32_t ip;
    int rc = 0;

    if (!nd) {
        return -ENODEV;
    }

    flags = net_lock();
    ip = nd->pending_ip ? nd->pending_ip : nd->ip;

    if (mask == 0) {
        addr_publish(nd, nd->pending_ip ? 0 : nd->ip, 0);
        nd->mask_provisional = 0;
        nd->pending_ip = 0;
    } else if (ip == 0) {
        rc = -EINVAL;
    } else if (!mask_ok(mask) || mask == 0xFFFFFFFFu) {
        rc = -EINVAL;
    } else if (ip == (ip & mask) || ip == (ip | ~mask)) {
        rc = -EINVAL;
    } else {
        addr_publish(nd, ip, mask);
        nd->mask_provisional = 0;
        nd->pending_ip = 0;
    }

    if (rc != 0 && (nd->mask_provisional || nd->pending_ip)) {
        addr_publish(nd, 0, 0);
        nd->mask_provisional = 0;
        nd->pending_ip = 0;
    }
    net_unlock(flags);
    return rc;
}

/** With no mask `netconf_gateway_ok` would take almost any address, so a
 * gateway needs the interface's address and mask first. */
int netconf_set_gateway(struct netdev *nd, uint32_t gw)
{
    uint32_t flags;
    int rc = 0;

    if (!nd) {
        return -ENODEV;
    }

    flags = net_lock();
    if (gw == 0) {
        nd->gateway = 0;
    } else if (nd->ip == 0 || nd->mask == 0) {
        rc = -EINVAL;
    } else if (!netconf_gateway_ok(gw, nd->ip, nd->mask)) {
        rc = -EINVAL;
    } else {
        nd->gateway = gw;
    }
    if (is_machine(nd)) {
        net_ip_gateway = nd->gateway;
    }
    net_unlock(flags);
    return rc;
}
