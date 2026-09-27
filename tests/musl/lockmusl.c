/**
 * Advisory locks between processes.
 *
 * A child's write lock over bytes 10 to 29 makes the parent's lock over
 * byte 25 fail with EAGAIN, while a lock beside it succeeds; F_GETLK reports
 * the child's pid and range. The child's unlock of bytes 15 to 19 splits its
 * lock in two, and its close of another descriptor of the file drops what is
 * left. A child's locks of the largest offset and of every byte before it
 * join into one F_GETLK reports with a length of 0. F_SETLKW waits until the
 * holder unlocks: the child that asks is seen asleep, with nothing taken,
 * before the parent unlocks. Two processes that each wait for the other's
 * lock, the parent asking only once the child is seen asleep in its wait: one
 * of the two waits fails with EDEADLK. A thread's F_SETLKW whose descriptor
 * another thread closes during the wait fails with EBADF when the holder lets
 * go, and leaves the byte free. A flock
 * LOCK_EX on one open makes LOCK_EX|LOCK_NB on another fail with
 * EWOULDBLOCK, a dup shares the lock, and the last close of a description
 * releases it. An open file description lock conflicts with a second open
 * in the same process, and with that process's POSIX lock.
 *
 * Prints "LOCKMUSL OK" on success and "LOCKMUSL FAIL (detail)" otherwise.
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/file.h>
#include <sys/wait.h>
#include <unistd.h>

#define LOCK_PATH "/tmp/lockmusl"

static int fail(const char *what)
{
    printf("LOCKMUSL FAIL (%s: errno %d)\n", what, errno);
    return 1;
}

static int lk(int fd, int cmd, short type, off_t start, off_t len)
{
    struct flock fl;

    memset(&fl, 0, sizeof(fl));
    fl.l_type = type;
    fl.l_whence = SEEK_SET;
    fl.l_start = start;
    fl.l_len = len;
    return fcntl(fd, cmd, &fl);
}

static void post(int fd)
{
    (void)write(fd, "x", 1);
}

static void await(int fd)
{
    char c;

    (void)read(fd, &c, 1);
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

/** Whether process `pid` is seen asleep within 3 s. */
static int seen_asleep(pid_t pid)
{
    int tries;

    for (tries = 0; tries < 300; tries++) {
        if (sleeping(pid)) {
            return 1;
        }
        usleep(10000);
    }
    return 0;
}

static int records(int fd)
{
    int to_child[2], to_parent[2];
    struct flock q;
    int status;
    pid_t pid;

    if (pipe(to_child) != 0 || pipe(to_parent) != 0) {
        return fail("pipes");
    }
    pid = fork();
    if (pid == 0) {
        int other;

        if (lk(fd, F_SETLK, F_WRLCK, 10, 20) != 0) {
            _exit(1);
        }
        post(to_parent[1]);
        await(to_child[0]);
        if (lk(fd, F_SETLK, F_UNLCK, 15, 5) != 0) {
            _exit(2);
        }
        post(to_parent[1]);
        await(to_child[0]);
        other = open(LOCK_PATH, O_RDONLY);
        close(other);
        post(to_parent[1]);
        await(to_child[0]);
        _exit(0);
    }
    await(to_parent[0]);
    if (lk(fd, F_SETLK, F_WRLCK, 0, 10) != 0) {
        return fail("a lock beside the child's");
    }
    if (lk(fd, F_SETLK, F_RDLCK, 25, 1) != -1 || (errno != EAGAIN && errno != EACCES)) {
        return fail("a lock over the child's did not fail");
    }
    memset(&q, 0, sizeof(q));
    q.l_type = F_WRLCK;
    q.l_whence = SEEK_SET;
    q.l_start = 12;
    q.l_len = 1;
    if (fcntl(fd, F_GETLK, &q) != 0 || q.l_type != F_WRLCK || q.l_pid != pid || q.l_start != 10 ||
        q.l_len != 20) {
        return fail("F_GETLK did not report the child's lock");
    }
    post(to_child[1]);
    await(to_parent[0]);
    if (lk(fd, F_SETLK, F_WRLCK, 15, 5) != 0 || lk(fd, F_SETLK, F_UNLCK, 15, 5) != 0) {
        return fail("the middle the child unlocked");
    }
    if (lk(fd, F_SETLK, F_WRLCK, 12, 1) != -1) {
        return fail("the child's unlock of the middle took the rest");
    }
    post(to_child[1]);
    await(to_parent[0]);
    if (lk(fd, F_SETLK, F_WRLCK, 10, 20) != 0) {
        return fail("the child's close of another descriptor left its locks");
    }
    post(to_child[1]);
    if (waitpid(pid, &status, 0) != pid || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        return fail("the locking child");
    }
    lk(fd, F_SETLK, F_UNLCK, 0, 0);
    close(to_child[0]);
    close(to_child[1]);
    close(to_parent[0]);
    close(to_parent[1]);
    return 0;
}

