/**
 * Named pipes.
 *
 * mkfifo and mknod with S_IFIFO make files stat reports as FIFOs. With no
 * reader, a nonblocking open for writing fails with ENXIO; a nonblocking
 * open for reading succeeds and reads the end of input. Bytes written at one
 * open are read at the other, and the reader sees the end of input when the
 * writer closes. A blocking open for reading waits for a child's open for
 * writing, and the child's bytes arrive. An open for reading and writing
 * does not wait.
 *
 * Prints "FIFOMUSL OK" on success and "FIFOMUSL FAIL (detail)" otherwise.
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#define FIFO_PATH "/tmp/fifomusl"
#define NODE_PATH "/tmp/fifomusl.node"

static int fail(const char *what)
{
    printf("FIFOMUSL FAIL (%s: errno %d)\n", what, errno);
    return 1;
}

int main(void)
{
    struct stat st;
    char buf[16];
    int r, w, status;
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

    pid = fork();
    if (pid == 0) {
        w = open(FIFO_PATH, O_WRONLY);
        _exit(w >= 0 && write(w, "child", 5) == 5 ? 0 : 1);
    }
    r = open(FIFO_PATH, O_RDONLY);
    if (r < 0 || read(r, buf, sizeof(buf)) != 5 || memcmp(buf, "child", 5) != 0) {
        return fail("the blocking opens");
    }
    if (waitpid(pid, &status, 0) != pid || !WIFEXITED(status) || WEXITSTATUS(status) != 0 ||
        read(r, buf, sizeof(buf)) != 0) {
        return fail("the writing child");
    }
    close(r);

    r = open(FIFO_PATH, O_RDWR);
    if (r < 0 || write(r, "rw", 2) != 2 || read(r, buf, sizeof(buf)) != 2) {
        return fail("an open for reading and writing");
    }
    close(r);
    unlink(FIFO_PATH);
    printf("FIFOMUSL OK\n");
    return 0;
}
