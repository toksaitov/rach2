/*
 * netif.c: the interface ioctls, and what changing an address costs.
 *
 * The contract is in netif.h. What is here is the `struct ifreq` layout as
 * offsets, because a struct would have to match an ABI this kernel does not
 * otherwise depend on, and the rule that an address change is not just a
 * number: everything that recorded the old one has to be told.
 */

#include "netif.h"
#include "net.h"
#include "netconf.h"
#include "driver/netdev.h"
#include "../lib/kerrno.h"
#include "../lib/kstring.h"
#include "../mm/usercopy.h"
#include "../proc/process.h"

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
#define RTENTRY_DEV     68
#define RTENTRY_LEN     84

#define RTF_UP      0x0001
#define RTF_GATEWAY 0x0002

#define ARPHRD_ETHER 1

/* The interface is up until somebody says otherwise. A machine whose network
 * came up at boot and was never configured is up, which is what every existing
 * test expects. */

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
    if (i == IFREQ_NAMESIZE - 1) {
        return 0; /* no terminator inside the field */
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
 * The name is checked first for every one of them, because a program that asks
 * about an interface this machine does not have must be told so rather than
 * told about the one it does have.
 */
static int32_t ifreq_ioctl(uint32_t request, uint32_t arg)
{
    uint8_t ifr[IFREQ_LEN];
    struct netdev *nd;
    int setter;

    if (copy_from_user(ifr, (const void __user *)arg, IFREQ_LEN) != 0) {
        return -EFAULT;
    }

    /* SIOCGIFNAME is the one that runs the other way: it takes an index in the
       union and answers with a name, so there is no name to check yet. */
    if (request == SIOCGIFNAME) {
        int32_t index =
            (int32_t)((uint32_t)ifr[IFREQ_UNION] | ((uint32_t)ifr[IFREQ_UNION + 1] << 8) |
                      ((uint32_t)ifr[IFREQ_UNION + 2] << 16) |
                      ((uint32_t)ifr[IFREQ_UNION + 3] << 24));

        struct netdev *at;

        if (index < 1 || index > netdev_count()) {
            return -ENODEV; /* judged before it is made a place in the table */
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
    if (setter && (!current_proc || current_proc->euid != 0)) {
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
        /* RUNNING is the link, not the card. `ifconfig` prints it, and a
         * bound card with nothing plugged into it reported the same as a
         * working one until the driver interface could be asked. The rule for
         * a card that cannot report a link belongs to that interface, so it
         * is asked rather than repeated here. */
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

        /* Returns here instead of falling through to the writeback: this
           request only reads its argument, and a copy back that failed would
           report a refusal for a change already made. */
        nd->up = (flags & IFF_UP) ? 1 : 0;
        return 0;
    }
    case SIOCSIFADDR:
        /* No mask, always: this ioctl sets an address, and the mask that goes
           with it is either the classful one until `SIOCSIFNETMASK` arrives or
           the wrong one carried over from the address before it. `ifconfig`
           issues the two in that order for exactly this reason. */
        return netconf_set_address(nd, be32(ifr + IFREQ_SA_ADDR), 0);
    case SIOCSIFNETMASK:
        return netconf_set_netmask(nd, be32(ifr + IFREQ_SA_ADDR));
    case SIOCSIFBRDADDR:
        /* The broadcast address of a subnet is the subnet's, not a choice, so
         * this accepts the one this interface already derives and refuses any
         * other instead of storing a second answer to the same question. */
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
 * Add or remove a route.
 *
 * This stack has one route it can hold, the default gateway, and it reaches
 * everything else by being on its subnet. So a default route sets or clears
 * the gateway; a route to the subnet this machine is already on is accepted
 * and does nothing, because it is already true; anything else is refused,
 * not accepted and forgotten.
 */
static int32_t route_ioctl(uint32_t request, uint32_t arg)
{
    uint8_t rt[RTENTRY_LEN];
    uint8_t name[IFREQ_NAMESIZE];
    uint32_t dst, gw, mask, dev;
    struct netdev *nd;

    if (!current_proc || current_proc->euid != 0) {
        return -EPERM;
    }
    if (copy_from_user(rt, (const void __user *)arg, RTENTRY_LEN) != 0) {
        return -EFAULT;
    }
    dst = be32(rt + RTENTRY_DST + 4);
    gw = be32(rt + RTENTRY_GATEWAY + 4);
    mask = be32(rt + RTENTRY_GENMASK + 4);

    /* The interface the route is for. A null pointer means "wherever it goes",
       which is the first interface, the one whose address is the machine's; a
       name is checked and the route is that interface's, so that a route for
       one interface cannot change the route of another. */
    dev = (uint32_t)rt[RTENTRY_DEV] | ((uint32_t)rt[RTENTRY_DEV + 1] << 8) |
          ((uint32_t)rt[RTENTRY_DEV + 2] << 16) | ((uint32_t)rt[RTENTRY_DEV + 3] << 24);
    nd = netdev_first();
    if (dev != 0) {
        /* A name is a string, and it is read as one: a fixed sixteen bytes from
           a caller whose name is five bytes long can run off the end of the
           page it sits in, and refusing that call would be refusing a correct
           program. */
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

    if (dst == 0 && mask == 0) {
        if (request == SIOCADDRT) {
            /* A default route with no gateway is one this stack cannot hold:
               everything off the subnet goes through a router, and there is
               nowhere else to send it. Refused without touching anything, so
               that `route add default dev eth0` cannot quietly remove the
               gateway a machine across the room is reached through. */
            if (gw == 0) {
                return -EINVAL;
            }
            return netconf_set_gateway(nd, gw);
        }
        if (request == SIOCDELRT) {
            /* Removing a route that is not there is an error, and saying so
               matters: a DHCP client's script removes old default routes in a
               loop that ends when the removal fails, and a removal that always
               succeeds is a machine that never gets past it. */
            if (nd->gateway == 0) {
                return -ESRCH;
            }
            if (gw != 0 && gw != nd->gateway) {
                return -ESRCH;
            }
            return netconf_set_gateway(nd, 0);
        }
        return -EINVAL;
    }
    if (request == SIOCADDRT && nd->mask != 0 && dst == (nd->ip & nd->mask) && mask == nd->mask &&
        gw == 0) {
        /* The subnet is reachable already, by being on it, so adding that route
           asks for what is already true. Removing it is another matter: it
           cannot be removed while the address that implies it is in force, and
           saying otherwise would be telling an administrator that traffic was
           isolated when it was not. */
        return 0;
    }
    return -EOPNOTSUPP;
}

int32_t netif_ioctl(uint32_t request, uint32_t arg, int *handled)
{
    *handled = 1;
    switch (request) {
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
    default:
        *handled = 0;
        return -ENOTTY;
    }
}
