/**
 * schedmusl: check how the kernel identifies and schedules a process, read
 * through musl.
 *
 * `gettid` returns the process id, in the process and in a forked child
 * alike, since each of this probe's processes has one thread; in a process
 * with several, a thread other than the leader has an id of its own.
 *
 * The nice value starts at 0, is kept as set, clamped to 19, inherited by a
 * child, and read raw as 20 less itself. Set for a process group it reaches
 * every member, and read for one it is the lowest. A caller of another user
 * may raise its own nice value but not lower it, and may not change root's
 * process; it may set its own user's. A user or a pid with no process fails
 * with ESRCH, and an unknown `which` with EINVAL.
 *
 * A parent may set its child's process group after a failed exec, but a
 * successful exec, by the leader or another thread, makes that EACCES.
 * The new image may still set its own group and the group of a fresh child.
 * SIGSTOP and waitpid order each check against the child's exec.
 *
 * `sched_yield` returns 0. Every process has SCHED_OTHER with priority 0, and
 * asking for it, with SCHED_RESET_ON_FORK or not, succeeds; SCHED_FIFO and
 * SCHED_RR fail with EPERM, and the other policies, a priority that does not
 * suit its policy, and an unknown policy with EINVAL. The priority ranges
 * are Linux's, and the time slice is one 10 ms tick. The affinity is the
 * processors /proc/cpuinfo lists, by their ids, which need not run without a
 * gap, read raw as the bytes of the longs the highest takes, four on i386 to
 * 32 processors and eight past them and on x86-64; a mask of the last
 * processor alone is accepted, kept, and moves the caller there, as getcpu
 * then says, and one of a processor not started fails with EINVAL. A caller
 * of another user may not change root's process, a pid of no process fails
 * with ESRCH, and a negative pid with EINVAL. musl answers the policy and
 * parameter calls with ENOSYS itself, so those rows call the kernel through
 * `syscall`.
 *
 * /proc/self/oom_score_adj reads 0 at first and is root's, mode 0644, while
 * the process is dumpable; a write, opened as a shell's redirection opens
 * it, reads back, a child inherits it, and a value past -1000 to 1000 or
 * text that is no number fails with EINVAL. A caller of user 1000,
 * made dumpable again after it gave up root, owns its file; it may raise its
 * value and lower it to the floor a privileged write left, but not below,
 * which is EACCES, and with no privileged write before, not below 0; it may
 * not open root's process's file for writing.
 *
 * The I/O priority starts as class none, level 0, is kept as set and
 * inherited; a level past 7 and class none with a level are EINVAL, and a
 * caller of another user may take the idle class but not the real-time one,
 * which is EPERM, nor change root's process. musl has no wrapper for these
 * calls, so they go through `syscall`.
 *
 * It prints "SCHEDMUSL OK" and exits 0 on success, or prints "SCHEDMUSL FAIL
 * (detail)" and exits 1 at the first failure.
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

/**
 * Set in `*set` the id of each processor /proc/cpuinfo lists on a
 * "processor" line. Returns their count, or 0 when the file cannot be read.
 * The ids need not run from 0 without a gap, since the kernel goes on
 * starting processors past one that does not start.
 */
static int cpuinfo_ids(cpu_set_t *set)
{
    char line[256];
    int n = 0, id;
    FILE *f = fopen("/proc/cpuinfo", "r");

    CPU_ZERO(set);
    if (!f) {
        return 0;
    }
    while (fgets(line, sizeof(line), f)) {
        if (sscanf(line, "processor\t: %d", &id) == 1 && id >= 0 && id < CPU_SETSIZE) {
            CPU_SET(id, set);
            n++;
        }
    }
    fclose(f);
    return n;
}

/** Report one failed check and return 1, the probe's failure status. */
static int fail(const char *what)
{
    printf("SCHEDMUSL FAIL (%s)\n", what);

    return 1;
}

/**
 * Check that the thread id is the process id, here and in a child. Returns 0,
 * or 1 after the failure is printed.
 */
static int thread_ids(void)
{
    pid_t child;
    int status;

    if (gettid() != getpid()) {
        return fail("gettid is not the process id");
    }
    child = fork();
    if (child < 0) {
        return fail("fork failed");
    }
    if (child == 0) {
        _exit(gettid() == getpid() ? 0 : 1);
    }
    if (waitpid(child, &status, 0) != child || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        return fail("a child's gettid is not its process id");
    }
    return 0;
}

