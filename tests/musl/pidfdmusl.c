/**
 * pidfdmusl: check pidfds, clone3's CLONE_PIDFD and CLONE_CLEAR_SIGHAND, the
 * page fault counts and MAP_POPULATE, through musl and raw system calls.
 *
 * The rows, each judged by what the kernel reports:
 *
 * clone3 with CLONE_CLEAR_SIGHAND, as glibc's posix_spawn passes it, makes a
 * child whose caught signal is back at SIG_DFL and whose ignored one stays
 * ignored, in one call; with CLONE_SIGHAND it is EINVAL.
 *
 * clone3 with CLONE_PIDFD stores a close-on-exec pidfd of the child. poll
 * finds it readable once the child has ended, waitid with P_PIDFD reports
 * and reaps the child's exit, and poll then finds it hung up too.
 *
 * pidfd_open gives a pidfd of a child that waits in pause: poll finds it not
 * readable, pidfd_send_signal kills the child, poll then finds it readable,
 * and waitid with P_PIDFD reports the death by SIGKILL; once the child is
 * reaped, pidfd_send_signal is ESRCH. waitid on a non-blocking pidfd of a
 * child that has not ended is EAGAIN.
 *
 * pidfd_getfd copies the caller's own pipe's write end, and a byte written
 * through the copy is read from the pipe; a descriptor that is not open is
 * EBADF and a flag is EINVAL. pidfd_open of pid 0 or with an unknown flag is
 * EINVAL, a read of a pidfd is EINVAL, and pidfd_send_signal on a
 * descriptor that is not a pidfd is EBADF.
 *
 * A sibling blocks in poll with no requested events on an unreaped child's
 * pidfd. Reaping the child wakes it with POLLHUP. Each queued-signal call
 * copies siginfo from a file page not yet faulted into the caller's mapping;
 * the signal retains its code, sender, errno and value. rt_sigqueueinfo
 * and rt_tgsigqueueinfo reject an unreadable siginfo before a missing
 * target, and pidfd_send_signal a descriptor that is not open before the
 * siginfo, as Linux 6.12 orders them; kernel codes stay within a process.
 *
 * pidfd_send_signal on the program's own pidfd with PIDFD_SIGNAL_THREAD
 * posts a blocked SIGUSR1 to the leading thread alone, which a new thread
 * does not find pending, with SI_TKILL; with PIDFD_SIGNAL_THREAD_GROUP it
 * posts it to the process, which a new thread finds pending, with SI_USER.
 * With PIDFD_SIGNAL_PROCESS_GROUP, SIGKILL by the pidfd of a child that
 * leads a group of two kills both; the pidfd of the member, which leads no
 * group, is ESRCH, and SI_USER by the program's own pidfd is EPERM. Once
 * the leader is killed and reaped, its pidfd is ESRCH without a flag but
 * still kills the member with the process-group scope, and is ESRCH with
 * it once the group is empty. Once a lone leader is reaped and a new child,
 * which /proc/sys/kernel/ns_last_pid makes take its pid, leads a group of
 * that number, the leader's pidfd is ESRCH with that scope and the child
 * lives, as Linux tells the two groups apart. A child in new user and PID
 * namespaces is refused with EINVAL by the pidfd of a process outside
 * them, and acknowledges that check before the process is reaped, and
 * again after. tkill and tgkill check the ids, then find the thread, and
 * only then check the signal: a signal out of range to no such thread, or
 * to a thread of another process, is ESRCH, and to this thread EINVAL.
 *
 * pidfd_open with PIDFD_THREAD gives a pidfd of a second thread and of the
 * leading one, whose F_GETFL is O_RDWR with O_EXCL, Linux's PIDFD_THREAD,
 * which a pidfd of the process lacks; without the flag the second thread is
 * EINVAL, as Linux 6.12 refuses a thread that does not lead its process. By
 * the second thread's pidfd, pidfd_getfd copies the thread's descriptor of
 * a pipe's write end, close-on-exec, and a byte written through the copy
 * is read from the pipe, PIDFD_SIGNAL_PROCESS_GROUP is ESRCH and waitid
 * with P_PIDFD ECHILD. pidfd_send_signal with no flag on it
 * posts a blocked SIGUSR1 to that thread alone, which the caller does not
 * find pending and the thread takes with SI_TKILL; on the leading thread's,
 * to the leading thread alone with SI_TKILL, and with
 * PIDFD_SIGNAL_THREAD_GROUP to the process with SI_USER. The second
 * thread's pidfd does not poll readable while it runs; once it has ended it
 * polls readable and hung up, while the process's own pidfd does not, and
 * pidfd_open of its id and pidfd_send_signal on its pidfd are ESRCH. A
 * child whose leading thread ends while another runs has a thread pidfd of
 * that leader that polls readable and not hung up, and a process pidfd that
 * does not poll readable until the child is killed; once it is reaped both
 * poll hung up. Once a thread has ended and a new thread is given its id
 * through ns_last_pid, the ended thread's pidfd is ESRCH to
 * pidfd_send_signal and polls readable and hung up. An edge-triggered
 * epoll of a thread's pidfd reports its end once, and a sibling's end,
 * which its own pidfd polls readable after, gives it no second event.
 * By the pidfd of a child's thread that the program traces, pidfd_getfd
 * copies a descriptor while the thread runs, and is ESRCH once it has ended
 * and released its descriptors but is not yet reaped, as Linux's
 * __pidfd_fget answers a task past exit_files().
 *
 * A child that led a group, left it for its parent's while a member of its
 * own stayed in it, is refused setsid with EPERM, and is let make its
 * session once the member is reaped, as Linux refuses while any process
 * has the caller's id as its group. In a new PID namespace, a write of
 * 40000 to ns_last_pid makes the next child 40001, above Linux's old
 * 32768, as the namespace allocates up to pid_max; a descriptor of
 * ns_last_pid opened in the first namespace and read in the new one reads
 * 40001, and one opened before a fork reads the fork's number, as each read
 * gives the reader's namespace at that read; with CAP_CHECKPOINT_RESTORE
 * alone the write is taken, and with no capability it is EPERM. With
 * /proc/sys/kernel/pid_max at 301, its least, forks come to EAGAIN within
 * 301 tries, and fork works again once pid_max is back.
 *
 * Touching 64 new pages raises getrusage's ru_minflt by at least 64; a
 * mapping of 64 pages made with MAP_POPULATE raises it by at least 64 in the
 * mmap call and by fewer than 64 when they are then written. A child that
 * touches 64 pages reports at least 64 faults to wait4, and the children's
 * count of getrusage rises by exactly what wait4 reported.
 *
 * It prints "PIDFDMUSL OK" and exits 0 on success, or prints "PIDFDMUSL FAIL
 * (detail)" and exits 1 at the first failure. etude's report
 * (probe_report.h), for the case ETUDE_CASE names, has an assertion for each
 * row in order: `clear-sighand`, `clear-sighand-einval`, `clone3-pidfd`,
 * `pidfd-readable`, `waitid-pidfd`, `pidfd-hangup`, `pidfd-open`,
 * `send-signal`, `killed`, `send-reaped`, `nonblock-eagain`, `getfd`,
 * `getfd-errors`, `pidfd-errors`, `self-faults`, `populate`, `wait4-faults`
 * and `children-faults`, followed by `hangup-wakes`, `pidfd-file-info`,
 * `process-file-info`, `thread-file-info`, `info-before-target`,
 * `queued-code-permission`, `scope-thread`, `scope-thread-group`,
 * `scope-process-group`, `scope-group-reaped`, `scope-group-reused`,
 * `reaped-namespace`, `tkill-order`, `thread-open`, `thread-refusals`,
 * `thread-signal`, `leader-thread-signal`, `thread-poll`,
 * `leader-thread-poll`, `thread-id-reused`, `thread-edge`,
 * `traced-zombie-getfd`, `setsid-left-group`, `last-pid-above`,
 * `last-pid-read`, `last-pid-cap` and `fork-eagain`. The first failure
 * fails its assertion and leaves the rest not run. The program runs as
 * root, which the writes of ns_last_pid and pid_max need.
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <sched.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/ptrace.h>
#include <sys/epoll.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#include "../probe_report.h"

#ifndef SYS_pidfd_send_signal
#define SYS_pidfd_send_signal 424
#endif
#ifndef SYS_pidfd_open
#define SYS_pidfd_open 434
#endif
#ifndef SYS_clone3
#define SYS_clone3 435
#endif
#ifndef SYS_pidfd_getfd
#define SYS_pidfd_getfd 438
#endif
#ifndef P_PIDFD
#define P_PIDFD 3
#endif

#define CLONE_PIDFD_FLAG    0x00001000ull
#define CLONE_SIGHAND_FLAG  0x00000800ull
#define CLONE_VM_FLAG       0x00000100ull
#define CLONE_CLEAR_SIGHAND 0x100000000ull
#define PIDFD_NONBLOCK      O_NONBLOCK
#define PIDFD_THREAD        O_EXCL

/* pidfd_send_signal's scopes since Linux 6.9: the pidfd's thread, its
 * process, and the process group whose id is the process's. */
