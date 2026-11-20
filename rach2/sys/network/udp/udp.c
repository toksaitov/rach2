/*
 * udp.c: UDP, as udp.h describes it: the list of bindings, which maps a port
 * at an address of this machine, or at any (`local_ip` 0), to a socket, and
 * the datagrams received and sent. The 8-byte header is, big-endian, the
 * source port, destination port, length and checksum.
 */

#include "../../klog.h"
#include "../../lib/kerrno.h"
#include "udp.h"
#include "../net.h"
#include "../socket/socket.h"
#include "../ip/ip.h"
#include "../ip6/ip6.h"
#include "../driver/netdev.h"
#include "../core/byteorder.h"
#include "../../serial.h"
#include "../../lib/kstring.h"
#include "../../lib/krandom.h"
#include "../../mm/kheap.h"

/* A destination port and the socket that receives its datagrams: one record
   from the heap for each binding, on a list, the newest first. */
typedef struct udp_binding {
    struct udp_binding *next;
    uint16_t port;
    struct socket *sock;
} udp_binding_t;

static udp_binding_t *bindings;

/* The bindings of each port, indexed by port, which `udp_bind_ephemeral`
 * reads in place of the list. Every port is counted, since a write of
 * ip_local_port_range may move the ephemeral range over ports bound
 * before it. */
static uint32_t udp_ports[65536];

/** Count a binding of `port` made (`delta` 1) or taken away (-1) in
 * `udp_ports`. */
static void udp_port_count(uint16_t port, int delta)
{
    udp_ports[port] += (uint32_t)delta;
}

/* The datagram being sent when it fits one frame; a larger one is built in
 * a block of the heap. */
static uint8_t udp_tx_buf[UDP_HDR_SIZE + 1500];

/**
 * The checksum of the UDP datagram of `len` bytes at `data` from `src` to
 * `dst` over IPv4, with its pseudo-header (RFC 768): the two addresses, the
 * protocol and the length. A datagram whose checksum field is right gives 0.
 */
static uint16_t udp4_checksum(uint32_t src, uint32_t dst, const uint8_t *data, uint16_t len)
{
    uint32_t sum = (uint16_t)~ip_checksum(data, len);

    sum += (src >> 16) + (src & 0xFFFFu) + (dst >> 16) + (dst & 0xFFFFu) + IP_PROTO_UDP + len;
    while (sum >> 16) {
        sum = (sum & 0xFFFFu) + (sum >> 16);
    }
    return (uint16_t)(sum ^ 0xFFFFu);
}

/** Two bindings to different addresses do not meet, as on Linux:
 * 127.0.0.1 and an interface's address may each have a server on port 53.
 * The list is searched only for a port `udp_ports` counts a binding of, so
 * that an ephemeral bind reads no other socket's binding. */
int udp_bind(uint16_t port, struct socket *sock)
{
    udp_binding_t *b;
    uint32_t flags;
    int met = 0, shared = sock && sock->reuseaddr;

    flags = net_lock();

    for (b = udp_ports[port] ? bindings : NULL; b; b = b->next) {
        if (b->port == port && (!b->sock || socket_bind_conflict(b->sock, sock))) {
            met = 1;
            if (!b->sock || !b->sock->reuseaddr) {
                shared = 0;
            }
        }
    }
    if (met && !shared) {
        net_unlock(flags);
        klog(KLOG_DEBUG, "udp", "bind: port %d is already bound", (int)port);
        return -1;
    }

    b = kzalloc(sizeof(*b));
    if (!b) {
        net_unlock(flags);
        klog(KLOG_WARN, "udp", "bind: no memory for a binding, port %d refused", (int)port);
        return -ENOMEM;
    }
    b->port = port;
    b->sock = sock;
    b->next = bindings;
    bindings = b;
    udp_port_count(port, 1);
    net_unlock(flags);
    return 0;
}

