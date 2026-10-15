/**
 * netifprobe: the interface ioctls, by which a program reads and changes the
 * machine's network configuration.
 *
 * A DHCP client learns the address after the machine is running and sets it
 * as on Linux, with `SIOCxIF*` on a socket and a `struct ifreq`. A change of
 * address also costs the ARP mappings, learned at the old address, and the
 * connections, which recorded it, so the probe holds a TCP connection open
 * across the change and requires it to be aborted with ENETRESET rather than
 * left sending from an address the machine no longer has. The address is put
 * back at the end and the gateway pinged, which shows that the machine works
 * after the change.
 *
 * On a machine booted with `rach2.ip=none` the probe configures the machine
 * instead, in the order `ifconfig` uses and a DHCP client runs: an address,
 * then a mask, then a route.
 *
 * `netifprobe second`, on a machine with two cards, takes the second card
 * down and requires the first to keep serving and the netlink link dump to
 * report each card's own state, and checks that /proc/net/dev,
 * SO_BINDTODEVICE, a TCP connection's addresses and the packet sockets each
 * name the right interface.
 *
 * Prints "NETIFPROBE DOWN" and "NETIFPROBE UP", at each of which the suite
 * sends a datagram from outside, and "NETIFPROBE OK" with exit 0 on success;
 * a failure prints "NETIFPROBE FAIL: " with the reason and exits 1.
 * Freestanding; direct system calls (probe.h).
 */

#include "probe.h"

typedef unsigned int uint32_t;
typedef unsigned short uint16_t;
typedef unsigned char uint8_t;

#define STDOUT_FILENO 1

#define AF_INET     2
#define AF_NETLINK  16
#define AF_PACKET   17
#define SOCK_STREAM 1
#define SOCK_DGRAM  2
#define SOCK_RAW    3
#define IPPROTO_RAW 255

/* The protocol argument of an AF_PACKET socket is in network order. */
#define ETH_P_ARP_BE 0x0608

/* The second card's subnet, which the two-card machine is booted on. */
#define SECOND_IP 0x0A000305UL /* 10.0.3.5 */
#define SECOND_GW 0x0A000302UL /* 10.0.3.2 */

#define SOL_SOCKET      1
#define SO_ERROR        4
#define SO_BINDTODEVICE 25

#define SIOCGIFNAME    0x8910
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

#define IFF_UP        0x0001
#define IFF_BROADCAST 0x0002
#define IFF_RUNNING   0x0040
#define IFF_MULTICAST 0x1000

/* Linux's netlink ABI, <linux/netlink.h> and <linux/rtnetlink.h>: a link
 * dump's request, and the types of the messages that answer it. */
#define NETLINK_ROUTE 0
#define NLM_F_REQUEST 0x001
#define NLM_F_DUMP    0x300
#define NLMSG_ERROR   2
#define NLMSG_DONE    3
#define RTM_NEWLINK   16
#define RTM_GETLINK   18

#define RTF_UP      0x0001
#define RTF_GATEWAY 0x0002

#define EPERM           1
#define EINVAL          22
#define ENODEV          19
#define ENETRESET       102
#define ESRCH           3
#define EPROTONOSUPPORT 93
#define EOPNOTSUPP      95
#define ENETUNREACH     101

#define POLLIN  0x001
#define POLLOUT 0x004
#define POLLERR 0x008

#define SLIRP_IP   0x0A00020FUL /* 10.0.2.15, what this machine boots with */
#define SLIRP_GW   0x0A000202UL /* 10.0.2.2 */
#define SLIRP_MASK 0xFFFFFF00UL
#define SLIRP_BRD  0x0A0002FFUL
#define OTHER_IP   0xC0A85A14UL /* 192.168.90.20 */
#define OTHER_GW   0xC0A85A01UL /* 192.168.90.1 */
#define OTHER_MASK 0xFFFFFF00UL

#define LOOP_PORT    7400
#define INBOUND_PORT 9999

/** write(2). */
static int sys_write(int fd, const void *buf, size_t count)
{
    return syscall3(SYS_WRITE, (long)fd, (long)buf, (long)count);
}

struct pollfd_probe {
    int fd;
    short events;
    short revents;
};

/** read(2). */
static int sys_read(int fd, void *buf, size_t count)
{
    return syscall3(SYS_READ, (long)fd, (long)buf, (long)count);
}

/** poll(2), with `timeout` in milliseconds. */
static int sys_poll(struct pollfd_probe *fds, unsigned int n, int timeout)
{
    return syscall3(SYS_POLL, (long)fds, (long)n, (long)timeout);
}

/** open(2) with no mode. */
static int sys_open(const char *path, int flags)
{
    return syscall3(SYS_OPEN, (long)path, (long)flags, (long)0);
}

/** close(2). */
static int sys_close(int fd)
{
    return syscall1(SYS_CLOSE, (long)fd);
}

/** exit(2); does not return. */
static void sys_exit(int status)
{
    syscall1(SYS_EXIT, (long)status);
    __builtin_unreachable();
}

/** ioctl(2). */
static int sys_ioctl(int fd, uint32_t request, void *arg)
{
    return syscall3(SYS_IOCTL, (long)fd, (long)request, (long)arg);
}

/** The socket operation `call` on `args` (`sock_call`), as an int. */
static int socketcall(int call, unsigned long *args)
{
    return (int)sock_call(call, args);
}

/** fork(2). */
static int sys_fork(void)
{
    return syscall0(SYS_FORK);
}

/** setuid(2). */
static int sys_setuid(int uid)
{
    return syscall1(SYS_SETUID, (long)uid);
}

/** waitpid(2). */
static int sys_waitpid(int pid, int *status, int options)
{
    return syscall3(SYS_WAITPID, (long)pid, (long)status, (long)options);
}

/** Write the string `s` to standard output. */
static void say(const char *s)
{
    size_t n = 0;

    while (s[n]) {
        n++;
    }
    sys_write(STDOUT_FILENO, s, n);
}

/**
 * Ping `ip`, up to four times: the first answer's result, or the last
 * failure's.
 *
 * The first echo after the ARP cache is emptied is dropped while the request
 * for the peer's hardware address is in flight, so one attempt does not show
 * whether the machine can reach anything.
 */
static int ping_retry(uint32_t ip)
{
    unsigned long args[6];
    int i, r = -1;

    for (i = 0; i < 4; i++) {
        args[0] = ip;
        r = socketcall(SC_PING, args);
        if (r >= 0) {
            return r;
        }
    }
    return r;
}

/** Print "NETIFPROBE FAIL: `what` (`n`)" and exit 1. */
static void fail_n(const char *what, int n)
{
    char buf[12];
    int i = (int)sizeof(buf);
    unsigned int v = (unsigned int)(n < 0 ? -n : n);

    buf[--i] = 0;
    do {
        buf[--i] = (char)('0' + (v % 10u));
        v /= 10u;
    } while (v && i > 1);
    if (n < 0) {
        buf[--i] = '-';
    }
    say("NETIFPROBE FAIL: ");
    say(what);
    say(" (");
    say(&buf[i]);
    say(")\n");
    sys_exit(1);
}

/** Print "NETIFPROBE FAIL: `what`" and exit 1. */
static void fail(const char *what)
{
    say("NETIFPROBE FAIL: ");
    say(what);
    say("\n");
    sys_exit(1);
}

