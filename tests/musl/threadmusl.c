/**
 * threadmusl: check threads, made and used through musl's pthreads.
 *
 * Four threads start and are joined, each with its own thread id, the process's
 * pid, and its own copy of a thread-local variable, and they count to 40000
 * under one mutex in memory they share. A condition variable passes a value to
 * a waiter seen asleep. pthread_kill reaches the thread it is given, which
 * alone runs the handler. A signal sent to the process while every thread
 * blocks it waits, as sigpending reports, with no handler run, and a thread
 * made after it takes it once it unblocks it. Of two signals sent to the
 * process at once, the second, which the first one's handler blocks, is taken
 * by another thread while that handler runs. A thread's exit leaves the process
 * running; /proc/self/task lists each live thread and has two links and one for
 * each, as Chromium's zygote reads it, a thread's directory there has no task
 * directory and keeps its path in a descriptor, down to the thread's descriptor
 * links, a task path reaches only a thread of its process, a thread's stat
 * there has its own time, and /proc/self/exe reads as the program's path. The
 * threads share the descriptors, the working directory and the umask: what one
 * opens, closes or changes the others see, and an execve that fails leaves its
 * thread in the shared table. A descriptor one thread closes while another
 * reads it stays with the read, which returns what is written after the close,
 * and its number is free once the read is over. A wait whose status page
 * another thread unmaps while it sleeps fails with EFAULT and leaves the zombie
 * to a later wait. A TCP receive, a TCP send and a UDP receive over the
 * loopback whose buffer goes the same way fail with EFAULT too: the receives
 * leave the data queued, and the send queues nothing. A child forked by a
 * thread has one thread. A thread that calls execve replaces the whole process,
 * whose other threads end. Of two threads in execve at once, the one whose call
 * first waits for the other threads to end replaces the process, and the other
 * ends: the probe traces the second and holds it at the entry of its call until
 * the first waits for it. A robust mutex a thread other than the first holds
 * when it calls execve is released, and its next locker gets EOWNERDEAD. A
 * thread that calls exit ends them all. A thread other than the first empties
 * the console's queue of lines, opens the framebuffer, sets its palette, writes
 * a pixel, reads the console as the display's owner reads it, raw scancodes, of
 * which it finds none, where the line discipline would answer EAGAIN, and
 * closes it, which gives the display back for another process to open: the
 * display belongs to the process, whichever thread opened it.
 *
 * It prints "THREADMUSL OK" and exits 0 on success, or prints "THREADMUSL
 * FAIL (detail: errno N)" and exits 1 at the first failure.
 *
 * Needs: the console as stdin, in canonical mode, as a boot of the default
 * suite has it; the framebuffer's row reads it.
 */

#define _GNU_SOURCE
#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/ptrace.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/times.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define THREADS 4
#define ROUNDS  10000

/* The loopback ports of the socket rows, on which no other probe listens. */
#define LOST_TCP_PORT 7480
#define LOST_UDP_PORT 7481

/* The framebuffer's requests, as the kernel numbers them. */
#define FB_SETPALETTE 0x4603

/* The counting threads' mutex and count, the condition row's value, each
 * counting thread's thread-local value and thread id, the thread that ran
 * SIGUSR1's handler, and the spare thread's flag once it blocks SIGUSR1. */
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t ready = PTHREAD_COND_INITIALIZER;
static int counter;
static int handed;
static __thread int mine;
static volatile pid_t ids[THREADS];
static volatile int handled_by;
static volatile int spare_blocked;

/** Report one failed check with errno and return 1, the failure status. */
static int fail(const char *what)
{
    printf("THREADMUSL FAIL (%s: errno %d)\n", what, errno);
    return 1;
}

/** Return the caller's thread id. */
static pid_t tid(void)
{
    return (pid_t)syscall(SYS_gettid);
}

