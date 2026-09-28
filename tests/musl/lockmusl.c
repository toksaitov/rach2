/**
 * Advisory locks between processes.
 *
 * A child's write lock over bytes 10 to 29 makes the parent's lock over
 * byte 25 fail with EAGAIN, while a lock beside it succeeds; F_GETLK reports
 * the child's pid and range. The child's unlock of bytes 15 to 19 splits its
 * lock in two: the parent's lock on either side fails, and F_GETLK reports
 * the child's pid with bytes 10 to 14 and with bytes 20 to 29. The child's
 * close of another descriptor of the file drops what is left. A child's locks
 * of the largest offset and of every byte before it join into one F_GETLK
 * reports with a length of 0. F_SETLKW waits until the holder unlocks: the
 * child that asks is seen asleep, with nothing taken, before the parent
 * unlocks. Two processes that each wait for the other's lock, the parent
 * asking only once the child is seen asleep in its wait: one of the two waits
 * fails with EDEADLK and the other takes the lock, and neither fails in any
 * other way. A thread's F_SETLKW whose descriptor another thread closes
 * during the wait fails with EBADF when the holder lets go, and leaves the
 * byte free, and so does one whose file a write moves during the wait. A
 * process that clone makes with CLONE_FILES shares its parent's descriptor
 * table, which owns the locks: F_GETLK in another process names the one of
 * the two that took each lock, a close by the child drops the parent's lock
 * too, and the child's exit leaves the table's locks. A lock is on the file:
 * a child's lock on a file leaves free another file whose name hashes
 * alike, and a lock taken before a rename, or before a write that moves the
 * file on the KRFFS root, refuses a child's lock through an open after it.
 * A flock LOCK_EX on one open makes LOCK_EX|LOCK_NB on another fail with
 * EWOULDBLOCK, and a dup shares the lock. A description's lock stays
 * through the close of one of its two descriptors, as another open's
 * LOCK_EX|LOCK_NB finds, and goes at the last close. An open file
 * description lock conflicts with a second open in the same process, and
 * with that process's POSIX lock.
 *
 * Prints "LOCKMUSL OK" on success and "LOCKMUSL FAIL (detail)" otherwise.
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#define LOCK_PATH "/tmp/lockmusl"
/* The directory of the rows about which file a lock is on. */
#define KEY_DIR   "/tmp/lockmusl.d"

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

/**
 * Whether the parent's write lock on byte `at` of `fd` fails, with EAGAIN or
 * EACCES, because child `pid` holds the `len` bytes from `start`, as F_GETLK
 * reports them.
 */
