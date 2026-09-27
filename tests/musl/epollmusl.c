/**
 * eventfd and epoll.
 *
 * An eventfd's reads take the counter, or one at a time with
 * EFD_SEMAPHORE; a nonblocking read of zero fails with EAGAIN, and a write
 * of the all-ones value with EINVAL. poll reports it readable once written.
 * An epoll set reports a pipe with data, level-triggered, at every wait
 * while the data is there; edge-triggered only once until more arrives;
 * with EPOLLONESHOT once until EPOLL_CTL_MOD. Its data word comes back as it
 * was given. A wait on nothing ready ends at its timeout with 0; a
 * registration is removed by EPOLL_CTL_DEL and dropped when its descriptor
 * is closed; a regular file is refused with EPERM and a second add with
 * EEXIST. A child's write wakes a parent blocked in epoll_wait.
 *
 * Prints "EPOLLMUSL OK" on success and "EPOLLMUSL FAIL (detail)" otherwise.
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static int fail(const char *what)
{
    printf("EPOLLMUSL FAIL (%s: errno %d)\n", what, errno);
    return 1;
}

static int counters(void)
{
    struct pollfd pfd;
    uint64_t v;
    int e = eventfd(0, EFD_NONBLOCK);
    int s = eventfd(3, EFD_SEMAPHORE);

    if (e < 0 || s < 0) {
        return fail("eventfd");
    }
    if (read(e, &v, sizeof(v)) != -1 || errno != EAGAIN) {
        return fail("a nonblocking read of zero was not EAGAIN");
    }
    pfd.fd = e;
    pfd.events = POLLIN;
    if (poll(&pfd, 1, 0) != 0) {
        return fail("a zero counter polled readable");
    }
    v = 2;
    if (write(e, &v, sizeof(v)) != 8 || write(e, &v, sizeof(v)) != 8) {
        return fail("eventfd write");
    }
    if (poll(&pfd, 1, 0) != 1 || read(e, &v, sizeof(v)) != 8 || v != 4) {
        return fail("the counter did not come back whole");
    }
    v = 0xFFFFFFFFFFFFFFFFull;
    if (write(e, &v, sizeof(v)) != -1 || errno != EINVAL) {
        return fail("a write of all ones was not EINVAL");
    }
    if (read(s, &v, sizeof(v)) != 8 || v != 1 || read(s, &v, sizeof(v)) != 8 || v != 1) {
        return fail("EFD_SEMAPHORE did not take one at a time");
    }
    close(e);
    close(s);
    return 0;
}

static int wait_one(int ep, int timeout, struct epoll_event *out)
{
    return epoll_wait(ep, out, 1, timeout);
}

static int sets(void)
{
    struct epoll_event ev, out;
    int p[2], q[2];
    int ep = epoll_create1(EPOLL_CLOEXEC);
    int file = open("/etc/passwd", O_RDONLY);
    char buf[4];

    if (ep < 0 || pipe(p) != 0 || pipe(q) != 0) {
        return fail("setup");
    }
    memset(&ev, 0, sizeof(ev));
    ev.events = EPOLLIN;
    ev.data.u64 = 0x1122334455667788ull;
    if (epoll_ctl(ep, EPOLL_CTL_ADD, p[0], &ev) != 0) {
        return fail("EPOLL_CTL_ADD");
    }
    if (epoll_ctl(ep, EPOLL_CTL_ADD, p[0], &ev) != -1 || errno != EEXIST) {
        return fail("a second add was not EEXIST");
    }
    if (file >= 0 && (epoll_ctl(ep, EPOLL_CTL_ADD, file, &ev) != -1 || errno != EPERM)) {
        return fail("a regular file was not EPERM");
    }
    if (wait_one(ep, 50, &out) != 0) {
        return fail("an empty pipe was reported");
    }
    if (write(p[1], "ab", 2) != 2 || wait_one(ep, 1000, &out) != 1 ||
        out.data.u64 != 0x1122334455667788ull || !(out.events & EPOLLIN)) {
        return fail("level-triggered data, or the data word");
    }
    if (wait_one(ep, 0, &out) != 1) {
        return fail("level-triggered data was not reported again");
    }

    ev.events = EPOLLIN | EPOLLET;
    if (epoll_ctl(ep, EPOLL_CTL_MOD, p[0], &ev) != 0 || wait_one(ep, 0, &out) != 1 ||
        wait_one(ep, 0, &out) != 0) {
        return fail("edge-triggered data was reported twice");
    }
    if (read(p[0], buf, 2) != 2 || write(p[1], "c", 1) != 1 || wait_one(ep, 0, &out) != 1) {
        return fail("an edge after the data went and came back");
    }
    (void)read(p[0], buf, 1);

    ev.events = EPOLLIN | EPOLLONESHOT;
    if (epoll_ctl(ep, EPOLL_CTL_MOD, p[0], &ev) != 0 || write(p[1], "d", 1) != 1 ||
        wait_one(ep, 0, &out) != 1 || wait_one(ep, 0, &out) != 0) {
        return fail("EPOLLONESHOT reported twice");
    }
    if (epoll_ctl(ep, EPOLL_CTL_MOD, p[0], &ev) != 0 || wait_one(ep, 0, &out) != 1) {
        return fail("EPOLL_CTL_MOD did not rearm EPOLLONESHOT");
    }
    if (epoll_ctl(ep, EPOLL_CTL_DEL, p[0], NULL) != 0 || wait_one(ep, 0, &out) != 0) {
        return fail("EPOLL_CTL_DEL");
    }
    (void)read(p[0], buf, 1);

    ev.events = EPOLLIN;
    if (epoll_ctl(ep, EPOLL_CTL_ADD, q[0], &ev) != 0 || write(q[1], "e", 1) != 1) {
        return fail("setup for a closed descriptor");
    }
    close(q[0]);
    if (wait_one(ep, 0, &out) != 0) {
        return fail("a closed descriptor was reported");
    }

    ev.events = EPOLLIN;
    ev.data.u32 = 7;
    if (epoll_ctl(ep, EPOLL_CTL_ADD, p[0], &ev) != 0) {
        return fail("setup for the child's wake");
    }
    {
        pid_t pid = fork();
        int status;

        if (pid == 0) {
            struct timespec pause = {0, 100000000};

            nanosleep(&pause, NULL);
            _exit(write(p[1], "w", 1) == 1 ? 0 : 1);
        }
        if (wait_one(ep, 5000, &out) != 1 || out.data.u32 != 7) {
            return fail("the child's write did not wake the wait");
        }
        waitpid(pid, &status, 0);
    }
    close(p[0]);
    close(p[1]);
    close(q[1]);
    close(ep);
    if (file >= 0) {
        close(file);
    }
    return 0;
}

int main(void)
{
    if (counters() || sets()) {
        return 1;
    }
    printf("EPOLLMUSL OK\n");
    return 0;
}