/**
 * Run as counting thread number `arg`: store its thread id in `ids`, set its
 * thread-local `mine`, and add `ROUNDS` to `counter` under `lock`. Returns 1
 * when `mine` kept its value and the thread id is not the pid, and 0
 * otherwise.
 */
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

/* The thread id of the receiver of `condition_row`, once it runs. */
static volatile pid_t receiver_tid;

/**
 * Run as the receiver of `condition_row`: wait on `ready` until `handed` is
 * set. Returns the value handed.
 */
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

/** Record in `handled_by` the thread that ran SIGUSR1's handler. */
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

/** Run as a thread that waits for SIGUSR1's handler in `await_usr1`. */
static void *wait_signal(void *arg)
{
    (void)arg;
    await_usr1();
    return NULL;
}

/**
 * Run as the thread pthread_kill is given: wait for the signal and keep it
 * blocked after, so the process-directed signal later can reach only the
 * thread made for that. Does not return.
 */
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
 * first thread still blocking it. Returns 0, or 1 after the failure is
 * printed.
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

/** Run as a thread that sleeps until the process ends. */
static void *idle(void *arg)
{
    (void)arg;
    for (;;) {
        pause();
    }
    return NULL;
}

/**
 * Return the number of threads /proc/self/task lists, or -1 when it cannot
 * be opened.
 */
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
 * Return the link count of this process's task directory, read with fstatat
 * through a descriptor on /proc as Chromium's zygote reads it, or -1 when
 * the read fails.
 */
static long task_links(void)
{
    struct stat st;
    int proc = open("/proc", O_RDONLY | O_DIRECTORY);
    long n = -1;

    if (proc >= 0 && fstatat(proc, "self/task/", &st, 0) == 0) {
        n = (long)st.st_nlink;
    }
    if (proc >= 0) {
        close(proc);
    }
    return n;
}

/**
 * Check the paths of thread `t` under this process's task directory: its
 * directory lists no task directory of its own, a descriptor on it keeps
 * its path, for its link and for a name relative to it, whose stat is the
 * thread's own, the thread's descriptor links are there six components
 * deep, and a task path under another process, a child made for the row,
 * or under none, reaches nothing. Returns 0, or 1 after the failure is printed.
 */
static int task_paths(pid_t t)
{
    char dir[64], path[96], link[96];
    struct dirent *e;
    struct stat st;
    DIR *d;
    int dirfd, fd, err, task = 0;
    pid_t other;
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
    n = fd < 0 ? -1 : read(fd, link, sizeof(link) - 1);
    if (fd >= 0) {
        close(fd);
    }
    if (n <= 0 || (link[n] = '\0', strtol(link, NULL, 10) != (long)t)) {
        return fail("a name relative to a thread's directory");
    }
    snprintf(path, sizeof(path), "%s/fd/0", dir);
    if (lstat(path, &st) != 0 || !S_ISLNK(st.st_mode)) {
        return fail("a thread's descriptor link");
    }
    other = fork();
    if (other == 0) {
        pause();
        _exit(0);
    }
    snprintf(path, sizeof(path), "/proc/%d/task/%d/status", (int)other, (int)t);
    fd = open(path, O_RDONLY);
    err = errno;
    if (fd >= 0) {
        close(fd);
    }
    if (other > 0) {
        kill(other, SIGKILL);
        waitpid(other, NULL, 0);
    }
    if (other < 0 || fd != -1 || err != ENOENT) {
        return fail("a thread under another process's task directory");
    }
    snprintf(path, sizeof(path), "/proc/999999/task/%d/status", (int)t);
    if (open(path, O_RDONLY) != -1 || errno != ENOENT) {
        return fail("a task path under no process");
    }
    return 0;
}

/**
 * Return the sum of the user and system ticks, fields 14 and 15, of the stat
 * file `path`, or -1 when it cannot be read.
 */
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

/* The spinning thread's own time in ticks, before and after it spun. */
static long spun_from, spun_to;

