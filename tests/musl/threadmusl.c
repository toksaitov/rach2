/**
 * Threads.
 *
 * Four threads start and are joined, each with its own thread id, the
 * process's pid, and its own copy of a thread-local variable, and they count
 * to 40000 under one mutex in memory they share. A condition variable
 * passes a value to a waiter seen asleep. pthread_kill reaches the thread
 * it names, which alone runs the handler. A signal sent to the process
 * while every thread blocks it waits, as sigpending reports, with no handler
 * run, and a thread made after it takes it once it unblocks it. Of two signals
 * sent to the process at once, the second, which the first one's handler
 * blocks, is taken by another thread while that handler runs. A thread's exit
 * leaves the process running; /proc/self/task lists each live thread, a
 * thread's directory there has no task directory and keeps its path in a
 * descriptor, down to the thread's descriptor links, a task path names only a
 * thread of its process, a thread's stat there has its own time, and
 * /proc/self/exe names the program. The threads share the descriptors, the
 * working directory and the umask: what one opens, closes or changes the others
 * see, and an execve that fails leaves its thread in the shared table. A
 * descriptor one thread closes while another reads it stays with the read,
 * which returns what is written after the close, and its number is free once
 * the read is over. A wait whose status page another thread unmaps while it
 * sleeps fails with EFAULT and leaves the zombie to a later wait. A child
 * forked by a thread has one thread. A thread that calls execve replaces the
 * whole process, whose other threads end. Of two threads in execve at once, the
 * one whose call first waits for the other threads to end replaces the process,
 * and the other ends: the probe traces the second and holds it at the entry of
 * its call until the first waits for it. A robust mutex a thread other than the
 * first holds when it calls execve is released, and its next locker gets
 * EOWNERDEAD. A thread that calls exit ends them all.
 *
 * Prints "THREADMUSL OK" on success and "THREADMUSL FAIL (detail)"
 * otherwise.
 */

#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/ptrace.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/times.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define THREADS 4
#define ROUNDS  10000

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t ready = PTHREAD_COND_INITIALIZER;
static int counter;
static int handed;
static __thread int mine;
static volatile pid_t ids[THREADS];
static volatile int handled_by;
static volatile int spare_blocked;

static int fail(const char *what)
{
    printf("THREADMUSL FAIL (%s: errno %d)\n", what, errno);
    return 1;
}

static pid_t tid(void)
{
    return (pid_t)syscall(SYS_gettid);
}

static void *count(void *arg)
{
    int i = (int)(long)arg;
    int n;

    ids[i] = tid();
    mine = i + 100;
    for (n = 0; n < ROUNDS; n++) {
        pthread_mutex_lock(&lock);
        counter++;
        pthread_mutex_unlock(&lock);
    }
    return (void *)(long)(mine == i + 100 && getpid() != ids[i] ? 1 : 0);
}

static volatile pid_t receiver_tid;

static void *receive(void *arg)
{
    (void)arg;
    receiver_tid = tid();
    pthread_mutex_lock(&lock);
    while (!handed) {
        pthread_cond_wait(&ready, &lock);
    }
    pthread_mutex_unlock(&lock);
    return (void *)(long)handed;
}

static void on_usr1(int sig)
{
    (void)sig;
    handled_by = tid();
}

/**
 * Wait until SIGUSR1's handler has run. The test of `handled_by` is made with
 * SIGUSR1 blocked, and sigsuspend unblocks it only for the wait, so a signal
 * that comes between the test and the wait ends the wait and is not lost.
 * SIGUSR1 stays blocked after.
 */
static void await_usr1(void)
{
    sigset_t usr1, during;

    sigemptyset(&usr1);
    sigaddset(&usr1, SIGUSR1);
    pthread_sigmask(SIG_BLOCK, &usr1, &during);
    sigdelset(&during, SIGUSR1);
    while (!handled_by) {
        sigsuspend(&during);
    }
}

static void *wait_signal(void *arg)
{
    (void)arg;
    await_usr1();
    return NULL;
}

