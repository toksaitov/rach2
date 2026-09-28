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
 * on the third, and a cycle of sets, of three or of two, is ELOOP. Five sets
 * may nest, each added to the one before, and a write to a pipe in the fifth
 * reaches a wait on the first; a sixth set is ELOOP whether the chain is built
 * from the outermost set in or from the innermost out, and so is a set that
 * another holds by two paths when the longer would make a chain of six. Waits
 * with room for one event report two ready descriptors in turn. A one-shot
 * event a wait cannot store, EFAULT, is reported by the next wait. The
 * registration of a closed eventfd does not report the eventfd made next at its
 * number, which can be added. epoll_pwait2 takes a timeout of 2^31 seconds, and
 * refuses negative seconds with EINVAL.
 *
 * An edge-triggered registration is told only of the kinds of change its
 * events watch, as in Linux: an AF_UNIX stream end watched for output is not
 * reported again when a byte comes to it, nor when a read makes room in its
 * own full queue, while one watched for input is reported for each byte, and
 * a shutdown of the end is an edge of every kind; a listener is not reported
 * again when an accept leaves a connection queued; a datagram, or a TCP
 * segment over the loopback, that comes to a socket watched for output is no
 * edge; and each write to a pipe is an edge of its read end watched for
 * input, the pipe empty before it or not.
 *
 * Prints "EPOLLMUSL OK" on success and "EPOLLMUSL FAIL (detail)" otherwise.
 */

#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/mman.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* The port of the TCP connection over the loopback, which no other probe
 * listens on. */
#define EDGE_PORT 7470

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

/* A child a row starts to write once it sees the parent asleep, and the
 * pipe the parent answers it on. */
struct woken {
    pid_t pid;
    int ack[2];
};

/** Wait up to 3 s to see `parent` asleep, then add `in` to the set `ep`
 * for input, with the descriptor as its data: exits 1 when the add fails
 * and 2 when the parent was never seen asleep. */
static void add_when_asleep(pid_t parent, int ep, int in)
{
    struct epoll_event ev;
    int tries;

    for (tries = 0; tries < 300 && !sleeping(parent); tries++) {
        usleep(10000);
    }
    if (tries == 300) {
        _exit(2);
    }
    memset(&ev, 0, sizeof(ev));
    ev.events = EPOLLIN;
    ev.data.u32 = (uint32_t)in;
    if (epoll_ctl(ep, EPOLL_CTL_ADD, in, &ev) != 0) {
        _exit(1);
    }
}

/**
 * Start a child that runs `writer` on `out`, and arm a 10 s alarm for the
 * wait its write must end, which the caller begins next with no time limit
 * of its own. A wait the alarm ends finds the event there as it wakes, and
 * returns it, so the caller fails its row when `alarms` counts one: only
 * the write's wake may end the wait. With `ep` not -1, the child first adds
 * `in` to the set `ep` once it sees this process asleep. 0, or -1.
 */
static int wake_start(struct woken *w, int out, int ep, int in)
{
    struct sigaction sa;

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_alarm;
    if (pipe(w->ack) != 0 || fcntl(w->ack[0], F_SETFL, O_NONBLOCK) != 0 ||
        sigaction(SIGALRM, &sa, NULL) != 0) {
        return -1;
    }
    alarms = 0;
    w->pid = fork();
    if (w->pid == 0) {
        if (ep != -1) {
            add_when_asleep(getppid(), ep, in);
        }
        writer(getppid(), out, w->ack[0]);
    }
    if (w->pid < 0) {
        return -1;
    }
    alarm(10);
    return 0;
}

/** End the row `wake_start` began: disarm the alarm, answer the child and
 * reap it. 0 when the child did its part, 2 when it never saw this process
 * asleep, 1 when it failed otherwise, and -1 when it could not be answered
 * or reaped. */