/**
 * Run as the spinning thread: spin until this thread's own time, read from
 * its stat under the task directory, counts five ticks more, or for a
 * thousand ticks since boot. Stores the times in `spun_from` and `spun_to`.
 */
static void *spin_main(void *arg)
{
    char own_stat[64];
    struct tms tms;
    long deadline;
    volatile unsigned long spin = 0;
    int i;

    (void)arg;
    snprintf(own_stat, sizeof(own_stat), "/proc/self/task/%d/stat", (int)tid());
    spun_from = stat_ticks(own_stat);
    deadline = (long)times(&tms) + 1000;
    do {
        for (i = 0; i < 1000000; i++) {
            spin++;
        }
        spun_to = stat_ticks(own_stat);
    } while (spun_from >= 0 && spun_to >= 0 && spun_to < spun_from + 5 &&
             (long)times(&tms) < deadline);
    return NULL;
}

/**
 * Check that a thread's stat under the task directory has the thread's own
 * time: a
 * thread made for the row spins until its own counts five ticks more, while
 * thread `t` sleeps and gains none of them, and the process's stat has the
 * ticks the spinning thread spun, though it has ended by then, beside this
 * thread's own. Returns 0, or 1 after the failure is printed.
 */
static int thread_times(pid_t t)
{
    char other[64], first[64];
    pthread_t s;
    long rested, own;

    snprintf(other, sizeof(other), "/proc/self/task/%d/stat", (int)t);
    snprintf(first, sizeof(first), "/proc/self/task/%d/stat", (int)tid());
    rested = stat_ticks(other);
    if (pthread_create(&s, NULL, spin_main, NULL) != 0 || pthread_join(s, NULL) != 0) {
        return fail("the thread that spins");
    }
    if (spun_from < 0 || spun_to < spun_from + 5) {
        return fail("a busy thread's own time");
    }
    if (rested < 0 || stat_ticks(other) - rested >= spun_to - spun_from) {
        return fail("a sleeping thread's time in its task stat");
    }
    own = stat_ticks(first);
    if (own < 0 || stat_ticks("/proc/self/stat") < own + (spun_to - spun_from)) {
        return fail("the process's time in its stat");
    }
    return 0;
}

/* The descriptor the threads of `sharing` open and close. */
static int shared_fd = -1;

/** Run as a thread: open /dev/null into `shared_fd`. */
static void *open_file(void *arg)
{
    (void)arg;
    shared_fd = open("/dev/null", O_WRONLY);
    return NULL;
}

/** Run as a thread: close `shared_fd`. Returns close's result. */
static void *close_file(void *arg)
{
    (void)arg;
    return (void *)(long)close(shared_fd);
}

/**
 * Run as a thread: call execve on a program that is not there, which fails,
 * then open /dev/null into `shared_fd`. Returns 1 when the execve failed with
 * ENOENT, and 0 otherwise.
 */
static void *exec_missing(void *arg)
{
    int missing;

    (void)arg;
    execl("/musl/no-such-program", "no-such-program", (char *)0);
    missing = errno == ENOENT;
    shared_fd = open("/dev/null", O_WRONLY);
    return (void *)(long)missing;
}

/**
 * Run as a thread: set the umask to 077 and change to /tmp. Returns chdir's
 * result.
 */
static void *change_dir(void *arg)
{
    (void)arg;
    umask(077);
    return (void *)(long)chdir("/tmp");
}

/* The pipe of the read another thread closes, and the reader's thread id. */
static int pipe_fds[2];
static volatile pid_t reader_tid;

/**
 * Run as the reader: read one byte from `pipe_fds[0]`. Returns 1 when it read
 * 'x', and 0 otherwise.
 */
static void *read_pipe(void *arg)
{
    char c = 0;
    long n;

    (void)arg;
    reader_tid = tid();
    n = read(pipe_fds[0], &c, 1);
    return (void *)(n == 1 && c == 'x' ? 1L : 0L);
}