/**
 * Run `test` in a forked child and return its exit status, or -1 when the
 * fork failed or the child did not exit.
 */
static int in_child(int (*test)(void))
{
    pid_t child = fork();
    int status;

    if (child < 0) {
        return -1;
    }
    if (child == 0) {
        _exit(test());
    }
    if (waitpid(child, &status, 0) != child || !WIFEXITED(status)) {
        return -1;
    }
    return WEXITSTATUS(status);
}

/**
 * Check in the new image that it may set its own group and a fresh child's
 * group, then stop for its parent's checks. Returns 0, or a failed step.
 */
static int exec_group_image(void)
{
    pid_t child;
    int status, rc;

    if (setpgid(0, 0) != 0 || getpgrp() != getpid()) {
        return 1;
    }
    child = fork();
    if (child < 0) {
        return 2;
    }
    if (child == 0) {
        kill(getpid(), SIGSTOP);
        _exit(0);
    }
    rc = waitpid(child, &status, WUNTRACED) == child && WIFSTOPPED(status) &&
                 WSTOPSIG(status) == SIGSTOP && setpgid(child, child) == 0 &&
                 getpgid(child) == child
             ? 0
             : 3;
    kill(child, SIGKILL);
    waitpid(child, NULL, 0);
    if (rc != 0) {
        return rc;
    }
    kill(getpid(), SIGSTOP);
    return 0;
}

/** Execute this probe's group helper from a new thread; exit on failure. */
static void *exec_group_thread(void *arg)
{
    (void)arg;
    execl("/musl/schedmusl", "schedmusl", "--exec-group", (char *)NULL);
    _exit(4);
}

/**
 * Check the parent's setpgid before and after its child's exec. With
 * `threaded`, a non-leader thread executes the image. Returns 0 on success,
 * or 1 after the failure is printed. All children are reaped before return.
 */
static int exec_groups(int threaded)
{
    pid_t child = fork();
    int status, rc = 0;
    pthread_t thread;

    if (child < 0) {
        return fail("fork for the exec process group");
    }
    if (child == 0) {
        execl("/schedmusl-no-such-file", "missing", (char *)NULL);
        if (errno != ENOENT) {
            _exit(5);
        }
        kill(getpid(), SIGSTOP);
        if (threaded) {
            if (pthread_create(&thread, NULL, exec_group_thread, NULL) != 0) {
                _exit(6);
            }
            for (;;) {
                pause();
            }
        }
        exec_group_thread(NULL);
    }
    if (waitpid(child, &status, WUNTRACED) != child || !WIFSTOPPED(status) ||
        WSTOPSIG(status) != SIGSTOP) {
        rc = fail("the child did not stop after its failed exec");
    } else if (setpgid(child, child) != 0 || getpgid(child) != child ||
               setpgid(child, getpgrp()) != 0) {
        rc = fail("setpgid after a child's failed exec");
    } else {
        kill(child, SIGCONT);
        if (waitpid(child, &status, WUNTRACED) != child || !WIFSTOPPED(status) ||
            WSTOPSIG(status) != SIGSTOP) {
            rc = fail("the new image could not set its own or its child's group");
        } else if (setpgid(child, child) != -1 || errno != EACCES ||
                   setpgid(child, getpgrp()) != -1 || errno != EACCES || getpgid(child) != child) {
            rc = fail("setpgid after a child's successful exec was not EACCES");
        }
    }
    kill(child, SIGKILL);
    waitpid(child, NULL, 0);
    return rc;
}

/** Run in a child: return 0 when it inherited the nice value -5, else 1. */
static int inherited(void)
{
    return getpriority(PRIO_PROCESS, 0) == -5 ? 0 : 1;
}

/**
 * Run in a child: make a process group of two, check that a nice value set
 * for the group reaches the other member, and that the group reads as its
 * lowest. Returns 0, or the number of the step that failed.
 */
