/**
 * epollmusl: check eventfd and epoll.
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
 * removed by EPOLL_CTL_DEL and dropped when its descriptor is closed; the add
 * of a regular file fails with EPERM and a second add with EEXIST. A child's
 * write wakes a parent the child has seen asleep in an epoll_wait with no
 * timeout. The child keeps running until the parent answers, so that the
 * kernel does not idle, which would scan the set again at every interrupt,
 * and only the write can end the wait. A registration of
 * no events reports a hangup. A pipe added to a set while a wait on it
 * sleeps, then written, wakes the wait. A set whose edge-triggered event was
 * reported does not poll readable, and a poll of a set is woken by a child's
 * write to a pipe it watches. A write to a pipe in a set inside a set inside
 * a third wakes a wait on the third, and a cycle of sets, of three or of two,
 * is ELOOP. Five sets may nest, each added to the one before, and a write to
 * a pipe in the fifth reaches a wait on the first; a sixth set is ELOOP
 * whether the chain is built from the outermost set in or from the innermost
 * out, and so is a set that another reaches by two paths when the longer would
 * make a chain of six. A child of a fork that closes its copy of the second
 * of five nested sets' numbers, which the parent's keeps open, gets ELOOP for
 * a sixth set under them and for them under a sixth, and its wait on the
 * first leaves the registration to the parent, whose wait reports a write to
 * a pipe in the fifth. Waits with room for one event report two ready
 * descriptors in turn. A one-shot event a wait cannot store, EFAULT, is
 * reported by the next wait. The registration of a closed eventfd does not
 * report the eventfd made next at its number, which can be added.
 * epoll_pwait2 takes a timeout of 2^31 seconds, and fails with EINVAL for
 * negative seconds.
 *
 * An edge-triggered registration reports only the kinds of change its events
 * watch, as in Linux: an AF_UNIX stream end watched for output is not
 * reported again when a byte comes to it, nor when a read makes room in its
 * own full queue, while one watched for input is reported for each byte, and
 * a shutdown of the end is an edge of every kind; a listener is not reported
 * again when an accept leaves a connection queued; a datagram, or a TCP
 * segment over the loopback, that comes to a socket watched for output is no
 * edge; each write to a pipe is an edge of its read end watched for input,
 * the pipe empty before it or not; and room a VINTR makes in a full
 * pseudoterminal is an edge of its master watched for output.
 *
 * Prints `EPOLLMUSL OK` and exits 0 on success, and prints
 * `EPOLLMUSL FAIL (detail: errno N)` and exits 1 otherwise. Writes etude's
 * report records (../probe_report.h) for the case ETUDE_CASE names, an
 * assertion for each check `checks` lists; a failure leaves the checks after
 * it not run. The TCP listener is on
 * port 7470 of every address; given `ports=OLD:NEW,...`, as a native run
 * gives it (tests/native/Readme.md), it is on the NEW of 7470 of 127.0.0.1.
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
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/mman.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

#include "../probe_args.h"
#include "../probe_report.h"

/* The port of the TCP connection over the loopback, which no other probe
 * listens on, and the address its listener binds: every address, or
 * 127.0.0.1 alone for a port of a native run's map. */
#define EDGE_PORT ((uint16_t)probe_port(7470))
#define EDGE_BIND (probe_arg("ports", NULL) != NULL ? INADDR_LOOPBACK : INADDR_ANY)

/* The checks, one assertion each, in the order the program makes them, and
 * the text it prints for each that fails. */
enum {
    EVENTFD_REGS,
    PIPE_REGS,
    EVENTFD,
    READ_ZERO_EAGAIN,
    READ_ZERO_BYTES,
    ZERO_NOT_READABLE,
    EVENTFD_WRITE,
    COUNTER_WHOLE,
    COUNTER_EMPTY,
    ALL_ONES,
    SEMAPHORE_ONE,
    SEMAPHORE_EMPTY,
    SETUP,
    CTL_ADD,
    ADD_EEXIST,
    FILE_EPERM,
    EMPTY_PIPE,
    LEVEL_DATA,
    LEVEL_AGAIN,
    EDGE_ONCE,
    EDGE_AGAIN,
    ONESHOT_ONCE,
    ONESHOT_REARM,
    CTL_DEL,
    CLOSED_SETUP,
    CLOSED_DROPPED,
    WAKE_SETUP,
    WAKE_REAPED,
    WAKE_ASLEEP,
    WAKE_WRITE,
    WAKE_WOKEN,
    WAKE_EVENT,
    HANGUP_SETUP,
    NO_EVENTS_QUIET,
    NO_EVENTS_HANGUP,
    REUSED_SETUP,
    REUSED_QUIET,
    REUSED_ADD,
    ADDED_SETUP,
    ADDED_ASLEEP,
    ADDED_CHILD,
    ADDED_WOKEN,
    EDGE_POLL_SETUP,
    EDGE_POLL_QUIET,
    POLL_SETUP,
    POLL_ASLEEP,
    POLL_CHILD,
    POLL_WOKEN,
    NESTED_SETUP,
    CHAIN_EMPTY,
    CYCLE_ELOOP,
    NESTED_WAKE_SETUP,
    NESTED_ASLEEP,
    NESTED_CHILD,
    NESTED_WOKEN,
    TURN_SETUP,
    IN_TURN,
    COPY_SETUP,
    COPY_EFAULT,
    COPY_AGAIN,
    PWAIT2_SETUP,
    PWAIT2_LONG,
    PWAIT2_NEGATIVE,
    PIPE_EDGE_SETUP,
    PIPE_EDGE_EACH,
    UNIX_OUT_SETUP,
    UNIX_IN_SETUP,
    UNIX_OUT_QUIET,
    UNIX_IN_EACH,
    SHUTDOWN_SETUP,
    SHUTDOWN_EDGE,
    OWN_ROOM_SETUP,
    OWN_ROOM_QUIET,
    ACCEPT_SETUP,
    ACCEPT_ONE,
    ACCEPT_QUIET,
    UDP_SETUP,
    UDP_ARRIVES,
    UDP_QUIET,
    TCP_LISTEN,
    TCP_CONNECT,
    TCP_WATCH,
    TCP_ARRIVES,
    TCP_QUIET,
    PTY_SETUP,
    PTY_ROOM,
    PTY_FULL,
    PTY_VINTR,
    PTY_EDGE,
    SIX_SETUP,
    FIVE_OUTER,
    SIXTH_BELOW,
    FIVE_REACH,
    FIVE_INNER,
    SIXTH_ABOVE,
    TWO_PATHS_SETUP,
    TWO_PATHS_ELOOP,
    FORKED_SETUP,
    FORKED_FORK,
    FORKED_CHILD,
    FORKED_BELOW,
    FORKED_ABOVE,
    FORKED_WAIT,
    FORKED_KEPT,
    CHECKS
};

