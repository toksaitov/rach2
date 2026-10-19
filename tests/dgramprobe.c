/**
 * dgramprobe: what a DHCP client needs of an ordinary datagram socket, a peer
 * it can be connected to and permission to broadcast.
 *
 * A client renews its lease by talking to the server it already has, which is
 * `connect` and `write` on a UDP socket, and asks for a lease in the first
 * place by broadcasting, which is `SO_BROADCAST`. What a connected socket
 * hears matters as much as what it sends: a client connected to its server
 * is not answered by whoever else is on the wire.
 *
 * The rows, in order: a socket with no peer refuses a write (EDESTADDRREQ);
 * connected, it writes to its peer and hears its peer alone, neither a third
 * socket on this machine nor the datagram the suite sends from outside at
 * "DGRAMPROBE OUTSIDE"; a connect on a descriptor that is not a socket is
 * ENOTSOCK; AF_UNSPEC disconnects a datagram socket and is EAFNOSUPPORT for a
 * stream; a packet socket cannot be connected; two sockets share a port only
 * when both set SO_REUSEADDR, and the reply goes to the one connected to the
 * sender; a port below 1024 is root's; a connect beside eight bound sockets
 * answers 0 or EAGAIN; a broadcast is EACCES until SO_BROADCAST is set; the
 * options read back; and SO_BINDTODEVICE takes the machine's interface, by
 * name or as the whole `struct ifreq` BusyBox sends, and no other.
 *
 * The peer is the machine itself at its own address, so both ends of the
 * exchanges are in this process. Prints "DGRAMPROBE OK" and exits 0 on
 * success; a failure prints "DGRAMPROBE FAIL: " with the reason and exits 1.
 * Freestanding; direct system calls (probe.h).
 */

#include "probe.h"

typedef unsigned int uint32_t;
typedef unsigned short uint16_t;
typedef unsigned char uint8_t;

#define STDOUT_FILENO 1

#define AF_INET     2
#define AF_UNSPEC   0
#define AF_PACKET   17
#define SOCK_DGRAM  2
#define SOCK_STREAM 1
#define ETH_P_IP_BE 0x0008

#define SOL_SOCKET      1
#define SO_REUSEADDR    2
#define SO_BROADCAST    6
#define SO_BINDTODEVICE 25

#define MSG_DONTWAIT 0x40

#define EACCES       13
#define EAGAIN       11
#define EDESTADDRREQ 89
#define ENODEV       19
#define ENOTSOCK     88
#define EADDRINUSE   98
#define EAFNOSUPPORT 97
#define EOPNOTSUPP   95

#define OUR_IP    0x0A00020FUL /* 10.0.2.15 */
#define BCAST_IP  0xFFFFFFFFUL
#define SUBNET_BC 0x0A0002FFUL /* 10.0.2.255 */

#define PEER_PORT    7500
#define OTHER_PORT   7501
#define OUTSIDE_PORT 7502
#define RENEW_PORT   7503

/** write(2). */
static int sys_write(int fd, const void *data, size_t count)
{
    return syscall3(SYS_WRITE, (long)fd, (long)data, (long)count);
}

