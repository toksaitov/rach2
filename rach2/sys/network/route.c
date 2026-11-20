/*
 * route.c: the IPv4 routing table, as driver/netdev.h declares it.
 *
 * The routes the cards imply are found from them when they are asked for:
 * each address's subnet or peer prefix, reached on the link, and a card's
 * gateway as its default route; `lo` has none. The routes a program adds,
 * through SIOCADDRT or RTM_NEWROUTE, are kept in a table of NETDEV_ROUTES. A
 * lookup takes, of the routes of interfaces that are up and have that
 * destination, the one of the longest prefix, then of the lowest metric, then
 * the first found, which puts an implied route before an added one of the
 * same prefix and metric, as Linux's kernel routes come first.
 */

#include "driver/netdev.h"
#include "net.h"
#include "netconf.h"
#include "../lib/kerrno.h"

#define NETDEV_ROUTES 32

/* An added route: datagrams for `dst`/`mask` leave by `nd`, to `gw`, or on
 * the link when it is 0, at `metric`. `nd` is NULL in a free entry. */
struct added_rt {
    struct netdev *nd;
    struct netdev_rt row;
};

static struct added_rt added[NETDEV_ROUTES];

/* The lookups the stack made and those that found no route, which
 * /proc/net/stat/rt_cache reports. */
uint32_t netdev_route_lookups;
uint32_t netdev_route_misses;

/** Whether `mask` is contiguous: ones from the top, then zeros. */
static int mask_contiguous(uint32_t mask)
{
    return (~mask & (~mask + 1u)) == 0;
}

/** The `n`th route `nd` implies, its subnets then its gateway, into `out`;
 * the count of them when `n` is past them, as a negative number. */
static int implied_at(const struct netdev *nd, int n, struct netdev_rt *out)
{
    int a, count = 0;

    if (!nd->up) {
        return 0;
    }
    for (a = -1; a < nd->naddr2; a++) {
        uint32_t ip = a < 0 ? nd->ip : nd->ip2[a];
        uint32_t mask = a < 0 ? nd->mask : nd->mask2[a];
        uint32_t dst = netdev_addr_dst(nd, a);

        if (!netdev_addr_has_route(ip, dst, mask) || netdev_secondary(nd, a) ||
            nd->addr4[a + 1].noprefix || (nd->addr4[a + 1].deleted & ADDR4_PREFIX)) {
            continue;
        }
        if (count++ == n) {
            out->dst = dst & mask;
            out->mask = mask;
            out->gw = 0;
            out->src = ip;
            out->kind = NETDEV_RT_LINK;
            out->proto = 2;
            out->scope = 253;
            out->prefsrc = ip;
            out->metric = nd->addr4[a + 1].metric;
            return 1;
        }
    }
    if (nd->gateway != 0 && count++ == n) {
        out->dst = out->mask = 0;
        out->gw = nd->gateway;
        out->src = netdev_source(nd, nd->gateway);
        out->kind = NETDEV_RT_DEFAULT;
        out->proto = 3;
        out->scope = 0;
        out->prefsrc = out->metric = 0;
        return 1;
    }
    return -count;
}

/** Whether this local-table route already appeared on the device. */
static int local_seen(const struct netdev *nd, int a, int bit, uint32_t dst)
{
    int i, k;
    for (i = -1; i <= a; i++) {
        for (k = bit == ADDR4_LOCAL ? ADDR4_LOCAL : ADDR4_BROADCAST;
             k <= (bit == ADDR4_LOCAL ? ADDR4_LOCAL : ADDR4_SUBNET_BROADCAST); k *= 2) {
            if (i == a && k == bit)
                return 0;
            if (netdev_addr_local(nd, i, k) == dst)
                return 1;
        }
    }
    return 0;
}

int netdev_local_route_at(int n, struct netdev_rt *out)
{
    int i, a, bit;
    for (i = 0; i < netdev_count(); i++) {
        struct netdev *nd = netdev_at(i);
        for (a = -1; a < nd->naddr2; a++) {
            for (bit = ADDR4_LOCAL; bit <= ADDR4_SUBNET_BROADCAST; bit *= 2) {
                uint32_t dst = netdev_addr_local(nd, a, bit);
                if (!dst || local_seen(nd, a, bit, dst) || n-- != 0)
                    continue;
                out->dst = dst;
                out->mask = 0xffffffffu;
                out->gw = out->metric = 0;
                out->src = out->prefsrc = a < 0 ? nd->ip : nd->ip2[a];
                out->kind = bit == ADDR4_LOCAL ? NETDEV_RT_LOCAL : NETDEV_RT_BROADCAST;
                out->scope = bit == ADDR4_LOCAL ? 254 : 253;
                out->proto = 2;
                out->ifindex = nd->index;
                return 1;
            }
        }
    }
    return 0;
}