/**
 * Return 1 when thread `t` of this process is asleep, state S in
 * /proc/self/task/<t>/stat, and 0 otherwise.
 */
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
 * end the wait. Returns 0, or 1 after the failure is printed.
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

/**
 * Check the sharing of descriptors, the working directory and the umask,
 * /proc/self/exe, and a descriptor one thread closes while another reads it.
 * Returns 0, or 1 after the failure is printed.
 */
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

/* The thread ids of the two takers of `handler_mask`, and the threads that
 * ran the SIGUSR1 and SIGUSR2 handlers. */
static volatile pid_t takers[2];
static int slots[2] = {0, 1}; /* each taker's index in `takers` */
static volatile pid_t usr1_by, usr2_by;

/**
 * Handle SIGUSR1 for `handler_mask`: record the thread in `usr1_by` and wait
 * up to 10 s for SIGUSR2's handler to run in another thread. The handler's
 * mask blocks SIGUSR2.
 */
static void on_usr1_waits(int sig)
{
    int i;

    (void)sig;
    usr1_by = tid();
    for (i = 0; i < 1000 && !usr2_by; i++) {
        usleep(10000);
    }
}

/** Record in `usr2_by` the thread that ran SIGUSR2's handler. */
static void on_usr2(int sig)
{
    (void)sig;
    usr2_by = tid();
}

/**
 * Run as a thread of `handler_mask`, whose index in `takers` is at `arg`:
 * sleep with both signals unblocked until the two handlers have run. The
 * thread keeps the two blocked, as it was created, between its look at the
 * handlers and its sleep, and sigsuspend unblocks them and sleeps in one
 * step: a pause after the look would sleep for good when this thread's
 * handler ran between the two, before the other thread's.
 */
static void *take_signals(void *arg)
{
    sigset_t none;

    takers[*(const int *)arg] = tid();
    sigemptyset(&none);
    while (!usr1_by || !usr2_by) {
        sigsuspend(&none);
    }
    return NULL;
}

/**
 * Return 1 when thread `t` of this process is asleep with no signal blocked,
 * as /proc/self/task/<t>/status gives its state and mask, and 0 otherwise.
 * A thread of `handler_mask` blocks SIGUSR1 and SIGUSR2 from its creation, so
 * it is in that state only inside its sigsuspend.
 */
static int asleep_unblocked(pid_t t)
{
    char path[64], line[128], *v;
    int asleep = 0, unblocked = 0;
    FILE *f;

    snprintf(path, sizeof(path), "/proc/self/task/%d/status", (int)t);
    f = fopen(path, "r");
    if (!f) {
        return 0;
    }
    while (fgets(line, sizeof(line), f)) {
        if (!strncmp(line, "State:", 6)) {
            v = line + 6 + strspn(line + 6, " \t");
            asleep = *v == 'S';
        } else if (!strncmp(line, "SigBlk:", 7)) {
            v = line + 7 + strspn(line + 7, " \t");
            unblocked = strspn(v, "0") == 16;
        }
    }
    fclose(f);
    return asleep && unblocked;
}

/**
 * Two threads asleep in sigsuspend with SIGUSR1 and SIGUSR2 unblocked, while
 * the first thread blocks both, and the two signals sent to the process one
 * after the other, so that the first sleeper may be woken for both. SIGUSR1's
 * handler blocks SIGUSR2 and waits for SIGUSR2's handler, which the other
 * sleeper must be woken to run. The signals are sent only once both threads
 * are seen in sigsuspend (`asleep_unblocked`), so a thread that comes late
 * cannot pass for one that was not woken. Returns 0 when each thread ran one
 * handler, 2 when the two were not both in sigsuspend within 10 s and no
 * signal was sent, and 1 otherwise.
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
    for (i = 0; i < 1000 && !(takers[0] && asleep_unblocked(takers[0]) && takers[1] &&
                              asleep_unblocked(takers[1]));
         i++) {
        usleep(10000);
    }
    if (i == 1000) {
        return 2;
    }
    kill(getpid(), SIGUSR1);
    kill(getpid(), SIGUSR2);
    if (pthread_join(a, NULL) != 0 || pthread_join(b, NULL) != 0) {
        return 1;
    }
    return usr1_by != 0 && usr2_by != 0 && usr1_by != usr2_by ? 0 : 1;
}

/**
 * Print each thread of the process `pid` as /proc gives it: its state, the
 * signals pending for it and for its process, and its blocked mask, so that
 * a failed row says where a signal waits.
 */