static int group(void)
{
    pid_t member;
    int rc = 0;

    if (setpgid(0, 0) != 0) {
        return 1;
    }
    member = fork();
    if (member < 0) {
        return 2;
    }
    if (member == 0) {
        pause();
        _exit(0);
    }
    if (setpriority(PRIO_PGRP, 0, 3) != 0 || getpriority(PRIO_PROCESS, member) != 3) {
        rc = 3;
    } else if (setpriority(PRIO_PROCESS, member, 7) != 0 || getpriority(PRIO_PGRP, 0) != 3) {
        rc = 4;
    }
    kill(member, SIGKILL);
    waitpid(member, NULL, 0);
    return rc;
}

/**
 * Run in a child of root: become user 1000, and check that it may raise its
 * own nice value and set its user's, and may neither lower its own nor
 * change root's process. Returns 0, or the number of the step that failed.
 */
static int unprivileged(void)
{
    if (setresgid(1000, 1000, 1000) != 0 || setresuid(1000, 1000, 1000) != 0) {
        return 1;
    }
    if (setpriority(PRIO_PROCESS, 0, 10) != 0) {
        return 2;
    }
    if (setpriority(PRIO_PROCESS, 0, 9) != -1 || errno != EACCES) {
        return 3;
    }
    if (setpriority(PRIO_PROCESS, getppid(), 10) != -1 || errno != EPERM) {
        return 4;
    }
    if (setpriority(PRIO_USER, 0, 12) != 0 || getpriority(PRIO_USER, 1000) != 12) {
        return 5;
    }
    return 0;
}

/**
 * Check the nice value: kept, clamped, inherited, raw, by group and by user,
 * and the errors. Returns 0, or 1 after the failure is printed.
 */
static int priorities(void)
{
    errno = 0;
    if (getpriority(PRIO_PROCESS, 0) != 0 || errno != 0) {
        return fail("the nice value does not start at 0");
    }
    if (setpriority(PRIO_PROCESS, 0, 5) != 0 || getpriority(PRIO_PROCESS, 0) != 5) {
        return fail("a nice value of 5 was not kept");
    }
    if (syscall(SYS_getpriority, PRIO_PROCESS, 0) != 15) {
        return fail("the raw getpriority does not return 20 less the nice value");
    }
    if (setpriority(PRIO_PROCESS, getpid(), 100) != 0 || getpriority(PRIO_PROCESS, 0) != 19) {
        return fail("a nice value past 19 was not clamped to 19");
    }
    if (setpriority(PRIO_PROCESS, 0, -5) != 0 || getpriority(PRIO_PROCESS, 0) != -5) {
        return fail("root could not lower the nice value");
    }
    if (in_child(inherited) != 0) {
        return fail("a child did not inherit the nice value");
    }
    if (in_child(group) != 0) {
        return fail("a process group's nice value was not set or read as one");
    }
    switch (in_child(unprivileged)) {
    case 0:
        break;
    case 2:
        return fail("user 1000 could not raise its own nice value");
    case 3:
        return fail("user 1000 lowered its own nice value");
    case 4:
        return fail("user 1000 changed root's process");
    case 5:
        return fail("user 1000 could not set its own user's nice value");
    default:
        return fail("the unprivileged child did not run");
    }
    if (getpriority(PRIO_USER, 4242) != -1 || errno != ESRCH ||
        getpriority(PRIO_PROCESS, 999999) != -1 || errno != ESRCH) {
        return fail("a priority of no process was not refused with ESRCH");
    }
    if (getpriority(3, 0) != -1 || errno != EINVAL) {
        return fail("an unknown which was not refused with EINVAL");
    }
    return 0;
}

/**
 * Ask the kernel to set `pid` to `policy` at `priority`. Returns 0, or -1
 * with errno set.
 */
static long set_policy(pid_t pid, int policy, int priority)
{
    struct sched_param sp;

    sp.sched_priority = priority;
    return syscall(SYS_sched_setscheduler, (long)pid, (long)policy, &sp);
}

/**
 * Run in a child of root: become user 1000, and check that it may change
 * neither the policy nor the affinity of root's process. Returns 0, or the
 * number of the step that failed.
 */
static int not_root(void)
{
    cpu_set_t set;

    if (setresgid(1000, 1000, 1000) != 0 || setresuid(1000, 1000, 1000) != 0) {
        return 1;
    }
    if (set_policy(getppid(), SCHED_OTHER, 0) != -1 || errno != EPERM) {
        return 2;
    }
    CPU_ZERO(&set);
    CPU_SET(0, &set);
    if (sched_setaffinity(getppid(), sizeof(set), &set) != -1 || errno != EPERM) {
        return 3;
    }
    return 0;
}

