/*
 * netconf.c: each interface's IPv4 configuration, parsed from the boot
 * command line and changed at runtime through the interface ioctls.
 *
 * The parsing is stricter than an address parser usually is, because the only
 * thing worse than refusing to configure is configuring something other than
 * what the operator wrote. `010.0.0.1` is rejected, not read as 10 or
 * as 8; a quartet over 255 is rejected, not truncated; a trailing
 * character is rejected, not ignored. A refusal of a boot-line option keeps
 * the machine on the network it already had and says so in the boot log.
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

/*
 * Primary IPv4 configuration, initialized to the SLIRP defaults.
 * These values match the first interface after configuration.
 */
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
 * Configure interfaces the boot line names by their hardware address.
 *
 * `rach2.ifaddr=52:54:00:12:34:57,10.0.3.5/24` gives that card that address,
 * and a gateway may follow after another comma. The card is named by its
 * hardware address rather than by the order the probe found it in, so a
 * machine whose cards are enumerated the other way round on the next boot
 * still puts each address on the card the operator meant.
 *
 * Report and skip each invalid token, including an oversized value or
 * an unknown hardware address. Continue with subsequent tokens.
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
            /* netconf_parse_cidr reads a whole string: copy to the comma. */
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

/** Write the machine's configuration through to the interface it belongs to. */
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

/**
 * Whether `ip` is one this stack must never take as its own or a peer's.
 *
 * The first octet decides all of it. Zero is "this network" and names no host,
 * 127 is loopback, which this stack aliases to its own address, and everything
 * from 224 up is multicast, then the reserved space above it, then the
 * all-ones broadcast at the very end. This is the one judgement of an address,
 * used by the boot line, by the ioctls and by the address cache alike, so they
 * cannot drift into disagreeing about what an address is.
 */
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
    /* A /31 or /32 has no host address beside a network and a broadcast. */
    if (prefix < 1u || prefix > 30u) {
        return 0;
    }
    m = (uint32_t)(0xFFFFFFFFu << (32u - prefix));
    if (netconf_address_is_impossible(v)) {
        return 0;
    }
    if ((v & ~m) == 0u || (v & ~m) == (~m & 0xFFFFFFFFu)) {
        return 0; /* the subnet's own network or broadcast address */
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
        return 0; /* off our subnet, so nothing could reach it */
    }
    if (gw == ip) {
        return 0;
    }
    if ((gw & ~mask) == 0u || (gw & ~mask) == (~mask & 0xFFFFFFFFu)) {
        return 0;
    }
    return 1;
}

/*
 * The two pieces of state an interface being configured keeps between one
 * ioctl and the next are in its own record, `nd->mask_provisional` and
 * `nd->pending_ip`.
 *
 * `mask_provisional` is whether the mask in force was derived, not given.
 * `ifconfig` sets an address and then a mask, two calls, and between them the
 * interface has the classful mask the address implies. The address is a host
 * under that mask, but it may be a network or broadcast address under the
 * mask still to come. If the mask to come is refused, the derived pair is
 * what would be left running, and an interface answering for a network
 * address is worse than one with no address at all. So the refusal
 * unconfigures instead.
 *
 * `pending_ip` is an address accepted but not yet in force. Some addresses
 * are hosts only under a mask wider than the classful one, and the mask is
 * the caller's next call. Putting such an address in force meanwhile would
 * leave the interface answering for a network address for as long as it takes
 * the next call to arrive, and forever if it never does, because the program
 * making the calls was killed. So it waits there instead and the interface
 * stays unconfigured, which is a state anyone can see and nobody mistakes for
 * working.
 */

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
    /* `none`: the device is probed and IPv4 left for userland to set. */
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
    net_ip_gateway = gw; /* zero when none was given, which means no route out */
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

/**
 * Whether `ip` is the all-ones address or the broadcast address of any
 * interface's subnet, not only the first's: a machine with two cards is on
 * two subnets and each has its own.
 */
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

/*
 * The classful mask an address implies, for the moment between being given an
 * address and being given a mask.
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
 * Update an interface's address and mask.
 *
 * The caller holds net_lock. On a change, drop the interface's ARP mappings
 * (every mapping, for the first interface) and notify TCP; then clear a
 * gateway that is no longer usable. Connections whose local address remains
 * configured are preserved. Changes to the first interface also update the
 * primary IPv4 globals.
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
        tcp_local_addr_changed(ip);
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
 * address, a contiguous mask short of all ones, and a host of its subnet;
 * 0, or EINVAL. */
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
    tcp_local_addr_changed(nd->ip);
    netdev_route_prune(nd);
    net_unlock(flags);
    return 0;
}

/**
 * Set `nd`'s address and mask.
 *
 * The whole change runs under net_lock, so an interrupt never finds the new
 * address published while the TCBs still claim the old one. An address of
 * zero, which a DHCP client writes when a lease ends, unconfigures the
 * interface and clears the mask. With a mask of zero the classful mask
 * stands in: an address that is a host under it goes into force at once,
 * and one that is not waits in `pending_ip`, with the interface
 * unconfigured, for the mask the caller sends next. Repeating the address in
 * force with no mask changes nothing, since a DHCP client runs the same
 * `ifconfig` on every renewal and taking the address down would abort every
 * connection each time.
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
        /* A network address and a broadcast address are not hosts. */
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
 * Set `nd`'s netmask, for the pending address when there is one.
 *
 * A mask with no address is refused, since it would give the interface a
 * subnet broadcast address derived from nothing. A refusal while the mask in
 * force is provisional, or while an address waits for its mask, unconfigures
 * the interface: it would otherwise be left on a pair nobody judged, or with
 * an address nothing will ever put in force.
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
        /* No mask is no subnet, which is what unconfiguring leaves. */
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
        /* With no subnet, netconf_gateway_ok would accept almost anything. */
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
