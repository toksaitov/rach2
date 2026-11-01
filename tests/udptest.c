/**
 * udptest: the UDP socket calls socket, bind, sendto and recvfrom, made as a
 * client of a host echo server at the SLIRP gateway 10.0.2.2:7777, or at the
 * port its argument names, and datagrams past one frame: to the server, and
 * of the largest sizes over 127.0.0.1 and ::1, whole or cut to a short
 * buffer. It exits 0 when the ten tests pass and 1 otherwise.
 *
 * Beside its own lines it writes etude's report records (probe_report.h),
 * one assertion for each test, for the case ETUDE_CASE names or
 * machine.socket.udptest, whose scenario runs it at the debug shell.
 *
 * A native run (probe_args.h) gives the echo server as `peer=A.B.C.D:PORT`,
 * in place of the gateway and the port, and the local port it binds through
 * `ports=OLD:NEW,...`, in place of 12345.
 */

#include "probe.h"
#include "probe_args.h"
#include "probe_report.h"

/* An address of the kernel's, outside user space: 1 MiB on i386, where the
 * kernel is linked, and on x86-64 the last page, in the kernel's half. */
#if defined __x86_64__
#define KERNEL_PTR 0xFFFFFFFFFFFFF000ul
#else
#define KERNEL_PTR 0x00100000
#endif

typedef unsigned int uint32_t;
typedef unsigned short uint16_t;
typedef unsigned char uint8_t;

#define STDOUT_FILENO 1

#define AF_INET      2
#define AF_INET6     10
#define SOCK_DGRAM   2
#define MSG_DONTWAIT 0x40
#define MSG_PEEK     0x02
#define MSG_TRUNC    0x20
#define EAGAIN       (-11)
#define EFAULT       (-14)
#define EMSGSIZE     (-90)

/* The data of the datagram to the echo server, past one frame and so sent
 * in fragments each way. A host's own limits decide the most it can echo:
 * macOS sends no UDP datagram past 9216 bytes by default, QEMU's from
 * among them. */
#define BIG_ECHO 8192
/* The most data a UDP datagram holds over IPv4 and over IPv6. */
#define UDP_MAX  65507
#define UDP6_MAX 65527

/* A message header for SC_RECVMSG, laid out as the kernel's. */
struct iovec_probe {
    void *base;
    unsigned long len;
};
struct msghdr_probe {
    void *name;
    unsigned int namelen;
    struct iovec_probe *iov;
    unsigned long iovlen;
    void *control;
    unsigned long controllen;
    int flags;
};

/* Test target: host echo server on the SLIRP gateway. */
#define DST_IP_0   10
#define DST_IP_1   0
#define DST_IP_2   2
#define DST_IP_3   2
#define DST_PORT   7777
#define LOCAL_PORT 12345

/* How long a wait for the echo lasts, in milliseconds, before the message is
 * sent again. It paces the resends and decides nothing: the row waits for as
 * many as it takes, and the case's watchdog ends a run whose echo never
 * comes. */
#define RESEND_MS 1000

#define POLLIN 0x001

struct pollfd_probe {
    int fd;
    short events;
    short revents;
};

/** The write call: the count written, or a negated errno. */
static int sys_write(int fd, const void *buf, size_t count)
{
    return syscall3(SYS_WRITE, (long)fd, (long)buf, (long)count);
}

/** End the process with `status`; it does not return. */
static void sys_exit(int status)
{
    syscall1(SYS_EXIT, (long)status);
    __builtin_unreachable();
}

/** Open `path` to read: a descriptor, or a negated errno. */
static int sys_open_ro(const char *path)
{
    return syscall3(SYS_OPEN, (long)path, 0L, 0L);
}

/** Read up to `n` bytes of `fd`: the count read, or a negated errno. */
static int sys_read(int fd, void *buf, int n)
{
    return syscall3(SYS_READ, (long)fd, (long)buf, (long)n);
}

/** Close `fd`. */
static void sys_close(int fd)
{
    syscall1(SYS_CLOSE, (long)fd);
}

/** The poll call, with `timeout` in milliseconds: the count of ready
 * descriptors, 0 when the timeout passed first, or a negated errno. */