/* struct ifreq: a 16-byte name, then a union whose largest member, struct
 * ifmap, begins with two unsigned longs. struct rtentry: a pad word of a
 * long, three sockaddrs, the flags, and further on a pointer to the
 * interface's name. Their sizes and the rtentry's offsets are Linux's for
 * each architecture. */
#if defined __x86_64__
#define IFREQ_SIZE 40
#define RT_SIZE    120
#define RT_DST     8
#define RT_GW      24
#define RT_MASK    40
#define RT_FLAGS   56
#define RT_DEV     88
#else
#define IFREQ_SIZE 32
#define RT_SIZE    84
#define RT_DST     4
#define RT_GW      20
#define RT_MASK    36
#define RT_FLAGS   52
#define RT_DEV     68
#endif
static uint8_t ifr[IFREQ_SIZE];
static uint8_t rt[RT_SIZE];
static const char other_dev[] = "eth9";
static uint8_t sa_listen[16];
static uint8_t sa_connect[16];

/** Clear `ifr` and put the interface name `name` in it; a name longer than
 * 15 bytes fails the probe. */
static void set_name(const char *name)
{
    int i;

    size_t n = 0;

    while (name[n]) {
        n++;
    }
    for (i = 0; i < IFREQ_SIZE; i++) {
        ifr[i] = 0;
    }
    if (n > 15) {
        fail("an interface name longer than the field it goes in");
    }
    for (i = 0; (size_t)i < n; i++) {
        ifr[i] = (uint8_t)name[i];
    }
}

/** The IPv4 address of the sockaddr_in in `ifr`, in host order. */
static uint32_t ifr_addr(void)
{
    return ((uint32_t)ifr[20] << 24) | ((uint32_t)ifr[21] << 16) | ((uint32_t)ifr[22] << 8) |
           (uint32_t)ifr[23];
}

/** Put the address `ip`, in host order, as a sockaddr_in in `ifr` after the
 * name `set_name` left there. */
static void set_ifr_addr(uint32_t ip)
{
    ifr[16] = AF_INET;
    ifr[20] = (uint8_t)(ip >> 24);
    ifr[21] = (uint8_t)(ip >> 16);
    ifr[22] = (uint8_t)(ip >> 8);
    ifr[23] = (uint8_t)ip;
}

/** Fill `rt` with the route to `dst`/`mask` through `gw`, which is 0 for no
 * gateway, and no interface. */
static void set_route(uint32_t dst, uint32_t gw, uint32_t mask)
{
    int i;

    for (i = 0; i < RT_SIZE; i++) {
        rt[i] = 0;
    }
    rt[RT_DST] = AF_INET;
    rt[RT_DST + 4] = (uint8_t)(dst >> 24);
    rt[RT_DST + 5] = (uint8_t)(dst >> 16);
    rt[RT_DST + 6] = (uint8_t)(dst >> 8);
    rt[RT_DST + 7] = (uint8_t)dst;
    rt[RT_GW] = AF_INET;
    rt[RT_GW + 4] = (uint8_t)(gw >> 24);
    rt[RT_GW + 5] = (uint8_t)(gw >> 16);
    rt[RT_GW + 6] = (uint8_t)(gw >> 8);
    rt[RT_GW + 7] = (uint8_t)gw;
    rt[RT_MASK] = AF_INET;
    rt[RT_MASK + 4] = (uint8_t)(mask >> 24);
    rt[RT_MASK + 5] = (uint8_t)(mask >> 16);
    rt[RT_MASK + 6] = (uint8_t)(mask >> 8);
    rt[RT_MASK + 7] = (uint8_t)mask;
    rt[RT_FLAGS] = (uint8_t)((RTF_UP | (gw ? RTF_GATEWAY : 0)) & 0xff);
}

/** Name the interface `dev` in the route: the pointer, a long's bytes. */
static void set_route_dev(const char *dev)
{
    unsigned long p = (unsigned long)dev;
    int i;

    for (i = 0; i < (int)sizeof(p); i++) {
        rt[RT_DEV + i] = (uint8_t)(p >> (8 * i));
    }
}

/** Fill the 16-byte sockaddr_in `sa` with `ip` and `port`, both given in
 * host order. */
static void set_sockaddr(uint8_t *sa, uint32_t ip, uint16_t port)
{
    int i;

    for (i = 0; i < 16; i++) {
        sa[i] = 0;
    }
    sa[0] = AF_INET;
    sa[2] = (uint8_t)(port >> 8);
    sa[3] = (uint8_t)port;
    sa[4] = (uint8_t)(ip >> 24);
    sa[5] = (uint8_t)(ip >> 16);
    sa[6] = (uint8_t)(ip >> 8);
    sa[7] = (uint8_t)ip;
}

/**
 * Configure a machine that came up with no address through `fd`, in the
 * order `ifconfig` uses: an address, then a mask, then a route, after which
 * the gateway answers. Between the first two the machine has the address
 * with the classful mask its first byte implies, so that a mask can follow.
 */
static void configure_from_nothing(int fd)
{
    set_name("eth0");
    if (sys_ioctl(fd, SIOCGIFNETMASK, ifr) != 0 || ifr_addr() != 0) {
        fail("a machine with no address reported a netmask");
    }
    set_name("eth0");
    set_ifr_addr(SLIRP_IP);
    if (sys_ioctl(fd, SIOCSIFADDR, ifr) != 0) {
        fail("an address could not be set on a machine that had none");
    }
    set_name("eth0");
    if (sys_ioctl(fd, SIOCGIFNETMASK, ifr) != 0 || ifr_addr() != 0xFF000000UL) {
        fail("an address with no mask did not take the classful one");
    }
    set_name("eth0");
    set_ifr_addr(SLIRP_MASK);
    if (sys_ioctl(fd, SIOCSIFNETMASK, ifr) != 0) {
        fail("the mask could not be set after the address");
    }
    set_name("eth0");
    if (sys_ioctl(fd, SIOCGIFADDR, ifr) != 0 || ifr_addr() != SLIRP_IP) {
        fail("the address did not survive the mask being set");
    }
    set_route(0, SLIRP_GW, 0);
    if (sys_ioctl(fd, SIOCADDRT, rt) != 0) {
        fail("the default route could not be set");
    }
    if (ping_retry(SLIRP_GW) < 0) {
        fail("the gateway did not answer a machine configured from nothing");
    }
}

/** Whether `hay` holds `needle`. */
static int holds(const char *hay, const char *needle)
{
    int i, j;

    for (i = 0; hay[i] != '\0'; i++) {
        for (j = 0; needle[j] != '\0' && hay[i + j] == needle[j]; j++) {
        }
        if (needle[j] == '\0') {
            return 1;
        }
    }
    return 0;
}

/**
 * `netifprobe second`: /proc/net/dev, a line per interface with that
 * interface's own counters, names both cards, eth0 and eth1. A file that
 * named one would be counting the second card's traffic on the first. The
 * file is read, since the boot log says what came up and not what the file
 * says afterwards.
 */
static void proc_names_both(void)
{
    char buf[2048];
    int fd, n, total = 0;

    fd = sys_open("/proc/net/dev", 0);
    if (fd < 0) {
        fail_n("/proc/net/dev did not open", fd);
    }
    while (total < (int)sizeof(buf) - 1) {
        n = sys_read(fd, buf + total, (size_t)((int)sizeof(buf) - 1 - total));
        if (n <= 0) {
            break;
        }
        total += n;
    }
    sys_close(fd);
    buf[total] = '\0';
    if (!holds(buf, "eth0:")) {
        fail("/proc/net/dev does not name the first interface");
    }
    if (!holds(buf, "eth1:")) {
        fail("/proc/net/dev names one interface on a machine with two");
    }
}