/**
 * Check yielding, and the policy and parameter calls that ask for what every
 * process has: SCHED_OTHER at priority 0. Returns 0, or 1 after the failure is
 * printed.
 */
static int policy_calls(void)
{
    struct sched_param sp;

    if (sched_yield() != 0) {
        return fail("sched_yield did not return 0");
    }
    if (syscall(SYS_sched_getscheduler, 0L) != SCHED_OTHER) {
        return fail("the policy is not SCHED_OTHER");
    }
    sp.sched_priority = -1;
    if (syscall(SYS_sched_getparam, 0L, &sp) != 0 || sp.sched_priority != 0) {
        return fail("the priority is not 0");
    }
    if (set_policy(0, SCHED_OTHER, 0) != 0 ||
        set_policy(0, SCHED_OTHER | SCHED_RESET_ON_FORK, 0) != 0) {
        return fail("SCHED_OTHER at priority 0 was refused");
    }
    sp.sched_priority = 0;
    if (syscall(SYS_sched_setparam, 0L, &sp) != 0) {
        return fail("sched_setparam of priority 0 was refused");
    }
    return 0;
}

/**
 * Check that the policies and priorities this kernel does not run each fail
 * with EPERM or EINVAL. Returns 0, or 1 after the failure is printed.
 */
static int policy_refusals(void)
{
    static const int refused[] = {SCHED_BATCH, SCHED_IDLE, SCHED_DEADLINE, 4, 42};
    struct sched_param sp;
    unsigned int i;

    if (set_policy(0, SCHED_FIFO, 10) != -1 || errno != EPERM ||
        set_policy(0, SCHED_RR, 99) != -1 || errno != EPERM) {
        return fail("a real-time policy was not refused with EPERM");
    }
    if (set_policy(0, SCHED_FIFO, 0) != -1 || errno != EINVAL ||
        set_policy(0, SCHED_OTHER, 10) != -1 || errno != EINVAL ||
        set_policy(0, SCHED_RR, 100) != -1 || errno != EINVAL) {
        return fail("a priority that does not suit its policy was not refused with EINVAL");
    }
    for (i = 0; i < sizeof(refused) / sizeof(refused[0]); i++) {
        if (set_policy(0, refused[i], 0) != -1 || errno != EINVAL) {
            return fail("a policy this kernel does not run was not refused with EINVAL");
        }
    }
    sp.sched_priority = 5;
    if (syscall(SYS_sched_setparam, 0L, &sp) != -1 || errno != EINVAL) {
        return fail("sched_setparam of priority 5 was not refused with EINVAL");
    }
    return 0;
}

/**
 * Check the priority ranges, which are Linux's, and the time slice, one
 * tick. Returns 0, or 1 after the failure is printed.
 */
static int ranges(void)
{
    struct timespec slice;

    if (sched_get_priority_max(SCHED_FIFO) != 99 || sched_get_priority_min(SCHED_FIFO) != 1 ||
        sched_get_priority_max(SCHED_RR) != 99 || sched_get_priority_min(SCHED_RR) != 1 ||
        sched_get_priority_max(SCHED_OTHER) != 0 || sched_get_priority_min(SCHED_OTHER) != 0 ||
        sched_get_priority_max(SCHED_IDLE) != 0 || sched_get_priority_min(SCHED_BATCH) != 0) {
        return fail("a priority range is not Linux's");
    }
    if (sched_get_priority_max(42) != -1 || errno != EINVAL) {
        return fail("the range of an unknown policy was not refused with EINVAL");
    }
    if (sched_rr_get_interval(0, &slice) != 0 || slice.tv_sec != 0 || slice.tv_nsec != 10000000L) {
        return fail("the time slice is not one 10 ms tick");
    }
    return 0;
}

/**
 * Check the affinity: the processors /proc/cpuinfo lists, its raw size, a mask
 * of the last processor alone, which moves the caller there, and one of a
 * processor not started. The raw size is the mask's bytes as Linux rounds them,
 * to longs enough for the highest processor: four on i386 up to 32 processors
 * and eight past them, and eight on x86-64. Returns 0, or 1 after the failure
 * is printed.
 */