static int waits(int fd)
{
    struct pollfd pfd;
    int done[2], ready[2];
    int status, parent_dead, child_dead;
    char c;
    pid_t pid;

    if (pipe(done) != 0 || pipe(ready) != 0 || lk(fd, F_SETLK, F_WRLCK, 0, 1) != 0) {
        return fail("setup for F_SETLKW");
    }
    pid = fork();
    if (pid == 0) {
        if (lk(fd, F_SETLKW, F_WRLCK, 0, 1) != 0) {
            _exit(1);
        }
        post(done[1]);
        _exit(0);
    }
    pfd.fd = done[0];
    pfd.events = POLLIN;
    if (!seen_asleep(pid) || poll(&pfd, 1, 0) != 0) {
        return fail("F_SETLKW did not wait");
    }
    lk(fd, F_SETLK, F_UNLCK, 0, 1);
    if (read(done[0], &c, 1) != 1 || waitpid(pid, &status, 0) != pid || !WIFEXITED(status) ||
        WEXITSTATUS(status) != 0) {
        return fail("F_SETLKW did not get the lock after the unlock");
    }

    if (lk(fd, F_SETLK, F_WRLCK, 0, 1) != 0) {
        return fail("setup for EDEADLK");
    }
    pid = fork();
    if (pid == 0) {
        int dead;

        if (lk(fd, F_SETLK, F_WRLCK, 1, 1) != 0) {
            _exit(3);
        }
        post(ready[1]);
        dead = lk(fd, F_SETLKW, F_WRLCK, 0, 1) != 0 && errno == EDEADLK;
        _exit(dead ? 1 : 0);
    }
    await(ready[0]);
    if (!seen_asleep(pid)) {
        return fail("the child's F_SETLKW did not wait for the parent's lock");
    }
    parent_dead = lk(fd, F_SETLKW, F_WRLCK, 1, 1) != 0 && errno == EDEADLK;
    if (parent_dead) {
        lk(fd, F_SETLK, F_UNLCK, 0, 1);
    }
    if (waitpid(pid, &status, 0) != pid || !WIFEXITED(status) || WEXITSTATUS(status) > 1) {
        return fail("the child of the deadlock");
    }
    child_dead = WEXITSTATUS(status) == 1;
    if (parent_dead + child_dead != 1) {
        return fail("not one EDEADLK for the two waits");
    }
    lk(fd, F_SETLK, F_UNLCK, 0, 0);
    close(done[0]);
    close(done[1]);
    close(ready[0]);
    close(ready[1]);
    return 0;
}

/**
 * Locks that reach the largest offset: a child's lock of the last byte alone
 * and its lock of every byte before it join into one lock of the whole file,
 * which F_GETLK reports from byte 0 with a length of 0.
 */
static int largest(int fd)
{
    const off_t max = 0x7FFFFFFFFFFFFFFFLL;
    int to_child[2], to_parent[2];
    struct flock q;
    int status;
    pid_t pid;

    if (pipe(to_child) != 0 || pipe(to_parent) != 0) {
        return fail("pipes for the largest offset");
    }
    pid = fork();
    if (pid == 0) {
        if (lk(fd, F_SETLK, F_WRLCK, max, 1) != 0 || lk(fd, F_SETLK, F_WRLCK, 0, max) != 0) {
            _exit(1);
        }
        post(to_parent[1]);
        await(to_child[0]);
        _exit(0);
    }
    await(to_parent[0]);
    memset(&q, 0, sizeof(q));
    q.l_type = F_RDLCK;
    q.l_whence = SEEK_SET;
    q.l_start = 5;
    q.l_len = 1;
    if (fcntl(fd, F_GETLK, &q) != 0 || q.l_type != F_WRLCK || q.l_start != 0 || q.l_len != 0) {
        return fail("the locks that reach the largest offset are not one to the end");
    }
    post(to_child[1]);
    if (waitpid(pid, &status, 0) != pid || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        return fail("the child of the largest offset");
    }
    close(to_child[0]);
    close(to_child[1]);
    close(to_parent[0]);
    close(to_parent[1]);
    return 0;
}

static int waiter_fd, waiter_rc, waiter_errno;

/** Wait for a write lock of byte 50 through `waiter_fd`. */
static void *wait_for_byte(void *arg)
{
    (void)arg;
    waiter_rc = lk(waiter_fd, F_SETLKW, F_WRLCK, 50, 1);
    waiter_errno = errno;
    return NULL;
}

