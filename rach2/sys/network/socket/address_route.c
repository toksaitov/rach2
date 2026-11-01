/* Route and local address checks shared by Internet socket callers. */
#include "address.h"
#include "socket.h"
#include "../ip/ip.h"
#include "../ip6/ip6.h"
#include "../driver/netdev.h"
#include "../../lib/kstring.h"
#include "../../lib/kerrno.h"

/** Check address ownership, scope, interface state and tentative addresses. */
int net_addr_local(net_addr_t a)
{
    struct netdev *nd;
    int i;
    if (a.family != AF_INET6)
        return ip_is_local(a.v4);
    nd = netdev_owner6(a.v6);
    if (!nd || !nd->up || (a.scope && a.scope != (uint32_t)nd->index))
        return 0;
    for (i = 0; i < nd->naddr6; i++) {
        if (ip6_equal(nd->addr6[i].addr, a.v6) &&
            !(nd->addr6[i].flags & (NETDEV_A6_TENTATIVE | NETDEV_A6_DADFAILED)))
            return 1;
    }
    return 0;
}

/** Resolve a route and source before a connect or checksum calculation. */
int net_addr_source(net_addr_t bound, net_addr_t dst, net_addr_t *src)
{
    struct netdev *nd;
    uint8_t via[16], from[16];
    uint32_t via4;
    int rc;
    if (!net_addr_any(bound) && bound.family != dst.family)
        return -ENETUNREACH;
    if (dst.family == AF_INET6) {
        nd = ip6_route(dst.v6, (int)dst.scope, via);
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
    if (!ip_is_local(dst.v4) && !netdev_route(dst.v4, &via4))
        return -ENETUNREACH;
    *src = net_addr_any(bound) ? net_addr4(ip_source(dst.v4)) : bound;
    if (!net_addr_local(*src))
        return -EADDRNOTAVAIL;
    if (IP_IS_LOOPBACK(src->v4) && !ip_is_local(dst.v4))
        return -EINVAL;
    return 0;
}