int netdev_has_link_route(const struct netdev *nd, uint32_t dst, uint32_t mask)
{
    struct netdev_rt r;
    int n;

    for (n = 0; implied_at(nd, n, &r) > 0 && r.kind == NETDEV_RT_LINK; n++) {
        if (r.dst == dst && r.mask == mask) {
            return 1;
        }
    }
    return 0;
}

int netdev_route_at(int n, struct netdev_rt *out)
{
    int i, got;

    for (i = 0; i < netdev_count(); i++) {
        struct netdev *nd = netdev_at(i);

        got = implied_at(nd, n, out);
        if (got > 0) {
            out->ifindex = nd->index;
            return 1;
        }
        n += got;
    }
    for (i = 0; i < NETDEV_ROUTES; i++) {
        if (added[i].nd && n-- == 0) {
            *out = added[i].row;
            out->src = out->prefsrc ? out->prefsrc
                                    : netdev_source(added[i].nd, out->gw ? out->gw : out->dst);
            return 1;
        }
    }
    return 0;
}

/** The card of index `index`, or NULL. */
static struct netdev *dev_of(int index)
{
    int i;

    for (i = 0; i < netdev_count(); i++) {
        if (netdev_at(i)->index == index) {
            return netdev_at(i);
        }
    }
    return 0;
}

struct netdev *netdev_route(uint32_t dst, uint32_t *via)
{
    return netdev_route_on(dst, 0, via);
}

struct netdev *netdev_route_on(uint32_t dst, int ifindex, uint32_t *via)
{
    struct netdev_rt row = {0};
    struct netdev *nd = netdev_route_lookup(dst, ifindex, &row);
    if (nd)
        *via = row.gw ? row.gw : dst;
    return nd;
}

struct netdev *netdev_route_lookup(uint32_t dst, int ifindex, struct netdev_rt *best)
{
    struct netdev_rt r;
    struct netdev *out = 0;
    int n;

    netdev_route_lookups++;
    for (n = 0; netdev_route_at(n, &r); n++) {
        struct netdev *nd = dev_of(r.ifindex);

        if (!nd || !nd->up || (dst & r.mask) != r.dst || (ifindex && r.ifindex != ifindex)) {
            continue;
        }
        if (!out || r.mask > best->mask || (r.mask == best->mask && r.metric < best->metric)) {
            *best = r;
            out = nd;
        }
    }
    if (!out && ifindex) {
        /* Linux's rule for a socket bound to a card: with no route of the
         * card's, the destination is taken to be on its link. */
        out = dev_of(ifindex);
        if (out && out->up) {
            struct netdev_rt direct = {0};
            direct.dst = dst;
            direct.mask = 0xffffffffu;
            direct.ifindex = ifindex;
            direct.src = netdev_source(out, dst);
            *best = direct;
            return out;
        }
        out = 0;
    }
    if (!out) {
        netdev_route_misses++;
        return 0;
    }
    if (!best->prefsrc)
        best->src = netdev_source(out, best->gw ? best->gw : dst);
    return out;
}

int netdev_gateway_reachable(const struct netdev *nd, uint32_t gw)
{
    struct netdev_rt r;
    int i;
    if (!nd->up || !gw)
        return 0;
    for (i = 0; implied_at(nd, i, &r) > 0; i++) {
        if (r.kind == NETDEV_RT_LINK && (gw & r.mask) == r.dst)
            return 1;
    }
    for (i = 0; i < NETDEV_ROUTES; i++) {
        r = added[i].row;
        if (added[i].nd == nd && !r.gw && r.scope >= 253 && (gw & r.mask) == r.dst)
            return 1;
    }
    return 0;
}

/** Match the prefix and the optional delete selectors. */
static int route_matches(const struct netdev_rt *old, const struct netdev_rt *r)
{
    return old->dst == r->dst && old->mask == r->mask && (!r->gw || old->gw == r->gw) &&
           (!r->metric || old->metric == r->metric) && (!r->proto || old->proto == r->proto) &&
           (r->scope == 255 || old->scope == r->scope) &&
           (!r->prefsrc || r->prefsrc == old->prefsrc);
}

int netdev_local_route_del(const struct netdev *only, const struct netdev_rt *r)
{
    struct netdev_rt old;
    int n, a, bit;
    for (n = 0; netdev_local_route_at(n, &old); n++) {
        struct netdev *nd = dev_of(old.ifindex);
        if ((only && nd != only) || (r->kind && r->kind != old.kind) || !route_matches(&old, r))
            continue;
        for (a = -1; a < nd->naddr2; a++) {
            for (bit = ADDR4_LOCAL; bit <= ADDR4_SUBNET_BROADCAST; bit *= 2) {
                if ((bit == ADDR4_LOCAL) == (old.kind == NETDEV_RT_LOCAL) &&
                    netdev_addr_local(nd, a, bit) == old.dst)
                    nd->addr4[a + 1].deleted |= bit;
            }
        }
        return 0;
    }
    return -ESRCH;
}

