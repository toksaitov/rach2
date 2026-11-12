/* Check idle tick counts, clock progress and timed waits, then power off. */
#include "../../../tests/probe.h"

#ifdef __x86_64__
#define SYS_TIMERFD_CREATE 283
#define SYS_TIMERFD_SETTIME 286
#else
#define SYS_TIMERFD_CREATE 322
#define SYS_TIMERFD_SETTIME 325
#endif

struct timespec { long sec; long ns; };
struct timeval { long sec; long us; };
struct itimerval { struct timeval interval; struct timeval value; };
struct pollfd { int fd; short events; short revents; };
static char before[32768], after[32768];
static int failures;

/** Write a literal string to the boot console. */
static void print(const char *s)
{
    long n = 0;

    while (s[n]) { n++; }
    syscall3(SYS_WRITE, 1, (long)s, n);
}

/** Record a failed contract, keeping the remaining checks runnable. */
static void check(int ok, const char *what)
{
    if (!ok) {
        failures++;
        print("NOHZPROBE FAIL: "); print(what); print("\n");
    }
}

/** Read a proc file whole into the fixed buffer; return its length. */
static long read_proc(const char *name, char *buf)
{
    long fd = syscall2(SYS_OPEN, (long)name, 0), n, used = 0;

    if (fd < 0) { return -1; }
    while (used < 32767 && (n = syscall3(SYS_READ, fd, (long)(buf + used), 32767 - used)) > 0) {
        used += n;
    }
    syscall1(SYS_CLOSE, fd);
    buf[used] = 0;
    return used;
}

/** Sum the scheduler timer's counters, excluding other vectors. */
static unsigned long ticks(const char *s)
{
    const char *line = s, *p, *end;
    unsigned long n, sum;
    int i, match;

    while (*line) {
        end = line;
        while (*end && *end != '\n') { end++; }
        match = 0;
        for (p = line; p + 5 <= end; p++) {
            if ((p == line || p[-1] == ' ' || p[-1] == '\t') &&
                p[0] == 't' && p[1] == 'i' && p[2] == 'm' && p[3] == 'e' &&
                p[4] == 'r' && (p + 5 == end || p[5] == ' ')) { match = 1; }
        }
        if (match) {
            p = line;
            while (p < end && *p != ':') { p++; }
            if (p == end) { return ~0ul; }
            p++;
            sum = 0;
            for (i = 0; i < 4; i++) {
                while (p < end && (*p == ' ' || *p == '\t')) { p++; }
                if (*p < '0' || *p > '9') { return i ? sum : ~0ul; }
                n = 0;
                while (p < end && *p >= '0' && *p <= '9') { n = n * 10 + (unsigned)(*p++ - '0'); }
                sum += n;
            }
            return sum;
        }
        line = *end ? end + 1 : end;
    }
    return ~0ul;
}

/** Run the idle interval and the waits with no other user thread runnable. */
PROBE_START void _start(void)
{
    struct timespec req, a, b;
    struct itimerval real;
    unsigned long mask = 1;
    unsigned long t0, t1;
    unsigned int word = 0;
    long fd, rc, count[2] = {0, 0};
    unsigned long long blocked = 1ull << 13;
    unsigned long long pending = 0;

    check(syscall3(SYS_SCHED_SETAFFINITY, 0, sizeof(mask), (long)&mask) == 0, "pin cpu0");
    req.sec = 0; req.ns = 20000000;
    check(syscall2(SYS_NANOSLEEP, (long)&req, 0) == 0, "settle");
    check(read_proc("/proc/interrupts", before) > 0, "read interrupt baseline");
    syscall2(SYS_CLOCK_GETTIME, 1, (long)&a);
    req.sec = 6; req.ns = 0;
    check(syscall2(SYS_NANOSLEEP, (long)&req, 0) == 0, "idle nanosleep");
    syscall2(SYS_CLOCK_GETTIME, 1, (long)&b);
    check(read_proc("/proc/interrupts", after) > 0, "read idle interrupts");
    t0 = ticks(before); t1 = ticks(after);
    print("NOHZPROBE interrupts before\n"); print(before);
    print("NOHZPROBE interrupts after\n"); print(after);
    check(t0 != ~0ul && t1 != ~0ul && t1 >= t0 && t1 - t0 < 16, "idle interval takes no periodic tick");
    check(b.sec - a.sec > 6 || (b.sec - a.sec == 6 && b.ns >= a.ns), "idle clock advances");
    check(syscall3(SYS_POLL, 0, 0, 17) == 0, "poll timeout with no runnable thread");
    req.sec = 0; req.ns = 3100000;
    rc = syscall6(SYS_FUTEX, (long)&word, 128, 1, (long)&req, 0, 0);
    check(rc == -11, "futex value check");
    rc = syscall6(SYS_FUTEX, (long)&word, 128, 0, (long)&req, 0, 0);
    check(rc == -110, "futex timeout with no runnable thread");
    fd = syscall2(SYS_TIMERFD_CREATE, 1, 0);
    check(fd >= 0, "timerfd create");
    if (fd >= 0) {
        struct { struct timespec interval; struct timespec value; } set;

        set.interval.sec = 0; set.interval.ns = 0;
        set.value.sec = 0; set.value.ns = 7000000;
        check(syscall4(SYS_TIMERFD_SETTIME, fd, 0, (long)&set, 0) == 0, "timerfd arm");
        check(syscall3(SYS_READ, fd, (long)count, 8) == 8 && count[0] == 1, "timerfd wakes reader");
        syscall1(SYS_CLOSE, fd);
    }
    check(syscall4(SYS_RT_SIGPROCMASK, 0, (long)&blocked, 0, 8) == 0, "block alarm");
    real.interval.sec = 0; real.interval.us = 0;
    real.value.sec = 0; real.value.us = 11000;
    check(syscall3(SYS_SETITIMER, 0, (long)&real, 0) == 0, "arm real alarm");
    check(syscall3(SYS_POLL, 0, 0, 23) == 0, "sleep past blocked alarm");
    check(syscall2(SYS_GETITIMER, 0, (long)&real) == 0 && real.value.sec == 0 &&
          real.value.us == 0, "real alarm expires without periodic ticks");
    check(syscall2(SYS_RT_SIGPENDING, (long)&pending, 8) == 0 && (pending & blocked),
          "real alarm posts its signal without a periodic tick");
    check(read_proc("/proc/stat", after) > 0, "read idle accounting");
    print("NOHZPROBE stat\n"); print(after);
    print(failures ? "NOHZPROBE FAIL\n" : "NOHZPROBE OK\n");
    syscall4(SYS_REBOOT, (long)0xfee1deadu, 672274793, 0x4321fedc, 0);
    syscall1(SYS_EXIT, failures);
    for (;;) {}
}