#define PIDFD_SIGNAL_THREAD        1L
#define PIDFD_SIGNAL_THREAD_GROUP  2L
#define PIDFD_SIGNAL_PROCESS_GROUP 4L

/* Namespaces for clone, which an older musl may not name. */
#define CLONE_NEWUSER_FLAG 0x10000000L
#define CLONE_NEWPID_FLAG  0x20000000L

/* _LINUX_CAPABILITY_VERSION_3, whose data is two words of each set, and
 * CAP_CHECKPOINT_RESTORE, bit 8 of the second. */
#define CAP_VERSION_3          0x20080522u
#define CAP_CHECKPOINT_RESTORE 40

/* The pages the fault rows touch. */
#define PAGES 64

/* Linux's struct clone_args, the eight fields of its first version. */
struct clone_args64 {
    uint64_t flags;
    uint64_t pidfd;
    uint64_t child_tid;
    uint64_t parent_tid;
    uint64_t exit_signal;
    uint64_t stack;
    uint64_t stack_size;
    uint64_t tls;
};

/** Report one failed check and return 1, the probe's failure status. */
static int fail(const char *what)
{
    printf("PIDFDMUSL FAIL (%s)\n", what);
    report_stop(what);
    return 1;
}

/** clone3 with `flags` and SIGCHLD, as fork makes a child, storing a pidfd
 * at `pidfd` under CLONE_PIDFD: the child's pid, 0 in the child, or -1. */
static long clone3_fork(uint64_t flags, int *pidfd)
{
    struct clone_args64 a;

    memset(&a, 0, sizeof(a));
    a.flags = flags;
    a.pidfd = (uint64_t)(uintptr_t)pidfd;
    a.exit_signal = SIGCHLD;
    return syscall(SYS_clone3, &a, sizeof(a));
}

static void on_usr1(int sig)
{
    (void)sig;
}

/** The minor faults getrusage reports of `who`. */
static long minflt(int who)
{
    struct rusage ru;

    return getrusage(who, &ru) == 0 ? ru.ru_minflt : -1;
}

/** Write a byte to each of `PAGES` pages at `p`. */
static void touch(volatile char *p)
{
    int i;

    for (i = 0; i < PAGES; i++) {
        p[i * 4096] = (char)i;
    }
}

/** Whether `fd` polls with `events` among its events now, or after a wait
 * when `wait` is set. */
static int polls(int fd, short events, int wait)
{
    struct pollfd p;

    p.fd = fd;
    p.events = POLLIN;
    p.revents = 0;
    return poll(&p, 1, wait ? -1 : 0) >= 0 && (p.revents & events) == events;
}

/** CLONE_CLEAR_SIGHAND and CLONE_PIDFD through clone3. */
static int clones(void)
{
    struct sigaction sa;
    siginfo_t si;
    long pid;
    int status;
    int pidfd = -1;

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_usr1;
    sigaction(SIGUSR1, &sa, NULL);
    signal(SIGUSR2, SIG_IGN);
    pid = clone3_fork(CLONE_CLEAR_SIGHAND, NULL);
    if (pid == 0) {
        struct sigaction one;
        struct sigaction two;

        sigaction(SIGUSR1, NULL, &one);
        sigaction(SIGUSR2, NULL, &two);
        _exit(one.sa_handler == SIG_DFL && two.sa_handler == SIG_IGN ? 0 : 1);
    }
    if (pid < 0 || waitpid((pid_t)pid, &status, 0) != pid || !WIFEXITED(status) ||
        WEXITSTATUS(status) != 0) {
        return fail("clone3 with CLONE_CLEAR_SIGHAND did not reset the caught handler alone");
    }
    report_held();
    if (clone3_fork(CLONE_CLEAR_SIGHAND | CLONE_SIGHAND_FLAG | CLONE_VM_FLAG, NULL) != -1 ||
        errno != EINVAL) {
        return fail("CLONE_CLEAR_SIGHAND with CLONE_SIGHAND was not refused with EINVAL");
    }
    report_held();

    pid = clone3_fork(CLONE_PIDFD_FLAG, &pidfd);
    if (pid == 0) {
        _exit(7);
    }
    if (pid < 0 || pidfd < 0 || !(fcntl(pidfd, F_GETFD) & FD_CLOEXEC)) {
        return fail("clone3 with CLONE_PIDFD gave no close-on-exec pidfd");
    }
    report_held();
    if (!polls(pidfd, POLLIN, 1)) {
        return fail("the pidfd did not poll readable at the child's exit");
    }
    report_held();
    memset(&si, 0, sizeof(si));
    if (waitid(P_PIDFD, (id_t)pidfd, &si, WEXITED) != 0 || si.si_pid != pid ||
        si.si_code != CLD_EXITED || si.si_status != 7) {
        return fail("waitid with P_PIDFD did not report the child's exit");
    }
    report_held();
    if (!polls(pidfd, POLLIN | POLLHUP, 0)) {
        return fail("the pidfd of a reaped child did not poll hung up");
    }
    close(pidfd);
    report_held();
    return 0;
}

/** A child that waits in pause until a signal ends it. */
static pid_t pausing_child(void)
{
    pid_t pid = fork();

    if (pid == 0) {
        for (;;) {
            pause();
        }
    }
    return pid;
}

/** pidfd_open, pidfd_send_signal and a non-blocking pidfd's waitid. */
static int opens(void)
{
    siginfo_t si;
    pid_t pid = pausing_child();
    int pidfd;

    pidfd = pid > 0 ? (int)syscall(SYS_pidfd_open, (long)pid, 0L) : -1;
    if (pidfd < 0 || polls(pidfd, POLLIN, 0)) {
        return fail("pidfd_open gave no pidfd, or one readable while the child runs");
    }
    report_held();
    if (syscall(SYS_pidfd_send_signal, (long)pidfd, (long)SIGKILL, 0L, 0L) != 0 ||
        !polls(pidfd, POLLIN, 1)) {
        return fail("pidfd_send_signal did not end the child");
    }
    report_held();
    memset(&si, 0, sizeof(si));
    if (waitid(P_PIDFD, (id_t)pidfd, &si, WEXITED) != 0 || si.si_pid != pid ||
        si.si_code != CLD_KILLED || si.si_status != SIGKILL) {
        return fail("waitid with P_PIDFD did not report the death by SIGKILL");
    }
    report_held();
    if (syscall(SYS_pidfd_send_signal, (long)pidfd, (long)SIGTERM, 0L, 0L) != -1 ||
        errno != ESRCH) {
        return fail("pidfd_send_signal to a reaped child was not ESRCH");
    }
    close(pidfd);
    report_held();

    pid = pausing_child();
    pidfd = pid > 0 ? (int)syscall(SYS_pidfd_open, (long)pid, (long)PIDFD_NONBLOCK) : -1;
    if (pidfd < 0 || waitid(P_PIDFD, (id_t)pidfd, &si, WEXITED) != -1 || errno != EAGAIN) {
        return fail("waitid on a non-blocking pidfd of a running child was not EAGAIN");
    }
    kill(pid, SIGKILL);
    waitpid(pid, NULL, 0);
    close(pidfd);
    report_held();
    return 0;
}

