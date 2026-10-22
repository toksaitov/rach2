/**
 * timerfdmusl: check timerfd's timers, reads and wakes.
 *
 * A timer armed for 50 ms with a 20 ms interval blocks a read until it
 * expires, then reads a count of at least one, and timerfd_gettime reports
 * the interval it was given. A nonblocking read of a timer not yet expired
 * fails with EAGAIN, and a read of zero bytes with EINVAL, as a read of fewer
 * than eight does; a timer armed with a zero value is disarmed and reports
 * zeros. An absolute time already past on the monotonic clock, 1 ns, which the
 * first timer's 50 ms have put behind it, expires once, as poll and a read
 * see. A timer in an epoll set wakes a wait with no timeout: a child arms the
 * timer only once it has seen the parent asleep in epoll_wait, and keeps
 * running until the parent answers, so that the kernel does not idle, which
 * would scan the set again at every interrupt. A timer of 200 days reports 200
 * days, and its interval of 200 days.
 *
 * Prints `TIMERFDMUSL OK` and exits 0 on success, and prints
 * `TIMERFDMUSL FAIL (detail)` and exits 1 otherwise. Writes etude's report
 * records (probe_report.h) for the case ETUDE_CASE names, an assertion for
 * each check, as `assertion_id` lists them; a failed check fails its
 * assertion and leaves those after it not run.
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/timerfd.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "../probe_report.h"

extern char **environ;

/* The assertions, a check each, in the order the program makes them. */
static const char *const assertion_id[] = {
    "create",       "settime",         "gettime-interval", "blocking-read",
    "disarm",       "nonblock-setup",  "read-eagain",      "read-einval",
    "abstime-set",  "abstime-expired", "epoll-setup",      "epoll-child",
    "epoll-asleep", "epoll-wake",      "long-timer-set",   "long-timer-reports"};

/** Report one failed check with errno, end the report at the assertion that
 * `what` fails, and return 1, the failure status. */
static int fail(const char *what)
{
    printf("TIMERFDMUSL FAIL (%s: errno %d)\n", what, errno);
    report_stop(what);
    return 1;
}

/** Begin the report and plan its assertions. */
static void report_start(void)
{
    unsigned int i;

    report_begin(report_case(environ, "guest.default.timerfdmusl"));
    for (i = 0; i < sizeof(assertion_id) / sizeof(assertion_id[0]); i++) {
        report_plan(assertion_id[i]);
    }
}

/** Fill `it` with a value of `value_ms` and an interval of `interval_ms`. */
static void set(struct itimerspec *it, long value_ms, long interval_ms)
{
    memset(it, 0, sizeof(*it));
    it->it_value.tv_sec = value_ms / 1000;
    it->it_value.tv_nsec = (value_ms % 1000) * 1000000L;
    it->it_interval.tv_sec = interval_ms / 1000;
    it->it_interval.tv_nsec = (interval_ms % 1000) * 1000000L;
}

/** The state letter /proc/<pid>/stat gives process `pid`, or 0 when it has
 * no stat file. */
static int proc_state(pid_t pid)
{
    char path[64], line[256], *state;
    int fd, n;

    snprintf(path, sizeof(path), "/proc/%d/stat", (int)pid);
    fd = open(path, O_RDONLY);
    if (fd < 0) {
        return 0;
    }
    n = (int)read(fd, line, sizeof(line) - 1);
    close(fd);
    if (n <= 0) {
        return 0;
    }
    line[n] = '\0';
    state = strrchr(line, ')');
    return state && state[1] == ' ' ? state[2] : 0;
}

/**
 * Wait for process `pid` to be asleep, state S in /proc/<pid>/stat, looking
 * every 10 ms. Returns 1 once it is, and 0 when it ends first, as a zombie or
 * gone from /proc.
 */
static int seen_asleep(pid_t pid)
{
    int state;

    for (;;) {
        state = proc_state(pid);
        if (state == 'S') {
            return 1;
        }
        if (state == 0 || state == 'Z') {
            return 0;
        }
        usleep(10000);
    }
}

/**
 * Wait to see `parent` asleep (`seen_asleep`), arm the timer `t` for 30 ms,
 * and keep running, reading the nonblocking `ack` in a loop, until it has a
 * byte. A process that runs keeps the kernel from idling, and an idle kernel
 * scans the parent's descriptors again at every interrupt, which would end
 * the wait without the timer's wake. Exits 0 when all of that happened, 2
 * when the parent was never seen asleep, and 1 when the timer could not be
 * armed.
 */
static void arm_and_run(pid_t parent, int t, int ack)
{
    struct itimerspec it;
    char byte;

    if (!seen_asleep(parent)) {
        _exit(2);
    }
    set(&it, 30, 0);
    if (timerfd_settime(t, 0, &it, NULL) != 0) {
        _exit(1);
    }
    while (read(ack, &byte, 1) != 1) {
    }
    _exit(0);
}

