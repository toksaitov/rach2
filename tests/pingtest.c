/**
 * Drives the kernel ICMP path through the SC_PING socketcall.
 *
 * Two positive controls first: three pings to the SLIRP gateway 10.0.2.2 must
 * get replies, and one to an unreachable host must time out cleanly rather than
 * hang. Then the four claims the sleeping wait rests on, all in one fork round,
 * with a child pinging an address nobody answers:
 *
 * Progress. The parent samples the clock in a loop while the child is inside
 * that four-second call, and the samples taken while it was alive must span at
 * least two seconds. On a wait that polls with sti/hlt from inside a syscall
 * the parent gets no CPU at all for the child's whole call, so its live-window
 * samples span milliseconds; this is the assertion the kernel change exists
 * for.
 *
 * Serialization. The child writes a byte immediately before its ping; the
 * parent reads it, waits for the child to be certainly inside the call, and
 * then pings the loopback address, which must be refused with exactly -EBUSY.
 * Neither side can score a false pass: a parent that claimed the slot first
 * gets its loopback reply and fails the assertion.
 *
 * The bound. The child exits 0 only if its ping returned exactly -ETIMEDOUT,
 * and a distinct code otherwise, so a wrong errno is not a pass; its whole
 * lifetime must fall between three and eight seconds. A lost wakeup fails as a
 * hung probe, not a green one.
 *
 * The release. After the reap the parent's own loopback ping must return a
 * non-negative round-trip time, which proves the slot came back and is the
 * loopback echo's one end-to-end assertion.
 *
 * Prints "PINGTEST OK" on a clean run, which is what `tests/run ping` matches.
 *
 * Freestanding; direct int 0x80.
 */

typedef unsigned int size_t;
typedef unsigned int uint32_t;

#define STDOUT_FILENO 1

#define SYS_EXIT         1
#define SYS_FORK         2
#define SYS_READ         3
#define SYS_WRITE        4
#define SYS_CLOSE        6
#define SYS_WAITPID      7
#define SYS_PIPE         42
#define SYS_GETTIMEOFDAY 78
#define SYS_SOCKETCALL   102
#define SYS_NANOSLEEP    162

#define SC_PING 0x1000 /* renumbered off Linux getsockopt=15 */

/* IPs in host byte order. */
#define GATEWAY_IP     0x0A000202UL /* 10.0.2.2  */
#define UNREACHABLE_IP 0x0A000263UL /* 10.0.2.99 */
#define LOOPBACK_IP    0x7F000001UL /* 127.0.0.1 */

/* What the kernel answers with, negated the way a raw int 0x80 returns it. */
#define PING_EBUSY     (-16)
#define PING_ETIMEDOUT (-110)

/* A ping at a dead address costs ICMP_PING_ATTEMPTS * ICMP_PING_WAIT, four
 * hundred ticks, which is the kernel's own clock and the same on every
 * machine: the call may not return before them, less the tick the
 * handshake with the child may cost. How long it took past them is the
 * machine's and is printed; a call that never returns is the harness's
 * to end. */
#define PING_MIN_MS 3990

struct timeval_probe {
    long tv_sec;
    long tv_usec;
};

struct timespec_probe {
    long tv_sec;
    long tv_nsec;
};

