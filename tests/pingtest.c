/**
 * pingtest: the kernel's ICMP echo, through the SC_PING socket operation.
 *
 * Two controls first: three pings to the SLIRP gateway 10.0.2.2 get replies,
 * and one to an address nobody answers fails. Then the four properties of
 * the sleeping wait, in one fork round, with a child pinging an address
 * nobody answers:
 *
 * Progress. The parent reads the clock in a loop while the child is inside
 * that four-second call, and at least one reading must be taken while the
 * child is alive. A wait that spun with sti and hlt inside the system call
 * would give the parent no processor for the child's whole call. The child
 * measures its own call and reports the milliseconds through the pipe, and
 * the call may not return before the kernel's four hundred ticks.
 *
 * Serialization. The child writes a byte just before its ping; the parent
 * reads it, waits 200 ms so that the child is inside the call, and pings the
 * loopback address, which is refused with -EBUSY. A parent that took the
 * slot first would get its loopback reply and fail the row.
 *
 * The bound. The child exits 0 only when its ping returned -ETIMEDOUT, and 3
 * for any other answer, so a wrong errno is not a pass, and its call lasts at
 * least PING_MIN_MS. A lost wakeup leaves the probe waiting, and the harness
 * ends it.
 *
 * The release. After the reap the parent's own loopback ping returns a
 * round-trip time of 0 or more, which shows that the slot came back, and is
 * the one row in which the loopback echo answers.
 *
 * Last, the data ping (SC_PING_DATA) refuses 1473 bytes of data with
 * EMSGSIZE, and on x86-64 a size past 32 bits too, whose low word would be a
 * size it takes.
 *
 * Prints "PINGTEST OK" and exits 0 when every test passed, which the suites
 * that run it match; otherwise exits 1. Freestanding; direct system calls
 * (probe.h).
 */

#include "probe.h"

typedef unsigned int uint32_t;

#define EMSGSIZE 90

#define STDOUT_FILENO 1

/* IPs in host byte order. */
#define GATEWAY_IP     0x0A000202UL /* 10.0.2.2  */
#define UNREACHABLE_IP 0x0A000263UL /* 10.0.2.99 */
#define LOOPBACK_IP    0x7F000001UL /* 127.0.0.1 */

/* What the kernel answers with, negated as a raw system call returns it. */
#define PING_EBUSY     (-16)
#define PING_ETIMEDOUT (-110)

/* A ping at a dead address costs ICMP_PING_ATTEMPTS * ICMP_PING_WAIT (icmp.c),
 * four hundred ticks of the kernel's clock on every machine: the call, which
 * the child measures, may not return before them, less the tick a reading of
 * the clock may lose. How much longer it took depends on the machine and is
 * printed; a call that never returns is ended by the harness. */
#define PING_MIN_MS 3990

struct timeval_probe {
    long tv_sec;
    long tv_usec;
};

struct timespec_probe {
    long tv_sec;
    long tv_nsec;
};

/** write(2). */
static int sys_write(int fd, const void *buf, size_t count)
{
    return syscall3(SYS_WRITE, (long)fd, (long)buf, (long)count);
}

/** exit(2); does not return. */
static void sys_exit(int status)
{
    syscall1(SYS_EXIT, (long)status);
    __builtin_unreachable();
}

/** The socket operation `call` on `args` (`sock_call`), as an int. */
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

/** Write the string `s` to standard output. */
static void print(const char *s)
{
    sys_write(STDOUT_FILENO, s, strlen(s));
}
/** Write the string `s` and a newline to standard output. */
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

/** fork(2). */
static int sys_fork(void)
{
    return syscall0(SYS_FORK);
}

/** read(2). */
static int sys_read(int fd, void *buf, size_t count)
{
    return syscall3(SYS_READ, (long)fd, (long)buf, (long)count);
}

/** close(2). */
static int sys_close(int fd)
{
    return syscall1(SYS_CLOSE, (long)fd);
}

/** pipe(2). */
static int sys_pipe(int fds[2])
{
    return syscall1(SYS_PIPE, (long)fds);
}

/** waitpid(2). */
static int sys_waitpid(int pid, int *status, int options)
{
    return syscall3(SYS_WAITPID, (long)pid, (long)status, (long)options);
}

/** gettimeofday(2) with no zone. */
static int sys_gettimeofday(struct timeval_probe *tv)
{
    return syscall2(SYS_GETTIMEOFDAY, (long)tv, (long)0);
}

/** nanosleep(2) with no remainder. */
static int sys_nanosleep(const struct timespec_probe *req)
{
    return syscall2(SYS_NANOSLEEP, (long)req, (long)0);
}

/** Milliseconds since the epoch, modulo 100000 seconds, small enough to
 * subtract without overflow. */
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

/** Ping `ip`: the round-trip time in ticks, or a negated errno. */
static int ping(uint32_t ip)
{
    unsigned long args[1];
    args[0] = ip;
    return socketcall(SC_PING, args);
}