/* The register eventfd and pipe must leave as it was, named in their checks. */
#if defined __x86_64__
#define CALLER_REG "RSI"
#else
#define CALLER_REG "ECX"
#endif

static const struct {
    const char *id;
    const char *text;
} checks[CHECKS] = {
    {"eventfd-regs", "eventfd changed the caller's " CALLER_REG},
    {"pipe-regs", "pipe changed the caller's " CALLER_REG},
    {"eventfd", "eventfd"},
    {"read-zero-eagain", "a nonblocking read of zero was not EAGAIN"},
    {"read-zero-bytes", "a read of zero bytes was not EINVAL"},
    {"zero-not-readable", "a zero counter polled readable"},
    {"eventfd-write", "eventfd write"},
    {"counter-whole", "the counter did not come back whole"},
    {"counter-empty", "a read did not leave the counter empty"},
    {"all-ones", "a write of all ones was not EINVAL"},
    {"semaphore-one", "EFD_SEMAPHORE did not take one at a time"},
    {"semaphore-empty", "three reads with EFD_SEMAPHORE did not empty a counter of 3"},
    {"setup", "setup"},
    {"ctl-add", "EPOLL_CTL_ADD"},
    {"add-eexist", "a second add was not EEXIST"},
    {"file-eperm", "a regular file was not EPERM"},
    {"empty-pipe", "an empty pipe was reported"},
    {"level-data", "level-triggered data, or the data word"},
    {"level-again", "level-triggered data was not reported again"},
    {"edge-once", "edge-triggered data was reported twice"},
    {"edge-again", "an edge after the data went and came back"},
    {"oneshot-once", "EPOLLONESHOT reported twice"},
    {"oneshot-rearm", "EPOLL_CTL_MOD did not rearm EPOLLONESHOT"},
    {"ctl-del", "EPOLL_CTL_DEL"},
    {"closed-setup", "setup for a closed descriptor"},
    {"closed-dropped", "a closed descriptor was reported"},
    {"wake-setup", "setup for the child's wake"},
    {"wake-reaped", "the child that writes"},
    {"wake-asleep", "the parent was never seen asleep in epoll_wait"},
    {"wake-write", "the child's write"},
    {"wake-woken", "the wait with no timeout ended without an event"},
    {"wake-event", "the wait the child's write ended"},
    {"hangup-setup", "setup for a registration of no events"},
    {"no-events-quiet", "a registration of no events was reported"},
    {"no-events-hangup", "a registration of no events was not told of the hangup"},
    {"reused-setup", "setup for a reused number"},
    {"reused-quiet", "a closed eventfd's registration reported the one after it"},
    {"reused-add", "the eventfd at a closed one's number could not be added"},
    {"added-setup", "setup for an add during a wait"},
    {"added-asleep", "the parent was never seen asleep in epoll_wait"},
    {"added-child", "the child that added the pipe"},
    {"added-woken", "a pipe added during a wait did not wake it"},
    {"edge-poll-setup", "setup for a poll after an edge"},
    {"edge-poll-quiet", "a set with its edge reported polled readable"},
    {"poll-setup", "setup for a poll of a set"},
    {"poll-asleep", "the parent was never seen asleep in poll"},
    {"poll-child", "the writing child"},
    {"poll-woken", "a write to a pipe in a set did not wake a poll of the set"},
    {"nested-setup", "setup for sets inside sets"},
    {"chain-empty", "a chain of sets over an empty pipe"},
    {"cycle-eloop", "a cycle of sets was not ELOOP"},
    {"nested-wake-setup", "setup for sets inside sets"},
    {"nested-asleep", "the parent was never seen asleep in epoll_wait"},
    {"nested-child", "the writing child"},
    {"nested-woken", "a write to a pipe two sets deep did not wake the outer set"},
    {"turn-setup", "setup for waits in turn"},
    {"in-turn", "two waits for one event reported the same descriptor"},
    {"copy-setup", "setup for a failed copy"},
    {"copy-efault", "a wait into a read-only buffer was not EFAULT"},
    {"copy-again", "a one-shot event a failed copy took was not reported again"},
    {"pwait2-setup", "setup for epoll_pwait2"},
    {"pwait2-long", "epoll_pwait2 of 2^31 seconds with a descriptor ready"},
    {"pwait2-negative", "epoll_pwait2 of negative seconds was not EINVAL"},
    {"pipe-edge-setup", "setup for writes to a pipe watched edge-triggered"},
    {"pipe-edge-each", "a write to a pipe with data in it was not an edge"},
    {"unix-out-setup", "setup for an AF_UNIX end watched for output"},
    {"unix-in-setup", "setup for an AF_UNIX end watched for input"},
    {"unix-out-quiet", "a byte sent to an AF_UNIX end watched for output alone was reported"},
    {"unix-in-each", "a second byte sent to an AF_UNIX end watched for input was not reported"},
    {"shutdown-setup", "setup for a shutdown of an AF_UNIX end"},
    {"shutdown-edge", "a shutdown of an AF_UNIX end was not an edge of its registration"},
    {"own-room-setup", "setup for an AF_UNIX end with its queue full"},
    {"own-room-quiet", "room made in an AF_UNIX end's own queue was reported as room to write"},
    {"accept-setup", "setup for a listener with two connections queued"},
    {"accept-one", "setup for an accept that leaves a connection queued"},
    {"accept-quiet", "an accept that left a connection queued was reported as a new one"},
    {"udp-setup", "setup for a datagram socket watched for output"},
    {"udp-arrives", "setup: a datagram sent over the loopback did not arrive"},
    {"udp-quiet", "a datagram that came to a socket watched for output alone was reported"},
    {"tcp-listen", "setup for a TCP listener on the loopback"},
    {"tcp-connect", "setup for a TCP connection over the loopback"},
    {"tcp-watch", "setup for a TCP connection watched for output"},
    {"tcp-arrives", "setup: data sent over a TCP connection did not arrive"},
    {"tcp-quiet", "data that came to a TCP connection watched for output alone was reported"},
    {"pty-setup", "setup for a pseudoterminal's room"},
    {"pty-room", "a pseudoterminal master with room was not reported for output"},
    {"pty-full", "a pseudoterminal master was not filled to EAGAIN"},
    {"pty-vintr", "a VINTR written to a full pseudoterminal master was not taken"},
    {"pty-edge", "room a VINTR made in a pseudoterminal was not an edge of its master"},
    {"six-setup", "setup for six sets"},
    {"five-outer", "an add that nests five sets, the outermost first, was refused"},
    {"sixth-below", "a sixth set added below five, the outermost first, was not ELOOP"},
    {"five-reach", "a write to a pipe in the fifth of five nested sets did not reach the first"},
    {"five-inner", "an add that nests five sets, the innermost first, was refused"},
    {"sixth-above", "a sixth set added above five, the innermost first, was not ELOOP"},
    {"two-paths-setup", "setup for a set held by two paths"},
    {"two-paths-eloop", "a set nested five below another by the longer of two paths was not ELOOP"},
    {"forked-setup", "setup for a child's nested sets"},
    {"forked-fork", "fork for a child's nested sets"},
    {"forked-child", "the child of the nested sets"},
    {"forked-below", "a child's add under five sets, one number closed, was not ELOOP"},
    {"forked-above", "a child's add of five sets under a sixth, one number closed, was not ELOOP"},
    {"forked-wait", "a child's wait on the nested sets"},
    {"forked-kept", "a child's close and wait took the parent's registration of a set"},
};