/**
 * The scan starts at a random port of the range, as Linux's
 * udp_lib_get_port starts, so that a host off the path cannot guess a
 * resolver's source port (RFC 5452, RFC 6056), goes forward, wrapping at the
 * top, past every port `udp_ports` counts a binding of, and stops at the
 * first failure to allocate, since no other port would help. A send from a
 * socket without a port (`dgram_send`), a `bind` to port 0 and a `connect`
 * call this, each in system call context, where `krandom_u32` may be
 * drawn. The port is recorded in `sock->local_port`, so that the socket's
 * later sends do not bind a fresh port each time and its close unbinds it.
 */
uint16_t udp_bind_ephemeral(struct socket *sock)
{
    uint32_t r = net_port_range, low = NET_PORT_LOW(r);
    uint32_t span = NET_PORT_HIGH(r) - low + 1u, start = krandom_u32() % span, i, k;
    uint16_t port;

    for (i = 0; i < span; i++) {
        k = start + i;
        if (k >= span) {
            k -= span;
        }
        port = (uint16_t)(low + k);
        if (udp_ports[port] == 0) {
            int rc = udp_bind(port, sock);

            if (rc == -ENOMEM) {
                return 0;
            }
            if (rc == 0) {
                sock->local_port = port;
                return port;
            }
        }
    }
    return 0;
}

/**
 * Bindings are matched by socket and not by port: two sockets may share a
 * port, and releasing the first entry with that port would take the other
 * one's binding away and leave this one's behind.
 */
void udp_unbind(struct socket *sock)
{
    udp_binding_t **link;
    uint32_t flags;

    flags = net_lock();
    link = &bindings;
    while (*link) {
        udp_binding_t *b = *link;

        if (b->sock == sock) {
            *link = b->next;
            udp_port_count(b->port, -1);
            kfree(b);
        } else {
            link = &b->next;
        }
    }
    net_unlock(flags);
}

/**
 * A length field below the header's size would underflow the payload's
 * length and hand the receive memory after it to user space. The socket
 * connected to the sender takes the datagram before any other, because it
 * asked for that sender by name, and a renewal's answer must not go to a
 * socket that is merely listening. The payload goes to `socket_udp_deliver`
 * with the sender's address and port. A datagram is counted in net_mib, or
 * for IPv6 in net_mib6.
 */
static int udp_rx_addr(net_addr_t src_ip, net_addr_t dst_ip, const uint8_t *data, uint16_t len,
                       const struct net_rxinfo *info)
{
    uint16_t dst_port, udp_len, payload_len;
    int v6 = dst_ip.family == AF_INET6;
    uint32_t *in_datagrams = v6 ? &net_mib6.udp6_in_datagrams : &net_mib.udp_in_datagrams;
    uint32_t *in_errors = v6 ? &net_mib6.udp6_in_errors : &net_mib.udp_in_errors;

    if (len < UDP_HDR_SIZE) {
        ++*in_errors;
        return 0;
    }

    dst_port = read16be(data + 2);
    udp_len = read16be(data + 4);

    if (udp_len < UDP_HDR_SIZE || udp_len > len) {
        ++*in_errors;
        return 0;
    }
    payload_len = (uint16_t)(udp_len - UDP_HDR_SIZE);

    {
        struct socket *listener = NULL;
        uint16_t src_port = read16be(data + 0);
        const udp_binding_t *b;

        for (b = bindings; b; b = b->next) {
            struct socket *cand = b->sock;

            if (b->port != dst_port || !cand || !socket_address_matches(cand, dst_ip)) {
                continue;
            }
            if (cand->connected) {
                if (net_addr_equal(cand->remote_ip, src_ip) && cand->remote_port == src_port) {
                    ++*in_datagrams;
                    socket_udp_deliver_info(cand, src_ip, src_port, data + UDP_HDR_SIZE,
                                            payload_len, info);
                    return 0;
                }
                continue;
            }
            if (!listener || (net_addr_any(listener->local_ip) && !net_addr_any(cand->local_ip))) {
                listener = cand;
            }
        }
        if (!listener) {
            if (v6) {
                net_mib6.udp6_no_ports++;
            } else {
                net_mib.udp_no_ports++;
            }
            return -1;
        }
        ++*in_datagrams;
        socket_udp_deliver_info(listener, src_ip, src_port, data + UDP_HDR_SIZE, payload_len, info);
    }
    return 0;
}

