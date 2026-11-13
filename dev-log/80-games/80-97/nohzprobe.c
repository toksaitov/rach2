/* Check idle tick counts, clock progress and timed waits, then power off. */
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

/** Write one setting or deliberate lockup to its kernel interface. */
static long write_proc(const char *name, const char *text, long len)
{
    long fd = syscall2(SYS_OPEN, (long)name, 1), rc;

    if (fd < 0) { return fd; }
    rc = syscall3(SYS_WRITE, fd, (long)text, len);
    syscall1(SYS_CLOSE, fd);
    return rc;
}

/** Sum the scheduler timer's counters, excluding other vectors. */
static unsigned long counts(const char *s, const char *name)
{
    const char *line = s, *p, *end;
    unsigned long n, sum;
    int i, j, match, length = 0;

    while (name[length]) { length++; }

    while (*line) {
        end = line;
        while (*end && *end != '\n') { end++; }
        match = 0;
        for (p = line; p + length <= end; p++) {
            if (p != line && p[-1] != ' ' && p[-1] != '\t') { continue; }
            for (j = 0; j < length && p[j] == name[j]; j++) {}
            if (j == length && (p + length == end || p[length] == ' ' || p[length] == '\t')) {
                match = 1;
            }
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

/** Count physical timer interrupts in a shared or split-vector kernel. */
static unsigned long timer_counts(const char *text)
{
    unsigned long ticks = counts(text, "timer"), events = counts(text, "hrtimer");

    return ticks == ~0ul ? ticks : ticks + (events == ~0ul ? 0 : events);
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
    long timex[32] = {0};
    int round;
    int timer = -1;
    struct { long value; int signo; int notify; char pad[64 - sizeof(long) - 8]; } event = {0};
    struct { struct timespec interval; struct timespec value; } setting;
    unsigned long long blocked = 1ull << 13;
    unsigned long long pending = 0;

    check(syscall3(SYS_SCHED_SETAFFINITY, 0, sizeof(mask), (long)&mask) == 0, "pin cpu0");
    req.sec = 0; req.ns = 20000000;
    check(syscall2(SYS_NANOSLEEP, (long)&req, 0) == 0, "settle");
    event.notify = 1;
    check(syscall3(SYS_TIMER_CREATE, 1, (long)&event, (long)&timer) == 0, "create SIGEV_NONE timer");
    setting.interval.sec = 0; setting.interval.ns = 100000;
    setting.value = setting.interval;
    check(syscall4(SYS_TIMER_SETTIME, timer, 0, (long)&setting, 0) == 0, "arm SIGEV_NONE timer");
    check(read_proc("/proc/interrupts", before) > 0, "read interrupt baseline");
    syscall2(SYS_CLOCK_GETTIME, 1, (long)&a);
    req.sec = 6; req.ns = 0;
    check(syscall2(SYS_NANOSLEEP, (long)&req, 0) == 0, "idle nanosleep");
    syscall2(SYS_CLOCK_GETTIME, 1, (long)&b);
    check(read_proc("/proc/interrupts", after) > 0, "read idle interrupts");
    t0 = counts(before, "timer"); t1 = counts(after, "timer");
    print("NOHZPROBE interrupts before\n"); print(before);
    print("NOHZPROBE interrupts after\n"); print(after);
    check(t0 != ~0ul && t1 != ~0ul && t1 >= t0 && t1 - t0 < 128, "idle interval takes no periodic tick");
    check(b.sec - a.sec > 6 || (b.sec - a.sec == 6 && b.ns >= a.ns), "idle clock advances");
    check(syscall2(SYS_TIMER_GETTIME, timer, (long)&setting) == 0 &&
          setting.interval.ns == 100000 && setting.value.sec == 0 &&
          setting.value.ns > 0 && setting.value.ns <= 100000, "SIGEV_NONE advances lazily");
    check(syscall1(SYS_TIMER_DELETE, timer) == 0, "delete SIGEV_NONE timer");
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
    check(read_proc("/proc/interrupts", before) > 0, "read slew baseline");
    t0 = timer_counts(before);
    timex[0] = 2;
    timex[2] = 32768000;
    check(syscall1(SYS_ADJTIMEX, (long)timex) >= 0, "set a 500 ppm slew");
    for (round = 0; round < 4096; round++) {
        if (syscall3(SYS_POLL, 0, 0, 1) != 0) {
            check(0, "one-tick timeout during a slew");
            break;
        }
    }
    check(read_proc("/proc/interrupts", after) > 0, "read slew interrupts");
    t1 = timer_counts(after);
    print("NOHZPROBE slew interrupts before\n"); print(before);
    print("NOHZPROBE slew interrupts after\n"); print(after);
    check(t0 != ~0ul && t1 != ~0ul && t1 >= t0 && t1 - t0 < 4096 * 4 + 128, "one-tick waits take bounded events during a slew");
    timex[0] = 2;
    timex[2] = 0;
    check(syscall1(SYS_ADJTIMEX, (long)timex) >= 0, "restore the clock rate");
    check(write_proc("/proc/sys/kernel/watchdog_thresh", "1\n", 2) == 2,
          "shorten the watchdog sample period");
    req.sec = 1; req.ns = 0;
    check(syscall2(SYS_NANOSLEEP, (long)&req, 0) == 0, "let idle watchdogs restart");
    print("NOHZPROBE watchdog hard begin\n");
    check(write_proc("/sys/kernel/debug/test_lockup", "hard 5\n", 7) == 7,
          "the deliberate hard lockup returns");
    print("NOHZPROBE watchdog hard end\n");
    check(read_proc("/proc/stat", after) > 0, "read idle accounting");
    print("NOHZPROBE stat\n"); print(after);
    print(failures ? "NOHZPROBE FAIL\n" : "NOHZPROBE OK\n");
    syscall4(SYS_REBOOT, (long)0xfee1deadu, 672274793, 0x4321fedc, 0);
    syscall1(SYS_EXIT, failures);
    for (;;) {}
}
