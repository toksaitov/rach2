/**
 * wchanmusl: check /proc/PID/syscall and /proc/PID/wchan, a thread's call
 * and the function it sleeps in, in Linux 6.12's formats.
 *
 * The rows: a process's directory and a thread's directory under task list
 * both files, syscall, environ and auxv of mode 0400, mem of mode 0600,
 * and wchan of mode 0444. A thread blocked
 * in a read of an empty pipe reads, in its syscall file, read's number, the
 * descriptor, the buffer and the count, then its stack and instruction
 * pointers, and pipe_read from its wchan file, with no newline. A thread
 * blocked in a FUTEX_WAIT_PRIVATE reads futex's number, the word's address,
 * the operation, the value and the null timeout, and futex_wait_queue. A
 * thread that spins reads "running" and 0. A child of user 1000 gets EACCES
 * from the root parent's syscall, environ, auxv and mem files and 0 from
 * its wchan file, once the
 * parent's stat shows it asleep in its wait for the child. Against a sleeper
 * of user 1000 in a pipe's read, Linux's ptrace check by file-system ids
 * lets a caller of real and effective ids 1001 and file-system ids 1000
 * read its call and pipe_read; a sleeper that is not dumpable, whose files
 * root owns, gives a caller of every id 1000 EACCES from the open and 0 from
 * wchan; and a caller of user 1000 and group 1001 opens the file its user
 * owns and gets EPERM from the read, and 0 from wchan. A process's 0555
 * directories keep its effective IDs in either dumpable state; its 0500
 * fd directory, 0511 ns directory and files belong to root when it is not
 * dumpable, and a process of user 1000 that is not dumpable gets EACCES
 * from a read open of its own ns directories, which O_PATH opens.
 *
 * Prints `WCHANMUSL OK` and exits 0 on success, and prints `WCHANMUSL FAIL
 * (detail)` and exits 1 otherwise. etude's report (probe_report.h), for the
 * case ETUDE_CASE names, has an assertion for each row in order: `listed`,
 * `modes`, `read-call`, with the thread and the pipe, `read-wchan`,
 * `futex-call`, with its thread, `futex-wchan`, `running`, with its thread,
 * `running-wchan`, `refused-syscall`, with the fork, `refused-wchan`,
 * `directory-owners`, `fs-ids`, `not-dumpable` and `other-group`.
 * The first failure fails its assertion and leaves the rest not run.
 *
 * Needs root, to make children of users 1000 and 1001, or their NEWs when a
 * native run gives `ids=OLD:NEW,...`.
 */

#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <string.h>
#include <sys/fsuid.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#include "../probe_args.h"
#include "../probe_report.h"

extern char **environ;

/* Linux's futex operations, which musl's headers do not give. */
#define FUTEX_WAIT_PRIVATE 128
#define FUTEX_WAKE_PRIVATE 129

/* The unprivileged users and groups, 1000 and 1001 or their native NEWs. */
#define OTHER_ID ((unsigned)probe_id(1000))
#define THIRD_ID ((unsigned)probe_id(1001))

/* The call and the pointers a syscall file gives: `nr`, then `args` when
 * `nr` is not -1, then `sp` and `pc`. */
struct call {
    long nr;
    unsigned long args[6];
    unsigned long sp, pc;
};

static int pipe_fds[2];
static char read_buf[16];
static volatile int futex_word;
static volatile int spin_stop;
/* Each thread's id, set by the thread before it blocks or spins. */
static volatile pid_t reader_tid, waiter_tid, spinner_tid;

/** Report one failed check and return 1, the probe's failure status. */
static int fail(const char *what)
{
    printf("WCHANMUSL FAIL (%s)\n", what);
    report_stop(what);
    return 1;
}

/** Read the file `path` into `buf` of `cap` bytes, ended by a NUL: its
 * length, or -1 with errno set. */
static int read_file(const char *path, char *buf, int cap)
{
    int fd = open(path, O_RDONLY);
    int n;

    if (fd < 0) {
        return -1;
    }
    n = (int)read(fd, buf, (size_t)cap - 1);
    close(fd);
    if (n < 0) {
        return -1;
    }
    buf[n] = '\0';
    return n;
}

