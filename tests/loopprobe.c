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
#define SYS_NANOSLEEP  162
#define SYS_FCNTL64    221

#define SC_SOCKET      1
#define SC_BIND        2
#define SC_CONNECT     3
#define SC_LISTEN      4
#define SC_ACCEPT      5
#define SC_GETPEERNAME 7
#define SC_SENDTO      11
#define MSG_DONTWAIT   0x40
#define EAGAIN         11
#define EDESTADDRREQ   89

#define SC_RECVFROM 12

#define AF_INET     2
#define SOCK_STREAM 1
#define SOCK_DGRAM  2

#define UDP_PORT 7100

/* The ports the bound-port case uses, distinct from every other case here. */
#define BOUND_LISTEN_PORT 7300
#define BOUND_CLIENT_PORT 7301

/* The two ports the simultaneous-open case dials at each other. */
#define SIM_PORT_A  7310
#define SIM_PORT_B  7311
#define SIM_PORT_C  7312
#define SIM_PORT_D  7313
#define TCP_PORT    7200
#define CLOSED_PORT 7300

#define EADDRINUSE  98
#define EINPROGRESS 115
#define EISCONN     106

#define F_SETFL      4
#define O_NONBLOCK   04000
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

static int sys_fcntl(int fd, int cmd, int arg)
{
    int r;
    __asm__ __volatile__("int $0x80" : "=a"(r) : "0"(SYS_FCNTL64), "b"(fd), "c"(cmd), "d"(arg));
    return r;
}

struct timespec_probe {
    long tv_sec;
    long tv_nsec;
};

static void sys_nanosleep_ms(long ms)
{
    struct timespec_probe req;
    int r;

    req.tv_sec = ms / 1000L;
    req.tv_nsec = (ms % 1000L) * 1000000L;
    __asm__ __volatile__("int $0x80" : "=a"(r) : "0"(SYS_NANOSLEEP), "b"(&req), "c"(0) : "memory");
    (void)r;
}

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

    /* A datagram of no bytes is a datagram. UDP carries an empty payload, and a
     * receive slot that read its own emptiness as "nothing here" would build
     * this one, send it, accept it, deliver it to the right socket, and drop it
     * at the last step, leaving the recvfrom below blocked forever. A hang here
     * is a detection too: the runner reports a probe that never returns. */
    args[0] = (uint32_t)s;
    args[1] = (uint32_t)msg;
    args[2] = 0;
    args[3] = 0;
    args[4] = (uint32_t)dst;
    args[5] = 16;
    if (socketcall(SC_SENDTO, args) != 0)
        fail("udp empty sendto");

    args[0] = (uint32_t)s;
    args[1] = (uint32_t)rbuf;
    args[2] = sizeof(rbuf);
    args[3] = 0;
    args[4] = (uint32_t)src;
    args[5] = (uint32_t)&salen;
    salen = 16;
    n = socketcall(SC_RECVFROM, args);
    if (n != 0)
        fail("udp empty payload");
    if (!mem_eq(src + 4, canon_ip, 4))
        fail("udp empty source not canonical");
    if (src[2] != (UDP_PORT >> 8) || src[3] != (UDP_PORT & 0xFF)) {
        fail("udp empty source port");
    }

    /* And the slot was cleared by that read, not merely reported: the next
     * datagram must arrive whole. */
    args[0] = (uint32_t)s;
    args[1] = (uint32_t)msg;
    args[2] = (uint32_t)strlen(msg);
    args[3] = 0;
    args[4] = (uint32_t)dst;
    args[5] = 16;
    if (socketcall(SC_SENDTO, args) != (int)strlen(msg))
        fail("udp sendto after empty");

    args[0] = (uint32_t)s;
    args[1] = (uint32_t)rbuf;
    args[2] = sizeof(rbuf);
    args[3] = 0;
    args[4] = (uint32_t)src;
    args[5] = (uint32_t)&salen;
    salen = 16;
    n = socketcall(SC_RECVFROM, args);
    if (n != (int)strlen(msg) || !mem_eq((const uint8_t *)rbuf, (const uint8_t *)msg, n)) {
        fail("udp payload after empty");
    }

    /* And an empty slot is still empty. This is the assertion a fix that made
     * the slot unconditionally ready would fail while passing both above. */
    args[0] = (uint32_t)s;
    args[1] = (uint32_t)rbuf;
    args[2] = sizeof(rbuf);
    args[3] = MSG_DONTWAIT;
    args[4] = (uint32_t)src;
    args[5] = (uint32_t)&salen;
    salen = 16;
    n = socketcall(SC_RECVFROM, args);
    if (n != -EAGAIN)
        fail("udp drained socket not empty");

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