/* The thread pthread_kill names: it waits for the signal and keeps it
 * blocked after, so the process-directed signal later can reach only the
 * thread made for that. */
static void *spare_main(void *arg)
{
    (void)arg;
    await_usr1();
    spare_blocked = 1;
    for (;;) {
        pause();
    }
    return NULL;
}

/**
 * A signal sent to the process while its two threads block it, the first
 * and the one `spare_main` runs: it waits, as sigpending reports, and no
 * handler runs. A thread made after it then takes it in `wait_signal`, the
 * first thread still blocking it. Returns 0, or 1 once the failure is
 * reported.
 */
static int process_signal_row(void)
{
    sigset_t block, pending;
    pthread_t w;

    handled_by = 0;
    sigemptyset(&block);
    sigaddset(&block, SIGUSR1);
    pthread_sigmask(SIG_BLOCK, &block, NULL);
    kill(getpid(), SIGUSR1);
    if (sigpending(&pending) != 0 || sigismember(&pending, SIGUSR1) != 1 || handled_by != 0) {
        return fail("a process signal all threads block was not pending with no handler run");
    }
    if (pthread_create(&w, NULL, wait_signal, NULL) != 0) {
        return fail("pthread_create for the process signal");
    }
    if (pthread_join(w, NULL) != 0 || handled_by == 0 || handled_by == tid()) {
        return fail("a process signal did not reach the thread that has it unblocked");
    }
    pthread_sigmask(SIG_UNBLOCK, &block, NULL);
    return 0;
}

static void *idle(void *arg)
{
    (void)arg;
    for (;;) {
        pause();
    }
    return NULL;
}

static int count_tasks(void)
{
    DIR *d = opendir("/proc/self/task");
    struct dirent *e;
    int n = 0;

    if (!d) {
        return -1;
    }
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] != '.') {
            n++;
        }
    }
    closedir(d);
    return n;
}

/**
 * The paths of thread `t` under this process's task directory: its
 * directory lists no task directory of its own, a descriptor on it keeps
 * its path, for its link and for a name relative to it, the thread's
 * descriptor links are there six components deep, and a task path under a
 * process the thread is not of, or under none, names nothing.
 */
static int task_paths(pid_t t)
{
    char dir[64], path[96], link[96];
    struct dirent *e;
    struct stat st;
    DIR *d;
    int dirfd, fd, task = 0;
    long n;

    snprintf(dir, sizeof(dir), "/proc/%d/task/%d", (int)getpid(), (int)t);
    d = opendir(dir);
    if (!d) {
        return fail("a thread's directory");
    }
    while ((e = readdir(d)) != NULL) {
        task |= strcmp(e->d_name, "task") == 0;
    }
    closedir(d);
    if (task) {
        return fail("a thread's directory lists a task directory");
    }
    dirfd = open(dir, O_RDONLY | O_DIRECTORY);
    snprintf(path, sizeof(path), "/proc/self/fd/%d", dirfd);
    n = readlink(path, link, sizeof(link) - 1);
    if (dirfd < 0 || n <= 0 || (link[n] = '\0', strcmp(link, dir) != 0)) {
        return fail("the link of a thread's directory");
    }
    fd = openat(dirfd, "stat", O_RDONLY);
    close(dirfd);
    if (fd < 0) {
        return fail("a name relative to a thread's directory");
    }
    close(fd);
    snprintf(path, sizeof(path), "%s/fd/0", dir);
    if (lstat(path, &st) != 0 || !S_ISLNK(st.st_mode)) {
        return fail("a thread's descriptor link");
    }
    snprintf(path, sizeof(path), "/proc/1/task/%d/status", (int)t);
    if (open(path, O_RDONLY) != -1 || errno != ENOENT) {
        return fail("a thread under another process's task directory");
    }
    snprintf(path, sizeof(path), "/proc/999999/task/%d/status", (int)t);
    if (open(path, O_RDONLY) != -1 || errno != ENOENT) {
        return fail("a task path under no process");
    }
    return 0;
}

/** The user and system ticks, fields 14 and 15, of the stat file `path`;
 * -1 when it cannot be read. */
