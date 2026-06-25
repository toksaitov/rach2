/**
 * Drives the kernel ICMP path through the SC_PING socketcall. Three pings to
 * the SLIRP gateway 10.0.2.2 must get replies; one to an unreachable host must
 * time out cleanly rather than hang.
 *
 * Freestanding; direct int 0x80.
 */

typedef unsigned int size_t;
typedef unsigned int uint32_t;

#define STDOUT_FILENO 1

#define SYS_EXIT       1
#define SYS_WRITE      4
#define SYS_SOCKETCALL 102

#define SC_PING 0x1000  /* renumbered off Linux getsockopt=15 */

/* IPs in host byte order. */
#define GATEWAY_IP     0x0A000202UL   /* 10.0.2.2  */
#define UNREACHABLE_IP 0x0A000263UL   /* 10.0.2.99 */

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

static int ping(uint32_t ip)
{
    uint32_t args[1];
    args[0] = ip;
    return socketcall(SC_PING, args);
}

void _start(void)
{
    int passed = 0, failed = 0, i, rtt;

    puts("ICMP ping test starting...");
    puts("");

    /* Test 1: three pings to the gateway should all get replies. */
    print("Test 1: ping 10.0.2.2 x3... ");
    {
        int ok = 1;
        for (i = 0; i < 3; i++) {
            rtt = ping(GATEWAY_IP);
            if (rtt < 0) { ok = 0; break; }
        }
        if (ok) { puts("OK"); passed++; }
        else { puts("FAILED (no reply)"); failed++; }
    }

    /* Test 2: an unreachable host must time out cleanly, not hang. */
    print("Test 2: ping 10.0.2.99 times out... ");
    rtt = ping(UNREACHABLE_IP);
    if (rtt < 0) { puts("OK (timeout)"); passed++; }
    else { print("FAILED (unexpected reply, rtt="); print_num(rtt); puts(")"); failed++; }

    puts("");
    print("Results: "); print_num(passed); print(" passed, ");
    print_num(failed); puts(" failed");

    if (failed == 0) {
        puts("All ping tests passed!");
        sys_exit(0);
    }
    puts("Some tests failed!");
    sys_exit(1);
}
