/**
 * tcptest: the TCP socket path, against a host echo server at the SLIRP
 * gateway 10.0.2.2:7777, or at the port its argument names.
 *
 * Connects, sends data and reads it back through `read()`/`write()` on the
 * socket descriptor, closes, then checks that connecting to a closed port
 * fails instead of hanging. It exits 0 when the four tests pass and 1
 * otherwise.
 */

#include "probe.h"

typedef unsigned int uint32_t;
typedef unsigned short uint16_t;
typedef unsigned char uint8_t;

#define STDOUT_FILENO 1

#define AF_INET     2
#define SOCK_STREAM 1

#define DST_PORT    7777
#define CLOSED_PORT 9

/** The write call: the count written, or a negated errno. */
static int sys_write(int fd, const void *buf, size_t count)
{
    return syscall3(SYS_WRITE, (long)fd, (long)buf, (long)count);
}

/** The read call: the count read, 0 at the end, or a negated errno. */
static int sys_read(int fd, void *buf, size_t count)
{
    return syscall3(SYS_READ, (long)fd, (long)buf, (long)count);
}

/** The close call: 0, or a negated errno. */
static int sys_close(int fd)
{
    return syscall1(SYS_CLOSE, (long)fd);
}

/** The socket operation `call`, an SC_ number, through `sock_call`. */
static int socketcall(int call, unsigned long *args)
{
    return (int)sock_call(call, args);
}

/** End the process with `status`; it does not return. */
static void sys_exit(int status)
{
    syscall1(SYS_EXIT, (long)status);
    __builtin_unreachable();
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

/** A TCP socket: its descriptor, or a negated errno. */
static int make_stream_socket(void)
{
    unsigned long args[3];
    args[0] = AF_INET;
    args[1] = SOCK_STREAM;
    args[2] = 0;
    return socketcall(SC_SOCKET, args);
}

/** Connect `fd` to `port` of the gateway 10.0.2.2: 0, or a negated errno. */
static int connect_to(int fd, uint16_t port)
{
    unsigned long args[3];
    uint8_t sa[16];
    make_sockaddr(sa, port, 10, 0, 2, 2);
    args[0] = (uint32_t)fd;
    args[1] = (unsigned long)sa;
    args[2] = 16;
    return socketcall(SC_CONNECT, args);
}

/** The echo server's port: the first argument when there is one, since
 * tests/system/suites.mk moves the socket suite's port with the run's block
 * of ports, else DST_PORT. */
static uint16_t server_port(unsigned long *sp)
{
    char **argv = (char **)(sp + 1);
    const char *p;
    uint32_t n = 0;

    if (sp[0] < 2)
        return DST_PORT;
    for (p = argv[1]; *p >= '0' && *p <= '9'; p++)
        n = n * 10 + (uint32_t)(*p - '0');
    return (uint16_t)n;
}

void probe_main(unsigned long *sp);

PROBE_SP_ENTRY;

/** The probe, entered with `sp` at argc: the four tests in order. A failed
 * socket or connect in test 1 ends it at once with status 1. */
void probe_main(unsigned long *sp)
{
    char msg[] = "hello-tcp-echo";
    uint8_t buf[64];
    int fd, fd2, n, msglen, passed = 0, failed = 0;

    msglen = (int)strlen(msg);

    puts("TCP socket test starting...");
    puts("");

    /* Test 1: connect to the echo server. */
    print("Test 1: connect to echo server... ");
    fd = make_stream_socket();
    if (fd < 0) {
        puts("FAILED (socket)");
        sys_exit(1);
    }
    if (connect_to(fd, server_port(sp)) == 0) {
        puts("OK");
        passed++;
    } else {
        puts("FAILED");
        failed++;
        sys_exit(1);
    }

    /* Test 2: write data and read the echo back over the socket fd. */
    print("Test 2: write + read echo... ");
    if (sys_write(fd, msg, (size_t)msglen) != msglen) {
        puts("FAILED (write)");
        failed++;
    } else {
        n = sys_read(fd, buf, sizeof(buf));
        if (n == msglen && mem_eq(buf, (const uint8_t *)msg, msglen)) {
            puts("OK");
            passed++;
        } else {
            print("FAILED (n=");
            print_num(n);
            puts(")");
            failed++;
        }
    }

    /* Test 3: graceful close. */
    print("Test 3: close... ");
    if (sys_close(fd) == 0) {
        puts("OK");
        passed++;
    } else {
        puts("FAILED");
        failed++;
    }

    /* Test 4: connecting to a closed port fails, and does not hang. */
    print("Test 4: connect to closed port fails... ");
    fd2 = make_stream_socket();
    if (fd2 < 0) {
        puts("FAILED (socket)");
        failed++;
    } else {
        if (connect_to(fd2, CLOSED_PORT) < 0) {
            puts("OK (refused)");
            passed++;
        } else {
            puts("FAILED (connected?)");
            failed++;
        }
        sys_close(fd2);
    }

    puts("");
    print("Results: ");
    print_num(passed);
    print(" passed, ");
    print_num(failed);
    puts(" failed");

    if (failed == 0) {
        puts("All TCP tests passed!");
        sys_exit(0);
    }
    puts("Some tests failed!");
    sys_exit(1);
}
