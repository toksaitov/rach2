/*
 * ip6.c: the IPv6 layer, as ip6.h describes it: the choice of a source
 * address, the checks of a received datagram and of its extension headers,
 * and the transmit. The address helpers and the checksum are in addr6.c,
 * fragments in frag6.c and the path MTU in pmtu6.c.
 */

#include "../../lib/kerrno.h"
#include "../../lib/kstring.h"
#include "ip6.h"
#include "../tcp/tcp.h"
#include "../udp/udp.h"
#include "icmp6.h"
#include "frag6.h"
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

/* The option types every node knows (RFC 8200, 4.2): one byte of padding,
 * and N bytes. */
#define IP6_OPT_PAD1 0
#define IP6_OPT_PADN 1

/** The datagram `dg` of `len` bytes as `icmp6_error` takes it, its length
 * held to 16 bits: an error quotes no more than 1232 bytes of it. */
static void ip6_param_problem(struct netdev *nd, const uint8_t *dg, uint32_t len, uint8_t code,
                              uint32_t pointer)
{
    icmp6_error(nd, dg, (uint16_t)(len > 0xffffu ? 0xffffu : len), ICMP6_PARAM_PROB, code, pointer);
}

/**
 * Process the options of the hop-by-hop or destination options header of
 * `hlen` bytes at offset `off` of `dg`. Padding is skipped; an option of
 * another type is one this machine does not know, and the top two bits of
 * its type say what to do (RFC 8200, 4.2): 00 skip it, 01 drop the
 * datagram, 10 drop it and send a parameter problem of code 2 pointing at
 * the type, and 11 do so only when the datagram was not sent to a group.
 * Router Alert, which multicast listener reports carry, is of the first
 * kind. Returns 0 to go on, or -1 when the datagram is dropped; an option
 * that runs past the header counts as a header error.
 */