static int sys_poll(struct pollfd_probe *fds, unsigned int n, int timeout)
{
    return syscall3(SYS_POLL, (long)fds, (long)n, (long)timeout);
}

/** The socket operation `call`, an SC_ number, through `sock_call`. */
static int socketcall(int call, unsigned long *args)
{
    return (int)sock_call(call, args);
}

/** The length of the string `s`. */
static size_t strlen(const char *s)
{
    size_t n = 0;
    while (s[n])
        n++;
    return n;
}

/** Write `s` to standard output. */
static void print(const char *s)
{
    sys_write(STDOUT_FILENO, s, strlen(s));
}
/** Write `s` and a newline to standard output. */
static void puts(const char *s)
{
    print(s);
    sys_write(STDOUT_FILENO, "\n", 1);
}

/** Write `n` in decimal to standard output. */
static void print_num(int n)
{
    char buf[12];
    int i = 0, neg = 0;
    if (n < 0) {
        neg = 1;
        n = -n;
    }
    if (n == 0)
        buf[i++] = '0';
    while (n > 0) {
        buf[i++] = (char)('0' + n % 10);
        n /= 10;
    }
    if (neg)
        sys_write(STDOUT_FILENO, "-", 1);
    while (i > 0)
        sys_write(STDOUT_FILENO, &buf[--i], 1);
}

/** Non-zero when the first `n` bytes of `a` and `b` are equal. */
static int mem_eq(const uint8_t *a, const uint8_t *b, int n)
{
    int i;
    for (i = 0; i < n; i++) {
        if (a[i] != b[i])
            return 0;
    }
    return 1;
}

/** Fill a 16-byte sockaddr_in: family, big-endian port, the IPv4 address
 * a.b.c.d. */
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

/* /proc/net/snmp, read by `snmp_ip`. */
static char snmp[2048];

/** Non-zero when the `n` bytes at `a` are the string `b`'s first `n`. */
static int str_eq(const char *a, const char *b, int n)
{
    int i;

    for (i = 0; i < n; i++) {
        if (a[i] != b[i])
            return 0;
    }
    return 1;
}

/**
 * The value of the column `name` of the Ip lines of /proc/net/snmp, a line
 * of names followed by one of values, or 0xFFFFFFFF when the file or the
 * column cannot be read.
 */
static uint32_t snmp_ip(const char *name)
{
    int fd, n = 0, r, i, col = 0, want = -1, len = (int)strlen(name);
    uint32_t v = 0;

    fd = sys_open_ro("/proc/net/snmp");
    if (fd < 0)
        return 0xFFFFFFFFu;
    while (n < (int)sizeof(snmp) - 1 && (r = sys_read(fd, snmp + n, (int)sizeof(snmp) - 1 - n)) > 0)
        n += r;
    sys_close(fd);
    snmp[n] = 0;
    if (n < 4 || !str_eq(snmp, "Ip: ", 4))
        return 0xFFFFFFFFu;
    for (i = 4; i < n && snmp[i] != '\n'; i++) {
        if (snmp[i - 1] == ' ' && str_eq(snmp + i, name, len) &&
            (snmp[i + len] == ' ' || snmp[i + len] == '\n'))
            want = col;
        if (snmp[i] == ' ')
            col++;
    }
    if (want < 0 || i + 5 >= n || !str_eq(snmp + i + 1, "Ip: ", 4))
        return 0xFFFFFFFFu;
    for (i += 5, col = 0; i < n && col < want; i++) {
        if (snmp[i] == ' ')
            col++;
    }
    for (; i < n && snmp[i] >= '0' && snmp[i] <= '9'; i++)
        v = v * 10 + (uint32_t)(snmp[i] - '0');
    return v;
}

/* The datagrams these rows send and receive, the largest of 65527 bytes. */
static uint8_t big_out[65536], big_in[65536];

/** Fill `big_out` with a pattern of period 251, so that a datagram cut or
 * shifted by any whole count of fragments does not compare equal. */
static void fill_big(void)
{
    unsigned int i;

    for (i = 0; i < sizeof(big_out); i++)
        big_out[i] = (uint8_t)(i % 251u);
}

