/** Route fields used by the native network utility models, not a host ABI. */
#ifndef NETUTIL_ROUTE_H
#define NETUTIL_ROUTE_H

#include <sys/socket.h>
#include <sys/ioctl.h>

struct rtentry {
    struct sockaddr rt_dst;
    struct sockaddr rt_genmask;
    struct sockaddr rt_gateway;
    unsigned short rt_flags;
    char *rt_dev;
};

#define RTF_UP      1
#define RTF_GATEWAY 2
#ifndef SIOCADDRT
#define SIOCADDRT 0x890b
#endif
#ifndef SIOCDELRT
#define SIOCDELRT 0x890c
#endif

#endif
