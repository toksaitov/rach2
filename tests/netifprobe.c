/**
 * Exercises the interface ioctls: how a program asks about this machine's
 * network and how it changes it.
 *
 * A DHCP client learns the address after the machine is running, so there has
 * to be a way for a program to set one, and this is Linux's way: `SIOCxIF*` on
 * a socket with a `struct ifreq`. What matters as much as the setting is what
 * a change costs. Every ARP mapping was learned by a machine at the old
 * address and every open connection recorded it, so this probe holds a TCP
 * connection open across the change and requires it to be aborted rather than
 * left sending from an address the machine no longer has.
 *
 * The machine's address is put back at the end and the gateway pinged, which
 * is what proves the change was survivable rather than merely accepted.
 *
 * Run on a machine booted with `rach2.ip=none` it does the other half instead:
 * configures an unconfigured machine in the order `ifconfig` does it, an
 * address then a mask then a route, which is the sequence a DHCP client runs
 * and the one a machine that refused to hold an address without a mask could
 * never complete.
 *
 * Prints "NETIFPROBE OK" and exits 0 on success. Freestanding; direct int 0x80.
 */

typedef unsigned int size_t;
typedef unsigned int uint32_t;
typedef unsigned short uint16_t;
typedef unsigned char uint8_t;

#define STDOUT_FILENO 1

#define SYS_EXIT       1
#define SYS_FORK       2
#define SYS_WRITE      4
#define SYS_CLOSE      6
#define SYS_WAITPID    7
#define SYS_IOCTL      54
#define SYS_READ       3
#define SYS_POLL       168
#define SYS_SOCKETCALL 102
#define SYS_NANOSLEEP  162
#define SYS_SETUID32   213

#define SC_SOCKET     1
#define SC_BIND       2
#define SC_CONNECT    3
#define SC_LISTEN     4
#define SC_ACCEPT     5
#define SC_RECV       10
#define SC_GETSOCKOPT 15
#define SC_PING       0x1000

#define AF_INET     2
#define SOCK_STREAM 1
#define SOCK_DGRAM  2
#define SOCK_RAW    3
#define IPPROTO_RAW 255

#define SOL_SOCKET 1
#define SO_ERROR   4

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

#define RTF_UP      0x0001
#define RTF_GATEWAY 0x0002

#define EPERM           1
#define EINVAL          22
#define ENODEV          19
#define ENETRESET       102
#define ESRCH           3
#define EPROTONOSUPPORT 93
#define EOPNOTSUPP      95

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

static int sys_write(int fd, const void *buf, size_t count)
{
    int r;
    __asm__ __volatile__("int $0x80"
                         : "=a"(r)
                         : "0"(SYS_WRITE), "b"(fd), "c"(buf), "d"(count)
                         : "memory");
    return r;
}

struct pollfd_probe {
    int fd;
    short events;
    short revents;
};

static int sys_read(int fd, void *buf, size_t count)
{
    int r;
    __asm__ __volatile__("int $0x80"
                         : "=a"(r)
                         : "0"(SYS_READ), "b"(fd), "c"(buf), "d"(count)
                         : "memory");
    return r;
}

static int sys_poll(struct pollfd_probe *fds, unsigned int n, int timeout)
{
    int r;
    __asm__ __volatile__("int $0x80"
                         : "=a"(r)
                         : "0"(SYS_POLL), "b"(fds), "c"(n), "d"(timeout)
                         : "memory");
    return r;
}

static int sys_close(int fd)
{
    int r;
    __asm__ __volatile__("int $0x80" : "=a"(r) : "0"(SYS_CLOSE), "b"(fd));
    return r;
}

static void sys_exit(int status)
{
    __asm__ __volatile__("int $0x80" : : "a"(SYS_EXIT), "b"(status));
    __builtin_unreachable();
}

static int sys_ioctl(int fd, uint32_t request, void *arg)
{
    int r;
    __asm__ __volatile__("int $0x80"
                         : "=a"(r)
                         : "0"(SYS_IOCTL), "b"(fd), "c"(request), "d"(arg)
                         : "memory");
    return r;
}

static int socketcall(int call, uint32_t *args)
{
    int r;
    __asm__ __volatile__("int $0x80"
                         : "=a"(r)
                         : "0"(SYS_SOCKETCALL), "b"(call), "c"(args)
                         : "memory");
    return r;
}

static int sys_fork(void)
{
    int r;
    __asm__ __volatile__("int $0x80" : "=a"(r) : "0"(SYS_FORK) : "memory");
    return r;
}

