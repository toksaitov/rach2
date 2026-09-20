/** Interface fields for native network utility models, not a host ABI. */
#ifndef NETUTIL_IF_H
#define NETUTIL_IF_H

#include <sys/socket.h>
#include <sys/ioctl.h>

#define IFNAMSIZ 16
struct ifreq {
    char ifr_name[IFNAMSIZ];
    struct sockaddr ifr_addr;
    struct sockaddr ifr_hwaddr;
    int ifr_ifindex;
};

#ifndef SIOCGIFINDEX
#define SIOCGIFINDEX 0x8933
#endif
#ifndef SIOCGIFHWADDR
#define SIOCGIFHWADDR 0x8927
#endif

#endif