/** pidfd_getfd and the refusals. */
static int getfds(void)
{
    char c = 0;
    int pipefd[2];
    int pidfd = (int)syscall(SYS_pidfd_open, (long)getpid(), 0L);
    int copy;

    if (pidfd < 0 || pipe(pipefd) != 0) {
        return fail("pidfd_open of the caller or pipe failed");
    }
    copy = (int)syscall(SYS_pidfd_getfd, (long)pidfd, (long)pipefd[1], 0L);
    if (copy < 0 || !(fcntl(copy, F_GETFD) & FD_CLOEXEC) || write(copy, "x", 1) != 1 ||
        read(pipefd[0], &c, 1) != 1 || c != 'x') {
        return fail("pidfd_getfd did not copy the pipe's write end");
    }
    report_held();
    if (syscall(SYS_pidfd_getfd, (long)pidfd, 999L, 0L) != -1 || errno != EBADF ||
        syscall(SYS_pidfd_getfd, (long)pidfd, (long)pipefd[1], 1L) != -1 || errno != EINVAL) {
        return fail("pidfd_getfd did not refuse a closed descriptor or a flag");
    }
    report_held();
    if (syscall(SYS_pidfd_open, 0L, 0L) != -1 || errno != EINVAL ||
        syscall(SYS_pidfd_open, (long)getpid(), 1L) != -1 || errno != EINVAL ||
        read(pidfd, &c, 1) != -1 || errno != EINVAL ||
        syscall(SYS_pidfd_send_signal, (long)pipefd[0], 0L, 0L, 0L) != -1 || errno != EBADF) {
        return fail("a bad pid, a flag, a read or a descriptor not a pidfd was not refused");
    }
    report_held();
    close(copy);
    close(pipefd[0]);
    close(pipefd[1]);
    close(pidfd);
    return 0;
}