static void say_threads(pid_t pid)
{
    char path[64], line[128];
    struct dirent *d;
    DIR *dir;
    FILE *f;

    snprintf(path, sizeof(path), "/proc/%d/task", (int)pid);
    dir = opendir(path);
    if (!dir) {
        return;
    }
    while ((d = readdir(dir)) != NULL) {
        if (d->d_name[0] == '.') {
            continue;
        }
        if (snprintf(path, sizeof(path), "/proc/%d/task/%s/status", (int)pid, d->d_name) >=
            (int)sizeof(path)) {
            continue;
        }
        f = fopen(path, "r");
        if (!f) {
            continue;
        }
        printf("THREADMUSL THREAD %s:", d->d_name);
        while (fgets(line, sizeof(line), f)) {
            if (!strncmp(line, "State:", 6) || !strncmp(line, "SigPnd:", 7) ||
                !strncmp(line, "ShdPnd:", 7) || !strncmp(line, "SigBlk:", 7)) {
                line[strcspn(line, "\n")] = '\0';
                printf(" %s", line);
            }
        }
        printf("\n");
        fclose(f);
    }
    closedir(dir);
}

/**
 * Run `handler_mask` in a child, where no other thread of the probe can take
 * the two signals, and judge it by the child's status; a child that does not
 * end in 30 s has its threads printed (`say_threads`) before it is killed.
 * Returns 0, or 1 after the failure is printed.
 */
static int handler_mask_row(void)
{
    int i, status = 0;
    pid_t pid = fork();

    if (pid == 0) {
        _exit(handler_mask());
    }
    for (i = 0; i < 3000 && waitpid(pid, &status, WNOHANG) == 0; i++) {
        usleep(10000);
    }
    if (i == 3000) {
        say_threads(pid);
        kill(pid, SIGKILL);
        waitpid(pid, &status, 0);
        return fail("a signal a handler's mask blocked did not wake the other thread");
    }
    if (WIFEXITED(status) && WEXITSTATUS(status) == 2) {
        return fail("the two threads were not seen asleep in sigsuspend");
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        return fail("the two signals were not taken by the two threads");
    }
    return 0;
}

/*
 * The waiter's thread id, and the page its status goes to, of
 * `lost_status_row`.
 */
static volatile pid_t waiter_tid;
static int *volatile status_page;