/** Read the file `name` of the task `tid` of this process into `buf`. */
static int task_file(pid_t tid, const char *name, char *buf, int cap)
{
    char path[64];

    snprintf(path, sizeof(path), "/proc/self/task/%d/%s", (int)tid, name);
    return read_file(path, buf, cap);
}

/** Wait until the thread `tid` is asleep, its stat's state S. */
static void await_sleep(pid_t tid)
{
    char buf[512];
    const char *p;

    for (;;) {
        if (task_file(tid, "stat", buf, (int)sizeof(buf)) > 0 && (p = strrchr(buf, ')')) &&
            p[1] == ' ' && p[2] == 'S') {
            return;
        }
        sched_yield();
    }
}

/** Wait until the process `pid` is asleep, its stat's state S, which
 * anyone may read. */
static void await_asleep(pid_t pid)
{
    char path[64], buf[512];
    const char *state;

    snprintf(path, sizeof(path), "/proc/%d/stat", (int)pid);
    while (read_file(path, buf, (int)sizeof(buf)) <= 0 || !(state = strrchr(buf, ')')) ||
           state[1] != ' ' || state[2] != 'S') {
        sched_yield();
    }
}

/** Parse a syscall file's `text` into `c`: 1 for a line in one of Linux's
 * two forms, with the newline that ends it, and 0 otherwise. */
static int parse_call(const char *text, struct call *c)
{
    int n;

    memset(c, 0, sizeof(*c));
    if (strncmp(text, "-1 ", 3) == 0) {
        c->nr = -1;
        return sscanf(text, "-1 0x%lx 0x%lx%n", &c->sp, &c->pc, &n) == 2 &&
               strcmp(text + n, "\n") == 0;
    }
    return sscanf(text, "%ld 0x%lx 0x%lx 0x%lx 0x%lx 0x%lx 0x%lx 0x%lx 0x%lx%n", &c->nr,
                  &c->args[0], &c->args[1], &c->args[2], &c->args[3], &c->args[4], &c->args[5],
                  &c->sp, &c->pc, &n) == 9 &&
           strcmp(text + n, "\n") == 0;
}

/** Read the syscall file of the task `tid` into `c`: 1 when it holds a call
 * in Linux's form with nonzero pointers, 0 otherwise. */
static int task_call(pid_t tid, struct call *c)
{
    char buf[256];

    if (task_file(tid, "syscall", buf, (int)sizeof(buf)) <= 0) {
        return 0;
    }
    printf("wchanmusl: task %d syscall: %s", (int)tid, buf);
    return parse_call(buf, c) && c->sp != 0 && c->pc != 0;
}

/** Whether the wchan file of the task `tid` reads `want` exactly. */
static int task_wchan(pid_t tid, const char *want)
{
    char buf[64];

    if (task_file(tid, "wchan", buf, (int)sizeof(buf)) < 0) {
        return 0;
    }
    printf("wchanmusl: task %d wchan: %s\n", (int)tid, buf);
    return strcmp(buf, want) == 0;
}

/** Whether the directory `path` lists both syscall and wchan. */
static int lists_both(const char *path)
{
    DIR *d = opendir(path);
    struct dirent *e;
    int found = 0;

    if (!d) {
        return 0;
    }
    while ((e = readdir(d)) != NULL) {
        found |= strcmp(e->d_name, "syscall") == 0 ? 1 : strcmp(e->d_name, "wchan") == 0 ? 2 : 0;
    }
    closedir(d);
    return found == 3;
}

/** The reader: block in a read of the empty pipe until the probe writes. */
static void *reader(void *arg)
{
    (void)arg;
    reader_tid = (pid_t)syscall(SYS_gettid);
    return (void *)(long)read(pipe_fds[0], read_buf, sizeof(read_buf));
}

/** The waiter: block in FUTEX_WAIT_PRIVATE on `futex_word` while it is 0. */
static void *waiter(void *arg)
{
    (void)arg;
    waiter_tid = (pid_t)syscall(SYS_gettid);
    while (futex_word == 0) {
        syscall(SYS_futex, &futex_word, FUTEX_WAIT_PRIVATE, 0, NULL, NULL, 0);
    }
    return NULL;
}

/** The spinner: run without a call until the probe stops it. */
static void *spinner(void *arg)
{
    (void)arg;
    spinner_tid = (pid_t)syscall(SYS_gettid);
    while (!spin_stop) {
    }
    return NULL;
}

