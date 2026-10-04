/**
 * The eight clocks, their resolutions, and the sleeps against them.
 *
 * Every row judges what the kernel counts. Time since boot is read within the
 * tick: two readings of `CLOCK_MONOTONIC` differ by less than a tick, raw and
 * boot time are the same clock, so a reading of either taken between two of
 * `CLOCK_MONOTONIC` lies between them, and the coarse clocks are no later than
 * their fine ones and no more than two ticks earlier, one for the tick in
 * progress and one for a tick the fine clock counts before its interrupt is
 * taken; the CPU clocks advance while this process spins and agree with what
 * `times` reports; a CPU clock by process id, the form `clock_getcpuclockid`
 * returns, reads that process's time, and an id of no process is refused. A
 * child blocked after reporting its own CPU time has a clock by id that reads
 * what it reported, or one tick more, and that advances by the three ticks it
 * spins between two reports. Real time and time since boot resolve to the
 * microsecond, and the coarse and CPU clocks to the 10 ms tick, and on i686 the
 * 64-bit call answers as the 32-bit one.
 *
 * `clock_nanosleep` is called through `syscall`, since musl answers some clocks
 * itself and uses `nanosleep` for a relative sleep of real time. An absolute
 * sleep must return with its clock at or past the time asked for, of time since
 * boot, boot time and real time alike; a relative one must take at least its
 * interval of time since boot; a time already past returns without waiting. A
 * signal ends a relative sleep with a remainder no greater than the interval,
 * and an absolute one with none written. The raw and coarse clocks and the
 * thread CPU clock cannot be slept on, and a bad clock or request is refused.
 * Real time set past 2^31 seconds reads back whole, fine and coarse; on x86-64
 * a relative sleep of 2^32 seconds and an absolute one to 2^62 seconds wait
 * until a signal ends them. The caller's CPU clock returns at once for a time
 * it has reached, and a sleep on it for an interval ends once another thread of
 * the process, spinning, has advanced it that far.
 *
 * Prints "CLOCKIDMUSL OK" on success and "CLOCKIDMUSL FAIL (detail)" otherwise.
 */

#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <sys/times.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

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
#define SYS_CLOCK_NANOSLEEP SYS_clock_nanosleep
#define SYS_CLOCK_GETRES    SYS_clock_getres
#else
#define SYS_CLOCK_NANOSLEEP SYS_clock_nanosleep_time32
#define SYS_CLOCK_GETRES    SYS_clock_getres_time32
#endif

/* One tick, the granularity of the coarse and CPU clocks, in nanoseconds. */
#define TICK_NS 10000000L

/* The most spinning a loop may take to be charged what it asks for, in
 * nanoseconds of time since boot. */
#define SPIN_BOUND_NS 10000000000LL

static volatile sig_atomic_t alarms;

/** Report one failed check and return 1, the probe's failure status. */
static int fail(const char *what)
{
    printf("CLOCKIDMUSL FAIL (%s)\n", what);

    return 1;
}

