/*
 * netif.c: the interface, route and ARP ioctls, as netif.h describes them,
 * and netdev.h's `net_ns_seen` and `netdev_seen_count`, which give what the
 * caller's network namespace sees.
 *
 * The `struct ifreq`, `struct rtentry` and `struct arpreq` layouts are here:
 * the fields of their sockaddrs as offsets, and the structures as C
 * declarations whose types give them each processor's layout. What a change
 * of address drops is in netconf.c.
 */

#include "netif.h"
#include "net.h"
#include "netconf.h"
#include "driver/netdev.h"
#include "arp/arp.h"
#include "ip6/ip6.h"
#include "ip6/route6.h"
#include "socket/socket.h"
#include "../lib/kerrno.h"
#include "../lib/kstring.h"
#include "../mm/usercopy.h"
#include "../proc/process.h"
#include "../proc/ns.h"
#include "../mm/kheap.h"

/* struct ifreq: name at 0 (16 bytes), then the union, whose widest member is
 * a struct ifmap of two longs: 32 bytes on i686 and 40 on x86-64. Inside the
 * union a sockaddr puts its family at 0 and an IPv4 address at 4; the flags
 * are a short at the union's start and the index an int there. */
struct ifreq_shape {
    char name[16];
    union {
        uint8_t addr[16];
        struct {
            unsigned long mem_start, mem_end;
            unsigned short base_addr;
            unsigned char irq, dma, port;
        } map;
        uintptr_t data;
    } u;
};

#define IFREQ_LEN      sizeof(struct ifreq_shape)
#define IFREQ_NAME     0
#define IFREQ_NAMESIZE 16
#define IFREQ_UNION    16
#define IFREQ_SA_ADDR  (IFREQ_UNION + 4)
#define IFREQ_HW_ADDR  (IFREQ_UNION + 2)

/* Linux's struct rtentry: a pad word, three sockaddrs (destination,
 * gateway, mask), the flags, the metric, and a pointer to the interface's
 * name, 84 bytes on i686 and 120 on x86-64. The name matters: a route names
 * a device, and a request naming one this machine does not have must not
 * change the one it does. */
struct kernel_rtentry {
    unsigned long pad1;
    uint8_t dst[16];
    uint8_t gateway[16];
    uint8_t genmask[16];
    unsigned short flags;
    short pad2;
    unsigned long pad3;
    uintptr_t pad4;
    short metric;
    uintptr_t dev;
    unsigned long mtu;
    unsigned long window;
    unsigned short irtt;
};

/* struct arpreq: the protocol address, a sockaddr with its IPv4 address at
 * 4, the hardware address, a sockaddr with its family at 16 and its bytes at
 * 18, the flags at 32, a mask, and the interface's name at 52, sixteen
 * bytes, for 68 in all. */
#define ARPREQ_PA_ADDR 4
#define ARPREQ_HA_FAM  16
#define ARPREQ_HA_ADDR 18
#define ARPREQ_FLAGS   32
#define ARPREQ_DEV     52
#define ARPREQ_LEN     68

#define ATF_PERM 0x04
#define ATF_PUBL 0x08

#define RTF_UP      0x0001
#define RTF_GATEWAY 0x0002

#define ARPHRD_ETHER 1

/** The 32-bit big-endian address at `p`, as a host-order number. */
static uint32_t be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

/** Store `v` at `p` in network order. */
static void put_be32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

int net_ns_seen(void)
{
    return proc_netns(current_proc) == &init_netns;
}

int netdev_seen_count(void)
{
    return net_ns_seen() ? netdev_listed() : 0;
}

struct netdev *netif_ns_lo(void)
{
    return current_proc ? proc_netns(current_proc)->lo : 0;
}

struct netdev *netif_ns_lo_new(void)
{
    struct netdev *nd = kmalloc(sizeof(*nd));

    if (nd) {
        netdev_loopback_init(nd, 1);
    }
    return nd;
}

void netif_ns_lo_set_up(struct netdev *nd, int up)
{
    nd->up = up;
    if (up && nd->ip == 0) {
        nd->ip = 0x7F000001u;
        nd->mask = 0xFF000000u;
    }
}