/** The fault counts and MAP_POPULATE. */
static int faults(void)
{
    struct rusage ru;
    char *p;
    long before, mapped, after;
    pid_t pid;
    int status;

    p = mmap(NULL, PAGES * 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    before = minflt(RUSAGE_SELF);
    touch(p);
    after = minflt(RUSAGE_SELF);
    if (p == MAP_FAILED || before < 0 || after - before < PAGES) {
        return fail("touching new pages did not count a minor fault each");
    }
    munmap(p, PAGES * 4096);
    report_held();

    before = minflt(RUSAGE_SELF);
    p = mmap(NULL, PAGES * 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE,
             -1, 0);
    mapped = minflt(RUSAGE_SELF);
    touch(p);
    after = minflt(RUSAGE_SELF);
    if (p == MAP_FAILED || mapped - before < PAGES || after - mapped >= PAGES) {
        return fail("MAP_POPULATE did not back the pages in the mmap call");
    }
    munmap(p, PAGES * 4096);
    report_held();

    before = minflt(RUSAGE_CHILDREN);
    pid = fork();
    if (pid == 0) {
        p = mmap(NULL, PAGES * 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (p != MAP_FAILED) {
            touch(p);
        }
        _exit(0);
    }
    memset(&ru, 0, sizeof(ru));
    if (pid < 0 || wait4(pid, &status, 0, &ru) != pid || ru.ru_minflt < PAGES) {
        return fail("wait4 did not report the child's faults");
    }
    report_held();
    if (minflt(RUSAGE_CHILDREN) - before != ru.ru_minflt) {
        return fail("the children's faults did not rise by what wait4 reported");
    }
    report_held();
    return 0;
}

/** Read a child's state from procfs, or return 0 when it cannot be read. */
static char child_state(pid_t pid)
{
    char path[64];
    char buf[512];
    char *end;
    int fd;
    ssize_t n;

    snprintf(path, sizeof(path), "/proc/%d/stat", (int)pid);
    fd = open(path, O_RDONLY);
    if (fd < 0) {
        return 0;
    }
    n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) {
        return 0;
    }
    buf[n] = 0;
    end = strrchr(buf, ')');
    return end && end[1] == ' ' ? end[2] : 0;
}

/** Reap an exited child while its sibling is already waiting for POLLHUP.
 * All children and descriptors are released on success or failure. */
static int hangup_wakes(void)
{
    pid_t target = -1, monitor = -1;
    int pidfd = -1, ready[2] = {-1, -1};
    int status, ok = 0;
    char byte, state;
    siginfo_t si;

    if (pipe(ready) != 0) {
        goto out;
    }
    target = fork();
    if (target == 0) {
        _exit(0);
    }
    if (target < 0) {
        goto out;
    }
    pidfd = (int)syscall(SYS_pidfd_open, (long)target, 0L);
    if (pidfd < 0 || waitid(P_PID, (id_t)target, &si, WEXITED | WNOWAIT) != 0) {
        goto out;
    }
    monitor = fork();
    if (monitor == 0) {
        struct pollfd p;
        int rc;

        close(ready[0]);
        if (write(ready[1], "r", 1) != 1) {
            _exit(1);
        }
        close(ready[1]);
        p.fd = pidfd;
        p.events = 0;
        p.revents = 0;
        do {
            rc = poll(&p, 1, -1);
        } while (rc < 0 && errno == EINTR);
        _exit(rc == 1 && (p.revents & POLLHUP) ? 0 : 1);
    }
    if (monitor < 0) {
        goto out;
    }
    close(ready[1]);
    ready[1] = -1;
    if (read(ready[0], &byte, 1) != 1) {
        goto out;
    }
    do {
        state = child_state(monitor);
        if (state == 0 || state == 'Z') {
            goto out;
        }
        sched_yield();
    } while (state != 'S');
    if (waitpid(target, &status, 0) != target) {
        goto out;
    }
    target = -1;
    if (waitpid(monitor, &status, 0) != monitor) {
        goto out;
    }
    monitor = -1;
    ok = WIFEXITED(status) && WEXITSTATUS(status) == 0;
out:
    if (monitor > 0) {
        kill(monitor, SIGKILL);
        waitpid(monitor, NULL, 0);
    }
    if (target > 0) {
        kill(target, SIGKILL);
        waitpid(target, NULL, 0);
    }
    if (pidfd >= 0) {
        close(pidfd);
    }
    if (ready[0] >= 0) {
        close(ready[0]);
    }
    if (ready[1] >= 0) {
        close(ready[1]);
    }
    if (!ok) {
        return fail("a pidfd waiter did not wake when another process reaped its target");
    }
    report_held();
    return 0;
}

/** Return the count of faults that read a file, or -1 on failure. */
static long major_faults(void)
{
    struct rusage ru;

    return getrusage(RUSAGE_SELF, &ru) == 0 ? ru.ru_majflt : -1;
}

/** Send queued siginfo through each call, faulting a fresh file page.
 * The temporary file is unlinked before use; mappings, descriptors and the
 * signal mask are restored on every return. */
static int file_info(void)
{
    const char *tmp = getenv("TMPDIR");
    char path[512] = {0};
    siginfo_t si, got;
    sigset_t set, old;
    struct timespec zero = {0, 0};
    char *mapping = MAP_FAILED;
    const char *error = "could not prepare a queued siginfo file mapping";
    int fd = -1, pidfd = -1, i;
    long before, rc;
    pid_t pid = getpid();

    sigemptyset(&set);
    sigaddset(&set, SIGUSR1);
    if (sigprocmask(SIG_BLOCK, &set, &old) != 0) {
        return fail(error);
    }
    if (snprintf(path, sizeof(path), "%s/pidfd-info.XXXXXX", tmp ? tmp : "/tmp") >=
        (int)sizeof(path)) {
        goto out;
    }
    fd = mkstemp(path);
    if (fd < 0) {
        goto out;
    }
    if (unlink(path) != 0) {
        goto out;
    }
    path[0] = 0;
    memset(&si, 0, sizeof(si));
    si.si_signo = SIGUSR1;
    si.si_code = SI_QUEUE;
    si.si_errno = 37;
    si.si_pid = pid;
    si.si_uid = getuid();
    if (ftruncate(fd, 3 * 4096) != 0) {
        goto out;
    }
    for (i = 0; i < 3; i++) {
        si.si_value.sival_int = 100 + i;
        if (pwrite(fd, &si, sizeof(si), i * 4096) != (ssize_t)sizeof(si)) {
            goto out;
        }
    }
    pidfd = (int)syscall(SYS_pidfd_open, (long)pid, 0L);
    mapping = mmap(NULL, 3 * 4096, PROT_READ, MAP_PRIVATE, fd, 0);
    if (pidfd < 0 || mapping == MAP_FAILED) {
        goto out;
    }
    for (i = 0; i < 3; i++) {
        before = major_faults();
        if (i == 0) {
            rc = syscall(SYS_pidfd_send_signal, (long)pidfd, (long)SIGUSR1, mapping + i * 4096, 0L);
        } else if (i == 1) {
            rc = syscall(SYS_rt_sigqueueinfo, (long)pid, (long)SIGUSR1, mapping + i * 4096);
        } else {
            rc = syscall(SYS_rt_tgsigqueueinfo, (long)pid, syscall(SYS_gettid), (long)SIGUSR1,
                         mapping + i * 4096);
        }
        error = "queued siginfo did not fault its file page or retain its fields";
        if (rc != 0 || before < 0 || major_faults() <= before ||
            sigtimedwait(&set, &got, &zero) != SIGUSR1 || got.si_code != SI_QUEUE ||
            got.si_errno != 37 || got.si_pid != pid || got.si_uid != getuid() ||
            got.si_value.sival_int != 100 + i) {
            goto out;
        }
        report_held();
    }
    error = NULL;
out:
    if (mapping != MAP_FAILED) {
        munmap(mapping, 3 * 4096);
    }
    if (pidfd >= 0) {
        close(pidfd);
    }
    if (fd >= 0) {
        close(fd);
    }
    if (fd >= 0 && path[0]) {
        unlink(path);
    }
    sigprocmask(SIG_SETMASK, &old, NULL);
    return error ? fail(error) : 0;
}

/** Check siginfo copy ordering, EBADF for pidfd_send_signal on no descriptor
 * before its unreadable siginfo and EFAULT for the queueing calls to no such
 * process, and preserve the restriction on kernel codes. A child used as the
 * foreign target is killed and reaped on every path. */
static int queued_checks(void)
{
    pid_t pid;
    int pidfd, ok;
    siginfo_t si;

    if (syscall(SYS_pidfd_send_signal, -1L, (long)SIGUSR1, (void *)1, 0L) != -1 || errno != EBADF ||
        syscall(SYS_rt_sigqueueinfo, 2000000000L, (long)SIGUSR1, (void *)1) != -1 ||
        errno != EFAULT ||
        syscall(SYS_rt_tgsigqueueinfo, 2000000000L, 2000000000L, (long)SIGUSR1, (void *)1) != -1 ||
        errno != EFAULT) {
        return fail("queued siginfo was not copied in Linux's order");
    }
    report_held();
    pid = pausing_child();
    if (pid < 0) {
        return fail("could not fork a queued signal target");
    }
    pidfd = (int)syscall(SYS_pidfd_open, (long)pid, 0L);
    memset(&si, 0, sizeof(si));
    si.si_signo = SIGUSR1;
    si.si_code = SI_USER;
    ok = pidfd >= 0 && syscall(SYS_pidfd_send_signal, (long)pidfd, (long)SIGUSR1, &si, 0L) == -1 &&
         errno == EPERM && syscall(SYS_rt_sigqueueinfo, (long)pid, (long)SIGUSR1, &si) == -1 &&
         errno == EPERM &&
         syscall(SYS_rt_tgsigqueueinfo, (long)pid, (long)pid, (long)SIGUSR1, &si) == -1 &&
         errno == EPERM;
    kill(pid, SIGKILL);
    waitpid(pid, NULL, 0);
    if (pidfd >= 0) {
        close(pidfd);
    }
    if (!ok) {
        return fail("a queued kernel code was not refused for another process");
    }
    report_held();
    return 0;
}

/** The start of a thread that tells whether SIGUSR1, which it blocks as
 * its creator does, is pending for it: 1 or 0 in the int at `arg`. */
static void *pending_here(void *arg)
{
    sigset_t set;

    *(int *)arg = sigpending(&set) == 0 ? sigismember(&set, SIGUSR1) : -1;
    return NULL;
}

/** Whether a new thread of this process finds SIGUSR1 pending, sent to the
 * process or to it: 1 or 0, or -1 when it could not tell. */
static int sibling_pending(void)
{
    pthread_t t;
    int has = -1;

    if (pthread_create(&t, NULL, pending_here, &has) != 0 || pthread_join(t, NULL) != 0) {
        return -1;
    }
    return has;
}

/** Send this process SIGUSR1, blocked, by its own pidfd `self` with the
 * scope `flags`, and take it back: whether a new thread found it pending
 * as `shared` says and it came with the code `code` and this process's
 * pid. */
static int scoped_to_self(int self, long flags, int shared, int code)
{
    struct timespec zero = {0, 0};
    siginfo_t si;
    sigset_t set;

    sigemptyset(&set);
    sigaddset(&set, SIGUSR1);
    memset(&si, 0, sizeof(si));
    return syscall(SYS_pidfd_send_signal, (long)self, (long)SIGUSR1, NULL, flags) == 0 &&
           sibling_pending() == shared && sigtimedwait(&set, &si, &zero) == SIGUSR1 &&
           si.si_code == code && si.si_pid == getpid();
}

/** pidfd_send_signal's thread and thread-group scopes on this process's
 * own pidfd. */
static int scopes(void)
{
    sigset_t set, old;
    int self = (int)syscall(SYS_pidfd_open, (long)getpid(), 0L);
    int thread, group;

    sigemptyset(&set);
    sigaddset(&set, SIGUSR1);
    if (self < 0 || sigprocmask(SIG_BLOCK, &set, &old) != 0) {
        return fail("pidfd_open of this process");
    }
    thread = scoped_to_self(self, PIDFD_SIGNAL_THREAD, 0, SI_TKILL);
    group = thread && scoped_to_self(self, PIDFD_SIGNAL_THREAD_GROUP, 1, SI_USER);
    sigprocmask(SIG_SETMASK, &old, NULL);
    close(self);
    if (!thread) {
        return fail("PIDFD_SIGNAL_THREAD did not signal the leading thread alone with SI_TKILL");
    }
    report_held();
    if (!group) {
        return fail("PIDFD_SIGNAL_THREAD_GROUP did not signal the process with SI_USER");
    }
    report_held();
    return 0;
}

/** Two pausing children in one process group, which the first leads, in
 * `*lead` and `*member`: 0, or -1 with neither left running. */
static int pausing_group(pid_t *lead, pid_t *member)
{
    *lead = pausing_child();
    *member = *lead > 0 ? pausing_child() : -1;
    if (*member > 0 && setpgid(*lead, *lead) == 0 && setpgid(*member, *lead) == 0) {
        return 0;
    }
    if (*lead > 0) {
        kill(*lead, SIGKILL);
        waitpid(*lead, NULL, 0);
    }
    if (*member > 0) {
        kill(*member, SIGKILL);
        waitpid(*member, NULL, 0);
    }
    return -1;
}

/** Whether waitpid reaps `pid` killed by SIGKILL. */
static int reaped_killed(pid_t pid)
{
    int st;

    return waitpid(pid, &st, 0) == pid && WIFSIGNALED(st) && WTERMSIG(st) == SIGKILL;
}

/** Write `pid` to /proc/sys/kernel/ns_last_pid: 0 when the file then reads
 * `pid`, or -1. */
static int set_last_pid(pid_t pid)
{
    char text[24];
    int fd, n, ok;

    n = snprintf(text, sizeof(text), "%d\n", (int)pid);
    fd = open("/proc/sys/kernel/ns_last_pid", O_WRONLY);
    ok = fd >= 0 && write(fd, text, (size_t)n) == n;
    if (fd >= 0) {
        close(fd);
    }
    fd = ok ? open("/proc/sys/kernel/ns_last_pid", O_RDONLY) : -1;
    n = fd >= 0 ? (int)read(fd, text, sizeof(text) - 1) : -1;
    if (fd >= 0) {
        close(fd);
    }
    if (n <= 0) {
        return -1;
    }
    text[n] = '\0';
    return atoi(text) == (int)pid ? 0 : -1;
}

/** A pausing child whose pid is `want`, which is free, made by setting
 * ns_last_pid below it: its pid, or -1 when a few tries, which another
 * process's fork may race, give the number to none. */
static pid_t pausing_child_at(pid_t want)
{
    int tries;

    for (tries = 0; tries < 8; tries++) {
        pid_t pid = set_last_pid(want - 1) == 0 ? pausing_child() : -1;

        if (pid == want || pid < 0) {
            return pid;
        }
        kill(pid, SIGKILL);
        waitpid(pid, NULL, 0);
    }
    return -1;
}

/** The process-group scope of the pidfd of a reaped lone leader, once a
 * new child given the leader's pid leads a group of that number: ESRCH,
 * with the child left alive, as Linux walks the leader's own struct pid. */
static int reused_group(void)
{
    pid_t lead, again;
    int lfd, ok;

    lead = pausing_child();
    if (lead <= 0 || setpgid(lead, lead) != 0) {
        if (lead > 0) {
            kill(lead, SIGKILL);
            waitpid(lead, NULL, 0);
        }
        return fail("could not make a child lead a group");
    }
    lfd = (int)syscall(SYS_pidfd_open, (long)lead, 0L);
    kill(lead, SIGKILL);
    ok = reaped_killed(lead) && lfd >= 0;
    again = ok ? pausing_child_at(lead) : -1;
    ok = ok && again == lead && setpgid(again, again) == 0 && kill(-again, 0) == 0 &&
         syscall(SYS_pidfd_send_signal, (long)lfd, (long)SIGKILL, NULL,
                 PIDFD_SIGNAL_PROCESS_GROUP) == -1 &&
         errno == ESRCH && waitpid(again, NULL, WNOHANG) == 0;
    if (again > 0) {
        kill(again, SIGKILL);
        waitpid(again, NULL, 0);
    }
    if (lfd >= 0) {
        close(lfd);
    }
    if (!ok) {
        return fail("PIDFD_SIGNAL_PROCESS_GROUP reached a later group given the reaped leader's "
                    "pid");
    }
    report_held();
    return 0;
}

/** pidfd_send_signal's process-group scope: the group a child leads, while
 * it lives and once it is reaped, and the refusals. */
static int process_groups(void)
{
    pid_t lead, member;
    int lfd, mfd, self, ok;
    siginfo_t si;

    if (pausing_group(&lead, &member) != 0) {
        return fail("could not make a process group of two children");
    }
    lfd = (int)syscall(SYS_pidfd_open, (long)lead, 0L);
    mfd = (int)syscall(SYS_pidfd_open, (long)member, 0L);
    self = (int)syscall(SYS_pidfd_open, (long)getpid(), 0L);
    memset(&si, 0, sizeof(si));
    si.si_signo = SIGUSR1;
    si.si_code = SI_USER;
    ok = lfd >= 0 && mfd >= 0 && self >= 0 &&
         syscall(SYS_pidfd_send_signal, (long)mfd, 0L, NULL, PIDFD_SIGNAL_PROCESS_GROUP) == -1 &&
         errno == ESRCH &&
         syscall(SYS_pidfd_send_signal, (long)self, (long)SIGUSR1, &si,
                 PIDFD_SIGNAL_PROCESS_GROUP) == -1 &&
         errno == EPERM &&
         syscall(SYS_pidfd_send_signal, (long)lfd, (long)SIGKILL, NULL,
                 PIDFD_SIGNAL_PROCESS_GROUP) == 0;
    if (!ok) {
        kill(lead, SIGKILL);
        kill(member, SIGKILL);
    }
    ok = reaped_killed(lead) && reaped_killed(member) && ok;
    close(lfd);
    close(mfd);
    close(self);
    if (!ok) {
        return fail("PIDFD_SIGNAL_PROCESS_GROUP did not kill the group a child leads");
    }
    report_held();

    if (pausing_group(&lead, &member) != 0) {
        return fail("could not make a process group of two children");
    }
    lfd = (int)syscall(SYS_pidfd_open, (long)lead, 0L);
    kill(lead, SIGKILL);
    ok = lfd >= 0 && reaped_killed(lead) &&
         syscall(SYS_pidfd_send_signal, (long)lfd, 0L, NULL, 0L) == -1 && errno == ESRCH &&
         syscall(SYS_pidfd_send_signal, (long)lfd, (long)SIGKILL, NULL,
                 PIDFD_SIGNAL_PROCESS_GROUP) == 0;
    if (!ok) {
        kill(member, SIGKILL);
    }
    ok = reaped_killed(member) && ok &&
         syscall(SYS_pidfd_send_signal, (long)lfd, 0L, NULL, PIDFD_SIGNAL_PROCESS_GROUP) == -1 &&
         errno == ESRCH;
    close(lfd);
    if (!ok) {
        return fail("PIDFD_SIGNAL_PROCESS_GROUP did not kill the group of a reaped leader");
    }
    report_held();
    return reused_group();
}

/** pidfd_send_signal from a PID namespace below the one of the pidfd's
 * process, before and after that process is reaped: the child acknowledges
 * its first check on `ack` before the parent kills and reaps the process,
 * and makes its second once `go` says the reap is done. */
static int namespaces(void)
{
    pid_t x, n;
    int px, go[2], ack[2], st, ok;
    char b = 'g';

    x = pausing_child();
    px = x > 0 ? (int)syscall(SYS_pidfd_open, (long)x, 0L) : -1;
    if (px < 0 || pipe(go) != 0 || pipe(ack) != 0) {
        if (x > 0) {
            kill(x, SIGKILL);
            waitpid(x, NULL, 0);
        }
        return fail("pidfd_open of a pausing child");
    }
    n = (pid_t)syscall(SYS_clone, CLONE_NEWUSER_FLAG | CLONE_NEWPID_FLAG | (long)SIGCHLD, 0L, 0L,
                       0L, 0L);
    if (n == 0) {
        close(go[1]);
        close(ack[0]);
        if (syscall(SYS_pidfd_send_signal, (long)px, 0L, NULL, 0L) != -1 || errno != EINVAL) {
            _exit(2);
        }
        if (write(ack[1], &b, 1) != 1 || read(go[0], &b, 1) != 1) {
            _exit(3);
        }
        _exit(syscall(SYS_pidfd_send_signal, (long)px, 0L, NULL, 0L) == -1 && errno == EINVAL ? 0
                                                                                              : 4);
    }
    close(go[0]);
    close(ack[1]);
    ok = n > 0 && read(ack[0], &b, 1) == 1;
    kill(x, SIGKILL);
    waitpid(x, NULL, 0);
    ok = ok && write(go[1], &b, 1) == 1;
    close(go[1]);
    close(ack[0]);
    ok = n > 0 && waitpid(n, &st, 0) == n && ok && WIFEXITED(st) && WEXITSTATUS(st) == 0;
    close(px);
    if (!ok) {
        return fail("pidfd_send_signal from a PID namespace below the process's was not EINVAL "
                    "before and after the reap");
    }
    report_held();
    return 0;
}

/** tkill's and tgkill's checks in Linux 6.12's order: the ids, then the
 * thread, then the signal. 65 is above the last signal, and 2147483647
 * above pid_max, so no thread has it. */
static int tkill_order(void)
{
    long self = syscall(SYS_gettid);

    if (syscall(SYS_tkill, 0L, 0L) != -1 || errno != EINVAL ||
        syscall(SYS_tgkill, 0L, self, 0L) != -1 || errno != EINVAL ||
        syscall(SYS_tkill, 2147483647L, 65L) != -1 || errno != ESRCH ||
        syscall(SYS_tgkill, (long)getpid(), 2147483647L, 65L) != -1 || errno != ESRCH ||
        syscall(SYS_tgkill, (long)getppid(), self, 65L) != -1 || errno != ESRCH ||
        syscall(SYS_tkill, self, 65L) != -1 || errno != EINVAL ||
        syscall(SYS_tgkill, (long)getpid(), self, 65L) != -1 || errno != EINVAL) {
        return fail("tkill and tgkill did not check the ids, the thread and the signal in "
                    "Linux's order");
    }
    report_held();
    return 0;
}

/* The second thread of the thread rows and its two pipes: its id, which it
 * sends on `to_main` when it starts, the go it waits for on `to_thread`,
 * and whether it then took the SIGUSR1 sent to it alone with SI_TKILL. */
static int to_main[2], to_thread[2];
static int took_tkill;

/** The second thread: send its id, wait for the go, and take the blocked
 * SIGUSR1 that is pending for it. */
static void *second_thread(void *arg)
{
    struct timespec zero = {0, 0};
    pid_t tid = (pid_t)syscall(SYS_gettid);
    siginfo_t si;
    sigset_t set;
    char b;

    (void)arg;
    sigemptyset(&set);
    sigaddset(&set, SIGUSR1);
    memset(&si, 0, sizeof(si));
    if (write(to_main[1], &tid, sizeof(tid)) == (ssize_t)sizeof(tid) &&
        read(to_thread[0], &b, 1) == 1) {
        took_tkill = sigtimedwait(&set, &si, &zero) == SIGUSR1 && si.si_code == SI_TKILL &&
                     si.si_pid == getpid();
    }
    return NULL;
}

/** The status flags of the pidfd `fd` have O_RDWR, and O_EXCL as `excl`
 * says. */
static int pidfd_flags(int fd, int excl)
{
    int fl = fcntl(fd, F_GETFL);

    return fl >= 0 && (fl & O_ACCMODE) == O_RDWR && ((fl & O_EXCL) != 0) == excl;
}

/** Whether this thread finds SIGUSR1 pending, for it or for the process. */
static int usr1_pending_here(void)
{
    sigset_t set;

    return sigpending(&set) == 0 && sigismember(&set, SIGUSR1);
}

/** pidfd_getfd through the pidfd `tfd` of the second thread, while it
 * runs, and the refusals of that pidfd: the copy of the write end of
 * `to_main` carries a byte to its read end. */
static int thread_refusals(int tfd)
{
    siginfo_t si;
    char b = 0;
    int copy = (int)syscall(SYS_pidfd_getfd, (long)tfd, (long)to_main[1], 0L);
    int ok = copy >= 0 && fcntl(copy, F_GETFD) == FD_CLOEXEC && write(copy, "c", 1) == 1 &&
             read(to_main[0], &b, 1) == 1 && b == 'c';

    if (copy >= 0) {
        close(copy);
    }
    memset(&si, 0, sizeof(si));
    return ok &&
           syscall(SYS_pidfd_send_signal, (long)tfd, 0L, NULL, PIDFD_SIGNAL_PROCESS_GROUP) == -1 &&
           errno == ESRCH && waitid(P_PIDFD, (id_t)tfd, &si, WEXITED | WNOHANG) == -1 &&
           errno == ECHILD;
}

/** The leading thread's signal scope, and the poll of the pidfd `tfd` of
 * the second thread `tid`, which has been joined. */
static int thread_ended(int self, int lfd, pid_t tid, int tfd)
{
    if (!scoped_to_self(lfd, 0L, 0, SI_TKILL) ||
        !scoped_to_self(lfd, PIDFD_SIGNAL_THREAD_GROUP, 1, SI_USER)) {
        return fail("the leading thread's pidfd did not signal it alone with no flag, or the "
                    "process with PIDFD_SIGNAL_THREAD_GROUP");
    }
    report_held();
    if (!polls(tfd, POLLIN, 1) || !polls(tfd, POLLIN | POLLHUP, 0) || polls(self, POLLIN, 0) ||
        syscall(SYS_pidfd_open, (long)tid, (long)PIDFD_THREAD) != -1 || errno != ESRCH ||
        syscall(SYS_pidfd_send_signal, (long)tfd, 0L, NULL, 0L) != -1 || errno != ESRCH) {
        return fail("the pidfd of a thread that ended did not poll readable and hung up apart "
                    "from its process, or its id was still found");
    }
    report_held();
    return 0;
}

/** Start the second thread with its pipes: 0 with its handle in `*t` and
 * its id in `*tid`, or -1. */
static int thread_start(pthread_t *t, pid_t *tid)
{
    if (pipe(to_main) != 0 || pipe(to_thread) != 0 ||
        pthread_create(t, NULL, second_thread, NULL) != 0) {
        return -1;
    }
    return read(to_main[0], tid, sizeof(*tid)) == (ssize_t)sizeof(*tid) ? 0 : -1;
}

/** Give the second thread `t` its go, join it and close its pipes: whether
 * it was joined. */
static int thread_stop(pthread_t t)
{
    char b = 'g';
    int ok = write(to_thread[1], &b, 1) == 1 && pthread_join(t, NULL) == 0;

    close(to_main[0]);
    close(to_main[1]);
    close(to_thread[0]);
    close(to_thread[1]);
    return ok;
}

/** The opening and the refusals of a pidfd of the second thread `tid`,
 * which runs: 0 with the pidfd in `*tfd`, or the failure. `self` is a pidfd
 * of this process, `lfd` one of its leading thread. */
static int thread_opens(int self, int lfd, pid_t tid, int *tfd)
{
    *tfd = (int)syscall(SYS_pidfd_open, (long)tid, (long)PIDFD_THREAD);
    if (*tfd < 0 || !pidfd_flags(*tfd, 1) || !pidfd_flags(lfd, 1) || !pidfd_flags(self, 0) ||
        syscall(SYS_pidfd_open, (long)tid, 0L) != -1 || errno != EINVAL) {
        return fail("pidfd_open with PIDFD_THREAD gave no pidfd with O_EXCL, or without it took a "
                    "thread that does not lead its process");
    }
    report_held();
    if (!thread_refusals(*tfd)) {
        return fail("pidfd_getfd by a pidfd of a thread that does not lead its process copied no "
                    "descriptor, or PIDFD_SIGNAL_PROCESS_GROUP or waitid by it was not refused");
    }
    report_held();
    return 0;
}

/** The rows of a pidfd of one thread in this process: its opening, its
 * refusals, its signal scope, and its poll at the thread's end. `self` is
 * a pidfd of this process, `lfd` one of its leading thread. */
static int thread_rows(int self, int lfd)
{
    pthread_t t;
    pid_t tid = 0;
    int tfd = -1, rc, sent;

    if (thread_start(&t, &tid) != 0) {
        return fail("could not start a second thread");
    }
    rc = thread_opens(self, lfd, tid, &tfd);
    sent = rc == 0 && !polls(tfd, POLLIN, 0) &&
           syscall(SYS_pidfd_send_signal, (long)tfd, (long)SIGUSR1, NULL, 0L) == 0 &&
           !usr1_pending_here();
    if (!thread_stop(t) && rc == 0) {
        rc = fail("could not join the second thread");
    }
    if (rc == 0 && (!sent || !took_tkill)) {
        rc = fail("pidfd_send_signal with no flag on a pidfd of a thread did not signal that "
                  "thread alone with SI_TKILL");
    }
    if (rc == 0) {
        report_held();
        rc = thread_ended(self, lfd, tid, tfd);
    }
    close(tfd);
    return rc;
}

/** The start of the child's second thread, which waits to be killed. */
static void *pausing_thread(void *arg)
{
    (void)arg;
    for (;;) {
        pause();
    }
    return NULL;
}

/** A child whose leading thread ends while its second thread runs: the
 * leader's thread pidfd and the process's pidfd at its end and its reap. */
static int leader_thread_poll(void)
{
    pthread_t t;
    pid_t pid = fork();
    int tfd, pfd, ok;

    if (pid == 0) {
        if (pthread_create(&t, NULL, pausing_thread, NULL) != 0) {
            _exit(2);
        }
        pthread_exit(NULL);
    }
    tfd = pid > 0 ? (int)syscall(SYS_pidfd_open, (long)pid, (long)PIDFD_THREAD) : -1;
    pfd = pid > 0 ? (int)syscall(SYS_pidfd_open, (long)pid, 0L) : -1;
    ok = tfd >= 0 && pfd >= 0 && polls(tfd, POLLIN, 1) && !polls(tfd, POLLHUP, 0) &&
         !polls(pfd, POLLIN, 0);
    if (pid > 0) {
        kill(pid, SIGKILL);
        ok = ok && polls(pfd, POLLIN, 1) && reaped_killed(pid) && polls(tfd, POLLHUP, 0) &&
             polls(pfd, POLLHUP, 0);
    }
    close(tfd);
    close(pfd);
    if (!ok) {
        return fail("a child's leading thread that ended before its process did not poll "
                    "readable alone, or its pidfds did not hang up at the reap");
    }
    report_held();
    return 0;
}

/* A thread that sends its id on `ready` and ends at the go on `go`. */
struct waiter {
    int ready[2];
    int go[2];
    pthread_t t;
    pid_t tid;
};

/** The waiter's thread: send its id, then wait for the go. */
static void *waiting_thread(void *arg)
{
    struct waiter *w = arg;
    pid_t tid = (pid_t)syscall(SYS_gettid);
    char b;

    if (write(w->ready[1], &tid, sizeof(tid)) == (ssize_t)sizeof(tid)) {
        (void)read(w->go[0], &b, 1);
    }
    return NULL;
}

/** Start the waiter `w`: 0 with its id in `w->tid`, or -1 with nothing
 * left open. */
static int waiter_start(struct waiter *w)
{
    if (pipe(w->ready) != 0) {
        return -1;
    }
    if (pipe(w->go) != 0) {
        close(w->ready[0]);
        close(w->ready[1]);
        return -1;
    }
    if (pthread_create(&w->t, NULL, waiting_thread, w) == 0) {
        if (read(w->ready[0], &w->tid, sizeof(w->tid)) == (ssize_t)sizeof(w->tid)) {
            return 0;
        }
        close(w->go[1]);
        pthread_join(w->t, NULL);
        w->go[1] = -1;
    }
    close(w->ready[0]);
    close(w->ready[1]);
    close(w->go[0]);
    if (w->go[1] >= 0) {
        close(w->go[1]);
    }
    return -1;
}

/** Give the waiter `w` its go, join it and close its pipes: whether it was
 * joined. */
static int waiter_stop(struct waiter *w)
{
    int ok = write(w->go[1], "g", 1) == 1 && pthread_join(w->t, NULL) == 0;

    close(w->ready[0]);
    close(w->ready[1]);
    close(w->go[0]);
    close(w->go[1]);
    return ok;
}

/** A waiter whose id is `want`, which is free, made by setting ns_last_pid
 * below it: 0, or -1 when a few tries, which another fork may race or a
 * record not yet freed may hold the id through, give the id to none. */
static int waiter_start_at(struct waiter *w, pid_t want)
{
    int tries;

    for (tries = 0; tries < 8; tries++) {
        if (set_last_pid(want - 1) != 0 || waiter_start(w) != 0) {
            return -1;
        }
        if (w->tid == want) {
            return 0;
        }
        waiter_stop(w);
    }
    return -1;
}

/** The pidfd of a thread that has ended, once a new thread of the process
 * is given its id through ns_last_pid: ESRCH to pidfd_send_signal and hung
 * up, as Linux's pidfd keeps the ended thread's struct pid, which the new
 * thread does not share. */
static int thread_id_reused(void)
{
    struct waiter w;
    pid_t tid;
    int tfd, ok;

    if (waiter_start(&w) != 0) {
        return fail("could not start a second thread");
    }
    tid = w.tid;
    tfd = (int)syscall(SYS_pidfd_open, (long)tid, (long)PIDFD_THREAD);
    if (!waiter_stop(&w) || tfd < 0) {
        if (tfd >= 0) {
            close(tfd);
        }
        return fail("pidfd_open with PIDFD_THREAD of a second thread, or its join");
    }
    if (waiter_start_at(&w, tid) != 0) {
        close(tfd);
        return fail("could not give a new thread the id of one that ended");
    }
    ok = syscall(SYS_pidfd_send_signal, (long)tfd, 0L, NULL, 0L) == -1 && errno == ESRCH &&
         polls(tfd, POLLIN | POLLHUP, 0);
    ok = waiter_stop(&w) && ok;
    close(tfd);
    if (!ok) {
        return fail("the pidfd of a thread that ended named a new thread given its id");
    }
    report_held();
    return 0;
}

/** An edge-triggered epoll of the pidfd of a thread that has ended, whose
 * event epoll_wait has taken: the end of a sibling gives it no second
 * event, as Linux wakes only the ended thread's struct pid. The sibling's
 * own pidfd polls readable once its end has woken its pollers. */
static int thread_edge(void)
{
    struct waiter a, b;
    struct epoll_event ev, out;
    int afd, bfd, ep, ok;

    if (waiter_start(&a) != 0) {
        return fail("could not start a second thread");
    }
    if (waiter_start(&b) != 0) {
        waiter_stop(&a);
        return fail("could not start a third thread");
    }
    afd = (int)syscall(SYS_pidfd_open, (long)a.tid, (long)PIDFD_THREAD);
    bfd = (int)syscall(SYS_pidfd_open, (long)b.tid, (long)PIDFD_THREAD);
    ep = epoll_create1(EPOLL_CLOEXEC);
    memset(&ev, 0, sizeof(ev));
    ev.events = EPOLLIN | EPOLLET;
    ok = afd >= 0 && bfd >= 0 && ep >= 0 && epoll_ctl(ep, EPOLL_CTL_ADD, afd, &ev) == 0 &&
         epoll_wait(ep, &out, 1, 0) == 0;
    ok = waiter_stop(&a) && ok && epoll_wait(ep, &out, 1, -1) == 1 && (out.events & EPOLLIN) != 0;
    ok = waiter_stop(&b) && ok && polls(bfd, POLLIN, 1) && epoll_wait(ep, &out, 1, 0) == 0;
    if (ep >= 0) {
        close(ep);
    }
    if (afd >= 0) {
        close(afd);
    }
    if (bfd >= 0) {
        close(bfd);
    }
    if (!ok) {
        return fail("an edge-triggered epoll of an ended thread's pidfd had a second event when a "
                    "sibling ended");
    }
    report_held();
    return 0;
}

/** The thread of a child that the caller traces, and that has ended but
 * is not yet reaped: by the pidfd of that thread, pidfd_getfd copies a
 * descriptor while it runs and is ESRCH once its end has released its
 * table, as Linux's __pidfd_fget answers a task past exit_files(). */
static int traced_zombie(void)
{
    struct waiter w;
    siginfo_t si;
    pid_t pid;
    int tfd = -1, copy = -1, ok = 0, st;

    w.tid = -1;
    if (pipe(w.ready) != 0) {
        return fail("pipe for a traced thread");
    }
    if (pipe(w.go) != 0) {
        close(w.ready[0]);
        close(w.ready[1]);
        return fail("pipe for a traced thread");
    }
    pid = fork();
    if (pid == 0) {
        if (pthread_create(&w.t, NULL, waiting_thread, &w) != 0) {
            _exit(2);
        }
        for (;;) {
            pause();
        }
    }
    close(w.ready[1]);
    if (pid > 0 && read(w.ready[0], &w.tid, sizeof(w.tid)) == (ssize_t)sizeof(w.tid)) {
        tfd = (int)syscall(SYS_pidfd_open, (long)w.tid, (long)PIDFD_THREAD);
        copy = tfd >= 0 ? (int)syscall(SYS_pidfd_getfd, (long)tfd, (long)w.go[0], 0L) : -1;
        memset(&si, 0, sizeof(si));
        ok = copy >= 0 && ptrace(PTRACE_SEIZE, w.tid, NULL, NULL) == 0 &&
             write(w.go[1], "g", 1) == 1 &&
             waitid(P_PID, (id_t)w.tid, &si, WEXITED | WNOWAIT | __WALL) == 0 &&
             si.si_pid == w.tid && si.si_code == CLD_EXITED &&
             syscall(SYS_pidfd_getfd, (long)tfd, (long)w.go[0], 0L) == -1 && errno == ESRCH &&
             waitpid(w.tid, &st, __WALL) == w.tid;
    }
    if (pid > 0) {
        kill(pid, SIGKILL);
        /* A traced thread not yet reaped holds its leader's reap back. */
        if (w.tid > 0 && !ok) {
            waitpid(w.tid, NULL, __WALL);
        }
        ok = reaped_killed(pid) && ok;
    }
    if (copy >= 0) {
        close(copy);
    }
    if (tfd >= 0) {
        close(tfd);
    }
    close(w.ready[0]);
    close(w.go[0]);
    close(w.go[1]);
    if (!ok) {
        return fail("pidfd_getfd by the pidfd of a traced thread that ended and was not reaped "
                    "was not ESRCH, or copied nothing while it ran");
    }
    report_held();
    return 0;
}

/** pidfd_open's PIDFD_THREAD: the rows of a pidfd of one thread. */
static int threads(void)
{
    sigset_t set, old;
    int self = (int)syscall(SYS_pidfd_open, (long)getpid(), 0L);
    int lfd = (int)syscall(SYS_pidfd_open, (long)getpid(), (long)PIDFD_THREAD);
    int rc;

    sigemptyset(&set);
    sigaddset(&set, SIGUSR1);
    if (self < 0 || sigprocmask(SIG_BLOCK, &set, &old) != 0) {
        return fail("pidfd_open of this process");
    }
    if (lfd < 0) {
        sigprocmask(SIG_SETMASK, &old, NULL);
        close(self);
        return fail("pidfd_open with PIDFD_THREAD of the leading thread");
    }
    rc = thread_rows(self, lfd);
    sigprocmask(SIG_SETMASK, &old, NULL);
    close(lfd);
    close(self);
    if (rc == 0) {
        rc = leader_thread_poll();
    }
    if (rc == 0) {
        rc = thread_id_reused();
    }
    if (rc == 0) {
        rc = thread_edge();
    }
    return rc != 0 ? rc : traced_zombie();
}

/** A child that leads a group, joins its parent's group while a member
 * stays in its own, and calls setsid: 0 when it is EPERM and succeeds once
 * the member is reaped, or the step that failed. */
static int left_group_child(void)
{
    pid_t self = getpid(), member;

    if (setpgid(0, 0) != 0) {
        return 2;
    }
    member = pausing_child();
    if (member < 0) {
        return 3;
    }
    if (setpgid(0, getpgid(getppid())) != 0 || getpgid(member) != self) {
        return 4;
    }
    if (setsid() != -1 || errno != EPERM) {
        return 5;
    }
    kill(member, SIGKILL);
    if (!reaped_killed(member)) {
        return 6;
    }
    return setsid() == self ? 0 : 7;
}

/** setsid by a former leader whose group still has a member. */
static int setsid_left_group(void)
{
    pid_t pid = fork();
    int st;

    if (pid == 0) {
        _exit(left_group_child());
    }
    if (pid < 0 || waitpid(pid, &st, 0) != pid || !WIFEXITED(st) || WEXITSTATUS(st) != 0) {
        return fail("setsid was not EPERM while a member stayed in the caller's old group, or "
                    "failed once it was reaped");
    }
    report_held();
    return 0;
}

/** The number a read of the descriptor `fd` of ns_last_pid gives from its
 * start, or -1. */
static long last_pid_at(int fd)
{
    char text[24];
    ssize_t n;

    if (lseek(fd, 0, SEEK_SET) != 0) {
        return -1;
    }
    n = read(fd, text, sizeof(text) - 1);
    if (n <= 0) {
        return -1;
    }
    text[n] = '\0';
    return atol(text);
}

/** A child that exits at once, reaped: its pid, or -1. */
static pid_t forked_and_reaped(void)
{
    pid_t pid = fork();

    if (pid == 0) {
        _exit(0);
    }
    return pid > 0 && waitpid(pid, NULL, 0) == pid ? pid : -1;
}

/** Make the effective and permitted sets `low` and `high`, the capabilities
 * 0 to 31 and 32 to 40, with none inheritable: capset's result. */
static int keep_caps(unsigned low, unsigned high)
{
    unsigned h[2] = {CAP_VERSION_3, 0};
    unsigned d[6];

    d[0] = d[1] = low;
    d[3] = d[4] = high;
    d[2] = d[5] = 0;
    return (int)syscall(SYS_capset, h, d);
}

/** The first process of a new PID namespace, with `outer` a descriptor of
 * ns_last_pid opened in the first one: 0 when the rows hold, or 1, 2 or 3
 * for the first that fails, `last-pid-above`, `last-pid-read` and
 * `last-pid-cap`. */
static int last_pid_child(int outer)
{
    int fd;
    long v;

    if (set_last_pid(40000) != 0 || forked_and_reaped() != 40001) {
        return 1;
    }
    fd = open("/proc/sys/kernel/ns_last_pid", O_RDONLY);
    if (last_pid_at(outer) != 40001 || fd < 0 || forked_and_reaped() != 40002) {
        return 2;
    }
    v = last_pid_at(fd);
    close(fd);
    if (v != 40002) {
        return 2;
    }
    if (keep_caps(0, 1u << (CAP_CHECKPOINT_RESTORE - 32)) != 0 || set_last_pid(50000) != 0) {
        return 3;
    }
    fd = keep_caps(0, 0) == 0 ? open("/proc/sys/kernel/ns_last_pid", O_WRONLY) : -1;
    if (fd < 0 || write(fd, "1\n", 2) != -1 || errno != EPERM) {
        return 3;
    }
    close(fd);
    return 0;
}

/** ns_last_pid in a new PID namespace: the range, the read and the
 * capability. */
static int last_pid_rows(void)
{
    static const char *const why[] = {
        "a write of 40000 to a new PID namespace's ns_last_pid did not make the next child 40001",
        "ns_last_pid did not read the reader's namespace at the read",
        "ns_last_pid did not take CAP_CHECKPOINT_RESTORE alone, or took a writer with no "
        "capability"};
    int outer = open("/proc/sys/kernel/ns_last_pid", O_RDONLY);
    pid_t pid;
    int st, code, i;

    if (outer < 0) {
        return fail("open of ns_last_pid for reading");
    }
    pid = (pid_t)syscall(SYS_clone, CLONE_NEWPID_FLAG | (long)SIGCHLD, 0L, 0L, 0L, 0L);
    if (pid == 0) {
        _exit(last_pid_child(outer));
    }
    close(outer);
    code = pid > 0 && waitpid(pid, &st, 0) == pid && WIFEXITED(st) ? WEXITSTATUS(st) : 1;
    for (i = 1; i <= 3; i++) {
        if (code == i || code > 3) {
            return fail(why[i - 1]);
        }
        report_held();
    }
    return 0;
}

/** Write the text `text` to /proc/sys/kernel/pid_max: 0, or -1. */
static int write_pid_max(const char *text)
{
    int fd = open("/proc/sys/kernel/pid_max", O_WRONLY);
    int ok = fd >= 0 && write(fd, text, strlen(text)) == (ssize_t)strlen(text);

    if (fd >= 0) {
        close(fd);
    }
    return ok ? 0 : -1;
}

/** Forks with pid_max at 301 come to EAGAIN, and fork works once it is
 * back; the children pause until they are killed. */
static int fork_eagain(void)
{
    pid_t kids[302];
    char old[24];
    int fd, n, tries, err = 0;

    fd = open("/proc/sys/kernel/pid_max", O_RDONLY);
    n = fd >= 0 ? (int)read(fd, old, sizeof(old) - 1) : -1;
    if (fd >= 0) {
        close(fd);
    }
    if (n <= 0 || write_pid_max("301\n") != 0) {
        return fail("pid_max could not be read or set to 301");
    }
    old[n] = '\0';
    for (tries = 0; tries < 302; tries++) {
        kids[tries] = pausing_child();
        if (kids[tries] < 0) {
            err = errno;
            break;
        }
    }
    n = write_pid_max(old);
    while (tries-- > 0 && kids[tries] > 0) {
        kill(kids[tries], SIGKILL);
        waitpid(kids[tries], NULL, 0);
    }
    if (n != 0) {
        return fail("pid_max could not be set back");
    }
    if (err != EAGAIN || forked_and_reaped() < 0) {
        return fail("fork did not come to EAGAIN with every pid below pid_max in use, or failed "
                    "once pid_max was back");
    }
    report_held();
    return 0;
}

/** Begin the report and plan its assertions, in the order they are
 * checked. */
static void plan(void)
{
    static const char *const ids[] = {"clear-sighand",
                                      "clear-sighand-einval",
                                      "clone3-pidfd",
                                      "pidfd-readable",
                                      "waitid-pidfd",
                                      "pidfd-hangup",
                                      "pidfd-open",
                                      "send-signal",
                                      "killed",
                                      "send-reaped",
                                      "nonblock-eagain",
                                      "getfd",
                                      "getfd-errors",
                                      "pidfd-errors",
                                      "self-faults",
                                      "populate",
                                      "wait4-faults",
                                      "children-faults",
                                      "hangup-wakes",
                                      "pidfd-file-info",
                                      "process-file-info",
                                      "thread-file-info",
                                      "info-before-target",
                                      "queued-code-permission",
                                      "scope-thread",
                                      "scope-thread-group",
                                      "scope-process-group",
                                      "scope-group-reaped",
                                      "scope-group-reused",
                                      "reaped-namespace",
                                      "tkill-order",
                                      "thread-open",
                                      "thread-refusals",
                                      "thread-signal",
                                      "leader-thread-signal",
                                      "thread-poll",
                                      "leader-thread-poll",
                                      "thread-id-reused",
                                      "thread-edge",
                                      "traced-zombie-getfd",
                                      "setsid-left-group",
                                      "last-pid-above",
                                      "last-pid-read",
                                      "last-pid-cap",
                                      "fork-eagain"};
    unsigned i;

    report_begin(report_case(environ, "guest.default.pidfdmusl"));
    for (i = 0; i < sizeof(ids) / sizeof(ids[0]); i++) {
        report_plan(ids[i]);
    }
}

int main(void)
{
    plan();
    if (clones() != 0 || opens() != 0 || getfds() != 0 || faults() != 0 || hangup_wakes() != 0 ||
        file_info() != 0 || queued_checks() != 0 || scopes() != 0 || process_groups() != 0 ||
        namespaces() != 0 || tkill_order() != 0 || threads() != 0 || setsid_left_group() != 0 ||
        last_pid_rows() != 0 || fork_eagain() != 0) {
        return 1;
    }
    report_end();
    printf("PIDFDMUSL OK\n");

    return 0;
}
