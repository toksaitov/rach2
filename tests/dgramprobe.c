/**
 * Exercises what a DHCP client needs of an ordinary datagram socket: a peer it
 * can be connected to, and permission to broadcast.
 *
 * A client renews its lease by talking to the server it already has, which is
 * `connect` and `write` on a UDP socket, and it asks for a lease in the first
 * place by broadcasting, which is `SO_BROADCAST`. Both were refused before.
 * What a connected socket hears is as much the point as what it sends: a
 * client connected to its server must not be answered by whoever else is on
 * the wire.
 *
 * The peer here is the machine itself over loopback, so both ends of every
 * exchange are in this one process and nothing outside is needed.
 *
 * Prints "DGRAMPROBE OK" and exits 0 on success. Freestanding; direct int 0x80.
 */

typedef unsigned int size_t;
typedef unsigned int uint32_t;
typedef unsigned short uint16_t;
typedef unsigned char uint8_t;

#define STDOUT_FILENO 1

#define SYS_EXIT       1
#define SYS_READ       3
#define SYS_WRITE      4
#define SYS_CLOSE      6
#define SYS_FORK       2
#define SYS_WAITPID    7
#define SYS_SOCKETCALL 102
#define SYS_SETUID32   213
#define SYS_NANOSLEEP  162

#define SC_SOCKET      1
#define SC_BIND        2
#define SC_CONNECT     3
#define SC_GETSOCKNAME 6
#define SC_SENDTO      11
#define SC_RECVFROM    12
#define SC_SETSOCKOPT  14
#define SC_GETSOCKOPT  15

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

static int sys_write(int fd, const void *data, size_t count)
{
    int r;
    __asm__ __volatile__("int $0x80"
                         : "=a"(r)
                         : "0"(SYS_WRITE), "b"(fd), "c"(data), "d"(count)
                         : "memory");
    return r;
}

