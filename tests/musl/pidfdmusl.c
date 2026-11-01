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
 * and `children-faults`. The first failure fails its assertion and leaves
 * the rest not run.
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
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

/** Begin the report and plan its assertions, in the order they are
 * checked. */
static void plan(void)
{
    static const char *const ids[] = {"clear-sighand",  "clear-sighand-einval", "clone3-pidfd",
                                      "pidfd-readable", "waitid-pidfd",         "pidfd-hangup",
                                      "pidfd-open",     "send-signal",          "killed",
                                      "send-reaped",    "nonblock-eagain",      "getfd",
                                      "getfd-errors",   "pidfd-errors",         "self-faults",
                                      "populate",       "wait4-faults",         "children-faults"};
    unsigned i;

    report_begin(report_case(environ, "guest.default.pidfdmusl"));
    for (i = 0; i < sizeof(ids) / sizeof(ids[0]); i++) {
        report_plan(ids[i]);
    }
}

int main(void)
{
    plan();
    if (clones() != 0 || opens() != 0 || getfds() != 0 || faults() != 0) {
        return 1;
    }
    report_end();
    printf("PIDFDMUSL OK\n");

    return 0;
}
