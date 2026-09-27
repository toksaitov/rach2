/**
 * eventfd and epoll.
 *
 * An eventfd's read takes the whole counter and leaves it empty; with
 * EFD_SEMAPHORE each read takes one, and a counter of 3 is empty after three. A
 * nonblocking read of zero fails with EAGAIN, and a read of zero bytes and a
 * write of the all-ones value with EINVAL. poll reports it readable once
 * written.
 *
 * An epoll set reports a pipe with data, level-triggered, at every wait while
 * the data is there; edge-triggered only once until more arrives; with
 * EPOLLONESHOT once until EPOLL_CTL_MOD. Its data word comes back as it was
 * given. A wait on nothing ready ends at its timeout with 0; a registration is
 * removed by EPOLL_CTL_DEL and dropped when its descriptor is closed; a regular
 * file is refused with EPERM and a second add with EEXIST. A child's write
 * wakes a parent the child has seen asleep in an epoll_wait with no timeout.
 * The child keeps running until the parent answers, so that the kernel does not
 * idle, which would scan the set again at every interrupt, and only the write
 * can end the wait; an alarm bounds it. A registration of no events is told of
 * a hangup. A pipe added to a set while a wait on it sleeps, then written,
 * wakes the wait. A set whose edge-triggered event was reported does not poll
 * readable, and a poll of a set is woken by a child's write to a pipe it
 * watches. A write to a pipe in a set inside a set inside a third wakes a wait
 * on the third, and a cycle of sets is ELOOP. Waits with room for one event
 * report two ready descriptors in turn. A one-shot event a wait cannot store,
 * EFAULT, is reported by the next wait. The registration of a closed eventfd
 * does not report the eventfd made next at its number, which can be added.
 * epoll_pwait2 takes a timeout of 2^31 seconds, and refuses negative seconds
 * with EINVAL.
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
#include <sys/mman.h>
#include <sys/eventfd.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
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

/**
 * Wait up to 3 s to see `parent` asleep, write a byte to `out`, and keep
 * running, reading the nonblocking `ack` in a loop, until it has a byte or
 * 15 s have passed. A process that runs keeps the kernel from idling, and an
 * idle kernel scans the parent's descriptors again at every interrupt, which
 * would end the wait without the write's wake. Exits 0 when all of that
 * happened, 2 when the parent was never seen asleep, and 3 when no byte came.
 */