/** The directories list both files, and both paths give each process
 * file Linux's mode, including the files only the owner may open. */
static int listing_rows(void)
{
    static const struct {
        const char *name;
        mode_t mode;
    } rows[] = {
        {"syscall", 0400}, {"wchan", 0444}, {"environ", 0400}, {"auxv", 0400}, {"mem", 0600}};
    char path[64];
    struct stat sc, sw;
    unsigned i;

    snprintf(path, sizeof(path), "/proc/self/task/%d", (int)syscall(SYS_gettid));
    if (!lists_both("/proc/self") || !lists_both(path)) {
        return fail("a process's and a thread's directories list syscall and wchan");
    }
    report_held();
    for (i = 0; i < sizeof(rows) / sizeof(rows[0]); i++) {
        snprintf(path, sizeof(path), "/proc/self/%s", rows[i].name);
        if (stat(path, &sc) != 0) {
            return fail("stat a process's file");
        }
        snprintf(path, sizeof(path), "/proc/self/task/%d/%s", (int)syscall(SYS_gettid),
                 rows[i].name);
        if (stat(path, &sw) != 0 || sc.st_mode != (S_IFREG | rows[i].mode) ||
            sw.st_mode != sc.st_mode) {
            return fail("a process or thread file has the wrong mode");
        }
    }
    report_held();
    return 0;
}

/** A thread blocked in a pipe's read shows the call and pipe_read. */
static int read_rows(void)
{
    pthread_t t;
    struct call c;
    void *got;

    if (pipe(pipe_fds) != 0 || pthread_create(&t, NULL, reader, NULL) != 0) {
        return fail("make the pipe and the reader");
    }
    while (reader_tid == 0) {
        sched_yield();
    }
    await_sleep(reader_tid);
    if (!task_call(reader_tid, &c) || c.nr != SYS_read || c.args[0] != (unsigned long)pipe_fds[0] ||
        c.args[1] != (unsigned long)read_buf || c.args[2] != sizeof(read_buf)) {
        return fail("a thread asleep in read shows read, its descriptor, buffer and count");
    }
    report_held();
    if (!task_wchan(reader_tid, "pipe_read")) {
        return fail("a thread asleep in a pipe's read has the wchan pipe_read");
    }
    report_held();
    if (write(pipe_fds[1], "x", 1) != 1 || pthread_join(t, &got) != 0 || (long)got != 1) {
        return fail("end the reader with a byte");
    }
    return 0;
}

/** A thread blocked in a futex wait shows the call and futex_wait_queue. */
static int futex_rows(void)
{
    pthread_t t;
    struct call c;

    if (pthread_create(&t, NULL, waiter, NULL) != 0) {
        return fail("make the futex waiter");
    }
    while (waiter_tid == 0) {
        sched_yield();
    }
    await_sleep(waiter_tid);
    if (!task_call(waiter_tid, &c) || c.nr != SYS_futex ||
        c.args[0] != (unsigned long)&futex_word || c.args[1] != FUTEX_WAIT_PRIVATE ||
        c.args[2] != 0 || c.args[3] != 0) {
        return fail("a thread asleep in futex shows futex, the word, the operation and value");
    }
    report_held();
    if (!task_wchan(waiter_tid, "futex_wait_queue")) {
        return fail("a thread asleep in a futex wait has the wchan futex_wait_queue");
    }
    report_held();
    futex_word = 1;
    syscall(SYS_futex, &futex_word, FUTEX_WAKE_PRIVATE, 1, NULL, NULL, 0);
    if (pthread_join(t, NULL) != 0) {
        return fail("end the futex waiter");
    }
    return 0;
}

/** A thread that spins reads running and has no wchan. */
static int running_rows(void)
{
    pthread_t t;
    char buf[64];

    if (pthread_create(&t, NULL, spinner, NULL) != 0) {
        return fail("make the spinner");
    }
    while (spinner_tid == 0) {
        sched_yield();
    }
    if (task_file(spinner_tid, "syscall", buf, (int)sizeof(buf)) < 0 ||
        strcmp(buf, "running\n") != 0) {
        spin_stop = 1;
        return fail("a thread that spins reads running");
    }
    report_held();
    if (!task_wchan(spinner_tid, "0")) {
        spin_stop = 1;
        return fail("a thread that spins has the wchan 0");
    }
    report_held();
    spin_stop = 1;
    if (pthread_join(t, NULL) != 0) {
        return fail("end the spinner");
    }
    return 0;
}