static int held_by(int fd, pid_t pid, off_t at, off_t start, off_t len)
{
    struct flock q;

    if (lk(fd, F_SETLK, F_WRLCK, at, 1) != -1 || (errno != EAGAIN && errno != EACCES)) {
        return 0;
    }
    memset(&q, 0, sizeof(q));
    q.l_type = F_WRLCK;
    q.l_whence = SEEK_SET;
    q.l_start = at;
    q.l_len = 1;
    return fcntl(fd, F_GETLK, &q) == 0 && q.l_type == F_WRLCK && q.l_pid == pid &&
           q.l_start == start && q.l_len == len;
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
    if (!held_by(fd, pid, 12, 10, 5) || !held_by(fd, pid, 25, 20, 10)) {
        return fail("the child's unlock of the middle did not leave bytes 10 to 14 and 20 to 29");
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
    int status, rc, err;
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
        if (lk(fd, F_SETLK, F_WRLCK, 1, 1) != 0) {
            _exit(3);
        }
        post(ready[1]);
        if (lk(fd, F_SETLKW, F_WRLCK, 0, 1) == 0) {
            _exit(0);
        }
        _exit(errno == EDEADLK ? 1 : 2);
    }
    await(ready[0]);
    if (!seen_asleep(pid)) {
        return fail("the child's F_SETLKW did not wait for the parent's lock");
    }
    rc = lk(fd, F_SETLKW, F_WRLCK, 1, 1);
    err = errno;
    lk(fd, F_SETLK, F_UNLCK, 0, 1);
    if (waitpid(pid, &status, 0) != pid || !WIFEXITED(status) || WEXITSTATUS(status) > 1) {
        return fail("the child of the deadlock took neither the lock nor EDEADLK");
    }
    errno = err;
    if (rc != 0 && err != EDEADLK) {
        return fail("the parent of the deadlock took neither the lock nor EDEADLK");
    }
    if ((rc == 0) == (WEXITSTATUS(status) == 0)) {
        return fail("not one lock and one EDEADLK for the two waits");
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
static volatile pid_t waiter_tid;

/** Whether thread `tid` of this process sleeps, by its state in
 * /proc/self/task/<tid>/stat. */
static int thread_sleeping(pid_t tid)
{
    char path[64], line[256], *state;
    int fd, n;

    snprintf(path, sizeof(path), "/proc/self/task/%d/stat", (int)tid);
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

/** Say this thread's id in `waiter_tid`, and wait for a write lock of byte
 * 50 through `waiter_fd`. */
static void *wait_for_byte(void *arg)
{
    (void)arg;
    waiter_tid = (pid_t)syscall(SYS_gettid);
    waiter_rc = lk(waiter_fd, F_SETLKW, F_WRLCK, 50, 1);
    waiter_errno = errno;
    return NULL;
}

/** Whether a child can take a write lock of byte `at` of the file at `path`
 * through an open of its own. */
static int byte_free_at(const char *path, off_t at)
{
    int status;
    pid_t pid = fork();

    if (pid == 0) {
        int fd = open(path, O_RDWR);

        _exit(fd >= 0 && lk(fd, F_SETLK, F_WRLCK, at, 1) == 0 ? 0 : 1);
    }
    return waitpid(pid, &status, 0) == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

/** Whether a child can take a write lock of byte 50. */
static int byte_free(void)
{
    return byte_free_at(LOCK_PATH, 50);
}

/**
 * A thread waits in F_SETLKW for a byte a child holds, and the main thread,
 * once it sees the thread asleep, closes the descriptor it waits through.
 * When the child lets go, the wait gains the lock, lets it go for the close,
 * and fails with EBADF, and a second child can take the byte.
 */
static int closed_wait(void)
{
    int to_child[2], to_parent[2];
    pthread_t waiter;
    int status, tries;
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
    waiter_tid = 0;
    if (waiter_fd < 0 || pthread_create(&waiter, NULL, wait_for_byte, NULL) != 0) {
        return fail("the waiting thread");
    }
    for (tries = 0; tries < 300 && !(waiter_tid != 0 && thread_sleeping(waiter_tid)); tries++) {
        usleep(10000);
    }
    if (tries == 300) {
        return fail("the waiting thread was never seen asleep in F_SETLKW");
    }
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

static char sharer_stack[16384] __attribute__((aligned(16)));
static int to_sharer[2], from_sharer[2];

/**
 * The process of `shared_table`, which shares its parent's descriptor table:
 * it locks byte 72 of the file whose descriptor `arg` points to, and when
 * the parent says so closes a descriptor of the file it opens for that; then
 * it exits when the parent says so.
 */
static int sharer(void *arg)
{
    int rc = lk(*(int *)arg, F_SETLK, F_WRLCK, 72, 1);
    int other;

    post(from_sharer[1]);
    if (rc != 0) {
        return 1;
    }
    await(to_sharer[0]);
    other = open(LOCK_PATH, O_RDONLY);
    if (other >= 0) {
        close(other);
    }
    post(from_sharer[1]);
    await(to_sharer[0]);
    return other >= 0 ? 0 : 2;
}

/** Fail with `what`, and let the process of `shared_table` run to its end. */
static int sharer_fail(const char *what)
{
    int rc = fail(what);

    post(to_sharer[1]);
    post(to_sharer[1]);
    return rc;
}

/** The process F_GETLK in a child names as the holder of byte `at` of the
 * file, or -1 when the child finds the byte free. */
static pid_t holder_of(off_t at)
{
    int p[2], status;
    pid_t pid, holder = -1;

    if (pipe(p) != 0) {
        return -1;
    }
    pid = fork();
    if (pid == 0) {
        int fd = open(LOCK_PATH, O_RDONLY);
        struct flock q;
        pid_t got = -1;

        memset(&q, 0, sizeof(q));
        q.l_type = F_WRLCK;
        q.l_whence = SEEK_SET;
        q.l_start = at;
        q.l_len = 1;
        if (fd >= 0 && fcntl(fd, F_GETLK, &q) == 0 && q.l_type != F_UNLCK) {
            got = q.l_pid;
        }
        _exit(write(p[1], &got, sizeof(got)) == (ssize_t)sizeof(got) ? 0 : 1);
    }
    if (pid > 0) {
        if (read(p[0], &holder, sizeof(holder)) != (ssize_t)sizeof(holder)) {
            holder = -1;
        }
        waitpid(pid, &status, 0);
    }
    close(p[0]);
    close(p[1]);
    return holder;
}

/**
 * A descriptor table owns its record locks, as Linux's files_struct does: a
 * process that clone makes with CLONE_FILES and not CLONE_THREAD shares its
 * parent's table, and so its locks. It locks byte 72 and the parent byte
 * 70, which do not touch and so stay two locks, and F_GETLK in another
 * process names the process that took each. Its close of a descriptor of
 * the file drops both, and its exit, while the parent holds byte 74, leaves
 * that lock with the table.
 */
static int shared_table(int fd)
{
    int status;
    pid_t pid;

    if (pipe(to_sharer) != 0 || pipe(from_sharer) != 0 || lk(fd, F_SETLK, F_WRLCK, 70, 1) != 0) {
        return fail("setup for the shared table");
    }
    pid = clone(sharer, sharer_stack + sizeof(sharer_stack), CLONE_FILES | SIGCHLD, &fd);
    if (pid < 0) {
        return fail("clone with CLONE_FILES");
    }
    await(from_sharer[0]);
    if (holder_of(70) != getpid() || holder_of(72) != pid) {
        return sharer_fail(
            "F_GETLK did not name the process of a shared table that took each lock");
    }
    post(to_sharer[1]);
    await(from_sharer[0]);
    if (!byte_free_at(LOCK_PATH, 70) || !byte_free_at(LOCK_PATH, 72)) {
        return sharer_fail("a close by a process sharing the table left the table's locks");
    }
    if (lk(fd, F_SETLK, F_WRLCK, 74, 1) != 0) {
        return sharer_fail("a lock after the close in the shared table");
    }
    post(to_sharer[1]);
    if (waitpid(pid, &status, 0) != pid || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        return fail("the process that shares the table");
    }
    if (byte_free_at(LOCK_PATH, 74)) {
        return fail("the exit of a process sharing the table dropped the table's locks");
    }
    lk(fd, F_SETLK, F_UNLCK, 0, 0);
    close(to_sharer[0]);
    close(to_sharer[1]);
    close(from_sharer[0]);
    close(from_sharer[1]);
    return 0;
}

/**
 * Two files whose names hash alike are two files: `bC` and `cb` in one
 * directory have one inode number on the KRFFS root, and a child's write
 * lock on byte 0 of one must leave byte 0 of the other free.
 */
static int same_hash(void)
{
    int to_child[2], to_parent[2];
    int one, two, status;
    pid_t pid;

    one = open(KEY_DIR "/bC", O_RDWR | O_CREAT | O_TRUNC, 0644);
    two = open(KEY_DIR "/cb", O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (one < 0 || two < 0 || pipe(to_child) != 0 || pipe(to_parent) != 0) {
        return fail("two files whose names hash alike");
    }
    pid = fork();
    if (pid == 0) {
        if (lk(one, F_SETLK, F_WRLCK, 0, 1) != 0) {
            _exit(1);
        }
        post(to_parent[1]);
        await(to_child[0]);
        _exit(0);
    }
    await(to_parent[0]);
    if (lk(two, F_SETLK, F_WRLCK, 0, 1) != 0) {
        return fail("a lock on one file refused a lock on another whose name hashes alike");
    }
    post(to_child[1]);
    if (waitpid(pid, &status, 0) != pid || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        return fail("the child that locked the file whose name hashes alike");
    }
    close(one);
    close(two);
    close(to_child[0]);
    close(to_child[1]);
    close(to_parent[0]);
    close(to_parent[1]);
    unlink(KEY_DIR "/bC");
    unlink(KEY_DIR "/cb");
    return 0;
}

/** Whether a child's write lock of byte 0 of the file at `path`, through an
 * open of its own, fails because another process holds the byte. */
static int byte_held(const char *path)
{
    int status;
    pid_t pid = fork();

    if (pid == 0) {
        int fd = open(path, O_RDWR);
        int refused =
            fd >= 0 && lk(fd, F_SETLK, F_WRLCK, 0, 1) == -1 && (errno == EAGAIN || errno == EACCES);

        _exit(refused ? 0 : 1);
    }
    return waitpid(pid, &status, 0) == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

/**
 * A lock is on the file whatever its name: the parent's write lock on byte
 * 0, taken before a rename, must refuse a child's lock of the byte through
 * an open of the new name.
 */
static int renamed(void)
{
    int fd = open(KEY_DIR "/old", O_RDWR | O_CREAT | O_TRUNC, 0644);

    if (fd < 0 || lk(fd, F_SETLK, F_WRLCK, 0, 1) != 0 ||
        rename(KEY_DIR "/old", KEY_DIR "/new") != 0) {
        return fail("a locked file renamed");
    }
    if (!byte_held(KEY_DIR "/new")) {
        return fail("a lock through the new name did not meet one taken before the rename");
    }
    close(fd);
    unlink(KEY_DIR "/new");
    return 0;
}

/**
 * A lock goes where its file goes. Two files made one after the other lie
 * side by side on the KRFFS root, so a write that grows the first past its
 * node moves it; the parent's write lock on byte 0, taken before that
 * write, must refuse a child's lock through an open made after it.
 */
static int moved(void)
{
    char block[1024];
    int fd, beside;

    memset(block, 'm', sizeof(block));
    fd = open(KEY_DIR "/moved", O_RDWR | O_CREAT | O_TRUNC, 0644);
    beside = open(KEY_DIR "/beside", O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (fd < 0 || beside < 0 || lk(fd, F_SETLK, F_WRLCK, 0, 1) != 0 ||
        write(fd, block, sizeof(block)) != (ssize_t)sizeof(block)) {
        return fail("a locked file grown past its node");
    }
    if (!byte_held(KEY_DIR "/moved")) {
        return fail("a lock through an open after a move did not meet one taken before it");
    }
    close(fd);
    close(beside);
    unlink(KEY_DIR "/moved");
    unlink(KEY_DIR "/beside");
    return 0;
}

/**
 * closed_wait, with the file moved during the wait: a thread waits for byte
 * 50 of a file whose node another file follows, the main thread closes the
 * descriptor it waits through and grows the file past its node, and when
 * the child that held the byte lets go, the wait fails with EBADF and a
 * second child can take the byte through an open of the file.
 */
static int closed_wait_moved(void)
{
    int to_child[2], to_parent[2];
    char block[1024];
    pthread_t waiter;
    int status, tries, made, beside, grow;
    pid_t pid;

    made = open(KEY_DIR "/waited", O_RDWR | O_CREAT | O_TRUNC, 0644);
    beside = open(KEY_DIR "/after", O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (made < 0 || beside < 0 || pipe(to_child) != 0 || pipe(to_parent) != 0) {
        return fail("a file for the closed wait across a move");
    }
    close(made);
    pid = fork();
    if (pid == 0) {
        int fd = open(KEY_DIR "/waited", O_RDWR);

        if (fd < 0 || lk(fd, F_SETLK, F_WRLCK, 50, 1) != 0) {
            _exit(1);
        }
        post(to_parent[1]);
        await(to_child[0]);
        _exit(0);
    }
    await(to_parent[0]);
    waiter_fd = open(KEY_DIR "/waited", O_RDWR);
    waiter_tid = 0;
    if (waiter_fd < 0 || pthread_create(&waiter, NULL, wait_for_byte, NULL) != 0) {
        return fail("the waiting thread across a move");
    }
    for (tries = 0; tries < 300 && !(waiter_tid != 0 && thread_sleeping(waiter_tid)); tries++) {
        usleep(10000);
    }
    if (tries == 300) {
        return fail("the waiting thread across a move was never seen asleep in F_SETLKW");
    }
    close(waiter_fd);
    memset(block, 'w', sizeof(block));
    grow = open(KEY_DIR "/waited", O_RDWR);
    if (grow < 0 || write(grow, block, sizeof(block)) != (ssize_t)sizeof(block)) {
        return fail("the file of the wait grown past its node");
    }
    close(grow);
    post(to_child[1]);
    if (pthread_join(waiter, NULL) != 0 || waitpid(pid, &status, 0) != pid || !WIFEXITED(status) ||
        WEXITSTATUS(status) != 0) {
        return fail("the waiting thread and the holding child across a move");
    }
    if (waiter_rc != -1 || waiter_errno != EBADF) {
        errno = waiter_errno;
        return fail("a closed wait across a move did not fail with EBADF");
    }
    if (!byte_free_at(KEY_DIR "/waited", 50)) {
        return fail("the lock of a closed wait whose file moved outlived the close");
    }
    close(beside);
    close(to_child[0]);
    close(to_child[1]);
    close(to_parent[0]);
    close(to_parent[1]);
    unlink(KEY_DIR "/waited");
    unlink(KEY_DIR "/after");
    return 0;
}

/**
 * The rows about which file a lock is on. The first three run whatever the
 * others answer, since each says its own failure and a fault that breaks
 * two of them is seen in both. The closed wait across a move runs only once
 * a lock has been seen to move with its file: where it does not, the wait
 * never meets the holder's unlock and would sleep for good.
 */
static int which_file(void)
{
    int failed = 0;

    failed |= same_hash();
    failed |= renamed();
    if (moved() != 0) {
        return 1;
    }
    failed |= closed_wait_moved();
    return failed;
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
    b = open(LOCK_PATH, O_RDWR);
    close(a);
    if (b < 0 || flock(b, LOCK_EX | LOCK_NB) != -1 || errno != EWOULDBLOCK) {
        return fail("the close of one of two descriptors released the flock lock");
    }
    close(d);
    if (flock(b, LOCK_EX | LOCK_NB) != 0) {
        return fail("the last close of a description did not release the flock lock");
    }
    close(b);

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
    if (mkdir(KEY_DIR, 0755) != 0 && errno != EEXIST) {
        return fail("the directory of the rows about which file a lock is on");
    }
    if (records(fd) || largest(fd) || waits(fd) || closed_wait() || shared_table(fd) ||
        which_file() || flocks()) {
        return 1;
    }
    close(fd);
    unlink(LOCK_PATH);
    rmdir(KEY_DIR);
    printf("LOCKMUSL OK\n");
    return 0;
}