static int sys_setuid(int uid)
{
    int r;
    __asm__ __volatile__("int $0x80" : "=a"(r) : "0"(SYS_SETUID32), "b"(uid));
    return r;
}

static int sys_waitpid(int pid, int *status, int options)
{
    int r;
    __asm__ __volatile__("int $0x80"
                         : "=a"(r)
                         : "0"(SYS_WAITPID), "b"(pid), "c"(status), "d"(options)
                         : "memory");
    return r;
}

static void say(const char *s)
{
    size_t n = 0;

    while (s[n]) {
        n++;
    }
    sys_write(STDOUT_FILENO, s, n);
}

/*
 * Ping `ip`, retrying a few times.
 *
 * The first echo after an ARP cache is emptied is dropped while the request
 * for the peer's hardware address is still in flight, so one attempt says
 * nothing about whether the machine can reach anything.
 */
static int ping_retry(uint32_t ip)
{
    uint32_t args[6];
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

static void fail(const char *what)
{
    say("NETIFPROBE FAIL: ");
    say(what);
    say("\n");
    sys_exit(1);
}

/* struct ifreq: a 16-byte name, then a union. */
static uint8_t ifr[32];
/* struct rtentry: a pad word, three sockaddrs, the flags at 52, and a pointer
   to the interface's name at 68. */
static uint8_t rt[84];
static const char other_dev[] = "eth9";
static uint8_t sa_listen[16];
static uint8_t sa_connect[16];

static void set_name(const char *name)
{
    int i;

    size_t n = 0;

    while (name[n]) {
        n++;
    }
    for (i = 0; i < 32; i++) {
        ifr[i] = 0;
    }
    if (n > 15) {
        fail("an interface name longer than the field it goes in");
    }
    for (i = 0; (size_t)i < n; i++) {
        ifr[i] = (uint8_t)name[i];
    }
}

static uint32_t ifr_addr(void)
{
    return ((uint32_t)ifr[20] << 24) | ((uint32_t)ifr[21] << 16) | ((uint32_t)ifr[22] << 8) |
           (uint32_t)ifr[23];
}

static void set_ifr_addr(uint32_t ip)
{
    ifr[16] = AF_INET;
    ifr[20] = (uint8_t)(ip >> 24);
    ifr[21] = (uint8_t)(ip >> 16);
    ifr[22] = (uint8_t)(ip >> 8);
    ifr[23] = (uint8_t)ip;
}

/** A default route through `gw`, or a route to `dst`/`mask` with no gateway. */
static void set_route(uint32_t dst, uint32_t gw, uint32_t mask)
{
    int i;

    for (i = 0; i < 84; i++) {
        rt[i] = 0;
    }
    rt[4] = AF_INET;
    rt[8] = (uint8_t)(dst >> 24);
    rt[9] = (uint8_t)(dst >> 16);
    rt[10] = (uint8_t)(dst >> 8);
    rt[11] = (uint8_t)dst;
    rt[20] = AF_INET;
    rt[24] = (uint8_t)(gw >> 24);
    rt[25] = (uint8_t)(gw >> 16);
    rt[26] = (uint8_t)(gw >> 8);
    rt[27] = (uint8_t)gw;
    rt[36] = AF_INET;
    rt[40] = (uint8_t)(mask >> 24);
    rt[41] = (uint8_t)(mask >> 16);
    rt[42] = (uint8_t)(mask >> 8);
    rt[43] = (uint8_t)mask;
    rt[52] = (uint8_t)((RTF_UP | (gw ? RTF_GATEWAY : 0)) & 0xff);
}

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

/*
 * Configure a machine that came up with nothing, in the order `ifconfig` does
 * it: an address, then a mask, then a route. Between the first two the machine
 * holds an address with the classful mask its first byte implies, which is the
 * state a machine that refused to hold it could never get out of.
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

void _start(void)
{
    uint32_t args[6];
    int fd, listener, client, conn, client2, conn2, pid, status;
    int inbound, tries, got_inbound, r;
    /* Written by the kernel through a pointer, which is why it is volatile: a
       reader of this file alone sees only the assignment before the call. */
    volatile int err;
    struct pollfd_probe pfd;
    uint32_t optlen;

    /* The descriptor a DHCP client asks through is a raw one: busybox's
       `udhcp_read_interface` opens AF_INET/SOCK_RAW/IPPROTO_RAW and does
       nothing with it but these ioctls. So that is what this uses. */
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
    /* And the request that runs the other way: an index in, a name out. */
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
    /* A machine booted with `rach2.ip=none` has no address, and configuring it
       from nothing is the sequence a DHCP client runs. It is a different boot,
       so it is a different half of this probe. */
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

    /* The query descriptor carries ioctls and refuses everything else, rather
       than answering with a silence a caller would wait in. */
    args[0] = (uint32_t)fd;
    args[1] = (uint32_t)sa_listen;
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

    /* A connection open across the change. It is to this machine itself, so
       both ends are here and both recorded the address being changed. */
    args[0] = AF_INET;
    args[1] = SOCK_STREAM;
    args[2] = 0;
    listener = socketcall(SC_SOCKET, args);
    if (listener < 0) {
        fail("the listener did not open");
    }
    set_sockaddr(sa_listen, 0, LOOP_PORT);
    args[0] = (uint32_t)listener;
    args[1] = (uint32_t)sa_listen;
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
    args[1] = (uint32_t)sa_connect;
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

    /* The connection opened from the old address must be gone, and every way of
       finding that out must say why. A bare failure reads as a permission error
       in the C library, which is not what happened to a program whose machine
       changed address under it. Each end holds the reason once, and the first
       reader takes it, as `SO_ERROR` does on Linux, so each check below uses an
       end whose reason nothing has taken yet. */
    if (sys_write(conn, ifr, 1) != -ENETRESET) {
        fail("a write to the aborted connection did not report why it ended");
    }
    if (sys_read(client, ifr, 1) != -ENETRESET) {
        fail("a read of the aborted connection did not report why it ended");
    }
    args[0] = (uint32_t)conn;
    args[1] = SOL_SOCKET;
    args[2] = SO_ERROR;
    args[3] = (uint32_t)&err;
    optlen = sizeof(err);
    args[4] = (uint32_t)&optlen;
    err = -1;
    if (socketcall(SC_GETSOCKOPT, args) != 0) {
        fail("the connection's error did not read");
    }
    if (err != 0) {
        fail("the reason was reported twice, so nothing took it");
    }

    /* And the listener must have survived it. A listener is not a connection:
       it answers whatever address this machine has, so a server started before
       a lease arrived must still be accepting after it. */
    args[0] = AF_INET;
    args[1] = SOCK_STREAM;
    args[2] = 0;
    client2 = socketcall(SC_SOCKET, args);
    if (client2 < 0) {
        fail("the second client did not open");
    }
    set_sockaddr(sa_connect, OTHER_IP, LOOP_PORT);
    args[0] = (uint32_t)client2;
    args[1] = (uint32_t)sa_connect;
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

    /* And reapplying the pair the machine already holds must not disturb the
       connection: a DHCP client runs the same `ifconfig` on every renewal, and
       a machine that reset its sessions each time would drop them at the lease
       interval. The connection accepted a moment ago is the witness. */
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
    args[3] = (uint32_t)&err;
    optlen = sizeof(err);
    args[4] = (uint32_t)&optlen;
    err = -1;
    if (socketcall(SC_GETSOCKOPT, args) != 0) {
        fail("the surviving connection's error did not read");
    }
    if (err != 0) {
        fail("reapplying the same address reset a connection");
    }

    /* An address ioctl brings its own mask. Moving from a class C address to a
       class B one must leave a class B mask, not the /24 that was there. */
    set_name("eth0");
    set_ifr_addr(0xAC100102UL); /* 172.16.1.2 */
    if (sys_ioctl(fd, SIOCSIFADDR, ifr) != 0) {
        fail("an address in another class did not set");
    }
    set_name("eth0");
    if (sys_ioctl(fd, SIOCGIFNETMASK, ifr) != 0 || ifr_addr() != 0xFFFF0000UL) {
        fail("an address in another class kept the mask of the one before it");
    }

    /* That was a second address change with the first connection's descriptor
       still open. Its reason was taken already, and a change that aborts
       nothing must not put another one there. */
    args[0] = (uint32_t)conn;
    args[1] = SOL_SOCKET;
    args[2] = SO_ERROR;
    args[3] = (uint32_t)&err;
    optlen = sizeof(err);
    args[4] = (uint32_t)&optlen;
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

    /* Down means down in both directions. The gateway answers a ping while the
       interface is up, and must not while it is down: a machine whose sockets
       saw traffic it could not answer would be worse than one that is quiet. */
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
    /* A socket for what the harness will send from outside. Down must mean the
       machine does not receive either, and the only way to prove that is to
       have something else put a datagram on the wire: the harness sends one
       through a host forward when it sees the token below. */
    args[0] = AF_INET;
    args[1] = SOCK_DGRAM;
    args[2] = 0;
    inbound = socketcall(SC_SOCKET, args);
    if (inbound < 0) {
        fail("the socket for the inbound case did not open");
    }
    set_sockaddr(sa_listen, 0, INBOUND_PORT);
    args[0] = (uint32_t)inbound;
    args[1] = (uint32_t)sa_listen;
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

    /* Now the other direction. The token asks the harness for a datagram, and
       nothing must arrive while the interface is down. */
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

    /* And with the interface up the same datagram must arrive, or the case
       above proved only that the harness never sent one. */
    /* The second connection was open across this address change too, so the
       nonblocking read path can be held to the same contract on it. */
    args[0] = (uint32_t)conn2;
    args[1] = (uint32_t)ifr;
    args[2] = 1;
    args[3] = 0x40; /* MSG_DONTWAIT */
    if (socketcall(SC_RECV, args) != -ENETRESET) {
        fail("a nonblocking read of the aborted connection did not report why it ended");
    }

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

    /* Routes. A default route sets the gateway; one off the subnet is not a
     * gateway; a route this stack cannot hold is refused rather than forgotten.
     */
    set_route(0, OTHER_GW, 0);
    if (sys_ioctl(fd, SIOCADDRT, rt) != 0) {
        fail("a default route was not taken");
    }
    set_route(0, SLIRP_GW, 0);
    if (sys_ioctl(fd, SIOCADDRT, rt) != -EINVAL) {
        fail("a gateway off the subnet was taken");
    }
    set_route(0x0A0A0A00UL, 0, 0xFFFFFF00UL);
    if (sys_ioctl(fd, SIOCADDRT, rt) != -EOPNOTSUPP) {
        fail("a route this stack cannot hold was not refused");
    }
    /* A default route with no gateway is one this stack cannot hold, and it
       must not be taken as an instruction to remove the one it has: a machine
       across the room is reached through that gateway. */
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

    /* A route names a device, and one this machine does not have must not
       change the route of the one it does. */
    set_route(0, SLIRP_GW, 0);
    rt[68] = (uint8_t)((uint32_t)(size_t)other_dev & 0xff);
    rt[69] = (uint8_t)(((uint32_t)(size_t)other_dev >> 8) & 0xff);
    rt[70] = (uint8_t)(((uint32_t)(size_t)other_dev >> 16) & 0xff);
    rt[71] = (uint8_t)(((uint32_t)(size_t)other_dev >> 24) & 0xff);
    if (sys_ioctl(fd, SIOCADDRT, rt) != -ENODEV) {
        fail("a route for an interface this machine does not have was taken");
    }
    set_route(0, 0, 0);
    rt[68] = (uint8_t)((uint32_t)(size_t)other_dev & 0xff);
    rt[69] = (uint8_t)(((uint32_t)(size_t)other_dev >> 8) & 0xff);
    rt[70] = (uint8_t)(((uint32_t)(size_t)other_dev >> 16) & 0xff);
    rt[71] = (uint8_t)(((uint32_t)(size_t)other_dev >> 24) & 0xff);
    if (sys_ioctl(fd, SIOCDELRT, rt) != -ENODEV) {
        fail("a removal for an interface this machine does not have was taken");
    }

    /* The subnet this machine is on is reachable by being on it, so adding that
       route is asking for what is true; removing it is not something this stack
       can do, and saying it did would be telling an administrator that traffic
       was isolated when it was not. */
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
    /* And removing it again must fail. A DHCP client's script removes old
       default routes in a loop that ends when the removal does, so a removal
       that always succeeds is a machine that never gets past it. */
    if (sys_ioctl(fd, SIOCDELRT, rt) != -ESRCH) {
        fail("removing a route that is not there succeeded");
    }

    /* A lease whose address is a network or broadcast address under the
       classful mask but an ordinary host under the mask it comes with. The
       address ioctl must take it and let the mask that follows settle it. */
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

    /* And a mask refused while the classful one is still standing in leaves the
       machine unconfigured, rather than answering for a network address that
       nothing judged. */
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

    /* Put the machine back where it was, and prove it still works: the ARP
     * cache was emptied by the change, so this exchange had to learn it again.
     */
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