static int affinity(void)
{
    cpu_set_t set, online;
    unsigned int cpu = 99;
    int cpus, id, last = -1, offline = -1;
    long mask_bytes;

    cpus = cpuinfo_ids(&online);
    for (id = 0; id < 64; id++) {
        if (CPU_ISSET(id, &online)) {
            last = id;
        } else if (offline < 0) {
            offline = id;
        }
    }
    if (cpus < 1 || cpus > 64 || last >= 64) {
        return fail("/proc/cpuinfo lists no processor, or one past the 64 the kernel has");
    }
    mask_bytes = (long)((unsigned)last / (8u * sizeof(long)) + 1u) * (long)sizeof(long);
    memset(&set, 0xFF, sizeof(set));
    if (sched_getaffinity(0, sizeof(set), &set) != 0 || !CPU_EQUAL(&set, &online)) {
        return fail("the affinity is not the processors /proc/cpuinfo lists");
    }
    if (syscall(SYS_sched_getaffinity, 0L, (long)sizeof(set), &set) != mask_bytes) {
        return fail("the raw sched_getaffinity did not return the bytes its mask takes");
    }
    if (syscall(SYS_sched_getaffinity, 0L, 3L, &set) != -1 || errno != EINVAL) {
        return fail("an affinity of three bytes was not refused with EINVAL");
    }
    CPU_ZERO(&set);
    CPU_SET(last, &set);
    if (sched_setaffinity(0, sizeof(set), &set) != 0) {
        return fail("an affinity of the last processor was refused");
    }
    memset(&set, 0xFF, sizeof(set));
    if (sched_getaffinity(0, sizeof(set), &set) != 0 || CPU_COUNT(&set) != 1 ||
        !CPU_ISSET(last, &set)) {
        return fail("the affinity set is not the one read back");
    }
    if (syscall(SYS_getcpu, &cpu, NULL, NULL) != 0 || cpu != (unsigned)last) {
        return fail("the caller did not move to the one processor of its affinity");
    }
    CPU_ZERO(&set);
    CPU_SET(offline < 0 ? 64 : offline, &set);
    if (sched_setaffinity(0, sizeof(set), &set) != -1 || errno != EINVAL) {
        return fail("an affinity of a processor that is not started was not refused with EINVAL");
    }
    memset(&set, 0xFF, sizeof(set));
    if (sched_setaffinity(0, sizeof(set), &set) != 0) {
        return fail("an affinity of every processor was refused");
    }
    return 0;
}

/**
 * Check yielding, the policy calls, the ranges, the time slice and the
 * affinity, then the errors for root's process, for a pid of no process and
 * for a negative pid. Returns 0, or 1 after the failure is printed.
 */
static int scheduling(void)
{
    struct timespec slice;
    cpu_set_t set;

    if (policy_calls() != 0 || policy_refusals() != 0 || ranges() != 0 || affinity() != 0) {
        return 1;
    }
    if (in_child(not_root) != 0) {
        return fail("user 1000 changed the scheduling of root's process");
    }
    if (syscall(SYS_sched_getscheduler, 999999L) != -1 || errno != ESRCH ||
        sched_rr_get_interval(999999, &slice) != -1 || errno != ESRCH ||
        sched_getaffinity(999999, sizeof(set), &set) != -1 || errno != ESRCH) {
        return fail("a pid of no process was not refused with ESRCH");
    }
    if (syscall(SYS_sched_getscheduler, -1L) != -1 || errno != EINVAL) {
        return fail("a negative pid was not refused with EINVAL");
    }
    return 0;
}

/* The calling process's OOM score adjustment. */
#define SELF_OOM "/proc/self/oom_score_adj"

/**
 * Read the OOM score adjustment at `path` into `*v`. Returns 0, or -1 when
 * the file cannot be read or has no number and newline.
 */
static int oom_read(const char *path, int *v)
{
    char buf[16];
    ssize_t n;
    int fd = open(path, O_RDONLY);

    if (fd < 0) {
        return -1;
    }
    n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 1 || buf[n - 1] != '\n') {
        return -1;
    }
    buf[n] = '\0';
    *v = atoi(buf);
    return 0;
}

/**
 * Write `text` to the OOM score adjustment at `path`, opened as a shell's
 * redirection opens it. Returns 0, or -1 with errno set.
 */
