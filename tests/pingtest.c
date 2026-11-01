/**
 * pingtest: the kernel's ICMP echo, through the SC_PING socket operation.
 *
 * Two controls first: three pings to the SLIRP gateway 10.0.2.2 get replies,
 * and one to an address nobody answers fails with ETIMEDOUT. Then the four
 * properties of the sleeping wait, in one fork round, with a child pinging an
 * address nobody answers:
 *
 * Progress. The parent reads the clock in a loop while the child is inside
 * that four-second call, and the readings it takes while the child is alive
 * must span some time. A wait that spun with sti and hlt inside the system call
 * would give the parent no processor for the child's whole call. The child
 * measures its own call and reports the milliseconds through the pipe, and
 * the call may not return before four seconds of the kernel's ticks.
 *
 * Serialization. The child writes a byte just before its ping; the parent
 * reads it, waits until /proc shows the child asleep, which after the byte
 * it is only inside the call, holding the slot, and pings the loopback
 * address, which is refused with -EBUSY. A parent that took the slot first
 * would get its loopback reply and fail the row, and a child that ends
 * without being seen asleep fails it too.
 *
 * The bound. The child exits 0 only when its ping returned -ETIMEDOUT, and 3
 * for any other answer, so a wrong errno is not a pass, and its call lasts at
 * least PING_MIN_MS. A lost wakeup leaves the probe waiting, and its case's
 * watchdog ends it.
 *
 * The release. After the reap the parent's own loopback ping returns a
 * round-trip time of 0 or more, which shows that the slot came back, and is
 * the one row in which the loopback echo answers.
 *
 * Last, the data ping (SC_PING_DATA) refuses 65508 bytes of data with
 * EMSGSIZE, and on x86-64 a size past 32 bits too, whose low word would be a
 * size it takes.
 *
 * A native run (probe_args.h) gives the address that answers as
 * `gateway=ADDRESS`, or else as the peer's, `peer=ADDRESS:PORT`, whose port
 * the probe does not use, and the address nobody answers as
 * `silent=ADDRESS`, in place of 10.0.2.2 and 10.0.2.99; and with
 * `machine=native` the probe measures its times by CLOCK_MONOTONIC rather
 * than the time of day, which the machine's NTP daemon may step.
 *
 * Prints "PINGTEST OK" and exits 0 when every test passed, which the suites
 * that run it match; otherwise exits 1. Beside its own lines it writes
 * etude's report records (probe_report.h), one assertion for each control,
 * each check of the fork round and the data ping, for the case ETUDE_CASE
 * names or guest.ping.pingtest. Freestanding; direct system calls (probe.h).
 */

#include "probe.h"
#include "probe_args.h"
#include "probe_report.h"

typedef unsigned int uint32_t;

#define EMSGSIZE 90

#define STDOUT_FILENO 1

/* IPs in host byte order. */
#define GATEWAY_IP     0x0A000202UL /* 10.0.2.2  */
#define UNREACHABLE_IP 0x0A000263UL /* 10.0.2.99 */
#define LOOPBACK_IP    0x7F000001UL /* 127.0.0.1 */

#define CLOCK_MONOTONIC 1

/* The address that answers and the one nobody answers, GATEWAY_IP and
 * UNREACHABLE_IP unless the arguments name others. */
static uint32_t gateway_ip, silent_ip;

/* What the kernel answers with, negated as a raw system call returns it. */
#define PING_EBUSY     (-16)
#define PING_ETIMEDOUT (-110)

/* A ping at a dead address costs ICMP_PING_ATTEMPTS * ICMP_PING_WAIT (icmp.c),
 * four seconds of the kernel's ticks on every machine: the call, which
 * the child measures, may not return before them, less the tick a reading of
 * the clock may lose. How much longer it took depends on the machine and is
 * printed; a call that never returns is ended by its case's watchdog. */
#define PING_MIN_MS 3990