static long stat_ticks(const char *path)
{
    char line[256], *paren;
    unsigned long u, s;
    int fd = open(path, O_RDONLY);
    long n;

    if (fd < 0) {
        return -1;
    }
    n = read(fd, line, sizeof(line) - 1);
    close(fd);
    if (n <= 0) {
        return -1;
    }
    line[n] = '\0';
    paren = strrchr(line, ')');
    if (!paren ||
        sscanf(paren + 1, " %*s %*s %*s %*s %*s %*s %*s %*s %*s %*s %*s %lu %lu", &u, &s) != 2) {
        return -1;
    }
    return (long)(u + s);
}

/**
 * A thread's stat under the task directory has the thread's own time: this
 * thread spins until its own counts five ticks more, bounded by the ticks
 * since boot, while thread `t` sleeps and gains none of them, and the
 * process's stat has at least as many as this thread's.
 */
static int thread_times(pid_t t)
{
    char own_stat[64], other[64];
    struct tms tms;
    long start, now, rested, deadline;
    volatile unsigned long spin = 0;
    int i;

    snprintf(own_stat, sizeof(own_stat), "/proc/self/task/%d/stat", (int)tid());
    snprintf(other, sizeof(other), "/proc/self/task/%d/stat", (int)t);
    start = stat_ticks(own_stat);
    rested = stat_ticks(other);
    deadline = (long)times(&tms) + 1000;
    do {
        for (i = 0; i < 1000000; i++) {
            spin++;
        }
        now = stat_ticks(own_stat);
    } while (start >= 0 && now >= 0 && now < start + 5 && (long)times(&tms) < deadline);
    if (start < 0 || now < start + 5) {
        return fail("a busy thread's own time");
    }
    if (rested < 0 || stat_ticks(other) - rested >= now - start) {
        return fail("a sleeping thread's time in its task stat");
    }
    if (stat_ticks("/proc/self/stat") < now) {
        return fail("the process's time in its stat");
    }
    return 0;
}

static int shared_fd = -1;

static void *open_file(void *arg)
{
    (void)arg;
    shared_fd = open("/dev/null", O_WRONLY);
    return NULL;
}

static void *close_file(void *arg)
{
    (void)arg;
    return (void *)(long)close(shared_fd);
}

/** Open a descriptor after an execve of a program that is not there, which
 * fails; 1 when it failed with ENOENT. */
static void *exec_missing(void *arg)
{
    int missing;

    (void)arg;
    execl("/musl/no-such-program", "no-such-program", (char *)0);
    missing = errno == ENOENT;
    shared_fd = open("/dev/null", O_WRONLY);
    return (void *)(long)missing;
}

static void *change_dir(void *arg)
{
    (void)arg;
    umask(077);
    return (void *)(long)chdir("/tmp");
}

static int pipe_fds[2];
static volatile pid_t reader_tid;

static void *read_pipe(void *arg)
{
    char c = 0;
    long n;

    (void)arg;
    reader_tid = tid();
    n = read(pipe_fds[0], &c, 1);
    return (void *)(n == 1 && c == 'x' ? 1L : 0L);
}

/** Whether thread `t` of this process sleeps, by its state in
 * /proc/self/task/<t>/stat. */