/** In the process that began the report, report every check before `check`
 * as held, `check` as failed with its text, those after it as not run, and
 * the report's end; with `check` CHECKS, every check held. */
static void report_at(int check)
{
    int i;

    if (report_ended || report_getpid() != report_owner) {
        return;
    }
    for (i = 0; i < CHECKS; i++) {
        if (i < check) {
            report_pass(checks[i].id);
        } else if (i == check) {
            report_fail(checks[i].id, checks[i].text);
        } else {
            report_notrun(checks[i].id, "an earlier check failed");
        }
    }
    report_end();
}

/** Report the failed check `check`, with errno, and return 1, the probe's
 * failure status. */
static int fail(int check)
{
    printf("EPOLLMUSL FAIL (%s: errno %d)\n", checks[check].text, errno);
    report_at(check);

    return 1;
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
 * Wait to see `parent` asleep (`seen_asleep`), write a byte to `out`, and keep
 * running, reading the nonblocking `ack` in a loop, until it has a byte. A
 * process that runs keeps the kernel from idling, and an idle kernel scans the
 * parent's descriptors again at every interrupt, which would end the wait
 * without the write's wake. Runs in the forked child and does not return:
 * exits 0 when all of that happened, 2 when the parent was never seen asleep,
 * and 1 when the write failed.
 */
static void writer(pid_t parent, int out, int ack)
{
    char byte;

    if (!seen_asleep(parent)) {
        _exit(2);
    }
    if (write(out, "w", 1) != 1) {
        _exit(1);
    }
    while (read(ack, &byte, 1) != 1) {
    }
    _exit(0);
}

/* A child a row starts to write once it sees the parent asleep, and the
 * pipe the parent answers it on. */
struct woken {
    pid_t pid;
    int ack[2];
};

/**
 * Wait to see `parent` asleep (`seen_asleep`), then add `in` to the set `ep`
 * for input, with the descriptor as its data. Runs in the forked child and
 * returns when the add succeeds; exits 1 when the add fails and 2 when the
 * parent was never seen asleep.
 */
static void add_when_asleep(pid_t parent, int ep, int in)
{
    struct epoll_event ev;

    if (!seen_asleep(parent)) {
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
 * Start a child that runs `writer` on `out`, whose write must end the wait
 * the caller begins next with no timeout. A wake the kernel misses leaves the
 * wait asleep, and the case's watchdog ends it. With `ep` not -1, the child
 * first adds `in` to the set `ep` once it sees this process asleep. Returns
 * 0, or -1.
 */
static int wake_start(struct woken *w, int out, int ep, int in)
{
    if (pipe(w->ack) != 0 || fcntl(w->ack[0], F_SETFL, O_NONBLOCK) != 0) {
        return -1;
    }
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
    return 0;
}

/**
 * End the row `wake_start` began: answer the child and reap it. Returns 0
 * when the child did its part, 2 when it never saw this process asleep, 1
 * when it failed otherwise, and -1 when it could not be answered or reaped.
 */
static int wake_end(struct woken *w)
{
    int status;
    int reaped;

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

/**
 * Check an eventfd's counter, with and without EFD_SEMAPHORE, its errors, and
 * poll. Returns 0, or 1 once the failure is reported.
 */
static int counters(void)
{
    struct pollfd pfd;
    uint64_t v;
    int e = eventfd(0, EFD_NONBLOCK);
    int s = eventfd(3, EFD_SEMAPHORE | EFD_NONBLOCK);
    int i;

    if (e < 0 || s < 0) {
        return fail(EVENTFD);
    }
    if (read(e, &v, sizeof(v)) != -1 || errno != EAGAIN) {
        return fail(READ_ZERO_EAGAIN);
    }
    if (read(e, &v, 0) != -1 || errno != EINVAL) {
        return fail(READ_ZERO_BYTES);
    }
    pfd.fd = e;
    pfd.events = POLLIN;
    if (poll(&pfd, 1, 0) != 0) {
        return fail(ZERO_NOT_READABLE);
    }
    v = 2;
    if (write(e, &v, sizeof(v)) != 8 || write(e, &v, sizeof(v)) != 8) {
        return fail(EVENTFD_WRITE);
    }
    if (poll(&pfd, 1, 0) != 1 || read(e, &v, sizeof(v)) != 8 || v != 4) {
        return fail(COUNTER_WHOLE);
    }
    if (read(e, &v, sizeof(v)) != -1 || errno != EAGAIN) {
        return fail(COUNTER_EMPTY);
    }
    v = 0xFFFFFFFFFFFFFFFFull;
    if (write(e, &v, sizeof(v)) != -1 || errno != EINVAL) {
        return fail(ALL_ONES);
    }
    for (i = 0; i < 3; i++) {
        if (read(s, &v, sizeof(v)) != 8 || v != 1) {
            return fail(SEMAPHORE_ONE);
        }
    }
    if (read(s, &v, sizeof(v)) != -1 || errno != EAGAIN) {
        return fail(SEMAPHORE_EMPTY);
    }
    close(e);
    close(s);
    return 0;
}

/** Wait on the set `ep` for one event into `out`; epoll_wait's result. */
static int wait_one(int ep, int timeout, struct epoll_event *out)
{
    return epoll_wait(ep, out, 1, timeout);
}

/**
 * Check an epoll set over pipes: level-triggered, edge-triggered and one-shot
 * reports, the data word, a timeout, removal, the errors of an add, and a
 * child's write that wakes a wait with no timeout. Returns 0, or 1 once the
 * failure is reported.
 */
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
        return fail(SETUP);
    }
    memset(&ev, 0, sizeof(ev));
    ev.events = EPOLLIN;
    ev.data.u64 = 0x1122334455667788ull;
    if (epoll_ctl(ep, EPOLL_CTL_ADD, p[0], &ev) != 0) {
        return fail(CTL_ADD);
    }
    if (epoll_ctl(ep, EPOLL_CTL_ADD, p[0], &ev) != -1 || errno != EEXIST) {
        return fail(ADD_EEXIST);
    }
    if (file >= 0 && (epoll_ctl(ep, EPOLL_CTL_ADD, file, &ev) != -1 || errno != EPERM)) {
        return fail(FILE_EPERM);
    }
    if (wait_one(ep, 50, &out) != 0) {
        return fail(EMPTY_PIPE);
    }
    if (write(p[1], "ab", 2) != 2 || wait_one(ep, 0, &out) != 1 ||
        out.data.u64 != 0x1122334455667788ull || !(out.events & EPOLLIN)) {
        return fail(LEVEL_DATA);
    }
    if (wait_one(ep, 0, &out) != 1) {
        return fail(LEVEL_AGAIN);
    }

    ev.events = EPOLLIN | EPOLLET;
    if (epoll_ctl(ep, EPOLL_CTL_MOD, p[0], &ev) != 0 || wait_one(ep, 0, &out) != 1 ||
        wait_one(ep, 0, &out) != 0) {
        return fail(EDGE_ONCE);
    }
    if (read(p[0], buf, 2) != 2 || write(p[1], "c", 1) != 1 || wait_one(ep, 0, &out) != 1) {
        return fail(EDGE_AGAIN);
    }
    (void)read(p[0], buf, 1);

    ev.events = EPOLLIN | EPOLLONESHOT;
    if (epoll_ctl(ep, EPOLL_CTL_MOD, p[0], &ev) != 0 || write(p[1], "d", 1) != 1 ||
        wait_one(ep, 0, &out) != 1 || wait_one(ep, 0, &out) != 0) {
        return fail(ONESHOT_ONCE);
    }
    if (epoll_ctl(ep, EPOLL_CTL_MOD, p[0], &ev) != 0 || wait_one(ep, 0, &out) != 1) {
        return fail(ONESHOT_REARM);
    }
    if (epoll_ctl(ep, EPOLL_CTL_DEL, p[0], NULL) != 0 || wait_one(ep, 0, &out) != 0) {
        return fail(CTL_DEL);
    }
    (void)read(p[0], buf, 1);

    ev.events = EPOLLIN;
    if (epoll_ctl(ep, EPOLL_CTL_ADD, q[0], &ev) != 0 || write(q[1], "e", 1) != 1) {
        return fail(CLOSED_SETUP);
    }
    close(q[0]);
    if (wait_one(ep, 0, &out) != 0) {
        return fail(CLOSED_DROPPED);
    }

    ev.events = EPOLLIN;
    ev.data.u32 = 7;
    if (epoll_ctl(ep, EPOLL_CTL_ADD, p[0], &ev) != 0) {
        return fail(WAKE_SETUP);
    }
    if (wake_start(&w, p[1], -1, -1) != 0) {
        return fail(WAKE_SETUP);
    }
    n = wait_one(ep, -1, &out);
    rc = wake_end(&w);
    if (rc != 0) {
        return fail(rc < 0 ? WAKE_REAPED : rc == 2 ? WAKE_ASLEEP : WAKE_WRITE);
    }
    if (n != 1) {
        return fail(WAKE_WOKEN);
    }
    if (out.data.u32 != 7) {
        return fail(WAKE_EVENT);
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

/**
 * Check that a wait on an empty set is woken by a pipe added to the set while
 * it waits and then written: a child shares the set, adds the pipe once it
 * sees the wait asleep, lets the wait look again, and writes once it sees it
 * asleep again. Returns 0, or 1 once the failure is reported.
 */
static int added_while_waiting(void)
{
    struct epoll_event out;
    struct woken w;
    int ep = epoll_create1(0);
    int p[2], n, rc;

    if (ep < 0 || pipe(p) != 0 || wake_start(&w, p[1], ep, p[0]) != 0) {
        return fail(ADDED_SETUP);
    }
    n = wait_one(ep, -1, &out);
    rc = wake_end(&w);
    if (rc != 0) {
        return fail(rc == 2 ? ADDED_ASLEEP : ADDED_CHILD);
    }
    if (n != 1 || out.data.u32 != (uint32_t)p[0]) {
        return fail(ADDED_WOKEN);
    }
    close(p[0]);
    close(p[1]);
    close(ep);
    return 0;
}

/**
 * Check that a set whose only event is edge-triggered and reported already does
 * not poll readable, as a wait on it would report nothing. Returns 0, or 1 once
 * the failure is reported.
 */
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
        return fail(EDGE_POLL_SETUP);
    }
    pfd.fd = ep;
    pfd.events = POLLIN;
    pfd.revents = 0;
    if (wait_one(ep, 0, &out) != 0 || poll(&pfd, 1, 0) != 0) {
        return fail(EDGE_POLL_QUIET);
    }
    close(p[0]);
    close(p[1]);
    close(ep);
    return 0;
}

/**
 * Check sets inside sets: with a pipe in A, A in B and B in C, a child's write
 * to the pipe wakes a wait on C, which reports B while the data is there; and
 * adding C to A, or B to A, a cycle, is ELOOP. Returns 0, or 1 once the failure
 * is reported.
 */
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
        return fail(NESTED_SETUP);
    }
    ev.data.u32 = 9;
    if (epoll_ctl(c, EPOLL_CTL_ADD, b, &ev) != 0 || wait_one(c, 0, &out) != 0) {
        return fail(CHAIN_EMPTY);
    }
    if (epoll_ctl(a, EPOLL_CTL_ADD, c, &ev) != -1 || errno != ELOOP ||
        epoll_ctl(a, EPOLL_CTL_ADD, b, &ev) != -1 || errno != ELOOP) {
        return fail(CYCLE_ELOOP);
    }
    if (wake_start(&w, p[1], -1, -1) != 0) {
        return fail(NESTED_WAKE_SETUP);
    }
    n = wait_one(c, -1, &out);
    rc = wake_end(&w);
    if (rc != 0) {
        return fail(rc == 2 ? NESTED_ASLEEP : NESTED_CHILD);
    }
    if (n != 1 || out.data.u32 != 9 || wait_one(c, 0, &out) != 1) {
        return fail(NESTED_WOKEN);
    }
    close(p[0]);
    close(p[1]);
    close(a);
    close(b);
    close(c);
    return 0;
}

