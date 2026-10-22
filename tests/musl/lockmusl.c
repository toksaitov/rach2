/**
 * lockmusl: check advisory locks between processes.
 *
 * A child's write lock over bytes 10 to 29 makes the parent's lock over
 * byte 25 fail with EAGAIN, while a lock beside it succeeds; F_GETLK reports
 * the child's pid and range. The child's unlock of bytes 15 to 19 splits its
 * lock in two: the parent's lock on either side fails, and F_GETLK reports
 * the child's pid with bytes 10 to 14 and with bytes 20 to 29. The child's
 * close of another descriptor of the file drops what is left. A child's locks
 * of the largest offset and of every byte before it join into one, which
 * F_GETLK reports with a length of 0. F_SETLKW waits until the holder
 * unlocks: the child that asks is seen asleep, with nothing taken, before the
 * parent unlocks. Two processes each wait for the other's lock, the parent
 * asking only once the child is seen asleep in its wait: one of the two waits
 * fails with EDEADLK and the other takes the lock, and neither fails in any
 * other way. A thread's F_SETLKW whose descriptor another thread closes
 * during the wait fails with EBADF when the holder lets go, and leaves the
 * byte free, and so does one whose file a write moves during the wait. A
 * process that clone makes with CLONE_FILES shares its parent's descriptor
 * table, which owns the locks: F_GETLK in another process reports which of
 * the two took each lock, a close by the child drops the parent's lock too,
 * and the child's exit leaves the table's locks. A lock is on the file: a
 * child's lock on a file leaves free another file whose name hashes alike,
 * and a lock taken before a rename, or before a write that moves the file on
 * the KRFFS root, blocks a child's lock through an open after it. A flock
 * LOCK_EX on one open makes LOCK_EX|LOCK_NB on another fail with
 * EWOULDBLOCK, and a dup shares the lock. A description's lock stays
 * through the close of one of its two descriptors, as another open's
 * LOCK_EX|LOCK_NB finds, and goes at the last close. An open file
 * description lock conflicts with a second open in the same process, and
 * with that process's POSIX lock. /proc/locks lists a flock lock, a POSIX
 * read lock and an open file description's lock in Linux's columns, with
 * the file's device and inode number as stat gives them, and none of them
 * once the descriptors are closed.
 *
 * Prints `LOCKMUSL OK` and exits 0 on success, and prints
 * `LOCKMUSL FAIL (detail)` and exits 1 otherwise. Writes etude's report
 * records (../probe_report.h) for the case ETUDE_CASE names, an assertion for
 * each check `checks` lists; the first failure leaves the checks after it not
 * run, those of the rows `which_file` goes on to among them.
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
#include <sys/sysmacros.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#include "../probe_report.h"

#define LOCK_PATH "/tmp/lockmusl"
/* The directory of the rows about which file a lock is on. */
#define KEY_DIR   "/tmp/lockmusl.d"

/* The checks, one assertion each, in the order the program makes them, and
 * the text it prints for each that fails. */
enum {
    THE_FILE,
    KEY_DIR_MADE,
    PIPES,
    BESIDE,
    OVER_EAGAIN,
    GETLK_CHILD,
    SPLIT_SIDES,
    SPLIT_GETLK,
    CLOSE_DROPS,
    LOCKING_CHILD,
    LARGEST_PIPES,
    LARGEST_JOIN,
    LARGEST_CHILD,
    SETLKW_SETUP,
    SETLKW_WAITS,
    SETLKW_GETS,
    DEADLK_SETUP,
    DEADLK_CHILD_WAITS,
    DEADLK_CHILD,
    DEADLK_PARENT,
    DEADLK_ONE,
    CLOSED_PIPES,
    WAITING_THREAD,
    THREAD_ASLEEP,
    THREAD_AND_HOLDER,
    CLOSED_EBADF,
    CLOSED_FREE,
    SHARED_SETUP,
    CLONE_SHARED,
    SHARED_GETLK,
    SHARED_CLOSE,
    SHARED_RELOCK,
    SHARER_EXIT,
    SHARED_KEPT,
    HASH_FILES,
    HASH_FREE,
    HASH_CHILD,
    RENAMED_FILE,
    RENAMED_HELD,
    MOVED_FILE,
    MOVED_HELD,
    MOVED_WAIT_FILE,
    MOVED_WAIT_THREAD,
    MOVED_WAIT_ASLEEP,
    MOVED_WAIT_GROW,
    MOVED_WAIT_END,
    MOVED_WAIT_EBADF,
    MOVED_WAIT_FREE,
    FLOCK_EX,
    FLOCK_NB,
    FLOCK_DUP,
    FLOCK_SH,
    FLOCK_CLOSE,
    FLOCK_ONE_CLOSE,
    FLOCK_LAST_CLOSE,
    OFD_SETLK,
    OFD_SECOND,
    OFD_POSIX,
    OFD_CLOSE,
    PROC_LOCKS_READ,
    PROC_LOCKS_COLUMNS,
    PROC_LOCKS_GONE,
    CHECKS
};