static int sys_read(int fd, void *data, size_t count)
{
    int r;
    __asm__ __volatile__("int $0x80"
                         : "=a"(r)
                         : "0"(SYS_READ), "b"(fd), "c"(data), "d"(count)
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

static int socketcall(int call, uint32_t *args)
{
    int r;
    __asm__ __volatile__("int $0x80"
                         : "=a"(r)
                         : "0"(SYS_SOCKETCALL), "b"(call), "c"(args)
                         : "memory");
    return r;
}

struct timespec_probe {
    long tv_sec;
    long tv_nsec;
};

static void nap(long ms)
{
    struct timespec_probe ts;
    int r;

    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (ms % 1000) * 1000000L;
    __asm__ __volatile__("int $0x80" : "=a"(r) : "0"(SYS_NANOSLEEP), "b"(&ts), "c"(0) : "memory");
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

static void fail(const char *what)
{
    say("DGRAMPROBE FAIL: ");
    say(what);
    say("\n");
    sys_exit(1);
}

static uint8_t sa[16];
static uint8_t buf[64];
static uint8_t ifreq[32];

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

/** Open and bind, answering -1 instead of failing when the table is full. */
static int open_bound_maybe(uint16_t port)
{
    uint32_t args[6];
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
    args[1] = (uint32_t)sa;
    args[2] = 16;
    if (socketcall(SC_BIND, args) != 0) {
        sys_close(fd);
        return -1;
    }
    return fd;
}

static int open_bound(uint16_t port)
{
    uint32_t args[6];
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
    args[1] = (uint32_t)sa;
    args[2] = 16;
    if (socketcall(SC_BIND, args) != 0) {
        fail("a datagram socket did not bind");
    }
    return fd;
}

void _start(void)
{
    uint32_t args[6];
    int peer, other, client, outside, stream, extra, listener, sender, r, i;
    int fillers[8];
    int pid, status;
    /* Written by the kernel through a pointer, which is why it is volatile: a
       reader of this file alone sees only the assignment before the call. */
    volatile int on;
    uint16_t client_port;
    /* Written by the kernel through a pointer, which is why it is volatile: a
       reader of this file alone sees only the assignment before the call. */
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
    args[1] = (uint32_t)sa;
    args[2] = 16;
    if (socketcall(SC_CONNECT, args) != 0) {
        fail("connecting a datagram socket failed");
    }
    if (sys_write(client, "hello", 5) != 5) {
        fail("a write to the connected peer failed");
    }
    args[0] = (uint32_t)peer;
    args[1] = (uint32_t)buf;
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
    args[1] = (uint32_t)sa;
    optlen = 16;
    args[2] = (uint32_t)&optlen;
    if (socketcall(SC_GETSOCKNAME, args) != 0) {
        fail("the client's own address did not read");
    }
    client_port = (uint16_t)(((uint16_t)sa[2] << 8) | sa[3]);
    if (client_port == 0) {
        fail("a connected datagram socket has no source port for a reply");
    }

    set_sockaddr(OUR_IP, client_port);
    args[0] = (uint32_t)peer;
    args[1] = (uint32_t)"peer";
    args[2] = 4;
    args[3] = 0;
    args[4] = (uint32_t)sa;
    args[5] = 16;
    if (socketcall(SC_SENDTO, args) != 4) {
        fail("the peer's reply did not send");
    }
    set_sockaddr(OUR_IP, client_port);
    args[0] = (uint32_t)other;
    args[1] = (uint32_t)"other";
    args[2] = 5;
    args[3] = 0;
    args[4] = (uint32_t)sa;
    args[5] = 16;
    if (socketcall(SC_SENDTO, args) != 5) {
        fail("the third socket's datagram did not send");
    }

    /* The peer's reply must arrive and the other one must not. The first read
       waits, because the loopback ring is drained by the timer and neither
       datagram is delivered the moment it is sent; the rest do not, and none of
       them may find anything, whichever order the two arrived in. */
    args[0] = (uint32_t)client;
    args[1] = (uint32_t)buf;
    args[2] = sizeof(buf);
    args[3] = 0;
    args[4] = 0;
    args[5] = 0;
    r = socketcall(SC_RECVFROM, args);
    if (r != 4 || buf[0] != 'p') {
        fail("a connected socket did not receive from its peer");
    }
    for (i = 0; i < 40; i++) {
        args[0] = (uint32_t)client;
        args[1] = (uint32_t)buf;
        args[2] = sizeof(buf);
        args[3] = MSG_DONTWAIT;
        args[4] = 0;
        args[5] = 0;
        if (socketcall(SC_RECVFROM, args) != -EAGAIN) {
            fail("a connected socket received from a sender it is not connected to");
        }
        nap(10);
    }

    /* And a sender at another address, which loopback cannot produce: the
       harness sends one from outside through a host forward when it sees the
       token. The socket for it is bound to a port the forward names, and
       connected to the peer here, so what arrives is a datagram from an
       address that is not its peer's. */
    outside = open_bound(OUTSIDE_PORT);
    set_sockaddr(OUR_IP, PEER_PORT);
    args[0] = (uint32_t)outside;
    args[1] = (uint32_t)sa;
    args[2] = 16;
    if (socketcall(SC_CONNECT, args) != 0) {
        fail("connecting the socket for the outside case failed");
    }
    say("DGRAMPROBE OUTSIDE\n");
    for (i = 0; i < 60; i++) {
        args[0] = (uint32_t)outside;
        args[1] = (uint32_t)buf;
        args[2] = sizeof(buf);
        args[3] = MSG_DONTWAIT;
        args[4] = 0;
        args[5] = 0;
        if (socketcall(SC_RECVFROM, args) != -EAGAIN) {
            fail("a connected socket received from an address that is not its peer's");
        }
        nap(10);
    }
    /* And the same socket must still hear its peer, or the case above proved
       only that nothing was sent. */
    set_sockaddr(OUR_IP, OUTSIDE_PORT);
    args[0] = (uint32_t)peer;
    args[1] = (uint32_t)"peer";
    args[2] = 4;
    args[3] = 0;
    args[4] = (uint32_t)sa;
    args[5] = 16;
    if (socketcall(SC_SENDTO, args) != 4) {
        fail("the peer's datagram to the outside-case socket did not send");
    }
    args[0] = (uint32_t)outside;
    args[1] = (uint32_t)buf;
    args[2] = sizeof(buf);
    args[3] = 0;
    args[4] = 0;
    args[5] = 0;
    if (socketcall(SC_RECVFROM, args) != 4) {
        fail("the socket that ignored the outsider also ignored its peer");
    }
    sys_close(outside);

    /* A call on something that is not a socket says so, whatever is wrong with
       the address it was given: the descriptor is judged first. */
    args[0] = 1; /* the console */
    args[1] = 0;
    args[2] = 2;
    if (socketcall(SC_CONNECT, args) != -ENOTSOCK) {
        fail("a connect on a descriptor that is not a socket reported the address instead");
    }

    /* Letting go of the peer takes two bytes, which is all a family is. */
    set_sockaddr(0, 0);
    sa[0] = AF_UNSPEC;
    args[0] = (uint32_t)client;
    args[1] = (uint32_t)sa;
    args[2] = 2;
    if (socketcall(SC_CONNECT, args) != 0) {
        fail("disconnecting a datagram socket failed");
    }
    if (sys_write(client, "x", 1) != -EDESTADDRREQ) {
        fail("a write after disconnecting was not refused");
    }

    /* A stream socket has no disconnect: two bytes is a family and nothing
       else, and a connection that took them would be dialling this function's
       own stack. */
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
    args[1] = (uint32_t)sa;
    args[2] = 2;
    if (socketcall(SC_CONNECT, args) != -EAFNOSUPPORT) {
        fail("a stream socket accepted a two-byte disconnect");
    }
    sys_close(stream);

    /* And a packet socket has no peer at all. Eight of them, opened and closed,
       prove it never took a slot in the UDP binding table on the way. */
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
        args[1] = (uint32_t)sa;
        args[2] = 16;
        if (socketcall(SC_CONNECT, args) != -EOPNOTSUPP) {
            fail("a packet socket was connected like a datagram one");
        }
        sys_close(pkt);
    }
    if (open_bound(OTHER_PORT + 10) < 0) {
        fail("the UDP binding table was exhausted by packet sockets");
    }

    /* Two sockets on one port, which is what a DHCP client's renewal needs: it
       keeps one bound to port 68 to listen on and opens a second bound to 68 to
       send the renewal from. Both must ask for that, and the datagram must go
       to the one connected to the sender, not to the listener. */
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
    args[1] = (uint32_t)sa;
    args[2] = 16;
    if (socketcall(SC_BIND, args) != -EADDRINUSE) {
        fail("a second socket took a bound port without asking to share it");
    }
    on = 1;
    args[0] = (uint32_t)sender;
    args[1] = SOL_SOCKET;
    args[2] = SO_REUSEADDR;
    args[3] = (uint32_t)&on;
    args[4] = sizeof(on);
    if (socketcall(SC_SETSOCKOPT, args) != 0) {
        fail("SO_REUSEADDR was refused on the renewal socket");
    }
    set_sockaddr(0, RENEW_PORT);
    args[0] = (uint32_t)sender;
    args[1] = (uint32_t)sa;
    args[2] = 16;
    if (socketcall(SC_BIND, args) != -EADDRINUSE) {
        fail("one socket asking to share was enough");
    }
    args[0] = (uint32_t)listener;
    args[1] = SOL_SOCKET;
    args[2] = SO_REUSEADDR;
    args[3] = (uint32_t)&on;
    args[4] = sizeof(on);
    if (socketcall(SC_SETSOCKOPT, args) != 0) {
        fail("SO_REUSEADDR was refused on the listener");
    }
    set_sockaddr(0, RENEW_PORT);
    args[0] = (uint32_t)sender;
    args[1] = (uint32_t)sa;
    args[2] = 16;
    if (socketcall(SC_BIND, args) != 0) {
        fail("two sockets that both asked to share a port could not");
    }

    /* The sender connects to the peer, so the peer's answer is its own. */
    set_sockaddr(OUR_IP, PEER_PORT);
    args[0] = (uint32_t)sender;
    args[1] = (uint32_t)sa;
    args[2] = 16;
    if (socketcall(SC_CONNECT, args) != 0) {
        fail("the renewal socket could not connect to the server");
    }
    if (sys_write(sender, "renew", 5) != 5) {
        fail("the renewal did not send");
    }
    args[0] = (uint32_t)peer;
    args[1] = (uint32_t)buf;
    args[2] = sizeof(buf);
    args[3] = 0;
    args[4] = 0;
    args[5] = 0;
    if (socketcall(SC_RECVFROM, args) != 5) {
        fail("the server did not receive the renewal");
    }
    set_sockaddr(OUR_IP, RENEW_PORT);
    args[0] = (uint32_t)peer;
    args[1] = (uint32_t)"ack";
    args[2] = 3;
    args[3] = 0;
    args[4] = (uint32_t)sa;
    args[5] = 16;
    if (socketcall(SC_SENDTO, args) != 3) {
        fail("the server's answer did not send");
    }
    args[0] = (uint32_t)sender;
    args[1] = (uint32_t)buf;
    args[2] = sizeof(buf);
    args[3] = 0;
    args[4] = 0;
    args[5] = 0;
    if (socketcall(SC_RECVFROM, args) != 3 || buf[0] != 'a') {
        fail("the answer went to the listener instead of the socket that asked for it");
    }
    args[0] = (uint32_t)listener;
    args[1] = (uint32_t)buf;
    args[2] = sizeof(buf);
    args[3] = MSG_DONTWAIT;
    args[4] = 0;
    args[5] = 0;
    if (socketcall(SC_RECVFROM, args) != -EAGAIN) {
        fail("the listener took a datagram the connected socket had asked for");
    }

    /* And closing the sender leaves the listener bound. */
    sys_close(sender);
    set_sockaddr(OUR_IP, RENEW_PORT);
    args[0] = (uint32_t)peer;
    args[1] = (uint32_t)"after";
    args[2] = 5;
    args[3] = 0;
    args[4] = (uint32_t)sa;
    args[5] = 16;
    if (socketcall(SC_SENDTO, args) != 5) {
        fail("the datagram after the close did not send");
    }
    args[0] = (uint32_t)listener;
    args[1] = (uint32_t)buf;
    args[2] = sizeof(buf);
    args[3] = 0;
    args[4] = 0;
    args[5] = 0;
    if (socketcall(SC_RECVFROM, args) != 5) {
        fail("closing one of two sockets on a port unbound the other");
    }
    sys_close(listener);

    /* And a port below 1024 belongs to root, so an ordinary user cannot take
       the one a DHCP client or an SSH server answers on. */
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
        args[1] = (uint32_t)sa;
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

    /* And a connect with the binding table full is answered, not searched for.
       The table is eight slots; filling it and connecting one more must come
       back at once instead of walking sixteen thousand ports with the
       machine's interrupts off. */
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
        args[1] = (uint32_t)sa;
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
    args[1] = (uint32_t)"b";
    args[2] = 1;
    args[3] = 0;
    args[4] = (uint32_t)sa;
    args[5] = 16;
    if (socketcall(SC_SENDTO, args) != -EACCES) {
        fail("a broadcast was sent by a socket that never asked to broadcast");
    }
    set_sockaddr(SUBNET_BC, PEER_PORT);
    args[4] = (uint32_t)sa;
    if (socketcall(SC_SENDTO, args) != -EACCES) {
        fail("the subnet's broadcast address was not treated as a broadcast");
    }

    on = 1;
    args[0] = (uint32_t)client;
    args[1] = SOL_SOCKET;
    args[2] = SO_BROADCAST;
    args[3] = (uint32_t)&on;
    args[4] = sizeof(on);
    if (socketcall(SC_SETSOCKOPT, args) != 0) {
        fail("SO_BROADCAST was refused");
    }
    set_sockaddr(BCAST_IP, PEER_PORT);
    args[0] = (uint32_t)client;
    args[1] = (uint32_t)"b";
    args[2] = 1;
    args[3] = 0;
    args[4] = (uint32_t)sa;
    args[5] = 16;
    if (socketcall(SC_SENDTO, args) != 1) {
        fail("a broadcast was refused after the socket asked to broadcast");
    }

    /* And the options read back, because programs check them. */
    on = 0;
    args[0] = (uint32_t)client;
    args[1] = SOL_SOCKET;
    args[2] = SO_BROADCAST;
    args[3] = (uint32_t)&on;
    optlen = sizeof(on);
    args[4] = (uint32_t)&optlen;
    if (socketcall(SC_GETSOCKOPT, args) != 0 || on != 1) {
        fail("SO_BROADCAST did not read back as set");
    }
    on = 1;
    args[1] = SOL_SOCKET;
    args[2] = SO_REUSEADDR;
    args[3] = (uint32_t)&on;
    args[4] = sizeof(on);
    if (socketcall(SC_SETSOCKOPT, args) != 0) {
        fail("SO_REUSEADDR was refused");
    }
    on = 0;
    args[3] = (uint32_t)&on;
    optlen = sizeof(on);
    args[4] = (uint32_t)&optlen;
    if (socketcall(SC_GETSOCKOPT, args) != 0 || on != 1) {
        fail("SO_REUSEADDR did not read back as set");
    }

    /* A socket can be tied to the interface this machine has, and to no
     * other. */
    args[0] = (uint32_t)client;
    args[1] = SOL_SOCKET;
    args[2] = SO_BINDTODEVICE;
    args[3] = (uint32_t)"eth0";
    args[4] = 5;
    if (socketcall(SC_SETSOCKOPT, args) != 0) {
        fail("binding to this machine's interface was refused");
    }
    args[3] = (uint32_t)"eth9";
    args[4] = 5;
    if (socketcall(SC_SETSOCKOPT, args) != -ENODEV) {
        fail("binding to an interface this machine does not have was allowed");
    }
    args[3] = (uint32_t)"";
    args[4] = 1;
    if (socketcall(SC_SETSOCKOPT, args) != 0) {
        fail("an empty name did not unbind");
    }
    /* And the shape busybox really sends: a whole `struct ifreq`, thirty-two
       bytes of it, with the name at the front. */
    for (i = 0; i < 32; i++) {
        ifreq[i] = 0;
    }
    ifreq[0] = 'e';
    ifreq[1] = 't';
    ifreq[2] = 'h';
    ifreq[3] = '0';
    args[3] = (uint32_t)ifreq;
    args[4] = 32;
    if (socketcall(SC_SETSOCKOPT, args) != 0) {
        fail("the whole struct ifreq a DHCP client sends was refused");
    }
    /* And it reads back as the name it is bound to, not as a number. */
    for (i = 0; i < 32; i++) {
        ifreq[i] = 0x5c;
    }
    optlen = 32;
    args[3] = (uint32_t)ifreq;
    args[4] = (uint32_t)&optlen;
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