static int sleeping(pid_t t)
{
    char path[64], line[256], *state;
    int fd, n;

    snprintf(path, sizeof(path), "/proc/self/task/%d/stat", (int)t);
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
 * A condition variable passes a value to a thread that is seen asleep in its
 * wait before the value is published, so that only the signal's wake can
 * end the wait. Returns 0, or 1 once the failure is reported.
 */
static int condition_row(void)
{
    pthread_t r;
    void *result;
    int i;

    if (pthread_create(&r, NULL, receive, NULL) != 0) {
        return fail("pthread_create for the condition");
    }
    for (i = 0; i < 500 && !(receiver_tid && sleeping(receiver_tid)); i++) {
        usleep(10000);
    }
    if (i == 500) {
        return fail("the receiver was never seen asleep in pthread_cond_wait");
    }
    pthread_mutex_lock(&lock);
    handed = 5;
    pthread_cond_signal(&ready);
    pthread_mutex_unlock(&lock);
    if (pthread_join(r, &result) != 0 || (long)result != 5) {
        return fail("the condition variable");
    }
    return 0;
}

/** The sharing of descriptors, the working directory and the umask. */
static int sharing(void)
{
    pthread_t t;
    void *result;
    char cwd[64], exe[64];
    long n;
    int i;

    if (pthread_create(&t, NULL, open_file, NULL) != 0 || pthread_join(t, NULL) != 0 ||
        shared_fd < 0 || write(shared_fd, "x", 1) != 1) {
        return fail("a descriptor a thread opened");
    }
    if (pthread_create(&t, NULL, close_file, NULL) != 0 || pthread_join(t, &result) != 0 ||
        result != 0 || fcntl(shared_fd, F_GETFD) != -1 || errno != EBADF) {
        return fail("a descriptor a thread closed");
    }
    shared_fd = -1;
    if (pthread_create(&t, NULL, exec_missing, NULL) != 0 || pthread_join(t, &result) != 0 ||
        (long)result != 1 || shared_fd < 0 || write(shared_fd, "x", 1) != 1 ||
        close(shared_fd) != 0) {
        return fail("a descriptor a thread opened after its execve failed");
    }
    if (pthread_create(&t, NULL, change_dir, NULL) != 0 || pthread_join(t, &result) != 0 ||
        result != 0 || !getcwd(cwd, sizeof(cwd)) || strcmp(cwd, "/tmp") != 0 || umask(022) != 077 ||
        chdir("/") != 0) {
        return fail("the directory and umask a thread changed");
    }
    n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (n <= 0 || (exe[n] = '\0', strcmp(exe, "/musl/threadmusl") != 0)) {
        return fail("/proc/self/exe");
    }

    if (pipe(pipe_fds) != 0 || pthread_create(&t, NULL, read_pipe, NULL) != 0) {
        return fail("the pipe and its reader");
    }
    for (i = 0; i < 500 && !(reader_tid && sleeping(reader_tid)); i++) {
        usleep(10000);
    }
    if (close(pipe_fds[0]) != 0 || write(pipe_fds[1], "x", 1) != 1 ||
        pthread_join(t, &result) != 0 || (long)result != 1) {
        return fail("a read on a descriptor another thread closed");
    }
    i = pipe_fds[0];
    close(pipe_fds[1]);
    if (pipe(pipe_fds) != 0 || pipe_fds[0] != i) {
        return fail("the number of a descriptor closed under a read");
    }
    close(pipe_fds[0]);
    close(pipe_fds[1]);
    return 0;
}

static volatile pid_t takers[2];
static int slots[2] = {0, 1}; /* each taker's index in `takers` */
static volatile pid_t usr1_by, usr2_by;

/** SIGUSR1's handler in `handler_mask`: its mask blocks SIGUSR2, and it
 * waits for SIGUSR2's handler to run in another thread. */
static void on_usr1_waits(int sig)
{
    int i;

    (void)sig;
    usr1_by = tid();
    for (i = 0; i < 1000 && !usr2_by; i++) {
        usleep(10000);
    }
}

static void on_usr2(int sig)
{
    (void)sig;
    usr2_by = tid();
}

/** A thread of `handler_mask`, which sleeps with both signals unblocked
 * until the two handlers have run. */
static void *take_signals(void *arg)
{
    sigset_t none;

    takers[*(const int *)arg] = tid();
    sigemptyset(&none);
    pthread_sigmask(SIG_SETMASK, &none, NULL);
    while (!usr1_by || !usr2_by) {
        pause();
    }
    return NULL;
}

/**
 * Two threads asleep in pause with SIGUSR1 and SIGUSR2 unblocked, while the
 * first thread blocks both, and the two signals sent to the process one after
 * the other, so that the first sleeper may be woken for both. SIGUSR1's
 * handler blocks SIGUSR2 and waits for SIGUSR2's handler, which the other
 * sleeper must be woken to run. Returns 0 when each thread ran one handler.
 */
static int handler_mask(void)
{
    struct sigaction sa;
    sigset_t both;
    pthread_t a, b;
    int i;

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_usr1_waits;
    sigemptyset(&sa.sa_mask);
    sigaddset(&sa.sa_mask, SIGUSR2);
    sigaction(SIGUSR1, &sa, NULL);
    sa.sa_handler = on_usr2;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGUSR2, &sa, NULL);
    sigemptyset(&both);
    sigaddset(&both, SIGUSR1);
    sigaddset(&both, SIGUSR2);
    pthread_sigmask(SIG_BLOCK, &both, NULL);
    if (pthread_create(&a, NULL, take_signals, &slots[0]) != 0 ||
        pthread_create(&b, NULL, take_signals, &slots[1]) != 0) {
        return 1;
    }
    for (i = 0; i < 500 && !(takers[0] && sleeping(takers[0]) && takers[1] && sleeping(takers[1]));
         i++) {
        usleep(10000);
    }
    kill(getpid(), SIGUSR1);
    kill(getpid(), SIGUSR2);
    if (pthread_join(a, NULL) != 0 || pthread_join(b, NULL) != 0) {
        return 1;
    }
    return usr1_by != 0 && usr2_by != 0 && usr1_by != usr2_by ? 0 : 1;
}