/*
 * The local port a client bound, and the exclusivity a listener holds.
 *
 * `bind` before `connect` is how a client pins its source port, and a
 * `tcp_connect` that allocated a fresh ephemeral regardless made that
 * impossible: the peer saw a port nobody asked for. What must still be refused
 * is only the exact triple the connection table keys on, so a second connection
 * from the same bound port to a different peer port is legal and a repeat of
 * the same pairing is not.
 *
 * Listener exclusivity is checked where it is published rather than at bind,
 * because binding both sockets first and listening afterwards walks past any
 * check that lives only in bind; both orders are tried here.
 */
static void tcp_ports(void)
{
    uint32_t args[6];
    uint8_t sa[16], peer[16];
    uint32_t salen = 16;
    int ls, cs, as, second;

    /* A listener to dial, and a second socket bound to the same port BEFORE
     * either listens, which is the order that walks past a check living only
     * in bind. */
    args[0] = AF_INET;
    args[1] = SOCK_STREAM;
    args[2] = 0;
    ls = socketcall(SC_SOCKET, args);
    if (ls < 0)
        fail("bound-port listen socket");

    make_sockaddr(sa, BOUND_LISTEN_PORT, 0, 0, 0, 0);
    args[0] = (uint32_t)ls;
    args[1] = (uint32_t)sa;
    args[2] = 16;
    if (socketcall(SC_BIND, args) != 0)
        fail("bound-port listen bind");

    args[0] = AF_INET;
    args[1] = SOCK_STREAM;
    args[2] = 0;
    second = socketcall(SC_SOCKET, args);
    if (second < 0)
        fail("second listener socket");
    args[0] = (uint32_t)second;
    args[1] = (uint32_t)sa;
    args[2] = 16;
    if (socketcall(SC_BIND, args) != 0)
        fail("a second bind before any listen was refused");

    args[0] = (uint32_t)ls;
    args[1] = 4;
    if (socketcall(SC_LISTEN, args) != 0)
        fail("bound-port listen");

    /* Both are bound; only one may listen. */
    args[0] = (uint32_t)second;
    args[1] = 4;
    if (socketcall(SC_LISTEN, args) != -EADDRINUSE)
        fail("second listener on one port not refused");
    sys_close(second);

    /* And once a listener is live, bind says so too, which is where a server
     * expects to hear it. That check is the courtesy; the one above is the
     * invariant. */
    args[0] = AF_INET;
    args[1] = SOCK_STREAM;
    args[2] = 0;
    second = socketcall(SC_SOCKET, args);
    if (second < 0)
        fail("late bind socket");
    args[0] = (uint32_t)second;
    args[1] = (uint32_t)sa;
    args[2] = 16;
    if (socketcall(SC_BIND, args) != -EADDRINUSE)
        fail("binding a live listener's port was not reported");
    sys_close(second);

    /* A listener with no bound port at all: this stack assigns none, so port 0
     * would be a listener no segment could ever reach. */
    args[0] = AF_INET;
    args[1] = SOCK_STREAM;
    args[2] = 0;
    second = socketcall(SC_SOCKET, args);
    if (second < 0)
        fail("unbound listener socket");
    args[0] = (uint32_t)second;
    args[1] = 4;
    if (socketcall(SC_LISTEN, args) != -EDESTADDRREQ)
        fail("unbound listener not refused");
    sys_close(second);

    /* A server restarted on a port its own last session still holds. `tcp_loop`
     * above ran a whole connection on TCP_PORT and closed it, so that port is
     * held by a TIME_WAIT remnant right now. Refusing on any in-use block
     * rather than on a live listener would fail this, which is what restarting
     * dropbear or httpd after a session looks like; `setsockopt` is a no-op
     * here, so `SO_REUSEADDR` would not buy the way out. */
    args[0] = AF_INET;
    args[1] = SOCK_STREAM;
    args[2] = 0;
    second = socketcall(SC_SOCKET, args);
    if (second < 0)
        fail("restart socket");
    make_sockaddr(sa, TCP_PORT, 0, 0, 0, 0);
    args[0] = (uint32_t)second;
    args[1] = (uint32_t)sa;
    args[2] = 16;
    if (socketcall(SC_BIND, args) != 0)
        fail("a port a closed session still holds could not be bound");
    args[0] = (uint32_t)second;
    args[1] = 4;
    if (socketcall(SC_LISTEN, args) != 0)
        fail("a server could not restart on its own last session's port");
    sys_close(second);

    /* And the client, which pins its own source port. */
    args[0] = AF_INET;
    args[1] = SOCK_STREAM;
    args[2] = 0;
    cs = socketcall(SC_SOCKET, args);
    if (cs < 0)
        fail("bound-port client socket");

    make_sockaddr(sa, BOUND_CLIENT_PORT, 0, 0, 0, 0);
    args[0] = (uint32_t)cs;
    args[1] = (uint32_t)sa;
    args[2] = 16;
    if (socketcall(SC_BIND, args) != 0)
        fail("bound-port client bind");

    make_sockaddr(sa, BOUND_LISTEN_PORT, 127, 0, 0, 1);
    args[0] = (uint32_t)cs;
    args[1] = (uint32_t)sa;
    args[2] = 16;
    if (socketcall(SC_CONNECT, args) != 0)
        fail("bound-port connect");

    args[0] = (uint32_t)ls;
    args[1] = (uint32_t)peer;
    args[2] = (uint32_t)&salen;
    salen = 16;
    as = socketcall(SC_ACCEPT, args);
    if (as < 0)
        fail("bound-port accept");

    /* The port the server sees is the one the client asked for, which is the
     * whole assertion: an ephemeral would be some other number. */
    if (peer[2] != (BOUND_CLIENT_PORT >> 8) || peer[3] != (BOUND_CLIENT_PORT & 0xFF)) {
        fail("the peer port is not the one the client bound");
    }

    /* A second connection from that same port to the same peer is the one
     * duplicate the table cannot tell apart. */
    args[0] = AF_INET;
    args[1] = SOCK_STREAM;
    args[2] = 0;
    second = socketcall(SC_SOCKET, args);
    if (second < 0)
        fail("duplicate pairing socket");
    make_sockaddr(sa, BOUND_CLIENT_PORT, 0, 0, 0, 0);
    args[0] = (uint32_t)second;
    args[1] = (uint32_t)sa;
    args[2] = 16;
    if (socketcall(SC_BIND, args) != 0)
        fail("duplicate pairing bind");
    make_sockaddr(sa, BOUND_LISTEN_PORT, 127, 0, 0, 1);
    args[0] = (uint32_t)second;
    args[1] = (uint32_t)sa;
    args[2] = 16;
    if (socketcall(SC_CONNECT, args) != -EADDRINUSE)
        fail("a repeated local-remote pairing was not refused");
    sys_close(second);

    sys_close(as);
    sys_close(cs);
    sys_close(ls);
}

