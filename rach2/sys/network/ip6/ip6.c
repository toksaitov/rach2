/*
 * ip6.c: the IPv6 layer, as ip6.h describes it: the choice of a source
 * address, the checks of a received datagram and the transmit. The address
 * helpers and the checksum are in addr6.c.
 */

#include "../../lib/kerrno.h"
#include "../../lib/kstring.h"
#include "ip6.h"
#include "icmp6.h"
#include "../ip/ip.h"
#include "../core/byteorder.h"
#include "../ethernet/ethernet.h"
#include "../driver/netdev.h"
#include "../socket/raw.h"

struct net_mib6 net_mib6;

/** The scope of RFC 6724 of a Linux scope number: a larger number is a
 * wider scope. */
static int scope_rank(int scope)
{
    switch (scope) {
    case NETDEV_SCOPE_HOST:
        return 1;
    case NETDEV_SCOPE_LINK:
        return 2;
    case NETDEV_SCOPE_SITE:
        return 5;
    default:
        return 14;
    }
}

/**
 * Whether the address `a` is a better source for `dst`, of scope rank
 * `want`, than `b`: rule 2 of RFC 6724, the smallest scope at least as wide
 * as the destination's, or the widest when none is; then rule 3, an address
 * that is not deprecated; then the longest prefix shared with `dst`.
 */
static int better_source(const struct netdev_addr6 *a, const struct netdev_addr6 *b,
                         const uint8_t *dst, int want)
{
    int ra = scope_rank(a->scope), rb = scope_rank(b->scope);
    int da = (a->flags & NETDEV_A6_DEPRECATED) != 0, db = (b->flags & NETDEV_A6_DEPRECATED) != 0;

    if (ra != rb) {
        if (ra < rb) {
            return ra >= want;
        }
        return rb < want;
    }
    if (da != db) {
        return db;
    }
    return ip6_common_prefix(a->addr, dst) > ip6_common_prefix(b->addr, dst);
}

int ip6_source(const struct netdev *out, const uint8_t *dst, uint8_t *src)
{
    const struct netdev_addr6 *best = 0;
    int want, i;

    if (ip6_is_loopback(dst)) {
        k_memcpy(src, ip6_loopback, IP6_ADDR_LEN);
        return 0;
    }
    if (!ip6_is_multicast(dst) && netdev_owner6(dst)) {
        k_memcpy(src, dst, IP6_ADDR_LEN);
        return 0;
    }
    if (!out) {
        return -EADDRNOTAVAIL;
    }
    if (ip6_is_multicast(dst)) {
        want = scope_rank(ip6_link_scoped(dst) ? NETDEV_SCOPE_LINK : NETDEV_SCOPE_GLOBAL);
    } else {
        want = scope_rank(ip6_scope(dst));
    }
    for (i = 0; i < out->naddr6; i++) {
        const struct netdev_addr6 *a = &out->addr6[i];

        if (a->flags & (NETDEV_A6_TENTATIVE | NETDEV_A6_DADFAILED)) {
            continue;
        }
        if (!best || better_source(a, best, dst, want)) {
            best = a;
        }
    }
    if (!best) {
        return -EADDRNOTAVAIL;
    }
    k_memcpy(src, best->addr, IP6_ADDR_LEN);
    return 0;
}

/** Whether `nh` is an extension header (RFC 8200, section 4, and RFC 7045):
 * hop-by-hop and destination options, routing, fragment, mobility, HIP and
 * shim6. */
static int extension_header(uint8_t nh)
{
    return nh == IP6_NH_HOPOPTS || nh == IP6_NH_ROUTING || nh == IP6_NH_FRAGMENT ||
           nh == IP6_NH_DSTOPTS || nh == 135 || nh == 139 || nh == 140;
}

/**
 * Whether the destination `dst` of a datagram received on `nd`, NULL for
 * the loopback ring, is this machine's. A link-local address belongs to one
 * link, so it is accepted only on the interface that holds it; another
 * address of any interface is accepted on any card, as IPv4's are.
 */
static int destined_here(struct netdev *nd, const uint8_t *dst)
{
    struct netdev *holder;

    if (ip6_is_multicast(dst)) {
        return nd && netdev_member6(nd, dst);
    }
    if (!nd && ip6_is_loopback(dst)) {
        return 1;
    }
    holder = netdev_owner6(dst);
    if (!holder) {
        return 0;
    }
    return !nd || !ip6_is_linklocal(dst) || holder == nd;
}

/**
 * A datagram from the wire with this machine's source, or with ::1 at
 * either end, is a spoof no peer sends, and is dropped before any answer
 * could go to it, as `ip_rx` drops IPv4's martians. A group is no sender.
 * The interface a raw socket reports a datagram arrived on is the card's,
 * or for the loopback ring the one that holds the destination, else `lo`.
 */
