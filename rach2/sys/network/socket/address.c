/* Internet address conversion and source selection for both transports. */
#include "address.h"
#include "socket.h"
#include "../ip6/ip6.h"
#include "../../lib/kstring.h"

/** Make a canonical IPv4 address. */
net_addr_t net_addr4(uint32_t ip)
{
    net_addr_t a;
    k_memset(&a, 0, sizeof(a));
    a.family = AF_INET;
    a.v4 = ip;
    return a;
}

/** Make a canonical IPv6 address with the scope needed to distinguish links. */
net_addr_t net_addr6(const uint8_t *ip, uint32_t scope)
{
    net_addr_t a = net_addr4(0);
    if (ip6_is_v4mapped(ip)) {
        a.v4 =
            ((uint32_t)ip[12] << 24) | ((uint32_t)ip[13] << 16) | ((uint32_t)ip[14] << 8) | ip[15];
    } else {
        a.family = AF_INET6;
        a.scope = ip6_link_scoped(ip) ? scope : 0;
        k_memcpy(a.v6, ip, 16);
    }
    return a;
}

/** Test the unspecified address of either family. */
int net_addr_any(net_addr_t a)
{
    return a.family == AF_INET6 ? ip6_is_unspecified(a.v6) : a.v4 == 0;
}

/** Compare canonical addresses, including a link's identity. */
int net_addr_equal(net_addr_t a, net_addr_t b)
{
    if (a.family != b.family)
        return 0;
    return a.family == AF_INET6 ? a.scope == b.scope && ip6_equal(a.v6, b.v6) : a.v4 == b.v4;
}

/** Convert a canonical address to the IPv6 socket representation; IPv4's
 * unspecified address, which an unbound socket of either family holds,
 * becomes ::. */
void net_addr_to6(net_addr_t a, uint8_t *out)
{
    if (a.family == AF_INET6) {
        k_memcpy(out, a.v6, 16);
    } else {
        k_memset(out, 0, 16);
        if (a.v4 == 0) {
            return;
        }
        out[10] = out[11] = 255;
        out[12] = (uint8_t)(a.v4 >> 24);
        out[13] = (uint8_t)(a.v4 >> 16);
        out[14] = (uint8_t)(a.v4 >> 8);
        out[15] = (uint8_t)a.v4;
    }
}
