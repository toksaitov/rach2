/**
 * Exercises loopback: the OS talking to itself over 127.0.0.1.
 *
 * The socket layer canonicalizes 127/8 to the stack's one address and the IP
 * layer turns a self-addressed datagram around through the loopback ring, so
 * both selves of each exchange live in this one process. A UDP datagram sent
 * to 127.0.0.1 at our own bound port comes back with the payload intact and
 * the canonical source address; a TCP connection to our own listener
 * completes its handshake through the timer-driven ring, moves data both
 * ways, and reports the canonical peer; and a connect to a closed loopback
 * port is refused promptly by the reset a matching connection does not
 * answer. Nothing here touches ARP or the wire.
 *
 * Prints "LOOPPROBE OK" and exits 0 on success. Freestanding; direct int 0x80.
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
#define SYS_SOCKETCALL 102

#define SC_SOCKET      1
#define SC_BIND        2
#define SC_CONNECT     3
#define SC_LISTEN      4
#define SC_ACCEPT      5
#define SC_GETPEERNAME 7
#define SC_SENDTO      11
#define SC_RECVFROM    12

#define AF_INET     2
#define SOCK_STREAM 1
#define SOCK_DGRAM  2

#define UDP_PORT    7100
#define TCP_PORT    7200
#define CLOSED_PORT 7300

#define ECONNREFUSED 111

static int sys_write(int fd, const void *buf, size_t count)
{
    int r;
    __asm__ __volatile__("int $0x80"
                         : "=a"(r)
                         : "0"(SYS_WRITE), "b"(fd), "c"(buf), "d"(count)
                         : "memory");
    return r;
}

static int sys_read(int fd, void *buf, size_t count)
{
    int r;
    __asm__ __volatile__("int $0x80"
                         : "=a"(r)
                         : "0"(SYS_READ), "b"(fd), "c"(buf), "d"(count)
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

static size_t strlen(const char *s)
{
    size_t n = 0;
    while (s[n])
        n++;
    return n;
}

static void print(const char *s)
{
    sys_write(STDOUT_FILENO, s, strlen(s));
}

static void fail(const char *what)
{
    print("LOOPPROBE FAIL (");
    print(what);
    print(")\n");
    sys_exit(1);
}

static int mem_eq(const uint8_t *a, const uint8_t *b, int n)
{
    int i;
    for (i = 0; i < n; i++) {
        if (a[i] != b[i])
            return 0;
    }
    return 1;
}

/* Fill a 16-byte sockaddr_in: family, big-endian port, big-endian IPv4. */
static void make_sockaddr(uint8_t *sa, uint16_t port, uint8_t a, uint8_t b, uint8_t c, uint8_t d)
{
    int i;
    for (i = 0; i < 16; i++)
        sa[i] = 0;
    sa[0] = AF_INET;
    sa[1] = 0;
    sa[2] = (uint8_t)(port >> 8);
    sa[3] = (uint8_t)port;
    sa[4] = a;
    sa[5] = b;
    sa[6] = c;
    sa[7] = d;
}

/* The canonical address every loopback peer must read back as: 10.0.2.15. */
static const uint8_t canon_ip[4] = {10, 0, 2, 15};

static void udp_loop(void)
{
    uint32_t args[6];
    uint8_t sa[16], dst[16], src[16];
    uint32_t salen = 16;
    char rbuf[32];
    const char *msg = "loopback-udp";
    int s, n;

    args[0] = AF_INET;
    args[1] = SOCK_DGRAM;
    args[2] = 0;
    s = socketcall(SC_SOCKET, args);
    if (s < 0)
        fail("udp socket");

    make_sockaddr(sa, UDP_PORT, 0, 0, 0, 0);
    args[0] = (uint32_t)s;
    args[1] = (uint32_t)sa;
    args[2] = 16;
    if (socketcall(SC_BIND, args) != 0)
        fail("udp bind");

    make_sockaddr(dst, UDP_PORT, 127, 0, 0, 1);
    args[0] = (uint32_t)s;
    args[1] = (uint32_t)msg;
    args[2] = (uint32_t)strlen(msg);
    args[3] = 0;
    args[4] = (uint32_t)dst;
    args[5] = 16;
    if (socketcall(SC_SENDTO, args) != (int)strlen(msg))
        fail("udp sendto");

    args[0] = (uint32_t)s;
    args[1] = (uint32_t)rbuf;
    args[2] = sizeof(rbuf);
    args[3] = 0;
    args[4] = (uint32_t)src;
    args[5] = (uint32_t)&salen;
    n = socketcall(SC_RECVFROM, args);
    if (n != (int)strlen(msg) || !mem_eq((const uint8_t *)rbuf, (const uint8_t *)msg, n)) {
        fail("udp payload");
    }
    if (!mem_eq(src + 4, canon_ip, 4))
        fail("udp source not canonical");
    if (src[2] != (UDP_PORT >> 8) || src[3] != (UDP_PORT & 0xFF)) {
        fail("udp source port");
    }

    sys_close(s);
}