/* struct sockaddr_ll: family, protocol, ifindex, hatype, pkttype, halen,
 * addr. */
struct sockaddr_ll_probe {
    uint16_t sll_family;
    uint16_t sll_protocol;
    int sll_ifindex;
    uint16_t sll_hatype;
    uint8_t sll_pkttype;
    uint8_t sll_halen;
    uint8_t sll_addr[8];
};

/** Fill `addr` for an ARP packet socket on the interface `index`, 0 for
 * every interface. */
static void set_packet_addr(struct sockaddr_ll_probe *addr, int index)
{
    int i;

    for (i = 0; i < (int)sizeof(*addr); i++) {
        ((uint8_t *)addr)[i] = 0;
    }
    addr->sll_family = AF_PACKET;
    addr->sll_protocol = ETH_P_ARP_BE;
    addr->sll_ifindex = index;
}

/** getsockname of the packet socket `fd` reports the whole sockaddr_ll, ARP,
 * and the interface `index`; with an interface, Ethernet and that card's
 * MAC, which `raw_fd` reads with SIOCGIFHWADDR, and with none, no hardware
 * address. */
static void packet_name(int fd, int index, int raw_fd)
{
    unsigned long args[3];
    uint32_t len = sizeof(struct sockaddr_ll_probe);
    struct sockaddr_ll_probe addr = {0};
    int i;

    args[0] = (uint32_t)fd;
    args[1] = (unsigned long)&addr;
    args[2] = (unsigned long)&len;
    if (socketcall(SC_GETSOCKNAME, args) != 0 || len != sizeof(addr) ||
        addr.sll_family != AF_PACKET || addr.sll_protocol != ETH_P_ARP_BE ||
        addr.sll_ifindex != index) {
        fail("packet getsockname reported the wrong interface");
    }
    if (!index) {
        if (addr.sll_halen || addr.sll_hatype) {
            fail("an all-interface packet socket reported a hardware address");
        }
        return;
    }
    set_name(index == 2 ? "eth1" : "eth0");
    if (sys_ioctl(raw_fd, SIOCGIFHWADDR, ifr) != 0 || addr.sll_halen != 6 || addr.sll_hatype != 1) {
        fail("packet getsockname did not report an Ethernet address");
    }
    for (i = 0; i < 6; i++) {
        if (addr.sll_addr[i] != ifr[18 + i]) {
            fail("packet getsockname reported another interface's MAC");
        }
    }
}

/** SO_BINDTODEVICE on `fd`: "eth1" is set and read back, "eth9" is ENODEV
 * and leaves "eth1", and an empty name clears it. */
static void device_option(int fd)
{
    unsigned long args[5], len;
    char name[16];
    int i;

    for (i = 0; i < 3; i++) {
        args[0] = (uint32_t)fd;
        args[1] = SOL_SOCKET;
        args[2] = SO_BINDTODEVICE;
        args[3] = (unsigned long)(i == 0 ? "eth1" : i == 1 ? "eth9" : "");
        args[4] = i == 2 ? 1 : 5;
        if (socketcall(SC_SETSOCKOPT, args) != (i == 1 ? -ENODEV : 0)) {
            fail("SO_BINDTODEVICE did not validate the interface name");
        }
        len = sizeof(name);
        args[3] = (unsigned long)name;
        args[4] = (unsigned long)&len;
        if (socketcall(SC_GETSOCKOPT, args) != 0) {
            fail("SO_BINDTODEVICE did not read back");
        }
        if (i == 2) {
            if (len != 1 || name[0]) {
                fail("SO_BINDTODEVICE did not clear");
            }
        } else if (len != 5 || name[0] != 'e' || name[1] != 't' || name[2] != 'h' ||
                   name[3] != '1' || name[4]) {
            fail("SO_BINDTODEVICE lost the second interface");
        }
    }
}

/** accept on a stream socket that never listened, and on a datagram socket,
 * is EINVAL. */
static void invalid_accept(void)
{
    unsigned long args[3];
    int type, fd;

    for (type = SOCK_STREAM; type <= SOCK_DGRAM; type++) {
        args[0] = AF_INET;
        args[1] = (uint32_t)type;
        args[2] = 0;
        fd = socketcall(SC_SOCKET, args);
        if (fd < 0) {
            fail("invalid-accept test socket did not open");
        }
        args[0] = (uint32_t)fd;
        args[1] = 0;
        args[2] = 0;
        if (socketcall(SC_ACCEPT, args) != -EINVAL) {
            fail("accept did not refuse a socket that never listened");
        }
        sys_close(fd);
    }
}

/** A TCP connection to the second interface's address has that address,
 * 10.0.3.5, at both ends, and accept on either connected end is EINVAL. */
static void tcp_names(void)
{
    unsigned long args[3], len;
    uint8_t addr[16];
    int listener, client, conn, i, fd;

    args[0] = AF_INET;
    args[1] = SOCK_STREAM;
    args[2] = 0;
    listener = socketcall(SC_SOCKET, args);
    client = socketcall(SC_SOCKET, args);
    if (listener < 0 || client < 0) {
        fail("TCP address test sockets did not open");
    }
    set_sockaddr(addr, 0, LOOP_PORT + 1);
    args[0] = (uint32_t)listener;
    args[1] = (unsigned long)addr;
    args[2] = sizeof(addr);
    if (socketcall(SC_BIND, args) != 0) {
        fail("TCP address test listener did not bind");
    }
    args[1] = 1;
    if (socketcall(SC_LISTEN, args) != 0) {
        fail("TCP address test listener did not listen");
    }
    set_sockaddr(addr, SECOND_IP, LOOP_PORT + 1);
    args[0] = (uint32_t)client;
    args[1] = (unsigned long)addr;
    args[2] = sizeof(addr);
    if (socketcall(SC_CONNECT, args) != 0) {
        fail("TCP connection to the second interface did not open");
    }
    args[0] = (uint32_t)listener;
    args[1] = 0;
    args[2] = 0;
    conn = socketcall(SC_ACCEPT, args);
    if (conn < 0) {
        fail("TCP connection on the second interface was not accepted");
    }
    for (i = 0; i < 2; i++) {
        fd = i ? conn : client;
        len = sizeof(addr);
        args[0] = (uint32_t)fd;
        args[1] = (unsigned long)addr;
        args[2] = (unsigned long)&len;
        if (socketcall(SC_GETSOCKNAME, args) != 0 || len != sizeof(addr) || addr[4] != 10 ||
            addr[5] != 0 || addr[6] != 3 || addr[7] != 5) {
            fail("TCP getsockname reported the first interface");
        }
        args[1] = 0;
        args[2] = 0;
        if (socketcall(SC_ACCEPT, args) != -EINVAL) {
            fail("accept did not refuse a connected stream");
        }
    }
    sys_close(conn);
    sys_close(client);
    sys_close(listener);
}

/**
 * `netifprobe second`: a packet socket bound to an interface sends and
 * receives on that interface alone.
 *
 * A machine with two cards is on two networks, and a client waiting for a
 * lease on one of them must be given that one's frames and send on that one.
 * A socket is bound to the second interface and an ARP request for that
 * subnet's gateway is sent through it: the answer can come back only if the
 * frame left by the second card, since the first card's network has no host
 * at that address, and every datagram the socket is given must say that it
 * arrived on the second interface. A second socket, bound to the first
 * interface, may be given nothing that arrived on the other. `raw_fd` is the
 * descriptor for the interface ioctls.
 */