/** `handler_mask` in a child, where no other thread of the probe can take
 * the two signals, judged by the child's status. */
static int handler_mask_row(void)
{
    int i, status = 0;
    pid_t pid = fork();

    if (pid == 0) {
        _exit(handler_mask());
    }
    for (i = 0; i < 2000 && waitpid(pid, &status, WNOHANG) == 0; i++) {
        usleep(10000);
    }
    if (i == 2000) {
        kill(pid, SIGKILL);
        waitpid(pid, &status, 0);
        return fail("a signal a handler's mask blocked did not wake the other thread");
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        return fail("the two signals were not taken by the two threads");
    }
    return 0;
}

static volatile pid_t waiter_tid;
static int *volatile status_page;

/** The thread of `lost_status_row` that waits for the child `*arg` with its
 * status in a page the first thread unmaps meanwhile; the errno of the wait,
 * or 0 when it did not fail. */
static void *wait_unmapped(void *arg)
{
    pid_t child = *(pid_t *)arg;

    waiter_tid = tid();
    if (waitpid(child, status_page, 0) != -1) {
        return (void *)0L;
    }
    return (void *)(long)errno;
}

/**
 * A wait whose status page another thread unmaps while it sleeps: when the
 * child ends the wait cannot store the status, so it fails with EFAULT and
 * leaves the zombie, which a second wait reaps with its status.
 */
static int lost_status_row(void)
{
    pthread_t t;
    void *result;
    int p[2], i, status = 0;
    pid_t child;
    char c;

    status_page = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (status_page == MAP_FAILED || pipe(p) != 0) {
        return fail("the status page and the pipe");
    }
    child = fork();
    if (child == 0) {
        _exit(read(p[0], &c, 1) == 1 ? 7 : 1);
    }
    if (pthread_create(&t, NULL, wait_unmapped, &child) != 0) {
        return fail("pthread_create for the wait");
    }
    for (i = 0; i < 500 && !(waiter_tid && sleeping(waiter_tid)); i++) {
        usleep(10000);
    }
    if (munmap(status_page, 4096) != 0 || write(p[1], "x", 1) != 1 ||
        pthread_join(t, &result) != 0 || (long)result != EFAULT) {
        return fail("a wait whose status page went while it slept did not fail with EFAULT");
    }
    if (waitpid(child, &status, 0) != child || !WIFEXITED(status) || WEXITSTATUS(status) != 7) {
        return fail("the zombie a failed wait left was not reaped with its status");
    }
    close(p[0]);
    close(p[1]);
    return 0;
}

