/*
 * netif.c: the interface and route ioctls.
 *
 * The contract is in netif.h. What is here is the `struct ifreq` and
 * `struct rtentry` layouts as offsets, because a struct would have to match
 * an ABI this kernel does not otherwise depend on. What an address change
 * costs, everything that recorded the old address being told, is in
 * netconf.c.
 */

#include "netif.h"
#include "net.h"
#include "netconf.h"
#include "driver/netdev.h"
#include "arp/arp.h"
#include "../lib/kerrno.h"
#include "../lib/kstring.h"
#include "../mm/usercopy.h"
#include "../proc/process.h"
#include "../proc/ns.h"

/* struct ifreq: name at 0 (16 bytes), then the union. Inside the union a
 * sockaddr puts its family at 0 and an IPv4 address at 4; the flags are a
 * short at the union's start and the index an int there. */
#define IFREQ_LEN      32
#define IFREQ_NAME     0
#define IFREQ_NAMESIZE 16
#define IFREQ_UNION    16
#define IFREQ_SA_ADDR  (IFREQ_UNION + 4)
#define IFREQ_HW_ADDR  (IFREQ_UNION + 2)

/* struct rtentry on i386, with the offset of each field this code reads: a pad
 * word, then three sockaddrs (destination, gateway, mask) of 16 bytes each at
 * 4, 20 and 36, the flags at 52, and a pointer to the interface's name at 68,
 * for 84 bytes in all. The name matters: a route names a device, and a request
 * naming one this machine does not have must not change the one it does. */
#define RTENTRY_DST     4
#define RTENTRY_GATEWAY 20
#define RTENTRY_GENMASK 36
#define RTENTRY_FLAGS   52
#define RTENTRY_METRIC  64
#define RTENTRY_DEV     68
#define RTENTRY_LEN     84

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

/**
 * Whether the first interface is up. An interface is up from the probe until
 * an administrator takes it down, which is what every existing test expects of
 * a machine whose network came up at boot and was never configured.
 */

int netif_is_up(void)
{
    struct netdev *nd = netdev_first();

    return nd ? nd->up : 0;
}

/** The 32-bit big-endian address at `p`, as a host-order number. */
static uint32_t be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

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
    return net_ns_seen() ? netdev_count() : 0;
}

/**
 * The interface a caller named, or NULL when this machine has no such one.
 *
 * The name comes out of a caller's `ifreq`, so it is read only as far as the
 * field goes and only up to its terminator; a name that fills the field
 * without one names nothing.
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
    if (i == IFREQ_NAMESIZE - 1 || !net_ns_seen()) {
        return 0; /* no terminator inside the field, or no interface seen */
    }
    want[i] = '\0';
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

/**
 * Handle the requests that read or write an interface.
 *
 * The name is checked first for every one of them but SIOCGIFNAME, which
 * takes an index in the union and answers with a name, because a program that
 * asks about an interface this machine does not have must be told so rather
 * than told about the one it does have. The setters are root's.
 *
 * The setters return without copying the request back, since they only read
 * it and a failed copy would report a refusal for a change already made.
 * SIOCSIFFLAGS sets only whether the interface is up. SIOCSIFADDR always
 * passes no mask: the mask that goes with the address is the classful one
 * until SIOCSIFNETMASK arrives, never the one left from the address before,
 * and `ifconfig` issues the two in that order. A subnet's broadcast address
 * follows from the subnet, so SIOCSIFBRDADDR accepts the one the interface
 * already derives and refuses any other.
 */