/**
 * Check that a poll of an epoll set is woken by a child's write to a pipe the
 * set watches. Returns 0, or 1 once the failure is reported.
 */
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
        return fail(POLL_SETUP);
    }
    pfd.fd = ep;
    pfd.events = POLLIN;
    pfd.revents = 0;
    n = poll(&pfd, 1, -1);
    rc = wake_end(&w);
    if (rc != 0) {
        return fail(rc == 2 ? POLL_ASLEEP : POLL_CHILD);
    }
    if (n != 1 || !(pfd.revents & POLLIN)) {
        return fail(POLL_WOKEN);
    }
    close(p[0]);
    close(p[1]);
    close(ep);
    return 0;
}

/**
 * Check that waits with room for one event report two ready descriptors in
 * turn. Returns 0, or 1 once the failure is reported.
 */
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
        return fail(TURN_SETUP);
    }
    ev.data.u32 = 2;
    if (epoll_ctl(ep, EPOLL_CTL_ADD, b, &ev) != 0) {
        return fail(TURN_SETUP);
    }
    if (wait_one(ep, 0, &first) != 1 || wait_one(ep, 0, &second) != 1 ||
        first.data.u32 == second.data.u32) {
        return fail(IN_TURN);
    }
    close(a);
    close(b);
    close(ep);
    return 0;
}