/*
 * Simultaneous open (RFC 793): both ends send a SYN and neither has seen an
 * ACK. It is reachable here because loopback is the same machine: two sockets,
 * each bound to the port the other dials, so each SYN arrives at a socket that
 * has already sent its own.
 *
 * A SYN with no ACK used to fall out of SYN_SENT and be dropped, so both
 * connects hung until their retransmissions gave up. The state machine now
 * answers with a SYN-ACK emitted at `iss`, which is the sequence the peer is
 * waiting on; an implementation that emitted at `iss + 1` instead would leave
 * both ends acknowledging nothing, and this case would hang rather than fail,
 * which the runner reports as a probe that never returned.
 */
static void tcp_simultaneous(void)
{
    uint32_t args[6];
    uint8_t sa[16];
    int a, b, rc, spin;
    const char *msg = "simultaneous";
    char rbuf[32];
    int n;

    args[0] = AF_INET;
    args[1] = SOCK_STREAM;
    args[2] = 0;
    a = socketcall(SC_SOCKET, args);
    if (a < 0)
        fail("simultaneous socket a");
    args[0] = AF_INET;
    args[1] = SOCK_STREAM;
    args[2] = 0;
    b = socketcall(SC_SOCKET, args);
    if (b < 0)
        fail("simultaneous socket b");

    make_sockaddr(sa, SIM_PORT_A, 0, 0, 0, 0);
    args[0] = (uint32_t)a;
    args[1] = (uint32_t)sa;
    args[2] = 16;
    if (socketcall(SC_BIND, args) != 0)
        fail("simultaneous bind a");
    make_sockaddr(sa, SIM_PORT_B, 0, 0, 0, 0);
    args[0] = (uint32_t)b;
    args[1] = (uint32_t)sa;
    args[2] = 16;
    if (socketcall(SC_BIND, args) != 0)
        fail("simultaneous bind b");

    /* Both non-blocking, which is the whole of how two SYNs get to be in
     * flight at once from one process: a blocking connect would sit inside the
     * first handshake, and its SYN would reach a port whose socket has no TCB
     * yet and earn a reset instead of a crossing SYN. */
    if (sys_fcntl(a, F_SETFL, O_NONBLOCK) < 0 || sys_fcntl(b, F_SETFL, O_NONBLOCK) < 0)
        fail("simultaneous nonblock");

    make_sockaddr(sa, SIM_PORT_B, 127, 0, 0, 1);
    args[0] = (uint32_t)a;
    args[1] = (uint32_t)sa;
    args[2] = 16;
    rc = socketcall(SC_CONNECT, args);
    if (rc != -EINPROGRESS && rc != 0)
        fail("simultaneous connect a");

    make_sockaddr(sa, SIM_PORT_A, 127, 0, 0, 1);
    args[0] = (uint32_t)b;
    args[1] = (uint32_t)sa;
    args[2] = 16;
    rc = socketcall(SC_CONNECT, args);
    if (rc != -EINPROGRESS && rc != 0)
        fail("simultaneous connect b");

    /* Both ends must come up. A SYN that fell out of SYN_SENT left both
     * hanging, so this loop is the assertion and its bound is the failure. */
    for (spin = 0; spin < 400; spin++) {
        args[0] = (uint32_t)a;
        args[1] = (uint32_t)sa;
        args[2] = 16;
        if (socketcall(SC_CONNECT, args) == -EISCONN) {
            break;
        }
        sys_nanosleep_ms(20);
    }
    if (spin >= 400)
        fail("the crossing SYNs never established");

    /* And a BLOCKING connect must return 0 through the same path. What this
     * does NOT gate is the wake: the loopback ring is drained by the timer,
     * which fires while the caller is still inside its own `connect`, so the
     * handshake usually finishes before the wait predicate is first evaluated
     * and no wake is needed. Over a real interface the segments arrive
     * milliseconds later and the caller certainly blocks, which is why the
     * completion path must wake an active opener rather than queue it on a
     * listener that does not exist; that half is argued in the network Readme
     * and not proved here. */
    sys_close(a);
    sys_close(b);
    args[0] = AF_INET;
    args[1] = SOCK_STREAM;
    args[2] = 0;
    a = socketcall(SC_SOCKET, args);
    args[0] = AF_INET;
    args[1] = SOCK_STREAM;
    args[2] = 0;
    b = socketcall(SC_SOCKET, args);
    if (a < 0 || b < 0)
        fail("blocking simultaneous sockets");
    make_sockaddr(sa, SIM_PORT_C, 0, 0, 0, 0);
    args[0] = (uint32_t)a;
    args[1] = (uint32_t)sa;
    args[2] = 16;
    if (socketcall(SC_BIND, args) != 0)
        fail("blocking simultaneous bind a");
    make_sockaddr(sa, SIM_PORT_D, 0, 0, 0, 0);
    args[0] = (uint32_t)b;
    args[1] = (uint32_t)sa;
    args[2] = 16;
    if (socketcall(SC_BIND, args) != 0)
        fail("blocking simultaneous bind b");
    if (sys_fcntl(a, F_SETFL, O_NONBLOCK) < 0)
        fail("blocking simultaneous nonblock a");
    make_sockaddr(sa, SIM_PORT_D, 127, 0, 0, 1);
    args[0] = (uint32_t)a;
    args[1] = (uint32_t)sa;
    args[2] = 16;
    rc = socketcall(SC_CONNECT, args);
    if (rc != -EINPROGRESS && rc != 0)
        fail("blocking simultaneous connect a");
    /* b blocks. Its SYN crosses a's, and what must wake it is its own connect
     * completing rather than an accept queue nothing is listening on. */
    make_sockaddr(sa, SIM_PORT_C, 127, 0, 0, 1);
    args[0] = (uint32_t)b;
    args[1] = (uint32_t)sa;
    args[2] = 16;
    if (socketcall(SC_CONNECT, args) != 0)
        fail("a blocking connect through crossed SYNs did not complete");

    /* And it carries data, which is what an established connection is for. */
    args[0] = (uint32_t)a;
    args[1] = (uint32_t)msg;
    args[2] = (uint32_t)strlen(msg);
    args[3] = 0;
    args[4] = 0;
    args[5] = 0;
    if (socketcall(SC_SENDTO, args) != (int)strlen(msg))
        fail("simultaneous send");
    for (spin = 0; spin < 400; spin++) {
        args[0] = (uint32_t)b;
        args[1] = (uint32_t)rbuf;
        args[2] = sizeof(rbuf);
        args[3] = MSG_DONTWAIT;
        args[4] = 0;
        args[5] = 0;
        n = socketcall(SC_RECVFROM, args);
        if (n > 0) {
            break;
        }
        sys_nanosleep_ms(20);
    }
    if (n != (int)strlen(msg) || !mem_eq((const uint8_t *)rbuf, (const uint8_t *)msg, n))
        fail("simultaneous payload");

    sys_close(a);
    sys_close(b);
}

void _start(void)
{
    udp_loop();
    tcp_loop();
    tcp_refused();
    tcp_ports();
    tcp_simultaneous();
    print("LOOPPROBE OK\n");
    sys_exit(0);
}
