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
#define SYS_OPEN       5
#define SYS_SOCKETCALL 102
#define SYS_NANOSLEEP  162
#define SYS_FCNTL64    221
#define SYS_PIPE       42

#define SC_SOCKET      1
#define SC_BIND        2
#define SC_CONNECT     3
#define SC_LISTEN      4
#define SC_ACCEPT      5
#define SC_GETSOCKNAME 6
#define SC_GETPEERNAME 7
#define SC_SENDTO      11
#define MSG_DONTWAIT   0x40
#define EAGAIN         11
#define EDESTADDRREQ   89

#define SC_RECVFROM 12
#define SC_SHUTDOWN 13

#define AF_INET     2
#define SOCK_STREAM 1
#define SOCK_DGRAM  2

#define UDP_PORT 7100

/* The ports the bound-port case uses, distinct from every other case here. */
#define BOUND_LISTEN_PORT 7300
#define BOUND_CLIENT_PORT 7301

/* Where the simultaneous-open case's port pairs start; each round takes two. */
#define SIM_PORT_A       7310
#define TCP_PORT         7200
#define SYS_FORK         2
#define SYS_WAITPID      7
#define SYS_GETTIMEOFDAY 78
/* Round trips of one byte. The same rounds run over a pipe first, and both
   times are printed for a person; the verdict is the kernel's count of who
   delivered the loopback's datagrams: with a single queued datagram left to
   the 100 Hz timer each leg would cost a tick, and the idle path, which
   drains the queue when the processor would otherwise park, is where the
   legs must be delivered. */
#define PINGPONG_ROUNDS  200
/* The round trips over the loopback may not wait for the timer: fewer than
 * a tick, ten milliseconds, each, on any machine. The pipe's time is printed
 * beside them for the record. */
#define PINGPONG_TICK_MS 10
#define CLOSED_PORT      7300

#define EADDRINUSE  98
#define EPIPE       32
#define ENOTCONN    107
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

#define O_RDONLY 0