/**
 * Check that a one-shot event a wait cannot store in a read-only buffer,
 * EFAULT, is reported by the next wait. Returns 0, or 1 once the failure is
 * reported.
 */
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
        return fail(COPY_SETUP);
    }
    if (epoll_wait(ep, (struct epoll_event *)ro, 1, 0) != -1 || errno != EFAULT) {
        return fail(COPY_EFAULT);
    }
    if (wait_one(ep, 0, &out) != 1) {
        return fail(COPY_AGAIN);
    }
    munmap(ro, 4096);
    close(e);
    close(ep);
    return 0;
}

/**
 * Check that a registration of no events still reports a hangup. Returns 0, or
 * 1 once the failure is reported.
 */
static int hangup_no_events(void)
{
    struct epoll_event ev, out;
    int ep = epoll_create1(0);
    int p[2];

    memset(&ev, 0, sizeof(ev));
    if (ep < 0 || pipe(p) != 0 || epoll_ctl(ep, EPOLL_CTL_ADD, p[0], &ev) != 0) {
        return fail(HANGUP_SETUP);
    }
    if (wait_one(ep, 0, &out) != 0) {
        return fail(NO_EVENTS_QUIET);
    }
    close(p[1]);
    if (wait_one(ep, 0, &out) != 1 || !(out.events & EPOLLHUP)) {
        return fail(NO_EVENTS_HANGUP);
    }
    close(p[0]);
    close(ep);
    return 0;
}

/**
 * Check that a registration of a closed eventfd is not the eventfd made next
 * at its number, whose description the kernel may put at the same address: a
 * wait does not report the new one for it, and the new one can be added.
 * Returns 0, or 1 once the failure is reported.
 */
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
        return fail(REUSED_SETUP);
    }
    close(e);
    again = eventfd(1, 0);
    if (again < 0 || wait_one(ep, 0, &out) != 0) {
        return fail(REUSED_QUIET);
    }
    ev.data.u32 = 2;
    if (epoll_ctl(ep, EPOLL_CTL_ADD, again, &ev) != 0 || wait_one(ep, 0, &out) != 1 ||
        out.data.u32 != 2) {
        return fail(REUSED_ADD);
    }
    close(again);
    close(ep);
    return 0;
}

