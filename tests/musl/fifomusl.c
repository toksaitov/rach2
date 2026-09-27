/**
 * Named pipes.
 *
 * mkfifo and mknod with S_IFIFO make files stat reports as FIFOs. With no
 * reader, a nonblocking open for writing fails with ENXIO; a nonblocking
 * open for reading succeeds and reads the end of input. Bytes written at one
 * open are read at the other, and the reader sees the end of input when the
 * writer closes. A blocking open for reading waits for a writer: a child
 * opens the FIFO for writing only once it has seen the parent asleep, and
 * first writes a byte to a pipe, which must be there when the parent's open
 * returns. The child's bytes then arrive. An open for reading and writing
 * does not wait, and once full it polls writable when a child's open reads a
 * byte.
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

/* A FIFO open for reading and writing does not wait, and once full polls
 * writable when another open reads a byte; the reader is a child, so the
 * poll must be woken. */
static int rdwr_opens(void)
{
    struct timespec pause = {0, 100000000};
    struct pollfd pfd;
    char buf[512];
    int rw, status;
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
    pid = fork();
    if (pid == 0) {
        int r;

        nanosleep(&pause, NULL);
        r = open(FIFO_PATH, O_RDONLY | O_NONBLOCK);
        _exit(r >= 0 && read(r, buf, 1) == 1 ? 0 : 1);
    }
    if (poll(&pfd, 1, 5000) != 1 || !(pfd.revents & POLLOUT)) {
        return fail("a read by another open did not wake a poll for output");
    }
    if (waitpid(pid, &status, 0) != pid || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        return fail("the reading child");
    }
    close(rw);
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
    printf("FIFOMUSL OK\n");
    return 0;
}