static int wake_end(struct woken *w)
{
    int status;
    int reaped;

    alarm(0);
    reaped = write(w->ack[1], "a", 1) == 1 && waitpid(w->pid, &status, 0) == w->pid;
    close(w->ack[0]);
    close(w->ack[1]);
    if (!reaped) {
        return -1;
    }
    if (!WIFEXITED(status)) {
        return 1;
    }
    return WEXITSTATUS(status) == 0 ? 0 : WEXITSTATUS(status) == 2 ? 2 : 1;
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
    struct woken w;
    int p[2], q[2];
    int n, rc;
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
    if (wake_start(&w, p[1], -1, -1) != 0) {
        return fail("setup for the child's wake");
    }
    n = wait_one(ep, -1, &out);
    rc = wake_end(&w);
    if (rc != 0) {
        return fail(rc < 0    ? "the child that writes"
                    : rc == 2 ? "the parent was never seen asleep in epoll_wait"
                              : "the child's write");
    }
    if (n != 1 || out.data.u32 != 7 || alarms) {
        return fail(alarms ? "the child's write did not wake the wait"
                           : "the wait the child's write ended");
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
 * waits and then written: a child shares the set, adds the pipe once it sees
 * the wait asleep, lets the wait look again, and writes once it sees it
 * asleep again. */
static int added_while_waiting(void)
{
    struct epoll_event out;
    struct woken w;
    int ep = epoll_create1(0);
    int p[2], n, rc;

    if (ep < 0 || pipe(p) != 0 || wake_start(&w, p[1], ep, p[0]) != 0) {
        return fail("setup for an add during a wait");
    }
    n = wait_one(ep, -1, &out);
    rc = wake_end(&w);
    if (rc != 0) {
        return fail(rc == 2 ? "the parent was never seen asleep in epoll_wait"
                            : "the child that added the pipe");
    }
    if (n != 1 || out.data.u32 != (uint32_t)p[0] || alarms) {
        return fail("a pipe added during a wait did not wake it");
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
 * adding C to A, or B to A, a cycle, is ELOOP. */
static int nested(void)
{
    struct epoll_event ev, out;
    struct woken w;
    int a = epoll_create1(0);
    int b = epoll_create1(0);
    int c = epoll_create1(0);
    int p[2], n, rc;

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
    if (epoll_ctl(a, EPOLL_CTL_ADD, c, &ev) != -1 || errno != ELOOP ||
        epoll_ctl(a, EPOLL_CTL_ADD, b, &ev) != -1 || errno != ELOOP) {
        return fail("a cycle of sets was not ELOOP");
    }
    if (wake_start(&w, p[1], -1, -1) != 0) {
        return fail("setup for sets inside sets");
    }
    n = wait_one(c, -1, &out);
    rc = wake_end(&w);
    if (rc != 0) {
        return fail(rc == 2 ? "the parent was never seen asleep in epoll_wait"
                            : "the writing child");
    }
    if (n != 1 || out.data.u32 != 9 || alarms || wait_one(c, 0, &out) != 1) {
        return fail("a write to a pipe two sets deep did not wake the outer set");
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
    struct woken w;
    int ep = epoll_create1(0);
    int p[2], n, rc;

    memset(&ev, 0, sizeof(ev));
    ev.events = EPOLLIN;
    if (ep < 0 || pipe(p) != 0 || epoll_ctl(ep, EPOLL_CTL_ADD, p[0], &ev) != 0 ||
        wake_start(&w, p[1], -1, -1) != 0) {
        return fail("setup for a poll of a set");
    }
    pfd.fd = ep;
    pfd.events = POLLIN;
    pfd.revents = 0;
    n = poll(&pfd, 1, -1);
    rc = wake_end(&w);
    if (rc != 0) {
        return fail(rc == 2 ? "the parent was never seen asleep in poll" : "the writing child");
    }
    if (n != 1 || !(pfd.revents & POLLIN) || alarms) {
        return fail("a write to a pipe in a set did not wake a poll of the set");
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
    long long ts[2] = {2147483648LL, 0}; /* the kernel's 64-bit timespec */
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
    if (syscall(441, ep, &out, 1, ts, NULL, 8) != 1) {
        return fail("epoll_pwait2 of 2^31 seconds with a descriptor ready");
    }
    ts[0] = -1;
    if (syscall(441, ep, &out, 1, ts, NULL, 8) != -1 || errno != EINVAL) {
        return fail("epoll_pwait2 of negative seconds was not EINVAL");
    }
    close(e);
    close(ep);
    return 0;
}

/* An AF_UNIX stream end watched edge-triggered for output alone is not
 * reported again when a byte comes to it, and one watched for input is
 * reported for each byte that comes. */
static int unix_kinds(void)
{
    struct epoll_event ev, out;
    int ep = epoll_create1(0);
    int ein = epoll_create1(0);
    int sv[2];

    memset(&ev, 0, sizeof(ev));
    ev.events = EPOLLOUT | EPOLLET;
    if (ep < 0 || ein < 0 || socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0 ||
        epoll_ctl(ep, EPOLL_CTL_ADD, sv[0], &ev) != 0 || wait_one(ep, 0, &out) != 1) {
        return fail("setup for an AF_UNIX end watched for output");
    }
    ev.events = EPOLLIN | EPOLLET;
    if (epoll_ctl(ein, EPOLL_CTL_ADD, sv[0], &ev) != 0 || write(sv[1], "a", 1) != 1 ||
        wait_one(ein, 0, &out) != 1) {
        return fail("setup for an AF_UNIX end watched for input");
    }
    if (wait_one(ep, 0, &out) != 0) {
        return fail("a byte sent to an AF_UNIX end watched for output alone was reported");
    }
    if (write(sv[1], "b", 1) != 1 || wait_one(ein, 0, &out) != 1) {
        return fail("a second byte sent to an AF_UNIX end watched for input was not reported");
    }
    close(sv[0]);
    close(sv[1]);
    close(ein);
    close(ep);
    return 0;
}

/* Each write to a pipe is an edge of an edge-triggered registration of its
 * read end for input, the pipe empty before it or not, as Linux has it for a
 * pipe it polls. */
static int pipe_each_write(void)
{
    struct epoll_event ev, out;
    int ep = epoll_create1(0);
    int p[2];

    memset(&ev, 0, sizeof(ev));
    ev.events = EPOLLIN | EPOLLET;
    if (ep < 0 || pipe(p) != 0 || epoll_ctl(ep, EPOLL_CTL_ADD, p[0], &ev) != 0 ||
        write(p[1], "a", 1) != 1 || wait_one(ep, 0, &out) != 1 || wait_one(ep, 0, &out) != 0) {
        return fail("setup for writes to a pipe watched edge-triggered");
    }
    if (write(p[1], "b", 1) != 1 || wait_one(ep, 0, &out) != 1) {
        return fail("a write to a pipe with data in it was not an edge");
    }
    close(p[0]);
    close(p[1]);
    close(ep);
    return 0;
}

/* A shutdown of an AF_UNIX stream end changes the end itself, and is an
 * edge of every kind: its registration for output is reported again. */
static int unix_shutdown_edge(void)
{
    struct epoll_event ev, out;
    int ep = epoll_create1(0);
    int sv[2];

    memset(&ev, 0, sizeof(ev));
    ev.events = EPOLLOUT | EPOLLET;
    if (ep < 0 || socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0 ||
        epoll_ctl(ep, EPOLL_CTL_ADD, sv[0], &ev) != 0 || wait_one(ep, 0, &out) != 1 ||
        wait_one(ep, 0, &out) != 0) {
        return fail("setup for a shutdown of an AF_UNIX end");
    }
    if (shutdown(sv[0], SHUT_WR) != 0 || wait_one(ep, 0, &out) != 1) {
        return fail("a shutdown of an AF_UNIX end was not an edge of its registration");
    }
    close(sv[0]);
    close(sv[1]);
    close(ep);
    return 0;
}

/* A read that makes room in an AF_UNIX stream end's own full queue is room
 * for the sends of its peer, not its own: an edge-triggered registration of
 * the end for output is not reported again. */
static int unix_own_room(void)
{
    struct epoll_event ev, out;
    char buf[4096];
    int ep = epoll_create1(0);
    int sv[2];

    if (ep < 0 || socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, sv) != 0) {
        return fail("setup for an AF_UNIX end with its queue full");
    }
    memset(buf, 'f', sizeof(buf));
    while (write(sv[1], buf, sizeof(buf)) > 0) {
    }
    memset(&ev, 0, sizeof(ev));
    ev.events = EPOLLOUT | EPOLLET;
    if (errno != EAGAIN || epoll_ctl(ep, EPOLL_CTL_ADD, sv[0], &ev) != 0 ||
        wait_one(ep, 0, &out) != 1) {
        return fail("setup for an AF_UNIX end with its queue full");
    }
    if (read(sv[0], buf, 1) != 1 || wait_one(ep, 0, &out) != 0) {
        return fail("room made in an AF_UNIX end's own queue was reported as room to write");
    }
    close(sv[0]);
    close(sv[1]);
    close(ep);
    return 0;
}

/* An AF_UNIX listener watched edge-triggered for input is not reported
 * again when an accept leaves a second connection queued: no connection
 * came. */
static int unix_accept_edge(void)
{
    struct sockaddr_un a;
    struct epoll_event ev, out;
    socklen_t alen;
    int ep = epoll_create1(0);
    int ls = socket(AF_UNIX, SOCK_STREAM, 0);
    int c1 = socket(AF_UNIX, SOCK_STREAM, 0);
    int c2 = socket(AF_UNIX, SOCK_STREAM, 0);
    int as;

    memset(&a, 0, sizeof(a));
    a.sun_family = AF_UNIX;
    memcpy(a.sun_path + 1, "epollmusl", 9);
    alen = (socklen_t)(offsetof(struct sockaddr_un, sun_path) + 10);
    memset(&ev, 0, sizeof(ev));
    ev.events = EPOLLIN | EPOLLET;
    if (ep < 0 || ls < 0 || c1 < 0 || c2 < 0 || bind(ls, (struct sockaddr *)&a, alen) != 0 ||
        listen(ls, 4) != 0 || connect(c1, (struct sockaddr *)&a, alen) != 0 ||
        connect(c2, (struct sockaddr *)&a, alen) != 0 ||
        epoll_ctl(ep, EPOLL_CTL_ADD, ls, &ev) != 0 || wait_one(ep, 0, &out) != 1) {
        return fail("setup for a listener with two connections queued");
    }
    as = accept(ls, NULL, NULL);
    if (as < 0) {
        return fail("setup for an accept that leaves a connection queued");
    }
    if (wait_one(ep, 0, &out) != 0) {
        return fail("an accept that left a connection queued was reported as a new one");
    }
    close(as);
    close(c1);
    close(c2);
    close(ls);
    close(ep);
    return 0;
}

/* A datagram that another socket sends over the loopback to a UDP socket
 * watched edge-triggered for output, which can always send, is no edge. */
static int udp_arrival(void)
{
    struct sockaddr_in me;
    struct epoll_event ev, out;
    struct pollfd pfd;
    socklen_t len = sizeof(me);
    int ep = epoll_create1(0);
    int rx = socket(AF_INET, SOCK_DGRAM, 0);
    int tx = socket(AF_INET, SOCK_DGRAM, 0);

    memset(&me, 0, sizeof(me));
    me.sin_family = AF_INET;
    me.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    memset(&ev, 0, sizeof(ev));
    ev.events = EPOLLOUT | EPOLLET;
    if (ep < 0 || rx < 0 || tx < 0 || bind(rx, (struct sockaddr *)&me, sizeof(me)) != 0 ||
        getsockname(rx, (struct sockaddr *)&me, &len) != 0 ||
        epoll_ctl(ep, EPOLL_CTL_ADD, rx, &ev) != 0 || wait_one(ep, 0, &out) != 1) {
        return fail("setup for a datagram socket watched for output");
    }
    pfd.fd = rx;
    pfd.events = POLLIN;
    pfd.revents = 0;
    if (sendto(tx, "u", 1, 0, (struct sockaddr *)&me, sizeof(me)) != 1 ||
        poll(&pfd, 1, 5000) != 1) {
        return fail("setup: a datagram sent over the loopback did not arrive");
    }
    if (wait_one(ep, 0, &out) != 0) {
        return fail("a datagram that came to a socket watched for output alone was reported");
    }
    close(tx);
    close(rx);
    close(ep);
    return 0;
}

/* Data that the other end of a TCP connection over the loopback sends to an
 * end watched edge-triggered for output is no edge. */
static int tcp_arrival(void)
{
    struct sockaddr_in sa;
    struct epoll_event ev, out;
    struct pollfd pfd;
    int ep = epoll_create1(0);
    int ls = socket(AF_INET, SOCK_STREAM, 0);
    int cs = socket(AF_INET, SOCK_STREAM, 0);
    int as;

    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons(EDGE_PORT);
    sa.sin_addr.s_addr = htonl(INADDR_ANY);
    if (ep < 0 || ls < 0 || cs < 0 || bind(ls, (struct sockaddr *)&sa, sizeof(sa)) != 0 ||
        listen(ls, 1) != 0) {
        return fail("setup for a TCP listener on the loopback");
    }
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(cs, (struct sockaddr *)&sa, sizeof(sa)) != 0) {
        return fail("setup for a TCP connection over the loopback");
    }
    as = accept(ls, NULL, NULL);
    memset(&ev, 0, sizeof(ev));
    ev.events = EPOLLOUT | EPOLLET;
    if (as < 0 || epoll_ctl(ep, EPOLL_CTL_ADD, cs, &ev) != 0 || wait_one(ep, 0, &out) != 1) {
        return fail("setup for a TCP connection watched for output");
    }
    pfd.fd = cs;
    pfd.events = POLLIN;
    pfd.revents = 0;
    if (write(as, "t", 1) != 1 || poll(&pfd, 1, 5000) != 1) {
        return fail("setup: data sent over a TCP connection did not arrive");
    }
    if (wait_one(ep, 0, &out) != 0) {
        return fail("data that came to a TCP connection watched for output alone was reported");
    }
    close(as);
    close(cs);
    close(ls);
    close(ep);
    return 0;
}

/* Six sets, each added to the one before: five may nest, as in Linux, and a
 * pipe in the fifth is reported by a wait on the first; the sixth is ELOOP,
 * counting the sets above the one it is added to when the chain is built from
 * the outermost in, and those below it when built from the innermost out. */
static int nested_six(void)
{
    struct epoll_event ev, out;
    int e[6], f[6];
    int p[2];
    int i;

    memset(&ev, 0, sizeof(ev));
    ev.events = EPOLLIN;
    for (i = 0; i < 6; i++) {
        e[i] = epoll_create1(0);
        f[i] = epoll_create1(0);
        if (e[i] < 0 || f[i] < 0) {
            return fail("setup for six sets");
        }
    }
    if (pipe(p) != 0 || epoll_ctl(e[4], EPOLL_CTL_ADD, p[0], &ev) != 0) {
        return fail("setup for six sets");
    }
    for (i = 0; i < 4; i++) {
        if (epoll_ctl(e[i], EPOLL_CTL_ADD, e[i + 1], &ev) != 0) {
            return fail("an add that nests five sets, the outermost first, was refused");
        }
    }
    if (epoll_ctl(e[4], EPOLL_CTL_ADD, e[5], &ev) != -1 || errno != ELOOP) {
        return fail("a sixth set added below five, the outermost first, was not ELOOP");
    }
    if (write(p[1], "n", 1) != 1 || wait_one(e[0], 0, &out) != 1) {
        return fail("a write to a pipe in the fifth of five nested sets did not reach the first");
    }
    for (i = 4; i > 0; i--) {
        if (epoll_ctl(f[i], EPOLL_CTL_ADD, f[i + 1], &ev) != 0) {
            return fail("an add that nests five sets, the innermost first, was refused");
        }
    }
    if (epoll_ctl(f[0], EPOLL_CTL_ADD, f[1], &ev) != -1 || errno != ELOOP) {
        return fail("a sixth set added above five, the innermost first, was not ELOOP");
    }
    for (i = 0; i < 6; i++) {
        close(e[i]);
        close(f[i]);
    }
    close(p[0]);
    close(p[1]);
    return 0;
}

/* A set held by two paths: I holds A, A holds X, and I holds X as well, and
 * X holds P, which holds Q. Adding I to a sixth set E would put Q five below
 * E by the longer path, which is ELOOP, whichever path a check takes first;
 * X, added to I last, is the first I's list gives. */
static int nested_two_paths(void)
{
    struct epoll_event ev;
    int e = epoll_create1(0);
    int in = epoll_create1(0);
    int a = epoll_create1(0);
    int x = epoll_create1(0);
    int p = epoll_create1(0);
    int q = epoll_create1(0);

    memset(&ev, 0, sizeof(ev));
    ev.events = EPOLLIN;
    if (e < 0 || in < 0 || a < 0 || x < 0 || p < 0 || q < 0 ||
        epoll_ctl(p, EPOLL_CTL_ADD, q, &ev) != 0 || epoll_ctl(x, EPOLL_CTL_ADD, p, &ev) != 0 ||
        epoll_ctl(a, EPOLL_CTL_ADD, x, &ev) != 0 || epoll_ctl(in, EPOLL_CTL_ADD, a, &ev) != 0 ||
        epoll_ctl(in, EPOLL_CTL_ADD, x, &ev) != 0) {
        return fail("setup for a set held by two paths");
    }
    if (epoll_ctl(e, EPOLL_CTL_ADD, in, &ev) != -1 || errno != ELOOP) {
        return fail("a set nested five below another by the longer of two paths was not ELOOP");
    }
    close(q);
    close(p);
    close(x);
    close(a);
    close(in);
    close(e);
    return 0;
}

int main(void)
{
    if (counters() || sets() || hangup_no_events() || number_reused() || added_while_waiting() ||
        polled_after_edge() || polled_set_woken() || nested() || in_turn() || failed_copy() ||
        long_timeout() || pipe_each_write() || unix_kinds() || unix_shutdown_edge() ||
        unix_own_room() || unix_accept_edge() || udp_arrival() || tcp_arrival() || nested_six() ||
        nested_two_paths()) {
        return 1;
    }
    printf("EPOLLMUSL OK\n");
    return 0;
}