/**
 * Check that epoll_pwait2 takes 64-bit seconds: a timeout of 2^31 seconds is
 * valid, and with a descriptor ready the wait returns at once; negative
 * seconds are EINVAL. Returns 0, or 1 once the failure is reported.
 */
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
        return fail(PWAIT2_SETUP);
    }
    if (syscall(441, ep, &out, 1, ts, NULL, 8) != 1) {
        return fail(PWAIT2_LONG);
    }
    ts[0] = -1;
    if (syscall(441, ep, &out, 1, ts, NULL, 8) != -1 || errno != EINVAL) {
        return fail(PWAIT2_NEGATIVE);
    }
    close(e);
    close(ep);
    return 0;
}

/**
 * Check that an AF_UNIX stream end watched edge-triggered for output alone is
 * not reported again when a byte comes to it, and one watched for input is
 * reported for each byte that comes. Returns 0, or 1 once the failure is
 * reported.
 */
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
        return fail(UNIX_OUT_SETUP);
    }
    ev.events = EPOLLIN | EPOLLET;
    if (epoll_ctl(ein, EPOLL_CTL_ADD, sv[0], &ev) != 0 || write(sv[1], "a", 1) != 1 ||
        wait_one(ein, 0, &out) != 1) {
        return fail(UNIX_IN_SETUP);
    }
    if (wait_one(ep, 0, &out) != 0) {
        return fail(UNIX_OUT_QUIET);
    }
    if (write(sv[1], "b", 1) != 1 || wait_one(ein, 0, &out) != 1) {
        return fail(UNIX_IN_EACH);
    }
    close(sv[0]);
    close(sv[1]);
    close(ein);
    close(ep);
    return 0;
}

/**
 * Check that each write to a pipe is an edge of an edge-triggered registration
 * of its read end for input, the pipe empty before it or not, as Linux has it
 * for a pipe it polls. Returns 0, or 1 once the failure is reported.
 */
static int pipe_each_write(void)
{
    struct epoll_event ev, out;
    int ep = epoll_create1(0);
    int p[2];

    memset(&ev, 0, sizeof(ev));
    ev.events = EPOLLIN | EPOLLET;
    if (ep < 0 || pipe(p) != 0 || epoll_ctl(ep, EPOLL_CTL_ADD, p[0], &ev) != 0 ||
        write(p[1], "a", 1) != 1 || wait_one(ep, 0, &out) != 1 || wait_one(ep, 0, &out) != 0) {
        return fail(PIPE_EDGE_SETUP);
    }
    if (write(p[1], "b", 1) != 1 || wait_one(ep, 0, &out) != 1) {
        return fail(PIPE_EDGE_EACH);
    }
    close(p[0]);
    close(p[1]);
    close(ep);
    return 0;
}

/**
 * Check that a shutdown of an AF_UNIX stream end, which changes the end
 * itself, is an edge of every kind: its registration for output is reported
 * again. Returns 0, or 1 once the failure is reported.
 */
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
        return fail(SHUTDOWN_SETUP);
    }
    if (shutdown(sv[0], SHUT_WR) != 0 || wait_one(ep, 0, &out) != 1) {
        return fail(SHUTDOWN_EDGE);
    }
    close(sv[0]);
    close(sv[1]);
    close(ep);
    return 0;
}

/**
 * Check that a read that makes room in an AF_UNIX stream end's own full queue,
 * room for the sends of its peer and not its own, does not report an
 * edge-triggered registration of the end for output again. Returns 0, or 1 once
 * the failure is reported.
 */
static int unix_own_room(void)
{
    struct epoll_event ev, out;
    char buf[4096];
    int ep = epoll_create1(0);
    int sv[2];

    if (ep < 0 || socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, sv) != 0) {
        return fail(OWN_ROOM_SETUP);
    }
    memset(buf, 'f', sizeof(buf));
    while (write(sv[1], buf, sizeof(buf)) > 0) {
    }
    memset(&ev, 0, sizeof(ev));
    ev.events = EPOLLOUT | EPOLLET;
    if (errno != EAGAIN || epoll_ctl(ep, EPOLL_CTL_ADD, sv[0], &ev) != 0 ||
        wait_one(ep, 0, &out) != 1) {
        return fail(OWN_ROOM_SETUP);
    }
    if (read(sv[0], buf, 1) != 1 || wait_one(ep, 0, &out) != 0) {
        return fail(OWN_ROOM_QUIET);
    }
    close(sv[0]);
    close(sv[1]);
    close(ep);
    return 0;
}

/**
 * Check that an AF_UNIX listener watched edge-triggered for input is not
 * reported again when an accept leaves a second connection queued, since no
 * connection came. Returns 0, or 1 once the failure is reported.
 */
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
        return fail(ACCEPT_SETUP);
    }
    as = accept(ls, NULL, NULL);
    if (as < 0) {
        return fail(ACCEPT_ONE);
    }
    if (wait_one(ep, 0, &out) != 0) {
        return fail(ACCEPT_QUIET);
    }
    close(as);
    close(c1);
    close(c2);
    close(ls);
    close(ep);
    return 0;
}

/**
 * Check that a datagram that another socket sends over the loopback to a UDP
 * socket watched edge-triggered for output, which can always send, is no
 * edge. Returns 0, or 1 once the failure is reported.
 */
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
        return fail(UDP_SETUP);
    }
    pfd.fd = rx;
    pfd.events = POLLIN;
    pfd.revents = 0;
    if (sendto(tx, "u", 1, 0, (struct sockaddr *)&me, sizeof(me)) != 1 || poll(&pfd, 1, -1) != 1) {
        return fail(UDP_ARRIVES);
    }
    if (wait_one(ep, 0, &out) != 0) {
        return fail(UDP_QUIET);
    }
    close(tx);
    close(rx);
    close(ep);
    return 0;
}