/* The assertions of the fork round, in the order the parent checks them. */
enum {
    ROW_IN_PING,
    ROW_BUSY,
    ROW_PROGRESS,
    ROW_LENGTH,
    ROW_EXIT,
    ROW_ETIMEDOUT,
    ROW_FLOOR,
    ROW_RELEASE,
    WAIT_ROWS
};

static const char *const wait_rows[WAIT_ROWS] = {
    "child-in-ping",   "busy",       "progress", "child-length", "child-exit",
    "child-etimedout", "wait-floor", "release",
};

/* The first row of the fork round that failed, WAIT_ROWS while none has,
 * and why it failed. */
static int wait_failed = WAIT_ROWS;
static const char *wait_why;

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

/** The state letter of process `pid`, the field after the name in
 * /proc/<pid>/stat, or 0 when it cannot be read. */
static char state_of(int pid)
{
    char path[32] = "/proc/";
    char digits[12], line[256];
    int i = 0, j = 6, fd, n;

    do {
        digits[i++] = (char)('0' + pid % 10);
        pid /= 10;
    } while (pid > 0);
    while (i > 0) {
        path[j++] = digits[--i];
    }
    for (i = 0; "/stat"[i] != '\0'; i++) {
        path[j++] = "/stat"[i];
    }
    path[j] = '\0';
    fd = syscall3(SYS_OPEN, (long)path, 0, 0);
    if (fd < 0) {
        return 0;
    }
    n = sys_read(fd, line, sizeof(line));
    sys_close(fd);
    for (i = n - 1; i >= 0 && line[i] != ')'; i--) {
    }
    return i >= 0 && i + 2 < n && line[i + 1] == ' ' ? line[i + 2] : 0;
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
    /* interval: a child is looked at again every 10 or 50 ms */
    return syscall2(SYS_NANOSLEEP, (long)req, (long)0);
}

/** Milliseconds since the epoch, modulo 100000 seconds, small enough to
 * subtract without overflow; given `machine=native`, since boot by
 * CLOCK_MONOTONIC, likewise. */
