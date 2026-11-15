/**
 * clockidmusl: check the eight clocks, their resolutions, and the sleeps
 * against them.
 *
 * Every row judges what the kernel counts. Time since boot is read within the
 * tick: two readings of `CLOCK_MONOTONIC` differ by less than a tick, boot
 * time is the same clock with no suspend, so a reading of it taken between
 * two of `CLOCK_MONOTONIC` lies between them, raw time never goes back but
 * is checked against no other clock, since the NTP state slews the monotonic
 * clock and not the raw one, as Linux's tkr_raw (kernel/time/
 * timekeeping.c:2044), and the coarse clocks read the time at
 * the last tick the kernel counted (`coarse_clocks`); the CPU clocks advance
 * while this process spins and agree with what `times` reports; a CPU clock by
 * process id, the form `clock_getcpuclockid` returns, reads that process's
 * time, and an id of no process fails with ESRCH.
 * A child blocked after reporting its own CPU time has a clock by id that reads
 * what it reported, or one tick more, and that advances by the three ticks it
 * spins between two reports. Real time and time since boot resolve to a
 * microsecond or less, and the coarse and CPU clocks to the 1 ms tick, and on
 * i686 the 64-bit call answers as the 32-bit one.
 *
 * `clock_nanosleep` is called through `syscall`, since musl answers some clocks
 * itself and uses `nanosleep` for a relative sleep of real time. An absolute
 * sleep must return with its clock at or past the time asked for, of time since
 * boot, boot time and real time alike; a relative one must take at least its
 * interval of time since boot; a time already past returns without waiting. A
 * signal ends a relative sleep with a remainder no greater than the interval,
 * and an absolute one with none written. A sleep on the raw and coarse clocks
 * and the thread CPU clock fails with EOPNOTSUPP, and a bad clock or request
 * with EINVAL. Real time set past 2^31 seconds reads back whole, fine and
 * coarse, and the boot time that /proc/stat and a synthetic node report moves
 * with it; on x86-64 a relative sleep of 2^32 seconds and an absolute one to
 * 2^62 seconds wait until a signal ends them. The caller's CPU clock returns at
 * once for a time it has reached, and a sleep on it for an interval ends once
 * another thread of the process, spinning, has advanced it that far.
 *
 * It prints "CLOCKIDMUSL OK" and exits 0 on success, or prints "CLOCKIDMUSL
 * FAIL (detail)" and exits 1 at the first failure. Given `machine=native`
 * (tests/native/Readme.md), it leaves out the row of real time past 2^31
 * seconds, since it sets the machine's clock.
 *
 * etude's report (probe_report.h), for the case ETUDE_CASE names, has an
 * assertion for each check in order, a setup step joined to the check after it:
 * `within-tick`, `boot-clocks`, `coarse-boot`, `coarse-real`, `res-fine`,
 * `res-coarse`, `res-time64`, `res-null`, `einval-clock`,
 * `process-cpu-advances`, `thread-cpu`, `cpu-times`, `cpuclockid-self`,
 * `cpuclockid-esrch`, `child-clock-reads`, `child-clock-advances`,
 * `absolute-sleep`, `relative-sleep`, `past-time`, `cpu-reached`,
 * `signal-relative`, `relative-remainder`, `signal-absolute`,
 * `absolute-no-remainder`, `eopnotsupp`, `sleep-einval`, `cpu-sleep-ends`,
 * `cpu-sleep-reached`, `wide-real`, `boot-time`, `wide-relative` and
 * `wide-absolute`. `res-time64` is i686's alone, and `wide-relative` and
 * `wide-absolute` x86-64's; the branch of the group `arch`, `i686` or `x86-64`,
 * says which ran. A native run reports `wide-real` and `boot-time` not run. The
 * first failure fails its assertion and leaves the rest not run.
 */

#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <sys/times.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "../probe_args.h"
#include "../probe_report.h"

/*
 * The kernel's timespec of a long's width, which the raw calls take: 32 bits
 * on i386, whose clock_nanosleep and clock_getres of a 64-bit timespec are
 * calls of their own, and 64 on x86-64, which has only the one of each.
 */
