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

/** One route, copied out for a reader. */
struct route6_row {
    int ifindex;
    uint8_t dst[16];
    uint8_t plen;
    uint8_t gw[16]; /* :: for a route on the link */
    uint32_t metric;
    uint8_t proto;    /* ROUTE6_PROTO_* */
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

/**
 * Add the route to `dst`/`plen` by `nd`, through the gateway `gw`, NULL or
 * :: for on the link, at `metric`, made by `proto`. Returns 0, -EINVAL for
 * a prefix past 128, a NULL `nd` or `lo`, or a gateway that is a group or
 * this machine's, -EEXIST for a route of that prefix, card, gateway and
 * metric, or -ENOSPC for a full table. Takes net_lock.
 */
int route6_add(struct netdev *nd, const uint8_t *dst, int plen, const uint8_t *gw, uint32_t metric,
               int proto);

/** Remove the table's route to `dst`/`plen` by `nd`, or by any card for
 * NULL, through `gw`, or any gateway for NULL: 0, or -ESRCH. Takes
 * net_lock. */
int route6_del(struct netdev *nd, const uint8_t *dst, int plen, const uint8_t *gw);

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

/** Remove the learned routes whose time has run out. Called from
 * `nd6_tick`. */
void route6_tick(void);

#endif /* _ROUTE6_H_ */