void ip6_rx(struct netdev *nd, const uint8_t *data, uint16_t len)
{
    const uint8_t *src = data + 8;
    const uint8_t *dst = data + 24;
    uint16_t plen;
    uint8_t nh;
    int ifindex;

    net_mib6.ip6_in_receives++;
    if (len < IP6_HDR_SIZE || (data[0] >> 4) != 6) {
        net_mib6.ip6_in_hdr_errors++;
        return;
    }
    plen = read16be(data + 4);
    if (plen == 0 || (uint32_t)IP6_HDR_SIZE + plen > len) {
        net_mib6.ip6_in_hdr_errors++;
        return;
    }
    if (ip6_is_multicast(src) ||
        (nd && (ip6_is_loopback(src) || ip6_is_loopback(dst) || netdev_owner6(src)))) {
        net_mib6.ip6_in_addr_errors++;
        return;
    }
    if (ip6_is_multicast(dst)) {
        net_mib6.ip6_in_mcast_pkts++;
    }
    if (!destined_here(nd, dst)) {
        if (!ip6_is_multicast(dst)) {
            net_mib6.ip6_in_addr_errors++;
        }
        return;
    }

    if (nd) {
        ifindex = nd->index;
    } else if (netdev_owner6(dst)) {
        ifindex = netdev_owner6(dst)->index;
    } else {
        ifindex = netdev_loopback() ? netdev_loopback()->index : 0;
    }

    nh = data[6];
    if (nh == IP6_NH_ICMP6) {
        net_mib6.ip6_in_delivers++;
        raw6_rx(ifindex, src, nh, data + IP6_HDR_SIZE, plen);
        icmp6_rx(nd, src, dst, data[7], data + IP6_HDR_SIZE, plen);
    } else if (nh == IP6_NH_NONE) {
        net_mib6.ip6_in_delivers++;
    } else if (extension_header(nh)) {
        net_mib6.ip6_in_discards++;
    } else {
        net_mib6.ip6_in_unknown_protos++;
        icmp6_error(nd, data, (uint16_t)(IP6_HDR_SIZE + plen), ICMP6_PARAM_PROB,
                    ICMP6_PARAM_NEXTHEADER, 6);
    }
}

/* The datagram being sent. */
static uint8_t ip6_tx_buf[IP6_MTU];

/**
 * The route comes first, since the interface decides the source. A
 * datagram for this machine goes round the loopback ring, whose drain gives
 * it to `ip6_rx`, so that a send never runs the receive path inside the
 * sender's own call, as `ip_tx_from` keeps IPv4's. A source of ::1 may
 * reach only this machine, which no card may carry.
 */
int ip6_tx(const uint8_t *src, const uint8_t *dst, int ifindex, uint8_t nh, const uint8_t *payload,
           uint16_t plen, uint8_t hlim)
{
    uint8_t via[IP6_ADDR_LEN], from[IP6_ADDR_LEN], mac[6];
    uint32_t total = (uint32_t)IP6_HDR_SIZE + plen;
    struct netdev *out;
    int sent;

    net_mib6.ip6_out_requests++;
    if (ip6_is_unspecified(dst) || ip6_is_v4mapped(dst)) {
        return -EINVAL;
    }
    out = ip6_route(dst, ifindex, via);
    if (!out) {
        if (ifindex == 0 && ip6_link_scoped(dst) && netdev_count() > 1) {
            return -EINVAL;
        }
        return -ENETUNREACH;
    }
    if (total > ip6_path_mtu(out, dst)) {
        return -EMSGSIZE;
    }
    if (!src || ip6_is_unspecified(src)) {
        int r = ip6_source(out, dst, from);

        if (r != 0) {
            return r;
        }
    } else {
        if (!ip6_is_loopback(src) && !netdev_owner6(src)) {
            return -EINVAL;
        }
        if (ip6_is_loopback(src) && !out->loopback) {
            return -EINVAL;
        }
        k_memcpy(from, src, IP6_ADDR_LEN);
    }

    ip6_tx_buf[0] = 0x60; /* version 6, traffic class 0 */
    ip6_tx_buf[1] = 0;
    ip6_tx_buf[2] = 0; /* flow label 0 */
    ip6_tx_buf[3] = 0;
    write16be(ip6_tx_buf + 4, plen);
    ip6_tx_buf[6] = nh;
    ip6_tx_buf[7] = hlim;
    k_memcpy(ip6_tx_buf + 8, from, IP6_ADDR_LEN);
    k_memcpy(ip6_tx_buf + 24, dst, IP6_ADDR_LEN);
    k_memcpy(ip6_tx_buf + IP6_HDR_SIZE, payload, plen);

    if (out->loopback) {
        if (ip_loop_put(ip6_tx_buf, (uint16_t)total) != 0) {
            net_mib6.ip6_out_discards++;
            return -ENOBUFS;
        }
        return 0;
    }
    if (!ip6_is_multicast(via)) {
        /* No neighbour cache: no unicast next hop's station address is known. */
        return -EHOSTUNREACH;
    }
    net_mib6.ip6_out_mcast_pkts++;
    ip6_mcast_mac(via, mac);
    sent = ethernet_tx(out, mac, ETH_TYPE_IPV6, ip6_tx_buf, (uint16_t)total);
    if (sent == ETHERNET_TX_TOOLONG) {
        return -EMSGSIZE;
    }
    if (sent != NETDEV_TX_OK) {
        net_mib6.ip6_out_discards++;
        return -ENOBUFS;
    }
    return 0;
}