static int sys_open(const char *path, int flags, int mode)
{
    int r;
    __asm__ __volatile__("int $0x80"
                         : "=a"(r)
                         : "0"(SYS_OPEN), "b"(path), "c"(flags), "d"(mode)
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

static void print_u32(uint32_t v)
{
    char d[12];
    int n = 0;

    if (v == 0) {
        print("0");
        return;
    }
    while (v > 0 && n < 12) {
        d[n++] = (char)('0' + (v % 10u));
        v /= 10u;
    }
    while (n > 0) {
        char one[2];

        one[0] = d[--n];
        one[1] = 0;
        print(one);
    }
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

/* The canonical address every loopback peer must read back as. It is the
   machine's own, which this probe asks for rather than assuming the
   emulator's: a UDP socket connected to 127.0.0.1 reports it through
   `getsockname`, which is the same rule the rows below judge. */
static uint8_t canon_ip[4];

static void learn_canon_ip(void)
{
    uint32_t args[6];
    uint8_t sa[16];
    uint8_t me[16];
    int len = 16;
    int fd;
    int i;

    for (i = 0; i < 16; i++) {
        me[i] = 0; /* the kernel fills this; a refusal must not read as an address */
    }

    args[0] = AF_INET;
    args[1] = SOCK_DGRAM;
    args[2] = 0;
    fd = socketcall(SC_SOCKET, args);
    if (fd < 0)
        fail("a socket to learn the local address");
    make_sockaddr(sa, 9, 127, 0, 0, 1);
    args[0] = (uint32_t)fd;
    args[1] = (uint32_t)sa;
    args[2] = 16;
    if (socketcall(SC_CONNECT, args) != 0)
        fail("connecting to learn the local address");
    args[0] = (uint32_t)fd;
    args[1] = (uint32_t)me;
    args[2] = (uint32_t)&len;
    if (socketcall(SC_GETSOCKNAME, args) != 0)
        fail("getsockname to learn the local address");
    sys_close(fd);
    canon_ip[0] = me[4];
    canon_ip[1] = me[5];
    canon_ip[2] = me[6];
    canon_ip[3] = me[7];
    if (canon_ip[0] == 127 || (canon_ip[0] | canon_ip[1] | canon_ip[2] | canon_ip[3]) == 0) {
        fail("the local address read back as a loopback or a zero address");
    }
}

/* The three system calls the round-trip row needs, in this file's shape. */
/* The two descriptors are written by the kernel through `fds`, which is why
   the operand is an output as well as an input. */
static int sys_pipe(int *fds)
{
    int r;

    __asm__ __volatile__("int $0x80" : "=a"(r), "=m"(*(int (*)[2])fds) : "0"(SYS_PIPE), "b"(fds));
    return r;
}

static int sys_fork(void)
{
    int r;

    __asm__ __volatile__("int $0x80" : "=a"(r) : "0"(SYS_FORK));
    return r;
}

static int sys_waitpid(int pid, int *status)
{
    int r;

    __asm__ __volatile__("int $0x80" : "=a"(r) : "0"(SYS_WAITPID), "b"(pid), "c"(status), "d"(0));
    return r;
}

/* The milliseconds from `a` to `b`, both as `gettimeofday` fills them. The
   microseconds are subtracted as signed values, so a second that borrows does
   not wrap through zero and report four billion. */
static int elapsed_ms(const uint32_t *a, const uint32_t *b)
{
    int sec = (int)(b[0] - a[0]);
    int usec = (int)b[1] - (int)a[1];

    return sec * 1000 + usec / 1000;
}

static int sys_gettimeofday(uint32_t *tv)
{
    int r;

    __asm__ __volatile__("int $0x80" : "=a"(r) : "0"(SYS_GETTIMEOFDAY), "b"(tv), "c"(0));
    return r;
}

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

    /* A datagram of no bytes is a datagram. UDP has an empty payload, and a
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

/* More bytes than the ring holds, through the ring, in one process.
 *
 * The loopback ring is a few dozen frames deep. A transfer larger than it is
 * exactly the case that used to stall: the sender filled the ring, every
 * datagram after it was dropped with a line in the log, and the reader waited
 * for bytes that were never delivered. The sender here writes without
 * blocking and the reader takes whatever has arrived, so the two make progress
 * in turn within the one process; what the row requires is that every byte
 * arrives and that the kernel's count of refused datagrams does not rise.
 * Entry 50 of milestone 50. */
#define BULK_BYTES      (256u * 1024u)
/* The ring's slots, from ip.c, and the least this transfer must reach in it.
 * The floor is what makes the row a measurement and not a hope: a transfer
 * that never pressed the ring proves nothing about its room. */
#define LOOP_RING_SLOTS 48u
#define LOOP_PEAK_FLOOR 8u

/* One unsigned counter of /proc/stat, by name. */
static uint32_t proc_stat_u32(const char *key, int keylen)
{
    int fd = sys_open("/proc/stat", O_RDONLY, 0);
    static char buf[2048];
    uint32_t v = 0;
    int n, i;

    if (fd < 0) {
        fail("opening /proc/stat");
    }
    n = sys_read(fd, buf, (int)sizeof(buf) - 1);
    sys_close(fd);
    if (n <= 0) {
        fail("reading /proc/stat");
    }
    buf[n] = 0;
    for (i = 0; i + keylen < n; i++) {
        if ((i == 0 || buf[i - 1] == '\n') &&
            mem_eq((const uint8_t *)(buf + i), (const uint8_t *)key, keylen)) {
            int k = i + keylen;

            while (buf[k] >= '0' && buf[k] <= '9') {
                v = v * 10u + (uint32_t)(buf[k] - '0');
                k++;
            }
            return v;
        }
    }
    fail("/proc/stat has no such counter");
    return 0;
}

static uint32_t loop_dropped_now(void)
{
    return proc_stat_u32("loopback_dropped ", 17);
}

static uint32_t loop_peak_now(void)
{
    return proc_stat_u32("loopback_peak ", 14);
}

/* The byte the stream carries at offset `n`. Every eight-bit window of the
   offset reaches the result, so two blocks of the buffer differ wherever their
   offsets do. */
static char stream_byte(uint32_t n)
{
    return (char)((n * 7u + (n >> 8) * 31u + (n >> 16) * 131u + 1u) & 0xFFu);
}

static void tcp_loop_bulk(void)
{
    uint32_t args[6];
    uint8_t sa[16];
    static char out[65536], in[16384];
    uint32_t sent = 0, got = 0, spins = 0;
    uint32_t dropped_before, dropped_after;
    int ls, cs, as, n, i;

    /* The byte at stream offset n is derived from the whole of n, not from
       its low bits: a pattern that repeats every 256 bytes would let a
       replayed buffer, or a copy taken from the wrong place in the ring, pass
       every comparison below. */
    for (i = 0; i < (int)sizeof(out); i++) {
        out[i] = stream_byte((uint32_t)i);
    }
    dropped_before = loop_dropped_now();

    args[0] = AF_INET;
    args[1] = SOCK_STREAM;
    args[2] = 0;
    ls = socketcall(SC_SOCKET, args);
    if (ls < 0)
        fail("bulk listener socket");
    make_sockaddr(sa, TCP_PORT + 3, 0, 0, 0, 0);
    args[0] = (uint32_t)ls;
    args[1] = (uint32_t)sa;
    args[2] = 16;
    if (socketcall(SC_BIND, args) != 0)
        fail("bulk bind");
    args[0] = (uint32_t)ls;
    args[1] = 2;
    if (socketcall(SC_LISTEN, args) != 0)
        fail("bulk listen");

    args[0] = AF_INET;
    args[1] = SOCK_STREAM;
    args[2] = 0;
    cs = socketcall(SC_SOCKET, args);
    if (cs < 0)
        fail("bulk client socket");
    make_sockaddr(sa, TCP_PORT + 3, 127, 0, 0, 1);
    args[0] = (uint32_t)cs;
    args[1] = (uint32_t)sa;
    args[2] = 16;
    if (socketcall(SC_CONNECT, args) != 0)
        fail("bulk connect");

    args[0] = (uint32_t)ls;
    args[1] = 0;
    args[2] = 0;
    as = socketcall(SC_ACCEPT, args);
    if (as < 0)
        fail("bulk accept");

    if (sys_fcntl(cs, F_SETFL, O_NONBLOCK) != 0)
        fail("bulk client non-blocking");
    if (sys_fcntl(as, F_SETFL, O_NONBLOCK) != 0)
        fail("bulk server non-blocking");

    while (got < BULK_BYTES && spins < 200000u) {
        spins++;
        if (sent < BULK_BYTES) {
            /* The stream is `out` repeated, so a write starts where the last
             * one stopped. A write that takes less than it was offered is
             * ordinary on a non-blocking socket, and starting again at the
             * beginning of the buffer would send bytes the reader below is
             * not expecting and call the kernel wrong for it. */
            uint32_t at = sent % sizeof(out);
            uint32_t want = (uint32_t)sizeof(out) - at;

            if (want > BULK_BYTES - sent)
                want = BULK_BYTES - sent;
            n = sys_write(cs, out + at, (int)want);
            if (n > 0)
                sent += (uint32_t)n;
        }
        n = sys_read(as, in, (int)sizeof(in));
        if (n > 0) {
            for (i = 0; i < n; i++) {
                if (in[i] != stream_byte((got + (uint32_t)i) % (uint32_t)sizeof(out))) {
                    fail("bulk bytes came back wrong");
                }
            }
            got += (uint32_t)n;
        }
    }
    if (got != BULK_BYTES) {
        fail("a transfer larger than the loopback ring did not arrive whole");
    }
    dropped_after = loop_dropped_now();
    if (dropped_after != dropped_before) {
        fail("the loopback ring refused a datagram during the transfer");
    }
    /* The writes above are a window's worth each, which is the largest a
     * single call can put in the ring: 65535 bytes in segments of 1460 is 45
     * of the 64 slots, and a call's exit drains the ring once it holds three
     * or more, so this is the pressure the ring is sized for. The refusal
     * count above is the assertion; the high-water mark below is a mark since
     * boot, so it can say that the ring has been pressed but not that this
     * transfer is what pressed it, and it is reported rather than judged. */
    if (loop_peak_now() < LOOP_PEAK_FLOOR) {
        fail("the loopback ring was never pressed in this boot");
    }
    print("LOOPPROBE ring peak ");
    print_u32(loop_peak_now());
    print(" of 64 since boot\n");
    sys_close(cs);
    sys_close(as);
    sys_close(ls);
}

/* A closed loopback port must refuse promptly (the RFC 793 reset), not
 * black-hole the SYN until the retransmissions give up. */
/*
 * A round trip of one byte each way, two hundred times, between two processes
 * over the loopback.
 *
 * Each leg queues one datagram, which is below the threshold a system call's
 * exit delivers at, so the only thing that can deliver it before the next tick
 * is the idle path: the sender blocks in its read, nothing else is runnable,
 * and the processor is about to be parked. Without that, every leg waits for
 * the timer and a local connection turns over one window per tick, which
 * measured 6.0 MB/s on the machine whatever the write size.
 */
static int pipe_pingpong_ms(void)
{
    int up[2], down[2];
    uint32_t t0[2], t1[2];
    char one = 'p';
    int pid, status = 0, round;

    if (sys_pipe(up) != 0 || sys_pipe(down) != 0)
        fail("pipes for the round-trip calibration");
    pid = sys_fork();
    if (pid < 0)
        fail("fork for the round-trip calibration");
    if (pid == 0) {
        sys_close(up[0]);
        sys_close(down[1]);
        for (round = 0; round < PINGPONG_ROUNDS; round++) {
            if (sys_read(down[0], &one, 1) != 1)
                sys_exit(1);
            if (sys_write(up[1], &one, 1) != 1)
                sys_exit(1);
        }
        sys_exit(0);
    }
    sys_close(up[1]);
    sys_close(down[0]);
    sys_gettimeofday(t0);
    for (round = 0; round < PINGPONG_ROUNDS; round++) {
        if (sys_write(down[1], &one, 1) != 1)
            fail("writing to the child in the round-trip calibration");
        if (sys_read(up[0], &one, 1) != 1)
            fail("reading from the child in the round-trip calibration");
    }
    sys_gettimeofday(t1);
    sys_waitpid(pid, &status);
    if (status != 0)
        fail("the calibration child did not finish");
    sys_close(up[0]);
    sys_close(down[1]);
    return elapsed_ms(t0, t1);
}

/*
 * A round trip of one byte each way, two hundred times, between two processes
 * over the loopback, beside the same rounds over a pipe, whose time is
 * printed for a person; the verdict is the kernel's count of the legs the
 * idle path delivered against the legs the timer did.
 *
 * Each loopback leg queues one datagram, which is below the threshold a system
 * call's exit delivers at, so the only thing that can deliver it before the
 * next tick is the idle path: the sender blocks in its read, nothing else is
 * runnable, and the processor is about to be parked. Without that, every leg
 * waits for the timer and a local connection turns over one window per tick,
 * which measured 6.0 MB/s on the machine whatever the write size. The pipe
 * rounds are the same shape without a ring, so their time is what a round trip
 * costs this machine now, and the loopback's is judged against it.
 */
static void tcp_loop_pingpong(void)
{
    uint32_t args[6];
    uint8_t sa[16];
    uint32_t t0[2], t1[2];
    char one = 'p';
    int ls, cs, as, pid, status = 0, round, ms, base;
    uint32_t idle, tick;

    base = pipe_pingpong_ms();

    args[0] = AF_INET;
    args[1] = SOCK_STREAM;
    args[2] = 0;
    ls = socketcall(SC_SOCKET, args);
    if (ls < 0)
        fail("ping-pong listener socket");
    make_sockaddr(sa, TCP_PORT + 5, 0, 0, 0, 0);
    args[0] = (uint32_t)ls;
    args[1] = (uint32_t)sa;
    args[2] = 16;
    if (socketcall(SC_BIND, args) != 0)
        fail("ping-pong bind");
    args[0] = (uint32_t)ls;
    args[1] = 2;
    if (socketcall(SC_LISTEN, args) != 0)
        fail("ping-pong listen");

    pid = sys_fork();
    if (pid < 0)
        fail("ping-pong fork");
    if (pid == 0) {
        args[0] = AF_INET;
        args[1] = SOCK_STREAM;
        args[2] = 0;
        cs = socketcall(SC_SOCKET, args);
        if (cs < 0)
            sys_exit(1);
        make_sockaddr(sa, TCP_PORT + 5, 127, 0, 0, 1);
        args[0] = (uint32_t)cs;
        args[1] = (uint32_t)sa;
        args[2] = 16;
        if (socketcall(SC_CONNECT, args) != 0)
            sys_exit(1);
        for (round = 0; round < PINGPONG_ROUNDS; round++) {
            if (sys_read(cs, &one, 1) != 1)
                sys_exit(1);
            if (sys_write(cs, &one, 1) != 1)
                sys_exit(1);
        }
        sys_close(cs);
        sys_exit(0);
    }

    args[0] = (uint32_t)ls;
    args[1] = 0;
    args[2] = 0;
    as = socketcall(SC_ACCEPT, args);
    if (as < 0)
        fail("ping-pong accept");
    idle = proc_stat_u32("loopback_delivered_idle ", 24);
    tick = proc_stat_u32("loopback_delivered_tick ", 24);
    sys_gettimeofday(t0);
    for (round = 0; round < PINGPONG_ROUNDS; round++) {
        if (sys_write(as, &one, 1) != 1)
            fail("ping-pong write");
        if (sys_read(as, &one, 1) != 1)
            fail("ping-pong read");
    }
    sys_gettimeofday(t1);
    sys_waitpid(pid, &status);
    if (status != 0)
        fail("the ping-pong child did not finish");
    sys_close(as);
    sys_close(ls);
    ms = elapsed_ms(t0, t1);
    idle = proc_stat_u32("loopback_delivered_idle ", 24) - idle;
    tick = proc_stat_u32("loopback_delivered_tick ", 24) - tick;
    print("LOOPPROBE ping-pong ");
    print_u32((uint32_t)ms);
    print(" ms over the loopback, ");
    print_u32((uint32_t)base);
    print(" over a pipe, for 200 round trips; delivered by the idle path ");
    print_u32(idle);
    print(", by the timer ");
    print_u32(tick);
    print("\n");
    /* Four hundred legs: at least half delivered by the idle path, and
       fewer by the timer than by it. A machine that parks a datagram for
       the tick fails here whatever the clock says. */
    if (ms < 0 || base < 0 || idle < 2 * PINGPONG_ROUNDS / 2 || tick > idle) {
        fail("a local round trip waits for the timer");
    }
}

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
 * A stream this end shut down has no connection to send on and no reason
 * recorded for it: the send is EPIPE, as POSIX names it, and a read, blocking
 * or not, is the end of input, not the bare refusal that reads as EPERM. A
 * stream that never connected has no connection either, and its read is
 * ENOTCONN. `cs` is a connected client; `sa` is scratch space of a byte.
 */
static void shutdown_rows(int cs, uint8_t *sa)
{
    uint32_t args[6];
    int fresh;

    args[0] = (uint32_t)cs;
    args[1] = 1; /* SHUT_WR */
    if (socketcall(SC_SHUTDOWN, args) != 0)
        fail("shutdown of the client");
    args[0] = (uint32_t)cs;
    args[1] = (uint32_t)"x";
    args[2] = 1;
    args[3] = 0;
    args[4] = 0;
    args[5] = 0;
    if (socketcall(SC_SENDTO, args) != -EPIPE)
        fail("a send after shutdown is not EPIPE");
    if (sys_read(cs, sa, 1) != 0)
        fail("a blocking read after shutdown is not the end of input");
    if (sys_fcntl(cs, F_SETFL, O_NONBLOCK) != 0)
        fail("nonblocking after shutdown");
    if (sys_read(cs, sa, 1) != 0)
        fail("a nonblocking read after shutdown is not the end of input");

    args[0] = AF_INET;
    args[1] = SOCK_STREAM;
    args[2] = 0;
    fresh = socketcall(SC_SOCKET, args);
    if (fresh < 0)
        fail("fresh stream socket");
    if (sys_fcntl(fresh, F_SETFL, O_NONBLOCK) != 0)
        fail("nonblocking on the fresh socket");
    if (sys_read(fresh, sa, 1) != -ENOTCONN)
        fail("a read of a stream that never connected is not ENOTCONN");
    sys_close(fresh);
}

/*
 * The local port a client bound, and the exclusivity a listener has.
 *
 * `bind` before `connect` is how a client pins its source port, and a
 * `tcp_connect` that allocated a fresh ephemeral regardless made that
 * impossible: the peer saw a port nobody asked for. What must still be refused
 * is only the exact triple the connection table keys on, so a second connection
 * from the same bound port to a different peer port is legal and a repeat of
 * the same pairing is not.
 *
 * Listener exclusivity is checked where it is published, not at bind,
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
     * held by a TIME_WAIT remnant right now. Refusing on any in-use block,
     * not on a live listener would fail this, which is what restarting
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
        fail("a port a closed session still has could not be bound");
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

    shutdown_rows(cs, sa);

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
 * both ends acknowledging nothing, and this case would hang, not fail,
 * which the runner reports as a probe that never returned.
 */
static void tcp_simultaneous(void)
{
    uint32_t args[6];
    uint8_t sa[16];
    int a, b, rc, spin, round, crossed = 0;
    const char *msg = "simultaneous";
    char rbuf[32];
    int n;

    /* Retried, because the crossing is a race this probe cannot win outright:
     * the loopback ring is drained by the 100 Hz timer, so a tick landing
     * between the two connects delivers the first SYN to a port whose socket
     * has no block yet and resets it. A round that loses that race is a
     * refused connect and not a failure; only losing every round is. */
    for (round = 0; round < 8 && !crossed; round++) {
        args[0] = AF_INET;
        args[1] = SOCK_STREAM;
        args[2] = 0;
        a = socketcall(SC_SOCKET, args);
        args[0] = AF_INET;
        args[1] = SOCK_STREAM;
        args[2] = 0;
        b = socketcall(SC_SOCKET, args);
        if (a < 0 || b < 0)
            fail("simultaneous sockets");

        make_sockaddr(sa, (uint16_t)(SIM_PORT_A + round * 2), 0, 0, 0, 0);
        args[0] = (uint32_t)a;
        args[1] = (uint32_t)sa;
        args[2] = 16;
        if (socketcall(SC_BIND, args) != 0)
            fail("simultaneous bind a");
        make_sockaddr(sa, (uint16_t)(SIM_PORT_A + round * 2 + 1), 0, 0, 0, 0);
        args[0] = (uint32_t)b;
        args[1] = (uint32_t)sa;
        args[2] = 16;
        if (socketcall(SC_BIND, args) != 0)
            fail("simultaneous bind b");

        /* Both non-blocking, which is how two SYNs get to be in flight at once
         * from one process: a blocking connect would sit inside the first
         * handshake, and its SYN would reach a port whose socket has no block
         * yet and earn a reset instead of a crossing SYN. */
        if (sys_fcntl(a, F_SETFL, O_NONBLOCK) < 0 || sys_fcntl(b, F_SETFL, O_NONBLOCK) < 0)
            fail("simultaneous nonblock");

        make_sockaddr(sa, (uint16_t)(SIM_PORT_A + round * 2 + 1), 127, 0, 0, 1);
        args[0] = (uint32_t)a;
        args[1] = (uint32_t)sa;
        args[2] = 16;
        rc = socketcall(SC_CONNECT, args);
        if (rc != -EINPROGRESS && rc != 0)
            fail("simultaneous connect a");

        make_sockaddr(sa, (uint16_t)(SIM_PORT_A + round * 2), 127, 0, 0, 1);
        args[0] = (uint32_t)b;
        args[1] = (uint32_t)sa;
        args[2] = 16;
        rc = socketcall(SC_CONNECT, args);
        if (rc != -EINPROGRESS && rc != 0)
            fail("simultaneous connect b");

        for (spin = 0; spin < 100; spin++) {
            args[0] = (uint32_t)a;
            args[1] = (uint32_t)sa;
            args[2] = 16;
            if (socketcall(SC_CONNECT, args) == -EISCONN) {
                break;
            }
            sys_nanosleep_ms(20);
        }

        /* Whether the round crossed is decided by data moving, not by the
         * connect's answer: a socket this stack reset keeps its block attached
         * in CLOSED, and a repeated connect reports -EISCONN for any attached
         * block that is not still in SYN_SENT, so the two outcomes are
         * indistinguishable from there. A connection that has a payload
         * crossed; anything else is a lost round. */
        n = 0;
        args[0] = (uint32_t)a;
        args[1] = (uint32_t)msg;
        args[2] = (uint32_t)strlen(msg);
        args[3] = 0;
        args[4] = 0;
        args[5] = 0;
        if (socketcall(SC_SENDTO, args) == (int)strlen(msg)) {
            for (spin = 0; spin < 100; spin++) {
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
        }
        if (n == (int)strlen(msg) && mem_eq((const uint8_t *)rbuf, (const uint8_t *)msg, n)) {
            crossed = 1;
        }
        sys_close(a);
        sys_close(b);
    }
    if (!crossed)
        fail("the crossing SYNs never established a connection that has data");
}

void _start(void)
{
    learn_canon_ip();
    udp_loop();
    tcp_loop();
    tcp_loop_bulk();
    tcp_loop_pingpong();
    tcp_refused();
    tcp_ports();
    tcp_simultaneous();
    print("LOOPPROBE OK\n");
    sys_exit(0);
}
