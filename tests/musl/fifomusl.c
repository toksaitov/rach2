/**
 * Named pipes.
 *
 * mkfifo and mknod with S_IFIFO make files stat reports as FIFOs. With no
 * reader, a nonblocking open for writing fails with ENXIO; a nonblocking
 * open for reading succeeds and reads the end of input. Bytes written at one
 * open are read at the other, and the reader sees the end of input when the
 * writer closes. A blocking open for reading waits for a writer: a child opens
 * the FIFO for writing only once it has seen the parent asleep, and first
 * writes a byte to a pipe, which must be there when the parent's open returns.
 * The child's bytes then arrive. An open for reading and writing does not wait,
 * and once full it polls writable when a child's open reads a byte. Two FIFOs
 * whose names have one hash, which the KRFFS root numbers a file by, are two
 * pipes; a renamed FIFO keeps its pipe; and one unlinked while an end is open
 * keeps its pipe for that end, apart from a FIFO made in its name.
 *
 * Prints "FIFOMUSL OK" on success and "FIFOMUSL FAIL (detail)" otherwise.
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define FIFO_PATH "/tmp/fifomusl"
#define NODE_PATH "/tmp/fifomusl.node"
/* Two names with one hash, the DJB hash the KRFFS root numbers files by. */
#define TWIN_A    "/tmp/fifomusl.a3"
#define TWIN_B    "/tmp/fifomusl.bP"
#define MOVED     "/tmp/fifomusl.moved"

static int fail(const char *what)
{
    printf("FIFOMUSL FAIL (%s: errno %d)\n", what, errno);
    return 1;
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

/** Wait up to 3 s to see `parent` asleep, write a byte to `told`, then open
 * the FIFO for writing and write to it: exits 0 when all of that happened,
 * 2 when the parent was never seen asleep. */
static void writer(pid_t parent, int told)
{
    int tries;
    int w;

    for (tries = 0; tries < 300 && !sleeping(parent); tries++) {
        usleep(10000);
    }
    if (tries == 300) {
        _exit(2);
    }
    if (write(told, "o", 1) != 1) {
        _exit(1);
    }
    w = open(FIFO_PATH, O_WRONLY);
    _exit(w >= 0 && write(w, "child", 5) == 5 ? 0 : 1);
}

static volatile sig_atomic_t alarms;

/** Count a SIGALRM. */
static void on_alarm(int sig)
{
    (void)sig;
    alarms++;
}

/** Wait up to 3 s to see `parent` asleep, open the FIFO for reading and
 * read a byte of it, and keep running, reading the nonblocking `ack` in a
 * loop, until it has a byte or 15 s have passed: an idle kernel scans a
 * sleeping poll's descriptors again at every interrupt, which would end the
 * parent's wait without the read's wake. Exits 0 when all of that happened,
 * 2 when the parent was never seen asleep, and 1 otherwise. */
static void drainer(pid_t parent, int ack)
{
    struct timespec start, now;
    char byte;
    int tries, r;

    for (tries = 0; tries < 300 && !sleeping(parent); tries++) {
        usleep(10000);
    }
    if (tries == 300) {
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

/* A FIFO open for reading and writing does not wait, and once full polls
 * writable when another open reads a byte. The reader is a child that reads
 * once it sees the poll asleep and keeps running after, and the poll has no
 * time limit of its own, only a 10 s alarm; a poll the alarm ends finds the
 * room as it wakes, so the row fails when the alarm fired. */
static int rdwr_opens(void)
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
    return 0;
}

/** A FIFO is its own pipe, by name hash, across a rename and past an
 * unlink. */
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
    return 0;
}

int main(void)
{
    struct stat st;
    char buf[16];
    int r, w, status;
    int told[2];
    pid_t parent = getpid();
    pid_t pid;

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

    if (pipe(told) != 0 || fcntl(told[0], F_SETFL, O_NONBLOCK) != 0) {
        return fail("the pipe the child writes to");
    }
    pid = fork();
    if (pid == 0) {
        writer(parent, told[1]);
    }
    r = open(FIFO_PATH, O_RDONLY);
    if (r < 0 || read(told[0], buf, 1) != 1) {
        kill(pid, SIGKILL);
        waitpid(pid, &status, 0);
        return fail("a blocking open for reading returned before a writer opened");
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

    if (rdwr_opens() != 0) {
        return 1;
    }
    unlink(FIFO_PATH);
    if (identity() != 0) {
        return 1;
    }
    printf("FIFOMUSL OK\n");
    return 0;
}