/** Send `len` bytes of `big_out` from `fd` to the `salen`-byte address `sa`:
 * the count sent, or a negated errno. */
static int send_big(int fd, int len, const uint8_t *sa, int salen)
{
    unsigned long args[6];

    args[0] = (uint32_t)fd;
    args[1] = (unsigned long)big_out;
    args[2] = (uint32_t)len;
    args[3] = 0;
    args[4] = (unsigned long)sa;
    args[5] = (uint32_t)salen;
    return socketcall(SC_SENDTO, args);
}

/** Receive one datagram on `fd` into `big_in`, of `cap` bytes, with the
 * receive flags `flags`: what recvfrom returns. */
static int recv_big(int fd, int cap, int flags)
{
    unsigned long args[6];

    args[0] = (uint32_t)fd;
    args[1] = (unsigned long)big_in;
    args[2] = (uint32_t)cap;
    args[3] = (uint32_t)flags;
    args[4] = 0;
    args[5] = 0;
    return socketcall(SC_RECVFROM, args);
}

/**
 * A socket of `family` bound to its loopback address at a port the kernel
 * picks, with that address and port in the `salen`-byte `sa`, so that a
 * datagram sent to `sa` comes back to it: the descriptor, or -1.
 */
static int loop_socket(int family, uint8_t *sa, int salen)
{
    unsigned long args[3], len = (unsigned long)salen;
    int fd, i;

    args[0] = (uint32_t)family;
    args[1] = SOCK_DGRAM;
    args[2] = 0;
    fd = socketcall(SC_SOCKET, args);
    if (fd < 0)
        return -1;
    for (i = 0; i < salen; i++)
        sa[i] = 0;
    sa[0] = (uint8_t)family;
    if (family == AF_INET) {
        sa[4] = 127;
        sa[7] = 1;
    } else {
        sa[8 + 15] = 1;
    }
    args[0] = (uint32_t)fd;
    args[1] = (unsigned long)sa;
    args[2] = (uint32_t)salen;
    if (socketcall(SC_BIND, args) != 0)
        return -1;
    args[2] = (unsigned long)&len;
    if (socketcall(SC_GETSOCKNAME, args) != 0 || (sa[2] == 0 && sa[3] == 0))
        return -1;
    return fd;
}

/** Print the outcome of the test `id` and report it: `ok` passes, else
 * `why` is the failure, with the number `got` printed beside it. */
static int verdict(const char *id, int ok, const char *why, int got)
{
    if (ok) {
        puts("OK");
        report_pass(id);
        return 1;
    }
    print("FAILED (got ");
    print_num(got);
    puts(")");
    report_fail(id, why);
    return 0;
}

/**
 * Test 6: a datagram of BIG_ECHO bytes to the echo server comes back whole,
 * sent again each RESEND_MS until it does. The echoes of test 4 may still
 * arrive, and are read and passed over. Outside a native run, where other
 * traffic moves the machine's counts, the reply's fragments must have been
 * reassembled here: ReasmOKs grows.
 */
static int big_echo(int fd, const uint8_t *dst_sa)
{
    struct pollfd_probe pfd;
    uint32_t oks = snmp_ip("ReasmOKs");
    int n = 0;

    print("Test 6: an echo of 8192 bytes comes back whole... ");
    while (n != BIG_ECHO) {
        if (send_big(fd, BIG_ECHO, dst_sa, 16) != BIG_ECHO)
            return verdict("big-echo", 0, "the send of 8192 bytes failed", n);
        pfd.fd = fd;
        pfd.events = POLLIN;
        pfd.revents = 0;
        if (sys_poll(&pfd, 1, RESEND_MS) <= 0)
            continue;
        while ((n = recv_big(fd, (int)sizeof(big_in), MSG_DONTWAIT)) >= 0 && n != BIG_ECHO) {
        }
    }
    if (!mem_eq(big_in, big_out, BIG_ECHO))
        return verdict("big-echo", 0, "the echo of 8192 bytes is not the bytes sent", n);
    return verdict("big-echo",
                   probe_native() || (oks != 0xFFFFFFFFu && snmp_ip("ReasmOKs") - oks >= 1),
                   "the echo's fragments were not counted as reassembled", (int)oks);
}

