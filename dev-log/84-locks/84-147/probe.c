/* Measure the 250 Hz tick and sub-tick waits, then power off. */
#include "../../../tests/probe.h"

#ifdef __x86_64__
#define SYS_ADJTIMEX 159
#define SYS_TIMERFD_CREATE 283
#define SYS_TIMERFD_SETTIME 286
#define SYS_TIMER_CREATE 222
#define SYS_TIMER_SETTIME 223
#define SYS_TIMER_GETTIME 224
#define SYS_TIMER_DELETE 226
#else
#define SYS_ADJTIMEX 124
#define SYS_TIMERFD_CREATE 322
#define SYS_TIMERFD_SETTIME 325
#define SYS_TIMER_CREATE 259
#define SYS_TIMER_SETTIME 260
#define SYS_TIMER_GETTIME 261
#define SYS_TIMER_DELETE 263
#endif

struct timespec { long sec; long ns; };
struct timeval { long sec; long us; };
struct itimerval { struct timeval interval; struct timeval value; };
struct pollfd { int fd; short events; short revents; };
static char after[32768];
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
        print("TICK250 FAIL: "); print(what); print("\n");
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

/** Read a monotonic time within the first seconds of this private boot. */
static unsigned long stamp(void)
{
    struct timespec t;

    syscall2(SYS_CLOCK_GETTIME, 1, (long)&t);
    return (unsigned long)t.sec * 1000000000ul + (unsigned long)t.ns;
}

/** Print an unsigned measurement without libc. */
static void number(unsigned long n)
{
    char b[24];
    int k = 0;

    do { b[k++] = (char)('0' + n % 10); n /= 10; } while (n);
    while (k) { syscall3(SYS_WRITE, 1, (long)&b[--k], 1); }
    print("\n");
}

/** Check coarse resolution and the conversion of CPU ticks to USER_HZ. */
static void accounting(void)
{
    struct timespec res, a, b;
    unsigned long start, elapsed;
    long ticks[4], ticks_after[4];
#ifdef __x86_64__
    const int getres = 229;
#else
    const int getres = 266;
#endif

    check(syscall2(getres, 6, (long)&res) == 0 && res.sec == 0 && res.ns == 4000000,
          "coarse resolution is 4 ms");
    check(syscall2(getres, 2, (long)&res) == 0 && res.sec == 0 && res.ns == 4000000,
          "CPU accounting resolution is 4 ms");
    syscall1(SYS_TIMES, (long)ticks);
    syscall2(SYS_CLOCK_GETTIME, 2, (long)&a);
    start = stamp();
    do { elapsed = stamp() - start; } while (elapsed < 1000000000ul);
    syscall2(SYS_CLOCK_GETTIME, 2, (long)&b);
    syscall1(SYS_TIMES, (long)ticks_after);
    elapsed = (unsigned long)(b.sec - a.sec) * 1000000000ul + b.ns - a.ns;
    start = (unsigned long)(ticks_after[0] + ticks_after[1] - ticks[0] - ticks[1]) * 10000000ul;
    check(elapsed > start ? elapsed - start < 20000000ul : start - elapsed < 20000000ul,
          "times and CPU clock use the same rate");
    print("TICK250 busy CPU ns: "); number(elapsed);
    print("TICK250 busy USER_HZ ns: "); number(start);
}

/** Measure waits shorter than one tick and count periodic fine timer events. */
static void fine_waits(void)
{
    struct timespec req = {0, 1000000};
    struct timeval sel = {0, 700};
    struct { struct timespec interval; struct timespec value; } setting = {{0, 250000}, {0, 250000}};
    unsigned long start, elapsed, fast = 0;
    unsigned long long expirations = 0;
    long fd;
    int i;

    for (i = 0; i < 128; i++) {
        start = stamp();
        check(syscall2(SYS_NANOSLEEP, (long)&req, 0) == 0, "one millisecond nanosleep");
        elapsed = stamp() - start;
        check(elapsed >= 1000000ul, "nanosleep never early");
        if (elapsed < 4000000ul) { fast++; }
    }
    print("TICK250 one-ms sleeps under one tick: "); number(fast);
    check(syscall5(SYS_SELECT, 0, 0, 0, 0, (long)&sel) == 0, "subtick select");
    fd = syscall2(SYS_TIMERFD_CREATE, 1, 0);
    check(fd >= 0, "timerfd create");
    if (fd >= 0) {
        check(syscall4(SYS_TIMERFD_SETTIME, fd, 0, (long)&setting, 0) == 0, "250 us timerfd");
        req.ns = 2000000;
        check(syscall2(SYS_NANOSLEEP, (long)&req, 0) == 0, "timerfd interval wait");
        check(syscall3(SYS_READ, fd, (long)&expirations, 8) == 8 && expirations >= 8,
              "timerfd counts subtick periods");
        syscall1(SYS_CLOSE, fd);
    }
}

/** Keep real interval timers in microseconds while CPU ticks are four ms. */
static void fine_alarm(void)
{
    struct timespec req = {0, 2000000};
    struct itimerval real = {{0, 500}, {0, 500}};
    unsigned long long blocked = 1ull << 13, pending = 0;

    check(syscall4(SYS_RT_SIGPROCMASK, 0, (long)&blocked, 0, 8) == 0, "block alarm");
    check(syscall3(SYS_SETITIMER, 0, (long)&real, 0) == 0, "500 us real interval timer");
    check(syscall2(SYS_GETITIMER, 0, (long)&real) == 0 && real.interval.us == 500,
          "real interval retains microseconds");
    check(syscall2(SYS_NANOSLEEP, (long)&req, 0) == 0, "wait past real alarm");
    check(syscall2(SYS_RT_SIGPENDING, (long)&pending, 8) == 0 && (pending & blocked),
          "real alarm posts between ticks");
    real.value.sec = real.value.us = real.interval.sec = real.interval.us = 0;
    check(syscall3(SYS_SETITIMER, 0, (long)&real, 0) == 0, "disarm real timer");
}

/** Run only this task's checks and power off the private guest. */
PROBE_START void _start(void)
{
    unsigned long mask = 1;

    check(syscall3(SYS_SCHED_SETAFFINITY, 0, sizeof(mask), (long)&mask) == 0, "pin cpu0");
    accounting();
    fine_waits();
    fine_alarm();
    check(read_proc("/proc/interrupts", after) > 0, "read interrupts");
    print(after);
    print(failures ? "TICK250 FAIL\n" : "TICK250 OK\n");
    syscall4(SYS_REBOOT, (long)0xfee1deadu, 672274793, 0x4321fedc, 0);
    syscall1(SYS_EXIT, failures);
    for (;;) {}
}