int net_admin(void)
{
    if (!current_proc) {
        return 0;
    }
    return net_ns_seen() ? proc_privileged(current_proc)
                         : ns_owner_privileged(current_proc, &proc_netns(current_proc)->c);
}

/**
 * The interface a caller named, or NULL when this machine has no such one.
 *
 * The name comes out of a caller's `ifreq`, so it is read only as far as the
 * field goes and only up to its terminator; a name that fills the field
 * without one names nothing. In a network namespace other than the first,
 * only `lo` names one, the namespace's own.
 */
static struct netdev *named(const uint8_t *name)
{
    char want[IFREQ_NAMESIZE];
    int i;

    for (i = 0; i < IFREQ_NAMESIZE - 1; i++) {
        want[i] = (char)name[i];
        if (want[i] == '\0') {
            break;
        }
    }
    if (i == IFREQ_NAMESIZE - 1) {
        return 0;
    }
    want[i] = '\0';
    if (!net_ns_seen()) {
        return want[0] == 'l' && want[1] == 'o' && want[2] == '\0' ? netif_ns_lo() : 0;
    }
    return netdev_by_name(want);
}

int netif_name_is_ours(const char *name)
{
    return netdev_by_name(name) != 0;
}

/** Fill the sockaddr in an ifreq's union with an IPv4 address. */
static void set_sa(uint8_t *ifr, uint32_t ip)
{
    k_memset(ifr + IFREQ_UNION, 0, IFREQ_LEN - IFREQ_UNION);
    ifr[IFREQ_UNION] = 2; /* AF_INET, host byte order, as Linux writes it */
    put_be32(ifr + IFREQ_SA_ADDR, ip);
}

/** Put `v` in the union of an ifreq, as the int or short there holds it. */
static void set_word(uint8_t *ifr, uint32_t v)
{
    k_memset(ifr + IFREQ_UNION, 0, IFREQ_LEN - IFREQ_UNION);
    ifr[IFREQ_UNION] = (uint8_t)(v & 0xff);
    ifr[IFREQ_UNION + 1] = (uint8_t)((v >> 8) & 0xff);
    ifr[IFREQ_UNION + 2] = (uint8_t)((v >> 16) & 0xff);
    ifr[IFREQ_UNION + 3] = (uint8_t)((v >> 24) & 0xff);
}

/** The hardware type of `nd`'s address: Ethernet, `lo`'s loopback, or
 * none for an interface of IP datagrams. */
static uint32_t if_type(const struct netdev *nd)
{
    return (uint32_t)NETDEV_TYPE(nd);
}

/** The flags SIOCGIFFLAGS reports: up as an administrator left it, running
 * while it is up and `netdev_running` judges the link up, as on Linux, and
 * those of the interface's kind (`NETIF_KIND_FLAGS`). */
static uint32_t if_flags(struct netdev *nd)
{
    uint32_t flags = NETIF_KIND_FLAGS(nd);

    if (nd->up) {
        flags |= IFF_UP;
        if (netdev_running(nd)) {
            flags |= IFF_RUNNING;
        }
    }
    return flags;
}

/** A setter for `lo`: its own address and mask again, and being up, change
 * nothing and are taken; anything else is EOPNOTSUPP. */
static int32_t loopback_set(uint32_t request, const uint8_t *ifr, const struct netdev *nd)
{
    uint32_t v = be32(ifr + IFREQ_SA_ADDR);

    if (request == SIOCSIFADDR || request == SIOCSIFNETMASK) {
        return v == (request == SIOCSIFADDR ? nd->ip : nd->mask) ? 0 : -EOPNOTSUPP;
    }
    return request == SIOCSIFFLAGS && (ifr[IFREQ_UNION] & IFF_UP) ? 0 : -EOPNOTSUPP;
}

/** A setter for a network namespace's `lo`: up or down, or 127.0.0.1 and
 * 255.0.0.0, its only address and mask; anything else is EOPNOTSUPP. */