void udp_refused(uint16_t local_port, uint32_t remote_ip, uint16_t remote_port)
{
    const udp_binding_t *b;

    for (b = bindings; b; b = b->next) {
        struct socket *s = b->sock;

        if (b->port == local_port && s && s->connected && s->ns == ip_ns &&
            net_addr_equal(s->remote_ip, net_addr4(remote_ip)) && s->remote_port == remote_port) {
            s->so_error = ECONNREFUSED;
            wq_wake(&s->waiters, WQ_DATA);
        }
    }
}

/**
 * Choose the interface and source of a datagram from `sock` to the IPv6
 * `dst` under the control messages `opt`, into `*dst` and `*from`, as
 * `udp_tx` says. Returns 0 or a negative errno. The caller holds net_lock.
 */
static int udp_route6(const struct socket *sock, net_addr_t *dst, const struct net_txopt *opt,
                      net_addr_t *from)
{
    if (opt && opt->ifindex) {
        if (dst->scope && dst->scope != (uint32_t)opt->ifindex) {
            return -EINVAL;
        }
        if (!netdev_by_index(opt->ifindex)) {
            return -ENODEV;
        }
        dst->scope = (uint32_t)opt->ifindex;
    } else if (!dst->scope && sock->bound_device) {
        dst->scope = (uint32_t)sock->bound_device; /* SO_BINDTODEVICE */
    } else if (!dst->scope && ip6_is_multicast(dst->v6) && sock->mif6) {
        dst->scope = (uint32_t)sock->mif6; /* a group of wider scope */
    }
    if (opt && opt->src.family == AF_INET6 && !net_addr_any(opt->src)) {
        net_addr_t src = opt->src;

        src.scope = 0;
        if (!net_addr_local(src)) {
            return -EINVAL;
        }
        return net_addr_source(src, *dst, 0, from);
    }
    return net_addr_source(sock->local_ip, *dst, 0, from);
}

/**
 * The shared transmit buffer is filled and sent under `net_lock`, since a
 * sender in an interrupt must not enter it at the same time (net.h); a
 * datagram past it is built in a block of the caller's own, which IP
 * fragments. The datagram is sent in the socket's network namespace
 * (`ip_ns`).
 */
