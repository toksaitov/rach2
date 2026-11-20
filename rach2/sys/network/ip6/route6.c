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
    uint8_t proto, local, retained;
    uint8_t gw[IP6_ADDR_LEN], prefsrc[IP6_ADDR_LEN];
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

enum addr6_route_kind {
    PREFIX6 = 1,
    PEER6 = NETDEV_A6_PEER_ROUTE,
    LOCAL6 = NETDEV_A6_LOCAL_ROUTE,
    PEER_PREFIX6 = 8
};

/** Describe one route installed by an address, when it is in force. */
static int address_route(const struct netdev *nd, int a, int kind, struct route6_row *r)
{
    const struct netdev_addr6 *x = &nd->addr6[a];
    const uint8_t *dst = x->addr;
    int plen = x->prefix;
    if (!nd->up || (x->deleted_routes & kind))
        return 0;
    if (kind == PREFIX6 && (x->noprefix || x->proto == NETDEV_PROTO_KERNEL_RA))
        return 0;
    if (kind == PEER6 || kind == PEER_PREFIX6) {
        if (ip6_is_unspecified(x->peer) || (kind == PEER_PREFIX6 && !x->peer_prefix))
            return 0;
        dst = x->peer;
    }
    if (kind == LOCAL6 || kind == PEER6) {
        if (x->flags & (NETDEV_A6_TENTATIVE | NETDEV_A6_DADFAILED))
            return 0;
        plen = 128;
    }
    k_memset(r, 0, sizeof(*r));
    r->ifindex = nd->index;
    r->plen = (uint8_t)plen;
    k_memcpy(r->dst, dst, IP6_ADDR_LEN);
    mask_prefix(r->dst, plen);
    r->local = kind == LOCAL6;
    r->metric = r->local ? 0 : x->metric ? x->metric : ROUTE6_METRIC_PREFIX;
    r->proto = ROUTE6_PROTO_KERNEL;
    if ((kind == PREFIX6 || kind == PEER_PREFIX6) && x->valid != NETDEV_LIFETIME_FOREVER) {
        uint64_t elapsed = tick_div(tick_count() - x->since, TICK_HZ, 0);
        r->expires = elapsed < x->valid ? x->valid - (uint32_t)elapsed : 0;
    }
    return 1;
}

/** Whether two address routes name the same FIB entry on one device. */
static int same_prefix(const struct route6_row *r, const struct route6_row *other)
{
    return r->plen == other->plen && r->local == other->local && r->metric == other->metric &&
           ip6_equal(r->dst, other->dst);
}

/** Whether an earlier address route supplies this entry. */
static int already_listed(const struct netdev *nd, int a, int kind, const struct route6_row *r)
{
    struct route6_row prior;
    int i, k;
    for (i = 0; i <= a; i++) {
        for (k = PREFIX6; k <= PEER_PREFIX6; k *= 2) {
            if (a == i && k == kind)
                return 0;
            if (address_route(nd, i, k, &prior) && same_prefix(r, &prior))
                return 1;
        }
    }
    return 0;
}

/** Whether another address supplies this generated prefix on the same card. */
static int other_prefix(struct netdev *nd, int slot, const struct route6_row *r)
{
    struct route6_row other;
    int i;
    for (i = 0; i < nd->naddr6; i++)
        if (i != slot && address_route(nd, i, PREFIX6, &other) && same_prefix(r, &other))
            return 1;
    return 0;
}

/** The longest remaining lifetime among addresses sharing this prefix. */
static uint32_t shared_lifetime(const struct netdev *nd, const struct route6_row *r)
{
    struct route6_row other;
    uint32_t left = r->expires;
    int i;
    for (i = 0; left && i < nd->naddr6; i++) {
        if (address_route(nd, i, PREFIX6, &other) && same_prefix(r, &other) &&
            (!other.expires || other.expires > left))
            left = other.expires;
    }
    return left;
}

