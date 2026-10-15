/**
 * rawprobe: the raw ICMP socket, on which `ping` and `traceroute` are built.
 *
 * A raw socket receives whole IPv4 datagrams of one protocol, header
 * included, and sends a message the caller built, with the header supplied
 * for it. The rows check what a working ping would not notice:
 *
 * * an ordinary user is refused one (EPERM), and a protocol with no path
 *   through this stack is refused too (EPROTONOSUPPORT);
 * * a datagram reaches every raw socket, byte for byte the same, and the
 *   kernel's own ICMP still answers: a kernel that handed each datagram to
 *   one socket would pass a test with a single ping in it;
 * * `IP_TTL`, the one option traceroute sets, is kept and read back, and the
 *   time-exceeded message a hop limit of one draws arrives on both sockets;
 * * `IP_HDRINCL` is refused rather than accepted and ignored;
 * * a message past the MTU is refused, and one for an address whose hardware
 *   address is not known yet is accepted, since the request has gone and a
 *   datagram promises no delivery;
 * * the queue is eight deep, and the ninth datagram is dropped, not the first;
 * * eight more raw sockets open at once, and while two children hold full
 *   descriptor tables of sockets a socket still opens and the first raw
 *   socket still receives.
 *
 * For the suite's capture checker (`pcapcheck rawwire`) it also sends raw
 * datagrams with hop limits 1 to 3, datagram sends with 4 to 6, and a message
 * with a wrong checksum, which must cross the wire as written. Booted with an
 * address and no gateway, it checks only that a raw send and a datagram send
 * off the subnet are ENETUNREACH.
 *
 * Runs on the test image with QEMU's user network, where 10.0.2.2 is the
 * gateway and answers. Prints "RAWPROBE START", then "  FAIL: " and the row
 * for each failure, then "RAWPROBE OK" and exits 0, or "RAWPROBE FAIL" and
 * exits with the count of failures. Freestanding; direct system calls
 * (probe.h).
 */

#include "probe.h"

typedef unsigned int uint32_t;
typedef unsigned short uint16_t;
typedef unsigned char uint8_t;

#define AF_INET    2
#define SOCK_DGRAM 2
#define SOCK_RAW   3

#define IPPROTO_ICMP 1
#define IPPROTO_TCP  6
#define IPPROTO_RAW  255

#define SOL_IP     0
#define IP_TTL     2
#define IP_HDRINCL 3

#define POLLIN 0x001

#define EPERM           1
#define EAGAIN          11
#define EMFILE          24
#define EMSGSIZE        90
#define ENOBUFS         105
#define ENETUNREACH     101
#define ENOMEM          12
#define ENOPROTOOPT     92
#define EPROTONOSUPPORT 93

#define MSG_DONTWAIT 0x40

/* The addresses of the network QEMU puts a guest on. */
#define GATEWAY_IP 0x0A000202u /* 10.0.2.2, answers */
#define OFFSUBNET  0x08080808u /* 8.8.8.8, reached through the gateway */
#define UNRESOLVED 0x0A000209u /* 10.0.2.9, on the subnet, nothing there */

#define ICMP_ECHO_REPLY    0
#define ICMP_ECHO_REQUEST  8
#define ICMP_TIME_EXCEEDED 11

struct pollfd_probe {
    int fd;
    short events;
    short revents;
};

/** exit(2); does not return. */
static void sys_exit(int status)
{
    syscall1(SYS_EXIT, status);
    __builtin_unreachable();
}
/** close(2). */
static int sys_close(int fd)
{
    return syscall1(SYS_CLOSE, fd);
}
/** The socket operation `call` on `args` (`sock_call`), as an int. */
static int socketcall(int call, unsigned long *args)
{
    return (int)sock_call(call, args);
}
/** pipe(2): the two ends in `fds`; 0, or the negative errno. */
static int sys_pipe(int *fds)
{
    return syscall1(SYS_PIPE, (long)fds);
}
/** poll(2), with `timeout` in milliseconds. */
static int sys_poll(struct pollfd_probe *fds, unsigned int n, int timeout)
{
    return syscall3(SYS_POLL, (long)fds, (int)n, timeout);
}
/** Sleep for `ms` milliseconds (nanosleep(2)). */
static void nap_ms(int ms)
{
    long ts[2];

    ts[0] = ms / 1000;
    ts[1] = (ms % 1000) * 1000000;
    syscall3(SYS_NANOSLEEP, (long)ts, 0, 0);
}