static const struct {
    const char *id;
    const char *text;
} checks[CHECKS] = {
    {"file", "the file"},
    {"key-dir", "the directory of the rows about which file a lock is on"},
    {"pipes", "pipes"},
    {"beside", "a lock beside the child's"},
    {"over-eagain", "a lock over the child's did not fail"},
    {"getlk-child", "F_GETLK did not report the child's lock"},
    {"split-sides", "the middle the child unlocked"},
    {"split-getlk", "the child's unlock of the middle did not leave bytes 10 to 14 and 20 to 29"},
    {"close-drops", "the child's close of another descriptor left its locks"},
    {"locking-child", "the locking child"},
    {"largest-pipes", "pipes for the largest offset"},
    {"largest-join", "the locks that reach the largest offset are not one to the end"},
    {"largest-child", "the child of the largest offset"},
    {"setlkw-setup", "setup for F_SETLKW"},
    {"setlkw-waits", "F_SETLKW did not wait"},
    {"setlkw-gets", "F_SETLKW did not get the lock after the unlock"},
    {"deadlk-setup", "setup for EDEADLK"},
    {"deadlk-child-waits", "the child's F_SETLKW did not wait for the parent's lock"},
    {"deadlk-child", "the child of the deadlock took neither the lock nor EDEADLK"},
    {"deadlk-parent", "the parent of the deadlock took neither the lock nor EDEADLK"},
    {"deadlk-one", "not one lock and one EDEADLK for the two waits"},
    {"closed-pipes", "pipes for the closed wait"},
    {"waiting-thread", "the waiting thread"},
    {"thread-asleep", "the waiting thread was never seen asleep in F_SETLKW"},
    {"thread-and-holder", "the waiting thread and the holding child"},
    {"closed-ebadf", "a wait whose descriptor was closed did not fail with EBADF"},
    {"closed-free", "the lock of a closed wait outlived the close"},
    {"shared-setup", "setup for the shared table"},
    {"clone-files", "clone with CLONE_FILES"},
    {"shared-getlk", "F_GETLK did not name the process of a shared table that took each lock"},
    {"shared-close", "a close by a process sharing the table left the table's locks"},
    {"shared-relock", "a lock after the close in the shared table"},
    {"sharer-exit", "the process that shares the table"},
    {"shared-kept", "the exit of a process sharing the table dropped the table's locks"},
    {"hash-files", "two files whose names hash alike"},
    {"hash-free", "a lock on one file refused a lock on another whose name hashes alike"},
    {"hash-child", "the child that locked the file whose name hashes alike"},
    {"renamed-file", "a locked file renamed"},
    {"renamed-held", "a lock through the new name did not meet one taken before the rename"},
    {"moved-file", "a locked file grown past its node"},
    {"moved-held", "a lock through an open after a move did not meet one taken before it"},
    {"moved-wait-file", "a file for the closed wait across a move"},
    {"moved-wait-thread", "the waiting thread across a move"},
    {"moved-wait-asleep", "the waiting thread across a move was never seen asleep in F_SETLKW"},
    {"moved-wait-grow", "the file of the wait grown past its node"},
    {"moved-wait-end", "the waiting thread and the holding child across a move"},
    {"moved-wait-ebadf", "a closed wait across a move did not fail with EBADF"},
    {"moved-wait-free", "the lock of a closed wait whose file moved outlived the close"},
    {"flock-ex", "flock LOCK_EX"},
    {"flock-nb", "a second open's LOCK_EX was not EWOULDBLOCK"},
    {"flock-dup", "the dup did not share the lock"},
    {"flock-sh", "LOCK_SH beside another's LOCK_EX"},
    {"flock-close", "the close did not release the flock lock"},
    {"flock-one-close", "the close of one of two descriptors released the flock lock"},
    {"flock-last-close", "the last close of a description did not release the flock lock"},
    {"ofd-setlk", "F_OFD_SETLK"},
    {"ofd-second", "a second open's OFD lock did not fail"},
    {"ofd-posix", "a POSIX lock over an OFD lock"},
    {"ofd-close", "the close did not release the OFD lock"},
    {"proc-locks-read", "the locks /proc/locks lists"},
    {"proc-locks-columns", "/proc/locks lacks a flock, a POSIX or an OFD lock in Linux's columns"},
    {"proc-locks-gone", "/proc/locks lists a lock after its descriptors are closed"},
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
    printf("LOCKMUSL FAIL (%s: errno %d)\n", checks[check].text, errno);
    report_at(check);

    return 1;
}