/**
 * Tests 7 to 10, over the loopback interface, whose MTU of 65536 carries
 * the largest IPv4 UDP datagram whole: 65507 bytes to 127.0.0.1 arrive
 * whole and 65508 are refused; a 100-byte buffer takes the first 100 bytes
 * with MSG_TRUNC in msg_flags, and MSG_TRUNC in the flags returns the whole
 * length, each taking the datagram; and 65527 bytes to ::1, sent in two
 * fragments, arrive whole and 65528 are refused.
 */
static int loopback_rows(void)
{
    struct msghdr_probe msg;
    struct iovec_probe iov;
    unsigned long args[3];
    uint8_t sa[16], sa6[28];
    int fd, fd6, n, ok, passed = 0;

    print("Test 7: 65507 bytes over 127.0.0.1 arrive whole... ");
    fd = loop_socket(AF_INET, sa, (int)sizeof(sa));
    n = fd < 0 ? -1 : send_big(fd, UDP_MAX, sa, (int)sizeof(sa));
    if (n == UDP_MAX)
        n = recv_big(fd, (int)sizeof(big_in), 0);
    passed += verdict("loopback-whole", n == UDP_MAX && mem_eq(big_in, big_out, UDP_MAX),
                      "the datagram of 65507 bytes did not arrive whole", n);

    print("Test 8: 65508 bytes over IPv4 are refused... ");
    n = fd < 0 ? -1 : send_big(fd, UDP_MAX + 1, sa, (int)sizeof(sa));
    passed += verdict("size-refused", n == EMSGSIZE, "the send was not refused with EMSGSIZE", n);

    print("Test 9: a short buffer gets the start, with MSG_TRUNC... ");
    n = fd < 0 ? -1 : send_big(fd, UDP_MAX, sa, (int)sizeof(sa));
    msg.name = 0;
    msg.namelen = 0;
    msg.iov = &iov;
    msg.iovlen = 1;
    msg.control = 0;
    msg.controllen = 0;
    msg.flags = 0;
    iov.base = big_in;
    iov.len = 100;
    if (n == UDP_MAX) {
        args[0] = (uint32_t)fd;
        args[1] = (unsigned long)&msg;
        args[2] = 0;
        n = socketcall(SC_RECVMSG, args);
    }
    ok = n == 100 && (msg.flags & MSG_TRUNC) && mem_eq(big_in, big_out, 100);
    n = ok ? send_big(fd, UDP_MAX, sa, (int)sizeof(sa)) : n;
    if (ok && n == UDP_MAX)
        n = recv_big(fd, 100, MSG_TRUNC);
    ok = ok && n == UDP_MAX && recv_big(fd, 100, MSG_DONTWAIT) == EAGAIN;
    passed +=
        verdict("truncated", ok,
                "the cut datagram was not 100 bytes with MSG_TRUNC, then its whole length", n);
    if (fd >= 0)
        sys_close(fd);

    print("Test 10: 65527 bytes over ::1 arrive whole... ");
    fd6 = loop_socket(AF_INET6, sa6, (int)sizeof(sa6));
    n = fd6 < 0 ? -1 : send_big(fd6, UDP6_MAX, sa6, (int)sizeof(sa6));
    if (n == UDP6_MAX)
        n = recv_big(fd6, (int)sizeof(big_in), 0);
    ok = n == UDP6_MAX && mem_eq(big_in, big_out, UDP6_MAX);
    n = ok ? send_big(fd6, UDP6_MAX + 1, sa6, (int)sizeof(sa6)) : n;
    passed += verdict("loopback6-whole", ok && n == EMSGSIZE,
                      "65527 bytes over ::1 did not arrive whole, or 65528 were not refused", n);
    if (fd6 >= 0)
        sys_close(fd6);
    return passed;
}

/* The echo server's host, the gateway unless `peer` names another. */
static uint8_t server_ip[4] = {DST_IP_0, DST_IP_1, DST_IP_2, DST_IP_3};