/** The length of the string `s`. */
static size_t strlen(const char *s)
{
    size_t n = 0;
    while (s[n]) {
        n++;
    }
    return n;
}
/** Write the string `s` to standard output. */
static void say(const char *s)
{
    syscall3(SYS_WRITE, 1, (long)s, (int)strlen(s));
}
/** Write `v` in decimal to standard output. */
static void say_int(int v)
{
    char b[16];
    int i = 15, neg = 0;

    b[i] = '\0';
    if (v < 0) {
        neg = 1;
        v = -v;
    }
    if (v == 0) {
        b[--i] = '0';
    }
    while (v > 0) {
        b[--i] = (char)('0' + (v % 10));
        v /= 10;
    }
    if (neg) {
        b[--i] = '-';
    }
    say(b + i);
}

/* The rows that have failed, which is the exit status. */
static int fails = 0;

/** Unless `cond`, count a failed row and print "  FAIL: `msg`". */
static void check(int cond, const char *msg)
{
    if (!cond) {
        fails++;
        say("  FAIL: ");
        say(msg);
        say("\n");
    }
}

/** The same, with the value `n` the row got. */
static void check_n(int cond, const char *msg, int n)
{
    if (!cond) {
        fails++;
        say("  FAIL: ");
        say(msg);
        say(" (");
        say_int(n);
        say(")\n");
    }
}

/** The 16-bit one's complement checksum an ICMP message carries. */
static uint16_t icmp_checksum(const uint8_t *buf, int len)
{
    uint32_t sum = 0;
    int i;

    for (i = 0; i + 1 < len; i += 2) {
        sum += (uint32_t)((buf[i] << 8) | buf[i + 1]);
    }
    if (i < len) {
        sum += (uint32_t)(buf[i] << 8);
    }
    while (sum >> 16) {
        sum = (sum & 0xffffu) + (sum >> 16);
    }
    return (uint16_t)(~sum & 0xffffu);
}

/** Fill the 16-byte sockaddr_in `out` with `ip` and `port`, both in host
 * order; a raw socket's port is 0. */
static void sockaddr_for(uint8_t *out, uint32_t ip, uint16_t port)
{
    int i;

    for (i = 0; i < 16; i++) {
        out[i] = 0;
    }
    out[0] = AF_INET;
    out[2] = (uint8_t)((port >> 8) & 0xff);
    out[3] = (uint8_t)(port & 0xff);
    out[4] = (uint8_t)((ip >> 24) & 0xff);
    out[5] = (uint8_t)((ip >> 16) & 0xff);
    out[6] = (uint8_t)((ip >> 8) & 0xff);
    out[7] = (uint8_t)(ip & 0xff);
}

/** Open a raw ICMP socket, with `args` as the call's scratch space: the
 * descriptor, or the negative errno. */
static int raw_socket(unsigned long *args)
{
    args[0] = AF_INET;
    args[1] = SOCK_RAW;
    args[2] = IPPROTO_ICMP;
    return socketcall(SC_SOCKET, args);
}

/** Read one waiting datagram without blocking; the length, or the errno. */
static int raw_take(unsigned long *args, int fd, uint8_t *buf, int cap)
{
    args[0] = (uint32_t)fd;
    args[1] = (unsigned long)buf;
    args[2] = (uint32_t)cap;
    args[3] = MSG_DONTWAIT;
    args[4] = 0;
    args[5] = 0;
    return socketcall(SC_RECVFROM, args);
}

/**
 * Wait up to `ms` for a datagram of ICMP type `want` and take it.
 *
 * Returns its length, with the datagram in `buf`, or 0 if none arrived. Other
 * ICMP the machine receives while waiting is taken and skipped, so a reply to
 * something else cannot be mistaken for the message under test.
 */