/**
 * Make the fcntl record-lock command `cmd` on `fd` with a lock of `type` over
 * the `len` bytes from `start`, a `len` of 0 reaching the end of the file.
 * Returns fcntl's result.
 */
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

/** Write a byte to the pipe `fd`, a signal to the process at the other end. */
static void post(int fd)
{
    (void)write(fd, "x", 1);
}

/** Read a byte from the pipe `fd`: wait for the other end's `post`. */
static void await(int fd)
{
    char c;

    (void)read(fd, &c, 1);
}

/**
 * Close, in a child that waits on the pipe `from_parent`, the end the parent
 * writes, so that the parent's exit ends every wait of the child's: a probe
 * that stops at a failed row then leaves no child behind holding its locks
 * for a later run to meet.
 */
static void leave_parent_end(int from_parent[2])
{
    close(from_parent[1]);
}

/** The state letter the stat file at `path` gives, or 0 when it cannot be
 * read. */
static int stat_state(const char *path)
{
    char line[256], *state;
    int fd, n;

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
 * Wait for the process `*id`, or with `thread` set the thread `*id` of this
 * process, to be asleep, state S in its stat file, looking every 10 ms. A
 * thread's `*id` may still be 0, for a thread that has yet to store it.
 * Returns 1 once it is asleep, and 0 when it ends first, as a zombie or gone
 * from /proc.
 */
static int asleep(const volatile pid_t *id, int thread)
{
    char path[64];
    int state;

    for (;;) {
        state = -1;
        if (*id != 0) {
            snprintf(path, sizeof(path), thread ? "/proc/self/task/%d/stat" : "/proc/%d/stat",
                     (int)*id);
            state = stat_state(path);
        }
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

/** Whether process `pid` is seen asleep (`asleep`). */
static int seen_asleep(pid_t pid)
{
    return asleep(&pid, 0);
}

/**
 * Check a child's lock of bytes 10 to 29 of `fd`, its split by an unlock of
 * the middle, and its release by a close of another descriptor of the file.
 * Returns 0, or 1 once the failure is reported.
 */
static int records(int fd)
{
    int to_child[2], to_parent[2];
    struct flock q;
    int status;
    pid_t pid;

    if (pipe(to_child) != 0 || pipe(to_parent) != 0) {
        return fail(PIPES);
    }
    pid = fork();
    if (pid == 0) {
        int other;

        leave_parent_end(to_child);
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
        return fail(BESIDE);
    }
    if (lk(fd, F_SETLK, F_RDLCK, 25, 1) != -1 || (errno != EAGAIN && errno != EACCES)) {
        return fail(OVER_EAGAIN);
    }
    memset(&q, 0, sizeof(q));
    q.l_type = F_WRLCK;
    q.l_whence = SEEK_SET;
    q.l_start = 12;
    q.l_len = 1;
    if (fcntl(fd, F_GETLK, &q) != 0 || q.l_type != F_WRLCK || q.l_pid != pid || q.l_start != 10 ||
        q.l_len != 20) {
        return fail(GETLK_CHILD);
    }
    post(to_child[1]);
    await(to_parent[0]);
    if (lk(fd, F_SETLK, F_WRLCK, 15, 5) != 0 || lk(fd, F_SETLK, F_UNLCK, 15, 5) != 0) {
        return fail(SPLIT_SIDES);
    }
    if (!held_by(fd, pid, 12, 10, 5) || !held_by(fd, pid, 25, 20, 10)) {
        return fail(SPLIT_GETLK);
    }
    post(to_child[1]);
    await(to_parent[0]);
    if (lk(fd, F_SETLK, F_WRLCK, 10, 20) != 0) {
        return fail(CLOSE_DROPS);
    }
    post(to_child[1]);
    if (waitpid(pid, &status, 0) != pid || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        return fail(LOCKING_CHILD);
    }
    lk(fd, F_SETLK, F_UNLCK, 0, 0);
    close(to_child[0]);
    close(to_child[1]);
    close(to_parent[0]);
    close(to_parent[1]);
    return 0;
}

/**
 * Check that F_SETLKW on `fd` waits until the holder unlocks, and that of two
 * processes that wait for each other's lock one takes it and the other's
 * wait fails with EDEADLK. Returns 0, or 1 once the failure is reported.
 */
static int waits(int fd)
{
    struct pollfd pfd;
    int done[2], ready[2];
    int status, rc, err;
    char c;
    pid_t pid;

    if (pipe(done) != 0 || pipe(ready) != 0 || lk(fd, F_SETLK, F_WRLCK, 0, 1) != 0) {
        return fail(SETLKW_SETUP);
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
        return fail(SETLKW_WAITS);
    }
    lk(fd, F_SETLK, F_UNLCK, 0, 1);
    if (read(done[0], &c, 1) != 1 || waitpid(pid, &status, 0) != pid || !WIFEXITED(status) ||
        WEXITSTATUS(status) != 0) {
        return fail(SETLKW_GETS);
    }

    if (lk(fd, F_SETLK, F_WRLCK, 0, 1) != 0) {
        return fail(DEADLK_SETUP);
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
        return fail(DEADLK_CHILD_WAITS);
    }
    rc = lk(fd, F_SETLKW, F_WRLCK, 1, 1);
    err = errno;
    lk(fd, F_SETLK, F_UNLCK, 0, 1);
    if (waitpid(pid, &status, 0) != pid || !WIFEXITED(status) || WEXITSTATUS(status) > 1) {
        return fail(DEADLK_CHILD);
    }
    errno = err;
    if (rc != 0 && err != EDEADLK) {
        return fail(DEADLK_PARENT);
    }
    if ((rc == 0) == (WEXITSTATUS(status) == 0)) {
        return fail(DEADLK_ONE);
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
 * which F_GETLK reports from byte 0 with a length of 0. Returns 0, or 1 once
 * the failure is reported.
 */
static int largest(int fd)
{
    const off_t max = 0x7FFFFFFFFFFFFFFFLL;
    int to_child[2], to_parent[2];
    struct flock q;
    int status;
    pid_t pid;

    if (pipe(to_child) != 0 || pipe(to_parent) != 0) {
        return fail(LARGEST_PIPES);
    }
    pid = fork();
    if (pid == 0) {
        leave_parent_end(to_child);
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
        return fail(LARGEST_JOIN);
    }
    post(to_child[1]);
    if (waitpid(pid, &status, 0) != pid || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        return fail(LARGEST_CHILD);
    }
    close(to_child[0]);
    close(to_child[1]);
    close(to_parent[0]);
    close(to_parent[1]);
    return 0;
}

/* The waiting thread's descriptor, its F_SETLKW's result and errno, and its
 * thread id, 0 until the thread has stored it. */
static int waiter_fd, waiter_rc, waiter_errno;
static volatile pid_t waiter_tid;

/**
 * Store this thread's id in `waiter_tid`, and wait for a write lock of byte
 * 50 through `waiter_fd`, with the result left in `waiter_rc` and
 * `waiter_errno`.
 */
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
    return pid > 0 && waitpid(pid, &status, 0) == pid && WIFEXITED(status) &&
           WEXITSTATUS(status) == 0;
}

/** Whether a child can take a write lock of byte 50. */
static int byte_free(void)
{
    return byte_free_at(LOCK_PATH, 50);
}

/**
 * Whether a child's write lock of byte `at` of the file at `path`, through
 * an open of its own, fails because another owner holds the byte: the child
 * must exit normally, having seen EAGAIN or EACCES, so a child that could
 * not open the file, or that died, is no proof that the byte is held.
 */
static int byte_held_at(const char *path, off_t at)
{
    int status;
    pid_t pid = fork();

    if (pid == 0) {
        int fd = open(path, O_RDWR);
        int refused = fd >= 0 && lk(fd, F_SETLK, F_WRLCK, at, 1) == -1 &&
                      (errno == EAGAIN || errno == EACCES);

        _exit(refused ? 0 : 1);
    }
    return pid > 0 && waitpid(pid, &status, 0) == pid && WIFEXITED(status) &&
           WEXITSTATUS(status) == 0;
}

/**
 * A thread waits in F_SETLKW for a byte a child holds, and the main thread,
 * once it sees the thread asleep, closes the descriptor it waits through.
 * When the child lets go, the wait gains the lock, lets it go for the close,
 * and fails with EBADF, and a second child can take the byte. Returns 0, or 1
 * once the failure is reported.
 */
static int closed_wait(void)
{
    int to_child[2], to_parent[2];
    pthread_t waiter;
    int status;
    pid_t pid;

    if (pipe(to_child) != 0 || pipe(to_parent) != 0) {
        return fail(CLOSED_PIPES);
    }
    pid = fork();
    if (pid == 0) {
        int fd = open(LOCK_PATH, O_RDWR);

        leave_parent_end(to_child);
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
        return fail(WAITING_THREAD);
    }
    if (!asleep(&waiter_tid, 1)) {
        return fail(THREAD_ASLEEP);
    }
    close(waiter_fd);
    post(to_child[1]);
    if (pthread_join(waiter, NULL) != 0 || waitpid(pid, &status, 0) != pid || !WIFEXITED(status) ||
        WEXITSTATUS(status) != 0) {
        return fail(THREAD_AND_HOLDER);
    }
    if (waiter_rc != -1 || waiter_errno != EBADF) {
        errno = waiter_errno;
        return fail(CLOSED_EBADF);
    }
    if (!byte_free()) {
        return fail(CLOSED_FREE);
    }
    close(to_child[0]);
    close(to_child[1]);
    close(to_parent[0]);
    close(to_parent[1]);
    return 0;
}

/* The stack of the process of `shared_table`, and the pipes to and from it. */
static char sharer_stack[16384] __attribute__((aligned(16)));
static int to_sharer[2], from_sharer[2];

/**
 * The process of `shared_table`, which shares its parent's descriptor table:
 * it locks byte 72 of the file whose descriptor `arg` points to, and when
 * the parent says so closes a descriptor of the file it opens for that; then
 * it exits when the parent says so. Returns its exit status: 0, 1 when the
 * lock failed, or 2 when the open failed.
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

/** Fail the check `check`, and let the process of `shared_table` run to its
 * end. */
static int sharer_fail(int check)
{
    int rc = fail(check);

    post(to_sharer[1]);
    post(to_sharer[1]);
    return rc;
}

/** The process F_GETLK in a child reports as the holder of byte `at` of the
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
 * process reports the process that took each. Its close of a descriptor of
 * the file drops both, and its exit, while the parent holds byte 74, leaves
 * that lock with the table. Returns 0, or 1 once the failure is reported.
 */
static int shared_table(int fd)
{
    int status;
    pid_t pid;

    if (pipe(to_sharer) != 0 || pipe(from_sharer) != 0 || lk(fd, F_SETLK, F_WRLCK, 70, 1) != 0) {
        return fail(SHARED_SETUP);
    }
    pid = clone(sharer, sharer_stack + sizeof(sharer_stack), CLONE_FILES | SIGCHLD, &fd);
    if (pid < 0) {
        return fail(CLONE_SHARED);
    }
    await(from_sharer[0]);
    if (holder_of(70) != getpid() || holder_of(72) != pid) {
        return sharer_fail(SHARED_GETLK);
    }
    post(to_sharer[1]);
    await(from_sharer[0]);
    if (!byte_free_at(LOCK_PATH, 70) || !byte_free_at(LOCK_PATH, 72)) {
        return sharer_fail(SHARED_CLOSE);
    }
    if (lk(fd, F_SETLK, F_WRLCK, 74, 1) != 0) {
        return sharer_fail(SHARED_RELOCK);
    }
    post(to_sharer[1]);
    if (waitpid(pid, &status, 0) != pid || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        return fail(SHARER_EXIT);
    }
    if (!byte_held_at(LOCK_PATH, 74)) {
        return fail(SHARED_KEPT);
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
 * lock on byte 0 of one must leave byte 0 of the other free. Returns 0, or 1
 * once the failure is reported.
 *
 * The child tells the parent it has tried its lock before it judges the
 * attempt, and the parent lets it go and reaps it before it judges its own
 * lock, so a failed row leaves no child behind holding byte 0 of `bC` while
 * the probe goes on past the row.
 */
static int same_hash(void)
{
    int to_child[2], to_parent[2];
    int one, two, status, refused, err, reaped;
    pid_t pid;

    one = open(KEY_DIR "/bC", O_RDWR | O_CREAT | O_TRUNC, 0644);
    two = open(KEY_DIR "/cb", O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (one < 0 || two < 0 || pipe(to_child) != 0 || pipe(to_parent) != 0) {
        return fail(HASH_FILES);
    }
    pid = fork();
    if (pid == 0) {
        int rc;

        leave_parent_end(to_child);
        rc = lk(one, F_SETLK, F_WRLCK, 0, 1);
        post(to_parent[1]);
        await(to_child[0]);
        _exit(rc == 0 ? 0 : 1);
    }
    if (pid > 0) {
        await(to_parent[0]);
    }
    refused = lk(two, F_SETLK, F_WRLCK, 0, 1) != 0;
    err = errno;
    post(to_child[1]);
    reaped =
        pid > 0 && waitpid(pid, &status, 0) == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0;
    close(one);
    close(two);
    close(to_child[0]);
    close(to_child[1]);
    close(to_parent[0]);
    close(to_parent[1]);
    unlink(KEY_DIR "/bC");
    unlink(KEY_DIR "/cb");
    if (refused) {
        errno = err;
        return fail(HASH_FREE);
    }
    if (!reaped) {
        return fail(HASH_CHILD);
    }
    return 0;
}

/** Whether a child's write lock of byte 0 of the file at `path`, through an
 * open of its own, fails because another process holds the byte. */
static int byte_held(const char *path)
{
    return byte_held_at(path, 0);
}

/**
 * A lock is on the file whatever its name: the parent's write lock on byte
 * 0, taken before a rename, must block a child's lock of the byte through
 * an open of the new name. Returns 0, or 1 once the failure is reported.
 */
static int renamed(void)
{
    int fd = open(KEY_DIR "/old", O_RDWR | O_CREAT | O_TRUNC, 0644);

    if (fd < 0 || lk(fd, F_SETLK, F_WRLCK, 0, 1) != 0 ||
        rename(KEY_DIR "/old", KEY_DIR "/new") != 0) {
        return fail(RENAMED_FILE);
    }
    if (!byte_held(KEY_DIR "/new")) {
        return fail(RENAMED_HELD);
    }
    close(fd);
    unlink(KEY_DIR "/new");
    return 0;
}

/**
 * A lock goes where its file goes. Two files made one after the other lie
 * side by side on the KRFFS root, so a write that grows the first past its
 * node moves it; the parent's write lock on byte 0, taken before that
 * write, must block a child's lock through an open made after it. Returns 0,
 * or 1 once the failure is reported.
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
        return fail(MOVED_FILE);
    }
    if (!byte_held(KEY_DIR "/moved")) {
        return fail(MOVED_HELD);
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
 * second child can take the byte through an open of the file. Returns 0, or 1
 * once the failure is reported.
 */
static int closed_wait_moved(void)
{
    int to_child[2], to_parent[2];
    char block[1024];
    pthread_t waiter;
    int status, made, beside, grow;
    pid_t pid;

    made = open(KEY_DIR "/waited", O_RDWR | O_CREAT | O_TRUNC, 0644);
    beside = open(KEY_DIR "/after", O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (made < 0 || beside < 0 || pipe(to_child) != 0 || pipe(to_parent) != 0) {
        return fail(MOVED_WAIT_FILE);
    }
    close(made);
    pid = fork();
    if (pid == 0) {
        int fd = open(KEY_DIR "/waited", O_RDWR);

        leave_parent_end(to_child);
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
        return fail(MOVED_WAIT_THREAD);
    }
    if (!asleep(&waiter_tid, 1)) {
        return fail(MOVED_WAIT_ASLEEP);
    }
    close(waiter_fd);
    memset(block, 'w', sizeof(block));
    grow = open(KEY_DIR "/waited", O_RDWR);
    if (grow < 0 || write(grow, block, sizeof(block)) != (ssize_t)sizeof(block)) {
        return fail(MOVED_WAIT_GROW);
    }
    close(grow);
    post(to_child[1]);
    if (pthread_join(waiter, NULL) != 0 || waitpid(pid, &status, 0) != pid || !WIFEXITED(status) ||
        WEXITSTATUS(status) != 0) {
        return fail(MOVED_WAIT_END);
    }
    if (waiter_rc != -1 || waiter_errno != EBADF) {
        errno = waiter_errno;
        return fail(MOVED_WAIT_EBADF);
    }
    if (!byte_free_at(KEY_DIR "/waited", 50)) {
        return fail(MOVED_WAIT_FREE);
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
 * never meets the holder's unlock and would sleep for good. Returns 0, or 1
 * when a row failed.
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

/**
 * Check flock between two opens, through a dup and across closes, and open
 * file description locks against a second open and a POSIX lock. Returns 0,
 * or 1 once the failure is reported.
 */
static int flocks(void)
{
    int a = open(LOCK_PATH, O_RDWR);
    int b = open(LOCK_PATH, O_RDWR);
    int d;
    struct flock fl;

    if (a < 0 || b < 0 || flock(a, LOCK_EX) != 0) {
        return fail(FLOCK_EX);
    }
    if (flock(b, LOCK_EX | LOCK_NB) != -1 || errno != EWOULDBLOCK) {
        return fail(FLOCK_NB);
    }
    d = dup(a);
    if (flock(d, LOCK_UN) != 0 || flock(b, LOCK_EX | LOCK_NB) != 0) {
        return fail(FLOCK_DUP);
    }
    if (flock(a, LOCK_SH | LOCK_NB) != -1 || errno != EWOULDBLOCK) {
        return fail(FLOCK_SH);
    }
    close(b);
    if (flock(a, LOCK_SH | LOCK_NB) != 0) {
        return fail(FLOCK_CLOSE);
    }
    b = open(LOCK_PATH, O_RDWR);
    close(a);
    if (b < 0 || flock(b, LOCK_EX | LOCK_NB) != -1 || errno != EWOULDBLOCK) {
        return fail(FLOCK_ONE_CLOSE);
    }
    close(d);
    if (flock(b, LOCK_EX | LOCK_NB) != 0) {
        return fail(FLOCK_LAST_CLOSE);
    }
    close(b);

    a = open(LOCK_PATH, O_RDWR);
    b = open(LOCK_PATH, O_RDWR);
    memset(&fl, 0, sizeof(fl));
    fl.l_type = F_WRLCK;
    fl.l_whence = SEEK_SET;
    fl.l_len = 10;
    if (fcntl(a, F_OFD_SETLK, &fl) != 0) {
        return fail(OFD_SETLK);
    }
    if (fcntl(b, F_OFD_SETLK, &fl) != -1 || errno != EAGAIN) {
        return fail(OFD_SECOND);
    }
    if (lk(b, F_SETLK, F_WRLCK, 0, 10) != -1) {
        return fail(OFD_POSIX);
    }
    close(a);
    if (fcntl(b, F_OFD_SETLK, &fl) != 0) {
        return fail(OFD_CLOSE);
    }
    close(b);
    return 0;
}

/**
 * Whether `text`, /proc/locks, has the line of a lock that begins with `head`,
 * was taken by `pid` on the file of `st` and covers `range`.
 */
static int listed(const char *text, const char *head, long pid, const struct stat *st,
                  const char *range)
{
    char want[128];

    snprintf(want, sizeof(want), "%s%ld %02x:%02x:%llu %s\n", head, pid, major(st->st_dev),
             minor(st->st_dev), (unsigned long long)st->st_ino, range);
    return strstr(text, want) != NULL;
}

/**
 * Read /proc/locks into `buf` of `cap` bytes, terminated. Returns the bytes
 * read, or -1.
 */
static int read_locks(char *buf, int cap)
{
    int fd = open("/proc/locks", O_RDONLY);
    int n = fd < 0 ? -1 : (int)read(fd, buf, (size_t)cap - 1);

    if (fd >= 0) {
        close(fd);
    }
    if (n >= 0) {
        buf[n] = '\0';
    }
    return n;
}

/**
 * Check that /proc/locks lists a flock, a POSIX and an open file description
 * lock in Linux's columns, and none of them after the close. Returns 0, or 1
 * once the failure is reported.
 */
static int proc_locks(void)
{
    static char text[8192];
    char ino[32];
    int a = open(LOCK_PATH, O_RDWR);
    int b = open(LOCK_PATH, O_RDWR);
    int c = open(LOCK_PATH, O_RDWR);
    struct flock fl;
    struct stat st;

    memset(&fl, 0, sizeof(fl));
    fl.l_type = F_WRLCK;
    fl.l_whence = SEEK_SET;
    fl.l_start = 20;
    if (a < 0 || b < 0 || c < 0 || fstat(a, &st) != 0 || flock(a, LOCK_EX) != 0 ||
        lk(b, F_SETLK, F_RDLCK, 5, 10) != 0 || fcntl(c, F_OFD_SETLK, &fl) != 0) {
        return fail(PROC_LOCKS_READ);
    }
    if (read_locks(text, (int)sizeof(text)) < 0 ||
        !listed(text, "FLOCK  ADVISORY  WRITE ", (long)getpid(), &st, "0 EOF") ||
        !listed(text, "POSIX  ADVISORY  READ  ", (long)getpid(), &st, "5 14") ||
        !listed(text, "OFDLCK ADVISORY  WRITE ", -1L, &st, "20 EOF")) {
        return fail(PROC_LOCKS_COLUMNS);
    }
    close(a);
    close(b);
    close(c);
    snprintf(ino, sizeof(ino), ":%llu ", (unsigned long long)st.st_ino);
    if (read_locks(text, (int)sizeof(text)) < 0 || strstr(text, ino)) {
        return fail(PROC_LOCKS_GONE);
    }
    return 0;
}

int main(void)
{
    char block[100];
    int fd;

    report_begin(report_case(environ, "guest.default.lockmusl"));
    memset(block, 'l', sizeof(block));
    fd = open(LOCK_PATH, O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (fd < 0 || write(fd, block, sizeof(block)) != (ssize_t)sizeof(block)) {
        return fail(THE_FILE);
    }
    if (mkdir(KEY_DIR, 0755) != 0 && errno != EEXIST) {
        return fail(KEY_DIR_MADE);
    }
    if (records(fd) || largest(fd) || waits(fd) || closed_wait() || shared_table(fd) ||
        which_file() || flocks() || proc_locks()) {
        return 1;
    }
    close(fd);
    unlink(LOCK_PATH);
    rmdir(KEY_DIR);
    report_at(CHECKS);
    printf("LOCKMUSL OK\n");
    return 0;
}
