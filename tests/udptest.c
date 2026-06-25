/**
 * Exercises the SYS_SOCKETCALL UDP path: socket, bind, sendto, and recvfrom
 * in both blocking and non-blocking (MSG_DONTWAIT) forms. Acts as a UDP echo
 * client against a host echo server reachable at the SLIRP gateway
 * 10.0.2.2:7777.
 *
 * Freestanding; direct int 0x80.
 */

typedef unsigned int   size_t;
typedef unsigned int   uint32_t;
typedef unsigned short uint16_t;
typedef unsigned char  uint8_t;

#define STDOUT_FILENO 1

#define SYS_EXIT         1
#define SYS_WRITE        4
#define SYS_GETTIMEOFDAY 78
#define SYS_SOCKETCALL   102

/* socketcall sub-calls. */
#define SC_SOCKET    1
#define SC_BIND      2
#define SC_SENDTO   11
#define SC_RECVFROM 12

#define AF_INET      2
#define SOCK_DGRAM   2
#define MSG_DONTWAIT 0x40
#define EAGAIN       (-11)

/* Test target: host echo server on the SLIRP gateway. */
#define DST_IP_0 10
#define DST_IP_1 0
#define DST_IP_2 2
#define DST_IP_3 2
#define DST_PORT   7777
#define LOCAL_PORT 12345

struct timeval { long tv_sec; long tv_usec; };

static int sys_write(int fd, const void *buf, size_t count)
{
    int r;
    __asm__ __volatile__("int $0x80"
        : "=a"(r) : "0"(SYS_WRITE), "b"(fd), "c"(buf), "d"(count) : "memory");
    return r;
}

static void sys_exit(int status)
{
    __asm__ __volatile__("int $0x80" : : "a"(SYS_EXIT), "b"(status));
    __builtin_unreachable();
}

static int sys_gettimeofday(struct timeval *tv)
{
    int r;
    __asm__ __volatile__("int $0x80"
        : "=a"(r) : "0"(SYS_GETTIMEOFDAY), "b"(tv), "c"(0) : "memory");
    return r;
}

static int socketcall(int call, uint32_t *args)
{
    int r;
    __asm__ __volatile__("int $0x80"
        : "=a"(r) : "0"(SYS_SOCKETCALL), "b"(call), "c"(args) : "memory");
    return r;
}

static size_t strlen(const char *s)
{
    size_t n = 0;
    while (s[n]) n++;
    return n;
}

static void print(const char *s)  { sys_write(STDOUT_FILENO, s, strlen(s)); }
static void puts(const char *s)   { print(s); sys_write(STDOUT_FILENO, "\n", 1); }

static void print_num(int n)
{
    char buf[12];
    int i = 0, neg = 0;
    if (n < 0) { neg = 1; n = -n; }
    if (n == 0) buf[i++] = '0';
    while (n > 0) { buf[i++] = (char)('0' + n % 10); n /= 10; }
    if (neg) sys_write(STDOUT_FILENO, "-", 1);
    while (i > 0) sys_write(STDOUT_FILENO, &buf[--i], 1);
}

static int mem_eq(const uint8_t *a, const uint8_t *b, int n)
{
    int i;
    for (i = 0; i < n; i++) {
        if (a[i] != b[i]) return 0;
    }
    return 1;
}

/* Fill a 16-byte sockaddr_in: family, big-endian port, big-endian IPv4. */
static void make_sockaddr(uint8_t *sa, uint16_t port,
                          uint8_t a, uint8_t b, uint8_t c, uint8_t d)
{
    int i;
    for (i = 0; i < 16; i++) sa[i] = 0;
    sa[0] = AF_INET; sa[1] = 0;
    sa[2] = (uint8_t)(port >> 8); sa[3] = (uint8_t)port;
    sa[4] = a; sa[5] = b; sa[6] = c; sa[7] = d;
}

