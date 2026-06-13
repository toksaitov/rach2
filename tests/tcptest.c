/*
 * tcptest.c - TCP socket test program for Rach2 OS
 *
 * Connects to a host TCP echo server at the SLIRP gateway 10.0.2.2:7777,
 * sends data and reads it back through read()/write() on the socket fd (the
 * unified descriptor path), closes gracefully, then verifies that connecting
 * to a closed port fails instead of hanging.
 *
 * Compile with:
 *   i686-elf-gcc -m32 -nostdlib -static -ffreestanding -Wl,-Ttext=0x08048000 -o tcptest tcptest.c
 */

typedef unsigned int   size_t;
typedef unsigned int   uint32_t;
typedef unsigned short uint16_t;
typedef unsigned char  uint8_t;

#define STDOUT_FILENO 1

#define SYS_EXIT       1
#define SYS_READ       3
#define SYS_WRITE      4
#define SYS_CLOSE      6
#define SYS_SOCKETCALL 102

#define SC_SOCKET    1
#define SC_CONNECT   3

#define AF_INET      2
#define SOCK_STREAM  1

#define DST_PORT       7777
#define CLOSED_PORT    9

static int sys_write(int fd, const void *buf, size_t count)
{
    int r;
    __asm__ __volatile__("int $0x80"
        : "=a"(r) : "0"(SYS_WRITE), "b"(fd), "c"(buf), "d"(count) : "memory");
    return r;
}

static int sys_read(int fd, void *buf, size_t count)
{
    int r;
    __asm__ __volatile__("int $0x80"
        : "=a"(r) : "0"(SYS_READ), "b"(fd), "c"(buf), "d"(count) : "memory");
    return r;
}

static int sys_close(int fd)
{
    int r;
    __asm__ __volatile__("int $0x80" : "=a"(r) : "0"(SYS_CLOSE), "b"(fd));
    return r;
}

static int socketcall(int call, uint32_t *args)
{
    int r;
    __asm__ __volatile__("int $0x80"
        : "=a"(r) : "0"(SYS_SOCKETCALL), "b"(call), "c"(args) : "memory");
    return r;
}

static void sys_exit(int status)
{
    __asm__ __volatile__("int $0x80" : : "a"(SYS_EXIT), "b"(status));
    __builtin_unreachable();
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

static void make_sockaddr(uint8_t *sa, uint16_t port,
                          uint8_t a, uint8_t b, uint8_t c, uint8_t d)
{
    int i;
    for (i = 0; i < 16; i++) sa[i] = 0;
    sa[0] = AF_INET; sa[1] = 0;
    sa[2] = (uint8_t)(port >> 8); sa[3] = (uint8_t)port;
    sa[4] = a; sa[5] = b; sa[6] = c; sa[7] = d;
}

static int make_stream_socket(void)
{
    uint32_t args[3];
    args[0] = AF_INET; args[1] = SOCK_STREAM; args[2] = 0;
    return socketcall(SC_SOCKET, args);
}

static int connect_to(int fd, uint16_t port)
{
    uint32_t args[3];
    uint8_t  sa[16];
    make_sockaddr(sa, port, 10, 0, 2, 2);
    args[0] = (uint32_t)fd; args[1] = (uint32_t)sa; args[2] = 16;
    return socketcall(SC_CONNECT, args);
}

void _start(void)
{
    char    msg[] = "hello-tcp-echo";
    uint8_t buf[64];
    int     fd, fd2, n, msglen, passed = 0, failed = 0;

    msglen = (int)strlen(msg);

    puts("TCP socket test starting...");
    puts("");

    /* Test 1: connect to the echo server. The first SYN may be dropped while
     * ARP resolves; the SYN retransmission completes the handshake. */
    print("Test 1: connect to echo server... ");
    fd = make_stream_socket();
    if (fd < 0) { puts("FAILED (socket)"); sys_exit(1); }
    if (connect_to(fd, DST_PORT) == 0) { puts("OK"); passed++; }
    else { puts("FAILED"); failed++; sys_exit(1); }

    /* Test 2: write data and read the echo back over the socket fd. */
    print("Test 2: write + read echo... ");
    if (sys_write(fd, msg, (size_t)msglen) != msglen) {
        puts("FAILED (write)"); failed++;
    } else {
        n = sys_read(fd, buf, sizeof(buf));
        if (n == msglen && mem_eq(buf, (const uint8_t *)msg, msglen)) {
            puts("OK"); passed++;
        } else {
            print("FAILED (n="); print_num(n); puts(")"); failed++;
        }
    }

    /* Test 3: graceful close. */
    print("Test 3: close... ");
    if (sys_close(fd) == 0) { puts("OK"); passed++; }
    else { puts("FAILED"); failed++; }

    /* Test 4: connecting to a closed port must fail, not hang. */
    print("Test 4: connect to closed port fails... ");
    fd2 = make_stream_socket();
    if (fd2 < 0) { puts("FAILED (socket)"); failed++; }
    else {
        if (connect_to(fd2, CLOSED_PORT) < 0) { puts("OK (refused)"); passed++; }
        else { puts("FAILED (connected?)"); failed++; }
        sys_close(fd2);
    }

    puts("");
    print("Results: "); print_num(passed); print(" passed, ");
    print_num(failed); puts(" failed");

    if (failed == 0) {
        puts("All TCP tests passed!");
        sys_exit(0);
    }
    puts("Some tests failed!");
    sys_exit(1);
}