/** A child of another user may not open root's owner-only files, and
 * reads 0 for its wchan while root sleeps in its wait: the child's exit
 * status is 0, or 1 for an open, or 2 for the wchan. */
static int refused_rows(void)
{
    static const char *const names[] = {"syscall", "environ", "auxv", "mem"};
    pid_t parent = getpid(), pid;
    int status;

    pid = fork();
    if (pid == 0) {
        char path[64], buf[64];
        int fd;
        unsigned i;

        if (setgid(OTHER_ID) != 0 || setuid(OTHER_ID) != 0) {
            _exit(3);
        }
        /* A ready parent would read 0 from wchan by right. */
        await_asleep(parent);
        for (i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
            snprintf(path, sizeof(path), "/proc/%d/%s", (int)parent, names[i]);
            fd = open(path, O_RDONLY);
            if (fd >= 0 || errno != EACCES) {
                _exit(1);
            }
        }
        snprintf(path, sizeof(path), "/proc/%d/wchan", (int)parent);
        _exit(read_file(path, buf, (int)sizeof(buf)) == 1 && buf[0] == '0' ? 0 : 2);
    }
    if (pid < 0 || waitpid(pid, &status, 0) != pid || !WIFEXITED(status) ||
        WEXITSTATUS(status) == 3) {
        return fail("fork a child of the other user");
    }
    if (WEXITSTATUS(status) == 1) {
        return fail("another user's open of root's syscall, environ, auxv or mem is EACCES");
    }
    report_held();
    if (WEXITSTATUS(status) != 0) {
        return fail("another user reads 0 from root's wchan file");
    }
    report_held();
    return 0;
}

/**
 * Fork a child of user OTHER_ID that sleeps in a read of a new pipe, whose
 * write end goes to `*wake`, with its address space dumpable when
 * `dumpable`: the setuid from root left it not dumpable, as on Linux. The
 * child's pid, or -1.
 */
static pid_t sleeper(int dumpable, int *wake)
{
    int fds[2];
    pid_t pid;
    char c;

    *wake = -1;
    if (pipe(fds) != 0) {
        return -1;
    }
    pid = fork();
    if (pid == 0) {
        close(fds[1]);
        if (setgid(OTHER_ID) != 0 || setuid(OTHER_ID) != 0 ||
            prctl(PR_SET_DUMPABLE, dumpable, 0, 0, 0) != 0) {
            _exit(3);
        }
        (void)read(fds[0], &c, 1);
        _exit(0);
    }
    close(fds[0]);
    if (pid < 0) {
        close(fds[1]);
        return -1;
    }
    *wake = fds[1];
    return pid;
}

/** Whether `path` has full mode `mode` and both owner IDs equal to `id`. */
static int owner_is(const char *path, mode_t mode, uid_t id)
{
    struct stat st;

    return stat(path, &st) == 0 && st.st_mode == mode && st.st_uid == id && st.st_gid == id;
}

/**
 * In a child of user OTHER_ID whose address space is dumpable when
 * `dumpable`, open its own ns directories, /proc/self/ns and its thread's,
 * for reading: each opens while the child owns it, and is EACCES once root
 * owns it, as mode 0511 answers a caller who is not the owner. O_PATH,
 * which reads nothing, opens each either way. 1 when every open answers so,
 * else 0.
 */
static int ns_opens(int dumpable)
{
    char thread[64];
    const char *paths[2];
    pid_t pid;
    int status, fd, i;

    pid = fork();
    if (pid == 0) {
        snprintf(thread, sizeof(thread), "/proc/self/task/%d/ns", (int)getpid());
        paths[0] = "/proc/self/ns";
        paths[1] = thread;
        if (setgid(OTHER_ID) != 0 || setuid(OTHER_ID) != 0 ||
            prctl(PR_SET_DUMPABLE, dumpable, 0, 0, 0) != 0) {
            _exit(1);
        }
        for (i = 0; i < 2; i++) {
            fd = open(paths[i], O_RDONLY | O_DIRECTORY);
            if (dumpable ? fd < 0 : fd >= 0 || errno != EACCES) {
                _exit(2);
            }
            if (fd >= 0) {
                close(fd);
            }
            fd = open(paths[i], O_PATH | O_DIRECTORY);
            if (fd < 0) {
                _exit(3);
            }
            close(fd);
        }
        _exit(0);
    }
    return pid > 0 && waitpid(pid, &status, 0) == pid && WIFEXITED(status) &&
           WEXITSTATUS(status) == 0;
}

