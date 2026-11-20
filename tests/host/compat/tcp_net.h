/* IPv4 route and socket models used by the two TCP state machine tests. */
#ifndef RACH2_HOST_TCP_NET_H
#define RACH2_HOST_TCP_NET_H

#include "network/ip6/ip6.h"
#include "network/driver/netdev.h"

/** Resolve every model peer through the fixture's one IPv4 interface. */
int net_addr_source(net_addr_t bound, net_addr_t dst, int ifindex, net_addr_t *src)
{
    (void)ifindex;
    *src = net_addr_any(bound) ? net_addr4(ip_source(dst.v4)) : bound;
    return 0;
}

/** Test ownership using the fixture's IPv4 interface and loopback range. */
int net_addr_local(net_addr_t addr)
{
    return addr.family != AF_INET6 && ip_is_local(addr.v4);
}

/* Whether the model's card reports its link down. */
static int link_down;

/** Report the model card's link, whatever the destination and binding. */
int net_addr_link_down(net_addr_t dst, int ifindex)
{
    (void)dst;
    (void)ifindex;
    return link_down;
}

/** Match the model's IPv4 listener address or its wildcard. */
int socket_address_matches(const struct socket *s, net_addr_t dst)
{
    return net_addr_any(s->local_ip) || net_addr_equal(s->local_ip, dst);
}

/** Test the overlap of two IPv4 model listeners. */
int socket_bind_conflict(const struct socket *a, const struct socket *b)
{
    return net_addr_any(a->local_ip) || net_addr_any(b->local_ip) ||
           net_addr_equal(a->local_ip, b->local_ip);
}

/** The model has no cross-family bindings. */
int socket_stream_conflict(const struct socket *s, uint16_t port)
{
    (void)s;
    (void)port;
    return 0;
}

/** No IPv6 route exists in the IPv4 state machine fixture. */
struct netdev *ip6_route(const uint8_t *dst, int scope, uint8_t *via)
{
    (void)dst;
    (void)scope;
    (void)via;
    return 0;
}

/** No IPv6 route exists for the segment size either. */
struct netdev *ip6_route_find(const uint8_t *dst, int ifindex, uint8_t *via)
{
    (void)dst;
    (void)ifindex;
    (void)via;
    return 0;
}

/** Report Ethernet's MTU for the unused IPv6 link. */
uint32_t nd6_link_mtu(const struct netdev *nd)
{
    (void)nd;
    return 1500;
}

/* The one card every IPv4 peer of the fixtures is reached by, with
 * Ethernet's MTU. */
static struct netdev model_card;

/** Route every IPv4 destination by the model card, to itself, whatever
 * card a binding names. */
struct netdev *netdev_route_on(uint32_t dst, int ifindex, uint32_t *via)
{
    (void)ifindex;
    model_card.mtu = 1500;
    *via = dst;
    return &model_card;
}

/* The card the segment being delivered came by: none, since the model's
 * segments come off no card, which every binding takes. */
int ip_rx_ifindex;

/** Send as the test's `ip_tx_from` does: the model has one card, so the
 * binding and the path MTU discovery mode change nothing. */
int ip_tx_opts(uint32_t src, uint32_t dst, uint8_t proto, const uint8_t *payload, uint16_t len,
               uint8_t ttl, int pmtudisc, int ifindex)
{
    (void)pmtudisc;
    (void)ifindex;
    return ip_tx_from(src, dst, proto, payload, len, ttl);
}

/** Report no card address as this machine's beyond what `ip_is_local`
 * already takes. */
int netdev_owns(uint32_t ip)
{
    (void)ip;
    return 0;
}

/** Report the card's MTU, which no fragmentation-needed report lowers. */
uint32_t ip_path_mtu(const struct netdev *out, uint32_t dst)
{
    (void)dst;
    return out->mtu;
}

/** Report Ethernet's MTU for the unused IPv6 output path. */
uint32_t ip6_path_mtu(const struct netdev *nd, const uint8_t *dst)
{
    (void)nd;
    (void)dst;
    return 1500;
}

/** Refuse IPv6 output, which these IPv4 fixtures never request. */
int ip6_tx(const uint8_t *src, const uint8_t *dst, int scope, uint8_t nh, const uint8_t *data,
           uint16_t len, uint8_t hops)
{
    (void)src;
    (void)dst;
    (void)scope;
    (void)nh;
    (void)data;
    (void)len;
    (void)hops;
    return -ENETUNREACH;
}

#endif