int udp_tx(const struct socket *sock, net_addr_t dst_ip, uint16_t dst_port, const uint8_t *payload,
           uint16_t plen, const struct net_txopt *opt)
{
    const struct netns *was;
    uint16_t udp_len;
    uint32_t flags;
    uint8_t *buf = udp_tx_buf;
    int r;
    net_addr_t from;

    if (plen > (dst_ip.family == AF_INET6 ? UDP6_PAYLOAD_MAX : UDP_PAYLOAD_MAX)) {
        return -EMSGSIZE;
    }

    udp_len = (uint16_t)(UDP_HDR_SIZE + plen);
    if (udp_len > sizeof(udp_tx_buf)) {
        buf = kmalloc(udp_len);
        if (!buf) {
            return -ENOMEM;
        }
    }

    flags = net_lock();

    if (dst_ip.family == AF_INET6) {
        r = udp_route6(sock, &dst_ip, opt, &from);
    } else {
        r = net_addr_source(sock->local_ip, dst_ip, sock->bound_device, &from);
    }
    if (r < 0) {
        net_unlock(flags);
        if (buf != udp_tx_buf) {
            kfree(buf);
        }
        return r;
    }
    write16be(buf + 0, sock->local_port);
    write16be(buf + 2, dst_port);
    write16be(buf + 4, udp_len);
    write16be(buf + 6, 0x0000);
    k_memcpy(buf + UDP_HDR_SIZE, payload, plen);

    was = ip_ns;
    ip_ns = sock->ns;
    if (dst_ip.family == AF_INET6) {
        uint16_t sum = ip6_checksum(from.v6, dst_ip.v6, IP6_NH_UDP, buf, udp_len);
        int tclass = opt && opt->tclass >= 0 ? opt->tclass : sock->tclass6;
        uint8_t hlim = opt && opt->hlim >= 0 ? (uint8_t)opt->hlim
                                             : socket_hops6(sock, dst_ip.v6, (int)dst_ip.scope);

        write16be(buf + 6, sum ? sum : 0xffff);
        r = ip6_tx_opts(from.v6, dst_ip.v6, (int)dst_ip.scope, IP6_NH_UDP, buf, udp_len, hlim,
                        IP6_TXF_TCLASS(tclass) | (sock->mloop6 ? IP6_TXF_LOOP : 0));
    } else {
        /* A source ip_tx_opts keeps is summed; one it chooses, for a
         * machine with no address, leaves 0, which says "no checksum". */
        if (from.v4 != 0 && ip_is_local(from.v4)) {
            uint16_t sum = udp4_checksum(from.v4, dst_ip.v4, buf, udp_len);

            write16be(buf + 6, sum ? sum : 0xffff);
        }
        r = ip_tx_opts(from.v4, dst_ip.v4, IP_PROTO_UDP, buf, udp_len, sock->ip_ttl, sock->pmtudisc,
                       sock->bound_device);
    }
    ip_ns = was;
    if (r == 0 && dst_ip.family == AF_INET6) {
        net_mib6.udp6_out_datagrams++;
    } else if (r == 0) {
        net_mib.udp_out_datagrams++;
    }

    net_unlock(flags);
    if (buf != udp_tx_buf) {
        kfree(buf);
    }
    return r;
}

/** Deliver an IPv4 datagram through the common address-aware binding list,
 * after checking its checksum when the sender gave one; 0 says it gave
 * none, which IPv4 allows. */
int udp_rx(uint32_t src, uint32_t dst, const uint8_t *data, uint16_t len)
{
    struct net_rxinfo info;

    if (len >= UDP_HDR_SIZE && read16be(data + 6) != 0 && read16be(data + 4) >= UDP_HDR_SIZE &&
        read16be(data + 4) <= len && udp4_checksum(src, dst, data, read16be(data + 4)) != 0) {
        net_mib.udp_in_errors++;
        return 0;
    }

    info.dst = net_addr4(dst);
    info.ifindex = 0;
    info.hlim = -1;
    info.tclass = -1;
    return udp_rx_addr(net_addr4(src), net_addr4(dst), data, len, &info);
}

/** Verify IPv6's mandatory UDP checksum before looking up a binding. */
int udp6_rx(int ifindex, const uint8_t *ip6h, const uint8_t *data, uint16_t len)
{
    const uint8_t *src = ip6h + 8, *dst = ip6h + 24;
    struct net_rxinfo info;
    uint16_t size;

    if (len < UDP_HDR_SIZE) {
        net_mib6.udp6_in_errors++;
        return 0;
    }
    size = read16be(data + 4);
    if (size < UDP_HDR_SIZE || size != len || !read16be(data + 6) ||
        ip6_checksum(src, dst, IP6_NH_UDP, data, size) != 0) {
        net_mib6.udp6_in_errors++;
        return 0;
    }
    info.dst = net_addr6(dst, (uint32_t)ifindex);
    info.ifindex = (uint32_t)ifindex;
    info.hlim = (int16_t)ip6h[7];
    info.tclass = (int16_t)(((ip6h[0] & 0x0f) << 4) | (ip6h[1] >> 4));
    return udp_rx_addr(net_addr6(src, (uint32_t)ifindex), net_addr6(dst, (uint32_t)ifindex), data,
                       size, &info);
}