/** Stat a sleeper's process, task, fdinfo and ns directories and its syscall
 * file. World-readable directories keep its effective IDs even when it
 * is not dumpable; the 0500 fd and 0511 ns directories and the file then
 * belong to root. The thread's ns directory has the same mode and owner,
 * and a process opens its own ns directories as their owner says
 * (`ns_opens`). Return 0 after reaping each sleeper, or report the failed
 * row. */
static int owner_rows(void)
{
    static const char *const dirs[] = {"", "/task", "/fdinfo"};
    char path[80];
    int dumpable, wake, status, ok;
    unsigned i;
    pid_t pid;

    for (dumpable = 0; dumpable <= 1; dumpable++) {
        pid = sleeper(dumpable, &wake);
        if (pid < 0) {
            return fail("fork a sleeper for directory ownership");
        }
        await_asleep(pid);
        ok = 1;
        for (i = 0; i < sizeof(dirs) / sizeof(dirs[0]); i++) {
            snprintf(path, sizeof(path), "/proc/%d%s", (int)pid, dirs[i]);
            ok = ok && owner_is(path, S_IFDIR | 0555, OTHER_ID);
        }
        snprintf(path, sizeof(path), "/proc/%d/task/%d", (int)pid, (int)pid);
        ok = ok && owner_is(path, S_IFDIR | 0555, OTHER_ID);
        snprintf(path, sizeof(path), "/proc/%d/syscall", (int)pid);
        ok = ok && owner_is(path, S_IFREG | 0400, dumpable ? OTHER_ID : 0);
        snprintf(path, sizeof(path), "/proc/%d/fd", (int)pid);
        ok = ok && owner_is(path, S_IFDIR | 0500, dumpable ? OTHER_ID : 0);
        snprintf(path, sizeof(path), "/proc/%d/ns", (int)pid);
        ok = ok && owner_is(path, S_IFDIR | 0511, dumpable ? OTHER_ID : 0);
        snprintf(path, sizeof(path), "/proc/%d/task/%d/ns", (int)pid, (int)pid);
        ok = ok && owner_is(path, S_IFDIR | 0511, dumpable ? OTHER_ID : 0);
        ok = ok && ns_opens(dumpable);
        close(wake);
        if (waitpid(pid, &status, 0) != pid || !WIFEXITED(status) || WEXITSTATUS(status) != 0 ||
            !ok) {
            return fail("a process directory has the wrong mode or dumpable owner");
        }
    }
    report_held();
    return 0;
}

/** Take the ids of `how` (`peek`) as root: 0, or -1. */
static int take_ids(int how)
{
    if (how != 'f') {
        return setgid(how == 'g' ? THIRD_ID : OTHER_ID) == 0 && setuid(OTHER_ID) == 0 ? 0 : -1;
    }
    if (setresgid(THIRD_ID, THIRD_ID, OTHER_ID) != 0 || setfsgid(OTHER_ID) < 0 ||
        setresuid(THIRD_ID, THIRD_ID, OTHER_ID) != 0 || setfsuid(OTHER_ID) < 0) {
        return -1;
    }
    return (unsigned)setfsuid((uid_t)-1) == OTHER_ID ? 0 : -1;
}

/** Read the syscall file of `target` as `peek` says: 0, 3 for the open or
 * 4 for the read. */
static int peek_call(pid_t target, int open_ok, int call_ok)
{
    char path[64], buf[512];
    struct call c;
    int fd, n;

    snprintf(path, sizeof(path), "/proc/%d/syscall", (int)target);
    fd = open(path, O_RDONLY);
    if (open_ok ? fd < 0 : fd >= 0 || errno != EACCES) {
        return 3;
    }
    if (fd < 0) {
        return 0;
    }
    n = (int)read(fd, buf, sizeof(buf) - 1);
    buf[n > 0 ? n : 0] = '\0';
    close(fd);
    if (call_ok ? n <= 0 || !parse_call(buf, &c) || c.nr != SYS_read : n != -1 || errno != EPERM) {
        return 4;
    }
    return 0;
}

