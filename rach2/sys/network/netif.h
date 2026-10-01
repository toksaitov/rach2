#ifndef _NETIF_H_
#define _NETIF_H_

#include "../lib/kint.h"

/*
 * netif: the interface a program configures the network through.
 *
 * Until this existed the machine's address came from the boot line and nothing
 * could change it, which is a machine that must be told where it is before it
 * is switched on. A DHCP client is a program that learns the address after the
 * machine is running, so there has to be a way for a program to set one. This
 * is that way, and it is Linux's: the `SIOCxIF*` ioctls on a socket, with a
 * `struct ifreq` naming the interface.
 *
 * The interface is `eth0` and there is one of it. A request naming anything
 * else is `ENODEV`, not quietly applied to the only one there is.
 *
 * Reading is anyone's; changing is root's. A program that could set this
 * machine's address could take another machine's, and could send from it.
 */

/* The requests, with Linux's numbers. `ifreq` is 16 bytes of name followed by
 * a union: a sockaddr for the addresses, a short for the flags, an int for the
 * index, 32 bytes in all. */
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

/* Interface flags, of which this machine has four to report. */
#define IFF_UP        0x0001
#define IFF_BROADCAST 0x0002
#define IFF_RUNNING   0x0040
#define IFF_MULTICAST 0x1000

/**
 * Handle one interface ioctl on a socket descriptor.
 *
 * `handled` is set to 0 for a request this does not own, so the caller can go
 * on looking; the return value is meaningless in that case.
 *
 * @param request the ioctl number.
 * @param arg     the user pointer to a `struct ifreq` or `struct rtentry`.
 * @return 0 or a negative errno.
 */
int32_t netif_ioctl(uint32_t request, uint32_t arg, int *handled);

/**
 * Whether `name` is this machine's interface.
 *
 * `SO_BINDTODEVICE` names an interface the way the ioctls do, so it is judged
 * the same way and by the same code.
 */
int netif_name_is_ours(const char *name);

/** Whether the interface is administratively up; frames are dropped when
 * not. */
int netif_is_up(void);

#endif /* _NETIF_H_ */
