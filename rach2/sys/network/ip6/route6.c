/*
 * route6.c: the IPv6 route lookup, as ip6.h describes it, and the routing
 * table behind it, as route6.h does. The path MTU is in pmtu6.c.
 */

#include "ip6.h"
#include "nd6.h"
#include "route6.h"
#include "../net.h"
#include "../driver/netdev.h"
#include "../../lib/kerrno.h"
#include "../../lib/kstring.h"
#include "../../timer.h"

/* A route of the table; `nd` is NULL in a free entry. `expires` is the
 * whole tick count a learned route ends at, and 0 for one that does not
 * end. */
struct route6 {
    struct netdev *nd;
    uint8_t dst[IP6_ADDR_LEN];
    uint8_t plen;
    uint8_t proto;
    uint8_t gw[IP6_ADDR_LEN];
    uint32_t metric;
    uint64_t expires;
};

static struct route6 table6[ROUTE6_MAX];

/** Clear the bits of `a` past its first `plen`. */
static void mask_prefix(uint8_t *a, int plen)
{
    int i;

    for (i = 0; i < IP6_ADDR_LEN; i++) {
        if (plen >= 8 * (i + 1)) {
            continue;
        }
        a[i] &= plen > 8 * i ? (uint8_t)(0xff << (8 - (plen - 8 * i))) : 0;
    }
}

/** Whether address `a` of `nd` implies a route: one a program gave or
 * made at link up, of a prefix shorter than 128 not implied by an address
 * before it. */
static int implies(const struct netdev *nd, int a)
{
    const struct netdev_addr6 *x = &nd->addr6[a];
    int i;

    if (x->prefix >= 128 || x->proto == NETDEV_PROTO_KERNEL_RA) {
        return 0;
    }
    for (i = 0; i < a; i++) {
        const struct netdev_addr6 *y = &nd->addr6[i];

        if (y->prefix == x->prefix && y->proto != NETDEV_PROTO_KERNEL_RA &&
            ip6_prefix_match(y->addr, x->addr, x->prefix)) {
            return 0;
        }
    }
    return 1;
}

int route6_at(int n, struct route6_row *out)
{
    int i, a;

    for (i = 0; i < netdev_count(); i++) {
        struct netdev *nd = netdev_at(i);

        for (a = 0; a < nd->naddr6; a++) {
            if (!implies(nd, a) || n-- != 0) {
                continue;
            }
            k_memset(out, 0, sizeof(*out));
            out->ifindex = nd->index;
            k_memcpy(out->dst, nd->addr6[a].addr, IP6_ADDR_LEN);
            out->plen = nd->addr6[a].prefix;
            mask_prefix(out->dst, out->plen);
            out->metric = ROUTE6_METRIC_PREFIX;
            out->proto = ROUTE6_PROTO_KERNEL;
            return 1;
        }
    }
    for (i = 0; i < ROUTE6_MAX; i++) {
        struct route6 *r = &table6[i];

        if (!r->nd || n-- != 0) {
            continue;
        }
        out->ifindex = r->nd->index;
        k_memcpy(out->dst, r->dst, IP6_ADDR_LEN);
        out->plen = r->plen;
        k_memcpy(out->gw, r->gw, IP6_ADDR_LEN);
        out->metric = r->metric;
        out->proto = r->proto;
        out->expires = 0;
        if (r->expires != 0 && r->expires > tick_count()) {
            out->expires = (uint32_t)tick_div(r->expires - tick_count() + TICK_HZ - 1, TICK_HZ, 0);
        }
        return 1;
    }
    return 0;
}

/** The card of index `index`, or NULL; `lo` has no routes of its own. */
static struct netdev *card_of(int index)
{
    int i;

    for (i = 0; i < netdev_count(); i++) {
        if (netdev_at(i)->index == index) {
            return netdev_at(i);
        }
    }
    return 0;
}

/**
 * A route better than `best`: a longer prefix, then a lower metric, then a
 * gateway that may be reachable where `best`'s is not, since RFC 4861,
 * 6.3.6, has a host leave a router that stopped answering for another.
 */
static int better(const struct route6_row *r, int usable, const struct route6_row *best,
                  int best_usable)
{
    if (r->plen != best->plen) {
        return r->plen > best->plen;
    }
    if (r->metric != best->metric) {
        return r->metric < best->metric;
    }
    return usable && !best_usable;
}

struct netdev *route6_lookup(const uint8_t *dst, int ifindex, uint8_t *via)
{
    struct route6_row r, best;
    struct netdev *out = 0;
    int n, usable, best_usable = 0;