static int32_t ifreq_ioctl(uint32_t request, uint32_t arg)
{
    uint8_t ifr[IFREQ_LEN];
    struct netdev *nd;
    int setter;

    if (copy_from_user(ifr, (const void __user *)arg, IFREQ_LEN) != 0) {
        return -EFAULT;
    }

    if (request == SIOCGIFNAME) {
        int32_t index =
            (int32_t)((uint32_t)ifr[IFREQ_UNION] | ((uint32_t)ifr[IFREQ_UNION + 1] << 8) |
                      ((uint32_t)ifr[IFREQ_UNION + 2] << 16) |
                      ((uint32_t)ifr[IFREQ_UNION + 3] << 24));

        struct netdev *at;

        if (index < 1 || index > netdev_seen_count()) {
            return -ENODEV; /* no interface has that index */
        }
        at = netdev_at(index - 1);
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

    nd = named(ifr + IFREQ_NAME);
    if (!nd) {
        return -ENODEV;
    }

    setter = (request == SIOCSIFADDR || request == SIOCSIFNETMASK || request == SIOCSIFBRDADDR ||
              request == SIOCSIFFLAGS);
    if (setter && (!current_proc || !proc_privileged(current_proc))) {
        return -EPERM;
    }

    switch (request) {
    case SIOCGIFINDEX:
        k_memset(ifr + IFREQ_UNION, 0, IFREQ_LEN - IFREQ_UNION);
        ifr[IFREQ_UNION] = (uint8_t)nd->index;
        break;
    case SIOCGIFHWADDR:
        k_memset(ifr + IFREQ_UNION, 0, IFREQ_LEN - IFREQ_UNION);
        ifr[IFREQ_UNION] = (uint8_t)ARPHRD_ETHER;
        k_memcpy(ifr + IFREQ_HW_ADDR, nd->mac, 6);
        break;
    case SIOCGIFADDR:
        set_sa(ifr, nd->ip);
        break;
    case SIOCGIFNETMASK:
        set_sa(ifr, nd->mask);
        break;
    case SIOCGIFBRDADDR:
        set_sa(ifr, nd->ip != 0 && nd->mask != 0 ? (nd->ip | ~nd->mask) : 0);
        break;
    case SIOCGIFMTU:
        k_memset(ifr + IFREQ_UNION, 0, IFREQ_LEN - IFREQ_UNION);
        ifr[IFREQ_UNION] = (uint8_t)(1500 & 0xff);
        ifr[IFREQ_UNION + 1] = (uint8_t)((1500 >> 8) & 0xff);
        break;
    case SIOCGIFFLAGS: {
        uint16_t flags = (uint16_t)(IFF_BROADCAST | IFF_MULTICAST);

        if (nd->up) {
            flags |= (uint16_t)IFF_UP;
        }
        /* IFF_RUNNING reports the link, as netdev_running judges it. */
        if (netdev_running(nd)) {
            flags |= (uint16_t)IFF_RUNNING;
        }
        k_memset(ifr + IFREQ_UNION, 0, IFREQ_LEN - IFREQ_UNION);
        ifr[IFREQ_UNION] = (uint8_t)(flags & 0xff);
        ifr[IFREQ_UNION + 1] = (uint8_t)((flags >> 8) & 0xff);
        break;
    }
    case SIOCSIFFLAGS: {
        uint16_t flags =
            (uint16_t)((uint16_t)ifr[IFREQ_UNION] | ((uint16_t)ifr[IFREQ_UNION + 1] << 8));

        nd->up = (flags & IFF_UP) ? 1 : 0;
        return 0;
    }
    case SIOCSIFADDR:
        return netconf_set_address(nd, be32(ifr + IFREQ_SA_ADDR), 0);
    case SIOCSIFNETMASK:
        return netconf_set_netmask(nd, be32(ifr + IFREQ_SA_ADDR));
    case SIOCSIFBRDADDR:
        if (be32(ifr + IFREQ_SA_ADDR) !=
            (nd->ip != 0 && nd->mask != 0 ? (nd->ip | ~nd->mask) : 0xFFFFFFFFu)) {
            return -EINVAL;
        }
        return 0;
    default:
        return -ENOTTY;
    }

    if (copy_to_user((void __user *)arg, ifr, IFREQ_LEN) != 0) {
        return -EFAULT;
    }
    return 0;
}

/**
 * Add or remove a route; root only.
 *
 * Each interface has one route it can keep, its default gateway, and reaches
 * everything else by being on its subnet. The route is for the interface the
 * request's `rt_dev` names, or for the first interface, the one whose
 * address is the machine's, when that pointer is null; so a route for one
 * interface cannot change another's. The name is read as a string, since a
 * fixed sixteen bytes could run off the end of the caller's page.
 *
 * A default route of no metric sets the gateway, and one with no gateway is
 * refused without touching anything, so `route add default dev eth0` cannot
 * quietly remove the gateway. Removing a default route clears the gateway,
 * and removing one that is not there is `ESRCH`: a DHCP client's script
 * removes default routes in a loop that ends when a removal fails. A route to
 * the interface's own subnet is accepted and does nothing, because it is
 * already true; it cannot be removed while the address that implies it is in
 * force. Any other route goes in the table (`netdev_route_add`), with the
 * metric the caller gives, which `route` sends one more than, as Linux takes
 * it.
 */
static int32_t route_ioctl(uint32_t request, uint32_t arg)
{
    uint8_t rt[RTENTRY_LEN];
    uint8_t name[IFREQ_NAMESIZE];
    uint32_t dst, gw, mask, dev, metric, flags;
    struct netdev *nd;
    int32_t rc;

    if (!current_proc || !proc_privileged(current_proc)) {
        return -EPERM;
    }
    if (copy_from_user(rt, (const void __user *)arg, RTENTRY_LEN) != 0) {
        return -EFAULT;
    }
    dst = be32(rt + RTENTRY_DST + 4);
    gw = be32(rt + RTENTRY_GATEWAY + 4);
    mask = be32(rt + RTENTRY_GENMASK + 4);
    metric = (uint32_t)rt[RTENTRY_METRIC] | ((uint32_t)rt[RTENTRY_METRIC + 1] << 8);
    metric = metric != 0 ? metric - 1u : 0u;

    dev = (uint32_t)rt[RTENTRY_DEV] | ((uint32_t)rt[RTENTRY_DEV + 1] << 8) |
          ((uint32_t)rt[RTENTRY_DEV + 2] << 16) | ((uint32_t)rt[RTENTRY_DEV + 3] << 24);
    nd = netdev_first();
    if (dev == 0 && gw != 0) {
        int i;

        for (i = 0; i < netdev_seen_count(); i++) {
            if (netdev_on_link(netdev_at(i), gw)) {
                nd = netdev_at(i);
                break;
            }
        }
    }
    if (dev != 0) {
        int got =
            strncpy_from_user((char *)name, (const char __user *)dev, (uint32_t)IFREQ_NAMESIZE);

        if (got < 0) {
            return got == -EFAULT ? -EFAULT : -ENODEV;
        }
        nd = named(name);
    }
    if (!nd) {
        return -ENODEV;
    }

    if (dst == 0 && mask == 0 && metric == 0 &&
        (request == SIOCADDRT ? nd->gateway == 0 || nd->gateway == gw : nd->gateway != 0)) {
        if (request == SIOCADDRT) {
            return gw == 0 ? -EINVAL : netconf_set_gateway(nd, gw);
        }
        if (gw != 0 && gw != nd->gateway) {
            return -ESRCH;
        }
        return netconf_set_gateway(nd, 0);
    }
    if (request == SIOCADDRT && nd->mask != 0 && dst == (nd->ip & nd->mask) && mask == nd->mask &&
        gw == 0) {
        return 0;
    }
    flags = net_lock();
    rc = request == SIOCADDRT ? netdev_route_add(nd, dst, mask, gw, metric, 0)
                              : netdev_route_del(dev != 0 ? nd : 0, dst, mask, gw);
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
 * answer, EOPNOTSUPP. Root only.
 */
static int32_t arp_ioctl(uint32_t request, uint32_t arg)
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
static int32_t ifconf_ioctl(uint32_t arg)
{
    uint8_t ifc[8];
    uint8_t ifr[IFREQ_LEN];
    uint32_t len, buf, used = 0;
    int i;

    if (copy_from_user(ifc, (const void __user *)arg, sizeof(ifc)) != 0) {
        return -EFAULT;
    }
    len = (uint32_t)ifc[0] | ((uint32_t)ifc[1] << 8) | ((uint32_t)ifc[2] << 16) |
          ((uint32_t)ifc[3] << 24);
    buf = (uint32_t)ifc[4] | ((uint32_t)ifc[5] << 8) | ((uint32_t)ifc[6] << 16) |
          ((uint32_t)ifc[7] << 24);
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
    ifc[0] = (uint8_t)used;
    ifc[1] = (uint8_t)(used >> 8);
    ifc[2] = (uint8_t)(used >> 16);
    ifc[3] = (uint8_t)(used >> 24);
    return copy_to_user((void __user *)arg, ifc, 4) == 0 ? 0 : -EFAULT;
}

int32_t netif_ioctl(uint32_t request, uint32_t arg, int *handled)
{
    *handled = 1;
    switch (request) {
    case SIOCGIFCONF:
        return ifconf_ioctl(arg);
    case SIOCGIFNAME:
    case SIOCGIFFLAGS:
    case SIOCSIFFLAGS:
    case SIOCGIFADDR:
    case SIOCSIFADDR:
    case SIOCGIFBRDADDR:
    case SIOCSIFBRDADDR:
    case SIOCGIFNETMASK:
    case SIOCSIFNETMASK:
    case SIOCGIFMTU:
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