static void tcp_loop(void)
{
    uint32_t args[6];
    uint8_t sa[16], peer[16];
    uint32_t salen = 16;
    char rbuf[32];
    const char *ping = "ping-over-loop";
    const char *pong = "pong";
    int ls, cs, as, n;

    args[0] = AF_INET;
    args[1] = SOCK_STREAM;
    args[2] = 0;
    ls = socketcall(SC_SOCKET, args);
    if (ls < 0)
        fail("tcp listener socket");

    make_sockaddr(sa, TCP_PORT, 0, 0, 0, 0);
    args[0] = (uint32_t)ls;
    args[1] = (uint32_t)sa;
    args[2] = 16;
    if (socketcall(SC_BIND, args) != 0)
        fail("tcp bind");

    args[0] = (uint32_t)ls;
    args[1] = 2;
    if (socketcall(SC_LISTEN, args) != 0)
        fail("tcp listen");

    args[0] = AF_INET;
    args[1] = SOCK_STREAM;
    args[2] = 0;
    cs = socketcall(SC_SOCKET, args);
    if (cs < 0)
        fail("tcp client socket");

    make_sockaddr(sa, TCP_PORT, 127, 0, 0, 1);
    args[0] = (uint32_t)cs;
    args[1] = (uint32_t)sa;
    args[2] = 16;
    if (socketcall(SC_CONNECT, args) != 0)
        fail("tcp connect");

    args[0] = (uint32_t)ls;
    args[1] = 0;
    args[2] = 0;
    as = socketcall(SC_ACCEPT, args);
    if (as < 0)
        fail("tcp accept");

    if (sys_write(cs, ping, strlen(ping)) != (int)strlen(ping)) {
        fail("tcp client write");
    }
    n = sys_read(as, rbuf, sizeof(rbuf));
    if (n != (int)strlen(ping) || !mem_eq((const uint8_t *)rbuf, (const uint8_t *)ping, n)) {
        fail("tcp server read");
    }

    if (sys_write(as, pong, strlen(pong)) != (int)strlen(pong)) {
        fail("tcp server write");
    }
    n = sys_read(cs, rbuf, sizeof(rbuf));
    if (n != (int)strlen(pong) || !mem_eq((const uint8_t *)rbuf, (const uint8_t *)pong, n)) {
        fail("tcp client read");
    }

    args[0] = (uint32_t)cs;
    args[1] = (uint32_t)peer;
    args[2] = (uint32_t)&salen;
    if (socketcall(SC_GETPEERNAME, args) != 0)
        fail("tcp getpeername");
    if (!mem_eq(peer + 4, canon_ip, 4))
        fail("tcp peer not canonical");
    if (peer[2] != (TCP_PORT >> 8) || peer[3] != (TCP_PORT & 0xFF)) {
        fail("tcp peer port");
    }

    sys_close(cs);
    sys_close(as);
    sys_close(ls);
}

/* A closed loopback port must refuse promptly (the RFC 793 reset), not
 * black-hole the SYN until the retransmissions give up. */
static void tcp_refused(void)
{
    uint32_t args[6];
    uint8_t sa[16];
    int cs, rc;

    args[0] = AF_INET;
    args[1] = SOCK_STREAM;
    args[2] = 0;
    cs = socketcall(SC_SOCKET, args);
    if (cs < 0)
        fail("refused socket");

    make_sockaddr(sa, CLOSED_PORT, 127, 0, 0, 1);
    args[0] = (uint32_t)cs;
    args[1] = (uint32_t)sa;
    args[2] = 16;
    rc = socketcall(SC_CONNECT, args);
    if (rc != -ECONNREFUSED)
        fail("closed port not refused");

    sys_close(cs);
}

void _start(void)
{
    udp_loop();
    tcp_loop();
    tcp_refused();
    print("LOOPPROBE OK\n");
    sys_exit(0);
}
