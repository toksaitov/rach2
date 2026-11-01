/*
 * route6.c: the IPv6 route and path MTU lookups, as ip6.h describes them:
 * this machine's addresses by `lo`, and the destinations of one link by the
 * interface the caller names.
 */

#include "ip6.h"
#include "../driver/netdev.h"
#include "../../lib/kstring.h"

/** The interface of index `ifindex`, or NULL for one that does not exist. */
static struct netdev *by_index(int ifindex)
{
    return ifindex >= 1 && ifindex <= netdev_listed() ? netdev_at(ifindex - 1) : 0;
}

/**
 * A group of interface scope, ff01::/16, and the reserved scopes 0 and 15
 * have no route, since no datagram to one may leave the machine, and `lo`
 * carries no multicast. A link-scoped destination with no interface named
 * has one only on a machine with one card, which is the link it means. An
 * interface that is down carries nothing, as `netdev_route` judges IPv4's.
 */
struct netdev *ip6_route(const uint8_t *dst, int ifindex, uint8_t *via)
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
    }
    if (nd && (!nd->up || (nd->loopback && !ip6_is_loopback(dst) && !netdev_owner6(dst)))) {
        nd = 0;
    }
    if (!nd) {
        net_mib6.ip6_out_no_routes++;
        return 0;
    }
    k_memcpy(via, dst, IP6_ADDR_LEN);
    return nd;
}

uint32_t ip6_path_mtu(const struct netdev *out, const uint8_t *dst)
{
    (void)dst;
    return out ? IP6_MTU : 0u;
}