static int wait_for_icmp(unsigned long *args, int fd, uint8_t *buf, int cap, int want, int ms)
{
    struct pollfd_probe pfd;
    int waited = 0;

    while (waited < ms) {
        pfd.fd = fd;
        pfd.events = POLLIN;
        pfd.revents = 0;
        if (sys_poll(&pfd, 1, 20) > 0 && (pfd.revents & POLLIN)) {
            int n = raw_take(args, fd, buf, cap);
            int hdr;

            if (n < 20) {
                waited += 20;
                continue;
            }
            hdr = (buf[0] & 0x0f) * 4;
            if (n > hdr && buf[hdr] == (uint8_t)want) {
                return n;
            }
        }
        waited += 20;
    }
    return 0;
}

/** open(2), read-only. */
static int sys_open_ro(const char *path)
{
    return syscall3(SYS_OPEN, (long)path, (long)0, (long)0);
}

/** read(2). */
static int sys_read(int fd, void *buf, int n)
{
    return syscall3(SYS_READ, (long)fd, (long)buf, (long)n);
}

/** 1 if `needle` occurs in `hay[0..len)`, else 0. */
static int contains(const char *hay, int len, const char *needle)
{
    int nl = 0, i, j;

    while (needle[nl]) {
        nl++;
    }
    for (i = 0; i + nl <= len; i++) {
        for (j = 0; j < nl && hay[i + j] == needle[j]; j++) {
            ;
        }
        if (j == nl) {
            return 1;
        }
    }
    return 0;
}

/* The boot line, read from /proc/cmdline. */
static char cmdline[512];

/**
 * Whether this boot was given an address and no gateway: the boot line has
 * `rach2.ip=` and no `rach2.gateway=`.
 *
 * The suite boots the probe both ways. With no route off the subnet the
 * probe checks that the machine says so, and cannot wait for the gateway's
 * answers.
 */
static int booted_without_a_route(void)
{
    int fd, n = 0;

    fd = sys_open_ro("/proc/cmdline");
    if (fd < 0) {
        return 0;
    }
    n = sys_read(fd, cmdline, (int)sizeof(cmdline) - 1);
    sys_close(fd);
    if (n <= 0) {
        return 0;
    }
    cmdline[n] = '\0';
    return contains(cmdline, n, "rach2.ip=") && !contains(cmdline, n, "rach2.gateway=");
}

static uint8_t frame_a[1600];
static uint8_t frame_b[1600];
static uint8_t icmp[1600];
static uint8_t sa[16];

/**
 * The body of a child of `socket_pressure`: open ordinary datagram sockets,
 * not raw ones, up to the descriptor limit, say on `ready_w` whether its
 * table filled, some held and the last open refused with EMFILE and not for
 * want of memory, and hold them until the parent closes the other end of
 * `release_r`, then let them go. Exits 0 when it held some.
 */
static void fill_and_hold(unsigned long *args, int ready_w, int release_r)
{
    int held = 0;
    int rc;
    char c;

    for (;;) {
        args[0] = AF_INET;
        args[1] = SOCK_DGRAM;
        args[2] = 0;
        rc = socketcall(SC_SOCKET, args);
        if (rc < 0) {
            break;
        }
        held++;
    }
    c = held > 0 && rc == -EMFILE ? 'y' : 'n';
    syscall3(SYS_WRITE, ready_w, (long)&c, 1);
    sys_close(ready_w);
    syscall3(SYS_READ, release_r, (long)&c, 1);
    sys_exit(held > 0 ? 0 : 4);
}

/**
 * Sockets come from the heap, and a process is bounded by its descriptor
 * limit, not by a pool: two children open sockets until their tables are
 * full and hold them, and a socket for this process still opens beside them,
 * and `fd_a`, the raw socket it already has, goes on receiving. Each child
 * says through `ready` that its table is full and holds its sockets until
 * this process closes `release`, so the checks run while both tables are
 * full, however long the children take to fill them.
 */