/** read(2). */
static int sys_read(int fd, void *data, size_t count)
{
    return syscall3(SYS_READ, (long)fd, (long)data, (long)count);
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

/** The socket operation `call` on `args` (`sock_call`), as an int. */
static int socketcall(int call, unsigned long *args)
{
    return (int)sock_call(call, args);
}

struct timespec_probe {
    long tv_sec;
    long tv_nsec;
};

/** Sleep for `ms` milliseconds (nanosleep(2)). */
static void nap(long ms)
{
    struct timespec_probe ts;

    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (ms % 1000) * 1000000L;
    (void)syscall2(SYS_NANOSLEEP, (long)(&ts), (long)0);
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

/** Print "DGRAMPROBE FAIL: `what`" and exit 1. */
static void fail(const char *what)
{
    say("DGRAMPROBE FAIL: ");
    say(what);
    say("\n");
    sys_exit(1);
}

static uint8_t sa[16];
static uint8_t buf[64];
/* A `struct ifreq`: a 16-byte name and a union whose largest member,
 * struct ifmap, begins with two unsigned longs; 32 bytes on i386 and 40 on
 * x86-64. */
#if defined __x86_64__
#define IFREQ_SIZE 40
#else
#define IFREQ_SIZE 32
#endif
static uint8_t ifreq[IFREQ_SIZE];

/** Fill the sockaddr_in `sa` with `ip` and `port`, both in host order. */
static void set_sockaddr(uint32_t ip, uint16_t port)
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

/** A datagram socket bound to `port`: its descriptor, or -1 when the socket
 * or the bind fails. */
static int open_bound_maybe(uint16_t port)
{
    unsigned long args[6];
    int fd;

    args[0] = AF_INET;
    args[1] = SOCK_DGRAM;
    args[2] = 0;
    fd = socketcall(SC_SOCKET, args);
    if (fd < 0) {
        return -1;
    }
    set_sockaddr(0, port);
    args[0] = (uint32_t)fd;
    args[1] = (unsigned long)sa;
    args[2] = 16;
    if (socketcall(SC_BIND, args) != 0) {
        sys_close(fd);
        return -1;
    }
    return fd;
}

/** The same, failing the probe when the socket or the bind fails. */
static int open_bound(uint16_t port)
{
    unsigned long args[6];
    int fd;

    args[0] = AF_INET;
    args[1] = SOCK_DGRAM;
    args[2] = 0;
    fd = socketcall(SC_SOCKET, args);
    if (fd < 0) {
        fail("a datagram socket did not open");
    }
    set_sockaddr(0, port);
    args[0] = (uint32_t)fd;
    args[1] = (unsigned long)sa;
    args[2] = 16;
    if (socketcall(SC_BIND, args) != 0) {
        fail("a datagram socket did not bind");
    }
    return fd;
}

/**
 * The machine's count of UDP datagrams that found no socket to take them,
 * NoPorts of the Udp line of /proc/net/snmp; fails the probe when the file
 * or the line cannot be read. A datagram to a port whose only socket is
 * connected to another sender is one of them.
 */
static uint32_t udp_no_ports(void)
{
    static char text[2048];
    int fd, n = 0, r, i;
    uint32_t v = 0;

    fd = syscall3(SYS_OPEN, (long)"/proc/net/snmp", 0, 0);
    if (fd < 0) {
        fail("/proc/net/snmp did not open");
    }
    while (n < (int)sizeof(text) - 1 && (r = sys_read(fd, text + n, sizeof(text) - 1 - n)) > 0) {
        n += r;
    }
    sys_close(fd);
    text[n] = '\0';
    for (i = 0; i + 6 < n; i++) {
        if (text[i] == '\n' && text[i + 1] == 'U' && text[i + 2] == 'd' && text[i + 3] == 'p' &&
            text[i + 4] == ':' && text[i + 5] == ' ' && text[i + 6] >= '0' && text[i + 6] <= '9') {
            break;
        }
    }
    if (i + 6 >= n) {
        fail("/proc/net/snmp has no Udp counts");
    }
    for (i += 6; text[i] >= '0' && text[i] <= '9'; i++) {
    }
    for (i++; text[i] >= '0' && text[i] <= '9'; i++) {
        v = v * 10 + (uint32_t)(text[i] - '0');
    }
    return v;
}

/** Fail with `what` unless the socket `fd` has nothing to read. */
static void expect_nothing(int fd, const char *what)
{
    unsigned long args[6];

    args[0] = (uint32_t)fd;
    args[1] = (unsigned long)buf;
    args[2] = sizeof(buf);
    args[3] = MSG_DONTWAIT;
    args[4] = 0;
    args[5] = 0;
    if (socketcall(SC_RECVFROM, args) != -EAGAIN) {
        fail(what);
    }
}

/**
 * Wait until a datagram has found no socket since /proc/net/snmp counted
 * `base` of them, which is the datagram sent to `fd`, a socket connected to
 * another sender, when `fd` does not take it; `fd` must have nothing to read
 * meanwhile and then, or the probe fails with `what`. Thirty seconds without
 * the datagram fail with `missing`.
 */
static void expect_refused(uint32_t base, int fd, const char *what, const char *missing)
{
    int naps;

    for (naps = 0; udp_no_ports() == base; naps++) {
        expect_nothing(fd, what);
        if (naps == 3000) {
            fail(missing);
        }
        nap(10);
    }
    expect_nothing(fd, what);
}

/**
 * The probe's entry: the rows of the list above, in its order.
 *
 * A datagram to the machine's own address goes on the loopback ring, which
 * the kernel delivers after the send returns, so the first read of each
 * exchange waits for it, and a read that expects nothing waits first for
 * /proc/net/snmp to count the datagram as refused (`expect_refused`). The
 * socket that ignores the outsider must still hear its peer afterwards, or
 * its silence would show only that nothing was sent. The eight packet
 * sockets, opened and closed, show that none of them took a UDP binding.
 * The connect beside eight bound sockets has to find an ephemeral port, and
 * answers at once, 0 or EAGAIN.
 */
PROBE_START void _start(void)
{
    unsigned long args[6];
    int peer, other, client, outside, stream, extra, listener, sender, r, i;
    int fillers[8];
    int pid, status;
    /* The kernel writes it through the pointer getsockopt is given. */
    volatile int on;
    uint16_t client_port;
    uint32_t refused;
    /* The kernel writes it through the pointer the call is given. */
    volatile uint32_t optlen;

    peer = open_bound(PEER_PORT);
    other = open_bound(OTHER_PORT);

    args[0] = AF_INET;
    args[1] = SOCK_DGRAM;
    args[2] = 0;
    client = socketcall(SC_SOCKET, args);
    if (client < 0) {
        fail("the client socket did not open");
    }

    /* A socket with nowhere to send says so instead of sending nowhere. */
    if (sys_write(client, "x", 1) != -EDESTADDRREQ) {
        fail("a write with no peer was not refused");
    }

    /* Connected, it writes to its peer without being told again. */
    set_sockaddr(OUR_IP, PEER_PORT);
    args[0] = (uint32_t)client;
    args[1] = (unsigned long)sa;
    args[2] = 16;
    if (socketcall(SC_CONNECT, args) != 0) {
        fail("connecting a datagram socket failed");
    }
    if (sys_write(client, "hello", 5) != 5) {
        fail("a write to the connected peer failed");
    }
    args[0] = (uint32_t)peer;
    args[1] = (unsigned long)buf;
    args[2] = sizeof(buf);
    args[3] = 0;
    args[4] = 0;
    args[5] = 0;
    r = socketcall(SC_RECVFROM, args);
    if (r != 5 || buf[0] != 'h' || buf[4] != 'o') {
        fail("what the peer received is not what was written");
    }

    /* And it hears from that peer and from nobody else. */
    args[0] = (uint32_t)client;
    args[1] = (unsigned long)sa;
    optlen = 16;
    args[2] = (unsigned long)&optlen;
    if (socketcall(SC_GETSOCKNAME, args) != 0) {
        fail("the client's own address did not read");
    }
    client_port = (uint16_t)(((uint16_t)sa[2] << 8) | sa[3]);
    if (client_port == 0) {
        fail("a connected datagram socket has no source port for a reply");
    }

    set_sockaddr(OUR_IP, client_port);
    args[0] = (uint32_t)peer;
    args[1] = (unsigned long)"peer";
    args[2] = 4;
    args[3] = 0;
    args[4] = (unsigned long)sa;
    args[5] = 16;
    if (socketcall(SC_SENDTO, args) != 4) {
        fail("the peer's reply did not send");
    }
    refused = udp_no_ports();
    set_sockaddr(OUR_IP, client_port);
    args[0] = (uint32_t)other;
    args[1] = (unsigned long)"other";
    args[2] = 5;
    args[3] = 0;
    args[4] = (unsigned long)sa;
    args[5] = 16;
    if (socketcall(SC_SENDTO, args) != 5) {
        fail("the third socket's datagram did not send");
    }

    /* The peer's reply arrives, in either order, and the other's does not. */
    args[0] = (uint32_t)client;
    args[1] = (unsigned long)buf;
    args[2] = sizeof(buf);
    args[3] = 0;
    args[4] = 0;
    args[5] = 0;
    r = socketcall(SC_RECVFROM, args);
    if (r != 4 || buf[0] != 'p') {
        fail("a connected socket did not receive from its peer");
    }
    expect_refused(refused, client,
                   "a connected socket received from a sender it is not connected to",
                   "the third socket's datagram was neither received nor refused");

    /* A sender at another address: the suite's datagram, through a forward. */
    outside = open_bound(OUTSIDE_PORT);
    set_sockaddr(OUR_IP, PEER_PORT);
    args[0] = (uint32_t)outside;
    args[1] = (unsigned long)sa;
    args[2] = 16;
    if (socketcall(SC_CONNECT, args) != 0) {
        fail("connecting the socket for the outside case failed");
    }
    refused = udp_no_ports();
    say("DGRAMPROBE OUTSIDE\n");
    expect_refused(refused, outside,
                   "a connected socket received from an address that is not its peer's",
                   "the suite's datagram from outside was neither received nor refused");
    /* The same socket still hears its peer. */
    set_sockaddr(OUR_IP, OUTSIDE_PORT);
    args[0] = (uint32_t)peer;
    args[1] = (unsigned long)"peer";
    args[2] = 4;
    args[3] = 0;
    args[4] = (unsigned long)sa;
    args[5] = 16;
    if (socketcall(SC_SENDTO, args) != 4) {
        fail("the peer's datagram to the outside-case socket did not send");
    }
    args[0] = (uint32_t)outside;
    args[1] = (unsigned long)buf;
    args[2] = sizeof(buf);
    args[3] = 0;
    args[4] = 0;
    args[5] = 0;
    if (socketcall(SC_RECVFROM, args) != 4) {
        fail("the socket that ignored the outsider also ignored its peer");
    }
    sys_close(outside);

    /* The descriptor is checked before the address. */
    args[0] = 1; /* the console */
    args[1] = 0;
    args[2] = 2;
    if (socketcall(SC_CONNECT, args) != -ENOTSOCK) {
        fail("a connect on a descriptor that is not a socket reported the address instead");
    }

    /* A two-byte AF_UNSPEC address disconnects a datagram socket. */
    set_sockaddr(0, 0);
    sa[0] = AF_UNSPEC;
    args[0] = (uint32_t)client;
    args[1] = (unsigned long)sa;
    args[2] = 2;
    if (socketcall(SC_CONNECT, args) != 0) {
        fail("disconnecting a datagram socket failed");
    }
    if (sys_write(client, "x", 1) != -EDESTADDRREQ) {
        fail("a write after disconnecting was not refused");
    }

    /* A stream socket has no disconnect. */
    set_sockaddr(0, 0);
    sa[0] = AF_UNSPEC;
    args[0] = AF_INET;
    args[1] = SOCK_STREAM;
    args[2] = 0;
    stream = socketcall(SC_SOCKET, args);
    if (stream < 0) {
        fail("the stream socket did not open");
    }
    args[0] = (uint32_t)stream;
    args[1] = (unsigned long)sa;
    args[2] = 2;
    if (socketcall(SC_CONNECT, args) != -EAFNOSUPPORT) {
        fail("a stream socket accepted a two-byte disconnect");
    }
    sys_close(stream);

    /* A packet socket has no peer, and takes no UDP binding. */
    for (i = 0; i < 8; i++) {
        int pkt;

        args[0] = AF_PACKET;
        args[1] = SOCK_DGRAM;
        args[2] = ETH_P_IP_BE;
        pkt = socketcall(SC_SOCKET, args);
        if (pkt < 0) {
            fail("a packet socket did not open");
        }
        set_sockaddr(OUR_IP, PEER_PORT);
        args[0] = (uint32_t)pkt;
        args[1] = (unsigned long)sa;
        args[2] = 16;
        if (socketcall(SC_CONNECT, args) != -EOPNOTSUPP) {
            fail("a packet socket was connected like a datagram one");
        }
        sys_close(pkt);
    }
    if (open_bound(OTHER_PORT + 10) < 0) {
        fail("the UDP binding table was exhausted by packet sockets");
    }

    /* Two sockets on one port, as a DHCP client's renewal binds port 68. */
    listener = open_bound(RENEW_PORT);
    args[0] = AF_INET;
    args[1] = SOCK_DGRAM;
    args[2] = 0;
    sender = socketcall(SC_SOCKET, args);
    if (sender < 0) {
        fail("the renewal socket did not open");
    }
    set_sockaddr(0, RENEW_PORT);
    args[0] = (uint32_t)sender;
    args[1] = (unsigned long)sa;
    args[2] = 16;
    if (socketcall(SC_BIND, args) != -EADDRINUSE) {
        fail("a second socket took a bound port without asking to share it");
    }
    on = 1;
    args[0] = (uint32_t)sender;
    args[1] = SOL_SOCKET;
    args[2] = SO_REUSEADDR;
    args[3] = (unsigned long)&on;
    args[4] = sizeof(on);
    if (socketcall(SC_SETSOCKOPT, args) != 0) {
        fail("SO_REUSEADDR was refused on the renewal socket");
    }
    set_sockaddr(0, RENEW_PORT);
    args[0] = (uint32_t)sender;
    args[1] = (unsigned long)sa;
    args[2] = 16;
    if (socketcall(SC_BIND, args) != -EADDRINUSE) {
        fail("one socket asking to share was enough");
    }
    args[0] = (uint32_t)listener;
    args[1] = SOL_SOCKET;
    args[2] = SO_REUSEADDR;
    args[3] = (unsigned long)&on;
    args[4] = sizeof(on);
    if (socketcall(SC_SETSOCKOPT, args) != 0) {
        fail("SO_REUSEADDR was refused on the listener");
    }
    set_sockaddr(0, RENEW_PORT);
    args[0] = (uint32_t)sender;
    args[1] = (unsigned long)sa;
    args[2] = 16;
    if (socketcall(SC_BIND, args) != 0) {
        fail("two sockets that both asked to share a port could not");
    }

    /* The sender connects to the peer, so the peer's answer is its own. */
    set_sockaddr(OUR_IP, PEER_PORT);
    args[0] = (uint32_t)sender;
    args[1] = (unsigned long)sa;
    args[2] = 16;
    if (socketcall(SC_CONNECT, args) != 0) {
        fail("the renewal socket could not connect to the server");
    }
    if (sys_write(sender, "renew", 5) != 5) {
        fail("the renewal did not send");
    }
    args[0] = (uint32_t)peer;
    args[1] = (unsigned long)buf;
    args[2] = sizeof(buf);
    args[3] = 0;
    args[4] = 0;
    args[5] = 0;
    if (socketcall(SC_RECVFROM, args) != 5) {
        fail("the server did not receive the renewal");
    }
    set_sockaddr(OUR_IP, RENEW_PORT);
    args[0] = (uint32_t)peer;
    args[1] = (unsigned long)"ack";
    args[2] = 3;
    args[3] = 0;
    args[4] = (unsigned long)sa;
    args[5] = 16;
    if (socketcall(SC_SENDTO, args) != 3) {
        fail("the server's answer did not send");
    }
    args[0] = (uint32_t)sender;
    args[1] = (unsigned long)buf;
    args[2] = sizeof(buf);
    args[3] = 0;
    args[4] = 0;
    args[5] = 0;
    if (socketcall(SC_RECVFROM, args) != 3 || buf[0] != 'a') {
        fail("the answer went to the listener instead of the socket that asked for it");
    }
    args[0] = (uint32_t)listener;
    args[1] = (unsigned long)buf;
    args[2] = sizeof(buf);
    args[3] = MSG_DONTWAIT;
    args[4] = 0;
    args[5] = 0;
    if (socketcall(SC_RECVFROM, args) != -EAGAIN) {
        fail("the listener took a datagram the connected socket had asked for");
    }

    /* Closing the sender leaves the listener bound. */
    sys_close(sender);
    set_sockaddr(OUR_IP, RENEW_PORT);
    args[0] = (uint32_t)peer;
    args[1] = (unsigned long)"after";
    args[2] = 5;
    args[3] = 0;
    args[4] = (unsigned long)sa;
    args[5] = 16;
    if (socketcall(SC_SENDTO, args) != 5) {
        fail("the datagram after the close did not send");
    }
    args[0] = (uint32_t)listener;
    args[1] = (unsigned long)buf;
    args[2] = sizeof(buf);
    args[3] = 0;
    args[4] = 0;
    args[5] = 0;
    if (socketcall(SC_RECVFROM, args) != 5) {
        fail("closing one of two sockets on a port unbound the other");
    }
    sys_close(listener);

    /* A port below 1024 is root's: uid 1000 binding port 68 is EACCES. */
    pid = sys_fork();
    if (pid == 0) {
        int low;

        args[0] = AF_INET;
        args[1] = SOCK_DGRAM;
        args[2] = 0;
        low = socketcall(SC_SOCKET, args);
        if (low < 0 || sys_setuid(1000) != 0) {
            sys_exit(2);
        }
        set_sockaddr(0, 68);
        args[0] = (uint32_t)low;
        args[1] = (unsigned long)sa;
        args[2] = 16;
        sys_exit(socketcall(SC_BIND, args) == -EACCES ? 0 : 3);
    }
    if (pid < 0) {
        fail("the fork for the low-port case failed");
    }
    status = -1;
    if (sys_waitpid(pid, &status, 0) != pid) {
        fail("waiting for the low-port case failed");
    }
    if ((status & 0x7f) != 0) {
        fail("the low-port case died on a signal");
    }
    if (((status >> 8) & 0xff) != 0) {
        fail("an ordinary user bound a port below 1024");
    }

    /* A connect beside eight bound sockets answers 0 or EAGAIN. */
    for (i = 0; i < 8; i++) {
        fillers[i] = open_bound_maybe((uint16_t)(7600 + i));
    }
    args[0] = AF_INET;
    args[1] = SOCK_DGRAM;
    args[2] = 0;
    extra = socketcall(SC_SOCKET, args);
    if (extra >= 0) {
        set_sockaddr(OUR_IP, PEER_PORT);
        args[0] = (uint32_t)extra;
        args[1] = (unsigned long)sa;
        args[2] = 16;
        r = socketcall(SC_CONNECT, args);
        if (r != 0 && r != -EAGAIN) {
            fail("a connect with a full binding table answered with something else");
        }
        sys_close(extra);
    }
    for (i = 0; i < 8; i++) {
        if (fillers[i] >= 0) {
            sys_close(fillers[i]);
        }
    }

    /* Broadcasting is asked for, not assumed. */
    set_sockaddr(BCAST_IP, PEER_PORT);
    args[0] = (uint32_t)client;
    args[1] = (unsigned long)"b";
    args[2] = 1;
    args[3] = 0;
    args[4] = (unsigned long)sa;
    args[5] = 16;
    if (socketcall(SC_SENDTO, args) != -EACCES) {
        fail("a broadcast was sent by a socket that never asked to broadcast");
    }
    set_sockaddr(SUBNET_BC, PEER_PORT);
    args[4] = (unsigned long)sa;
    if (socketcall(SC_SENDTO, args) != -EACCES) {
        fail("the subnet's broadcast address was not treated as a broadcast");
    }

    on = 1;
    args[0] = (uint32_t)client;
    args[1] = SOL_SOCKET;
    args[2] = SO_BROADCAST;
    args[3] = (unsigned long)&on;
    args[4] = sizeof(on);
    if (socketcall(SC_SETSOCKOPT, args) != 0) {
        fail("SO_BROADCAST was refused");
    }
    set_sockaddr(BCAST_IP, PEER_PORT);
    args[0] = (uint32_t)client;
    args[1] = (unsigned long)"b";
    args[2] = 1;
    args[3] = 0;
    args[4] = (unsigned long)sa;
    args[5] = 16;
    if (socketcall(SC_SENDTO, args) != 1) {
        fail("a broadcast was refused after the socket asked to broadcast");
    }

    /* The options read back. */
    on = 0;
    args[0] = (uint32_t)client;
    args[1] = SOL_SOCKET;
    args[2] = SO_BROADCAST;
    args[3] = (unsigned long)&on;
    optlen = sizeof(on);
    args[4] = (unsigned long)&optlen;
    if (socketcall(SC_GETSOCKOPT, args) != 0 || on != 1) {
        fail("SO_BROADCAST did not read back as set");
    }
    on = 1;
    args[1] = SOL_SOCKET;
    args[2] = SO_REUSEADDR;
    args[3] = (unsigned long)&on;
    args[4] = sizeof(on);
    if (socketcall(SC_SETSOCKOPT, args) != 0) {
        fail("SO_REUSEADDR was refused");
    }
    on = 0;
    args[3] = (unsigned long)&on;
    optlen = sizeof(on);
    args[4] = (unsigned long)&optlen;
    if (socketcall(SC_GETSOCKOPT, args) != 0 || on != 1) {
        fail("SO_REUSEADDR did not read back as set");
    }

    /* SO_BINDTODEVICE takes the machine's interface and no other. */
    args[0] = (uint32_t)client;
    args[1] = SOL_SOCKET;
    args[2] = SO_BINDTODEVICE;
    args[3] = (unsigned long)"eth0";
    args[4] = 5;
    if (socketcall(SC_SETSOCKOPT, args) != 0) {
        fail("binding to this machine's interface was refused");
    }
    args[3] = (unsigned long)"eth9";
    args[4] = 5;
    if (socketcall(SC_SETSOCKOPT, args) != -ENODEV) {
        fail("binding to an interface this machine does not have was allowed");
    }
    args[3] = (unsigned long)"";
    args[4] = 1;
    if (socketcall(SC_SETSOCKOPT, args) != 0) {
        fail("an empty name did not unbind");
    }
    /* BusyBox sends a whole `struct ifreq`, with the name at the front. */
    for (i = 0; i < IFREQ_SIZE; i++) {
        ifreq[i] = 0;
    }
    ifreq[0] = 'e';
    ifreq[1] = 't';
    ifreq[2] = 'h';
    ifreq[3] = '0';
    args[3] = (unsigned long)ifreq;
    args[4] = IFREQ_SIZE;
    if (socketcall(SC_SETSOCKOPT, args) != 0) {
        fail("the whole struct ifreq a DHCP client sends was refused");
    }
    /* It reads back as the name, five bytes with the NUL. */
    for (i = 0; i < IFREQ_SIZE; i++) {
        ifreq[i] = 0x5c;
    }
    optlen = IFREQ_SIZE;
    args[3] = (unsigned long)ifreq;
    args[4] = (unsigned long)&optlen;
    if (socketcall(SC_GETSOCKOPT, args) != 0) {
        fail("the bound interface did not read back");
    }
    if (optlen != 5 || ifreq[0] != 'e' || ifreq[3] != '0' || ifreq[4] != 0) {
        fail("the bound interface did not read back as its name");
    }

    sys_close(client);
    sys_close(other);
    sys_close(peer);
    say("DGRAMPROBE OK\n");
    sys_exit(0);
}