static void packet_on_second(int raw_fd)
{
    unsigned long args[6];
    struct sockaddr_ll_probe to, from = {0};
    uint8_t mac[6];
    uint8_t arp[28];
    uint8_t buf[64];
    uint32_t fromlen;
    int on_first, on_second, i, tries, got;

    args[0] = AF_PACKET;
    args[1] = SOCK_DGRAM;
    args[2] = ETH_P_ARP_BE;
    on_second = socketcall(SC_SOCKET, args);
    if (on_second < 0) {
        fail("a packet socket for the second interface did not open");
    }
    args[0] = AF_PACKET;
    args[1] = SOCK_DGRAM;
    args[2] = ETH_P_ARP_BE;
    on_first = socketcall(SC_SOCKET, args);
    if (on_first < 0) {
        fail("a packet socket for the first interface did not open");
    }

    packet_name(on_second, 0, raw_fd);
    set_packet_addr(&to, 2);
    args[0] = (uint32_t)on_second;
    args[1] = (unsigned long)&to;
    args[2] = sizeof(to);
    if (socketcall(SC_BIND, args) != 0) {
        fail("the socket would not bind to the second interface");
    }
    set_packet_addr(&to, 1);
    args[0] = (uint32_t)on_first;
    args[1] = (unsigned long)&to;
    args[2] = sizeof(to);
    if (socketcall(SC_BIND, args) != 0) {
        fail("the socket would not bind to the first interface");
    }

    packet_name(on_first, 1, raw_fd);
    packet_name(on_second, 2, raw_fd);

    /* The reply goes to the sender the request names: the second card. */
    set_name("eth1");
    if (sys_ioctl(raw_fd, SIOCGIFHWADDR, ifr) != 0) {
        fail("the second interface would not report its station address");
    }
    for (i = 0; i < 6; i++) {
        mac[i] = ifr[18 + i];
    }
    for (i = 0; i < (int)sizeof(arp); i++) {
        arp[i] = 0;
    }
    arp[1] = 1;    /* Ethernet */
    arp[2] = 0x08; /* IPv4 */
    arp[4] = 6;
    arp[5] = 4;
    arp[7] = 1; /* a request */
    for (i = 0; i < 6; i++) {
        arp[8 + i] = mac[i];
    }
    arp[14] = (uint8_t)(SECOND_IP >> 24);
    arp[15] = (uint8_t)(SECOND_IP >> 16);
    arp[16] = (uint8_t)(SECOND_IP >> 8);
    arp[17] = (uint8_t)SECOND_IP;
    arp[24] = (uint8_t)(SECOND_GW >> 24);
    arp[25] = (uint8_t)(SECOND_GW >> 16);
    arp[26] = (uint8_t)(SECOND_GW >> 8);
    arp[27] = (uint8_t)SECOND_GW;

    set_packet_addr(&to, 2);
    to.sll_halen = 6;
    for (i = 0; i < 6; i++) {
        to.sll_addr[i] = 0xff;
    }
    args[0] = (uint32_t)on_second;
    args[1] = (unsigned long)arp;
    args[2] = sizeof(arp);
    args[3] = 0;
    args[4] = (unsigned long)&to;
    args[5] = sizeof(to);
    if (socketcall(SC_SENDTO, args) != (int)sizeof(arp)) {
        fail("the request would not send through the second interface");
    }

    /* The answer, which exists only on the second card's network. */
    got = 0;
    for (tries = 0; tries < 200 && !got; tries++) {
        struct pollfd_probe pfd;

        pfd.fd = on_second;
        pfd.events = POLLIN;
        pfd.revents = 0;
        sys_poll(&pfd, 1, 50);
        for (i = 0; i < (int)sizeof(from); i++) {
            ((uint8_t *)&from)[i] = 0;
        }
        fromlen = sizeof(from);
        args[0] = (uint32_t)on_second;
        args[1] = (unsigned long)buf;
        args[2] = sizeof(buf);
        args[3] = 0x40; /* MSG_DONTWAIT */
        args[4] = (unsigned long)&from;
        args[5] = (unsigned long)&fromlen;
        if (socketcall(SC_RECVFROM, args) < 0) {
            continue;
        }
        if (from.sll_ifindex != 2) {
            fail_n("a datagram on the second interface claimed another", from.sll_ifindex);
        }
        if (from.sll_hatype != 1) {
            fail_n("the answer did not arrive over Ethernet", (int)from.sll_hatype);
        }
        if (from.sll_pkttype != 0 && from.sll_pkttype != 1) {
            fail_n("the answer was addressed to neither this machine nor everyone",
                   (int)from.sll_pkttype);
        }
        got = 1;
    }
    if (!got) {
        fail("nothing answered on the second interface, so the frame did not "
             "leave by it");
    }

    /* What the first interface's socket was given arrived there. */
    for (tries = 0; tries < 8; tries++) {
        for (i = 0; i < (int)sizeof(from); i++) {
            ((uint8_t *)&from)[i] = 0;
        }
        fromlen = sizeof(from);
        args[0] = (uint32_t)on_first;
        args[1] = (unsigned long)buf;
        args[2] = sizeof(buf);
        args[3] = 0x40; /* MSG_DONTWAIT */
        args[4] = (unsigned long)&from;
        args[5] = (unsigned long)&fromlen;
        if (socketcall(SC_RECVFROM, args) < 0) {
            break;
        }
        if (from.sll_ifindex != 1) {
            fail_n("the first interface's socket was given another interface's "
                   "traffic",
                   from.sll_ifindex);
        }
    }

    sys_close(on_second);
    sys_close(on_first);
}

/**
 * The flags RTM_GETLINK's dump, which `ip link` reads, gives the interface
 * of index `index`: the `ifi_flags` of its `struct ifinfomsg`. Fails the
 * probe when the dump cannot be had or does not list the interface.
 */
static uint32_t link_flags(int index)
{
    static uint32_t buf[1024];
    uint32_t req[8];
    unsigned long args[6];
    uint32_t flags = 0;
    int fd, n, off, found = 0;

    args[0] = AF_NETLINK;
    args[1] = SOCK_RAW;
    args[2] = NETLINK_ROUTE;
    fd = socketcall(SC_SOCKET, args);
    if (fd < 0) {
        fail_n("a netlink route socket did not open", fd);
    }
    for (n = 0; n < 8; n++) {
        req[n] = 0;
    }
    req[0] = sizeof(req); /* nlmsghdr, then a struct ifinfomsg of zeros */
    req[1] = RTM_GETLINK | ((uint32_t)(NLM_F_REQUEST | NLM_F_DUMP) << 16);
    req[2] = 1;
    args[0] = (unsigned long)fd;
    args[1] = (unsigned long)req;
    args[2] = sizeof(req);
    args[3] = 0;
    n = socketcall(SC_SEND, args);
    if (n != (int)sizeof(req)) {
        fail_n("the link dump could not be asked for", n);
    }
    for (;;) {
        args[1] = (unsigned long)buf;
        args[2] = sizeof(buf);
        n = socketcall(SC_RECV, args);
        if (n <= 0) {
            fail_n("the link dump ended without NLMSG_DONE", n);
        }
        for (off = 0; off + 16 <= n; off += (int)((buf[off / 4] + 3u) & ~3u)) {
            uint32_t len = buf[off / 4];
            uint32_t type = buf[off / 4 + 1] & 0xFFFFu;

            if (len < 16 || off + (int)len > n) {
                fail("the link dump had a message of a bad length");
            }
            if (type == NLMSG_DONE) {
                sys_close(fd);
                if (!found) {
                    fail_n("the link dump did not list the interface", index);
                }
                return flags;
            }
            if (type == NLMSG_ERROR) {
                fail("the link dump was answered with an error");
            }
            if (type == RTM_NEWLINK && len >= 32 && (int)buf[off / 4 + 5] == index) {
                flags = buf[off / 4 + 6];
                found = 1;
            }
        }
    }
}

