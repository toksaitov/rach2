#ifndef _NETIF_H_
#define _NETIF_H_

#include "../lib/kint.h"

/*
 * netif: the interface ioctls, Linux's `SIOC*` requests on a socket, through
 * which `ifconfig`, `route`, `arp` and a DHCP client read and set each
 * interface's address, mask, flags and routes and the ARP cache's mappings.
 * A request names its interface in a `struct ifreq`, `struct rtentry` or
 * `struct arpreq`, and one that names no interface the caller sees, a card
 * or `lo`, is ENODEV.
 *
 * Reading is anyone's; changing is root's, EPERM otherwise, since a program
 * that could set this machine's address could take another machine's and
 * send from it.
 */

/* The requests, with Linux's numbers. `ifreq` is 16 bytes of name followed by
 * a union: a sockaddr for the addresses, a short for the flags, an int for the
 * index, 32 bytes in all on i686 and 40 on x86-64. */
#define SIOCGIFNAME    0x8910
#define SIOCGIFCONF    0x8912
#define SIOCGIFFLAGS   0x8913
#define SIOCSIFFLAGS   0x8914
#define SIOCGIFADDR    0x8915
#define SIOCSIFADDR    0x8916
#define SIOCGIFBRDADDR 0x8919
#define SIOCSIFBRDADDR 0x891a
#define SIOCGIFNETMASK 0x891b
#define SIOCSIFNETMASK 0x891c
#define SIOCGIFMTU     0x8921
#define SIOCGIFHWADDR  0x8927
#define SIOCGIFINDEX   0x8933
#define SIOCADDRT      0x890b
#define SIOCDELRT      0x890c
#define SIOCDARP       0x8953
#define SIOCSARP       0x8955

/* The bridge and VLAN requests of brctl(8) and vconfig(8), which Linux
 * answers with ENOPKG when neither module is in the kernel. */
#define SIOCGIFBR   0x8940
#define SIOCSIFBR   0x8941
#define SIOCGIFVLAN 0x8982
#define SIOCSIFVLAN 0x8983
#define SIOCBRADDBR 0x89a0
#define SIOCBRDELBR 0x89a1

/* The interface flags this kernel reports, with Linux's values. */
#define IFF_UP        0x0001
#define IFF_BROADCAST 0x0002
#define IFF_LOOPBACK  0x0008
#define IFF_RUNNING   0x0040
#define IFF_MULTICAST 0x1000

/**
 * Answer one interface ioctl, which `sys_ioctl` passes for a socket of any
 * family: the requests above, with the bridge and VLAN ones ENOPKG.
 *
 * `handled` is set to 0 for a request this does not own, so the caller can go
 * on looking; the return value is then -ENOTTY and means nothing. System
 * call context.
 *
 * @param request the ioctl number.
 * @param arg     the user pointer to a `struct ifreq`, `struct ifconf`,
 *                `struct rtentry` or `struct arpreq`.
 * @return 0 or a negative errno.
 */
int32_t netif_ioctl(uint32_t request, uintptr_t arg, int *handled);

/** Whether `name` is the name of an interface, a card or `lo`
 * (`netdev_by_name`), whatever the caller's network namespace. */
int netif_name_is_ours(const char *name);

#endif /* _NETIF_H_ */