/** The rows that follow the first ones: what the threads share, a signal a
 * handler's mask blocks, and a wait whose status page goes; 0 when all pass.
 * They are one call so that `main` stays within the complexity
 * tools/verify/sizes.reasons allows it. */
static int later_rows(void)
{
    return sharing() != 0 || handler_mask_row() != 0 || lost_status_row() != 0;
}

static void *exec_self(void *arg)
{
    (void)arg;
    execl("/bin/sh", "sh", "-c", "exit 9", (char *)0);
    return NULL;
}

static void *exit_all(void *arg)
{
    (void)arg;
    exit(8);
}

static int tid_pipe[2];

/** The second exec of the two: the thread makes the probe its tracer, says
 * its id, stops, and then calls execve. */
static void *exec_second(void *arg)
{
    pid_t self = tid();

    (void)arg;
    if (ptrace(PTRACE_TRACEME, 0, NULL, NULL) != 0 ||
        write(tid_pipe[1], &self, sizeof(self)) != sizeof(self)) {
        _exit(2);
    }
    syscall(SYS_tgkill, getpid(), self, SIGSTOP);
    execl("/bin/sh", "sh", "-c", "exit 7", (char *)0);
    _exit(3);
}

static void on_alarm(int sig)
{
    (void)sig;
}

/* The process-shared robust mutex of the robust row, in shared memory. */
static pthread_mutex_t *robust;

/** Lock the robust mutex, and keep it into an execve. */
static void *lock_and_exec(void *arg)
{
    (void)arg;
    if (pthread_mutex_lock(robust) != 0) {
        _exit(2);
    }
    execl("/bin/sh", "sh", "-c", "exit 7", (char *)0);
    _exit(3);
}

/**
 * A robust mutex that a thread other than the first holds when it calls
 * execve is released under the id the thread locked it with, before the
 * thread takes the process id, so the probe's lock after the child's end
 * gets EOWNERDEAD and the mutex. A release that looked for the process id
 * would leave the mutex held for good; a time limit bounds the lock.
 */