static int32_t ns_loopback_set(uint32_t request, const uint8_t *ifr, struct netdev *nd)
{
    uint32_t v = be32(ifr + IFREQ_SA_ADDR);

    if (request == SIOCSIFFLAGS) {
        netif_ns_lo_set_up(nd, (ifr[IFREQ_UNION] & IFF_UP) != 0);
        return 0;
    }
    if (request == SIOCSIFADDR && v == 0x7F000001u) {
        nd->ip = v;
        nd->mask = 0xFF000000u;
        return 0;
    }
    return request == SIOCSIFNETMASK && nd->ip != 0 && v == nd->mask ? 0 : -EOPNOTSUPP;
}

/**
 * Answer SIOCGIFNAME: name in the request `ifr`, read from user memory at
 * `arg`, the interface whose index its union holds, and write the request
 * back. Returns 0, -ENODEV for an index the caller does not see, or -EFAULT.
 */
static int32_t ifreq_name(uint8_t *ifr, uintptr_t arg)
{
    int32_t index =
        (int32_t)((uint32_t)ifr[IFREQ_UNION] | ((uint32_t)ifr[IFREQ_UNION + 1] << 8) |
                  ((uint32_t)ifr[IFREQ_UNION + 2] << 16) | ((uint32_t)ifr[IFREQ_UNION + 3] << 24));
    struct netdev *at;

    if (!net_ns_seen()) {
        at = index == 1 ? netif_ns_lo() : 0;
    } else {
        at = netdev_by_index(index);
    }
    if (!at || at->index != index) {
        return -ENODEV;
    }
    k_memset(ifr + IFREQ_NAME, 0, IFREQ_NAMESIZE);
    k_memcpy(ifr + IFREQ_NAME, at->name, k_strlen(at->name));
    if (copy_to_user((void __user *)arg, ifr, IFREQ_LEN) != 0) {
        return -EFAULT;
    }
    return 0;
}

/**
 * Handle the requests that read or write an interface.
 *
 * The name is checked first for every one of them but SIOCGIFNAME, which
 * takes an index in the union and answers with a name, so that a request
 * about an interface the caller does not see is ENODEV whatever it asks. The
 * setters need privilege over the caller's network namespace (`net_admin`).
 *
 * The setters return without copying the request back, since they only read
 * it and a failed copy would report a refusal for a change already made.
 * SIOCSIFFLAGS sets only whether the interface is up, and SIOCSIFMTU sets
 * the MTU within the interface's range (`netdev_set_mtu`), EINVAL outside
 * it. The machine's `lo` takes no other change but its own address and
 * mask again and being up, EOPNOTSUPP; a namespace's takes what
 * `ns_loopback_set` says. Both report Linux's loopback type, MTU and
 * flags. SIOCSIFADDR gives
 * `netconf_set_address` no mask: the mask that goes with the address is the
 * classful one until SIOCSIFNETMASK arrives, never the one left from the
 * address before, and `ifconfig` issues the two in that order. A subnet's
 * broadcast address follows from the subnet, so SIOCSIFBRDADDR accepts the
 * one the interface already derives, or 255.255.255.255 with no subnet, and
 * refuses any other with EINVAL.
 *
 * Returns 0 or a negative errno: EFAULT for a request that cannot be read
 * or written back, ENODEV for an interface the caller does not see, EPERM
 * for a setter without privilege, and the error of the netconf.h call.
 */