/** Test 1: three pings to the gateway all get replies. Prints the verdict
 * and returns 1 when the test passed. */
static int gateway_replies(void)
{
    int i;

    for (i = 0; i < 3; i++) {
        if (ping(GATEWAY_IP) < 0) {
            puts("FAILED (no reply)");
            return 0;
        }
    }
    puts("OK");
    return 1;
}

/**
 * The child of test 3: write a byte to `ready` just before it pings an
 * address nobody answers, so that the parent knows it is about to be inside
 * the call; then write the call's length in milliseconds, since an exit
 * status has eight bits and the parent's clock starts after the call does;
 * and exit 0 only when the ping returned -ETIMEDOUT, 3 for any other answer.
 * Does not return.
 */
static void dead_ping_child(int ready[2])
{
    long began, ms;
    int rtt;

    sys_close(ready[0]);
    sys_write(ready[1], "p", 1);
    began = now_ms();
    rtt = ping(UNREACHABLE_IP);
    ms = now_ms() - began;
    sys_write(ready[1], &ms, sizeof(ms));
    sys_close(ready[1]);
    sys_exit(rtt == PING_ETIMEDOUT ? 0 : 3);
}

/** Test 4: a data ping of 1473 bytes, and on x86-64 one of a size past 32
 * bits, is refused with EMSGSIZE. Prints the verdict and returns 1 when the
 * test passed. */
static int data_ping_refused(void)
{
    unsigned long args[3];
    int r1, r2 = -EMSGSIZE;

    args[0] = GATEWAY_IP;
    args[1] = 1473;
    args[2] = 64;
    r1 = socketcall(SC_PING_DATA, args);
#if defined __x86_64__
    args[1] = 0x100000001UL;
    r2 = socketcall(SC_PING_DATA, args);
#endif
    if (r1 == -EMSGSIZE && r2 == -EMSGSIZE) {
        puts("OK");
        return 1;
    }
    print("FAILED (");
    print_num(r1);
    print(", ");
    print_num(r2);
    puts(")");
    return 0;
}

/** The probe's entry: the four tests in order, then the count of each
 * verdict. */
PROBE_START void _start(void)
{
    int passed = 0, failed = 0, rtt;

    puts("ICMP ping test starting...");
    puts("");

    /* Test 1: three pings to the gateway get replies. */
    print("Test 1: ping 10.0.2.2 x3... ");
    if (gateway_replies()) {
        passed++;
    } else {
        failed++;
    }

    /* Test 2: a ping to an address nobody answers fails. */
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

    /* Test 3: progress, serialization, the bound and the release. */
    print("Test 3: the wait sleeps, serializes, is bounded, and releases... ");
    {
        int ready[2];
        int child;
        int status = 0;
        int reaped;
        int busy;
        long began, ended, live_span = 0, last_live = 0;
        long child_ms = 0;
        char token = 0;
        int ok = 1;
        const char *why = "";

        if (sys_pipe(ready) != 0) {
            puts("FAILED (no pipe)");
            failed++;
        } else {
            child = sys_fork();
            if (child == 0) {
                dead_ping_child(ready);
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
                began = now_ms();
                if (ok) {
                    /* While the child is in its call, a ping is EBUSY. */
                    sleep_ms(200);
                    busy = ping(LOOPBACK_IP);
                    if (busy != PING_EBUSY) {
                        ok = 0;
                        why = "FAILED (a second ping was not refused)";
                    }
                }
                /* Progress: the time the parent ran while the child waited. */
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
                if (ok && live_span == 0) {
                    ok = 0;
                    why = "FAILED (nothing else ran while the ping waited)";
                }
                if (ok && reaped != child) {
                    reaped = sys_waitpid(child, &status, 0);
                }
                ended = now_ms();
                child_ms = 0;
                if (sys_read(ready[0], &child_ms, sizeof(child_ms)) != (int)sizeof(child_ms)) {
                    ok = 0;
                    why = "FAILED (the child did not report its call's length)";
                }
                sys_close(ready[0]);
                if (ok && (reaped != child || (status & 0x7f) != 0)) {
                    ok = 0;
                    why = "FAILED (the pinging child did not exit normally)";
                }
                if (ok && ((status >> 8) & 0xff) != 0) {
                    ok = 0;
                    why = "FAILED (the child's ping did not return ETIMEDOUT)";
                }
                puts("PINGTEST dead-address call ms:");
                print_num((int)child_ms);
                puts("");
                puts("PINGTEST parent's wait ms:");
                print_num((int)(ended - began));
                puts("");
                if (ok && child_ms < PING_MIN_MS) {
                    ok = 0;
                    why = "FAILED (the wait ended before the kernel's four hundred ticks)";
                }
                if (ok) {
                    /* The release: the slot is free again. */
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

    /* Test 4: a data ping past its 1472 bytes is refused before it is sent. */
    print("Test 4: a data ping past 1472 bytes is refused... ");
    if (data_ping_refused()) {
        passed++;
    } else {
        failed++;
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