static void socket_pressure(unsigned long *args, int fd_a)
{
    int kids[2], ready[2] = {-1, -1}, release[2] = {-1, -1};
    int k, n, status, full = 0;
    char c;

    check(sys_pipe(ready) == 0 && sys_pipe(release) == 0,
          "two pipes for the children that fill their tables");
    for (k = 0; k < 2; k++) {
        kids[k] = syscall1(SYS_FORK, 0);
        if (kids[k] == 0) {
            sys_close(ready[0]);
            sys_close(release[1]);
            fill_and_hold(args, ready[1], release[0]);
        }
    }
    sys_close(ready[1]);
    sys_close(release[0]);
    check(kids[0] > 0 && kids[1] > 0, "two children forked to fill their tables");
    for (k = 0; k < 2; k++) {
        c = 0;
        if (syscall3(SYS_READ, ready[0], (long)&c, 1) == 1 && c == 'y') {
            full++;
        }
    }
    check_n(full == 2, "both children filled their tables before the checks", full);

    args[0] = AF_INET;
    args[1] = SOCK_DGRAM;
    args[2] = 0;
    n = socketcall(SC_SOCKET, args);
    check_n(n >= 0, "a socket opens while two others hold a thousand each", n);
    if (n >= 0) {
        sys_close(n);
    }

    /* The raw socket this process opened before still receives. */
    args[0] = GATEWAY_IP;
    n = socketcall(SC_PING, args);
    check_n(n >= 0, "the machine can still reach the gateway", n);
    n = wait_for_icmp(args, fd_a, frame_a, (int)sizeof(frame_a), ICMP_ECHO_REPLY, 2000);
    check(n > 0, "and a socket opened before the tables filled still receives");

    sys_close(release[1]);
    sys_close(ready[0]);
    for (k = 0; k < 2; k++) {
        status = -1;
        check(syscall3(SYS_WAITPID, kids[k], (long)&status, 0) == kids[k] && (status & 0x7f) == 0 &&
                  ((status >> 8) & 0xff) == 0,
              "the child that held sockets held some");
    }
    args[0] = AF_INET;
    args[1] = SOCK_DGRAM;
    args[2] = 0;
    n = socketcall(SC_SOCKET, args);
    check_n(n >= 0, "and a socket opens after they are closed", n);
    if (n >= 0) {
        sys_close(n);
    }
}

/**
 * The probe's entry: the rows in the order of the list above, then the
 * socket pressure. The queue row closes the second raw socket, so that one
 * socket counts the replies, and first takes what the earlier rows left
 * queued; the replies to the sends for the capture checker are taken and
 * not read.
 */