static int32_t ifreq_ioctl(uint32_t request, uintptr_t arg)
{
    uint8_t ifr[IFREQ_LEN];
    struct netdev *nd;
    int setter;

    if (copy_from_user(ifr, (const void __user *)arg, IFREQ_LEN) != 0) {
        return -EFAULT;
    }

    if (request == SIOCGIFNAME) {
        return ifreq_name(ifr, arg);
    }

    nd = named(ifr + IFREQ_NAME);
    if (!nd) {
        return -ENODEV;
    }

    setter = (request == SIOCSIFADDR || request == SIOCSIFNETMASK || request == SIOCSIFBRDADDR ||
              request == SIOCSIFFLAGS || request == SIOCSIFMTU || request == SIOCSIFDSTADDR);
    if (setter && !net_admin()) {
        return -EPERM;
    }
    if (request == SIOCSIFMTU) {
        uint32_t mtu = (uint32_t)ifr[IFREQ_UNION] | ((uint32_t)ifr[IFREQ_UNION + 1] << 8) |
                       ((uint32_t)ifr[IFREQ_UNION + 2] << 16) |
                       ((uint32_t)ifr[IFREQ_UNION + 3] << 24);

        return netdev_set_mtu(nd, mtu);
    }

    if (setter && !net_ns_seen()) {
        return ns_loopback_set(request, ifr, nd);
    }
    if (setter && nd->loopback) {
        return loopback_set(request, ifr, nd);
    }
    switch (request) {
    case 0x8b01: /* cfg80211 exposes no wireless extensions. */
        return -EOPNOTSUPP;
    case SIOCGIFINDEX:
        set_word(ifr, (uint32_t)nd->index);
        break;
    case SIOCGIFHWADDR:
        set_word(ifr, if_type(nd));
        k_memcpy(ifr + IFREQ_HW_ADDR, nd->mac, 6);
        break;
    case SIOCGIFADDR:
        set_sa(ifr, nd->ip);
        break;
    case SIOCGIFDSTADDR:
        if (!nd->ip) {
            return -EADDRNOTAVAIL;
        }
        set_sa(ifr, netdev_addr_dst(nd, -1));
        break;
    case SIOCGIFNETMASK:
        set_sa(ifr, nd->mask);
        break;
    case SIOCGIFBRDADDR:
        set_sa(ifr, netdev_addr_brd(nd, -1));
        break;
    case SIOCGIFMTU:
        set_word(ifr, nd->mtu);
        break;
    case SIOCGIFFLAGS:
        set_word(ifr, if_flags(nd));
        break;
    case SIOCSIFFLAGS: {
        uint16_t flags =
            (uint16_t)((uint16_t)ifr[IFREQ_UNION] | ((uint16_t)ifr[IFREQ_UNION + 1] << 8));

        return netconf_set_up(nd, (flags & IFF_UP) != 0);
    }
    case SIOCSIFADDR:
        return netconf_set_address(nd, be32(ifr + IFREQ_SA_ADDR), 0);
    case SIOCSIFDSTADDR:
        if (ifr[IFREQ_UNION] != AF_INET || ifr[IFREQ_UNION + 1] != 0) {
            return -EINVAL;
        }
        return netconf_set_peer(nd, be32(ifr + IFREQ_SA_ADDR));
    case SIOCSIFNETMASK:
        return netconf_set_netmask(nd, be32(ifr + IFREQ_SA_ADDR));
    case SIOCSIFBRDADDR:
        return netconf_set_broadcast(nd, be32(ifr + IFREQ_SA_ADDR));
    default:
        return -ENOTTY;
    }

    if (copy_to_user((void __user *)arg, ifr, IFREQ_LEN) != 0) {
        return -EFAULT;
    }
    return 0;
}

/**
 * The interface a route is for: the one the name at the caller's address
 * `dev` names, or, when `dev` is null, the one whose subnet has the gateway
 * `gw`, or the first. 0 with it in `*out`, or ENODEV when there is no such
 * interface, EOPNOTSUPP for `lo`, which no route names, and EFAULT when the
 * name cannot be read.
 */
static int32_t route_device(uintptr_t dev, uint32_t gw, struct netdev **out)
{
    uint8_t name[IFREQ_NAMESIZE];
    int i, got;

    *out = netdev_first();
    for (i = 0; dev == 0 && gw != 0 && i < netdev_seen_count(); i++) {
        if (netdev_gateway_reachable(netdev_at(i), gw)) {
            *out = netdev_at(i);
            break;
        }
    }
    if (dev != 0) {
        got = strncpy_from_user((char *)name, (const char __user *)dev, (uint32_t)IFREQ_NAMESIZE);
        if (got < 0) {
            return got == -EFAULT ? -EFAULT : -ENODEV;
        }
        *out = named(name);
    }
    return !*out ? -ENODEV : (*out)->loopback ? -EOPNOTSUPP : 0;
}