/**
 * The timer `t`, disarmed, in an epoll set: a child arms it once it has seen
 * the parent asleep in an epoll_wait with no timeout, and the timer's wake
 * must end the wait; a wake the kernel misses leaves the wait asleep, and the
 * case's watchdog ends it. Returns 0, or 1 once the failure is reported.
 */
static int epoll_row(int t)
{
    struct epoll_event ev, out;
    int ack[2];
    int ep, n, status;
    pid_t parent = getpid();
    pid_t pid;

    ep = epoll_create1(0);
    memset(&ev, 0, sizeof(ev));
    ev.events = EPOLLIN;
    ev.data.fd = t;
    if (ep < 0 || epoll_ctl(ep, EPOLL_CTL_ADD, t, &ev) != 0 || pipe(ack) != 0 ||
        fcntl(ack[0], F_SETFL, O_NONBLOCK) != 0) {
        return fail("setup for epoll");
    }
    report_held();
    pid = fork();
    if (pid == 0) {
        arm_and_run(parent, t, ack[0]);
    }
    memset(&out, 0, sizeof(out));
    n = epoll_wait(ep, &out, 1, -1);
    if (pid < 0 || write(ack[1], "a", 1) != 1 || waitpid(pid, &status, 0) != pid) {
        return fail("the child that arms the timer");
    }
    report_held();
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        return fail(WIFEXITED(status) && WEXITSTATUS(status) == 2
                        ? "the parent was never seen asleep in epoll_wait"
                        : "the child that arms the timer");
    }
    report_held();
    if (n != 1 || out.data.fd != t) {
        return fail("the timer did not wake the epoll wait");
    }
    report_held();
    close(ack[0]);
    close(ack[1]);
    close(ep);
    return 0;
}

/**
 * Check that a timer of 200 days, longer than one wait of ticks spans,
 * reports the whole time, and so does its interval. Returns 0, or 1 once the
 * failure is reported.
 */
static int long_timer(void)
{
    struct itimerspec it, got;
    int t = timerfd_create(CLOCK_MONOTONIC, 0);

    memset(&it, 0, sizeof(it));
    it.it_value.tv_sec = 200L * 86400L;
    it.it_interval.tv_sec = 200L * 86400L;
    if (t < 0 || timerfd_settime(t, 0, &it, NULL) != 0 || timerfd_gettime(t, &got) != 0) {
        return fail("a timer of 200 days");
    }
    report_held();
    if (got.it_value.tv_sec < 199L * 86400L || got.it_value.tv_sec > 200L * 86400L ||
        got.it_interval.tv_sec != 200L * 86400L) {
        return fail("a timer of 200 days did not report 200 days");
    }
    report_held();
    close(t);
    return 0;
}

int main(void)
{
    struct itimerspec it, got;
    struct pollfd pfd;
    uint64_t n;
    int t;

    report_start();
    t = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC);
    if (t < 0) {
        return fail("timerfd_create");
    }
    report_held();
    set(&it, 50, 20);
    if (timerfd_settime(t, 0, &it, NULL) != 0) {
        return fail("timerfd_settime");
    }
    report_held();
    if (timerfd_gettime(t, &got) != 0 || got.it_interval.tv_sec != 0 ||
        got.it_interval.tv_nsec != 20000000L) {
        return fail("timerfd_gettime did not report the interval");
    }
    report_held();
    if (read(t, &n, sizeof(n)) != 8 || n < 1) {
        return fail("a blocking read did not return an expiration");
    }
    report_held();

    set(&it, 0, 0);
    if (timerfd_settime(t, 0, &it, NULL) != 0 || timerfd_gettime(t, &got) != 0 ||
        got.it_value.tv_sec != 0 || got.it_value.tv_nsec != 0) {
        return fail("a zero value did not disarm the timer");
    }
    report_held();
    close(t);

    t = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK);
    set(&it, 5000, 0);
    if (t < 0 || timerfd_settime(t, 0, &it, NULL) != 0) {
        return fail("setup for a nonblocking read");
    }
    report_held();
    if (read(t, &n, sizeof(n)) != -1 || errno != EAGAIN) {
        return fail("a nonblocking read of an unexpired timer was not EAGAIN");
    }
    report_held();
    if (read(t, &n, 0) != -1 || errno != EINVAL) {
        return fail("a read of zero bytes was not EINVAL");
    }
    report_held();

    memset(&it, 0, sizeof(it));
    it.it_value.tv_nsec = 1;
    if (timerfd_settime(t, TFD_TIMER_ABSTIME, &it, NULL) != 0) {
        return fail("an absolute time");
    }
    report_held();
    pfd.fd = t;
    pfd.events = POLLIN;
    if (poll(&pfd, 1, -1) != 1 || read(t, &n, sizeof(n)) != 8 || n != 1) {
        return fail("an absolute time already past did not expire");
    }
    report_held();

    if (epoll_row(t) != 0 || long_timer() != 0) {
        return 1;
    }
    report_end();
    close(t);
    printf("TIMERFDMUSL OK\n");
    return 0;
}