    for (n = 0; route6_at(n, &r); n++) {
        struct netdev *nd = card_of(r.ifindex);

        if (!nd || !nd->up || nd6_disabled(nd) || (ifindex != 0 && r.ifindex != ifindex) ||
            !ip6_prefix_match(dst, r.dst, r.plen)) {
            continue;
        }
        usable = ip6_is_unspecified(r.gw) || nd6_router_usable(nd, r.gw);
        if (!out || better(&r, usable, &best, best_usable)) {
            best = r;
            best_usable = usable;
            out = nd;
        }
    }
    if (out) {
        k_memcpy(via, ip6_is_unspecified(best.gw) ? dst : best.gw, IP6_ADDR_LEN);
    }
    return out;
}

/** The table's route of `nd`, `dst`/`plen` (already masked) and `gw`, of
 * `metric` unless it is 0xFFFFFFFF; any card for a NULL `nd` and any
 * gateway for a NULL `gw`. -1 for none. */
static int find(const struct netdev *nd, const uint8_t *dst, int plen, const uint8_t *gw,
                uint32_t metric)
{
    int i;

    for (i = 0; i < ROUTE6_MAX; i++) {
        struct route6 *r = &table6[i];

        if (r->nd && (!nd || r->nd == nd) && r->plen == plen && ip6_equal(r->dst, dst) &&
            (!gw || ip6_equal(r->gw, gw)) && (metric == 0xFFFFFFFFu || r->metric == metric)) {
            return i;
        }
    }
    return -1;
}

/** A free entry of the table, or -1. */
static int free_slot(void)
{
    int i;

    for (i = 0; i < ROUTE6_MAX; i++) {
        if (!table6[i].nd) {
            return i;
        }
    }
    return -1;
}

int route6_add(struct netdev *nd, const uint8_t *dst, int plen, const uint8_t *gw, uint32_t metric,
               int proto)
{
    uint8_t d[IP6_ADDR_LEN], g[IP6_ADDR_LEN];
    uint32_t lock;
    int i, rc = 0;

    if (!nd || nd->loopback || plen < 0 || plen > 128) {
        return -EINVAL;
    }
    k_memcpy(d, dst, IP6_ADDR_LEN);
    mask_prefix(d, plen);
    k_memset(g, 0, sizeof(g));
    if (gw) {
        if (ip6_is_multicast(gw) || ip6_is_loopback(gw) || ip6_is_v4mapped(gw)) {
            return -EINVAL;
        }
        k_memcpy(g, gw, IP6_ADDR_LEN);
    }
    lock = net_lock();
    if (!ip6_is_unspecified(g) && netdev_owner6(g)) {
        rc = -EINVAL;
    } else if (find(nd, d, plen, g, metric) >= 0) {
        rc = -EEXIST;
    } else if ((i = free_slot()) < 0) {
        rc = -ENOSPC;
    } else {
        table6[i].nd = nd;
        k_memcpy(table6[i].dst, d, IP6_ADDR_LEN);
        table6[i].plen = (uint8_t)plen;
        k_memcpy(table6[i].gw, g, IP6_ADDR_LEN);
        table6[i].metric = metric;
        table6[i].proto = (uint8_t)proto;
        table6[i].expires = 0;
    }
    net_unlock(lock);
    return rc;
}

int route6_del(struct netdev *nd, const uint8_t *dst, int plen, const uint8_t *gw)
{
    uint8_t d[IP6_ADDR_LEN];
    uint32_t lock;
    int i;

    if (plen < 0 || plen > 128) {
        return -ESRCH;
    }
    k_memcpy(d, dst, IP6_ADDR_LEN);
    mask_prefix(d, plen);
    lock = net_lock();
    i = find(nd, d, plen, gw, 0xFFFFFFFFu);
    if (i >= 0) {
        table6[i].nd = 0;
    }
    net_unlock(lock);
    return i >= 0 ? 0 : -ESRCH;
}

/** A full table refuses the route; the advertisement that would refresh it
 * comes again. */
void route6_learn(struct netdev *nd, const uint8_t *dst, int plen, const uint8_t *gw,
                  uint32_t metric, uint32_t lifetime)
{
    uint8_t d[IP6_ADDR_LEN], g[IP6_ADDR_LEN];
    int i;

    k_memcpy(d, dst, IP6_ADDR_LEN);
    mask_prefix(d, plen);
    k_memset(g, 0, sizeof(g));
    if (gw) {
        k_memcpy(g, gw, IP6_ADDR_LEN);
    }
    i = find(nd, d, plen, g, metric);
    if (i >= 0 && table6[i].proto != ROUTE6_PROTO_RA) {
        return;
    }
    if (lifetime == 0) {
        if (i >= 0) {
            table6[i].nd = 0;
        }
        return;
    }
    if (i < 0) {
        i = free_slot();
        if (i < 0) {
            return;
        }
        table6[i].nd = nd;
        k_memcpy(table6[i].dst, d, IP6_ADDR_LEN);
        table6[i].plen = (uint8_t)plen;
        k_memcpy(table6[i].gw, g, IP6_ADDR_LEN);
        table6[i].metric = metric;
        table6[i].proto = ROUTE6_PROTO_RA;
    }
    if (lifetime == NETDEV_LIFETIME_FOREVER) {
        table6[i].expires = 0;
    } else {
        table6[i].expires = tick_count() + (uint64_t)lifetime * TICK_HZ;
    }
}