/**
 * Add or remove a route; root only.
 *
 * The route is for the interface the request's `rt_dev` names; when that
 * pointer is null, for the interface whose subnet has the gateway, or for the
 * first interface, the one whose address is the machine's. So a route for
 * one interface cannot change another's. The name is read as a string, since
 * a fixed sixteen bytes could run off the end of the caller's page. A
 * gateway off that interface's subnet is `ENETUNREACH`, as on Linux.
 *
 * Direct and gateway routes use the same table as rtnetlink. A nonzero
 * rt_metric is one more than the route priority.
 */
static int32_t route_ioctl(uint32_t request, uintptr_t arg)
{
    struct kernel_rtentry rt;
    uint32_t dst, gw, mask, metric, flags;
    uintptr_t dev;
    struct netdev *nd;
    int32_t rc;
    struct netdev_rt r = {0};

    if (!current_proc || !proc_privileged(current_proc)) {
        return -EPERM;
    }
    if (copy_from_user(&rt, (const void __user *)arg, sizeof(rt)) != 0) {
        return -EFAULT;
    }
    dst = be32(rt.dst + 4);
    gw = be32(rt.gateway + 4);
    mask = be32(rt.genmask + 4);
    metric = (uint16_t)rt.metric;
    metric = metric != 0 ? metric - 1u : 0u;
    dev = rt.dev;
    rc = route_device(dev, gw, &nd);
    if (rc != 0) {
        return rc;
    }
    r.dst = dst;
    r.mask = (rt.flags & 4) ? 0xffffffffu : mask;
    r.gw = gw;
    r.metric = metric;
    r.scope = request == SIOCDELRT ? 255 : gw ? 0 : 253;
    r.proto = request == SIOCDELRT ? 0 : 3;
    flags = net_lock();
    rc = netdev_route_change(request == SIOCDELRT && !dev ? 0 : nd, &r, request == SIOCDELRT, 0);
    net_unlock(flags);
    return rc;
}

/**
 * SIOCDARP: forget the mapping the caller's `struct arpreq` names, learned on
 * the interface its `arp_dev` names or on any when that is empty, for
 * `arp -d`; ENXIO when there is none, as on Linux. SIOCSARP: store the
 * mapping it names, permanent with ATF_PERM, which `arp -s` sets, on the
 * interface it names or the one whose subnet has the address, ENETUNREACH
 * when none has; a hardware address of another kind than Ethernet is
 * EINVAL, and a published one, for proxy ARP, which this stack does not
 * answer, EOPNOTSUPP, as is a mapping on `lo`, which resolves nothing. Root
 * only.
 */
static int32_t arp_ioctl(uint32_t request, uintptr_t arg)
{
    uint8_t req[ARPREQ_LEN];
    struct netdev *nd = 0;

    if (!current_proc || !proc_privileged(current_proc)) {
        return -EPERM;
    }
    if (copy_from_user(req, (const void __user *)arg, ARPREQ_LEN) != 0) {
        return -EFAULT;
    }
    if (req[ARPREQ_DEV] != 0) {
        nd = named(req + ARPREQ_DEV);
        if (!nd) {
            return -ENODEV;
        }
    }
    if (request == SIOCSARP) {
        uint32_t ip = be32(req + ARPREQ_PA_ADDR);
        uint32_t flags = (uint32_t)req[ARPREQ_FLAGS] | ((uint32_t)req[ARPREQ_FLAGS + 1] << 8);
        int i;

        if (flags & ATF_PUBL) {
            return -EOPNOTSUPP;
        }
        if (req[ARPREQ_HA_FAM] != ARPHRD_ETHER || req[ARPREQ_HA_FAM + 1] != 0) {
            return -EINVAL;
        }
        for (i = 0; !nd && i < netdev_seen_count(); i++) {
            if (netdev_on_link(netdev_at(i), ip)) {
                nd = netdev_at(i);
            }
        }
        if (nd && nd->loopback) {
            return -EOPNOTSUPP;
        }
        return nd ? arp_set(nd, ip, req + ARPREQ_HA_ADDR, (flags & ATF_PERM) != 0) : -ENETUNREACH;
    }
    return arp_forget(nd, be32(req + ARPREQ_PA_ADDR)) == 0 ? 0 : -ENXIO;
}

