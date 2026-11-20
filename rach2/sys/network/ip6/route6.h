#ifndef _ROUTE6_H_
#define _ROUTE6_H_

#include "../../lib/kint.h"

/*
 * route6: the IPv6 routing table behind `ip6_route`.
 *
 * The routes a card's addresses imply are found from them when asked for:
 * the prefix of each address given by a program or made at link up, at
 * metric 256, reached on the link. The routes a router advertises, its
 * on-link prefixes at metric 256 and itself as a default route at metric
 * 1024, and those a program adds, are kept in a table of ROUTE6_MAX, the
 * learned ones with the tick they expire at. A SLAAC address implies no
 * route: its prefix's route is the advertisement's, as on Linux.
 *
 * A lookup takes, of the routes of cards that are up with IPv6 enabled and
 * that match, the one of the longest prefix, then of the lowest metric,
 * then one whose gateway may be reachable (`nd6_router_usable`), then the
 * first found, which puts an implied route before a table's.
 */

#define ROUTE6_MAX 32

/* Who made a route, in Linux's RTPROT_ numbers: the kernel, from an
 * address; the boot line or start-up; a program; a router advertisement. */
#define ROUTE6_PROTO_KERNEL 2
#define ROUTE6_PROTO_BOOT   3
#define ROUTE6_PROTO_STATIC 4
#define ROUTE6_PROTO_RA     9

/* The metrics Linux gives a prefix route and a router's default route. */
#define ROUTE6_METRIC_PREFIX  256
#define ROUTE6_METRIC_DEFAULT 1024

struct netdev;
struct netdev_addr6;

/** One route, copied out for a reader. */
struct route6_row {
    int ifindex;
    uint8_t dst[16];
    uint8_t plen;
    uint8_t prefsrc[16]; /* :: leaves source selection to the interface */
    uint8_t gw[16];      /* :: for a route on the link */
    uint32_t metric;
    uint8_t proto;    /* ROUTE6_PROTO_* */
    uint8_t local;    /* a host route in the local table */
    uint32_t expires; /* seconds left, or 0 for a route that does not expire */
};

/**
 * The `n`th route, counting the implied routes of each card first, then
 * the table's, into `out`: 1, or 0 past the last. Call with interrupts off.
 */
int route6_at(int n, struct route6_row *out);

/**
 * The card a datagram to the unicast `dst` leaves by, of the routes above,
 * only `ifindex`'s when it is not 0, with the next hop, the gateway or
 * `dst` itself, in `via`; NULL with no route. Call with interrupts off.
 */
struct netdev *route6_lookup(const uint8_t *dst, int ifindex, uint8_t *via);

/** Update a distinct peer on an existing address under net_lock. */
void route6_peer_change(struct netdev *nd, struct netdev_addr6 *a, const uint8_t *peer);

/** Preserve a prefix with an independent lifetime before removing slot.
 * The caller holds net_lock and slot is still present. */
void route6_address_removed(struct netdev *nd, int slot);

/** The device of an explicit local route covering dst, or NULL. */
struct netdev *route6_added_local(const uint8_t *dst);

/** Copy the route's specified source and return one, or return zero
 * when ordinary source selection applies. Call under net_lock. */
int route6_preferred_source(const struct netdev *nd, const uint8_t *dst, uint8_t *src);

/** Select a main-table route, including its gateway and preferred source.
 * An interface index of zero searches every card. Call under net_lock. */
struct netdev *route6_resolve(const uint8_t *dst, int ifindex, struct route6_row *row);

/** Apply a netlink or ioctl route under net_lock. A zero metric or
 * protocol in a deletion omits that selector. Replacement validates
 * the new route before removing the old one. */
int route6_change_row(struct netdev *nd, const struct route6_row *r, int del, int replace);

/**
 * Put in the table, or refresh, the route a router advertised: to
 * `dst`/`plen` by `nd`, through `gw` or on the link for NULL, at `metric`,
 * for `lifetime` seconds, NETDEV_LIFETIME_FOREVER for no end; a lifetime of
 * 0 removes it. A route a program added is left alone. Interrupts off.
 */
void route6_learn(struct netdev *nd, const uint8_t *dst, int plen, const uint8_t *gw,
                  uint32_t metric, uint32_t lifetime);

/** Remove the table's routes by `nd` that `proto` made, or all of them for
 * -1. Interrupts off. */
void route6_forget(struct netdev *nd, int proto);

/**
 * Move the default routes routers advertised by `nd` to `metric`, each
 * keeping its time left; one whose router already has a route at `metric`
 * is merged into it, which keeps the later end. Interrupts off.
 */
void route6_ra_metric(struct netdev *nd, uint32_t metric);

/** Remove the learned routes whose time has run out. Called from
 * `nd6_tick`. */
void route6_tick(void);

#endif /* _ROUTE6_H_ */
