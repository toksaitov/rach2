/* Route and local address checks shared by Internet socket callers. */
#include "address.h"
#include "socket.h"
#include "../net.h"
#include "../ip/ip.h"
#include "../ip6/ip6.h"
#include "../ip6/nd6.h"
#include "../driver/netdev.h"
#include "../../lib/kstring.h"
#include "../../lib/kerrno.h"

/** Check address ownership, scope, interface state and tentative addresses. */
int net_addr_local(net_addr_t a)
{
    struct netdev *nd;
    uint32_t flags;
    int local;

    if (a.family != AF_INET6)
        return ip_is_local(a.v4);
    flags = net_lock();
    nd = netdev_owner6(a.v6);
    local = nd && nd->up && (!a.scope || a.scope == (uint32_t)nd->index) && netdev_holds6(nd, a.v6);
    net_unlock(flags);
    return local;
}

/** Resolve a route and source before a connect or checksum calculation. */
int net_addr_source(net_addr_t bound, net_addr_t dst, int ifindex, net_addr_t *src)
{
    struct netdev *nd;
    uint8_t via[16], from[16];
    uint32_t from4;
    int rc;
    if (!net_addr_any(bound) && bound.family != dst.family)
        return -ENETUNREACH;
    if (dst.family == AF_INET6) {
        nd = ip6_route(dst.v6, dst.scope ? (int)dst.scope : ifindex, via);
        if (!nd)
            return -ENETUNREACH;
        if (!net_addr_any(bound)) {
            if (!net_addr_local(bound) || (ip6_is_loopback(bound.v6) && !nd->loopback))
                return -EADDRNOTAVAIL;
            if (bound.scope && !nd->loopback && bound.scope != (uint32_t)nd->index)
                return -EINVAL;
            *src = bound;
            return 0;
        }
        rc = ip6_source(nd, dst.v6, from);
        if (rc)
            return rc;
        *src = net_addr6(from, nd->loopback ? dst.scope : (uint32_t)nd->index);
        return 0;
    }
    rc = ip_source_on(dst.v4, ifindex, &from4);
    if (rc)
        return rc;
    *src = net_addr_any(bound) ? net_addr4(from4) : bound;
    if (!net_addr_local(*src))
        return -EADDRNOTAVAIL;
    if (IP_IS_LOOPBACK(src->v4) && !ip_is_local(dst.v4))
        return -EINVAL;
    return 0;
}

int net_addr_link_down(net_addr_t dst, int ifindex)
{
    struct netdev *nd;
    uint8_t via[16];
    uint32_t via4;

    if (dst.family == AF_INET6) {
        nd = ip6_route(dst.v6, ifindex ? ifindex : (int)dst.scope, via);
    } else {
        nd = ip_is_local(dst.v4) ? 0 : netdev_route_on(dst.v4, ifindex, &via4);
    }
    return nd && !nd->loopback && !netdev_running(nd);
}

/** A card's hop limit is the routers'; `lo` has none advertised. */
uint8_t socket_hops6(const struct socket *sock, const uint8_t *dst, int ifindex)
{
    uint8_t via[16];
    struct netdev *out;

    if (ip6_is_multicast(dst)) {
        return (uint8_t)sock->mhops6;
    }
    if (sock->hops6 >= 0) {
        return (uint8_t)sock->hops6;
    }
    out = ip6_route_find(dst, ifindex, via);
    return out && !out->loopback ? nd6_hop_limit(out) : IP6_HOP_LIMIT_DEFAULT;
}