/**
 * SIOCGIFCONF: the interfaces that have an IPv4 address, each an ifreq with
 * its name and its address, in the buffer of the caller's `struct ifconf`,
 * whose length is set to the bytes filled; with no buffer, the bytes all of
 * them would take, which is how a caller sizes one. An interface the buffer
 * has no room for whole is left out, as on Linux.
 */
static int32_t ifconf_ioctl(uintptr_t arg)
{
    struct {
        int len;
        uintptr_t buf;
    } ifc; /* Linux's struct ifconf, in the processor's layout */
    uint8_t ifr[IFREQ_LEN];
    uint32_t len, used = 0;
    uintptr_t buf;
    int i;

    if (copy_from_user(&ifc, (const void __user *)arg, sizeof(ifc)) != 0) {
        return -EFAULT;
    }
    len = (uint32_t)ifc.len;
    buf = ifc.buf;
    for (i = 0; i < netdev_seen_count(); i++) {
        struct netdev *nd = netdev_at(i);

        if (!nd || nd->ip == 0) {
            continue;
        }
        if (buf != 0) {
            if ((int32_t)len < 0 || used + IFREQ_LEN > len) {
                break;
            }
            k_memset(ifr, 0, sizeof(ifr));
            k_memcpy(ifr + IFREQ_NAME, nd->name, k_strlen(nd->name));
            set_sa(ifr, nd->ip);
            if (copy_to_user((void __user *)(buf + used), ifr, IFREQ_LEN) != 0) {
                return -EFAULT;
            }
        }
        used += IFREQ_LEN;
    }
    ifc.len = (int)used;
    return copy_to_user((void __user *)arg, &ifc.len, sizeof(ifc.len)) == 0 ? 0 : -EFAULT;
}

/* Linux's struct in6_ifreq: the address, the prefix's length and the
 * interface's index; and its struct in6_rtmsg, whose `info` is a long. */
struct in6_ifreq_shape {
    uint8_t addr[16];
    uint32_t prefixlen;
    int ifindex;
};

struct in6_rtmsg_shape {
    uint8_t dst[16];
    uint8_t src[16];
    uint8_t gateway[16];
    uint32_t type;
    uint16_t dst_len;
    uint16_t src_len;
    uint32_t metric;
    unsigned long info;
    uint32_t flags;
    int ifindex;
};

/** The interface of index `index` the caller sees, or NULL. */
static struct netdev *of_index(int index)
{
    int i;

    for (i = 0; i < netdev_seen_count(); i++) {
        if (netdev_at(i) && netdev_at(i)->index == index) {
            return netdev_at(i);
        }
    }
    return 0;
}

/**
 * SIOCSIFADDR and SIOCDIFADDR on an AF_INET6 socket: give the interface of
 * the caller's `in6_ifreq` its address and prefix, permanent, as Linux's
 * `addrconf_add_ifaddr` does, or take it away; root only. Returns the
 * result of `netconf_add_address6` or `netconf_del_address6`, -ENODEV for
 * an index no interface has, or -EINVAL for a prefix past 128.
 */
static int32_t in6_addr_ioctl(uint32_t request, uintptr_t arg)
{
    struct in6_ifreq_shape r;
    struct netdev *nd;

    if (!current_proc || !proc_privileged(current_proc)) {
        return -EPERM;
    }
    if (copy_from_user(&r, (const void __user *)arg, sizeof(r)) != 0) {
        return -EFAULT;
    }
    nd = of_index(r.ifindex);
    if (!nd) {
        return -ENODEV;
    }
    if (r.prefixlen > 128) {
        return -EINVAL;
    }
    if (request == SIOCDIFADDR) {
        return netconf_del_address6(nd, r.addr);
    }
    return netconf_add_address6(nd, r.addr, (int)r.prefixlen, NETDEV_A6_PERMANENT,
                                NETDEV_LIFETIME_FOREVER, NETDEV_LIFETIME_FOREVER);
}

