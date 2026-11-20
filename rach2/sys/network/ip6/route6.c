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
#include "../../mm/kheap.h"

/* A route of the table; `nd` is NULL in a free entry. `expires` is the
 * whole tick count the route ends at, and 0 for one that does not
 * end. */
struct route6 {
    struct netdev *nd;
    uint8_t dst[IP6_ADDR_LEN];
    uint8_t plen;
    uint8_t proto, local, learned;
    uint8_t gw[IP6_ADDR_LEN], prefsrc[IP6_ADDR_LEN];
    uint32_t metric;
    uint64_t expires;
};

/* The table: `initial_routes` until more are needed, then an array that
 * `free_slot` doubles, which `trim_table` gives back once the routes fit
 * the first again. */
#define ROUTE6_INITIAL 32

static struct route6 initial_routes[ROUTE6_INITIAL];
static struct route6 *table6 = initial_routes;
static int table_size = ROUTE6_INITIAL;

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

/** Copy the local host route of a usable address that still has one. */
static int local_route(const struct netdev *nd, int slot, struct route6_row *r)
{
    const struct netdev_addr6 *a = &nd->addr6[slot];
    if (!nd->up || a->local_route_deleted ||
        (a->flags & (NETDEV_A6_TENTATIVE | NETDEV_A6_DADFAILED)))
        return 0;
    k_memset(r, 0, sizeof(*r));
    r->ifindex = nd->index;
    r->plen = 128;
    r->local = 1;
    r->proto = ROUTE6_PROTO_KERNEL;
    k_memcpy(r->dst, a->addr, IP6_ADDR_LEN);
    return 1;
}