/**
 * Check that data that the other end of a TCP connection over the loopback
 * sends to an end watched edge-triggered for output is no edge. Returns 0, or 1
 * once the failure is reported.
 */
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
    sa.sin_addr.s_addr = htonl(EDGE_BIND);
    if (ep < 0 || ls < 0 || cs < 0 || bind(ls, (struct sockaddr *)&sa, sizeof(sa)) != 0 ||
        listen(ls, 1) != 0) {
        return fail(TCP_LISTEN);
    }
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(cs, (struct sockaddr *)&sa, sizeof(sa)) != 0) {
        return fail(TCP_CONNECT);
    }
    as = accept(ls, NULL, NULL);
    memset(&ev, 0, sizeof(ev));
    ev.events = EPOLLOUT | EPOLLET;
    if (as < 0 || epoll_ctl(ep, EPOLL_CTL_ADD, cs, &ev) != 0 || wait_one(ep, 0, &out) != 1) {
        return fail(TCP_WATCH);
    }
    pfd.fd = cs;
    pfd.events = POLLIN;
    pfd.revents = 0;
    if (write(as, "t", 1) != 1 || poll(&pfd, 1, -1) != 1) {
        return fail(TCP_ARRIVES);
    }
    if (wait_one(ep, 0, &out) != 0) {
        return fail(TCP_QUIET);
    }
    close(as);
    close(cs);
    close(ls);
    close(ep);
    return 0;
}

/**
 * Check that room a signal character makes in a pseudoterminal is an edge of
 * its master watched for output: with ISIG on and echo off on the slave, the
 * master's first report is taken, the master is written until it has no room,
 * and a VINTR written then flushes the slave's input, which the next wait
 * reports with no wait between to see the room go. Returns 0, or 1 once the
 * failure is reported.
 */
static int pty_signal_room(void)
{
    struct epoll_event ev, out;
    struct termios tio;
    int master = posix_openpt(O_RDWR | O_NOCTTY | O_NONBLOCK);
    int ep = epoll_create1(0);
    int slave = -1;
    const char *name;
    void (*was)(int);
    int i;

    if (master < 0 || ep < 0 || grantpt(master) != 0 || unlockpt(master) != 0 ||
        (name = ptsname(master)) == NULL || (slave = open(name, O_RDWR | O_NOCTTY)) < 0 ||
        tcgetattr(slave, &tio) != 0) {
        return fail(PTY_SETUP);
    }
    tio.c_lflag |= ISIG;
    tio.c_lflag &= ~(tcflag_t)(ECHO | ICANON);
    was = signal(SIGINT, SIG_IGN);
    memset(&ev, 0, sizeof(ev));
    ev.events = EPOLLOUT | EPOLLET;
    if (tcsetattr(slave, TCSANOW, &tio) != 0 || epoll_ctl(ep, EPOLL_CTL_ADD, master, &ev) != 0) {
        return fail(PTY_SETUP);
    }
    if (wait_one(ep, 0, &out) != 1) {
        return fail(PTY_ROOM);
    }
    for (i = 0; i < 65536 && write(master, "x", 1) == 1; i++) {
    }
    if (i == 65536 || errno != EAGAIN) {
        return fail(PTY_FULL);
    }
    if (write(master, "\003", 1) != 1) {
        return fail(PTY_VINTR);
    }
    if (wait_one(ep, 0, &out) != 1 || !(out.events & EPOLLOUT)) {
        return fail(PTY_EDGE);
    }
    signal(SIGINT, was);
    close(slave);
    close(master);
    close(ep);
    return 0;
}

/**
 * Check six sets, each added to the one before: five may nest, as in Linux,
 * and a pipe in the fifth is reported by a wait on the first; the sixth is
 * ELOOP, counting the sets above the one it is added to when the chain is
 * built from the outermost in, and those below it when built from the
 * innermost out. Returns 0, or 1 once the failure is reported.
 */
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
            return fail(SIX_SETUP);
        }
    }
    if (pipe(p) != 0 || epoll_ctl(e[4], EPOLL_CTL_ADD, p[0], &ev) != 0) {
        return fail(SIX_SETUP);
    }
    for (i = 0; i < 4; i++) {
        if (epoll_ctl(e[i], EPOLL_CTL_ADD, e[i + 1], &ev) != 0) {
            return fail(FIVE_OUTER);
        }
    }
    if (epoll_ctl(e[4], EPOLL_CTL_ADD, e[5], &ev) != -1 || errno != ELOOP) {
        return fail(SIXTH_BELOW);
    }
    if (write(p[1], "n", 1) != 1 || wait_one(e[0], 0, &out) != 1) {
        return fail(FIVE_REACH);
    }
    for (i = 4; i > 0; i--) {
        if (epoll_ctl(f[i], EPOLL_CTL_ADD, f[i + 1], &ev) != 0) {
            return fail(FIVE_INNER);
        }
    }
    if (epoll_ctl(f[0], EPOLL_CTL_ADD, f[1], &ev) != -1 || errno != ELOOP) {
        return fail(SIXTH_ABOVE);
    }
    for (i = 0; i < 6; i++) {
        close(e[i]);
        close(f[i]);
    }
    close(p[0]);
    close(p[1]);
    return 0;
}

/**
 * Check a set reached by two paths: I has A, A has X, I has X as well, and X
 * has P, which has Q. Adding I to a sixth set E would put Q five below E by the
 * longer path, which is ELOOP, whichever path a check takes first; X, added to
 * I last, is the first in I's list. Returns 0, or 1 once the failure is
 * reported.
 */
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
        return fail(TWO_PATHS_SETUP);
    }
    if (epoll_ctl(e, EPOLL_CTL_ADD, in, &ev) != -1 || errno != ELOOP) {
        return fail(TWO_PATHS_ELOOP);
    }
    close(q);
    close(p);
    close(x);
    close(a);
    close(in);
    close(e);
    return 0;
}

