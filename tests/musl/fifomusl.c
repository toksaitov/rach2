/**
 * fifomusl: check named pipes.
 *
 * mkfifo and mknod with S_IFIFO make files stat reports as FIFOs. With no
 * reader, a nonblocking open for writing fails with ENXIO; a nonblocking
 * open for reading succeeds and reads the end of input. Bytes written at one
 * open are read at the other, and the reader sees the end of input when the
 * writer closes. A nonblocking reader polls without hangup until a writer
 * opens and closes; each read open starts that wait anew, and duplicates
 * share it even after O_NONBLOCK is cleared. Buffered bytes stay readable
 * with hangup until drained. A blocking open for reading waits for a writer:
 * a child opens the FIFO for writing only once it has seen the parent asleep.
 * It writes a byte to a pipe first, which must be there when the parent's
 * open returns.
 * The child's bytes then arrive. An open for reading and writing does not wait,
 * and once full it polls writable when a child's open reads a byte. Two FIFOs
 * whose names have one hash, which the KRFFS root numbers a file by, are two
 * pipes; a renamed FIFO keeps its pipe; and one unlinked while an end is open
 * keeps its pipe for that end, apart from a FIFO made in its name. Through a
 * bind of a directory of the root, a FIFO has the pipe it has through its
 * source, and the last close through the bind of an end of a FIFO unlinked
 * through the source frees its node, which statfs counts.
 *
 * Prints `FIFOMUSL OK` and exits 0 on success, and prints
 * `FIFOMUSL FAIL (detail)` and exits 1 otherwise.
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define FIFO_PATH "/tmp/fifomusl"
#define NODE_PATH "/tmp/fifomusl.node"
/* Two names with one hash, the DJB hash the KRFFS root numbers files by. */
#define TWIN_A    "/tmp/fifomusl.a3"
#define TWIN_B    "/tmp/fifomusl.bP"
#define MOVED     "/tmp/fifomusl.moved"
/* A directory of the KRFFS root and the bind of it. */
#define BIND_SRC  "/tmp/fifomusl.src"
#define BIND_DST  "/tmp/fifomusl.dst"
/* The FIFOs whose nodes the bind's row frees; each node's header is 357
 * bytes, so together they are some 17 KiB of the root. */
#define ALIASES   48

/** Report one failed check with errno and return 1, the failure status. */
static int fail(const char *what)
{
    printf("FIFOMUSL FAIL (%s: errno %d)\n", what, errno);
    return 1;
}

/**
 * Poll `count` entries without waiting. Return 1 if the first entry reports
 * `first` and every later entry reports `rest`, or 0 on any other result.
 */
static int poll_events(struct pollfd *fds, int count, short first, short rest)
{
    int i;
    int ready = (first != 0) + (rest != 0) * (count - 1);

    if (poll(fds, (nfds_t)count, 0) != ready) {
        return 0;
    }
    for (i = 0; i < count; i++) {
        if (fds[i].revents != (i == 0 ? first : rest)) {
            return 0;
        }
    }
    return 1;
}

/**
 * Check FIFO poll before and after writer opens, separately for each read
 * description and its duplicate. Use the caller's FIFO_PATH and close every
 * descriptor before returning 0, or 1 after reporting a failure.
 */
static int reader_hangup(void)
{
    struct pollfd fds[4];
    const char *error = NULL;
    char byte;
    int i, w = -1;

    for (i = 0; i < 4; i++) {
        fds[i].fd = -1;
        fds[i].events = POLLIN;
        fds[i].revents = 0;
    }
    fds[0].fd = open(FIFO_PATH, O_RDONLY | O_NONBLOCK);
    if (fds[0].fd < 0 || !poll_events(fds, 1, 0, 0)) {
        error = "a new FIFO reader reported hangup before a writer opened";
        goto out;
    }
    w = open(FIFO_PATH, O_WRONLY | O_NONBLOCK);
    if (w < 0 || !poll_events(fds, 1, 0, 0)) {
        error = "an empty FIFO with a writer polled ready";
        goto out;
    }
    close(w);
    w = -1;
    if (!poll_events(fds, 1, POLLHUP, 0)) {
        error = "a FIFO reader missed hangup after a writer opened and closed";
        goto out;
    }
    fds[1].fd = open(FIFO_PATH, O_RDONLY | O_NONBLOCK);
    fds[2].fd = dup(fds[1].fd);
    if (fds[1].fd < 0 || fds[2].fd < 0 || fcntl(fds[2].fd, F_SETFL, 0) != 0 ||
        !poll_events(fds, 3, POLLHUP, 0)) {
        error = "FIFO read opens did not keep separate hangup state";
        goto out;
    }
    w = open(FIFO_PATH, O_WRONLY | O_NONBLOCK);
    fds[3].fd = open(FIFO_PATH, O_RDONLY | O_NONBLOCK);
    if (w < 0 || fds[3].fd < 0 || !poll_events(fds, 4, 0, 0) || write(w, "x", 1) != 1) {
        error = "a reopened FIFO writer did not clear hangup";
        goto out;
    }
    close(w);
    w = -1;
    if (!poll_events(fds, 4, POLLIN | POLLHUP, POLLIN | POLLHUP)) {
        error = "FIFO readers missed buffered data with hangup";
        goto out;
    }
    if (read(fds[0].fd, &byte, 1) != 1 || byte != 'x' || !poll_events(fds, 4, POLLHUP, POLLHUP)) {
        error = "FIFO readers missed hangup after draining data";
        goto out;
    }
out:
    if (w >= 0) {
        close(w);
    }
    for (i = 0; i < 4; i++) {
        if (fds[i].fd >= 0) {
            close(fds[i].fd);
        }
    }
    return error ? fail(error) : 0;
}