/** Whether a child can take a write lock of byte 50. */
static int byte_free(void)
{
    int status;
    pid_t pid = fork();

    if (pid == 0) {
        int fd = open(LOCK_PATH, O_RDWR);

        _exit(fd >= 0 && lk(fd, F_SETLK, F_WRLCK, 50, 1) == 0 ? 0 : 1);
    }
    return waitpid(pid, &status, 0) == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

/**
 * A thread waits in F_SETLKW for a byte a child holds, and the main thread
 * closes the descriptor it waits through. When the child lets go, the wait
 * gains the lock, lets it go for the close, and fails with EBADF, and a
 * second child can take the byte.
 */
static int closed_wait(void)
{
    int to_child[2], to_parent[2];
    pthread_t waiter;
    int status;
    pid_t pid;

    if (pipe(to_child) != 0 || pipe(to_parent) != 0) {
        return fail("pipes for the closed wait");
    }
    pid = fork();
    if (pid == 0) {
        int fd = open(LOCK_PATH, O_RDWR);

        if (fd < 0 || lk(fd, F_SETLK, F_WRLCK, 50, 1) != 0) {
            _exit(1);
        }
        post(to_parent[1]);
        await(to_child[0]);
        _exit(0);
    }
    await(to_parent[0]);
    waiter_fd = open(LOCK_PATH, O_RDWR);
    if (waiter_fd < 0 || pthread_create(&waiter, NULL, wait_for_byte, NULL) != 0) {
        return fail("the waiting thread");
    }
    (void)poll(NULL, 0, 200);
    close(waiter_fd);
    post(to_child[1]);
    if (pthread_join(waiter, NULL) != 0 || waitpid(pid, &status, 0) != pid || !WIFEXITED(status) ||
        WEXITSTATUS(status) != 0) {
        return fail("the waiting thread and the holding child");
    }
    if (waiter_rc != -1 || waiter_errno != EBADF) {
        errno = waiter_errno;
        return fail("a wait whose descriptor was closed did not fail with EBADF");
    }
    if (!byte_free()) {
        return fail("the lock of a closed wait outlived the close");
    }
    close(to_child[0]);
    close(to_child[1]);
    close(to_parent[0]);
    close(to_parent[1]);
    return 0;
}

static int flocks(void)
{
    int a = open(LOCK_PATH, O_RDWR);
    int b = open(LOCK_PATH, O_RDWR);
    int d;
    struct flock fl;

    if (a < 0 || b < 0 || flock(a, LOCK_EX) != 0) {
        return fail("flock LOCK_EX");
    }
    if (flock(b, LOCK_EX | LOCK_NB) != -1 || errno != EWOULDBLOCK) {
        return fail("a second open's LOCK_EX was not EWOULDBLOCK");
    }
    d = dup(a);
    if (flock(d, LOCK_UN) != 0 || flock(b, LOCK_EX | LOCK_NB) != 0) {
        return fail("the dup did not share the lock");
    }
    if (flock(a, LOCK_SH | LOCK_NB) != -1 || errno != EWOULDBLOCK) {
        return fail("LOCK_SH beside another's LOCK_EX");
    }
    close(b);
    if (flock(a, LOCK_SH | LOCK_NB) != 0) {
        return fail("the close did not release the flock lock");
    }
    close(a);
    close(d);

    a = open(LOCK_PATH, O_RDWR);
    b = open(LOCK_PATH, O_RDWR);
    memset(&fl, 0, sizeof(fl));
    fl.l_type = F_WRLCK;
    fl.l_whence = SEEK_SET;
    fl.l_len = 10;
    if (fcntl(a, F_OFD_SETLK, &fl) != 0) {
        return fail("F_OFD_SETLK");
    }
    if (fcntl(b, F_OFD_SETLK, &fl) != -1 || errno != EAGAIN) {
        return fail("a second open's OFD lock did not fail");
    }
    if (lk(b, F_SETLK, F_WRLCK, 0, 10) != -1) {
        return fail("a POSIX lock over an OFD lock");
    }
    close(a);
    if (fcntl(b, F_OFD_SETLK, &fl) != 0) {
        return fail("the close did not release the OFD lock");
    }
    close(b);
    return 0;
}

int main(void)
{
    char block[100];
    int fd;

    memset(block, 'l', sizeof(block));
    fd = open(LOCK_PATH, O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (fd < 0 || write(fd, block, sizeof(block)) != (ssize_t)sizeof(block)) {
        return fail("the file");
    }
    if (records(fd) || largest(fd) || waits(fd) || closed_wait() || flocks()) {
        return 1;
    }
    close(fd);
    unlink(LOCK_PATH);
    printf("LOCKMUSL OK\n");
    return 0;
}