void route6_forget(struct netdev *nd, int proto)
{
    int i;

    for (i = 0; i < ROUTE6_MAX; i++) {
        if (table6[i].nd == nd && (proto < 0 || table6[i].proto == proto)) {
            table6[i].nd = 0;
        }
    }
}

void route6_ra_metric(struct netdev *nd, uint32_t metric)
{
    int i, j;

    for (i = 0; i < ROUTE6_MAX; i++) {
        struct route6 *r = &table6[i];

        if (r->nd != nd || r->proto != ROUTE6_PROTO_RA || r->plen != 0 || r->metric == metric) {
            continue;
        }
        j = find(nd, r->dst, 0, r->gw, metric);
        if (j < 0) {
            r->metric = metric;
            continue;
        }
        if (table6[j].proto == ROUTE6_PROTO_RA && table6[j].expires != 0 &&
            (r->expires == 0 || r->expires > table6[j].expires)) {
            table6[j].expires = r->expires;
        }
        r->nd = 0;
    }
}

void route6_tick(void)
{
    int i;

    for (i = 0; i < ROUTE6_MAX; i++) {
        struct route6 *r = &table6[i];

        if (r->nd && r->expires != 0 && tick_count() >= r->expires) {
            r->nd = 0;
        }
    }
}

/** The interface of index `ifindex`, or NULL for one that does not exist. */
static struct netdev *by_index(int ifindex)
{
    return netdev_by_index(ifindex);
}

/**
 * A group of interface scope, ff01::/16, and the reserved scopes 0 and 15
 * have no route, since no datagram to one may leave the machine, and `lo`
 * carries no multicast. A link-scoped destination with no interface named
 * has one only on a machine with one card, which is the link it means. Any
 * other unicast destination is the table's. An interface that is down or
 * has IPv6 disabled carries nothing, as `netdev_route` judges IPv4's.
 *
 * An address of this machine's goes by `lo`, except a link-scoped one that
 * `ifindex` places on a card other than the one that holds it: that is
 * another host's on the named link, and goes out by that card, as Linux
 * finds the local route only on the address's own interface.
 */
struct netdev *ip6_route(const uint8_t *dst, int ifindex, uint8_t *via)
{
    struct netdev *nd = ip6_route_find(dst, ifindex, via);

    if (!nd) {
        net_mib6.ip6_out_no_routes++;
    }
    return nd;
}

/**
 * Whether `dst`, a unicast address an interface holds, goes by `lo`: not
 * when it is link-scoped and `ifindex` names a card other than its holder.
 */
static int is_local(const uint8_t *dst, int ifindex)
{
    struct netdev *owner = ip6_is_multicast(dst) ? 0 : netdev_owner6(dst);
    struct netdev *named = ifindex != 0 ? by_index(ifindex) : 0;

    if (!owner) {
        return 0;
    }
    return !ip6_link_scoped(dst) || !named || named == owner || named->loopback;
}

struct netdev *ip6_route_find(const uint8_t *dst, int ifindex, uint8_t *via)
{
    struct netdev *nd = 0;

    if (ip6_is_loopback(dst) || is_local(dst, ifindex)) {
        nd = netdev_loopback();
    } else if (ip6_link_scoped(dst)) {
        if (ifindex != 0) {
            nd = by_index(ifindex);
        } else if (netdev_count() == 1) {
            nd = netdev_first();
        }
    } else if (ip6_is_multicast(dst)) {
        int scope = dst[1] & 0x0f;

        if (scope != 0 && scope != 1 && scope != 15) {
            nd = ifindex != 0 ? by_index(ifindex) : netdev_first();
        }
    } else {
        nd = route6_lookup(dst, ifindex, via);
        if (nd) {
            return nd;
        }
    }
    if (nd && (!nd->up || nd6_disabled(nd) ||
               (nd->loopback && !ip6_is_loopback(dst) && !netdev_owner6(dst)))) {
        nd = 0;
    }
    if (!nd) {
        return 0;
    }
    k_memcpy(via, dst, IP6_ADDR_LEN);
    return nd;
}