PROBE_START void _start(void)
{
    unsigned long args[6];
    int fd_a, fd_b, i, n, m, pid, status, val;
    volatile int got_ttl;

    say("RAWPROBE START\n");

    /* A raw socket is root's, since it reads every ICMP datagram. */
    pid = syscall1(SYS_FORK, 0);
    if (pid == 0) {
        if (syscall1(SYS_SETUID, 1000) != 0) {
            sys_exit(2);
        }
        sys_exit(raw_socket(args) == -EPERM ? 0 : 3);
    }
    check(pid > 0, "fork for the unprivileged case");
    status = -1;
    syscall3(SYS_WAITPID, pid, (long)&status, 0);
    check((status & 0x7f) == 0, "the unprivileged case did not die on a signal");
    check(((status >> 8) & 0xff) == 0, "an ordinary user was refused a raw socket");

    /* A protocol with no path through this stack is refused. */
    args[0] = AF_INET;
    args[1] = SOCK_RAW;
    args[2] = IPPROTO_TCP;
    check(socketcall(SC_SOCKET, args) == -EPROTONOSUPPORT,
          "a raw socket for a protocol with no path here was refused");

    /* An address and no gateway: no route off the subnet, and no more rows. */
    if (booted_without_a_route()) {
        int fd_r, udp_fd;

        fd_r = raw_socket(args);
        check_n(fd_r >= 0, "a raw socket opens with no route configured", fd_r);
        sockaddr_for(sa, OFFSUBNET, 0);
        for (i = 0; i < 8; i++) {
            icmp[i] = 0;
        }
        icmp[0] = ICMP_ECHO_REQUEST;
        args[0] = (uint32_t)fd_r;
        args[1] = (unsigned long)icmp;
        args[2] = 8;
        args[3] = 0;
        args[4] = (unsigned long)sa;
        args[5] = 16;
        check(socketcall(SC_SENDTO, args) == -ENETUNREACH, "a raw send with no route says so");

        args[0] = AF_INET;
        args[1] = SOCK_DGRAM;
        args[2] = 0;
        udp_fd = socketcall(SC_SOCKET, args);
        check_n(udp_fd >= 0, "a datagram socket opens", udp_fd);
        sockaddr_for(sa, OFFSUBNET, 9);
        args[0] = (uint32_t)udp_fd;
        args[1] = (unsigned long)icmp;
        args[2] = 4;
        args[3] = 0;
        args[4] = (unsigned long)sa;
        args[5] = 16;
        check(socketcall(SC_SENDTO, args) == -ENETUNREACH,
              "and so does a datagram send, which is the same route");
        sys_close(udp_fd);
        sys_close(fd_r);
        if (fails == 0) {
            say("RAWPROBE OK\n");
            sys_exit(0);
        }
        say("RAWPROBE FAIL\n");
        sys_exit(fails);
    }

    fd_a = raw_socket(args);
    fd_b = raw_socket(args);
    check_n(fd_a >= 0, "the first raw socket opens", fd_a);
    check_n(fd_b >= 0, "the second raw socket opens", fd_b);
    if (fd_a < 0 || fd_b < 0) {
        say("RAWPROBE FAIL\n");
        sys_exit(1);
    }

    /* One echo reply reaches both sockets and the kernel's own ping. */
    args[0] = GATEWAY_IP;
    /* The ping's result is its round-trip time in ticks, or a negated errno. */
    n = socketcall(SC_PING, args);
    check_n(n >= 0, "the kernel's own ping reached the gateway", n);

    n = wait_for_icmp(args, fd_a, frame_a, (int)sizeof(frame_a), ICMP_ECHO_REPLY, 2000);
    m = wait_for_icmp(args, fd_b, frame_b, (int)sizeof(frame_b), ICMP_ECHO_REPLY, 2000);
    check(n > 0, "the first socket was given the echo reply");
    check(m > 0, "and so was the second, which is what a tap means");
    if (n > 0 && m > 0) {
        check(n == m, "both were given the same number of bytes");
        if (n == m) {
            int same = 1;
            for (i = 0; i < n; i++) {
                if (frame_a[i] != frame_b[i]) {
                    same = 0;
                }
            }
            check(same, "and the same bytes, not one copy shared out");
        }
        check((frame_a[0] >> 4) == 4, "what arrived is an IPv4 datagram");
        check(frame_a[9] == IPPROTO_ICMP, "of the protocol the socket asked for");
        check(frame_a[12] == 10 && frame_a[13] == 0 && frame_a[14] == 2 && frame_a[15] == 2,
              "from the address that answered");
    }

    /* A hop limit of one: the first router's time-exceeded reaches both. */
    {
        int udp_fd;

        args[0] = AF_INET;
        args[1] = SOCK_DGRAM;
        args[2] = 0;
        udp_fd = socketcall(SC_SOCKET, args);
        check_n(udp_fd >= 0, "a datagram socket opens", udp_fd);

        val = 1;
        args[0] = (uint32_t)udp_fd;
        args[1] = SOL_IP;
        args[2] = IP_TTL;
        args[3] = (unsigned long)&val;
        args[4] = sizeof(int);
        check(socketcall(SC_SETSOCKOPT, args) == 0, "IP_TTL is accepted");

        /* The kernel writes it through the pointer getsockopt is given. */
        got_ttl = 0;
        n = (int)sizeof(int);
        args[0] = (uint32_t)udp_fd;
        args[1] = SOL_IP;
        args[2] = IP_TTL;
        args[3] = (unsigned long)&got_ttl;
        args[4] = (unsigned long)&n;
        check(socketcall(SC_GETSOCKOPT, args) == 0 && got_ttl == 1,
              "and reported back, so it is kept and not swallowed");

        sockaddr_for(sa, OFFSUBNET, 33434);
        args[0] = (uint32_t)udp_fd;
        args[1] = (unsigned long)icmp;
        args[2] = 4;
        args[3] = 0;
        args[4] = (unsigned long)sa;
        args[5] = 16;
        check(socketcall(SC_SENDTO, args) == 4, "a probe leaves with that hop limit");

        n = wait_for_icmp(args, fd_a, frame_a, (int)sizeof(frame_a), ICMP_TIME_EXCEEDED, 3000);
        m = wait_for_icmp(args, fd_b, frame_b, (int)sizeof(frame_b), ICMP_TIME_EXCEEDED, 3000);
        check(n > 0, "the hop limit expired and said so");
        check(m > 0, "and the other socket was told as well");
        sys_close(udp_fd);
    }

    /* IP_HDRINCL is refused, not accepted and ignored. */
    val = 1;
    args[0] = (uint32_t)fd_a;
    args[1] = SOL_IP;
    args[2] = IP_HDRINCL;
    args[3] = (unsigned long)&val;
    args[4] = sizeof(int);
    check(socketcall(SC_SETSOCKOPT, args) == -ENOPROTOOPT, "IP_HDRINCL is refused");

    /* A message past the MTU is refused by the send itself. */
    sockaddr_for(sa, GATEWAY_IP, 0);
    args[0] = (uint32_t)fd_a;
    args[1] = (unsigned long)icmp;
    args[2] = 1501;
    args[3] = 0;
    args[4] = (unsigned long)sa;
    args[5] = 16;
    check(socketcall(SC_SENDTO, args) == -EMSGSIZE, "a message past the MTU is refused");

    /* A send to a neighbour whose address is not resolved is accepted. */
    for (i = 0; i < 8; i++) {
        icmp[i] = 0;
    }
    icmp[0] = ICMP_ECHO_REQUEST;
    icmp[4] = 0x52;
    icmp[5] = 0x50;
    icmp[6] = 0;
    icmp[7] = 1;
    {
        uint16_t csum = icmp_checksum(icmp, 8);
        icmp[2] = (uint8_t)(csum >> 8);
        icmp[3] = (uint8_t)(csum & 0xff);
    }
    sockaddr_for(sa, UNRESOLVED, 0);
    args[0] = (uint32_t)fd_a;
    args[1] = (unsigned long)icmp;
    args[2] = 8;
    args[3] = 0;
    args[4] = (unsigned long)sa;
    args[5] = 16;
    check(socketcall(SC_SENDTO, args) == 8, "a send to an unresolved neighbour is not an error");

    /* Nine replies to a queue eight deep: the first eight are kept. */
    {
        int got = 0;

        sys_close(fd_b);
        while (raw_take(args, fd_a, frame_a, (int)sizeof(frame_a)) > 0) {
            ;
        }
        for (i = 0; i < 9; i++) {
            uint16_t csum;

            for (n = 0; n < 8; n++) {
                icmp[n] = 0;
            }
            icmp[0] = ICMP_ECHO_REQUEST;
            icmp[4] = 0x52;
            icmp[5] = 0x50;
            icmp[6] = 0;
            icmp[7] = (uint8_t)(i + 1);
            csum = icmp_checksum(icmp, 8);
            icmp[2] = (uint8_t)(csum >> 8);
            icmp[3] = (uint8_t)(csum & 0xff);
            sockaddr_for(sa, GATEWAY_IP, 0);
            args[0] = (uint32_t)fd_a;
            args[1] = (unsigned long)icmp;
            args[2] = 8;
            args[3] = 0;
            args[4] = (unsigned long)sa;
            args[5] = 16;
            check(socketcall(SC_SENDTO, args) == 8, "an echo request leaves");
            nap_ms(30);
        }
        nap_ms(300);
        for (i = 0; i < 12; i++) {
            n = raw_take(args, fd_a, frame_a, (int)sizeof(frame_a));
            if (n <= 0) {
                break;
            }
            got++;
        }
        check_n(got == 8, "a queue eight deep keeps eight and no more", got);
    }

    /* For the capture checker: hop limits 1 to 6 and a wrong checksum. */
    {
        int udp_fd, ttl;

        for (ttl = 1; ttl <= 3; ttl++) {
            uint16_t csum;

            val = ttl;
            args[0] = (uint32_t)fd_a;
            args[1] = SOL_IP;
            args[2] = IP_TTL;
            args[3] = (unsigned long)&val;
            args[4] = sizeof(int);
            check(socketcall(SC_SETSOCKOPT, args) == 0, "the raw socket takes a hop limit");
            for (n = 0; n < 8; n++) {
                icmp[n] = 0;
            }
            icmp[0] = ICMP_ECHO_REQUEST;
            icmp[4] = 0x52;
            icmp[5] = 0x54;
            icmp[6] = 0;
            icmp[7] = (uint8_t)ttl;
            csum = icmp_checksum(icmp, 8);
            icmp[2] = (uint8_t)(csum >> 8);
            icmp[3] = (uint8_t)(csum & 0xff);
            sockaddr_for(sa, GATEWAY_IP, 0);
            args[0] = (uint32_t)fd_a;
            args[1] = (unsigned long)icmp;
            args[2] = 8;
            args[3] = 0;
            args[4] = (unsigned long)sa;
            args[5] = 16;
            check(socketcall(SC_SENDTO, args) == 8, "and sends with it");
            nap_ms(50);
        }

        /* The wrong checksum, which must cross the wire as it was written. */
        for (n = 0; n < 8; n++) {
            icmp[n] = 0;
        }
        icmp[0] = ICMP_ECHO_REQUEST;
        icmp[2] = 0x12;
        icmp[3] = 0x34;
        icmp[4] = 0x52;
        icmp[5] = 0x54;
        icmp[7] = 9;
        args[0] = (uint32_t)fd_a;
        args[1] = (unsigned long)icmp;
        args[2] = 8;
        args[3] = 0;
        args[4] = (unsigned long)sa;
        args[5] = 16;
        check(socketcall(SC_SENDTO, args) == 8, "a message with a wrong checksum sends");
        nap_ms(50);

        args[0] = AF_INET;
        args[1] = SOCK_DGRAM;
        args[2] = 0;
        udp_fd = socketcall(SC_SOCKET, args);
        check_n(udp_fd >= 0, "a datagram socket opens for the hop limits", udp_fd);
        for (ttl = 4; ttl <= 6; ttl++) {
            val = ttl;
            args[0] = (uint32_t)udp_fd;
            args[1] = SOL_IP;
            args[2] = IP_TTL;
            args[3] = (unsigned long)&val;
            args[4] = sizeof(int);
            check(socketcall(SC_SETSOCKOPT, args) == 0, "the datagram socket takes one");
            sockaddr_for(sa, GATEWAY_IP, (uint16_t)(40000 + ttl));
            args[0] = (uint32_t)udp_fd;
            args[1] = (unsigned long)icmp;
            args[2] = 4;
            args[3] = 0;
            args[4] = (unsigned long)sa;
            args[5] = 16;
            check(socketcall(SC_SENDTO, args) == 4, "and sends with it");
            nap_ms(50);
        }
        sys_close(udp_fd);
        while (raw_take(args, fd_a, frame_a, (int)sizeof(frame_a)) > 0) {
            ;
        }
        val = 64;
        args[0] = (uint32_t)fd_a;
        args[1] = SOL_IP;
        args[2] = IP_TTL;
        args[3] = (unsigned long)&val;
        args[4] = sizeof(int);
        socketcall(SC_SETSOCKOPT, args);
    }

    /* Eight more raw sockets open at once beside `fd_a`, each from the heap. */
    {
        int held[8];

        for (i = 0; i < 8; i++) {
            held[i] = raw_socket(args);
            check_n(held[i] >= 0, "a raw socket past the four the table held opens", held[i]);
        }
        for (i = 0; i < 8; i++) {
            if (held[i] >= 0) {
                sys_close(held[i]);
            }
        }
    }

    socket_pressure(args, fd_a);

    sys_close(fd_a);

    if (fails == 0) {
        say("RAWPROBE OK\n");
        sys_exit(0);
    }
    say("RAWPROBE FAIL\n");
    sys_exit(fails);
}