/**
 * Check that a child of a fork shares its parent's sets: s0 has s1, and so on
 * to s4, and a pipe is in s4. The child closes its copy of s1's number, which
 * the parent's keeps open, so s0's registration of s1 lasts and keeps s1: the
 * child's add of a sixth set under s4 is ELOOP, and so is its add of s0 under a
 * sixth; and its wait on s0 leaves the registration to the parent, whose wait
 * on s0 reports a write to the pipe. Returns 0, or 1 once the failure is
 * reported.
 */
static int nested_forked(void)
{
    struct epoll_event ev, out;
    int s[5], p[2];
    int sixth = epoll_create1(0);
    int above = epoll_create1(0);
    int i, status;
    pid_t pid;

    memset(&ev, 0, sizeof(ev));
    ev.events = EPOLLIN;
    for (i = 0; i < 5; i++) {
        s[i] = epoll_create1(0);
    }
    if (sixth < 0 || above < 0 || s[0] < 0 || s[1] < 0 || s[2] < 0 || s[3] < 0 || s[4] < 0 ||
        pipe(p) != 0 || epoll_ctl(s[4], EPOLL_CTL_ADD, p[0], &ev) != 0) {
        return fail(FORKED_SETUP);
    }
    for (i = 0; i < 4; i++) {
        if (epoll_ctl(s[i], EPOLL_CTL_ADD, s[i + 1], &ev) != 0) {
            return fail(FORKED_SETUP);
        }
    }
    pid = fork();
    if (pid < 0) {
        return fail(FORKED_FORK);
    }
    if (pid == 0) {
        close(s[1]);
        if (epoll_ctl(s[4], EPOLL_CTL_ADD, sixth, &ev) != -1 || errno != ELOOP) {
            _exit(1);
        }
        if (epoll_ctl(above, EPOLL_CTL_ADD, s[0], &ev) != -1 || errno != ELOOP) {
            _exit(2);
        }
        _exit(epoll_wait(s[0], &out, 1, 0) < 0 ? 3 : 0);
    }
    if (waitpid(pid, &status, 0) != pid || !WIFEXITED(status) || WEXITSTATUS(status) > 3) {
        return fail(FORKED_CHILD);
    }
    if (WEXITSTATUS(status) == 1) {
        return fail(FORKED_BELOW);
    }
    if (WEXITSTATUS(status) == 2) {
        return fail(FORKED_ABOVE);
    }
    if (WEXITSTATUS(status) == 3) {
        return fail(FORKED_WAIT);
    }
    if (write(p[1], "f", 1) != 1 || wait_one(s[0], 0, &out) != 1) {
        return fail(FORKED_KEPT);
    }
    for (i = 0; i < 5; i++) {
        close(s[i]);
    }
    close(sixth);
    close(above);
    close(p[0]);
    close(p[1]);
    return 0;
}

#if defined __x86_64__
/**
 * Check that eventfd (284) and pipe (22), which the kernel serves as eventfd2
 * and pipe2 without flags, leave the caller's RSI, where those take their
 * flags, as it was: `syscall` keeps every register but RAX, RCX and R11, and a
 * compiler keeps a value in RSI across a call that does not use it. Returns 0,
 * or 1 once the failure is reported.
 */
static int calls_keep_registers(void)
{
    unsigned long si;
    long r;
    int fds[2];

    __asm__ __volatile__("syscall"
                         : "=a"(r), "=S"(si)
                         : "0"(284L), "D"(0L), "1"(0x5A5A5A5A5A5A5A5AUL)
                         : "rcx", "r11", "memory");
    if (r < 0 || si != 0x5A5A5A5A5A5A5A5AUL) {
        return fail(EVENTFD_REGS);
    }
    close((int)r);
    __asm__ __volatile__("syscall"
                         : "=a"(r), "=S"(si)
                         : "0"(22L), "D"(fds), "1"(0x5A5A5A5A5A5A5A5AUL)
                         : "rcx", "r11", "memory");
    if (r != 0 || si != 0x5A5A5A5A5A5A5A5AUL) {
        return fail(PIPE_REGS);
    }
    close(fds[0]);
    close(fds[1]);
    return 0;
}
#else
/**
 * Check that eventfd (323) and pipe (42), which the kernel serves as eventfd2
 * and pipe2 without flags, leave the caller's ECX as it was: a compiler keeps a
 * value there across a call that does not use it. Returns 0, or 1 once the
 * failure is reported.
 */
static int calls_keep_registers(void)
{
    unsigned int c;
    int fds[2];
    int r;

    __asm__ __volatile__("int $0x80"
                         : "=a"(r), "=c"(c)
                         : "0"(323), "b"(0), "1"(0x5A5A5A5Au)
                         : "memory");
    if (r < 0 || c != 0x5A5A5A5Au) {
        return fail(EVENTFD_REGS);
    }
    close(r);
    __asm__ __volatile__("int $0x80"
                         : "=a"(r), "=c"(c)
                         : "0"(42), "b"(fds), "1"(0x5A5A5A5Au)
                         : "memory");
    if (r != 0 || c != 0x5A5A5A5Au) {
        return fail(PIPE_REGS);
    }
    close(fds[0]);
    close(fds[1]);
    return 0;
}
#endif

int main(int argc, char **argv)
{
    probe_args(argc, argv);
    report_begin(report_case(environ, "guest.default.epollmusl"));
    if (calls_keep_registers() || counters() || sets() || hangup_no_events() || number_reused() ||
        added_while_waiting() || polled_after_edge() || polled_set_woken() || nested() ||
        in_turn() || failed_copy() || long_timeout() || pipe_each_write() || unix_kinds() ||
        unix_shutdown_edge() || unix_own_room() || unix_accept_edge() || udp_arrival() ||
        tcp_arrival() || pty_signal_room() || nested_six() || nested_two_paths() ||
        nested_forked()) {
        return 1;
    }
    report_at(CHECKS);
    printf("EPOLLMUSL OK\n");
    return 0;
}