int route6_at(int n, struct route6_row *out)
{
    int i, a;
    uint64_t now = tick_count();

    for (i = 0; i < netdev_count(); i++) {
        struct netdev *nd = netdev_at(i);
        for (a = 0; a < nd->naddr6; a++) {
            if (local_route(nd, a, out) && n-- == 0)
                return 1;
        }
    }
    for (i = 0; i < table_size; i++) {
        struct route6 *r = &table6[i];

        if (!r->nd || !r->nd->up || (r->expires && r->expires <= now))
            continue;
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
        out->learned = r->learned;
        out->local = r->local;
        out->expires = 0;
        if (r->expires != 0) {
            out->expires = (uint32_t)tick_div(r->expires - now + TICK_HZ - 1, TICK_HZ, 0);
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
    for (i = 0; i < table_size; i++) {
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

    for (i = 0; i < table_size; i++) {
        struct route6 *r = &table6[i];

        if (r->nd && !r->local && (!nd || r->nd == nd) && r->plen == plen &&
            ip6_equal(r->dst, dst) && (!gw || ip6_equal(r->gw, gw)) && r->metric == metric) {
            return i;
        }
    }
    return -1;
}

/** A free entry of the table, made by doubling it when every entry is
 * taken; -1 when the heap has no room. */
static int free_slot(void)
{
    int i, next;
    struct route6 *rows;
    for (i = 0; i < table_size; i++)
        if (!table6[i].nd)
            return i;
    if ((unsigned)table_size > 0x7fffffffu / sizeof(*rows) / 2)
        return -1;
    next = table_size * 2;
    rows = kzalloc((size_t)next * sizeof(*rows));
    if (!rows)
        return -1;
    k_memcpy(rows, table6, (size_t)table_size * sizeof(*rows));
    if (table6 != initial_routes)
        kfree(table6);
    table6 = rows;
    table_size = next;
    return i;
}

/** Give back the grown table once its routes fit the first entries. */
static void trim_table(void)
{
    int i;
    if (table6 == initial_routes)
        return;
    for (i = ROUTE6_INITIAL; i < table_size; i++)
        if (table6[i].nd)
            return;
    k_memcpy(initial_routes, table6, sizeof(initial_routes));
    kfree(table6);
    table6 = initial_routes;
    table_size = ROUTE6_INITIAL;
}

/** Match an IPv6 prefix and any selectors supplied for deletion. */
static int matches(const struct route6_row *r, const struct route6_row *want)
{
    return r->local == want->local && r->plen == want->plen && ip6_equal(r->dst, want->dst) &&
           (!want->proto || r->proto == want->proto) &&
           (!want->metric || r->metric == want->metric) &&
           (ip6_is_unspecified(want->gw) || ip6_equal(r->gw, want->gw));
}

/** Find an implied local route and remove it when requested. */
static int generated_local(const struct netdev *only, const struct route6_row *want, int remove)
{
    struct route6_row r;
    int i, a;
    if (!want->local)
        return 0;
    for (i = 0; i < netdev_count(); i++) {
        struct netdev *nd = netdev_at(i);
        if (only && nd != only)
            continue;
        for (a = 0; a < nd->naddr6; a++) {
            if (local_route(nd, a, &r) && matches(&r, want)) {
                if (remove)
                    nd->addr6[a].local_route_deleted = 1;
                return 1;
            }
        }
    }
    return 0;
}

/** The first table slot matching the requested prefix and selectors. */
static int table_match(const struct netdev *nd, const struct route6_row *want)
{
    int i;
    for (i = 0; i < table_size; i++) {
        struct route6 *r = &table6[i];
        if (want->local == r->local && r->nd && (!nd || nd == r->nd) && r->plen == want->plen &&
            ip6_equal(want->dst, r->dst) && (!want->proto || want->proto == r->proto) &&
            (!want->metric || want->metric == r->metric) &&
            (ip6_is_unspecified(want->gw) || ip6_equal(want->gw, r->gw)))
            return i;
    }
    return -1;
}

/** Select the lowest-metric route matching the supplied FIB selectors. */
static int select_row6(const struct netdev *nd, const struct route6_row *key,
                       struct route6_row *selected)
{
    struct route6_row r;
    int n, found = 0;
    for (n = 0; route6_at(n, &r); n++) {
        if ((!nd || r.ifindex == nd->index) && matches(&r, key) &&
            (!found || r.metric < selected->metric)) {
            *selected = r;
            found = 1;
        }
    }
    return found;
}

/** Find the stored copy of a selected row, including its exact gateway. */
static int selected_slot(const struct netdev *nd, const struct route6_row *r)
{
    int i;
    for (i = 0; i < table_size; i++) {
        const struct route6 *entry = &table6[i];
        if (entry->nd == nd && entry->plen == r->plen && entry->local == r->local &&
            entry->proto == r->proto && entry->metric == r->metric &&
            ip6_equal(entry->dst, r->dst) && ip6_equal(entry->gw, r->gw))
            return i;
    }
    return -1;
}

/** Remove the selected FIB entry, including its address references. */
static void remove_row6(const struct route6_row *r)
{
    struct netdev *nd = card_of(r->ifindex);
    int i;
    if (!generated_local(nd, r, 1)) {
        i = selected_slot(nd, r);
        if (i >= 0)
            table6[i].nd = 0;
    }
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
        rc = select_row6(nd, &r, &selector) ? 0 : -ESRCH;
        if (!rc)
            remove_row6(&selector);
        net_unlock(lock);
        return rc;
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
    generated = generated_local(nd, &selector, 0);
    i = replace || r.local ? table_match(nd, &selector) : find(nd, r.dst, r.plen, r.gw, r.metric);
    if ((generated || i >= 0) && !replace) {
        net_unlock(lock);
        return -EEXIST;
    }
    if (i < 0)
        i = free_slot();
    if (i < 0) {
        net_unlock(lock);
        return -ENOBUFS;
    }
    if (generated)
        generated_local(nd, &selector, 1);
    table6[i].nd = nd;
    table6[i].local = r.local;
    table6[i].learned = 0;
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

/** Select the lowest-metric stored route for this device and exact prefix.
 * A direct selection excludes gateways. Return its slot, or -1. */
static int prefix_slot(const struct netdev *nd, const uint8_t *dst, int plen, int direct)
{
    int i, best = -1;
    for (i = 0; i < table_size; i++) {
        const struct route6 *r = &table6[i];
        if (r->nd == nd && !r->local && r->plen == plen && ip6_prefix_match(dst, r->dst, plen) &&
            (!direct || ip6_is_unspecified(r->gw)) && (best < 0 || r->metric < table6[best].metric))
            best = i;
    }
    return best;
}

/** Insert an address route, refreshing a duplicate only when it expires.
 * expires is an absolute tick count, or zero for a permanent route. A
 * refreshed advertised prefix stays learned, so that a router's later
 * withdrawal still removes it, as Linux's fib6_add_rt2node keeps the
 * existing route's RTF_ADDRCONF. */
static void add_prefix(struct netdev *nd, const uint8_t *dst, int plen, uint32_t metric,
                       uint64_t expires)
{
    uint8_t prefix[IP6_ADDR_LEN];
    int i;
    k_memcpy(prefix, dst, sizeof(prefix));
    mask_prefix(prefix, plen);
    i = find(nd, prefix, plen, ip6_any, metric);
    if (i >= 0) {
        if (table6[i].expires)
            table6[i].expires = expires;
        return;
    }
    i = free_slot();
    if (i < 0)
        return;
    k_memset(&table6[i], 0, sizeof(table6[i]));
    table6[i].nd = nd;
    k_memcpy(table6[i].dst, prefix, sizeof(prefix));
    table6[i].plen = (uint8_t)plen;
    table6[i].proto = ROUTE6_PROTO_KERNEL;
    table6[i].metric = metric;
    table6[i].expires = expires;
}

/** Update the selected direct route for a former permanent address.
 * When the metric changes, install its new kernel route after deletion. */
static void update_prefix(struct netdev *nd, const struct netdev_addr6 *a, const uint8_t *dst,
                          int modify)
{
    uint32_t metric = a->metric ? a->metric : ROUTE6_METRIC_PREFIX;
    uint64_t expires =
        a->valid == NETDEV_LIFETIME_FOREVER ? 0 : a->since + (uint64_t)a->valid * TICK_HZ;
    int i = modify ? prefix_slot(nd, dst, a->prefix, 1) : -1;
    if (i >= 0) {
        if (table6[i].metric == metric) {
            table6[i].expires = expires;
            return;
        }
        table6[i].nd = 0;
    }
    add_prefix(nd, dst, a->prefix, metric, expires);
}

/** Delete a permanent address's prefix or let the remaining addresses
 * supply a deadline for a route that was permanent. */
static void cleanup_prefix(struct netdev *nd, const struct netdev_addr6 *a)
{
    uint64_t expires = 0;
    int i;
    for (i = 0; i < nd->naddr6; i++) {
        const struct netdev_addr6 *other = &nd->addr6[i];
        uint64_t end;
        if (other == a || other->prefix != a->prefix ||
            !ip6_prefix_match(other->addr, a->addr, a->prefix))
            continue;
        if (other->noprefix || (other->flags & NETDEV_A6_PERMANENT))
            return;
        end = other->since + (uint64_t)other->valid * TICK_HZ;
        if (end > expires)
            expires = end;
    }
    i = prefix_slot(nd, a->addr, a->prefix, 1);
    if (i < 0)
        return;
    if (!expires)
        table6[i].nd = 0;
    else if (!table6[i].expires)
        table6[i].expires = expires;
}

void route6_address_ready(struct netdev *nd, const struct netdev_addr6 *a)
{
    if (!ip6_is_unspecified(a->peer))
        add_prefix(nd, a->peer, 128, a->metric ? a->metric : ROUTE6_METRIC_PREFIX, 0);
}

void route6_address_update(struct netdev *nd, const struct netdev_addr6 *old,
                           const struct netdev_addr6 *a)
{
    int modify = old && (old->flags & NETDEV_A6_PERMANENT) && !old->noprefix;
    if (nd->loopback)
        return;
    if (old && !ip6_is_unspecified(old->peer) && !ip6_equal(old->peer, a->peer)) {
        int i = prefix_slot(nd, old->peer, old->prefix, 1);
        if (i >= 0)
            table6[i].nd = 0;
    }
    if (!a->noprefix) {
        update_prefix(nd, a, a->addr, modify);
        if (old && !ip6_is_unspecified(a->peer))
            update_prefix(nd, a, a->peer, modify);
    } else if (modify)
        cleanup_prefix(nd, old);
    if (!old && !(a->flags & (NETDEV_A6_TENTATIVE | NETDEV_A6_DADFAILED)))
        route6_address_ready(nd, a);
}

void route6_address_removed(struct netdev *nd, int slot)
{
    struct netdev_addr6 *a = &nd->addr6[slot];
    int i;
    for (i = 0; i < table_size; i++)
        if (table6[i].nd && ip6_equal(table6[i].prefsrc, a->addr) &&
            !has_source(table6[i].nd, a->addr, a))
            k_memset(table6[i].prefsrc, 0, IP6_ADDR_LEN);
    if (!ip6_is_unspecified(a->peer)) {
        i = prefix_slot(nd, a->peer, 128, 0);
        if (i >= 0)
            table6[i].nd = 0;
    }
    if ((a->flags & NETDEV_A6_PERMANENT) && !a->noprefix)
        cleanup_prefix(nd, a);
}

struct netdev *route6_added_local(const uint8_t *dst)
{
    const struct route6 *best = 0;
    int i;
    for (i = 0; i < table_size; i++) {
        const struct route6 *r = &table6[i];
        if (r->nd && r->local && ip6_prefix_match(dst, r->dst, r->plen) &&
            (!best || r->plen > best->plen || (r->plen == best->plen && r->metric < best->metric)))
            best = r;
    }
    return best ? best->nd : 0;
}

/** A table the heap has no room to grow refuses the route; the
 * advertisement that would refresh it comes again. */
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
    if (i >= 0 && !table6[i].learned) {
        if (gw || !lifetime || !table6[i].expires)
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
        table6[i].learned = 1;
        k_memcpy(table6[i].dst, d, IP6_ADDR_LEN);
        table6[i].plen = (uint8_t)plen;
        k_memcpy(table6[i].gw, g, IP6_ADDR_LEN);
        table6[i].metric = metric;
        k_memset(table6[i].prefsrc, 0, IP6_ADDR_LEN);
        table6[i].proto = gw ? ROUTE6_PROTO_RA : ROUTE6_PROTO_KERNEL;
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

    for (i = 0; i < table_size; i++) {
        if (table6[i].nd == nd && (proto < 0 || table6[i].proto == proto)) {
            table6[i].nd = 0;
        }
    }
}

void route6_ra_metric(struct netdev *nd, uint32_t metric)
{
    int i, j;

    for (i = 0; i < table_size; i++) {
        struct route6 *r = &table6[i];

        if (r->nd != nd || !r->learned || r->plen != 0 || r->metric == metric) {
            continue;
        }
        j = find(nd, r->dst, 0, r->gw, metric);
        if (j < 0) {
            r->metric = metric;
            continue;
        }
        if (table6[j].learned && table6[j].expires != 0 &&
            (r->expires == 0 || r->expires > table6[j].expires)) {
            table6[j].expires = r->expires;
        }
        r->nd = 0;
    }
}

void route6_tick(void)
{
    int i;

    for (i = 0; i < table_size; i++) {
        struct route6 *r = &table6[i];

        if (r->nd && r->expires != 0 && tick_count() >= r->expires) {
            r->nd = 0;
        }
    }
    trim_table();
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

/** Resolve the route and next hop under the caller's net_lock. */
static struct netdev *route_find(const uint8_t *dst, int ifindex, uint8_t *via)
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

struct netdev *ip6_route_find(const uint8_t *dst, int ifindex, uint8_t *via)
{
    uint32_t flags = net_lock();
    struct netdev *nd = route_find(dst, ifindex, via);

    net_unlock(flags);
    return nd;
}
