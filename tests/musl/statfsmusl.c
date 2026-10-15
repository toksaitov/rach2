/**
 * statfsmusl: check that fstatfs names the file system of every kind of
 * descriptor, and the one statfs names for the path it was opened by.
 *
 * A descriptor opened by a path, a file, a FIFO, a directory of /proc, /dev
 * or /sys, a pty slave or an O_PATH descriptor on /dev/log, must report the
 * file system statfs reports for that path, or for /dev in the case of the
 * link, which statfs follows. A pipe, a socket, a namespace file, a message
 * queue, an eventfd and an epoll set have no path, and must report the file
 * system Linux keeps them on: pipefs, sockfs, nsfs, mqueue and the
 * anonymous inode file system.
 *
 * Prints `STATFSMUSL OK` and exits 0 on success, and prints
 * `STATFSMUSL FAIL (detail)` and exits 1 otherwise.
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <mqueue.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <unistd.h>

/* The FIFO and the message queue the rows make and remove. */
#define FIFO_PATH  "/tmp/statfsmusl.fifo"
#define QUEUE_NAME "/statfsmusl"

/* Linux's magic numbers (man/man2/statfs.2). */
#define PROC_MAGIC   0x9fa0L
#define DEVFS_MAGIC  0x1373L
#define SYSFS_MAGIC  0x62656572L
#define PIPEFS_MAGIC 0x50495045L
#define SOCKFS_MAGIC 0x534f434bL
#define NSFS_MAGIC   0x6e736673L
#define MQUEUE_MAGIC 0x19800202L
#define ANON_MAGIC   0x09041934L

/** Report one failed check with errno and return 1, the failure status. */
static int fail(const char *what)
{
    printf("STATFSMUSL FAIL (%s: errno %d)\n", what, errno);
    return 1;
}

/** The magic fstatfs reports for `fd`, or -1. */
static long fd_magic(int fd)
{
    struct statfs sf;

    return fstatfs(fd, &sf) == 0 ? (long)sf.f_type : -1L;
}

/** The magic statfs reports for `path`, or -1. */
static long path_magic(const char *path)
{
    struct statfs sf;

    return statfs(path, &sf) == 0 ? (long)sf.f_type : -1L;
}

/** Whether fstatfs of `fd`, which open gave for `path` with `flags`, and
 * statfs of `path` report the same magic, and that one is `magic`, or any
 * magic when `magic` is 0. The descriptor is closed. */
static int agrees(const char *path, int flags, long magic)
{
    int fd = open(path, flags);
    long got = fd < 0 ? -1L : fd_magic(fd);

    if (fd >= 0) {
        close(fd);
    }
    return got != -1L && got == path_magic(path) && (magic == 0 || got == magic);
}

/**
 * Check the descriptors opened by a path: a file of the root, a FIFO, the
 * directories of /proc, /dev and /sys, a pty master and slave, and /dev/log
 * itself. Returns 0, or 1 once the failure is reported.
 */