/** Remove the generated route named by a successful table lookup. */
static void remove_generated(struct netdev *nd, const struct netdev_rt *r)
{
    int a;
    if (r->kind == NETDEV_RT_DEFAULT) {
        nd->gateway = 0;
        if (nd == netdev_first())
            net_ip_gateway = 0;
        return;
    }
    for (a = -1; a < nd->naddr2; a++) {
        uint32_t mask = a < 0 ? nd->mask : nd->mask2[a];
        if (mask == r->mask && (netdev_addr_dst(nd, a) & mask) == r->dst) {
            nd->addr4[a + 1].deleted |= ADDR4_PREFIX;
        }
    }
}

/** Find a generated route to delete or replace. */
static struct netdev *generated_match(struct netdev *nd, const struct netdev_rt *r, int del,
                                      struct netdev_rt *old)
{
    int i, n;
    for (i = 0; i < netdev_count(); i++) {
        struct netdev *at = netdev_at(i);
        if (del && nd && nd != at)
            continue;
        for (n = 0; implied_at(at, n, old) > 0; n++) {
            if (old->dst == r->dst && old->mask == r->mask &&
                (del ? route_matches(old, r) : old->metric == r->metric))
                return at;
        }
    }
    return 0;
}

/** Check the prefix, device, preferred source and gateway before insertion. */
static int route_valid(struct netdev *nd, const struct netdev_rt *r)
{
    if (!nd || !mask_contiguous(r->mask) || (r->dst & ~r->mask) != 0 ||
        (r->prefsrc && !netdev_owns(r->prefsrc)))
        return -EINVAL;
    if (!nd->up && r->scope != 254)
        return -ENETDOWN;
    if (r->gw && (r->scope >= 253 || !netdev_gateway_reachable(nd, r->gw)))
        return -ENETUNREACH;
    return 0;
}

int netdev_route_change(struct netdev *nd, const struct netdev_rt *r, int del, int replace)
{
    struct netdev_rt old;
    struct netdev *generated;
    int i, slot = -1, rc;

    if (!del && (rc = route_valid(nd, r)) != 0)
        return rc;
    generated = generated_match(nd, r, del, &old);
    if (generated && del) {
        remove_generated(generated, &old);
        return 0;
    }
    if (generated && !replace)
        return -EEXIST;
    for (i = 0; i < NETDEV_ROUTES; i++) {
        const struct netdev_rt *row = &added[i].row;
        if (!added[i].nd) {
            if (slot < 0)
                slot = i;
        } else if (del) {
            if ((!nd || nd == added[i].nd) && route_matches(row, r)) {
                added[i].nd = 0;
                return 0;
            }
        } else if (row->dst == r->dst && row->mask == r->mask && row->metric == r->metric) {
            if (!replace)
                return -EEXIST;
            slot = i;
            break;
        }
    }
    if (del)
        return -ESRCH;
    if (slot < 0)
        return -ENOSPC;
    if (generated)
        remove_generated(generated, &old);
    added[slot].nd = nd;
    added[slot].row = *r;
    added[slot].row.kind = NETDEV_RT_ADDED;
    added[slot].row.ifindex = nd->index;
    return 0;
}

int netdev_route_add(struct netdev *nd, uint32_t dst, uint32_t mask, uint32_t gw, uint32_t metric,
                     int replace)
{
    struct netdev_rt r = {0};
    r.dst = dst;
    r.mask = mask;
    r.gw = gw;
    r.metric = metric;
    r.scope = gw ? 0 : 253;
    r.proto = 3;
    return netdev_route_change(nd, &r, 0, replace);
}

uint32_t netdev_default_gateway(const struct netdev *nd)
{
    uint32_t gw = nd->gateway, metric = 0;
    int i;

    if (gw != 0) {
        return gw;
    }
    for (i = 0; i < NETDEV_ROUTES; i++) {
        if (added[i].nd == nd && added[i].row.mask == 0 && added[i].row.gw != 0 &&
            (gw == 0 || added[i].row.metric < metric)) {
            gw = added[i].row.gw;
            metric = added[i].row.metric;
        }
    }
    return gw;
}

int netdev_route_del(struct netdev *nd, uint32_t dst, uint32_t mask, uint32_t gw)
{
    struct netdev_rt r = {0};
    r.dst = dst;
    r.mask = mask;
    r.gw = gw;
    r.scope = 255;
    return netdev_route_change(nd, &r, 1, 0);
}

void netdev_route_flush(struct netdev *nd, int all)
{
    int i;
    for (i = 0; i < NETDEV_ROUTES; i++) {
        if (added[i].nd == nd && (all || added[i].row.scope != 254))
            added[i].nd = 0;
    }
    nd->gateway = 0;
    if (nd == netdev_first())
        net_ip_gateway = 0;
}

void netdev_route_lost_source(uint32_t ip)
{
    int i;
    if (!ip || netdev_owns(ip))
        return;
    for (i = 0; i < NETDEV_ROUTES; i++) {
        if (added[i].nd && added[i].row.prefsrc == ip)
            added[i].nd = 0;
    }
}
