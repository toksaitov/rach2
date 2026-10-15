/*
 * route.c: the IPv4 routing table, as driver/netdev.h declares it.
 *
 * The routes the cards imply are found from them when they are asked for:
 * the subnet of each address, reached on the link, and a card's gateway as
 * its default route; `lo` has none. The routes a program adds, through
 * SIOCADDRT or RTM_NEWROUTE, are kept in a table of NETDEV_ROUTES. A lookup
 * takes, of the routes of interfaces that are up and have that destination,
 * the one of the longest prefix, then of the lowest metric, then the first
 * found, which puts an implied route before an added one of the same prefix
 * and metric, as Linux's kernel routes come first.
 */

#include "driver/netdev.h"
#include "net.h"
#include "../lib/kerrno.h"

#define NETDEV_ROUTES 32

/* An added route: datagrams for `dst`/`mask` leave by `nd`, to `gw`, or on
 * the link when it is 0, at `metric`. `nd` is NULL in a free entry. */
struct added_rt {
    struct netdev *nd;
    uint32_t dst, mask, gw, metric;
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

/** Whether the subnet `ip`/`mask` of `nd` is that of an address before
 * address `a` of its list, -1 being the first, so it is listed once. */
static int subnet_seen(const struct netdev *nd, int a, uint32_t ip, uint32_t mask)
{
    int i;

    if (a >= 0 && nd->mask == mask && (nd->ip & mask) == (ip & mask)) {
        return 1;
    }
    for (i = 0; i < a; i++) {
        if (nd->mask2[i] == mask && (nd->ip2[i] & mask) == (ip & mask)) {
            return 1;
        }
    }
    return 0;
}

/** The `n`th route `nd` implies, its subnets then its gateway, into `out`;
 * the count of them when `n` is past them, as a negative number. */
static int implied_at(const struct netdev *nd, int n, struct netdev_rt *out)
{
    int a, count = 0;

    for (a = -1; a < nd->naddr2; a++) {
        uint32_t ip = a < 0 ? nd->ip : nd->ip2[a];
        uint32_t mask = a < 0 ? nd->mask : nd->mask2[a];

        if (ip == 0 || mask == 0 || subnet_seen(nd, a, ip, mask)) {
            continue;
        }
        if (count++ == n) {
            out->dst = ip & mask;
            out->mask = mask;
            out->gw = 0;
            out->src = ip;
            out->kind = NETDEV_RT_LINK;
            return 1;
        }
    }
    if (nd->gateway != 0 && count++ == n) {
        out->dst = out->mask = 0;
        out->gw = nd->gateway;
        out->src = netdev_source(nd, nd->gateway);
        out->kind = NETDEV_RT_DEFAULT;
        return 1;
    }
    return -count;
}

int netdev_route_at(int n, struct netdev_rt *out)
{
    int i, got;

    for (i = 0; i < netdev_count(); i++) {
        struct netdev *nd = netdev_at(i);

        got = implied_at(nd, n, out);
        if (got > 0) {
            out->metric = 0;
            out->ifindex = nd->index;
            return 1;
        }
        n += got;
    }
    for (i = 0; i < NETDEV_ROUTES; i++) {
        if (added[i].nd && n-- == 0) {
            out->dst = added[i].dst;
            out->mask = added[i].mask;
            out->gw = added[i].gw;
            out->metric = added[i].metric;
            out->src = netdev_source(added[i].nd, added[i].gw ? added[i].gw : added[i].dst);
            out->ifindex = added[i].nd->index;
            out->kind = NETDEV_RT_ADDED;
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
    struct netdev_rt r, best;
    struct netdev *out = 0;
    int n;

    netdev_route_lookups++;
    for (n = 0; netdev_route_at(n, &r); n++) {
        struct netdev *nd = dev_of(r.ifindex);

        if (!nd || !nd->up || (dst & r.mask) != r.dst) {
            continue;
        }
        if (!out || r.mask > best.mask || (r.mask == best.mask && r.metric < best.metric)) {
            best = r;
            out = nd;
        }
    }
    if (!out) {
        netdev_route_misses++;
        return 0;
    }
    *via = best.gw ? best.gw : dst;
    return out;
}

/** The added route to `dst`/`mask` of `metric`, by any interface, or -1. */
static int added_find(uint32_t dst, uint32_t mask, uint32_t metric)
{
    int i;

    for (i = 0; i < NETDEV_ROUTES; i++) {
        if (added[i].nd && added[i].dst == dst && added[i].mask == mask &&
            added[i].metric == metric) {
            return i;
        }
    }
    return -1;
}

int netdev_route_add(struct netdev *nd, uint32_t dst, uint32_t mask, uint32_t gw, uint32_t metric,
                     int replace)
{
    int i = added_find(dst, mask, metric);

    if (!mask_contiguous(mask) || (dst & ~mask) != 0) {
        return -EINVAL;
    }
    if (gw != 0 && !netdev_on_link(nd, gw)) {
        return -ENETUNREACH;
    }
    if (i >= 0 && !replace) {
        return -EEXIST;
    }
    if (i < 0) {
        for (i = 0; i < NETDEV_ROUTES && added[i].nd; i++) {
        }
        if (i == NETDEV_ROUTES) {
            return -ENOSPC;
        }
    }
    added[i].nd = nd;
    added[i].dst = dst;
    added[i].mask = mask;
    added[i].gw = gw;
    added[i].metric = metric;
    return 0;
}

int netdev_route_del(struct netdev *nd, uint32_t dst, uint32_t mask, uint32_t gw)
{
    int i;

    for (i = 0; i < NETDEV_ROUTES; i++) {
        if (added[i].nd && (!nd || added[i].nd == nd) && added[i].dst == dst &&
            added[i].mask == mask && (gw == 0 || added[i].gw == gw)) {
            added[i].nd = 0;
            return 0;
        }
    }
    return -ESRCH;
}

void netdev_route_prune(struct netdev *nd)
{
    int i;

    for (i = 0; i < NETDEV_ROUTES; i++) {
        if (added[i].nd == nd && added[i].gw != 0 && !netdev_on_link(nd, added[i].gw)) {
            added[i].nd = 0;
        }
    }
}