/**
 * The address and port of the argument `peer=A.B.C.D:PORT` (probe_args.h)
 * in `ip` and `*port`: 1, or 0 when there is no such argument or it is not
 * of that form, which leaves both as they were.
 */
static int peer_address(uint8_t ip[4], uint16_t *port)
{
    const char *s = probe_arg("peer", 0);
    long part[5];
    int i;

    for (i = 0; s != 0 && i < 5; i++) {
        part[i] = probe_number(&s);
        if (part[i] < 0 || part[i] > (i < 4 ? 255 : 65535) ||
            (i < 4 && *s++ != (i < 3 ? '.' : ':'))) {
            return 0;
        }
    }
    if (s == 0) {
        return 0;
    }
    for (i = 0; i < 4; i++) {
        ip[i] = (uint8_t)part[i];
    }
    *port = (uint16_t)part[4];
    return 1;
}

/** The echo server's port: the peer's when `peer` is given, else the first
 * positional argument when there is one, since tests/system/suites.mk moves
 * the socket suite's port with the run's block of ports, else DST_PORT; the
 * peer's address is then the server's host. */
static uint16_t server_port(void)
{
    const char *p = probe_word(1);
    uint16_t port = DST_PORT;
    uint32_t n = 0;

    if (peer_address(server_ip, &port))
        return port;
    if (p == 0)
        return DST_PORT;
    for (; *p >= '0' && *p <= '9'; p++)
        n = n * 10 + (uint32_t)(*p - '0');
    return (uint16_t)n;
}

/** Require EFAULT from a receive into a kernel address, with an echo queued.
 * Return 1 on success, or print the failed row and return 0. */
static int reject_kernel_pointer(int fd, int queued)
{
    unsigned long args[6];
    int n;

    print("Test 5: recvfrom into a kernel pointer is rejected... ");
    if (!queued) {
        puts("FAILED (no queued echo)");
        report_notrun("kernel-pointer", "no echo was queued to receive");
        return 0;
    }
    args[0] = (uint32_t)fd;
    args[1] = KERNEL_PTR;
    args[2] = 64;
    args[3] = MSG_DONTWAIT;
    args[4] = 0;
    args[5] = 0;
    n = socketcall(SC_RECVFROM, args);
    if (n == EFAULT) {
        puts("OK (EFAULT)");
        report_pass("kernel-pointer");
        return 1;
    } else {
        print("FAILED (got ");
        print_num(n);
        puts(")");
        report_fail("kernel-pointer", "the receive did not answer EFAULT");
        return 0;
    }
}

void probe_main(unsigned long *sp);

PROBE_SP_ENTRY;

/**
 * The probe, entered with `sp` at argc: the ten tests in order. A failed
 * socket in test 1 ends it at once with status 1.
 *
 * Test 4 sends the message and waits in poll for the echo, sending the
 * message again each time RESEND_MS passes with no echo, for as long
 * as it takes: a datagram between the emulator's network and the host can
 * be lost, and a datagram sent before ARP has the gateway's address waits
 * in the kernel for the reply. The count of sends decides nothing. The echo
 * is then read with MSG_PEEK, so test 5 finds it still queued.
 */