struct kts {
    long sec;
    long nsec;
};

#if defined __x86_64__
/* interval: the sleeps are the calls tested */
#define SYS_CLOCK_NANOSLEEP SYS_clock_nanosleep
#define SYS_CLOCK_GETRES    SYS_clock_getres
#else
/* interval: the sleeps are the calls tested */
#define SYS_CLOCK_NANOSLEEP SYS_clock_nanosleep_time32
#define SYS_CLOCK_GETRES    SYS_clock_getres_time32
#endif

/* One tick, the granularity of the coarse and CPU clocks, in nanoseconds. */
#define TICK_NS 4000000L

/* One count of the clock_t that times reports, USER_HZ's 100 a second. */
#define CLOCK_T_NS 10000000L

/* The count of SIGALRM deliveries to on_alarm. */
static volatile sig_atomic_t alarms;

/** Report one failed check and return 1, the probe's failure status. */
static int fail(const char *what)
{
    printf("CLOCKIDMUSL FAIL (%s)\n", what);
    report_stop(what);

    return 1;
}

/** Return the reading of clock `clk` in nanoseconds, or -1 on failure. */
static long long ns_of(clockid_t clk)
{
    struct timespec ts;

    if (clock_gettime(clk, &ts) != 0) {
        return -1;
    }
    return (long long)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

/** Convert a kernel timespec to nanoseconds. */
static long long ns_kts(const struct kts *ts)
{
    return (long long)ts->sec * 1000000000LL + ts->nsec;
}

/** Convert a time in nanoseconds to a kernel timespec. */
static void to_kts(long long ns, struct kts *ts)
{
    ts->sec = (long)(ns / 1000000000LL);
    ts->nsec = (long)(ns % 1000000000LL);
}

/**
 * Make the raw clock_nanosleep with the kernel timespecs `req` and `rem`.
 * Returns 0, or -1 with errno set.
 */
static long sleep_raw(long clk, long flags, const struct kts *req, struct kts *rem)
{
    /* interval: the sleeps are the calls tested */
    return syscall(SYS_CLOCK_NANOSLEEP, clk, flags, req, rem);
}

/**
 * Spend a while in user mode without a system call. Returns the sum the loop
 * computed, so that the compiler keeps the loop.
 */
static unsigned int spin(void)
{
    volatile unsigned int x = 0;
    unsigned int i;

    for (i = 0; i < 200000u; i++) {
        x += i;
    }
    return x;
}

/**
 * Spin until clock `clk` has advanced by `want` ns. Returns how far it
 * advanced, in nanoseconds.
 */
static long long spin_until(clockid_t clk, long long want)
{
    long long start = ns_of(clk);
    long long now = start;

    while (now - start < want) {
        spin();
        now = ns_of(clk);
    }
    return now - start;
}

/** Count a SIGALRM. */
static void on_alarm(int sig)
{
    (void)sig;
    alarms++;
}

/** Arm a single SIGALRM `usec` microseconds from now; 0 disarms the timer. */
static void alarm_in(long usec)
{
    struct itimerval it;

    memset(&it, 0, sizeof(it));
    it.it_value.tv_sec = usec / 1000000;
    it.it_value.tv_usec = usec % 1000000;
    setitimer(ITIMER_REAL, &it, NULL);
}

/**
 * Send SIGALRM every `usec` microseconds until `alarm_in(0)`, so that a sleep
 * begun after one alarm, by a process descheduled between the arming and the
 * call, is still ended by the next.
 */
static void alarm_every(long usec)
{
    struct itimerval it;

    it.it_value.tv_sec = it.it_interval.tv_sec = usec / 1000000;
    it.it_value.tv_usec = it.it_interval.tv_usec = usec % 1000000;
    setitimer(ITIMER_REAL, &it, NULL);
}

/**
 * Check that the coarse clocks read the time at the last tick the kernel
 * counted, the count `times` returns.
 *
 * Coarse time since boot is that count of ticks, read between two calls of
 * `times`, plus the part of a tick, under one, that the NTP state's slew
 * has carried, since a slewed tick counts only once the monotonic clock
 * has gone a whole tick (clock.c's `rtc_tick_advance`), as on the Lenovo,
 * whose ntpd slews the clock; and it is no later than a fine reading after
 * it. The fine clock can be
 * later by any number of ticks, since it counts the ticks of an interrupt not
 * yet taken, as many as an emulator holds back while it does not run the
 * boot processor. Coarse real time is coarse time since boot plus the offset
 * of real time from time since boot, which the fine clocks have too: real time
 * less that offset, read between two readings of `CLOCK_MONOTONIC`, lies
 * between them. The offset is taken from coarse readings within one tick,
 * read again until they are. Returns 0, or 1 after the failure is printed
 * with its readings.
 */
static int coarse_clocks(void)
{
    long long before;
    long long x;
    long long after;
    long long offset;
    long long t0;
    long long t1;

    t0 = (long long)times(NULL) * CLOCK_T_NS;
    x = ns_of(CLOCK_MONOTONIC_COARSE);
    t1 = ((long long)times(NULL) + 1) * CLOCK_T_NS;
    after = ns_of(CLOCK_MONOTONIC);
    if (x < t0 || x >= t1 + TICK_NS || x > after) {
        printf("CLOCKIDMUSL FAIL (coarse time since boot is not time since boot at the last tick: "
               "ticks %lld ns, coarse %lld ns, ticks %lld ns, fine %lld ns)\n",
               t0, x, t1, after);
        report_stop("coarse time since boot is not time since boot at the last tick");
        return 1;
    }
    report_held();
    do {
        before = ns_of(CLOCK_MONOTONIC_COARSE);
        x = ns_of(CLOCK_REALTIME_COARSE);
        after = ns_of(CLOCK_MONOTONIC_COARSE);
    } while (before != after);
    offset = x - before;
    before = ns_of(CLOCK_MONOTONIC);
    x = ns_of(CLOCK_REALTIME) - offset;
    after = ns_of(CLOCK_MONOTONIC);
    if (x < before || x > after) {
        printf("CLOCKIDMUSL FAIL (coarse real time is not real time at the last tick: "
               "offset %lld ns, fine %lld ns, real less offset %lld ns, fine %lld ns)\n",
               offset, before, x, after);
        report_stop("coarse real time is not real time at the last tick");
        return 1;
    }
    report_held();
    return 0;
}

/**
 * Check the CPU clocks: the process's advances while it spins, the thread's
 * reads the process's time, which `times` reports too, and a clock by id
 * reads this process's time and is refused for no process. Returns 0, or 1
 * after the failure is printed.
 */
static int cpu_clocks(void)
{
    struct tms tm;
    clockid_t cpu;
    long long before;
    long long after;
    long long x;

    if (spin_until(CLOCK_PROCESS_CPUTIME_ID, 5 * TICK_NS) < 5 * TICK_NS) {
        return fail("the process CPU clock did not advance while the process spun");
    }
    report_held();
    before = ns_of(CLOCK_THREAD_CPUTIME_ID);
    x = ns_of(CLOCK_PROCESS_CPUTIME_ID);
    after = ns_of(CLOCK_THREAD_CPUTIME_ID);
    if (x < before || x > after) {
        return fail("the thread CPU clock does not read the process's time");
    }
    report_held();
    before = ns_of(CLOCK_PROCESS_CPUTIME_ID);
    times(&tm);
    after = ns_of(CLOCK_PROCESS_CPUTIME_ID);
    /* times truncates the user and the system time each to a clock_t. */
    x = ((long long)tm.tms_utime + tm.tms_stime) * CLOCK_T_NS;
    if (x + 2 * CLOCK_T_NS <= before || x > after) {
        printf("clockidmusl: cpu clock %lld, times %lld, cpu clock %lld\n", before, x, after);
        return fail("the process CPU clock does not read what times reports");
    }
    report_held();
    if (clock_getcpuclockid(0, &cpu) != 0) {
        return fail("clock_getcpuclockid gave no clock for this process");
    }
    before = ns_of(CLOCK_PROCESS_CPUTIME_ID);
    x = ns_of(cpu);
    after = ns_of(CLOCK_PROCESS_CPUTIME_ID);
    if (x < before || x > after) {
        return fail("the CPU clock by id does not read this process's time");
    }
    report_held();
    if (clock_getcpuclockid(999999, &cpu) != ESRCH) {
        return fail("a CPU clock of no such process was not refused");
    }
    report_held();
    return 0;
}

/**
 * Check that boot time reads between two readings of CLOCK_MONOTONIC, and
 * that raw time does not go back over 1000 readings: the NTP state slews the
 * monotonic clock and not the raw one, as Linux's tkr_raw
 * (kernel/time/timekeeping.c:2044), so raw time is held to no other clock.
 * Returns 0, or 1 after the failure is printed.
 */
static int boot_clocks(void)
{
    long long before = ns_of(CLOCK_MONOTONIC);
    long long x = ns_of(CLOCK_BOOTTIME);
    long long after = ns_of(CLOCK_MONOTONIC);
    int i;

    if (x < before || x > after) {
        return fail("boot time does not read CLOCK_MONOTONIC");
    }
    before = ns_of(CLOCK_MONOTONIC_RAW);
    for (i = 0; i < 1000; i++) {
        after = ns_of(CLOCK_MONOTONIC_RAW);
        if (after < before) {
            return fail("raw time went back");
        }
        before = after;
    }
    return 0;
}

/**
 * Check the eight clocks and their resolutions, and then the CPU clocks
 * (`cpu_clocks`). Returns 0, or 1 after the failure is printed.
 */
static int clocks(void)
{
    static const clockid_t fine[] = {CLOCK_REALTIME, CLOCK_MONOTONIC, CLOCK_MONOTONIC_RAW,
                                     CLOCK_BOOTTIME};
    static const clockid_t coarse[] = {CLOCK_PROCESS_CPUTIME_ID, CLOCK_THREAD_CPUTIME_ID,
                                       CLOCK_REALTIME_COARSE, CLOCK_MONOTONIC_COARSE};
    struct timespec res;
    long long before;
    long long after;
    long long step = TICK_NS;
    unsigned int i;

    before = ns_of(CLOCK_MONOTONIC);
    for (i = 0; i < 1000000 && step >= TICK_NS; i++) {
        after = ns_of(CLOCK_MONOTONIC);
        if (after != before) {
            step = after - before;
            before = after;
        }
    }
    if (step >= TICK_NS) {
        return fail("time since boot does not read within the tick");
    }
    report_held();
    if (boot_clocks() != 0) {
        return 1;
    }
    report_held();
    if (coarse_clocks() != 0) {
        return 1;
    }
    for (i = 0; i < sizeof(fine) / sizeof(fine[0]); i++) {
        if (clock_getres(fine[i], &res) != 0 || res.tv_sec != 0 ||
            (res.tv_nsec < 1 || res.tv_nsec > 1000)) {
            return fail("real time or time since boot does not resolve to a microsecond or less");
        }
    }
    report_held();
    for (i = 0; i < sizeof(coarse) / sizeof(coarse[0]); i++) {
        if (clock_getres(coarse[i], &res) != 0 || res.tv_sec != 0 || res.tv_nsec != TICK_NS) {
            return fail("a coarse or CPU clock does not resolve to the tick");
        }
    }
    report_held();
#if defined SYS_clock_getres_time64
    for (i = 0; i <= 7; i++) {
        struct kts r32;
        long long r64[2];

        if (syscall(SYS_clock_getres_time32, (long)i, &r32) != 0 ||
            syscall(SYS_clock_getres_time64, (long)i, r64) != 0 || r64[0] != r32.sec ||
            r64[1] != r32.nsec) {
            return fail("clock_getres_time64 does not answer as clock_getres");
        }
    }
    report_held();
#endif
    if (syscall(SYS_CLOCK_GETRES, (long)CLOCK_MONOTONIC, NULL) != 0) {
        return fail("a resolution asked for with no timespec was refused");
    }
    report_held();
    if (clock_gettime((clockid_t)12, &res) == 0 || errno != EINVAL) {
        return fail("clock 12 was not refused with EINVAL");
    }
    report_held();
    return cpu_clocks();
}

/**
 * Run in the forked child: report its own CPU time on `fd`, wait for a byte
 * on `go`, spin three ticks, report again and wait to be killed. Does not
 * return.
 */
static void child_reports(int fd, int go)
{
    long long own;
    char byte;

    own = ns_of(CLOCK_PROCESS_CPUTIME_ID);
    if (write(fd, &own, sizeof(own)) != sizeof(own) || read(go, &byte, 1) != 1) {
        _exit(1);
    }
    spin_until(CLOCK_PROCESS_CPUTIME_ID, 3 * TICK_NS);
    own = ns_of(CLOCK_PROCESS_CPUTIME_ID);
    if (write(fd, &own, sizeof(own)) != sizeof(own)) {
        _exit(1);
    }
    pause();
    _exit(0);
}

/**
 * Check a child's CPU clock by id, read each time the child has reported its
 * own time and blocked: at least what it reported and at most one tick more,
 * and three ticks further on after it spins. The parent does not run
 * meanwhile, so a clock that read the caller's time does not advance.
 * Returns 0, or 1 after the failure is printed.
 */
static int child_clock(void)
{
    long long own[2];
    long long seen[2];
    clockid_t cpu;
    pid_t child;
    int up[2];
    int down[2];
    int status;
    int bad;

    if (pipe(up) != 0 || pipe(down) != 0) {
        return fail("pipe failed");
    }
    child = fork();
    if (child < 0) {
        return fail("fork failed");
    }
    if (child == 0) {
        child_reports(up[1], down[0]);
    }
    close(up[1]);
    close(down[0]);
    bad = clock_getcpuclockid(child, &cpu) != 0 ||
          read(up[0], &own[0], sizeof(own[0])) != sizeof(own[0]);
    if (!bad) {
        seen[0] = ns_of(cpu);
        bad = write(down[1], "g", 1) != 1 || read(up[0], &own[1], sizeof(own[1])) != sizeof(own[1]);
    }
    if (!bad) {
        seen[1] = ns_of(cpu);
    }
    kill(child, SIGKILL);
    waitpid(child, &status, 0);
    close(up[0]);
    close(down[1]);
    if (bad) {
        return fail("the child's clock or its reports could not be read");
    }
    if (seen[0] < own[0] || seen[0] > own[0] + TICK_NS || seen[1] < own[1] ||
        seen[1] > own[1] + TICK_NS) {
        return fail("a child's CPU clock by id did not read the time the child reported");
    }
    report_held();
    if (seen[1] - seen[0] < 3 * TICK_NS) {
        return fail("a child's CPU clock by id did not advance while the child spun");
    }
    report_held();
    return 0;
}

/**
 * Check the sleeps against each clock, their remainders and their errors.
 * Returns 0, or 1 after the failure is printed.
 */
static int sleeps(void)
{
    static const long nosleep[] = {CLOCK_THREAD_CPUTIME_ID, CLOCK_MONOTONIC_RAW,
                                   CLOCK_REALTIME_COARSE, CLOCK_MONOTONIC_COARSE};
    static const long absolute[] = {CLOCK_MONOTONIC, CLOCK_BOOTTIME, CLOCK_REALTIME};
    struct sigaction sa;
    struct kts req;
    struct kts rem;
    long long due;
    long long start;
    unsigned int i;

    for (i = 0; i < sizeof(absolute) / sizeof(absolute[0]); i++) {
        due = ns_of((clockid_t)absolute[i]) + 50000000LL;
        to_kts(due, &req);
        if (sleep_raw(absolute[i], TIMER_ABSTIME, &req, NULL) != 0) {
            return fail("an absolute sleep did not return 0");
        }
        if (ns_of((clockid_t)absolute[i]) < due) {
            return fail("an absolute sleep returned before its clock read the time asked");
        }
    }
    report_held();
    start = ns_of(CLOCK_MONOTONIC);
    to_kts(50000000LL, &req);
    if (sleep_raw(CLOCK_MONOTONIC, 0, &req, NULL) != 0 ||
        ns_of(CLOCK_MONOTONIC) - start < 50000000LL) {
        return fail("a relative sleep of 50 ms took less than 50 ms of time since boot");
    }
    report_held();
    start = ns_of(CLOCK_MONOTONIC);
    to_kts(start / 2, &req);
    if (sleep_raw(CLOCK_MONOTONIC, TIMER_ABSTIME, &req, NULL) != 0 ||
        ns_of(CLOCK_MONOTONIC) - start > 1000000000LL) {
        return fail("an absolute sleep to a time already past did not return");
    }
    report_held();
    req.sec = 0;
    req.nsec = 0;
    if (sleep_raw(CLOCK_PROCESS_CPUTIME_ID, TIMER_ABSTIME, &req, NULL) != 0) {
        return fail("the CPU clock did not return at once for a time it had reached");
    }
    report_held();

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_alarm;
    sigaction(SIGALRM, &sa, NULL);
    req.sec = 5;
    req.nsec = 0;
    rem.sec = -1;
    rem.nsec = -1;
    alarm_in(100000);
    if (sleep_raw(CLOCK_MONOTONIC, 0, &req, &rem) != -1 || errno != EINTR || alarms != 1) {
        return fail("a signal did not end a relative sleep with EINTR");
    }
    report_held();
    if (rem.sec < 0 || rem.nsec < 0 || rem.nsec >= 1000000000L || ns_kts(&rem) == 0 ||
        ns_kts(&rem) > ns_kts(&req)) {
        return fail("the remainder of a relative sleep is not within its interval");
    }
    report_held();
    to_kts(ns_of(CLOCK_MONOTONIC) + 5000000000LL, &req);
    rem.sec = -1;
    rem.nsec = -1;
    alarm_in(100000);
    if (sleep_raw(CLOCK_MONOTONIC, TIMER_ABSTIME, &req, &rem) != -1 || errno != EINTR ||
        alarms != 2) {
        return fail("a signal did not end an absolute sleep with EINTR");
    }
    report_held();
    if (rem.sec != -1 || rem.nsec != -1) {
        return fail("an absolute sleep a signal ended wrote a remainder");
    }
    report_held();

    to_kts(10000000LL, &req);
    for (i = 0; i < sizeof(nosleep) / sizeof(nosleep[0]); i++) {
        if (sleep_raw(nosleep[i], 0, &req, NULL) != -1 || errno != EOPNOTSUPP) {
            return fail("a clock Linux cannot sleep on was not refused with EOPNOTSUPP");
        }
    }
    report_held();
    if (sleep_raw(12, 0, &req, NULL) != -1 || errno != EINVAL) {
        return fail("a sleep on clock 12 was not refused with EINVAL");
    }
    req.nsec = 1000000000L;
    if (sleep_raw(CLOCK_MONOTONIC, 0, &req, NULL) != -1 || errno != EINVAL) {
        return fail("a sleep of a whole second in nanoseconds was not refused");
    }
    report_held();
    return 0;
}

/* Nonzero while spinner is to go on spinning. */
static volatile int spinning;

/**
 * Run as another thread of the process, whose time the process's CPU clock
 * counts: spin until `spinning` is cleared.
 */
static void *spinner(void *arg)
{
    (void)arg;
    while (spinning) {
        spin();
    }
    return NULL;
}

/**
 * Check that a sleep of 50 ms on the process CPU clock, while another thread
 * of the process spins, returns 0 once the clock has advanced that far. A
 * SIGALRM ten seconds on ends the sleep with EINTR if the clock is not read
 * while the caller sleeps. Returns 0, or 1 after the failure is printed.
 */
static int cpu_sleep_sibling(void)
{
    struct sigaction sa;
    struct kts req;
    pthread_t t;
    long long before;
    long rc;

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_alarm;
    sigaction(SIGALRM, &sa, NULL);
    spinning = 1;
    if (pthread_create(&t, NULL, spinner, NULL) != 0) {
        return fail("a spinning thread could not be made");
    }
    before = ns_of(CLOCK_PROCESS_CPUTIME_ID);
    to_kts(50000000LL, &req);
    alarm_in(10000000);
    rc = sleep_raw(CLOCK_PROCESS_CPUTIME_ID, 0, &req, NULL);
    alarm_in(0);
    spinning = 0;
    pthread_join(t, NULL);
    if (rc != 0) {
        return fail("a sleep on the process CPU clock another thread advanced did not end");
    }
    report_held();
    if (ns_of(CLOCK_PROCESS_CPUTIME_ID) - before < 50000000LL) {
        return fail("a sleep on the process CPU clock ended before the clock reached its time");
    }
    report_held();
    return 0;
}

/** Set real time to `t` and then on by the time since boot from `mono0`. */
static void real_set_back(struct timespec t, const struct timespec *mono0)
{
    struct timespec mono1;

    clock_gettime(CLOCK_MONOTONIC, &mono1);
    t.tv_sec += mono1.tv_sec - mono0->tv_sec;
    t.tv_nsec += mono1.tv_nsec - mono0->tv_nsec;
    if (t.tv_nsec < 0) {
        t.tv_nsec += 1000000000L;
        t.tv_sec--;
    } else if (t.tv_nsec >= 1000000000L) {
        t.tv_nsec -= 1000000000L;
        t.tv_sec++;
    }
    clock_settime(CLOCK_REALTIME, &t);
}

/* The real time the wide row sets: ten seconds past 2^31, early 2038. */
#define PAST_2038 (((time_t)1 << 31) + 10)

/**
 * Return 1 when `clk` reads from `least` seconds to a minute past
 * `PAST_2038`, and 0 otherwise.
 */
static int reads_past_2038(clockid_t clk, time_t least)
{
    struct timespec t;

    return clock_gettime(clk, &t) == 0 && t.tv_sec >= least && t.tv_sec <= PAST_2038 + 60;
}

/**
 * Return NULL when the boot time agrees with the real time less the time since
 * boot to within 2 s, in `btime` of /proc/stat and in the modification time of
 * /dev/null, a synthetic node, and otherwise what disagrees.
 */
static const char *boot_time_disagrees(void)
{
    struct timespec now, mono;
    struct stat st;
    char line[256];
    long long expected, btime = -1;
    FILE *f;

    if (clock_gettime(CLOCK_REALTIME, &now) != 0 || clock_gettime(CLOCK_MONOTONIC, &mono) != 0) {
        return "reading real time and time since boot";
    }
    expected = (long long)now.tv_sec - mono.tv_sec;
    f = fopen("/proc/stat", "r");
    if (f == NULL) {
        return "opening /proc/stat";
    }
    while (fgets(line, sizeof(line), f) != NULL) {
        if (strncmp(line, "btime ", 6) == 0) {
            btime = strtoll(line + 6, NULL, 10);
        }
    }
    fclose(f);
    if (btime < expected - 2 || btime > expected + 2) {
        return "btime of /proc/stat is not the real time less the time since boot";
    }
    if (stat("/dev/null", &st) != 0) {
        return "stat of /dev/null";
    }
    if ((long long)st.st_mtim.tv_sec != btime) {
        return "/dev/null does not report the btime of /proc/stat";
    }
    return NULL;
}

/**
 * Check real time past 32 bits: set to ten seconds past 2^31 it reads back
 * whole, fine and coarse, the coarse clock as much as two ticks earlier, and
 * the boot time moves with it (`boot_time_disagrees`); it is set back after.
 * Returns 0, or 1 after the failure is printed.
 */
static int wide_real_time(void)
{
    struct timespec saved, mono0, past;
    const char *boot;
    int fine, coarse;

    if (clock_gettime(CLOCK_REALTIME, &saved) != 0 || clock_gettime(CLOCK_MONOTONIC, &mono0) != 0) {
        return fail("reading real time and time since boot");
    }
    past.tv_sec = PAST_2038;
    past.tv_nsec = 0;
    if (clock_settime(CLOCK_REALTIME, &past) != 0) {
        return fail("setting real time past 2^31 seconds");
    }
    fine = reads_past_2038(CLOCK_REALTIME, PAST_2038);
    coarse = reads_past_2038(CLOCK_REALTIME_COARSE, PAST_2038 - 1);
    boot = boot_time_disagrees();
    real_set_back(saved, &mono0);
    if (!fine) {
        return fail("real time set past 2^31 seconds does not read back whole");
    }
    if (!coarse) {
        return fail("coarse real time set past 2^31 seconds does not read back whole");
    }
    report_held();
    if (boot != NULL) {
        return fail(boot);
    }
    report_held();
    return 0;
}

/**
 * Check times past 32 bits: the real time of `wide_real_time`, which a native
 * run leaves out, and on x86-64, whose raw calls take
 * a long, a relative sleep of 2^32 seconds and an absolute sleep to 2^62
 * seconds of time since boot, which each wait until a signal ends them.
 * Returns 0, or 1 after the failure is printed.
 */
static int wide_times(void)
{
    if (!probe_native() && wide_real_time() != 0) {
        return 1;
    }
#if defined __x86_64__
    {
        struct sigaction sa;
        struct kts req;
        long r;

        memset(&sa, 0, sizeof(sa));
        sa.sa_handler = on_alarm;
        sigaction(SIGALRM, &sa, NULL);
        req.sec = 1L << 32;
        req.nsec = 0;
        alarms = 0;
        alarm_every(100000);
        /* interval: the sleeps are the calls tested */
        r = syscall(SYS_nanosleep, &req, NULL);
        alarm_in(0);
        if (r != -1 || errno != EINTR || alarms < 1) {
            return fail("a sleep of 2^32 seconds did not wait for the signal");
        }
        report_held();
        req.sec = 1L << 62;
        alarms = 0;
        alarm_every(100000);
        r = sleep_raw(CLOCK_MONOTONIC, TIMER_ABSTIME, &req, NULL);
        alarm_in(0);
        if (r != -1 || errno != EINTR || alarms < 1) {
            return fail("an absolute sleep to 2^62 seconds did not wait for the signal");
        }
        report_held();
    }
#endif
    return 0;
}

/**
 * Begin the report, with the branch of the architecture, and plan its
 * assertions in the order they are checked; a native run reports those of
 * real time past 2^31 seconds not run.
 */
static void plan(void)
{
    static const char *const before[] = {"within-tick", "boot-clocks", "coarse-boot",
                                         "coarse-real", "res-fine",    "res-coarse"};
    static const char *const after[] = {"res-null",
                                        "einval-clock",
                                        "process-cpu-advances",
                                        "thread-cpu",
                                        "cpu-times",
                                        "cpuclockid-self",
                                        "cpuclockid-esrch",
                                        "child-clock-reads",
                                        "child-clock-advances",
                                        "absolute-sleep",
                                        "relative-sleep",
                                        "past-time",
                                        "cpu-reached",
                                        "signal-relative",
                                        "relative-remainder",
                                        "signal-absolute",
                                        "absolute-no-remainder",
                                        "eopnotsupp",
                                        "sleep-einval",
                                        "cpu-sleep-ends",
                                        "cpu-sleep-reached"};
    static const char *const wide[] = {"wide-real", "boot-time"};
    unsigned i;

    report_begin(report_case(environ, "guest.default.clockidmusl"));
#if defined __x86_64__
    report_branch("arch", "x86-64");
#else
    report_branch("arch", "i686");
#endif
    for (i = 0; i < sizeof(before) / sizeof(before[0]); i++) {
        report_plan(before[i]);
    }
#if defined SYS_clock_getres_time64
    report_plan("res-time64");
#endif
    for (i = 0; i < sizeof(after) / sizeof(after[0]); i++) {
        report_plan(after[i]);
    }
    for (i = 0; i < sizeof(wide) / sizeof(wide[0]); i++) {
        if (probe_native()) {
            report_notrun(wide[i], "a native run does not set the machine's clock");
        } else {
            report_plan(wide[i]);
        }
    }
#if defined __x86_64__
    report_plan("wide-relative");
    report_plan("wide-absolute");
#endif
}

int main(int argc, char **argv)
{
    probe_args(argc, argv);
    plan();
    if (clocks() != 0 || child_clock() != 0 || sleeps() != 0 || cpu_sleep_sibling() != 0 ||
        wide_times() != 0) {
        return 1;
    }
    report_end();
    printf("CLOCKIDMUSL OK\n");

    return 0;
}
