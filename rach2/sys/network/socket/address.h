/* Internet addresses shared by sockets and transports. */
#ifndef _NET_ADDRESS_H_
#define _NET_ADDRESS_H_
#include "../../lib/kint.h"

/* IPv4 is a host-order word; IPv6 is in wire order. Scope is an interface
 * index for link-scoped IPv6 addresses, zero otherwise. Mapped addresses
 * are stored as IPv4 and converted at the socket boundary. */
typedef struct net_addr {
    uint16_t family;
    uint32_t scope;
    uint32_t v4;
    uint8_t v6[16];
} net_addr_t;

/** Make an IPv4 address from a host-order word. */
net_addr_t net_addr4(uint32_t ip);
/** Make an IPv6 address, normalizing mapped addresses to IPv4. */
net_addr_t net_addr6(const uint8_t *ip, uint32_t scope);
/** Whether an address is unspecified in its family. */
int net_addr_any(net_addr_t a);
/** Whether two canonical addresses, including scope, are equal. */
int net_addr_equal(net_addr_t a, net_addr_t b);
/** Write an IPv6 address or the mapped form of an IPv4 address; IPv4's
 * unspecified address is written as ::. */
void net_addr_to6(net_addr_t a, uint8_t *out);
/** Whether the address belongs to this machine and is usable now. */
int net_addr_local(net_addr_t a);
/** Choose a usable source and route for dst, respecting a bound source.
 * Returns zero or a negative errno before a transport allocates state.
 * The caller holds net_lock. */
int net_addr_source(net_addr_t bound, net_addr_t dst, net_addr_t *src);
#endif