/**
 * In a child, take the ids of `how` and read the files of `target`, a
 * sleeper of user OTHER_ID, once its stat shows it asleep. `how` is 'f' for
 * real and effective user and group THIRD_ID with file-system ones OTHER_ID,
 * which Linux's ptrace check with PTRACE_MODE_FSCREDS lets read the files;
 * 'u' for every id OTHER_ID; and 'g' for user OTHER_ID with group THIRD_ID,
 * whose user owns the syscall file and whose group the check refuses. With
 * `open_ok` the open of syscall must succeed and with `call_ok` its read
 * give the read; otherwise the open is EACCES, or the read EPERM. The
 * wchan reads pipe_read when `call_ok` and 0 otherwise. The child's exit
 * status: 0, 1 for the ids, 2 for the wchan, 3 for the open and 4 for the
 * read.
 */
static int peek(pid_t target, int how, int open_ok, int call_ok)
{
    char path[64], buf[64];
    pid_t pid = fork();
    int status;

    if (pid != 0) {
        return pid < 0 || waitpid(pid, &status, 0) != pid || !WIFEXITED(status)
                   ? 1
                   : WEXITSTATUS(status);
    }
    if (take_ids(how) != 0) {
        _exit(1);
    }
    await_asleep(target);
    snprintf(path, sizeof(path), "/proc/%d/wchan", (int)target);
    if (read_file(path, buf, (int)sizeof(buf)) < 0 ||
        strcmp(buf, call_ok ? "pipe_read" : "0") != 0) {
        _exit(2);
    }
    _exit(peek_call(target, open_ok, call_ok));
}

/**
 * Linux's ptrace check judges /proc/PID/syscall and wchan by the caller's
 * file-system ids, every id of the process and its dumpable flag: a caller
 * whose file-system ids alone are the sleeper's reads both; one with every
 * id the sleeper's gets EACCES from the open of a sleeper that is not
 * dumpable, which root owns, and 0 from its wchan; and one with the
 * sleeper's user and another group opens the file it owns and gets EPERM
 * from the read, and 0 from the wchan.
 */
static int cred_rows(void)
{
    static const struct {
        int dumpable, how, open_ok, call_ok;
        const char *what;
    } rows[] = {{1, 'f', 1, 1, "a caller with the sleeper's file-system ids alone reads its files"},
                {0, 'u', 0, 0, "a sleeper that is not dumpable refuses its owner's user"},
                {1, 'g', 1, 0, "a caller of the sleeper's user and another group reads EPERM"}};
    char why[160];
    unsigned i;
    int wake, rc, status;
    pid_t pid;

    for (i = 0; i < sizeof(rows) / sizeof(rows[0]); i++) {
        pid = sleeper(rows[i].dumpable, &wake);
        if (pid < 0) {
            return fail("fork a sleeper of the other user");
        }
        rc = peek(pid, rows[i].how, rows[i].open_ok, rows[i].call_ok);
        close(wake);
        if (waitpid(pid, &status, 0) != pid || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
            return fail("a sleeper of the other user sleeps and ends");
        }
        if (rc != 0) {
            snprintf(why, sizeof(why), "%s (step %d)", rows[i].what, rc);
            return fail(why);
        }
        report_held();
    }
    return 0;
}

/** Begin the report and plan its assertions, in the order they are
 * checked. */
static void plan(void)
{
    static const char *const ids[] = {
        "listed",           "modes",   "read-call",     "read-wchan",      "futex-call",
        "futex-wchan",      "running", "running-wchan", "refused-syscall", "refused-wchan",
        "directory-owners", "fs-ids",  "not-dumpable",  "other-group"};
    unsigned i;

    report_begin(report_case(environ, "guest.default.wchanmusl"));
    for (i = 0; i < sizeof(ids) / sizeof(ids[0]); i++) {
        report_plan(ids[i]);
    }
}

int main(int argc, char **argv)
{
    probe_args(argc, argv);
    setvbuf(stdout, NULL, _IONBF, 0);
    plan();
    if (listing_rows() != 0 || read_rows() != 0 || futex_rows() != 0 || running_rows() != 0 ||
        refused_rows() != 0 || owner_rows() != 0 || cred_rows() != 0) {
        return 1;
    }
    report_end();
    printf("WCHANMUSL OK\n");
    return 0;
}