static int sys_write(int fd, const void *buf, size_t count)
{
    int r;
    __asm__ __volatile__("int $0x80"
                         : "=a"(r)
                         : "0"(SYS_WRITE), "b"(fd), "c"(buf), "d"(count)
                         : "memory");
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
static void puts(const char *s)
{
    print(s);
    sys_write(STDOUT_FILENO, "\n", 1);
}

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

static int sys_fork(void)
{
    int r;
    __asm__ __volatile__("int $0x80" : "=a"(r) : "0"(SYS_FORK));
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

static int sys_pipe(int fds[2])
{
    int r;
    __asm__ __volatile__("int $0x80" : "=a"(r) : "0"(SYS_PIPE), "b"(fds) : "memory");
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

static int sys_gettimeofday(struct timeval_probe *tv)
{
    int r;
    __asm__ __volatile__("int $0x80" : "=a"(r) : "0"(SYS_GETTIMEOFDAY), "b"(tv), "c"(0) : "memory");
    return r;
}

static int sys_nanosleep(const struct timespec_probe *req)
{
    int r;
    __asm__ __volatile__("int $0x80" : "=a"(r) : "0"(SYS_NANOSLEEP), "b"(req), "c"(0) : "memory");
    return r;
}

/** Milliseconds since the epoch, low enough to subtract without overflow. */
static long now_ms(void)
{
    struct timeval_probe tv;

    tv.tv_sec = 0;
    tv.tv_usec = 0;
    sys_gettimeofday(&tv);

    return (tv.tv_sec % 100000L) * 1000L + tv.tv_usec / 1000L;
}

/** Sleep `ms` milliseconds. */
static void sleep_ms(long ms)
{
    struct timespec_probe req;

    req.tv_sec = ms / 1000L;
    req.tv_nsec = (ms % 1000L) * 1000000L;
    sys_nanosleep(&req);
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
            if (rtt < 0) {
                ok = 0;
                break;
            }
        }
        if (ok) {
            puts("OK");
            passed++;
        } else {
            puts("FAILED (no reply)");
            failed++;
        }
    }

    /* Test 2: an unreachable host must time out cleanly, not hang. */
    print("Test 2: ping 10.0.2.99 times out... ");
    rtt = ping(UNREACHABLE_IP);
    if (rtt < 0) {
        puts("OK (timeout)");
        passed++;
    } else {
        print("FAILED (unexpected reply, rtt=");
        print_num(rtt);
        puts(")");
        failed++;
    }

    /* Test 3: one fork round, four claims. The child pings an address nobody
     * answers and the parent watches what it can still do while that runs. */
    print("Test 3: the wait sleeps, serializes, is bounded, and releases... ");
    {
        int ready[2];
        int child;
        int status = 0;
        int reaped;
        int busy;
        long began, ended, live_span = 0, last_live = 0;
        char token = 0;
        int ok = 1;
        const char *why = "";

        if (sys_pipe(ready) != 0) {
            puts("FAILED (no pipe)");
            failed++;
        } else {
            child = sys_fork();
            if (child == 0) {
                sys_close(ready[0]);
                /* The byte goes out immediately before the call, so what the
                 * parent waits on is the child being about to be inside it. */
                sys_write(ready[1], "p", 1);
                sys_close(ready[1]);
                rtt = ping(UNREACHABLE_IP);
                /* Exactly the timeout, not merely a failure: a wrong errno
                 * here would otherwise be scored as a pass. */
                sys_exit(rtt == PING_ETIMEDOUT ? 0 : 3);
            }
            sys_close(ready[1]);
            if (child < 0) {
                ok = 0;
                why = "FAILED (no fork)";
            } else {
                if (sys_read(ready[0], &token, 1) != 1) {
                    ok = 0;
                    why = "FAILED (the child never reached its ping)";
                }
                sys_close(ready[0]);
                began = now_ms();
                if (ok) {
                    /* Certainly inside the call, then the claim must be
                     * refused. A parent that got here first would be answered
                     * by the loopback instead, and fail this. */
                    sleep_ms(200);
                    busy = ping(LOOPBACK_IP);
                    if (busy != PING_EBUSY) {
                        ok = 0;
                        why = "FAILED (a second ping was not refused)";
                    }
                }
                /* Progress: how much wall time the parent could still sample
                 * while the child was alive inside its four-second call. */
                while (ok) {
                    long sample = now_ms();

                    reaped = sys_waitpid(child, &status, 1);
                    if (reaped == child) {
                        break;
                    }
                    if (last_live != 0) {
                        live_span += sample - last_live;
                    }
                    last_live = sample;
                    sleep_ms(50);
                }
                /* The parent ran while the child was inside its call: at
                 * least one sample taken with the child alive. */
                if (ok && live_span == 0) {
                    ok = 0;
                    why = "FAILED (nothing else ran while the ping waited)";
                }
                if (ok && reaped != child) {
                    reaped = sys_waitpid(child, &status, 0);
                }
                ended = now_ms();
                if (ok && (reaped != child || (status & 0x7f) != 0)) {
                    ok = 0;
                    why = "FAILED (the pinging child did not exit normally)";
                }
                if (ok && ((status >> 8) & 0xff) != 0) {
                    ok = 0;
                    why = "FAILED (the child's ping did not return ETIMEDOUT)";
                }
                if (ok && ended - began < PING_MIN_MS) {
                    ok = 0;
                    why = "FAILED (the wait ended before the kernel's four hundred ticks)";
                }
                if (ok) {
                    /* And the slot came back, which is also the loopback
                     * echo's only end-to-end assertion. */
                    rtt = ping(LOOPBACK_IP);
                    if (rtt < 0) {
                        ok = 0;
                        why = "FAILED (the slot was not released)";
                    }
                }
            }
            if (ok) {
                puts("OK");
                passed++;
            } else {
                puts(why);
                failed++;
            }
        }
    }

    puts("");
    print("Results: ");
    print_num(passed);
    print(" passed, ");
    print_num(failed);
    puts(" failed");

    if (failed == 0) {
        puts("All ping tests passed!");
        puts("PINGTEST OK");
        sys_exit(0);
    }
    puts("Some tests failed!");
    sys_exit(1);
}