/** A clock's reading in nanoseconds, or -1 when it cannot be read. */
static long long ns_of(clockid_t clk)
{
    struct timespec ts;

    if (clock_gettime(clk, &ts) != 0) {
        return -1;
    }
    return (long long)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

/** A kernel timespec as nanoseconds. */
static long long ns_kts(const struct kts *ts)
{
    return (long long)ts->sec * 1000000000LL + ts->nsec;
}

/** A time in nanoseconds as a kernel timespec. */
static void to_kts(long long ns, struct kts *ts)
{
    ts->sec = (long)(ns / 1000000000LL);
    ts->nsec = (long)(ns % 1000000000LL);
}

/** The raw clock_nanosleep: 0, or -1 with errno set. */
static long sleep_raw(long clk, long flags, const struct kts *req, struct kts *rem)
{
    return syscall(SYS_CLOCK_NANOSLEEP, clk, flags, req, rem);
}

/** Spend a while in user mode without a system call. */
static unsigned int spin(void)
{
    volatile unsigned int x = 0;
    unsigned int i;

    for (i = 0; i < 200000u; i++) {
        x += i;
    }
    return x;
}

/** Spin until clock `clk` has advanced by `want` ns, or for SPIN_BOUND_NS of
 * time since boot; returns how far it advanced. */
static long long spin_until(clockid_t clk, long long want)
{
    long long start = ns_of(clk);
    long long began = ns_of(CLOCK_MONOTONIC);
    long long now = start;

    while (now - start < want && ns_of(CLOCK_MONOTONIC) - began < SPIN_BOUND_NS) {
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

/** Arm a single SIGALRM `usec` microseconds from now. */
static void alarm_in(long usec)
{
    struct itimerval it;

    memset(&it, 0, sizeof(it));
    it.it_value.tv_sec = usec / 1000000;
    it.it_value.tv_usec = usec % 1000000;
    setitimer(ITIMER_REAL, &it, NULL);
}

/** Send SIGALRM every `usec` microseconds until `alarm_in(0)`: a sleep
 * begun after one alarm, by a process descheduled between the arming and
 * the call, is still ended by the next. */
static void alarm_every(long usec)
{
    struct itimerval it;

    it.it_value.tv_sec = it.it_interval.tv_sec = usec / 1000000;
    it.it_value.tv_usec = it.it_interval.tv_usec = usec % 1000000;
    setitimer(ITIMER_REAL, &it, NULL);
}

/** The eight clocks, their resolutions, and the CPU clocks by id. */
static int clocks(void)
{
    static const clockid_t boot_clocks[] = {CLOCK_MONOTONIC_RAW, CLOCK_BOOTTIME};
    static const clockid_t fine[] = {CLOCK_REALTIME, CLOCK_MONOTONIC, CLOCK_MONOTONIC_RAW,
                                     CLOCK_BOOTTIME};
    static const clockid_t coarse[] = {CLOCK_PROCESS_CPUTIME_ID, CLOCK_THREAD_CPUTIME_ID,
                                       CLOCK_REALTIME_COARSE, CLOCK_MONOTONIC_COARSE};
    struct timespec res;
    struct tms tm;
    clockid_t cpu;
    long long before;
    long long after;
    long long x;
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
    for (i = 0; i < sizeof(boot_clocks) / sizeof(boot_clocks[0]); i++) {
        before = ns_of(CLOCK_MONOTONIC);
        x = ns_of(boot_clocks[i]);
        after = ns_of(CLOCK_MONOTONIC);
        if (x < before || x > after) {
            return fail("a clock of time since boot does not read CLOCK_MONOTONIC");
        }
    }
    before = ns_of(CLOCK_MONOTONIC);
    x = ns_of(CLOCK_MONOTONIC_COARSE);
    after = ns_of(CLOCK_MONOTONIC);
    if (x > after || x < before - 2 * TICK_NS) {
        return fail("coarse time since boot is not time since boot at the last tick");
    }
    before = ns_of(CLOCK_REALTIME);
    x = ns_of(CLOCK_REALTIME_COARSE);
    after = ns_of(CLOCK_REALTIME);
    if (x > after || x < before - 2 * TICK_NS) {
        return fail("coarse real time is not real time at the last tick");
    }
    for (i = 0; i < sizeof(fine) / sizeof(fine[0]); i++) {
        if (clock_getres(fine[i], &res) != 0 || res.tv_sec != 0 || res.tv_nsec != 1000) {
            return fail("real time or time since boot does not resolve to the microsecond");
        }
    }
    for (i = 0; i < sizeof(coarse) / sizeof(coarse[0]); i++) {
        if (clock_getres(coarse[i], &res) != 0 || res.tv_sec != 0 || res.tv_nsec != TICK_NS) {
            return fail("a coarse or CPU clock does not resolve to the tick");
        }
    }
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
#endif
    if (syscall(SYS_CLOCK_GETRES, (long)CLOCK_MONOTONIC, NULL) != 0) {
        return fail("a resolution asked for with no timespec was refused");
    }
    if (clock_gettime((clockid_t)12, &res) == 0 || errno != EINVAL) {
        return fail("clock 12 was not refused with EINVAL");
    }

    if (spin_until(CLOCK_PROCESS_CPUTIME_ID, 5 * TICK_NS) < 5 * TICK_NS) {
        return fail("the process CPU clock did not advance while the process spun");
    }
    before = ns_of(CLOCK_THREAD_CPUTIME_ID);
    x = ns_of(CLOCK_PROCESS_CPUTIME_ID);
    after = ns_of(CLOCK_THREAD_CPUTIME_ID);
    if (x < before || x > after) {
        return fail("the thread CPU clock does not read the process's time");
    }
    before = ns_of(CLOCK_PROCESS_CPUTIME_ID);
    times(&tm);
    after = ns_of(CLOCK_PROCESS_CPUTIME_ID);
    x = ((long long)tm.tms_utime + tm.tms_stime) * TICK_NS;
    if (x < before || x > after) {
        return fail("the process CPU clock does not read what times reports");
    }
    if (clock_getcpuclockid(0, &cpu) != 0) {
        return fail("clock_getcpuclockid gave no clock for this process");
    }
    before = ns_of(CLOCK_PROCESS_CPUTIME_ID);
    x = ns_of(cpu);
    after = ns_of(CLOCK_PROCESS_CPUTIME_ID);
    if (x < before || x > after) {
        return fail("the CPU clock by id does not read this process's time");
    }
    if (clock_getcpuclockid(999999, &cpu) != ESRCH) {
        return fail("a CPU clock of no such process was not refused");
    }
    return 0;
}

/** Report the child's own CPU time on `fd`, wait for a byte on `go`, spin
 * three ticks, report again and wait to be killed. */
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

/** A child's CPU clock by id, read each time the child has reported its own
 * time and blocked: at least what it reported and at most one tick more, and
 * three ticks further on after it spins. The parent does not run meanwhile,
 * so a clock that read the caller's time does not advance. */
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
    if (seen[1] - seen[0] < 3 * TICK_NS) {
        return fail("a child's CPU clock by id did not advance while the child spun");
    }
    return 0;
}

/** The sleeps against each clock, their remainders and their refusals. */
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
    start = ns_of(CLOCK_MONOTONIC);
    to_kts(50000000LL, &req);
    if (sleep_raw(CLOCK_MONOTONIC, 0, &req, NULL) != 0 ||
        ns_of(CLOCK_MONOTONIC) - start < 50000000LL) {
        return fail("a relative sleep of 50 ms took less than 50 ms of time since boot");
    }
    start = ns_of(CLOCK_MONOTONIC);
    to_kts(start - 1000000000LL, &req);
    if (sleep_raw(CLOCK_MONOTONIC, TIMER_ABSTIME, &req, NULL) != 0 ||
        ns_of(CLOCK_MONOTONIC) - start > 1000000000LL) {
        return fail("an absolute sleep to a time already past did not return");
    }
    req.sec = 0;
    req.nsec = 0;
    if (sleep_raw(CLOCK_PROCESS_CPUTIME_ID, TIMER_ABSTIME, &req, NULL) != 0) {
        return fail("the CPU clock did not return at once for a time it had reached");
    }

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
    if (rem.sec < 0 || rem.nsec < 0 || rem.nsec >= 1000000000L || ns_kts(&rem) == 0 ||
        ns_kts(&rem) > ns_kts(&req)) {
        return fail("the remainder of a relative sleep is not within its interval");
    }
    to_kts(ns_of(CLOCK_MONOTONIC) + 5000000000LL, &req);
    rem.sec = -1;
    rem.nsec = -1;
    alarm_in(100000);
    if (sleep_raw(CLOCK_MONOTONIC, TIMER_ABSTIME, &req, &rem) != -1 || errno != EINTR ||
        alarms != 2) {
        return fail("a signal did not end an absolute sleep with EINTR");
    }
    if (rem.sec != -1 || rem.nsec != -1) {
        return fail("an absolute sleep a signal ended wrote a remainder");
    }

    to_kts(10000000LL, &req);
    for (i = 0; i < sizeof(nosleep) / sizeof(nosleep[0]); i++) {
        if (sleep_raw(nosleep[i], 0, &req, NULL) != -1 || errno != EOPNOTSUPP) {
            return fail("a clock Linux cannot sleep on was not refused with EOPNOTSUPP");
        }
    }
    if (sleep_raw(12, 0, &req, NULL) != -1 || errno != EINVAL) {
        return fail("a sleep on clock 12 was not refused with EINVAL");
    }
    req.nsec = 1000000000L;
    if (sleep_raw(CLOCK_MONOTONIC, 0, &req, NULL) != -1 || errno != EINVAL) {
        return fail("a sleep of a whole second in nanoseconds was not refused");
    }
    return 0;
}

static volatile int spinning;

/** Spin until `spinning` is cleared: another thread of the process, whose
 * time the process's CPU clock counts. */
static void *spinner(void *arg)
{
    (void)arg;
    while (spinning) {
        spin();
    }
    return NULL;
}

/**
 * A sleep of 50 ms on the process CPU clock, while another thread of the
 * process spins, returns 0 once the clock has advanced that far. A SIGALRM
 * ten seconds on ends the sleep with EINTR if the clock is not read while
 * the caller sleeps.
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
    if (ns_of(CLOCK_PROCESS_CPUTIME_ID) - before < 50000000LL) {
        return fail("a sleep on the process CPU clock ended before the clock reached its time");
    }
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

/** Whether `clk` reads from `least` seconds to a minute past PAST_2038. */
static int reads_past_2038(clockid_t clk, time_t least)
{
    struct timespec t;

    return clock_gettime(clk, &t) == 0 && t.tv_sec >= least && t.tv_sec <= PAST_2038 + 60;
}

/*
 * Times past 32 bits. Real time set to ten seconds past 2^31 reads back
 * whole, fine and coarse, the coarse clock as much as two ticks earlier, and
 * is set back after. On x86-64, whose raw calls
 * take a long, a relative sleep of 2^32 seconds and an absolute sleep to 2^62
 * seconds of time since boot each wait until a signal ends them.
 */
static int wide_times(void)
{
    struct timespec saved, mono0, past;
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
    real_set_back(saved, &mono0);
    if (!fine) {
        return fail("real time set past 2^31 seconds does not read back whole");
    }
    if (!coarse) {
        return fail("coarse real time set past 2^31 seconds does not read back whole");
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
        r = syscall(SYS_nanosleep, &req, NULL);
        alarm_in(0);
        if (r != -1 || errno != EINTR || alarms < 1) {
            return fail("a sleep of 2^32 seconds did not wait for the signal");
        }
        req.sec = 1L << 62;
        alarms = 0;
        alarm_every(100000);
        r = sleep_raw(CLOCK_MONOTONIC, TIMER_ABSTIME, &req, NULL);
        alarm_in(0);
        if (r != -1 || errno != EINTR || alarms < 1) {
            return fail("an absolute sleep to 2^62 seconds did not wait for the signal");
        }
    }
#endif
    return 0;
}

int main(void)
{
    if (clocks() != 0 || child_clock() != 0 || sleeps() != 0 || cpu_sleep_sibling() != 0 ||
        wide_times() != 0) {
        return 1;
    }
    printf("CLOCKIDMUSL OK\n");

    return 0;
}