/**
 * `netifprobe second`: with two cards, each gateway answers through its own
 * card, and taking the second card down leaves the first serving, which
 * shows that the flag belongs to the interface and not to the stack. The
 * link dump says the same of each card: eth1 down and not running, eth0 up
 * and running, and eth1 up again once it is brought back. The other rows of
 * the two-card machine run, and the probe exits with NETIFPROBE OK; it does
 * not return.
 */
static void second_down(void)
{
    unsigned long args[6];
    int fd;

    args[0] = AF_INET;
    args[1] = SOCK_RAW;
    args[2] = IPPROTO_RAW;
    fd = socketcall(SC_SOCKET, args);
    if (fd < 0) {
        fail("the raw descriptor the interface is asked through did not open");
    }

    /* A neighbour on each subnet, reached through the card that is on it. */
    if (ping_retry(SLIRP_GW) < 0) {
        fail("the first interface's gateway did not answer");
    }
    if (ping_retry(SECOND_GW) < 0) {
        fail("the second interface's gateway did not answer");
    }

    set_name("eth1");
    if (sys_ioctl(fd, SIOCGIFFLAGS, ifr) != 0) {
        fail("the second interface could not be asked about its flags");
    }
    if (!(ifr[16] & IFF_UP)) {
        fail("the second interface was not up to begin with");
    }
    set_name("eth1");
    ifr[16] = 0;
    ifr[17] = 0;
    if (sys_ioctl(fd, SIOCSIFFLAGS, ifr) != 0) {
        fail("the second interface could not be taken down");
    }
    set_name("eth0");
    if (sys_ioctl(fd, SIOCGIFFLAGS, ifr) != 0 || !(ifr[16] & IFF_UP)) {
        fail("the first interface went down with the second");
    }
    if (ping_retry(SLIRP_GW) < 0) {
        fail("the gateway stopped answering when the second interface went down");
    }
    if (link_flags(2) & (IFF_UP | IFF_RUNNING)) {
        fail("the link dump reported the second interface up while it was down");
    }
    if ((link_flags(1) & (IFF_UP | IFF_RUNNING)) != (IFF_UP | IFF_RUNNING)) {
        fail("the link dump reported the first interface down with the second");
    }
    set_name("eth1");
    ifr[16] = IFF_UP;
    ifr[17] = 0;
    if (sys_ioctl(fd, SIOCSIFFLAGS, ifr) != 0) {
        fail("the second interface could not be brought back up");
    }
    if (!(link_flags(2) & IFF_UP)) {
        fail("the link dump reported the second interface down once it was up");
    }
    proc_names_both();
    device_option(fd);
    invalid_accept();
    tcp_names();
    packet_on_second(fd);
    say("NETIFPROBE OK\n");
    sys_exit(0);
}

static void netif_main(void);

/** The probe's entry, given the initial stack `sp`: `netifprobe second`
 * runs `second_down`, and any other command line `netif_main`. */
void probe_main(unsigned long *sp)
{
    int argc = (int)sp[0];
    char **argv = (char **)(sp + 1);

    if (argc > 1 && argv[1][0] == 's') {
        second_down();
    }
    netif_main();
}

PROBE_SP_ENTRY;

/**
 * The interface ioctls on a machine with one card, in order; exits with
 * NETIFPROBE OK, or fails the probe.
 *
 * The descriptor is the one BusyBox's `udhcp_read_interface` opens,
 * AF_INET/SOCK_RAW/IPPROTO_RAW, on which a DHCP client makes only these
 * ioctls; a raw socket for TCP is EPROTONOSUPPORT. The machine reports
 * itself: eth0 is index 1 and index 1 is eth0, a name or index it lacks is
 * ENODEV, and the card has an Ethernet address, 10.0.2.15/24 with the
 * subnet's broadcast address, an MTU of 1500 and the flags IFF_RUNNING,
 * IFF_MULTICAST and IFF_UP or IFF_BROADCAST. A machine booted with no
 * address is configured from nothing (`configure_from_nothing`), and the
 * probe ends there. The query descriptor refuses bind, connect, listen, read
 * and write with EOPNOTSUPP, and a poll reports POLLERR at once rather than
 * leaving a caller waiting.
 *
 * A TCP connection to the machine itself is open across the change of
 * address, so both its ends recorded the old one. A process that is not root
 * may not set the address (EPERM). After the change a write to one end and a
 * read of the other are ENETRESET, and not a bare -1, which the C library
 * reads as EPERM: each end has the reason once and its first reader takes
 * it, as SO_ERROR does on Linux, so SO_ERROR then reads 0. The
 * listener survives, since it answers whatever address the machine has, and
 * accepts a connection to the new address. The same address and mask set
 * again, as a DHCP client sets them at every renewal, reset nothing. An
 * address in another class takes that class's mask, and that later change
 * does not arm the old connection's reset again.
 *
 * Down is down in both directions: with the interface down the gateway does
 * not answer a ping, and the datagram the suite sends at "NETIFPROBE DOWN"
 * does not arrive. With the interface up again the datagram the suite sends
 * at "NETIFPROBE UP" arrives, which shows that the suite sends one. The
 * second connection, open across that change of address, reads ENETRESET
 * without blocking.
 *
 * Addresses in 0/8, in 240/4 and of multicast are EINVAL. A default route
 * sets the gateway, a gateway off the subnet is ENETUNREACH as on Linux, and
 * another route goes into the table and out again. A default route with no
 * gateway is EINVAL and does not remove the gateway the machine has. A route
 * for an interface the machine lacks is ENODEV. The route to the machine's
 * own subnet may be added, since it holds, but not removed (EOPNOTSUPP),
 * since the stack cannot isolate that traffic. The default route is removed
 * once, and a second removal is ESRCH, which ends the loop in which a DHCP
 * client's script removes old default routes.
 *
 * An address that is a network address under its classful mask and a host
 * under a wider one, 192.168.1.0, is accepted and not put in force until the
 * wider mask makes it a host; a mask under which it is a network address is
 * EINVAL, and one refused while the classful mask still stands leaves the
 * machine with no address. Last, the machine's address, mask and route are
 * put back, and the gateway answers once ARP has learned it again.
 */