void probe_main(unsigned long *sp)
{
    unsigned long args[6], rcv_len = 16;
    uint8_t local_sa[16], dst_sa[16], rcv_sa[16];
    char msg[] = "rach2-udp-echo";
    uint8_t buf[64];
    uint16_t dst_port;
    struct pollfd_probe pfd;
    int fd, n, msglen, passed = 0, failed = 0, ready;

    probe_args((int)sp[0], (char **)(sp + 1));
    report_begin(report_case((char **)(sp + 2 + sp[0]), "machine.socket.udptest"));
    msglen = (int)strlen(msg);

    puts("UDP socket test starting...");
    puts("");

    /* Test 1: create a datagram socket. */
    print("Test 1: socket(AF_INET, SOCK_DGRAM)... ");
    args[0] = AF_INET;
    args[1] = SOCK_DGRAM;
    args[2] = 0;
    fd = socketcall(SC_SOCKET, args);
    if (fd >= 0) {
        puts("OK");
        report_pass("socket");
        passed++;
    } else {
        puts("FAILED");
        report_fail("socket", "socket(AF_INET, SOCK_DGRAM) failed");
        report_notrun("bind", "no socket opened");
        report_notrun("empty-eagain", "no socket opened");
        report_notrun("echo", "no socket opened");
        report_notrun("kernel-pointer", "no socket opened");
        report_notrun("big-echo", "no socket opened");
        report_notrun("loopback-whole", "no socket opened");
        report_notrun("size-refused", "no socket opened");
        report_notrun("truncated", "no socket opened");
        report_notrun("loopback6-whole", "no socket opened");
        report_end();
        failed++;
        sys_exit(1);
    }

    /* Test 2: bind a local port. */
    print("Test 2: bind local port... ");
    make_sockaddr(local_sa, (uint16_t)probe_port(LOCAL_PORT), 0, 0, 0, 0);
    args[0] = (uint32_t)fd;
    args[1] = (unsigned long)local_sa;
    args[2] = 16;
    if (socketcall(SC_BIND, args) == 0) {
        puts("OK");
        report_pass("bind");
        passed++;
    } else {
        puts("FAILED");
        report_fail("bind", "the bind of the local port failed");
        failed++;
    }

    /* Test 3: non-blocking recv on an empty socket returns EAGAIN. */
    print("Test 3: recvfrom(MSG_DONTWAIT) with no data... ");
    args[0] = (uint32_t)fd;
    args[1] = (unsigned long)buf;
    args[2] = sizeof(buf);
    args[3] = MSG_DONTWAIT;
    args[4] = 0;
    args[5] = 0;
    n = socketcall(SC_RECVFROM, args);
    if (n == EAGAIN) {
        puts("OK (EAGAIN)");
        report_pass("empty-eagain");
        passed++;
    } else {
        print("FAILED (got ");
        print_num(n);
        puts(")");
        report_fail("empty-eagain", "the receive on the empty socket did not answer EAGAIN");
        failed++;
    }

    /* Test 4: echo round trip, sent again until the echo arrives. */
    print("Test 4: sendto + recvfrom echo... ");
    dst_port = server_port();
    make_sockaddr(dst_sa, dst_port, server_ip[0], server_ip[1], server_ip[2], server_ip[3]);
    ready = 0;
    while (ready == 0) {
        args[0] = (uint32_t)fd;
        args[1] = (unsigned long)msg;
        args[2] = (uint32_t)msglen;
        args[3] = 0;
        args[4] = (unsigned long)dst_sa;
        args[5] = 16;
        socketcall(SC_SENDTO, args);

        pfd.fd = fd;
        pfd.events = POLLIN;
        pfd.revents = 0;
        ready = sys_poll(&pfd, 1, RESEND_MS);
    }

    args[0] = (uint32_t)fd;
    args[1] = (unsigned long)buf;
    args[2] = sizeof(buf);
    args[3] = MSG_DONTWAIT | MSG_PEEK;
    args[4] = (unsigned long)rcv_sa;
    args[5] = (unsigned long)&rcv_len;
    n = ready < 0 ? ready : socketcall(SC_RECVFROM, args);

    if (n == msglen && mem_eq(buf, (const uint8_t *)msg, msglen)) {
        puts("OK");
        report_pass("echo");
        passed++;
    } else {
        print("FAILED (n=");
        print_num(n);
        puts(")");
        report_fail("echo", "the echo is not the bytes sent");
        failed++;
    }

    if (reject_kernel_pointer(fd, n == msglen && mem_eq(buf, (const uint8_t *)msg, msglen))) {
        passed++;
    } else {
        failed++;
    }

    fill_big();
    if (big_echo(fd, dst_sa)) {
        passed++;
    } else {
        failed++;
    }
    n = loopback_rows();
    passed += n;
    failed += 4 - n;

    report_end();
    puts("");
    print("Results: ");
    print_num(passed);
    print(" passed, ");
    print_num(failed);
    puts(" failed");

    if (failed == 0) {
        puts("All UDP tests passed!");
        sys_exit(0);
    }
    puts("Some tests failed!");
    sys_exit(1);
}