static long now_ms(void)
{
    struct timeval_probe tv;

    if (probe_native()) {
        struct timespec_probe ts;

        ts.tv_sec = 0;
        ts.tv_nsec = 0;
        syscall2(SYS_CLOCK_GETTIME, CLOCK_MONOTONIC, (long)&ts);
        return (ts.tv_sec % 100000L) * 1000L + ts.tv_nsec / 1000000L;
    }
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

/** Wait until /proc shows process `pid` asleep, looking every 10 ms. Returns
 * 1 once it is, and 0 when it ends first, as a zombie or gone from /proc. */
static int seen_asleep(int pid)
{
    char state;

    while ((state = state_of(pid)) != 'S') {
        if (state == 0 || state == 'Z') {
            return 0;
        }
        sleep_ms(10);
    }
    return 1;
}

/**
 * Wait for the byte the child writes just before its ping, set `*began` to
 * the time the wait ended, and wait until /proc shows the child asleep in
 * the call.
 * Returns "", or why the row fails.
 */
static const char *child_in_ping(int fd, int child, long *began)
{
    char token;
    int got = sys_read(fd, &token, 1);

    *began = now_ms();
    if (got != 1) {
        return "the child never reached its ping";
    }
    if (!seen_asleep(child)) {
        return "the child was never seen asleep in its ping";
    }
    return "";
}

/** The IPv4 address written in dots at the start of `s`, in host byte order,
 * or `fallback` when `s` is NULL or does not begin with one. */
static uint32_t arg_ip(const char *s, uint32_t fallback)
{
    uint32_t ip = 0;
    long part;
    int i;

    for (i = 0; s != 0 && i < 4; i++) {
        part = probe_number(&s);
        if (part < 0 || part > 255 || (i < 3 && *s++ != '.')) {
            return fallback;
        }
        ip = (ip << 8) | (uint32_t)part;
    }
    return s != 0 ? ip : fallback;
}

/** Print `ip`, in host byte order, in dots. */
static void print_ip(uint32_t ip)
{
    int i;

    for (i = 3; i >= 0; i--) {
        print_num((int)((ip >> (8 * i)) & 0xFF));
        if (i > 0) {
            print(".");
        }
    }
}

/** Record that the row `row` of the fork round failed for the reason `why`,
 * unless an earlier check of the round failed first, and return `why`. */
static const char *wait_fails(int row, const char *why)
{
    if (wait_failed == WAIT_ROWS) {
        wait_failed = row;
        wait_why = why;
    }
    return why;
}

/** Report the rows of the fork round: those before the first that failed
 * held, that one failed with its reason, and those after it were not run. */
static void report_wait_rows(void)
{
    int k;

    for (k = 0; k < WAIT_ROWS; k++) {
        if (k < wait_failed) {
            report_pass(wait_rows[k]);
        } else if (k == wait_failed) {
            report_fail(wait_rows[k], wait_why);
        } else {
            report_notrun(wait_rows[k], "an earlier check of the fork round failed");
        }
    }
}

/** Ping `ip`: the round-trip time in ticks, or a negated errno. */
static int ping(uint32_t ip)
{
    unsigned long args[1];
    args[0] = ip;
    return socketcall(SC_PING, args);
}

/** Print `head`, the address `ip` in dots, and `tail`. */
static void print_test(const char *head, uint32_t ip, const char *tail)
{
    print(head);
    print_ip(ip);
    print(tail);
}

/** Keep the arguments of the initial stack `sp` (probe_args.h) and set the
 * addresses from them: the gateway from `gateway`, else `peer`, else QEMU's,
 * and the silent address from `silent`, else QEMU's. */
static void addresses(unsigned long *sp)
{
    probe_args((int)sp[0], (char **)(sp + 1));
    gateway_ip = arg_ip(probe_arg("gateway", 0), arg_ip(probe_arg("peer", 0), GATEWAY_IP));
    silent_ip = arg_ip(probe_arg("silent", 0), UNREACHABLE_IP);
}

/** Test 1: three pings to the gateway all get replies. Prints the verdict
 * and returns 1 when the test passed. */
static int gateway_replies(void)
{
    int i;

    for (i = 0; i < 3; i++) {
        if (ping(gateway_ip) < 0) {
            puts("FAILED (no reply)");
            report_fail("gateway-replies", "a ping to the gateway got no reply");
            return 0;
        }
    }
    puts("OK");
    report_pass("gateway-replies");
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
    rtt = ping(silent_ip);
    ms = now_ms() - began;
    sys_write(ready[1], &ms, sizeof(ms));
    sys_close(ready[1]);
    sys_exit(rtt == PING_ETIMEDOUT ? 0 : 3);
}

/** Test 4: a data ping of 65508 bytes, and on x86-64 one of a size past 32
 * bits, is refused with EMSGSIZE. Prints the verdict and returns 1 when the
 * test passed. */
static int data_ping_refused(void)
{
    unsigned long args[3];
    int r1, r2 = -EMSGSIZE;

    args[0] = gateway_ip;
    args[1] = 65508;
    args[2] = 64;
    r1 = socketcall(SC_PING_DATA, args);
#if defined __x86_64__
    args[1] = 0x100000001UL;
    r2 = socketcall(SC_PING_DATA, args);
#endif
    if (r1 == -EMSGSIZE && r2 == -EMSGSIZE) {
        puts("OK");
        report_pass("data-size");
        return 1;
    }
    print("FAILED (");
    print_num(r1);
    print(", ");
    print_num(r2);
    puts(")");
    report_fail("data-size", "a data ping past 65507 bytes was not refused with EMSGSIZE");
    return 0;
}

void probe_main(unsigned long *sp);

PROBE_SP_ENTRY;

/** The probe's entry: the four tests in order, then the count of each
 * verdict. */
void probe_main(unsigned long *sp)
{
    int passed = 0, failed = 0, rtt;

    addresses(sp);
    report_begin(report_case((char **)(sp + 2 + sp[0]), "guest.ping.pingtest"));
    puts("ICMP ping test starting...");
    puts("");

    /* Test 1: three pings to the gateway get replies. */
    print_test("Test 1: ping ", gateway_ip, " x3... ");
    if (gateway_replies()) {
        passed++;
    } else {
        failed++;
    }

    /* Test 2: a ping to an address nobody answers fails with ETIMEDOUT. */
    print_test("Test 2: ping ", silent_ip, " times out... ");
    rtt = ping(silent_ip);
    if (rtt == PING_ETIMEDOUT) {
        puts("OK (timeout)");
        report_pass("silent-timeout");
        passed++;
    } else {
        print("FAILED (not ETIMEDOUT, rtt=");
        print_num(rtt);
        puts(")");
        report_fail("silent-timeout", "the unanswered ping did not end in ETIMEDOUT");
        failed++;
    }

    /* Test 3: progress, serialization, the bound and the release. */
    print("Test 3: the wait sleeps, serializes, is bounded, and releases... ");
    {
        int ready[2];
        int child;
        int status = 0;
        int reaped = -1;
        int busy;
        long began, ended, live_span = 0, last_live = 0;
        long child_ms = 0;
        int ok = 1;
        const char *why = "";

        if (sys_pipe(ready) != 0) {
            wait_fails(ROW_IN_PING, "no pipe");
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
                why = wait_fails(ROW_IN_PING, "no fork");
            } else {
                why = child_in_ping(ready[0], child, &began);
                ok = why[0] == '\0';
                if (!ok) {
                    wait_fails(ROW_IN_PING, why);
                }
                if (ok) {
                    /* While the child is in its call, a ping is EBUSY. */
                    busy = ping(LOOPBACK_IP);
                    if (busy != PING_EBUSY) {
                        ok = 0;
                        why = wait_fails(ROW_BUSY, "a second ping was not refused");
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
                    why = wait_fails(ROW_PROGRESS, "nothing else ran while the ping waited");
                }
                if (reaped != child) {
                    reaped = sys_waitpid(child, &status, 0);
                }
                ended = now_ms();
                child_ms = 0;
                if (sys_read(ready[0], &child_ms, sizeof(child_ms)) != (int)sizeof(child_ms)) {
                    ok = 0;
                    why = wait_fails(ROW_LENGTH, "the child did not report its call's length");
                }
                sys_close(ready[0]);
                if (ok && (reaped != child || (status & 0x7f) != 0)) {
                    ok = 0;
                    why = wait_fails(ROW_EXIT, "the pinging child did not exit normally");
                }
                if (ok && ((status >> 8) & 0xff) != 0) {
                    ok = 0;
                    why = wait_fails(ROW_ETIMEDOUT, "the child's ping did not return ETIMEDOUT");
                }
                puts("PINGTEST dead-address call ms:");
                print_num((int)child_ms);
                puts("");
                puts("PINGTEST parent's wait ms:");
                print_num((int)(ended - began));
                puts("");
                if (ok && child_ms < PING_MIN_MS) {
                    ok = 0;
                    why = wait_fails(ROW_FLOOR,
                                     "the wait ended before four seconds of the kernel's ticks");
                }
                if (ok) {
                    /* The release: the slot is free again. */
                    rtt = ping(LOOPBACK_IP);
                    if (rtt < 0) {
                        ok = 0;
                        why = wait_fails(ROW_RELEASE, "the slot was not released");
                    }
                }
            }
            if (ok) {
                puts("OK");
                passed++;
            } else {
                print("FAILED (");
                print(why);
                puts(")");
                failed++;
            }
        }
        report_wait_rows();
    }

    /* Test 4: a data ping past its 65507 bytes is refused before it is sent. */
    print("Test 4: a data ping past 65507 bytes is refused... ");
    if (data_ping_refused()) {
        passed++;
    } else {
        failed++;
    }

    report_end();
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