/**
 * Run as the thread of `lost_status_row` that waits for the child `*arg`
 * with its status in a page the first thread unmaps meanwhile. Returns the
 * errno of the wait, or 0 when it did not fail.
 */
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
 * Check a wait whose status page another thread unmaps while it sleeps: when
 * the child ends the wait cannot store the status, so it fails with EFAULT
 * and leaves the zombie, which a second wait reaps with its status.
 * Returns 0, or 1 after the failure is printed.
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
    if (i == 500) {
        return fail("the wait was never seen asleep before its status page went");
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

/*
 * The thread id of the socket call `lost_buffer_call` starts, and the page of
 * its buffer, which the first thread unmaps while the call sleeps.
 */
static volatile pid_t lost_tid;
static char *volatile lost_page;

/**
 * Run as the thread of `lost_buffer_call` that receives on the socket `*arg`
 * into `lost_page`. Returns the errno of the receive, or 0 when it did not
 * fail.
 */
static void *recv_lost(void *arg)
{
    lost_tid = tid();
    if (recv(*(int *)arg, lost_page, 4096, 0) != -1) {
        return (void *)0L;
    }
    return (void *)(long)errno;
}

/**
 * Run as the thread of `lost_buffer_call` that sends from `lost_page` on the
 * socket `*arg`. Returns the errno of the send, or 0 when it did not fail.
 */
static void *send_lost(void *arg)
{
    lost_tid = tid();
    if (send(*(int *)arg, lost_page, 4096, 0) != -1) {
        return (void *)0L;
    }
    return (void *)(long)errno;
}

/**
 * Start `body` on the socket `*fd` with its buffer in a page of its own,
 * wait until the thread is seen asleep, and unmap the page. Returns 0, or 1
 * after the failure is printed. A thread never seen asleep keeps its page,
 * since its call may not have begun, and is left for the exit to end.
 */
static int lost_buffer_call(pthread_t *t, void *(*body)(void *), int *fd)
{
    int i;

    lost_tid = 0;
    lost_page = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (lost_page == MAP_FAILED || pthread_create(t, NULL, body, fd) != 0) {
        return fail("setup for a socket call whose buffer goes");
    }
    for (i = 0; i < 500 && !(lost_tid && sleeping(lost_tid)); i++) {
        usleep(10000);
    }
    if (i == 500) {
        return fail("a socket call was never seen asleep before its buffer went");
    }
    if (munmap(lost_page, 4096) != 0) {
        return fail("munmap of a sleeping socket call's buffer");
    }
    return 0;
}

/** Whether the thread `t` returned the errno EFAULT. */
static int failed_efault(pthread_t t)
{
    void *result;

    return pthread_join(t, &result) == 0 && (long)result == EFAULT;
}

/**
 * Check a TCP receive and a TCP send over the loopback whose buffer another
 * thread unmaps while they sleep: each fails with EFAULT when it wakes, the
 * byte that woke the receive is left for the next one, and the send queues
 * nothing, so that what the peer reads is what was sent before it. Returns
 * 0, or 1 after the failure is printed.
 */
static int lost_tcp_rows(void)
{
    static char chunk[4096];
    struct sockaddr_in sa;
    pthread_t t;
    int ls = socket(AF_INET, SOCK_STREAM, 0);
    int cs = socket(AF_INET, SOCK_STREAM, 0);
    int as;
    long queued = 0, got = 0, n;
    char c = 0;

    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons(LOST_TCP_PORT);
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (ls < 0 || cs < 0 || bind(ls, (struct sockaddr *)&sa, sizeof(sa)) != 0 ||
        listen(ls, 1) != 0 || connect(cs, (struct sockaddr *)&sa, sizeof(sa)) != 0 ||
        (as = accept(ls, NULL, NULL)) < 0) {
        return fail("setup for a TCP connection over the loopback");
    }
    if (lost_buffer_call(&t, recv_lost, &cs) != 0) {
        return 1;
    }
    if (write(as, "x", 1) != 1 || !failed_efault(t)) {
        return fail("a TCP receive whose buffer went while it slept did not fail with EFAULT");
    }
    if (recv(cs, &c, 1, 0) != 1 || c != 'x') {
        return fail("the byte a failed TCP receive woke for was not left queued");
    }
    while ((n = send(cs, chunk, sizeof(chunk), MSG_DONTWAIT)) > 0 && queued < 4L << 20) {
        queued += n;
    }
    if (n != -1 || errno != EAGAIN) {
        return fail("setup: filling a TCP connection's send buffer");
    }
    if (lost_buffer_call(&t, send_lost, &cs) != 0) {
        return 1;
    }
    while (got < queued && (n = read(as, chunk, sizeof(chunk))) > 0) {
        got += n;
    }
    if (!failed_efault(t)) {
        return fail("a TCP send whose buffer went while it slept did not fail with EFAULT");
    }
    if (got != queued || recv(as, chunk, 1, MSG_DONTWAIT) != -1 || errno != EAGAIN) {
        return fail("a TCP send that failed with EFAULT queued bytes");
    }
    close(as);
    close(cs);
    close(ls);
    return 0;
}

/**
 * Check a UDP receive over the loopback whose buffer another thread unmaps
 * while it sleeps: it fails with EFAULT when the datagram comes, and the
 * datagram is left for the next receive. Returns 0, or 1 after the failure
 * is printed.
 */
static int lost_udp_row(void)
{
    struct sockaddr_in sa;
    pthread_t t;
    int us = socket(AF_INET, SOCK_DGRAM, 0);
    char c = 0;

    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons(LOST_UDP_PORT);
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (us < 0 || bind(us, (struct sockaddr *)&sa, sizeof(sa)) != 0) {
        return fail("setup for a UDP socket on the loopback");
    }
    if (lost_buffer_call(&t, recv_lost, &us) != 0) {
        return 1;
    }
    if (sendto(us, "y", 1, 0, (struct sockaddr *)&sa, sizeof(sa)) != 1 || !failed_efault(t)) {
        return fail("a UDP receive whose buffer went while it slept did not fail with EFAULT");
    }
    if (recv(us, &c, 1, MSG_DONTWAIT) != 1 || c != 'y') {
        return fail("the datagram a failed UDP receive woke for was not left queued");
    }
    close(us);
    return 0;
}

/* The errno of the step `use_framebuffer` failed at. */
static volatile int fb_errno;

/**
 * Empty the line discipline's queue of the console, which has O_NONBLOCK
 * set, reading lines and an end of file until it answers EAGAIN. Returns 0
 * then, or -1 when it does not within 64 reads.
 */
static int console_emptied(void)
{
    unsigned char line[256];
    int i;

    for (i = 0; i < 64; i++) {
        if (read(0, line, sizeof(line)) < 0) {
            return errno == EAGAIN ? 0 : -1;
        }
    }
    return -1;
}

/**
 * The thread of `framebuffer_row`, which is not the first. With O_NONBLOCK
 * on the console, empty its queue of lines while no process owns the
 * display; then open the framebuffer, set its palette, write a pixel, and
 * read the console, which for the display's owner reads raw scancodes and,
 * none typed, answers 0, where the line discipline, its queue empty, answers
 * EAGAIN; and close it. Returns 0, or the number of the step that failed,
 * with its errno in `fb_errno`.
 */
static void *use_framebuffer(void *arg)
{
    static unsigned char palette[768];
    unsigned char keys[16];
    long step = 0;
    int fd = -1;
    int flags = fcntl(0, F_GETFL);

    (void)arg;
    if (flags < 0 || fcntl(0, F_SETFL, flags | O_NONBLOCK) != 0 || console_emptied() != 0) {
        step = 1;
    } else if ((fd = open("/fb", O_RDWR)) < 0) {
        step = 2;
    } else if (ioctl(fd, FB_SETPALETTE, palette) != 0) {
        step = 3;
    } else if (write(fd, "\017", 1) != 1) {
        step = 4;
    } else if (read(0, keys, sizeof(keys)) != 0) {
        step = 5;
    }
    if (step != 0) {
        fb_errno = errno;
    }
    if (flags >= 0) {
        fcntl(0, F_SETFL, flags);
    }
    if (fd >= 0 && close(fd) != 0 && step == 0) {
        fb_errno = errno;
        step = 6;
    }
    return (void *)step;
}

/**
 * Check the framebuffer, opened, used and closed by a thread that is not the
 * first: the display is the process's, so the thread's palette and pixel are
 * taken, its read of the console is the owner's, and its close gives the
 * display back, which a child process then opens. Returns 0, or 1 after the
 * failure is printed.
 */
static int framebuffer_row(void)
{
    static const char *const steps[] = {"the console's queue of lines could not be emptied",
                                        "a thread's open of the framebuffer",
                                        "a thread's palette on the framebuffer it opened",
                                        "a thread's pixel on the framebuffer it opened",
                                        "a thread that owns the framebuffer reads raw scancodes",
                                        "a thread's close of the framebuffer"};
    pthread_t t;
    void *result;
    pid_t child;
    int status;

    if (pthread_create(&t, NULL, use_framebuffer, NULL) != 0 || pthread_join(t, &result) != 0) {
        return fail("a thread for the framebuffer");
    }
    if ((long)result != 0) {
        errno = fb_errno;
        return fail(steps[(long)result - 1]);
    }
    child = fork();
    if (child == 0) {
        int fd = open("/fb", O_RDWR);

        _exit(fd >= 0 && close(fd) == 0 ? 0 : 1);
    }
    if (child < 0 || waitpid(child, &status, 0) != child || !WIFEXITED(status) ||
        WEXITSTATUS(status) != 0) {
        return fail("the display a thread's close gave back");
    }
    return 0;
}

/**
 * Run the rows that follow the first ones: what the threads share, a signal a
 * handler's mask blocks, a wait whose status page goes, the socket calls whose
 * buffers go, and the framebuffer a thread that is not the first uses. Returns
 * 0 when all pass, and 1 after a failure is printed. They are one call so that
 * `main` stays within the complexity tools/verify/sizes.reasons allows it.
 */
static int later_rows(void)
{
    return sharing() != 0 || handler_mask_row() != 0 || lost_status_row() != 0 ||
           lost_tcp_rows() != 0 || lost_udp_row() != 0 || framebuffer_row() != 0;
}

/**
 * Run as a thread: exec a shell that exits 9. Returns only when the exec fails.
 */
static void *exec_self(void *arg)
{
    (void)arg;
    execl("/bin/sh", "sh", "-c", "exit 9", (char *)0);
    return NULL;
}

/** Run as a thread: end the process with exit status 8. */
static void *exit_all(void *arg)
{
    (void)arg;
    exit(8);
}

/* The pipe the second thread of `two_execs` sends its thread id on. */
static int tid_pipe[2];

/**
 * Run as the second thread of `two_execs`: make the probe its tracer, send
 * its id, stop, and then call execve. Does not return.
 */
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

/**
 * Do nothing, so that SIGALRM interrupts a blocked call and does not end the
 * process.
 */
static void on_alarm(int sig)
{
    (void)sig;
}

/* The process-shared robust mutex of the robust row, in shared memory. */
static pthread_mutex_t *robust;

/**
 * Run as a thread: lock the robust mutex and keep it into an execve. Does not
 * return.
 */
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
 * Check that a robust mutex that a thread other than the first holds when it
 * calls execve is released under the id the thread locked it with, before the
 * thread takes the process id, so the probe's lock after the child's end gets
 * EOWNERDEAD and the mutex. A release that looked for the process id would
 * leave the mutex held for good; a time limit bounds the lock. Returns 0, or 1
 * after the failure is printed.
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
 * SIGKILL ends the stop, and the second call does not run, as on Linux: its
 * thread ends, and the child runs the first one's program. An alarm bounds
 * the waits, since a second call that ran and waited for the first would
 * never end. Returns 0, or 1 after the failure is printed.
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
    if (task_links() != 4) {
        return fail("/proc/self/task of two threads did not have four links");
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
        _exit(count_tasks() != 1 ? 1 : task_links() != 3 ? 2 : 0);
    }
    if (waitpid(pid, &status, 0) != pid || !WIFEXITED(status) || WEXITSTATUS(status) == 1) {
        return fail("a child forked by a thread did not have one thread");
    }
    if (WEXITSTATUS(status) != 0) {
        return fail("/proc/self/task of one thread did not have three links");
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