static void writer(pid_t parent, int out, int ack)
{
    struct timespec start, now;
    char byte;
    int tries;

    for (tries = 0; tries < 300 && !sleeping(parent); tries++) {
        usleep(10000);
    }
    if (tries == 300) {
        _exit(2);
    }
    if (write(out, "w", 1) != 1 || clock_gettime(CLOCK_MONOTONIC, &start) != 0) {
        _exit(1);
    }
    do {
        if (read(ack, &byte, 1) == 1) {
            _exit(0);
        }
        clock_gettime(CLOCK_MONOTONIC, &now);
    } while (now.tv_sec - start.tv_sec < 15);
    _exit(3);
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
        if (pipe(ack) != 0 || fcntl(ack[0], F_SETFL, O_NONBLOCK) != 0 ||
            sigaction(SIGALRM, &sa, NULL) != 0) {
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

/* A wait on an empty set is woken by a pipe added to the set while it
 * waits and then written: a child shares the set, adds the pipe, lets the
 * wait look again, and writes. */
static int added_while_waiting(void)
{
    struct epoll_event out;
    int ep = epoll_create1(0);
    int p[2], status;
    pid_t pid;

    if (ep < 0 || pipe(p) != 0) {
        return fail("setup for an add during a wait");
    }
    pid = fork();
    if (pid == 0) {
        struct timespec pause = {0, 100000000};
        struct epoll_event ev;

        memset(&ev, 0, sizeof(ev));
        ev.events = EPOLLIN;
        ev.data.u32 = 3;
        nanosleep(&pause, NULL);
        if (epoll_ctl(ep, EPOLL_CTL_ADD, p[0], &ev) != 0) {
            _exit(1);
        }
        nanosleep(&pause, NULL);
        _exit(write(p[1], "a", 1) == 1 ? 0 : 1);
    }
    if (wait_one(ep, 5000, &out) != 1 || out.data.u32 != 3) {
        return fail("a pipe added during a wait did not wake it");
    }
    if (waitpid(pid, &status, 0) != pid || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        return fail("the child that added the pipe");
    }
    close(p[0]);
    close(p[1]);
    close(ep);
    return 0;
}

/* A set whose only event is edge-triggered and reported already does not
 * poll readable, as a wait on it would report nothing. */
static int polled_after_edge(void)
{
    struct epoll_event ev, out;
    struct pollfd pfd;
    int ep = epoll_create1(0);
    int p[2];

    memset(&ev, 0, sizeof(ev));
    ev.events = EPOLLIN | EPOLLET;
    if (ep < 0 || pipe(p) != 0 || epoll_ctl(ep, EPOLL_CTL_ADD, p[0], &ev) != 0 ||
        write(p[1], "e", 1) != 1 || wait_one(ep, 0, &out) != 1) {
        return fail("setup for a poll after an edge");
    }
    pfd.fd = ep;
    pfd.events = POLLIN;
    pfd.revents = 0;
    if (wait_one(ep, 0, &out) != 0 || poll(&pfd, 1, 0) != 0) {
        return fail("a set with its edge reported polled readable");
    }
    close(p[0]);
    close(p[1]);
    close(ep);
    return 0;
}

/* Sets inside sets: with a pipe in A, A in B and B in C, a child's write to
 * the pipe wakes a wait on C, which reports B while the data is there; and
 * adding C to A, a cycle, is ELOOP. */
static int nested(void)
{
    struct epoll_event ev, out;
    int a = epoll_create1(0);
    int b = epoll_create1(0);
    int c = epoll_create1(0);
    int p[2], status;
    pid_t pid;

    memset(&ev, 0, sizeof(ev));
    ev.events = EPOLLIN;
    if (a < 0 || b < 0 || c < 0 || pipe(p) != 0 || epoll_ctl(a, EPOLL_CTL_ADD, p[0], &ev) != 0 ||
        epoll_ctl(b, EPOLL_CTL_ADD, a, &ev) != 0) {
        return fail("setup for sets inside sets");
    }
    ev.data.u32 = 9;
    if (epoll_ctl(c, EPOLL_CTL_ADD, b, &ev) != 0 || wait_one(c, 0, &out) != 0) {
        return fail("a chain of sets over an empty pipe");
    }
    if (epoll_ctl(a, EPOLL_CTL_ADD, c, &ev) != -1 || errno != ELOOP) {
        return fail("a cycle of sets was not ELOOP");
    }
    pid = fork();
    if (pid == 0) {
        struct timespec pause = {0, 100000000};

        nanosleep(&pause, NULL);
        _exit(write(p[1], "n", 1) == 1 ? 0 : 1);
    }
    if (wait_one(c, 5000, &out) != 1 || out.data.u32 != 9 || wait_one(c, 0, &out) != 1) {
        return fail("a write to a pipe two sets deep did not wake the outer set");
    }
    if (waitpid(pid, &status, 0) != pid || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        return fail("the writing child");
    }
    close(p[0]);
    close(p[1]);
    close(a);
    close(b);
    close(c);
    return 0;
}

/* A poll of an epoll set is woken by a child's write to a pipe the set
 * watches. */
static int polled_set_woken(void)
{
    struct epoll_event ev;
    struct pollfd pfd;
    int ep = epoll_create1(0);
    int p[2], status;
    pid_t pid;

    memset(&ev, 0, sizeof(ev));
    ev.events = EPOLLIN;
    if (ep < 0 || pipe(p) != 0 || epoll_ctl(ep, EPOLL_CTL_ADD, p[0], &ev) != 0) {
        return fail("setup for a poll of a set");
    }
    pid = fork();
    if (pid == 0) {
        struct timespec pause = {0, 100000000};

        nanosleep(&pause, NULL);
        _exit(write(p[1], "q", 1) == 1 ? 0 : 1);
    }
    pfd.fd = ep;
    pfd.events = POLLIN;
    pfd.revents = 0;
    if (poll(&pfd, 1, 5000) != 1 || !(pfd.revents & POLLIN)) {
        return fail("a write to a pipe in a set did not wake a poll of the set");
    }
    if (waitpid(pid, &status, 0) != pid || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        return fail("the writing child");
    }
    close(p[0]);
    close(p[1]);
    close(ep);
    return 0;
}

/* Two descriptors ready, and waits with room for one event: the waits
 * report each in turn. */
static int in_turn(void)
{
    struct epoll_event ev, first, second;
    int ep = epoll_create1(0);
    int a = eventfd(0, 0);
    int b = eventfd(0, 0);

    memset(&ev, 0, sizeof(ev));
    ev.events = EPOLLOUT;
    ev.data.u32 = 1;
    if (ep < 0 || a < 0 || b < 0 || epoll_ctl(ep, EPOLL_CTL_ADD, a, &ev) != 0) {
        return fail("setup for waits in turn");
    }
    ev.data.u32 = 2;
    if (epoll_ctl(ep, EPOLL_CTL_ADD, b, &ev) != 0) {
        return fail("setup for waits in turn");
    }
    if (wait_one(ep, 0, &first) != 1 || wait_one(ep, 0, &second) != 1 ||
        first.data.u32 == second.data.u32) {
        return fail("two waits for one event reported the same descriptor");
    }
    close(a);
    close(b);
    close(ep);
    return 0;
}

/* A one-shot event a wait cannot store in a read-only buffer, EFAULT, is
 * reported by the next wait. */
static int failed_copy(void)
{
    struct epoll_event ev, out;
    void *ro = mmap(NULL, 4096, PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    uint64_t v = 1;
    int ep = epoll_create1(0);
    int e = eventfd(0, 0);

    memset(&ev, 0, sizeof(ev));
    ev.events = EPOLLIN | EPOLLONESHOT;
    if (ro == MAP_FAILED || ep < 0 || e < 0 || write(e, &v, sizeof(v)) != 8 ||
        epoll_ctl(ep, EPOLL_CTL_ADD, e, &ev) != 0) {
        return fail("setup for a failed copy");
    }
    if (epoll_wait(ep, (struct epoll_event *)ro, 1, 0) != -1 || errno != EFAULT) {
        return fail("a wait into a read-only buffer was not EFAULT");
    }
    if (wait_one(ep, 0, &out) != 1) {
        return fail("a one-shot event a failed copy took was not reported again");
    }
    munmap(ro, 4096);
    close(e);
    close(ep);
    return 0;
}

/* A registration of no events is still told of a hangup. */
static int hangup_no_events(void)
{
    struct epoll_event ev, out;
    int ep = epoll_create1(0);
    int p[2];

    memset(&ev, 0, sizeof(ev));
    if (ep < 0 || pipe(p) != 0 || epoll_ctl(ep, EPOLL_CTL_ADD, p[0], &ev) != 0) {
        return fail("setup for a registration of no events");
    }
    if (wait_one(ep, 0, &out) != 0) {
        return fail("a registration of no events was reported");
    }
    close(p[1]);
    if (wait_one(ep, 0, &out) != 1 || !(out.events & EPOLLHUP)) {
        return fail("a registration of no events was not told of the hangup");
    }
    close(p[0]);
    close(ep);
    return 0;
}

/* A registration of a closed eventfd is not the eventfd made next at its
 * number, whose description the kernel may put at the same address: a wait
 * does not report the new one for it, and the new one can be added. */
static int number_reused(void)
{
    struct epoll_event ev, out;
    int ep = epoll_create1(0);
    int e = eventfd(0, 0);
    int again;

    memset(&ev, 0, sizeof(ev));
    ev.events = EPOLLIN;
    ev.data.u32 = 1;
    if (ep < 0 || e < 0 || epoll_ctl(ep, EPOLL_CTL_ADD, e, &ev) != 0) {
        return fail("setup for a reused number");
    }
    close(e);
    again = eventfd(1, 0);
    if (again < 0 || wait_one(ep, 0, &out) != 0) {
        return fail("a closed eventfd's registration reported the one after it");
    }
    ev.data.u32 = 2;
    if (epoll_ctl(ep, EPOLL_CTL_ADD, again, &ev) != 0 || wait_one(ep, 0, &out) != 1 ||
        out.data.u32 != 2) {
        return fail("the eventfd at a closed one's number could not be added");
    }
    close(again);
    close(ep);
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
    if (counters() || sets() || hangup_no_events() || number_reused() || added_while_waiting() ||
        polled_after_edge() || polled_set_woken() || nested() || in_turn() || failed_copy() ||
        long_timeout()) {
        return 1;
    }
    printf("EPOLLMUSL OK\n");
    return 0;
}
