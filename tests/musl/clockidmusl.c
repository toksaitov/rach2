/**
 * The eight clocks, their resolutions, and the sleeps against them.
 *
 * Every row judges what the kernel counts. The clocks of time since boot are
 * one clock under four names, so each reading taken between two readings of
 * `CLOCK_MONOTONIC` lies between them; coarse real time is no later than real
 * time and no more than two ticks earlier, one for the tick in progress and
 * one for a tick the fine clock counts before its interrupt is taken; the CPU
 * clocks advance while this process spins and agree with
 * what `times` reports; a CPU clock by process id, the form
 * `clock_getcpuclockid` returns, reads that process's time, and an id of no
 * process is refused. Real time resolves to the microsecond and every other
 * clock to the 10 ms tick.
 *
 * `clock_nanosleep` is called through `syscall`, since musl answers some
 * clocks itself and uses `nanosleep` for a relative sleep of real time. An
 * absolute sleep must return with its clock at or past the time asked for, of
 * time since boot, boot time and real time alike; a relative one must take at
 * least its interval of time since boot; a time already past returns without
 * waiting. A signal ends a relative sleep with a remainder no greater than the
 * interval, and an absolute one with none written. The raw and coarse clocks
 * and the thread CPU clock cannot be slept on, and a bad clock or request is
 * refused. The caller's CPU clock returns at once for a time it has reached.
 *
 * Prints "CLOCKIDMUSL OK" on success and "CLOCKIDMUSL FAIL (detail)"
 * otherwise.
 */

#define _GNU_SOURCE
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <sys/times.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* The kernel's 32-bit timespec, which the raw calls take. */
struct ts32 {
    long sec;
    long nsec;
};

/* One tick, the granularity of every clock but real time, in nanoseconds. */
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

/** A 32-bit timespec as nanoseconds. */
static long long ns32(const struct ts32 *ts)
{
    return (long long)ts->sec * 1000000000LL + ts->nsec;
}

/** A time in nanoseconds as a 32-bit timespec. */
static void to32(long long ns, struct ts32 *ts)
{
    ts->sec = (long)(ns / 1000000000LL);
    ts->nsec = (long)(ns % 1000000000LL);
}

/** The raw clock_nanosleep: 0, or -1 with errno set. */
static long sleep_raw(long clk, long flags, const struct ts32 *req, struct ts32 *rem)
{
    return syscall(SYS_clock_nanosleep_time32, clk, flags, req, rem);
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

/** The eight clocks, their resolutions, and the CPU clocks by id. */
static int clocks(void)
{
    static const clockid_t boot_clocks[] = {CLOCK_MONOTONIC_RAW, CLOCK_MONOTONIC_COARSE,
                                            CLOCK_BOOTTIME};
    struct timespec res;
    struct tms tm;
    clockid_t cpu;
    long long before;
    long long after;
    long long x;
    unsigned int i;
    pid_t child;
    int status;

    for (i = 0; i < sizeof(boot_clocks) / sizeof(boot_clocks[0]); i++) {
        before = ns_of(CLOCK_MONOTONIC);
        x = ns_of(boot_clocks[i]);
        after = ns_of(CLOCK_MONOTONIC);
        if (x < before || x > after) {
            return fail("a clock of time since boot does not read CLOCK_MONOTONIC");
        }
    }
    before = ns_of(CLOCK_REALTIME);
    x = ns_of(CLOCK_REALTIME_COARSE);
    after = ns_of(CLOCK_REALTIME);
    if (x > after || x < before - 2 * TICK_NS) {
        return fail("coarse real time is not real time at the last tick");
    }
    if (clock_getres(CLOCK_REALTIME, &res) != 0 || res.tv_sec != 0 || res.tv_nsec != 1000) {
        return fail("real time does not resolve to the microsecond");
    }
    for (i = 1; i <= 7; i++) {
        if (clock_getres((clockid_t)i, &res) != 0 || res.tv_sec != 0 || res.tv_nsec != TICK_NS) {
            return fail("a clock other than real time does not resolve to the tick");
        }
    }
    if (syscall(SYS_clock_getres_time32, (long)CLOCK_MONOTONIC, NULL) != 0) {
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

    child = fork();
    if (child < 0) {
        return fail("fork failed");
    }
    if (child == 0) {
        spin_until(CLOCK_PROCESS_CPUTIME_ID, 3 * TICK_NS);
        pause();
        _exit(0);
    }
    if (clock_getcpuclockid(child, &cpu) != 0) {
        kill(child, SIGKILL);
        waitpid(child, &status, 0);
        return fail("clock_getcpuclockid gave no clock for a child");
    }
    before = ns_of(CLOCK_MONOTONIC);
    x = ns_of(cpu);
    while (x < 3 * TICK_NS && ns_of(CLOCK_MONOTONIC) - before < SPIN_BOUND_NS) {
        usleep(20000);
        x = ns_of(cpu);
    }
    kill(child, SIGKILL);
    waitpid(child, &status, 0);
    if (x < 3 * TICK_NS) {
        return fail("a child's CPU clock by id did not show the time it spun");
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
    struct ts32 req;
    struct ts32 rem;
    long long due;
    long long start;
    unsigned int i;

    for (i = 0; i < sizeof(absolute) / sizeof(absolute[0]); i++) {
        due = ns_of((clockid_t)absolute[i]) + 50000000LL;
        to32(due, &req);
        if (sleep_raw(absolute[i], TIMER_ABSTIME, &req, NULL) != 0) {
            return fail("an absolute sleep did not return 0");
        }
        if (ns_of((clockid_t)absolute[i]) < due) {
            return fail("an absolute sleep returned before its clock read the time asked");
        }
    }
    start = ns_of(CLOCK_MONOTONIC);
    to32(50000000LL, &req);
    if (sleep_raw(CLOCK_MONOTONIC, 0, &req, NULL) != 0 ||
        ns_of(CLOCK_MONOTONIC) - start < 50000000LL) {
        return fail("a relative sleep of 50 ms took less than 50 ms of time since boot");
    }
    start = ns_of(CLOCK_MONOTONIC);
    to32(start - 1000000000LL, &req);
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
    if (rem.sec < 0 || rem.nsec < 0 || rem.nsec >= 1000000000L || ns32(&rem) == 0 ||
        ns32(&rem) > ns32(&req)) {
        return fail("the remainder of a relative sleep is not within its interval");
    }
    to32(ns_of(CLOCK_MONOTONIC) + 5000000000LL, &req);
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

    to32(10000000LL, &req);
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

int main(void)
{
    if (clocks() != 0 || sleeps() != 0) {
        return 1;
    }
    printf("CLOCKIDMUSL OK\n");

    return 0;
}