/**
 * SIOCADDRT and SIOCDELRT on an AF_INET6 socket: add the route of the
 * caller's `in6_rtmsg` to the IPv6 table (`route6_add`), through its
 * gateway when RTF_GATEWAY is set, by the interface of its index, or with
 * none by the one its gateway is reached by, at its metric, 1024 for 0 as
 * on Linux; or remove the table's route of that prefix by that interface,
 * or any, through that gateway, or any (`route6_del`). Root only. -ENODEV
 * for an index no interface has or a route no interface carries,
 * -EOPNOTSUPP on `lo`, and -EINVAL for a prefix past 128.
 */
static int32_t in6_route_ioctl(uint32_t request, uintptr_t arg)
{
    struct in6_rtmsg_shape r;
    uint8_t via[IP6_ADDR_LEN];
    const uint8_t *gw;
    struct netdev *nd = 0;
    uint32_t lock;

    if (!current_proc || !proc_privileged(current_proc)) {
        return -EPERM;
    }
    if (copy_from_user(&r, (const void __user *)arg, sizeof(r)) != 0) {
        return -EFAULT;
    }
    if (r.dst_len > 128) {
        return -EINVAL;
    }
    gw = (r.flags & RTF_GATEWAY) ? r.gateway : 0;
    if (r.ifindex != 0) {
        nd = of_index(r.ifindex);
        if (!nd) {
            return -ENODEV;
        }
    } else if (gw && request == SIOCADDRT) {
        lock = net_lock();
        nd = ip6_route(gw, 0, via);
        net_unlock(lock);
    }
    if (nd && nd->loopback) {
        return -EOPNOTSUPP;
    }
    if (request == SIOCDELRT) {
        return route6_del(nd, r.dst, r.dst_len, gw);
    }
    if (!nd) {
        return -ENODEV;
    }
    return route6_add(nd, r.dst, r.dst_len, gw, r.metric != 0 ? r.metric : 1024, ROUTE6_PROTO_BOOT);
}

int32_t netif_ioctl(uint32_t request, uintptr_t arg, int family, int *handled)
{
    *handled = 1;
    if (family == AF_INET6 && (request == SIOCSIFADDR || request == SIOCDIFADDR)) {
        return in6_addr_ioctl(request, arg);
    }
    if (family == AF_INET6 && (request == SIOCADDRT || request == SIOCDELRT)) {
        return in6_route_ioctl(request, arg);
    }
    switch (request) {
    case 0x8b01: /* SIOCGIWNAME */
        return ifreq_ioctl(request, arg);
    case SIOCGIFCONF:
        return ifconf_ioctl(arg);
    case SIOCGIFNAME:
    case SIOCGIFFLAGS:
    case SIOCSIFFLAGS:
    case SIOCGIFADDR:
    case SIOCSIFADDR:
    case SIOCGIFDSTADDR:
    case SIOCSIFDSTADDR:
    case SIOCGIFBRDADDR:
    case SIOCSIFBRDADDR:
    case SIOCGIFNETMASK:
    case SIOCSIFNETMASK:
    case SIOCGIFMTU:
    case SIOCSIFMTU:
    case SIOCGIFHWADDR:
    case SIOCGIFINDEX:
        return ifreq_ioctl(request, arg);
    case SIOCADDRT:
    case SIOCDELRT:
        return route_ioctl(request, arg);
    case SIOCDARP:
    case SIOCSARP:
        return arp_ioctl(request, arg);
    case SIOCGIFBR:
    case SIOCSIFBR:
    case SIOCGIFVLAN:
    case SIOCSIFVLAN:
    case SIOCBRADDBR:
    case SIOCBRDELBR:
        return -ENOPKG;
    default:
        *handled = 0;
        return -ENOTTY;
    }
}