static int oom_write(const char *path, const char *text)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    ssize_t n;
    int saved;

    if (fd < 0) {
        return -1;
    }
    n = write(fd, text, strlen(text));
    saved = errno;
    close(fd);
    errno = saved;
    return n == (ssize_t)strlen(text) ? 0 : -1;
}

/**
 * Give up root for user 1000 and become dumpable again, which makes the
 * process's /proc files its own. Returns 0, or -1.
 */
static int become_user(void)
{
    if (setresgid(1000, 1000, 1000) != 0 || setresuid(1000, 1000, 1000) != 0 ||
        prctl(PR_SET_DUMPABLE, 1, 0, 0, 0) != 0) {
        return -1;
    }
    return 0;
}

/**
 * Run in a child of root: check its adjustment's first value, mode and owner,
 * a write that reads back, a child that inherits it, and the range. Returns
 * 0, or the number of the row that failed, 1 to 5.
 */
static int root_rows(void)
{
    struct stat st;
    pid_t pid;
    int v, status;

    if (oom_read(SELF_OOM, &v) != 0 || v != 0) {
        return 1;
    }
    if (stat(SELF_OOM, &st) != 0 || (st.st_mode & 07777) != 0644 || st.st_uid != 0) {
        return 2;
    }
    if (oom_write(SELF_OOM, "-500\n") != 0 || oom_read(SELF_OOM, &v) != 0 || v != -500) {
        return 3;
    }
    pid = fork();
    if (pid == 0) {
        _exit(oom_read(SELF_OOM, &v) == 0 && v == -500 ? 0 : 1);
    }
    if (pid < 0 || waitpid(pid, &status, 0) != pid || !WIFEXITED(status) ||
        WEXITSTATUS(status) != 0) {
        return 4;
    }
    if (oom_write(SELF_OOM, "1001") != -1 || errno != EINVAL ||
        oom_write(SELF_OOM, "-1001") != -1 || errno != EINVAL || oom_write(SELF_OOM, "one") != -1 ||
        errno != EINVAL) {
        return 5;
    }
    return 0;
}

/**
 * Run in a child of root: set the adjustment to 100 and become user 1000,
 * and check that the user may raise it and lower it to that floor but not
 * below, and may not open root's process's file for writing. Returns 0, or
 * the number of the row that failed, 6 to 9.
 */
static int user_rows(void)
{
    char parent[64];
    int v;

    snprintf(parent, sizeof(parent), "/proc/%d/oom_score_adj", (int)getppid());
    if (oom_write(SELF_OOM, "100") != 0 || become_user() != 0) {
        return 6;
    }
    if (oom_write(SELF_OOM, "200") != 0 || oom_write(SELF_OOM, "100") != 0) {
        return 7;
    }
    if (oom_write(SELF_OOM, "50") != -1 || errno != EACCES || oom_read(SELF_OOM, &v) != 0 ||
        v != 100) {
        return 8;
    }
    if (open(parent, O_WRONLY) != -1 || errno != EACCES || oom_read(parent, &v) != 0) {
        return 9;
    }
    return 0;
}

/**
 * Run in a child of root: become user 1000 before any privileged write, and
 * check that the user may not lower its adjustment below 0. Returns 0, or 1
 * when it could, and 2 when it could not become the user.
 */
static int floor_zero(void)
{
    if (become_user() != 0) {
        return 2;
    }
    if (oom_write(SELF_OOM, "10") != 0 || oom_write(SELF_OOM, "-1") != -1 || errno != EACCES) {
        return 1;
    }
    return 0;
}

/**
 * Check the OOM score adjustment with `root_rows`, `user_rows` and
 * `floor_zero`, each in a child. Returns 0, or 1 after the failure is printed.
 */