/* The longest wait, in seconds, for another process to be seen asleep: a
 * bound that ends a wait the kernel never satisfies, far past a correct one. */
#define ASLEEP_BOUND 20

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
 * gone from /proc, or when ASLEEP_BOUND seconds pass.
 */
static int seen_asleep(pid_t pid)
{
    struct timespec start, now;
    int state;

    clock_gettime(CLOCK_MONOTONIC, &start);
    for (;;) {
        state = proc_state(pid);
        if (state == 'S') {
            return 1;
        }
        clock_gettime(CLOCK_MONOTONIC, &now);
        if (state == 0 || state == 'Z' || now.tv_sec - start.tv_sec >= ASLEEP_BOUND) {
            return 0;
        }
        usleep(10000);
    }
}

/**
 * Wait to see `parent` asleep (`seen_asleep`), write `o` to `told`, then open
 * the FIFO for writing and write to it. A parent never seen asleep gets `n`
 * on `told` instead, and an open that does not wait, which ends the parent's
 * open if it is still in one. Runs in the forked child and does not return:
 * exits 0 when all of that happened, 2 when the parent was never seen
 * asleep, and 1 otherwise.
 */
static void writer(pid_t parent, int told)
{
    int w;

    if (!seen_asleep(parent)) {
        if (write(told, "n", 1) == 1) {
            open(FIFO_PATH, O_WRONLY | O_NONBLOCK);
        }
        _exit(2);
    }
    if (write(told, "o", 1) != 1) {
        _exit(1);
    }
    w = open(FIFO_PATH, O_WRONLY);
    _exit(w >= 0 && write(w, "child", 5) == 5 ? 0 : 1);
}

/* The SIGALRMs delivered; an alarm ends a poll that nothing else did. */
static volatile sig_atomic_t alarms;

/** Count a SIGALRM. */
static void on_alarm(int sig)
{
    (void)sig;
    alarms++;
}

/**
 * Wait to see `parent` asleep (`seen_asleep`), open the FIFO for reading and
 * read a byte of it, and keep running, reading the nonblocking `ack` in a
 * loop, until it has a byte or 15 s have passed: an idle kernel scans a
 * sleeping poll's descriptors again at every interrupt, which would end the
 * parent's wait without the read's wake. Runs in the forked child and does
 * not return: exits 0 when all of that happened, 2 when the parent was never
 * seen asleep, and 1 otherwise.
 */
static void drainer(pid_t parent, int ack)
{
    struct timespec start, now;
    char byte;
    int r;

    if (!seen_asleep(parent)) {
        _exit(2);
    }
    r = open(FIFO_PATH, O_RDONLY | O_NONBLOCK);
    if (r < 0 || read(r, &byte, 1) != 1 || clock_gettime(CLOCK_MONOTONIC, &start) != 0) {
        _exit(1);
    }
    do {
        if (read(ack, &byte, 1) == 1) {
            _exit(0);
        }
        clock_gettime(CLOCK_MONOTONIC, &now);
    } while (now.tv_sec - start.tv_sec < 15);
    _exit(1);
}

/**
 * Check that an open of the FIFO for reading and writing does not wait, and
 * that once full it polls writable when another open reads a byte. The
 * reader is a child that reads once it sees the poll asleep and keeps running
 * after, and the poll has no time limit of its own, only a 10 s alarm; a poll
 * the alarm ends finds the room as it wakes, so the row fails when the alarm
 * fired. Then check read-only opens with `reader_hangup`. Returns 0, or 1
 * once the failure is reported.
 */
