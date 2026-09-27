/**
 * eventfd and epoll.
 *
 * An eventfd's read takes the whole counter and leaves it empty; with
 * EFD_SEMAPHORE each read takes one, and a counter of 3 is empty after three.
 * A nonblocking read of zero fails with EAGAIN, and a read of zero bytes and a
 * write of the all-ones value with EINVAL. poll reports it readable once
 * written.
 * An epoll set reports a pipe with data, level-triggered, at every wait
 * while the data is there; edge-triggered only once until more arrives;
 * with EPOLLONESHOT once until EPOLL_CTL_MOD. Its data word comes back as it
 * was given. A wait on nothing ready ends at its timeout with 0; a
 * registration is removed by EPOLL_CTL_DEL and dropped when its descriptor
 * is closed; a regular file is refused with EPERM and a second add with
 * EEXIST. A child's write wakes a parent the child has seen asleep in an
 * epoll_wait with no timeout. The child stays alive until the parent
 * answers, so that only the write can end the wait, and an alarm bounds
 * it. epoll_pwait2 takes a timeout of 2^31 seconds, and refuses negative
 * seconds with EINVAL.
 *
 * Prints "EPOLLMUSL OK" on success and "EPOLLMUSL FAIL (detail)" otherwise.
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
#include <sys/eventfd.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

static volatile sig_atomic_t alarms;

static int fail(const char *what)
{
    printf("EPOLLMUSL FAIL (%s: errno %d)\n", what, errno);
    return 1;
}

/** Count a SIGALRM. */
static void on_alarm(int sig)
{
    (void)sig;
    alarms++;
}

/** Whether process `pid` sleeps, by its state in /proc/<pid>/stat. */
static int sleeping(pid_t pid)
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
    return state && state[1] == ' ' && state[2] == 'S';
}

/** Wait up to 3 s to see `parent` asleep, write a byte to `out`, and exit
 * only once `ack` has one: 0 when all of that happened, 2 when the parent
 * was never seen asleep. */
static void writer(pid_t parent, int out, int ack)
{
    char byte;
    int tries;

    for (tries = 0; tries < 300 && !sleeping(parent); tries++) {
        usleep(10000);
    }
    if (tries == 300) {
        _exit(2);
    }
    _exit(write(out, "w", 1) == 1 && read(ack, &byte, 1) == 1 ? 0 : 1);
}

static int counters(void)
{
    struct pollfd pfd;
    uint64_t v;
    int e = eventfd(0, EFD_NONBLOCK);
    int s = eventfd(3, EFD_SEMAPHORE | EFD_NONBLOCK);
    int i;

    if (e < 0 || s < 0) {
        return fail("eventfd");
    }
    if (read(e, &v, sizeof(v)) != -1 || errno != EAGAIN) {
        return fail("a nonblocking read of zero was not EAGAIN");
    }
    if (read(e, &v, 0) != -1 || errno != EINVAL) {
        return fail("a read of zero bytes was not EINVAL");
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
    if (read(e, &v, sizeof(v)) != -1 || errno != EAGAIN) {
        return fail("a read did not leave the counter empty");
    }
    v = 0xFFFFFFFFFFFFFFFFull;
    if (write(e, &v, sizeof(v)) != -1 || errno != EINVAL) {
        return fail("a write of all ones was not EINVAL");
    }
    for (i = 0; i < 3; i++) {
        if (read(s, &v, sizeof(v)) != 8 || v != 1) {
            return fail("EFD_SEMAPHORE did not take one at a time");
        }
    }
    if (read(s, &v, sizeof(v)) != -1 || errno != EAGAIN) {
        return fail("three reads with EFD_SEMAPHORE did not empty a counter of 3");
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
        struct sigaction sa;
        int ack[2];
        pid_t pid;
        int status;
        int n;

        memset(&sa, 0, sizeof(sa));
        sa.sa_handler = on_alarm;
        if (pipe(ack) != 0 || sigaction(SIGALRM, &sa, NULL) != 0) {
            return fail("setup for the child's wake");
        }
        pid = fork();
        if (pid == 0) {
            writer(getppid(), p[1], ack[0]);
        }
        alarm(10);
        n = wait_one(ep, -1, &out);
        alarm(0);
        if (pid < 0 || write(ack[1], "a", 1) != 1 || waitpid(pid, &status, 0) != pid) {
            return fail("the child that writes");
        }
        close(ack[0]);
        close(ack[1]);
        if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
            return fail(WIFEXITED(status) && WEXITSTATUS(status) == 2
                            ? "the parent was never seen asleep in epoll_wait"
                            : "the child's write");
        }
        if (n != 1 || out.data.u32 != 7) {
            return fail(alarms ? "the child's write did not wake the wait"
                               : "the wait the child's write ended");
        }
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

/* epoll_pwait2 takes 64-bit seconds: a timeout of 2^31 seconds is valid,
 * and with a descriptor ready the wait returns at once. */
static int long_timeout(void)
{
    struct {
        long long sec;
        long long nsec;
    } ts = {2147483648LL, 0};
    struct epoll_event ev, out;
    uint64_t v = 1;
    int ep = epoll_create1(0);
    int e = eventfd(0, 0);

    memset(&ev, 0, sizeof(ev));
    ev.events = EPOLLIN;
    if (ep < 0 || e < 0 || write(e, &v, sizeof(v)) != 8 ||
        epoll_ctl(ep, EPOLL_CTL_ADD, e, &ev) != 0) {
        return fail("setup for epoll_pwait2");
    }
    if (syscall(441, ep, &out, 1, &ts, NULL, 8) != 1) {
        return fail("epoll_pwait2 of 2^31 seconds with a descriptor ready");
    }
    ts.sec = -1;
    if (syscall(441, ep, &out, 1, &ts, NULL, 8) != -1 || errno != EINVAL) {
        return fail("epoll_pwait2 of negative seconds was not EINVAL");
    }
    close(e);
    close(ep);
    return 0;
}

int main(void)
{
    if (counters() || sets() || long_timeout()) {
        return 1;
    }
    printf("EPOLLMUSL OK\n");
    return 0;
}