int route6_at(int n, struct route6_row *out)
{
    int i, a, k;

    for (i = 0; i < netdev_count(); i++) {
        struct netdev *nd = netdev_at(i);
        for (a = 0; a < nd->naddr6; a++) {
            for (k = PREFIX6; k <= PEER_PREFIX6; k *= 2) {
                if (address_route(nd, a, k, out) && !already_listed(nd, a, k, out) && n-- == 0) {
                    if (k == PREFIX6 && out->expires)
                        out->expires = shared_lifetime(nd, out);
                    return 1;
                }
            }
        }
    }
    for (i = 0; i < ROUTE6_MAX; i++) {
        struct route6 *r = &table6[i];

        if (!r->nd)
            continue;
        if (r->retained) {
            struct route6_row key = {0};
            key.plen = r->plen;
            key.metric = r->metric;
            k_memcpy(key.dst, r->dst, IP6_ADDR_LEN);
            if (other_prefix(r->nd, -1, &key))
                continue;
        }
        if (n-- != 0) {
            continue;
        }
        out->ifindex = r->nd->index;
        k_memcpy(out->dst, r->dst, IP6_ADDR_LEN);
        out->plen = r->plen;
        k_memcpy(out->gw, r->gw, IP6_ADDR_LEN);
        k_memcpy(out->prefsrc, r->prefsrc, IP6_ADDR_LEN);
        out->metric = r->metric;
        out->proto = r->proto;
        out->local = r->local;
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

struct netdev *route6_resolve(const uint8_t *dst, int ifindex, struct route6_row *best)
{
    struct route6_row r;
    struct netdev *out = 0;
    int n, usable, best_usable = 0;

    for (n = 0; route6_at(n, &r); n++) {
        struct netdev *nd = card_of(r.ifindex);

        if (r.local || !nd || !nd->up || nd6_disabled(nd) ||
            (ifindex != 0 && r.ifindex != ifindex) || !ip6_prefix_match(dst, r.dst, r.plen)) {
            continue;
        }
        usable = ip6_is_unspecified(r.gw) || nd6_router_usable(nd, r.gw);
        if (!out || better(&r, usable, best, best_usable)) {
            *best = r;
            best_usable = usable;
            out = nd;
        }
    }
    return out;
}

int route6_preferred_source(const struct netdev *nd, const uint8_t *dst, uint8_t *src)
{
    struct route6_row row;
    int i;
    for (i = 0; i < ROUTE6_MAX; i++) {
        const struct route6 *r = &table6[i];
        if (r->nd == nd && !ip6_is_unspecified(r->prefsrc) &&
            ip6_prefix_match(dst, r->dst, r->plen)) {
            if (!route6_resolve(dst, nd->index, &row) || ip6_is_unspecified(row.prefsrc))
                break;
            k_memcpy(src, row.prefsrc, IP6_ADDR_LEN);
            return 1;
        }
    }
    return 0;
}

struct netdev *route6_lookup(const uint8_t *dst, int ifindex, uint8_t *via)
{
    struct route6_row best;
    struct netdev *out = route6_resolve(dst, ifindex, &best);
    if (out)
        k_memcpy(via, ip6_is_unspecified(best.gw) ? dst : best.gw, IP6_ADDR_LEN);
    return out;
}

/** The table's route of `nd`, `dst`/`plen` (already masked) and `gw`, of
 * `metric`; any card for a NULL `nd` and any
 * gateway for a NULL `gw`. -1 for none. */
static int find(const struct netdev *nd, const uint8_t *dst, int plen, const uint8_t *gw,
                uint32_t metric)
{
    int i;

    for (i = 0; i < ROUTE6_MAX; i++) {
        struct route6 *r = &table6[i];

        if (r->nd && !r->local && (!nd || r->nd == nd) && r->plen == plen &&
            ip6_equal(r->dst, dst) && (!gw || ip6_equal(r->gw, gw)) && r->metric == metric) {
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

/** Match an IPv6 prefix and any selectors supplied for deletion. */
static int matches(const struct route6_row *r, const struct route6_row *want)
{
    return r->local == want->local && r->plen == want->plen && ip6_equal(r->dst, want->dst) &&
           (!want->proto || r->proto == want->proto) &&
           (!want->metric || r->metric == want->metric) &&
           (ip6_is_unspecified(want->gw) || ip6_equal(r->gw, want->gw));
}

/** Find or withdraw all address references to one generated prefix. */
static int generated6(const struct netdev *only, const struct route6_row *want, int remove)
{
    struct route6_row r;
    int i, a, k, found = 0;
    for (i = 0; i < netdev_count(); i++) {
        struct netdev *nd = netdev_at(i);
        if (only && nd != only)
            continue;
        for (a = 0; a < nd->naddr6; a++) {
            for (k = PREFIX6; k <= PEER_PREFIX6; k *= 2) {
                if (address_route(nd, a, k, &r) && matches(&r, want)) {
                    if (!remove)
                        return 1;
                    nd->addr6[a].deleted_routes |= (uint8_t)k;
                    found = 1;
                }
            }
        }
        if (found)
            break;
    }
    return found;
}

/** The first table slot matching the requested prefix and selectors. */
static int table_match(const struct netdev *nd, const struct route6_row *want)
{
    int i;
    for (i = 0; i < ROUTE6_MAX; i++) {
        struct route6 *r = &table6[i];
        if (want->local == r->local && r->nd && (!nd || nd == r->nd) && r->plen == want->plen &&
            ip6_equal(want->dst, r->dst) && (!want->proto || want->proto == r->proto) &&
            (!want->metric || want->metric == r->metric) &&
            (ip6_is_unspecified(want->gw) || ip6_equal(want->gw, r->gw)))
            return i;
    }
    return -1;
}

/** Check the new route before consuming or replacing a table entry. */
static int valid_gateway(const struct netdev *nd, const uint8_t *gw)
{
    struct route6_row via;
    if (ip6_is_unspecified(gw))
        return 0;
    if (ip6_is_multicast(gw) || ip6_is_loopback(gw) || ip6_is_v4mapped(gw) || netdev_owner6(gw))
        return -EINVAL;
    if (!ip6_link_scoped(gw) &&
        (route6_resolve(gw, nd->index, &via) != nd || !ip6_is_unspecified(via.gw)))
        return -EHOSTUNREACH;
    return 0;
}

/** Whether src remains usable on this link, or any card for a global source.
 * Ignore the address being withdrawn while its slot is still present. */
static int has_source(const struct netdev *nd, const uint8_t *src, const struct netdev_addr6 *lost)
{
    int i, a;
    for (i = 0; i < netdev_listed(); i++) {
        struct netdev *at = netdev_at(i);
        if (at != nd && ip6_link_scoped(src))
            continue;
        for (a = 0; a < at->naddr6; a++) {
            const struct netdev_addr6 *ip = &at->addr6[a];
            if (ip != lost && !(ip->flags & (NETDEV_A6_TENTATIVE | NETDEV_A6_DADFAILED)) &&
                ip6_equal(ip->addr, src))
                return 1;
        }
    }
    return 0;
}

/** Validate a proposed row before changing an existing route. */
static int route6_valid(const struct netdev *nd, const struct route6_row *r)
{
    if (!nd->up)
        return -ENETDOWN;
    if (!ip6_is_unspecified(r->prefsrc) && !has_source(nd, r->prefsrc, 0))
        return -EINVAL;
    if (r->local && !ip6_is_unspecified(r->gw))
        return -EINVAL;
    return valid_gateway(nd, r->gw);
}

int route6_change_row(struct netdev *nd, const struct route6_row *input, int del, int replace)
{
    struct route6_row r = *input, selector;
    uint32_t lock;
    int i, generated, rc;
    if (r.plen > 128 || (!del && !nd))
        return -EINVAL;
    if (nd && nd->loopback)
        return -EOPNOTSUPP;
    mask_prefix(r.dst, r.plen);
    lock = net_lock();
    if (del) {
        if (generated6(nd, &r, 1)) {
            net_unlock(lock);
            return 0;
        }
        i = table_match(nd, &r);
        if (i >= 0)
            table6[i].nd = 0;
        net_unlock(lock);
        return i >= 0 ? 0 : -ESRCH;
    }
    rc = route6_valid(nd, &r);
    if (rc) {
        net_unlock(lock);
        return rc;
    }
    if (!r.metric)
        r.metric = 1024;
    selector = r;
    selector.proto = 0;
    if (replace)
        k_memset(selector.gw, 0, sizeof(selector.gw));
    generated = generated6(nd, &selector, 0);
    i = replace || r.local ? table_match(nd, &selector) : find(nd, r.dst, r.plen, r.gw, r.metric);
    if ((generated || i >= 0) && !replace) {
        net_unlock(lock);
        return -EEXIST;
    }
    if (i < 0)
        i = free_slot();
    if (i < 0) {
        net_unlock(lock);
        return -ENOSPC;
    }
    if (generated)
        generated6(nd, &selector, 1);
    table6[i].nd = nd;
    table6[i].local = r.local;
    table6[i].retained = 0;
    k_memcpy(table6[i].dst, r.dst, IP6_ADDR_LEN);
    table6[i].plen = r.plen;
    k_memcpy(table6[i].gw, r.gw, IP6_ADDR_LEN);
    table6[i].metric = r.metric;
    k_memcpy(table6[i].prefsrc, r.prefsrc, IP6_ADDR_LEN);
    table6[i].proto = r.proto ? r.proto : ROUTE6_PROTO_BOOT;
    table6[i].expires = 0;
    net_unlock(lock);
    return 0;
}

/** Keep a prefix in the routing table after its address record is removed.
 * expires is an absolute tick count or zero for a permanent route. */
static void retain_prefix(struct netdev *nd, const struct route6_row *r, uint64_t expires)
{
    int i = find(nd, r->dst, r->plen, r->gw, r->metric);
    if (i < 0) {
        i = free_slot();
        if (i < 0)
            return;
        k_memset(&table6[i], 0, sizeof(table6[i]));
        table6[i].nd = nd;
        table6[i].plen = r->plen;
        table6[i].proto = ROUTE6_PROTO_KERNEL;
        table6[i].retained = 1;
        table6[i].metric = r->metric;
        k_memcpy(table6[i].dst, r->dst, IP6_ADDR_LEN);
        table6[i].expires = expires;
    } else if (table6[i].retained && table6[i].expires &&
               (!expires || expires > table6[i].expires)) {
        table6[i].expires = expires;
    }
}

void route6_peer_change(struct netdev *nd, struct netdev_addr6 *a, const uint8_t *peer)
{
    struct route6_row old;
    int slot = (int)(a - nd->addr6);
    if (!peer || ip6_equal(peer, a->addr) || ip6_equal(peer, a->peer))
        return;
    if (a->prefix != 128 && address_route(nd, slot, PEER6, &old))
        retain_prefix(nd, &old, 0);
    if (!(a->flags & NETDEV_A6_TENTATIVE))
        a->deleted_routes |= PEER6;
    k_memcpy(a->peer, peer, IP6_ADDR_LEN);
}

void route6_address_removed(struct netdev *nd, int slot)
{
    struct netdev_addr6 *a = &nd->addr6[slot];
    struct route6_row r;
    uint64_t expires;
    int i;
    for (i = 0; i < ROUTE6_MAX; i++)
        if (table6[i].nd && ip6_equal(table6[i].prefsrc, a->addr) &&
            !has_source(table6[i].nd, a->addr, a))
            k_memset(table6[i].prefsrc, 0, IP6_ADDR_LEN);
    if (!address_route(nd, slot, PREFIX6, &r))
        return;
    if (!(a->flags & NETDEV_A6_PERMANENT)) {
        expires = a->since + (uint64_t)a->valid * TICK_HZ;
        if (expires > tick_count())
            retain_prefix(nd, &r, expires);
        return;
    }
    for (i = 0; i < nd->naddr6; i++) {
        struct netdev_addr6 *other = &nd->addr6[i];
        if (other->noprefix && other->prefix == r.plen &&
            ip6_prefix_match(other->addr, r.dst, r.plen)) {
            retain_prefix(nd, &r, 0);
            return;
        }
    }
    i = find(nd, r.dst, r.plen, r.gw, r.metric);
    if (i >= 0 && table6[i].retained && !other_prefix(nd, slot, &r))
        table6[i].nd = 0;
}

struct netdev *route6_added_local(const uint8_t *dst)
{
    const struct route6 *best = 0;
    int i;
    for (i = 0; i < ROUTE6_MAX; i++) {
        const struct route6 *r = &table6[i];
        if (r->nd && r->local && ip6_prefix_match(dst, r->dst, r->plen) &&
            (!best || r->plen > best->plen || (r->plen == best->plen && r->metric < best->metric)))
            best = r;
    }
    return best ? best->nd : 0;
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
        table6[i].local = 0;
        table6[i].retained = 0;
        k_memcpy(table6[i].dst, d, IP6_ADDR_LEN);
        table6[i].plen = (uint8_t)plen;
        k_memcpy(table6[i].gw, g, IP6_ADDR_LEN);
        table6[i].metric = metric;
        k_memset(table6[i].prefsrc, 0, IP6_ADDR_LEN);
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
 */
struct netdev *ip6_route(const uint8_t *dst, int ifindex, uint8_t *via)
{
    struct netdev *nd = ip6_route_find(dst, ifindex, via);

    if (!nd) {
        net_mib6.ip6_out_no_routes++;
    }
    return nd;
}

struct netdev *ip6_route_find(const uint8_t *dst, int ifindex, uint8_t *via)
{
    struct netdev *nd = 0;

    if (ip6_is_loopback(dst) || (!ip6_is_multicast(dst) && netdev_owner6(dst))) {
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