static int oom_score(void)
{
    static const char *const rows[] = {
        "the OOM score adjustment does not read 0 at first",
        "oom_score_adj is not root's with mode 0644",
        "a write of the OOM score adjustment through a redirection did not read back",
        "a child did not inherit the OOM score adjustment",
        "an OOM score adjustment past -1000 to 1000 or no number was not refused with EINVAL",
        "root could not set the OOM score adjustment and become user 1000",
        "user 1000 could not raise its OOM score adjustment and lower it to the floor",
        "user 1000 lowered its OOM score adjustment below the floor root's write set",
        "user 1000 opened root's process's oom_score_adj for writing"};
    int rc = in_child(root_rows);

    if (rc == 0) {
        rc = in_child(user_rows);
    }
    if (rc != 0) {
        return fail(rc >= 1 && rc <= 9 ? rows[rc - 1] : "an OOM score child did not run");
    }
    rc = in_child(floor_zero);
    if (rc != 0) {
        return fail(rc == 1 ? "user 1000 lowered its OOM score adjustment below 0"
                            : "the second OOM score child did not run");
    }
    return 0;
}

/* ioprio_set and ioprio_get's which and classes, as Linux numbers them. */
#define IOPRIO_WHO_PROCESS 1
#define IOPRIO_CLASS_RT    1
#define IOPRIO_CLASS_BE    2
#define IOPRIO_CLASS_IDLE  3
#define IOPRIO(c, l)       ((c) << 13 | (l))

/**
 * Run in a child: return 0 when it inherited the I/O priority best effort 3,
 * and 1 otherwise.
 */
static int io_inherited(void)
{
    return syscall(SYS_ioprio_get, IOPRIO_WHO_PROCESS, 0) == IOPRIO(IOPRIO_CLASS_BE, 3) ? 0 : 1;
}

/**
 * Run in a child of root: become user 1000, and check that it may take the
 * idle class but not the real-time one, and may not change root's process.
 * Returns 0, or the number of the step that failed.
 */
static int io_unprivileged(void)
{
    if (setresgid(1000, 1000, 1000) != 0 || setresuid(1000, 1000, 1000) != 0) {
        return 1;
    }
    if (syscall(SYS_ioprio_set, IOPRIO_WHO_PROCESS, 0, IOPRIO(IOPRIO_CLASS_IDLE, 0)) != 0) {
        return 2;
    }
    if (syscall(SYS_ioprio_set, IOPRIO_WHO_PROCESS, 0, IOPRIO(IOPRIO_CLASS_RT, 0)) != -1 ||
        errno != EPERM) {
        return 3;
    }
    if (syscall(SYS_ioprio_set, IOPRIO_WHO_PROCESS, getppid(), IOPRIO(IOPRIO_CLASS_IDLE, 0)) !=
            -1 ||
        errno != EPERM) {
        return 4;
    }
    return 0;
}

/**
 * Check the I/O priority: its first value, kept, inherited, and the errors.
 * Returns 0, or 1 after the failure is printed.
 */
static int io_priorities(void)
{
    if (syscall(SYS_ioprio_get, IOPRIO_WHO_PROCESS, 0) != 0) {
        return fail("the first I/O priority is class none, level 0");
    }
    if (syscall(SYS_ioprio_set, IOPRIO_WHO_PROCESS, 0, IOPRIO(IOPRIO_CLASS_BE, 3)) != 0 ||
        syscall(SYS_ioprio_get, IOPRIO_WHO_PROCESS, 0) != IOPRIO(IOPRIO_CLASS_BE, 3)) {
        return fail("an I/O priority set and read back");
    }
    if (in_child(io_inherited) != 0) {
        return fail("a child inherits the I/O priority");
    }
    if (syscall(SYS_ioprio_set, IOPRIO_WHO_PROCESS, 0, IOPRIO(IOPRIO_CLASS_BE, 8)) != -1 ||
        errno != EINVAL || syscall(SYS_ioprio_set, IOPRIO_WHO_PROCESS, 0, 1) != -1 ||
        errno != EINVAL) {
        return fail("a level past 7 and class none with a level are EINVAL");
    }
    if (in_child(io_unprivileged) != 0) {
        return fail("another user's caller and the real-time class or root's process");
    }
    return 0;
}

/** Run the checks, or the new image's helper when --exec-group is given. */
int main(int argc, char **argv)
{
    if (argc == 2 && strcmp(argv[1], "--exec-group") == 0) {
        return exec_group_image();
    }
    if (thread_ids() != 0 || exec_groups(0) != 0 || exec_groups(1) != 0 || priorities() != 0 ||
        scheduling() != 0 || oom_score() != 0 || io_priorities() != 0) {
        return 1;
    }
    printf("SCHEDMUSL OK\n");

    return 0;
}