static int robust_exec(void)
{
    pthread_mutexattr_t attr;
    struct timespec deadline;
    int status, rc;
    pid_t pid;

    robust = mmap(NULL, sizeof(*robust), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (robust == MAP_FAILED || pthread_mutexattr_init(&attr) != 0 ||
        pthread_mutexattr_setpshared(&attr, PTHREAD_PROCESS_SHARED) != 0 ||
        pthread_mutexattr_setrobust(&attr, PTHREAD_MUTEX_ROBUST) != 0 ||
        pthread_mutex_init(robust, &attr) != 0) {
        return fail("the robust mutex");
    }
    pid = fork();
    if (pid == 0) {
        pthread_t t;

        if (pthread_create(&t, NULL, lock_and_exec, NULL) == 0) {
            pthread_join(t, NULL);
        }
        _exit(4);
    }
    if (waitpid(pid, &status, 0) != pid || !WIFEXITED(status) || WEXITSTATUS(status) != 7) {
        return fail("the child whose thread locked a robust mutex and called execve");
    }
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += 5;
    rc = pthread_mutex_timedlock(robust, &deadline);
    if (rc != EOWNERDEAD) {
        errno = rc;
        return fail("a robust mutex a thread held into its execve");
    }
    pthread_mutex_consistent(robust);
    pthread_mutex_unlock(robust);
    munmap(robust, sizeof(*robust));
    return 0;
}

/**
 * Two execs at once. The child's second thread stops at the entry of its
 * execve under the probe's PTRACE_SYSCALL, and only then does the first
 * thread call execve, which sends it SIGKILL and waits for it to end. The
 * SIGKILL ends the stop, and the second call must give up and let its
 * thread end rather than wait for the first: the child then runs the first
 * one's program. An alarm bounds the waits, since two calls that each
 * waited for the other would never end.
 */
static int two_execs(void)
{
    struct sigaction sa;
    int go[2], status, ok;
    pid_t pid, second;
    char c;

    if (pipe(go) != 0 || pipe(tid_pipe) != 0) {
        return fail("the pipes of the two execs");
    }
    pid = fork();
    if (pid == 0) {
        pthread_t t;

        close(go[1]);
        if (pthread_create(&t, NULL, exec_second, NULL) != 0 || read(go[0], &c, 1) != 1) {
            _exit(4);
        }
        execl("/bin/sh", "sh", "-c", "exit 9", (char *)0);
        _exit(5);
    }
    close(go[0]);
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_alarm;
    sigaction(SIGALRM, &sa, NULL);
    alarm(20);
    ok = read(tid_pipe[0], &second, sizeof(second)) == sizeof(second) &&
         waitpid(second, &status, __WALL) == second && WIFSTOPPED(status) &&
         WSTOPSIG(status) == SIGSTOP && ptrace(PTRACE_SYSCALL, second, NULL, NULL) == 0 &&
         waitpid(second, &status, __WALL) == second && WIFSTOPPED(status) &&
         WSTOPSIG(status) == SIGTRAP && write(go[1], "g", 1) == 1 &&
         waitpid(second, &status, __WALL) == second && WIFSIGNALED(status) &&
         WTERMSIG(status) == SIGKILL && waitpid(pid, &status, 0) == pid && WIFEXITED(status) &&
         WEXITSTATUS(status) == 9;
    alarm(0);
    close(go[1]);
    close(tid_pipe[0]);
    close(tid_pipe[1]);
    return ok ? 0 : fail("two execs at once");
}

int main(void)
{
    pthread_t t[THREADS], spare;
    void *result;
    int i, status;
    pid_t pid;

    mine = 1;
    for (i = 0; i < THREADS; i++) {
        if (pthread_create(&t[i], NULL, count, (void *)(long)i) != 0) {
            return fail("pthread_create");
        }
    }
    for (i = 0; i < THREADS; i++) {
        if (pthread_join(t[i], &result) != 0 || (long)result != 1) {
            return fail("a thread's id or its thread-local variable");
        }
    }
    if (counter != THREADS * ROUNDS || mine != 1 || ids[0] == ids[1]) {
        return fail("the count under the mutex, or the ids");
    }

    if (condition_row() != 0) {
        return 1;
    }

    signal(SIGUSR1, on_usr1);
    if (pthread_create(&spare, NULL, spare_main, NULL) != 0) {
        return fail("pthread_create for pthread_kill");
    }
    pthread_kill(spare, SIGUSR1);
    while (!spare_blocked) {
        sched_yield();
    }
    if (handled_by == tid()) {
        return fail("pthread_kill ran the handler in the wrong thread");
    }
    if (count_tasks() != 2) {
        return fail("/proc/self/task did not list two threads");
    }
    if (task_paths((pid_t)handled_by) != 0 || thread_times((pid_t)handled_by) != 0 ||
        later_rows() != 0) {
        return 1;
    }

    if (process_signal_row() != 0) {
        return 1;
    }

    pid = fork();
    if (pid == 0) {
        _exit(count_tasks() == 1 ? 0 : 1);
    }
    if (waitpid(pid, &status, 0) != pid || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        return fail("a child forked by a thread did not have one thread");
    }

    pid = fork();
    if (pid == 0) {
        pthread_t e, s;

        pthread_create(&s, NULL, idle, NULL);
        pthread_create(&e, NULL, exec_self, NULL);
        pthread_join(e, NULL);
        _exit(1);
    }
    if (waitpid(pid, &status, 0) != pid || !WIFEXITED(status) || WEXITSTATUS(status) != 9) {
        return fail("execve from a thread");
    }
    if (two_execs() != 0 || robust_exec() != 0) {
        return 1;
    }

    pid = fork();
    if (pid == 0) {
        pthread_t e, s;

        pthread_create(&s, NULL, idle, NULL);
        pthread_create(&e, NULL, exit_all, NULL);
        for (;;) {
            pause();
        }
    }
    if (waitpid(pid, &status, 0) != pid || !WIFEXITED(status) || WEXITSTATUS(status) != 8) {
        return fail("exit from a thread did not end the process");
    }
    printf("THREADMUSL OK\n");
    return 0;
}