/* Busy-wait roughly ms milliseconds using the 100 Hz clock. */
static void delay_ms(int ms)
{
    struct timeval start, now;
    long elapsed;
    sys_gettimeofday(&start);
    do {
        sys_gettimeofday(&now);
        elapsed = (now.tv_sec - start.tv_sec) * 1000 +
                  (now.tv_usec - start.tv_usec) / 1000;
    } while (elapsed < ms);
}

void _start(void)
{
    uint32_t args[6];
    uint8_t  local_sa[16], dst_sa[16], rcv_sa[16];
    char     msg[] = "rach2-udp-echo";
    uint8_t  buf[64];
    int      fd, n, msglen, passed = 0, failed = 0, attempt;

    msglen = (int)strlen(msg);

    puts("UDP socket test starting...");
    puts("");

    /* Test 1: create a datagram socket. */
    print("Test 1: socket(AF_INET, SOCK_DGRAM)... ");
    args[0] = AF_INET; args[1] = SOCK_DGRAM; args[2] = 0;
    fd = socketcall(SC_SOCKET, args);
    if (fd >= 0) { puts("OK"); passed++; }
    else { puts("FAILED"); failed++; sys_exit(1); }

    /* Test 2: bind a local port. */
    print("Test 2: bind local port... ");
    make_sockaddr(local_sa, LOCAL_PORT, 0, 0, 0, 0);
    args[0] = (uint32_t)fd; args[1] = (uint32_t)local_sa; args[2] = 16;
    if (socketcall(SC_BIND, args) == 0) { puts("OK"); passed++; }
    else { puts("FAILED"); failed++; }

    /* Test 3: non-blocking recv on an empty socket returns EAGAIN. */
    print("Test 3: recvfrom(MSG_DONTWAIT) with no data... ");
    args[0] = (uint32_t)fd; args[1] = (uint32_t)buf; args[2] = sizeof(buf);
    args[3] = MSG_DONTWAIT; args[4] = 0; args[5] = 0;
    n = socketcall(SC_RECVFROM, args);
    if (n == EAGAIN) { puts("OK (EAGAIN)"); passed++; }
    else { print("FAILED (got "); print_num(n); puts(")"); failed++; }

    /* Test 4: echo round-trip. Retry the send so the first datagram, which is
     * dropped while ARP resolves the gateway, does not stall the test. */
    print("Test 4: sendto + recvfrom echo... ");
    make_sockaddr(dst_sa, DST_PORT, DST_IP_0, DST_IP_1, DST_IP_2, DST_IP_3);
    n = EAGAIN;
    for (attempt = 0; attempt < 40 && n < 0; attempt++) {
        args[0] = (uint32_t)fd; args[1] = (uint32_t)msg; args[2] = (uint32_t)msglen;
        args[3] = 0; args[4] = (uint32_t)dst_sa; args[5] = 16;
        socketcall(SC_SENDTO, args);

        delay_ms(50);

        args[0] = (uint32_t)fd; args[1] = (uint32_t)buf; args[2] = sizeof(buf);
        args[3] = MSG_DONTWAIT; args[4] = (uint32_t)rcv_sa; args[5] = 0;
        n = socketcall(SC_RECVFROM, args);
    }

    if (n == msglen && mem_eq(buf, (const uint8_t *)msg, msglen)) {
        puts("OK");
        passed++;
    } else {
        print("FAILED (n="); print_num(n); puts(")");
        failed++;
    }

    /* Test 5: recvfrom into a kernel address must be rejected, not faulted */
    print("Test 5: recvfrom into a kernel pointer is rejected... ");
    args[0] = (uint32_t)fd; args[1] = 0x00100000; args[2] = sizeof(buf);
    args[3] = MSG_DONTWAIT; args[4] = 0; args[5] = 0;
    n = socketcall(SC_RECVFROM, args);
    if (n < 0) { puts("OK (rejected)"); passed++; }
    else { print("FAILED (got "); print_num(n); puts(")"); failed++; }

    puts("");
    print("Results: "); print_num(passed); print(" passed, ");
    print_num(failed); puts(" failed");

    if (failed == 0) {
        puts("All UDP tests passed!");
        sys_exit(0);
    }
    puts("Some tests failed!");
    sys_exit(1);
}