static int path_rows(void)
{
    char *slave;
    int master, fd;
    long got;

    if (!agrees("/", O_RDONLY | O_DIRECTORY, 0)) {
        return fail("the root");
    }
    unlink(FIFO_PATH);
    if (mkfifo(FIFO_PATH, 0600) != 0) {
        return fail("make the FIFO");
    }
    if (!agrees(FIFO_PATH, O_RDONLY | O_NONBLOCK, 0) ||
        path_magic(FIFO_PATH) != path_magic("/tmp")) {
        unlink(FIFO_PATH);
        return fail("a FIFO");
    }
    unlink(FIFO_PATH);
    if (!agrees("/proc", O_RDONLY | O_DIRECTORY, PROC_MAGIC)) {
        return fail("/proc");
    }
    if (!agrees("/proc/self", O_RDONLY | O_DIRECTORY, PROC_MAGIC)) {
        return fail("/proc/self");
    }
    if (!agrees("/proc/self/fd", O_RDONLY | O_DIRECTORY, PROC_MAGIC)) {
        return fail("/proc/self/fd");
    }
    if (!agrees("/dev", O_RDONLY | O_DIRECTORY, DEVFS_MAGIC)) {
        return fail("/dev");
    }
    if (!agrees("/dev/pts", O_RDONLY | O_DIRECTORY, DEVFS_MAGIC)) {
        return fail("/dev/pts");
    }
    if (!agrees("/sys", O_RDONLY | O_DIRECTORY, SYSFS_MAGIC)) {
        return fail("/sys");
    }
    master = posix_openpt(O_RDWR | O_NOCTTY);
    if (master < 0 || grantpt(master) != 0 || unlockpt(master) != 0 ||
        (slave = ptsname(master)) == NULL) {
        return fail("open a pty");
    }
    got = fd_magic(master);
    if (got != DEVFS_MAGIC || !agrees(slave, O_RDWR | O_NOCTTY, DEVFS_MAGIC)) {
        close(master);
        return fail("a pty master and slave");
    }
    close(master);
    fd = open("/dev/log", O_PATH | O_NOFOLLOW);
    got = fd < 0 ? -1L : fd_magic(fd);
    if (fd >= 0) {
        close(fd);
    }
    if (got != DEVFS_MAGIC) {
        return fail("the link /dev/log");
    }
    return 0;
}

/**
 * Check the descriptors that have no path: the two ends of a pipe, an
 * Internet and an AF_UNIX socket, a namespace file, a message queue, an
 * eventfd and an epoll set. Returns 0, or 1 once the failure is reported.
 */
static int pathless_rows(void)
{
    int p[2], sv[2], fd, ok;
    mqd_t q;
    long got, mount;

    if (pipe(p) != 0) {
        return fail("make a pipe");
    }
    ok = fd_magic(p[0]) == PIPEFS_MAGIC && fd_magic(p[1]) == PIPEFS_MAGIC;
    close(p[0]);
    close(p[1]);
    if (!ok) {
        return fail("a pipe");
    }
    fd = socket(AF_INET, SOCK_DGRAM, 0);
    got = fd < 0 ? -1L : fd_magic(fd);
    if (fd >= 0) {
        close(fd);
    }
    if (got != SOCKFS_MAGIC) {
        return fail("an Internet socket");
    }
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) {
        return fail("make an AF_UNIX pair");
    }
    ok = fd_magic(sv[0]) == SOCKFS_MAGIC && fd_magic(sv[1]) == SOCKFS_MAGIC;
    close(sv[0]);
    close(sv[1]);
    if (!ok) {
        return fail("an AF_UNIX socket");
    }
    fd = open("/proc/self/ns/net", O_RDONLY);
    got = fd < 0 ? -1L : fd_magic(fd);
    if (fd >= 0) {
        close(fd);
    }
    if (got != NSFS_MAGIC) {
        return fail("a namespace file");
    }
    mq_unlink(QUEUE_NAME);
    q = mq_open(QUEUE_NAME, O_CREAT | O_EXCL | O_RDWR, 0600, NULL);
    if (q == (mqd_t)-1) {
        return fail("make a message queue");
    }
    got = fd_magic((int)q);
    mount = path_magic("/dev/mqueue");
    mq_close(q);
    mq_unlink(QUEUE_NAME);
    if (got != MQUEUE_MAGIC || mount != MQUEUE_MAGIC) {
        return fail("a message queue");
    }
    fd = eventfd(0, 0);
    got = fd < 0 ? -1L : fd_magic(fd);
    if (fd >= 0) {
        close(fd);
    }
    if (got != ANON_MAGIC) {
        return fail("an eventfd");
    }
    fd = epoll_create1(0);
    got = fd < 0 ? -1L : fd_magic(fd);
    if (fd >= 0) {
        close(fd);
    }
    if (got != ANON_MAGIC) {
        return fail("an epoll set");
    }
    return 0;
}

int main(void)
{
    if (path_rows() != 0 || pathless_rows() != 0) {
        return 1;
    }
    printf("STATFSMUSL OK\n");
    return 0;
}