static int ip6_options(struct netdev *nd, const uint8_t *dg, uint32_t len, uint32_t off,
                       uint32_t hlen)
{
    uint32_t p = off + 2, end = off + hlen;

    while (p < end) {
        uint8_t type = dg[p];

        if (type == IP6_OPT_PAD1) {
            p++;
            continue;
        }
        if (end - p < 2 || (uint32_t)dg[p + 1] + 2 > end - p) {
            net_mib6.ip6_in_hdr_errors++;
            return -1;
        }
        if (type != IP6_OPT_PADN) {
            switch (type >> 6) {
            case 0:
                break;
            case 1:
                net_mib6.ip6_in_hdr_errors++;
                return -1;
            case 3:
                if (ip6_is_multicast(dg + 24)) {
                    net_mib6.ip6_in_hdr_errors++;
                    return -1;
                }
                /* fall through */
            default:
                net_mib6.ip6_in_hdr_errors++;
                ip6_param_problem(nd, dg, len, ICMP6_PARAM_OPTION, p);
                return -1;
            }
        }
        p += (uint32_t)dg[p + 1] + 2;
    }
    return 0;
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
 * What lies past the payload length, an Ethernet frame's padding, is not
 * the datagram's.
 */
void ip6_rx(struct netdev *nd, const uint8_t *data, uint16_t len)
{
    const uint8_t *src = data + 8;
    const uint8_t *dst = data + 24;
    uint16_t plen;

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

    ip6_walk(nd, data, (uint32_t)IP6_HDR_SIZE + plen, IP6_HDR_SIZE, data[6], 6, 0);
}

/**
 * The headers come one after another, each naming the next, until an
 * upper layer's or the end of the chain. Every extension header is a
 * multiple of 8 bytes and at least 8 long, so the walk moves forward at
 * each step and ends within the datagram. The interface a raw socket
 * reports a message arrived on is the card's, or for the loopback ring the
 * one that holds the destination, else `lo`.
 */
void ip6_walk(struct netdev *nd, const uint8_t *dg, uint32_t len, uint32_t off, uint8_t nh,
              uint32_t nhoff, int reassembled)
{
    const uint8_t *dst = dg + 24;
    uint32_t hlen;
    int ifindex;

    for (;;) {
        switch (nh) {
        case IP6_NH_HOPOPTS:
            if (off != IP6_HDR_SIZE) {
                net_mib6.ip6_in_unknown_protos++;
                ip6_param_problem(nd, dg, len, ICMP6_PARAM_NEXTHEADER, nhoff);
                return;
            }
            /* fall through */
        case IP6_NH_DSTOPTS:
        case IP6_NH_ROUTING:
            if (len - off < 8 || (hlen = ((uint32_t)dg[off + 1] + 1) * 8) > len - off) {
                net_mib6.ip6_in_hdr_errors++;
                return;
            }
            if (nh != IP6_NH_ROUTING) {
                if (ip6_options(nd, dg, len, off, hlen) != 0) {
                    return;
                }
            } else if (dg[off + 3] != 0) {
                /* Segments left: this machine forwards nothing, so no
                 * routing type is one it can act on (RFC 5095 for type 0). */
                net_mib6.ip6_in_hdr_errors++;
                ip6_param_problem(nd, dg, len, ICMP6_PARAM_HEADER, off + 2);
                return;
            }
            nhoff = off;
            nh = dg[off];
            off += hlen;
            break;
        case IP6_NH_FRAGMENT:
            if (len - off < IP6_FRAG_HDR_SIZE || reassembled) {
                net_mib6.ip6_in_hdr_errors++;
                return;
            }
            if ((read16be(dg + off + 2) & (IP6_FRAG_OFFSET | IP6_FRAG_MORE)) != 0) {
                frag6_input(nd, dg, len, off, nhoff);
                return;
            }
            /* An atomic fragment, at offset 0 with no more to come, is the
             * whole datagram, and needs no queue (RFC 6946). */
            net_mib6.ip6_reasm_reqds++;
            net_mib6.ip6_reasm_oks++;
            nhoff = off;
            nh = dg[off];
            off += IP6_FRAG_HDR_SIZE;
            break;
        case IP6_NH_NONE:
            net_mib6.ip6_in_delivers++;
            return;
        case IP6_NH_ICMP6:
        case IP6_NH_TCP:
        case IP6_NH_UDP:
            if (nd) {
                ifindex = nd->index;
            } else if (netdev_owner6(dst)) {
                ifindex = netdev_owner6(dst)->index;
            } else {
                ifindex = netdev_loopback() ? netdev_loopback()->index : 0;
            }
            net_mib6.ip6_in_delivers++;
            raw6_rx(ifindex, dg + 8, nh, dg + off, (uint16_t)(len - off));
            if (nh == IP6_NH_TCP) {
                tcp6_rx(ifindex, dg + 8, dst, dg + off, (uint16_t)(len - off));
            } else if (nh == IP6_NH_UDP) {
                if (udp6_rx(ifindex, dg + 8, dst, dg + off, (uint16_t)(len - off)) < 0) {
                    uint16_t quote = (uint16_t)(len > 1280 ? 1280 : len);
                    icmp6_error(nd, dg, quote, ICMP6_DST_UNREACH, 4, 0);
                }
            } else {
                icmp6_rx(nd, dg + 8, dst, dg[7], dg + off, (uint16_t)(len - off));
            }
            return;
        default:
            net_mib6.ip6_in_unknown_protos++;
            ip6_param_problem(nd, dg, len, ICMP6_PARAM_NEXTHEADER, nhoff);
            return;
        }
    }
}

void ip6_header(uint8_t *buf, const uint8_t *src, const uint8_t *dst, uint8_t nh, uint16_t plen,
                uint8_t hlim)
{
    buf[0] = 0x60; /* version 6, traffic class 0 */
    buf[1] = 0;
    buf[2] = 0; /* flow label 0 */
    buf[3] = 0;
    write16be(buf + 4, plen);
    buf[6] = nh;
    buf[7] = hlim;
    k_memcpy(buf + 8, src, IP6_ADDR_LEN);
    k_memcpy(buf + 24, dst, IP6_ADDR_LEN);
}

int ip6_emit(struct netdev *out, const uint8_t *via, const uint8_t *buf, uint32_t len)
{
    uint8_t mac[6];
    int sent;

    if (out->loopback) {
        if (ip_loop_put(buf, (uint16_t)len) != 0) {
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
    sent = ethernet_tx(out, mac, ETH_TYPE_IPV6, buf, (uint16_t)len);
    if (sent == ETHERNET_TX_TOOLONG) {
        return -EMSGSIZE;
    }
    if (sent != NETDEV_TX_OK) {
        net_mib6.ip6_out_discards++;
        return -ENOBUFS;
    }
    return 0;
}

void ip6_tick(void)
{
    frag6_tick();
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
    uint8_t via[IP6_ADDR_LEN], from[IP6_ADDR_LEN];
    uint32_t total = (uint32_t)IP6_HDR_SIZE + plen;
    uint32_t mtu;
    struct netdev *out;

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

    mtu = ip6_path_mtu(out, dst);
    if (total > mtu) {
        return frag6_output(out, via, from, dst, nh, payload, plen, hlim, mtu);
    }
    ip6_header(ip6_tx_buf, from, dst, nh, plen, hlim);
    k_memcpy(ip6_tx_buf + IP6_HDR_SIZE, payload, plen);
    return ip6_emit(out, via, ip6_tx_buf, total);
}

/** No source is chosen and nothing in the header is checked against this
 * machine's addresses: the caller is privileged, and a datagram that names
 * another source on purpose is the point of such a socket. */
int ip6_tx_whole(const uint8_t *dg, uint32_t len, const uint8_t *dst, int ifindex)
{
    uint8_t via[IP6_ADDR_LEN];
    struct netdev *out;

    if (len < IP6_HDR_SIZE || (dg[0] >> 4) != 6 ||
        (uint32_t)read16be(dg + 4) + IP6_HDR_SIZE != len) {
        return -EINVAL;
    }
    net_mib6.ip6_out_requests++;
    if (ip6_is_unspecified(dst) || ip6_is_v4mapped(dst)) {
        return -EINVAL;
    }
    out = ip6_route(dst, ifindex, via);
    if (!out) {
        return ifindex == 0 && ip6_link_scoped(dst) && netdev_count() > 1 ? -EINVAL : -ENETUNREACH;
    }
    if (len > ip6_path_mtu(out, dst)) {
        return -EMSGSIZE;
    }
    return ip6_emit(out, via, dg, len);
}