static void netif_main(void)
{
    unsigned long args[6];
    int fd, listener, client, conn, client2, conn2, pid, status;
    int inbound, tries, got_inbound, r;
    /* The kernel writes it through the pointer getsockopt is given. */
    volatile int err;
    struct pollfd_probe pfd;
    uint32_t optlen;

    /* The descriptor BusyBox's `udhcp_read_interface` makes its ioctls on. */
    args[0] = AF_INET;
    args[1] = SOCK_RAW;
    args[2] = IPPROTO_RAW;
    fd = socketcall(SC_SOCKET, args);
    if (fd < 0) {
        fail("the raw descriptor a DHCP client asks the interface through did not open");
    }
    args[0] = AF_INET;
    args[1] = SOCK_RAW;
    args[2] = 6;
    if (socketcall(SC_SOCKET, args) != -EPROTONOSUPPORT) {
        fail("a raw socket for a protocol this stack does not carry was not refused");
    }

    /* What this machine says about itself. */
    set_name("eth0");
    if (sys_ioctl(fd, SIOCGIFINDEX, ifr) != 0 || ifr[16] != 1) {
        fail("the interface did not report index 1");
    }
    set_name("eth9");
    if (sys_ioctl(fd, SIOCGIFINDEX, ifr) != -ENODEV) {
        fail("an interface this machine does not have was not refused");
    }
    /* An index in, a name out. */
    set_name("");
    ifr[16] = 1;
    if (sys_ioctl(fd, SIOCGIFNAME, ifr) != 0) {
        fail("the interface's name did not read back from its index");
    }
    if (ifr[0] != 'e' || ifr[1] != 't' || ifr[2] != 'h' || ifr[3] != '0' || ifr[4] != 0) {
        fail("the name that came back is not this machine's");
    }
    set_name("");
    ifr[16] = 7;
    if (sys_ioctl(fd, SIOCGIFNAME, ifr) != -ENODEV) {
        fail("an index this machine does not have was not refused");
    }

    set_name("eth0");
    if (sys_ioctl(fd, SIOCGIFHWADDR, ifr) != 0) {
        fail("the hardware address did not read");
    }
    if (ifr[16] != 1) {
        fail("the hardware address is not reported as Ethernet");
    }
    if (ifr[18] == 0 && ifr[19] == 0 && ifr[20] == 0 && ifr[21] == 0 && ifr[22] == 0 &&
        ifr[23] == 0) {
        fail("the hardware address is all zeros");
    }
    /* A machine booted with `rach2.ip=none` has no address. */
    set_name("eth0");
    if (sys_ioctl(fd, SIOCGIFADDR, ifr) != 0) {
        fail("the address did not read back");
    }
    if (ifr_addr() == 0) {
        configure_from_nothing(fd);
        say("NETIFPROBE OK\n");
        sys_exit(0);
    }
    if (ifr_addr() != SLIRP_IP) {
        fail("the address this machine booted with is not the one it should be");
    }
    set_name("eth0");
    if (sys_ioctl(fd, SIOCGIFNETMASK, ifr) != 0 || ifr_addr() != SLIRP_MASK) {
        fail("the netmask did not read back");
    }
    set_name("eth0");
    if (sys_ioctl(fd, SIOCGIFBRDADDR, ifr) != 0 || ifr_addr() != SLIRP_BRD) {
        fail("the broadcast address is not the subnet's");
    }
    set_name("eth0");
    if (sys_ioctl(fd, SIOCGIFMTU, ifr) != 0 || ifr[16] != 0xdc || ifr[17] != 0x05) {
        fail("the MTU is not 1500");
    }
    set_name("eth0");
    if (sys_ioctl(fd, SIOCGIFFLAGS, ifr) != 0) {
        fail("the flags did not read");
    }
    if (!(ifr[16] & (IFF_UP | IFF_BROADCAST)) || !(ifr[17] & (IFF_MULTICAST >> 8)) ||
        !(ifr[16] & IFF_RUNNING)) {
        fail("an interface that is up and running did not say so");
    }

    /* The query descriptor refuses everything but its ioctls. */
    args[0] = (uint32_t)fd;
    args[1] = (unsigned long)sa_listen;
    args[2] = 16;
    if (socketcall(SC_BIND, args) != -EOPNOTSUPP) {
        fail("the query descriptor accepted a bind");
    }
    if (socketcall(SC_CONNECT, args) != -EOPNOTSUPP) {
        fail("the query descriptor accepted a connect");
    }
    args[1] = 4;
    if (socketcall(SC_LISTEN, args) != -EOPNOTSUPP) {
        fail("the query descriptor accepted a listen");
    }
    if (sys_read(fd, ifr, 1) != -EOPNOTSUPP) {
        fail("the query descriptor was read");
    }
    if (sys_write(fd, ifr, 1) != -EOPNOTSUPP) {
        fail("the query descriptor was written");
    }
    pfd.fd = fd;
    pfd.events = POLLIN | POLLOUT;
    pfd.revents = 0;
    if (sys_poll(&pfd, 1, 0) != 1 || !(pfd.revents & POLLERR)) {
        fail("a poll of the query descriptor did not report at once that it is neither");
    }

    /* A connection to this machine itself, open across the change. */
    args[0] = AF_INET;
    args[1] = SOCK_STREAM;
    args[2] = 0;
    listener = socketcall(SC_SOCKET, args);
    if (listener < 0) {
        fail("the listener did not open");
    }
    set_sockaddr(sa_listen, 0, LOOP_PORT);
    args[0] = (uint32_t)listener;
    args[1] = (unsigned long)sa_listen;
    args[2] = 16;
    if (socketcall(SC_BIND, args) != 0) {
        fail("the listener did not bind");
    }
    args[0] = (uint32_t)listener;
    args[1] = 4;
    if (socketcall(SC_LISTEN, args) != 0) {
        fail("the listener did not listen");
    }
    args[0] = AF_INET;
    args[1] = SOCK_STREAM;
    args[2] = 0;
    client = socketcall(SC_SOCKET, args);
    if (client < 0) {
        fail("the client did not open");
    }
    set_sockaddr(sa_connect, SLIRP_IP, LOOP_PORT);
    args[0] = (uint32_t)client;
    args[1] = (unsigned long)sa_connect;
    args[2] = 16;
    if (socketcall(SC_CONNECT, args) != 0) {
        fail("the connection to this machine itself did not complete");
    }
    args[0] = (uint32_t)listener;
    args[1] = 0;
    args[2] = 0;
    conn = socketcall(SC_ACCEPT, args);
    if (conn < 0) {
        fail("the connection was not accepted");
    }

    /* Setting is root's, and a child that is not root must be told so. */
    pid = sys_fork();
    if (pid == 0) {
        set_name("eth0");
        set_ifr_addr(OTHER_IP);
        if (sys_setuid(1000) != 0) {
            sys_exit(2);
        }
        sys_exit(sys_ioctl(fd, SIOCSIFADDR, ifr) == -EPERM ? 0 : 3);
    }
    if (pid < 0) {
        fail("the fork for the unprivileged case failed");
    }
    status = -1;
    if (sys_waitpid(pid, &status, 0) != pid) {
        fail("waiting for the unprivileged case failed");
    }
    if ((status & 0x7f) != 0) {
        fail("the unprivileged case died on a signal");
    }
    if (((status >> 8) & 0xff) != 0) {
        fail("an ordinary user was allowed to set the machine's address");
    }

    /* And the change itself. */
    set_name("eth0");
    set_ifr_addr(OTHER_IP);
    if (sys_ioctl(fd, SIOCSIFADDR, ifr) != 0) {
        fail("the address did not set");
    }
    set_name("eth0");
    set_ifr_addr(OTHER_MASK);
    if (sys_ioctl(fd, SIOCSIFNETMASK, ifr) != 0) {
        fail("the netmask did not set");
    }
    set_name("eth0");
    if (sys_ioctl(fd, SIOCGIFADDR, ifr) != 0 || ifr_addr() != OTHER_IP) {
        fail("the address that was set did not read back");
    }

    /* The connection is aborted, and each end reports ENETRESET once. */
    if (sys_write(conn, ifr, 1) != -ENETRESET) {
        fail("a write to the aborted connection did not report why it ended");
    }
    if (sys_read(client, ifr, 1) != -ENETRESET) {
        fail("a read of the aborted connection did not report why it ended");
    }
    args[0] = (uint32_t)conn;
    args[1] = SOL_SOCKET;
    args[2] = SO_ERROR;
    args[3] = (unsigned long)&err;
    optlen = sizeof(err);
    args[4] = (unsigned long)&optlen;
    err = -1;
    if (socketcall(SC_GETSOCKOPT, args) != 0) {
        fail("the connection's error did not read");
    }
    if (err != 0) {
        fail("the reason was reported twice, so nothing took it");
    }

    /* The listener survives and accepts at the new address. */
    args[0] = AF_INET;
    args[1] = SOCK_STREAM;
    args[2] = 0;
    client2 = socketcall(SC_SOCKET, args);
    if (client2 < 0) {
        fail("the second client did not open");
    }
    set_sockaddr(sa_connect, OTHER_IP, LOOP_PORT);
    args[0] = (uint32_t)client2;
    args[1] = (unsigned long)sa_connect;
    args[2] = 16;
    if (socketcall(SC_CONNECT, args) != 0) {
        fail("a connection to the new address was refused, so the listener died with the old one");
    }
    args[0] = (uint32_t)listener;
    args[1] = 0;
    args[2] = 0;
    conn2 = socketcall(SC_ACCEPT, args);
    if (conn2 < 0) {
        fail("the listener did not accept after the address changed");
    }

    /* The same address and mask set again reset nothing. */
    set_name("eth0");
    set_ifr_addr(OTHER_IP);
    if (sys_ioctl(fd, SIOCSIFADDR, ifr) != 0) {
        fail("reapplying the same address failed");
    }
    set_name("eth0");
    set_ifr_addr(OTHER_MASK);
    if (sys_ioctl(fd, SIOCSIFNETMASK, ifr) != 0) {
        fail("reapplying the same mask failed");
    }
    args[0] = (uint32_t)conn2;
    args[1] = SOL_SOCKET;
    args[2] = SO_ERROR;
    args[3] = (unsigned long)&err;
    optlen = sizeof(err);
    args[4] = (unsigned long)&optlen;
    err = -1;
    if (socketcall(SC_GETSOCKOPT, args) != 0) {
        fail("the surviving connection's error did not read");
    }
    if (err != 0) {
        fail("reapplying the same address reset a connection");
    }

    /* An address in another class takes that class's mask. */
    set_name("eth0");
    set_ifr_addr(0xAC100102UL); /* 172.16.1.2 */
    if (sys_ioctl(fd, SIOCSIFADDR, ifr) != 0) {
        fail("an address in another class did not set");
    }
    set_name("eth0");
    if (sys_ioctl(fd, SIOCGIFNETMASK, ifr) != 0 || ifr_addr() != 0xFFFF0000UL) {
        fail("an address in another class kept the mask of the one before it");
    }

    /* That change does not arm the old connection's reset again. */
    args[0] = (uint32_t)conn;
    args[1] = SOL_SOCKET;
    args[2] = SO_ERROR;
    args[3] = (unsigned long)&err;
    optlen = sizeof(err);
    args[4] = (unsigned long)&optlen;
    err = -1;
    if (socketcall(SC_GETSOCKOPT, args) != 0) {
        fail("the closed connection's error did not read");
    }
    if (err != 0) {
        fail("a later address change armed the same reset again");
    }
    set_name("eth0");
    set_ifr_addr(OTHER_IP);
    if (sys_ioctl(fd, SIOCSIFADDR, ifr) != 0) {
        fail("the address did not come back");
    }
    set_name("eth0");
    set_ifr_addr(OTHER_MASK);
    if (sys_ioctl(fd, SIOCSIFNETMASK, ifr) != 0) {
        fail("the mask did not come back");
    }

    /* Down is down: no ping answered and no datagram received. */
    set_name("eth0");
    set_ifr_addr(SLIRP_IP);
    if (sys_ioctl(fd, SIOCSIFADDR, ifr) != 0) {
        fail("the address for the down case did not set");
    }
    set_name("eth0");
    set_ifr_addr(SLIRP_MASK);
    if (sys_ioctl(fd, SIOCSIFNETMASK, ifr) != 0) {
        fail("the mask for the down case did not set");
    }
    if (ping_retry(SLIRP_GW) < 0) {
        fail("the gateway did not answer while the interface was up");
    }
    /* The socket for the datagrams the suite sends from outside. */
    args[0] = AF_INET;
    args[1] = SOCK_DGRAM;
    args[2] = 0;
    inbound = socketcall(SC_SOCKET, args);
    if (inbound < 0) {
        fail("the socket for the inbound case did not open");
    }
    set_sockaddr(sa_listen, 0, INBOUND_PORT);
    args[0] = (uint32_t)inbound;
    args[1] = (unsigned long)sa_listen;
    args[2] = 16;
    if (socketcall(SC_BIND, args) != 0) {
        fail("the socket for the inbound case did not bind");
    }

    set_name("eth0");
    ifr[16] = 0;
    ifr[17] = 0;
    if (sys_ioctl(fd, SIOCSIFFLAGS, ifr) != 0) {
        fail("the interface could not be taken down");
    }
    set_name("eth0");
    if (sys_ioctl(fd, SIOCGIFFLAGS, ifr) != 0 || (ifr[16] & IFF_UP)) {
        fail("an interface that was taken down still says it is up");
    }
    args[0] = SLIRP_GW;
    if (socketcall(SC_PING, args) >= 0) {
        fail("the gateway answered a machine whose interface was down");
    }

    /* At the token the suite sends a datagram, which may not arrive. */
    say("NETIFPROBE DOWN\n");
    for (tries = 0; tries < 60; tries++) {
        pfd.fd = inbound;
        pfd.events = POLLIN;
        pfd.revents = 0;
        if (sys_poll(&pfd, 1, 50) > 0 && (pfd.revents & POLLIN)) {
            fail("a datagram reached a machine whose interface was down");
        }
    }
    set_name("eth0");
    ifr[16] = IFF_UP;
    ifr[17] = 0;
    if (sys_ioctl(fd, SIOCSIFFLAGS, ifr) != 0) {
        fail("the interface could not be brought back up");
    }
    r = ping_retry(SLIRP_GW);
    if (r < 0) {
        fail_n("the gateway did not answer after the interface came back up", r);
    }

    /* The second connection, open across this change, read without blocking. */
    args[0] = (uint32_t)conn2;
    args[1] = (unsigned long)ifr;
    args[2] = 1;
    args[3] = 0x40; /* MSG_DONTWAIT */
    if (socketcall(SC_RECV, args) != -ENETRESET) {
        fail("a nonblocking read of the aborted connection did not report why it ended");
    }

    /* With the interface up, the datagram the suite sends arrives. */
    say("NETIFPROBE UP\n");
    got_inbound = 0;
    for (tries = 0; tries < 200 && !got_inbound; tries++) {
        pfd.fd = inbound;
        pfd.events = POLLIN;
        pfd.revents = 0;
        if (sys_poll(&pfd, 1, 50) > 0 && (pfd.revents & POLLIN)) {
            got_inbound = 1;
        }
    }
    if (!got_inbound) {
        fail("no datagram arrived once the interface was up, so the case above proved nothing");
    }
    sys_close(inbound);

    set_name("eth0");
    set_ifr_addr(OTHER_IP);
    if (sys_ioctl(fd, SIOCSIFADDR, ifr) != 0) {
        fail("the address did not return after the down case");
    }
    set_name("eth0");
    set_ifr_addr(OTHER_MASK);
    if (sys_ioctl(fd, SIOCSIFNETMASK, ifr) != 0) {
        fail("the mask did not return after the down case");
    }

    /* And the addresses no machine can have. */
    set_name("eth0");
    set_ifr_addr(0x00010203UL); /* 0.1.2.3, which names no host */
    if (sys_ioctl(fd, SIOCSIFADDR, ifr) != -EINVAL) {
        fail("an address in 0/8 was taken");
    }
    set_name("eth0");
    set_ifr_addr(0xF0010203UL); /* 240.1.2.3, reserved */
    if (sys_ioctl(fd, SIOCSIFADDR, ifr) != -EINVAL) {
        fail("a reserved address was taken");
    }
    set_name("eth0");
    set_ifr_addr(0xE0000001UL); /* 224.0.0.1, multicast */
    if (sys_ioctl(fd, SIOCSIFADDR, ifr) != -EINVAL) {
        fail("a multicast address was taken");
    }

    /* Routes: the default, a gateway off the subnet, another network. */
    set_route(0, OTHER_GW, 0);
    if (sys_ioctl(fd, SIOCADDRT, rt) != 0) {
        fail("a default route was not taken");
    }
    set_route(0, SLIRP_GW, 0);
    if (sys_ioctl(fd, SIOCADDRT, rt) != -ENETUNREACH) {
        fail("a gateway off the subnet was taken");
    }
    set_route(0x0A0A0A00UL, 0, 0xFFFFFF00UL);
    if (sys_ioctl(fd, SIOCADDRT, rt) != 0) {
        fail("a route to another network was not taken");
    }
    set_route(0x0A0A0A00UL, 0, 0xFFFFFF00UL);
    if (sys_ioctl(fd, SIOCDELRT, rt) != 0) {
        fail("a route to another network was not removed");
    }
    /* A default route with no gateway is refused and removes nothing. */
    set_route(0, OTHER_GW, 0);
    if (sys_ioctl(fd, SIOCADDRT, rt) != 0) {
        fail("the default route did not go back for the next case");
    }
    set_route(0, 0, 0);
    if (sys_ioctl(fd, SIOCADDRT, rt) != -EINVAL) {
        fail("a default route with no gateway was taken");
    }
    set_route(0, OTHER_GW, 0);
    if (sys_ioctl(fd, SIOCDELRT, rt) != 0) {
        fail("the gateway was removed by the route that was refused");
    }
    set_route(0, OTHER_GW, 0);
    if (sys_ioctl(fd, SIOCADDRT, rt) != 0) {
        fail("the default route did not go back for the removal cases");
    }

    /* A route for an interface this machine does not have. */
    set_route(0, SLIRP_GW, 0);
    set_route_dev(other_dev);
    if (sys_ioctl(fd, SIOCADDRT, rt) != -ENODEV) {
        fail("a route for an interface this machine does not have was taken");
    }
    set_route(0, 0, 0);
    set_route_dev(other_dev);
    if (sys_ioctl(fd, SIOCDELRT, rt) != -ENODEV) {
        fail("a removal for an interface this machine does not have was taken");
    }

    /* The route to the machine's own subnet: added, and not removed. */
    set_route(OTHER_IP & OTHER_MASK, 0, OTHER_MASK);
    if (sys_ioctl(fd, SIOCADDRT, rt) != 0) {
        fail("the route to this machine's own subnet was refused");
    }
    set_route(OTHER_IP & OTHER_MASK, 0, OTHER_MASK);
    if (sys_ioctl(fd, SIOCDELRT, rt) != -EOPNOTSUPP) {
        fail("removing the route this machine has by being on the subnet succeeded");
    }
    set_route(0, 0, 0);
    if (sys_ioctl(fd, SIOCDELRT, rt) != 0) {
        fail("the default route was not removed");
    }
    /* A second removal is ESRCH, which ends a DHCP script's loop. */
    if (sys_ioctl(fd, SIOCDELRT, rt) != -ESRCH) {
        fail("removing a route that is not there succeeded");
    }

    /* 192.168.1.0: a network address until a wider mask makes it a host. */
    set_name("eth0");
    set_ifr_addr(0xC0A80100UL); /* 192.168.1.0 */
    if (sys_ioctl(fd, SIOCSIFADDR, ifr) != 0) {
        fail("an address that is a host only under a wider mask was refused");
    }
    set_name("eth0");
    if (sys_ioctl(fd, SIOCGIFADDR, ifr) != 0 || ifr_addr() != 0) {
        fail("an address that is not yet a host was put in force");
    }
    set_name("eth0");
    set_ifr_addr(0xFFFF0000UL);
    if (sys_ioctl(fd, SIOCSIFNETMASK, ifr) != 0) {
        fail("the wider mask that makes it a host was refused");
    }
    set_name("eth0");
    if (sys_ioctl(fd, SIOCGIFADDR, ifr) != 0 || ifr_addr() != 0xC0A80100UL) {
        fail("the address did not survive the mask that justified it");
    }
    set_name("eth0");
    set_ifr_addr(0xFFFFFF00UL);
    if (sys_ioctl(fd, SIOCSIFNETMASK, ifr) != -EINVAL) {
        fail("a mask that would make this address a network address was taken");
    }

    /* A mask refused under the classful one leaves no address. */
    set_name("eth0");
    set_ifr_addr(0);
    if (sys_ioctl(fd, SIOCSIFADDR, ifr) != 0) {
        fail("the machine could not be unconfigured");
    }
    set_name("eth0");
    set_ifr_addr(0xC0A80100UL); /* 192.168.1.0, waiting for a mask */
    if (sys_ioctl(fd, SIOCSIFADDR, ifr) != 0) {
        fail("the waiting address was refused");
    }
    set_name("eth0");
    set_ifr_addr(0xFFFFFF00UL);
    if (sys_ioctl(fd, SIOCSIFNETMASK, ifr) != -EINVAL) {
        fail("the mask that refuses that address was taken");
    }
    set_name("eth0");
    if (sys_ioctl(fd, SIOCGIFADDR, ifr) != 0 || ifr_addr() != 0) {
        fail("a refused mask left the machine on a pair nobody judged");
    }

    /* The machine as it booted, its ARP cache emptied by the changes. */
    set_name("eth0");
    set_ifr_addr(SLIRP_IP);
    if (sys_ioctl(fd, SIOCSIFADDR, ifr) != 0) {
        fail("the machine's own address did not come back");
    }
    set_name("eth0");
    set_ifr_addr(SLIRP_MASK);
    if (sys_ioctl(fd, SIOCSIFNETMASK, ifr) != 0) {
        fail("the netmask did not come back");
    }
    set_route(0, SLIRP_GW, 0);
    if (sys_ioctl(fd, SIOCADDRT, rt) != 0) {
        fail("the gateway did not come back");
    }
    if (ping_retry(SLIRP_GW) < 0) {
        fail("the gateway did not answer after the address was put back");
    }

    sys_close(conn2);
    sys_close(client2);
    sys_close(conn);
    sys_close(client);
    sys_close(listener);
    sys_close(fd);
    say("NETIFPROBE OK\n");
    sys_exit(0);
}