static int poll_opens(void)
{
    struct sigaction sa;
    struct pollfd pfd;
    char buf[512];
    int rw, n, status = 0;
    int ack[2];
    pid_t parent = getpid();
    pid_t pid;

    rw = open(FIFO_PATH, O_RDWR);
    if (rw < 0 || write(rw, "rw", 2) != 2 || read(rw, buf, sizeof(buf)) != 2) {
        return fail("an open for reading and writing");
    }
    close(rw);
    rw = open(FIFO_PATH, O_RDWR | O_NONBLOCK);
    if (rw < 0) {
        return fail("an open for reading and writing to fill");
    }
    memset(buf, 'f', sizeof(buf));
    while (write(rw, buf, sizeof(buf)) > 0) {
    }
    pfd.fd = rw;
    pfd.events = POLLOUT;
    pfd.revents = 0;
    if (errno != EAGAIN || poll(&pfd, 1, 0) != 0) {
        return fail("a full FIFO polled writable");
    }
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_alarm;
    if (pipe(ack) != 0 || fcntl(ack[0], F_SETFL, O_NONBLOCK) != 0 ||
        sigaction(SIGALRM, &sa, NULL) != 0) {
        return fail("setup for the reading child");
    }
    pid = fork();
    if (pid == 0) {
        drainer(parent, ack[0]);
    }
    alarms = 0;
    alarm(10);
    n = poll(&pfd, 1, -1);
    alarm(0);
    if (pid < 0 || write(ack[1], "a", 1) != 1 || waitpid(pid, &status, 0) != pid ||
        !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        return fail(pid > 0 && WIFEXITED(status) && WEXITSTATUS(status) == 2
                        ? "the parent was never seen asleep in poll"
                        : "the reading child");
    }
    if (n != 1 || !(pfd.revents & POLLOUT) || alarms) {
        return fail("a read by another open did not wake a poll for output");
    }
    close(ack[0]);
    close(ack[1]);
    close(rw);
    return reader_hangup();
}

/**
 * Check a FIFO of the KRFFS root through a bind of its directory: a writer
 * through the bind joins a reader through the source. Then ends opened
 * through the bind, each on a FIFO unlinked through the source, keep the
 * FIFOs' nodes until their close, which frees them: statfs counts the space
 * taken while the ends are open and the same space back after. Returns 0, or
 * 1 once the failure is reported.
 */
static int aliased(void)
{
    struct statfs before, held, after;
    char src[64], dst[64], buf[4];
    int ends[ALIASES];
    int r, w, i;

    if (mkdir(BIND_SRC, 0755) != 0 || mkdir(BIND_DST, 0755) != 0 ||
        mount(BIND_SRC, BIND_DST, NULL, MS_BIND, NULL) != 0) {
        return fail("a bind of a directory of the KRFFS root");
    }
    if (mkfifo(BIND_SRC "/f", 0600) != 0) {
        return fail("a FIFO under the bind's source");
    }
    r = open(BIND_SRC "/f", O_RDONLY | O_NONBLOCK);
    w = r < 0 ? -1 : open(BIND_DST "/f", O_WRONLY | O_NONBLOCK);
    if (w < 0 || write(w, "bd", 2) != 2 || read(r, buf, sizeof(buf)) != 2) {
        return fail("a writer through a bind did not join a reader through its source");
    }
    close(w);
    close(r);
    if (unlink(BIND_SRC "/f") != 0 || statfs("/tmp", &before) != 0) {
        return fail("statfs before the FIFOs");
    }
    for (i = 0; i < ALIASES; i++) {
        snprintf(src, sizeof(src), BIND_SRC "/o%d", i);
        snprintf(dst, sizeof(dst), BIND_DST "/o%d", i);
        if (mkfifo(src, 0600) != 0 || (ends[i] = open(dst, O_RDWR)) < 0 || unlink(src) != 0) {
            return fail("an end through a bind of a FIFO unlinked through its source");
        }
    }
    if (statfs("/tmp", &held) != 0 || held.f_bfree + 8 > before.f_bfree) {
        return fail("the FIFOs held through a bind took no space statfs counts");
    }
    for (i = 0; i < ALIASES; i++) {
        close(ends[i]);
    }
    if (statfs("/tmp", &after) != 0 || after.f_bfree + 2 < before.f_bfree) {
        return fail("the last close through a bind did not free unlinked FIFOs");
    }
    if (umount2(BIND_DST, 0) != 0 || rmdir(BIND_DST) != 0 || rmdir(BIND_SRC) != 0) {
        return fail("the bind unmounts and its directories go");
    }
    return 0;
}

/**
 * Check that a FIFO has a pipe of its own: apart from another whose name has
 * the same hash, across a rename, past an unlink, and through a bind
 * (`aliased`). Returns 0, or 1 once the failure is reported.
 */
