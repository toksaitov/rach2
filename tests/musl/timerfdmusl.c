/**
 * timerfd.
 *
 * A timer armed for 50 ms with a 20 ms interval blocks a read until it
 * expires, then reads a count of at least one, and timerfd_gettime reports
 * the interval it was given. A nonblocking read of a timer not yet expired
 * fails with EAGAIN; a timer armed with a zero value is disarmed and reports
 * zeros. An absolute time already past on the monotonic clock, 1 ns, which
 * the first timer's 50 ms have put behind it, expires at once, as poll sees.
 * A timer in an epoll set wakes the wait.
 *
 * Prints "TIMERFDMUSL OK" on success and "TIMERFDMUSL FAIL (detail)"
 * otherwise.
 */

#define _GNU_SOURCE
#include <errno.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/timerfd.h>
#include <time.h>
#include <unistd.h>

static int fail(const char *what)
{
    printf("TIMERFDMUSL FAIL (%s: errno %d)\n", what, errno);
    return 1;
}

static void set(struct itimerspec *it, long value_ms, long interval_ms)
{
    memset(it, 0, sizeof(*it));
    it->it_value.tv_sec = value_ms / 1000;
    it->it_value.tv_nsec = (value_ms % 1000) * 1000000L;
    it->it_interval.tv_sec = interval_ms / 1000;
    it->it_interval.tv_nsec = (interval_ms % 1000) * 1000000L;
}

int main(void)
{
    struct itimerspec it, got;
    struct pollfd pfd;
    struct epoll_event ev;
    uint64_t n;
    int t = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC);
    int ep;

    if (t < 0) {
        return fail("timerfd_create");
    }
    set(&it, 50, 20);
    if (timerfd_settime(t, 0, &it, NULL) != 0) {
        return fail("timerfd_settime");
    }
    if (timerfd_gettime(t, &got) != 0 || got.it_interval.tv_sec != 0 ||
        got.it_interval.tv_nsec != 20000000L) {
        return fail("timerfd_gettime did not report the interval");
    }
    if (read(t, &n, sizeof(n)) != 8 || n < 1) {
        return fail("a blocking read did not return an expiration");
    }

    set(&it, 0, 0);
    if (timerfd_settime(t, 0, &it, NULL) != 0 || timerfd_gettime(t, &got) != 0 ||
        got.it_value.tv_sec != 0 || got.it_value.tv_nsec != 0) {
        return fail("a zero value did not disarm the timer");
    }
    close(t);

    t = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK);
    set(&it, 5000, 0);
    if (t < 0 || timerfd_settime(t, 0, &it, NULL) != 0) {
        return fail("setup for a nonblocking read");
    }
    if (read(t, &n, sizeof(n)) != -1 || errno != EAGAIN) {
        return fail("a nonblocking read of an unexpired timer was not EAGAIN");
    }

    memset(&it, 0, sizeof(it));
    it.it_value.tv_nsec = 1;
    if (timerfd_settime(t, TFD_TIMER_ABSTIME, &it, NULL) != 0) {
        return fail("an absolute time");
    }
    pfd.fd = t;
    pfd.events = POLLIN;
    if (poll(&pfd, 1, 5000) != 1 || read(t, &n, sizeof(n)) != 8 || n != 1) {
        return fail("an absolute time already past did not expire");
    }

    ep = epoll_create1(0);
    memset(&ev, 0, sizeof(ev));
    ev.events = EPOLLIN;
    ev.data.fd = t;
    set(&it, 30, 0);
    if (ep < 0 || epoll_ctl(ep, EPOLL_CTL_ADD, t, &ev) != 0 ||
        timerfd_settime(t, 0, &it, NULL) != 0) {
        return fail("setup for epoll");
    }
    if (epoll_wait(ep, &ev, 1, 5000) != 1 || ev.data.fd != t) {
        return fail("the timer did not wake the epoll wait");
    }
    close(ep);
    close(t);
    printf("TIMERFDMUSL OK\n");
    return 0;
}