static int identity(void)
{
    char buf[4];
    int r, w;

    unlink(TWIN_A);
    unlink(TWIN_B);
    unlink(MOVED);
    if (mkfifo(TWIN_A, 0600) != 0 || mkfifo(TWIN_B, 0600) != 0) {
        return fail("two FIFOs whose names have one hash");
    }
    r = open(TWIN_A, O_RDONLY | O_NONBLOCK);
    if (r < 0) {
        return fail("a reader of the first");
    }
    if (open(TWIN_B, O_WRONLY | O_NONBLOCK) != -1 || errno != ENXIO) {
        return fail("the second found the first's reader");
    }
    if (rename(TWIN_A, MOVED) != 0) {
        return fail("the rename");
    }
    w = open(MOVED, O_WRONLY | O_NONBLOCK);
    if (w < 0 || write(w, "mv", 2) != 2 || read(r, buf, sizeof(buf)) != 2) {
        return fail("the renamed FIFO lost its reader");
    }
    close(w);
    if (unlink(MOVED) != 0 || mkfifo(MOVED, 0600) != 0) {
        return fail("a FIFO in an unlinked one's name");
    }
    if (open(MOVED, O_WRONLY | O_NONBLOCK) != -1 || errno != ENXIO) {
        return fail("a new FIFO found the unlinked one's reader");
    }
    close(r);
    unlink(MOVED);
    unlink(TWIN_B);
    return aliased();
}

/**
 * Check that a blocking open of the FIFO for reading waits for a writer. The
 * child `writer` opens the FIFO only once it has seen this process asleep,
 * and first writes `o` to a pipe, which must be there when the open returns;
 * the child's bytes then arrive, and the end of input after its exit.
 * Returns 0, or 1 once the failure is reported.
 */
static int blocking_open(void)
{
    char buf[16];
    int r, status;
    int told[2];
    pid_t parent = getpid();
    pid_t pid;

    if (pipe(told) != 0 || fcntl(told[0], F_SETFL, O_NONBLOCK) != 0) {
        return fail("the pipe the child writes to");
    }
    pid = fork();
    if (pid == 0) {
        writer(parent, told[1]);
    }
    buf[0] = '\0';
    r = open(FIFO_PATH, O_RDONLY);
    if (r < 0 || read(told[0], buf, 1) != 1 || buf[0] != 'o') {
        kill(pid, SIGKILL);
        waitpid(pid, &status, 0);
        return fail(buf[0] == 'n' ? "the blocking open for reading was never seen asleep"
                                  : "a blocking open for reading returned before a writer opened");
    }
    if (read(r, buf, sizeof(buf)) != 5 || memcmp(buf, "child", 5) != 0) {
        return fail("the child's bytes");
    }
    if (waitpid(pid, &status, 0) != pid || !WIFEXITED(status) || WEXITSTATUS(status) != 0 ||
        read(r, buf, sizeof(buf)) != 0) {
        return fail("the writing child");
    }
    close(r);
    close(told[0]);
    close(told[1]);
    return 0;
}

int main(void)
{
    struct stat st;
    char buf[16];
    int r, w;

    unlink(FIFO_PATH);
    unlink(NODE_PATH);
    if (mkfifo(FIFO_PATH, 0600) != 0 || stat(FIFO_PATH, &st) != 0 || !S_ISFIFO(st.st_mode)) {
        return fail("mkfifo");
    }
    if (mknodat(AT_FDCWD, NODE_PATH, S_IFIFO | 0600, 0) != 0 || stat(NODE_PATH, &st) != 0 ||
        !S_ISFIFO(st.st_mode)) {
        return fail("mknodat S_IFIFO");
    }
    unlink(NODE_PATH);

    if (open(FIFO_PATH, O_WRONLY | O_NONBLOCK) != -1 || errno != ENXIO) {
        return fail("a writer with no reader was not ENXIO");
    }
    r = open(FIFO_PATH, O_RDONLY | O_NONBLOCK);
    if (r < 0 || read(r, buf, sizeof(buf)) != 0) {
        return fail("a nonblocking reader with no writer");
    }
    w = open(FIFO_PATH, O_WRONLY | O_NONBLOCK);
    if (w < 0 || write(w, "fifo", 4) != 4 || read(r, buf, sizeof(buf)) != 4 ||
        memcmp(buf, "fifo", 4) != 0) {
        return fail("bytes through the FIFO");
    }
    close(w);
    if (read(r, buf, sizeof(buf)) != 0) {
        return fail("no end of input after the writer closed");
    }
    close(r);

    if (blocking_open() != 0 || poll_opens() != 0) {
        unlink(FIFO_PATH);
        return 1;
    }
    unlink(FIFO_PATH);
    if (identity() != 0) {
        return 1;
    }
    printf("FIFOMUSL OK\n");
    return 0;
}
