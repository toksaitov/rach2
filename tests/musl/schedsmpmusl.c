/* Measure the fair scheduler on four processors: shares by nice value, policy
 * and session, wake waits, RR's turns, the fair server, a priority-inheriting
 * futex owner, balance, a stopped tick and a suspend to idle. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include "../probe_report.h"

extern char **environ;

#define NS_PER_MS  1000000LL
#define NS_PER_SEC 1000000000LL
#define MAX_KIDS   32

/* The warm-up and the span of a share's measurement, as the plan's Done
 * gives them: shares settle in the first, and are read over the second. */
#define WARM_NS  (2 * NS_PER_SEC)
#define SPAN_NS  (10 * NS_PER_SEC)
#define SHORT_NS (3 * NS_PER_SEC)

/* sched_prio_to_weight's entries for nice 0, 5 and 19, and SCHED_IDLE's. */
#define WEIGHT_NICE0  1024.0
#define WEIGHT_NICE5  335.0
#define WEIGHT_NICE19 15.0
#define WEIGHT_IDLE   3.0

/* The sleeper of the wake row: 1 ms asleep and 0.1 ms running (`run_for`),
 * this many times. */
#define WAKE_SLEEP_NS 1000000L
#define WAKE_RUN_NS   100000LL
#define WAKE_ROUNDS   2000

/* RR's slice, Linux's RR_TIMESLICE of 100 ms. */
#define RR_SLICE_NS (100 * NS_PER_MS)

/* The real-time priorities of the rows that use them, and the kernel's
 * priority of a thread of real-time priority p, MAX_RT_PRIO - 1 - p. */
#define RT_LOW   10
#define RT_HIGH  20
#define KPRIO(p) (99 - (p))

/* The owner of the priority-inheriting mutex gives up waiting to be seen
 * boosted after this much of its own processor time. */
#define PI_GIVEUP_NS NS_PER_SEC

/* What a process may run while a suspend to idle holds it. */
#define FROZEN_RUN_NS (200 * NS_PER_MS)
#define WAKE_AHEAD_S  3

#define RTC_RD_TIME   0x80247009u
#define RTC_WKALM_SET 0x4028700Fu

/* struct rtc_wkalrm: the bytes enabled and pending and two of padding, then
 * a struct rtc_time of nine ints. Only `enabled` is set. */
struct wkalrm {
    unsigned char flags[4];
    int tm[9];
};

/** The fields of /proc/PID/sched this test reads; -1 for one not shown. */
struct snap {
    long long exec_ns;    /* se.sum_exec_runtime */
    long long migrations; /* se.nr_migrations */
    long long nivcsw;     /* nr_involuntary_switches */
    long long wait_sum;   /* wait_sum, nanoseconds */
    long long wait_count; /* wait_count */
    long long prio;       /* prio, the kernel's 0 to 139 */
    long long policy;     /* policy */
};

/** What the end of a row does with a process the test made. */
enum fate {
    KILL,      /* kill it: a session's spinner, which its leader reaps */
    KILL_REAP, /* kill it and reap it: a child of this process */
    REAP       /* reap it: a session's leader, which exits once its spinners die */
};

/** A process this test made, and what the end of its row does with it. */
struct kid {
    pid_t pid;
    enum fate fate;
};

static struct kid kids[MAX_KIDS];
static int nkids;
static int saved_autogroup = -1, saved_schedstats = -1;

/** The RR row's record of turns, shared with its two spinners: the last to
 * run, and each one's count of the times it took over from the other. */
struct turns {
    volatile int last;
    volatile long count[2];
};
static struct turns *turns;
static long long ref_wait_ns = -1;
static int rows_done, failed;

static const char *const rows[] = {"setup",         "nice-shares", "idle-shares", "autogroup-on",
                                   "autogroup-off", "wakes",       "rr-turns",    "fair-server",
                                   "pi-owner",      "migration",   "nohz-pull",   "suspend",
                                   "cleanup"};
#define NROWS ((int)(sizeof(rows) / sizeof(rows[0])))
static int selected[NROWS];

static void kill_kids(void);

/** Write `v` to the sysctl file `path`; 0 or -1 with errno. */
static int write_int(const char *path, int v)
{
    char text[16];
    int fd = open(path, O_WRONLY), n, rc;

    if (fd < 0)
        return -1;
    n = snprintf(text, sizeof text, "%d\n", v);
    rc = write(fd, text, (size_t)n) == n ? 0 : -1;
    close(fd);
    return rc;
}

/** Read the integer the file `path` holds, or -1 when it cannot be read. */
static long long read_int(const char *path)
{
    char text[32];
    int fd = open(path, O_RDONLY);
    ssize_t n;

    if (fd < 0)
        return -1;
    n = read(fd, text, sizeof text - 1);
    close(fd);
    if (n <= 0)
        return -1;
    text[n] = '\0';
    return strtoll(text, NULL, 10);
}

/** Put back the sysctls this test changed, and stop every process it made. */
static void cleanup(void)
{
    kill_kids();
    if (saved_autogroup >= 0)
        write_int("/proc/sys/kernel/sched_autogroup_enabled", saved_autogroup);
    if (saved_schedstats >= 0)
        write_int("/proc/sys/kernel/sched_schedstats", saved_schedstats);
    saved_autogroup = saved_schedstats = -1;
}

/** Stop at once on SIGTERM, SIGINT or SIGHUP, stopping the loads first. */
static void caught(int signo)
{
    int i;

    for (i = 0; i < nkids; i++)
        kill(kids[i].pid, SIGKILL);
    signal(signo, SIG_DFL);
    raise(signo);
}

/** Report a failure that ends the test: the open row fails with `what` and
 * the errno, each row after it is not run, and the loads are stopped. */
static void fatal(const char *what)
{
    char why[200];
    int i;

    snprintf(why, sizeof why, "%s: %s", what, strerror(errno));
    fprintf(stderr, "schedsmp: %s\n", why);
    failed++;
    cleanup();
    if (report_part_id != 0)
        report_miss(why);
    report_part_end();
    for (i = rows_done + 1; i < NROWS; i++)
        report_notrun(rows[i], "an earlier check failed");
    report_end();
    exit(1);
}

/** Record that a check of the open row failed, printing what was seen. */
static void miss(const char *fmt, ...)
{
    char why[ETUDE_REPORT_LINE_MAX];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(why, sizeof why, fmt, ap);
    va_end(ap);
    printf("schedsmp: miss: %s: %s\n", report_part_id ? report_part_id : "-", why);
    report_miss(why);
    failed++;
}

/** Sleep `ns` nanoseconds, all of them, through signals. */
static void nap(long long ns)
{
    struct timespec t;

    t.tv_sec = (time_t)(ns / NS_PER_SEC);
    t.tv_nsec = (long)(ns % NS_PER_SEC);
    /* interval: a load runs for the span its share is measured over */
    while (nanosleep(&t, &t) != 0 && errno == EINTR) {
    }
}

/** The calling thread's own processor time in nanoseconds, which the kernel
 * counts. */
static long long thread_ns(void)
{
    struct timespec t;

    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t);
    return (long long)t.tv_sec * NS_PER_SEC + t.tv_nsec;
}

/**
 * Spin for `ns` nanoseconds of the monotonic clock. A short run is timed by
 * the monotonic clock, not the thread's processor clock, which Rach2 counts
 * in whole ticks of 4 ms, so that a run of 0.1 ms lasts 0.1 ms on every
 * kernel; on Linux the two agree here, since nothing preempts the thread
 * within so short a run.
 */
static void run_for(long long ns)
{
    struct timespec t;
    long long until;

    clock_gettime(CLOCK_MONOTONIC, &t);
    until = (long long)t.tv_sec * NS_PER_SEC + t.tv_nsec + ns;
    do {
        clock_gettime(CLOCK_MONOTONIC, &t);
    } while ((long long)t.tv_sec * NS_PER_SEC + t.tv_nsec < until);
}

/** Parse the value of a /proc/PID/sched line, `ms.ns` for a time (returned
 * in nanoseconds) or an integer. */
static long long sched_value(const char *s)
{
    long long whole, frac = 0;
    int neg = 0, digits = 0;
    char *end;

    while (*s == ' ')
        s++;
    if (*s == '-') {
        neg = 1;
        s++;
    }
    whole = strtoll(s, &end, 10);
    if (*end != '.')
        return neg ? -whole : whole;
    for (s = end + 1; *s >= '0' && *s <= '9' && digits < 6; s++, digits++)
        frac = frac * 10 + (*s - '0');
    while (digits++ < 6)
        frac *= 10;
    whole = whole * NS_PER_MS + frac;
    return neg ? -whole : whole;
}

/** Read the thread `tid` of the process `pid`'s /proc sched file into `s`;
 * `tid` 0 reads the process's own file. Returns 0, or -1 when the file
 * cannot be read or shows no run time. */
static int sched_read(pid_t pid, pid_t tid, struct snap *s)
{
    static const char *const names[] = {"se.sum_exec_runtime",
                                        "se.nr_migrations",
                                        "nr_involuntary_switches",
                                        "wait_sum",
                                        "wait_count",
                                        "prio",
                                        "policy"};
    long long *field[7];
    char path[64], text[8192], *line, *next;
    size_t len;
    ssize_t n, got = 0;
    int fd, i;

    field[0] = &s->exec_ns;
    field[1] = &s->migrations;
    field[2] = &s->nivcsw;
    field[3] = &s->wait_sum;
    field[4] = &s->wait_count;
    field[5] = &s->prio;
    field[6] = &s->policy;
    for (i = 0; i < 7; i++)
        *field[i] = -1;
    if (tid)
        snprintf(path, sizeof path, "/proc/%d/task/%d/sched", (int)pid, (int)tid);
    else
        snprintf(path, sizeof path, "/proc/%d/sched", (int)pid);
    fd = open(path, O_RDONLY);
    if (fd < 0)
        return -1;
    while ((n = read(fd, text + got, sizeof text - 1 - (size_t)got)) > 0)
        got += n;
    close(fd);
    text[got] = '\0';
    for (line = text; line && *line; line = next) {
        next = strchr(line, '\n');
        if (next)
            *next++ = '\0';
        for (i = 0; i < 7; i++) {
            len = strlen(names[i]);
            if (!strncmp(line, names[i], len) && (line[len] == ' ' || line[len] == ':')) {
                char *colon = strchr(line + len, ':');

                if (colon)
                    *field[i] = sched_value(colon + 1);
            }
        }
    }
    return s->exec_ns < 0 ? -1 : 0;
}

/** Read process `pid`'s run time, ending the test when it cannot be read. */
static long long exec_of(pid_t pid)
{
    struct snap s;

    if (sched_read(pid, 0, &s) != 0)
        fatal("read /proc/PID/sched");
    return s.exec_ns;
}

/** Remember the process `pid` for the row's end, which does `fate` to it. */
static void keep(pid_t pid, enum fate fate)
{
    if (nkids == MAX_KIDS) {
        kill(pid, SIGKILL);
        errno = ENOSPC;
        fatal("too many processes");
    }
    kids[nkids].pid = pid;
    kids[nkids].fate = fate;
    nkids++;
}

/** Kill every spinner the row made, and reap this process's children; a
 * session's leader reaps its spinners before it exits. */
static void kill_kids(void)
{
    int i, status;

    for (i = 0; i < nkids; i++)
        if (kids[i].fate != REAP)
            kill(kids[i].pid, SIGKILL);
    for (i = 0; i < nkids; i++) {
        if (kids[i].fate != KILL) {
            while (waitpid(kids[i].pid, &status, 0) < 0 && errno == EINTR) {
            }
        }
    }
    nkids = 0;
}

/** Set the calling thread's processors to the bits of `mask`. */
static int pin(unsigned mask)
{
    cpu_set_t set;
    int cpu;

    CPU_ZERO(&set);
    for (cpu = 0; cpu < 32; cpu++)
        if (mask & (1u << cpu))
            CPU_SET(cpu, &set);
    return sched_setaffinity(0, sizeof set, &set);
}

/** Set process `pid`'s processors to the bits of `mask`. */
static int pin_pid(pid_t pid, unsigned mask)
{
    cpu_set_t set;
    int cpu;

    CPU_ZERO(&set);
    for (cpu = 0; cpu < 32; cpu++)
        if (mask & (1u << cpu))
            CPU_SET(cpu, &set);
    return sched_setaffinity(pid, sizeof set, &set);
}

/** Give the calling thread `policy` at real-time priority `prio`, or, for
 * SCHED_OTHER, the nice value `nice`. */
static int set_class(int policy, int prio, int nice)
{
    struct sched_param sp;

    memset(&sp, 0, sizeof sp);
    if (policy == SCHED_OTHER)
        return setpriority(PRIO_PROCESS, 0, nice);
    sp.sched_priority = prio;
    /* musl's sched_setscheduler is a stub that fails with ENOSYS. */
    return (int)syscall(SYS_sched_setscheduler, 0, policy, &sp);
}

/**
 * Start a spinner, a child of the calling process that runs without end on
 * the processors of `mask` with the class that `policy`, `prio` and `nice`
 * give it, and dies with its parent. A spinner of `turn` 0 or 1 counts in
 * `turns` each time it runs after the other; -1 counts nothing. Returns its
 * pid once it spins, or -1 with errno.
 */
static pid_t spinner(unsigned mask, int policy, int prio, int nice, int turn)
{
    int ready[2], err = 0;
    pid_t parent = getpid(), pid;
    ssize_t n;

    if (pipe(ready) != 0)
        return -1;
    pid = fork();
    if (pid == 0) {
        volatile unsigned long spins = 0;

        close(ready[0]);
        errno = 0;
        if (prctl(PR_SET_PDEATHSIG, SIGKILL) != 0 || getppid() != parent || pin(mask) != 0 ||
            set_class(policy, prio, nice) != 0)
            err = errno ? errno : ESRCH;
        if (write(ready[1], &err, sizeof err) != (ssize_t)sizeof err || err)
            _exit(1);
        close(ready[1]);
        if (turn < 0)
            for (;;)
                spins++;
        for (;;) {
            if (turns->last != turn) {
                turns->last = turn;
                turns->count[turn]++;
            }
        }
    }
    close(ready[1]);
    if (pid < 0) {
        close(ready[0]);
        return -1;
    }
    n = read(ready[0], &err, sizeof err);
    close(ready[0]);
    if (n != (ssize_t)sizeof err || err) {
        kill(pid, SIGKILL);
        waitpid(pid, NULL, 0);
        errno = n == (ssize_t)sizeof err ? err : EPIPE;
        return -1;
    }
    return pid;
}

/** Start a spinner that counts no turns and keep it for the row's end, or
 * end the test. */
static pid_t spin(unsigned mask, int policy, int prio, int nice)
{
    pid_t pid = spinner(mask, policy, prio, nice, -1);

    if (pid < 0)
        fatal("start a spinner");
    keep(pid, KILL_REAP);
    return pid;
}

/**
 * Start a session of `n` nice-0 spinners on the processors of `mask`: a
 * leader that calls setsid, starts them as its children, reaps them once
 * they are killed and then exits. Their pids go into `pids`.
 */
static void session(int n, unsigned mask, pid_t *pids)
{
    int out[2], i, err;
    pid_t parent = getpid(), leader;

    if (pipe(out) != 0)
        fatal("a pipe for a session");
    leader = fork();
    if (leader == 0) {
        close(out[0]);
        if (prctl(PR_SET_PDEATHSIG, SIGKILL) != 0 || getppid() != parent || setsid() < 0)
            _exit(1);
        for (i = 0; i < n; i++) {
            pids[i] = spinner(mask, SCHED_OTHER, 0, 0, -1);
            if (pids[i] < 0)
                _exit(1);
        }
        if (write(out[1], pids, sizeof(pid_t) * (size_t)n) != (ssize_t)(sizeof(pid_t) * (size_t)n))
            _exit(1);
        close(out[1]);
        while (wait(NULL) > 0 || errno == EINTR) {
        }
        _exit(0);
    }
    close(out[1]);
    if (leader < 0)
        fatal("start a session");
    keep(leader, REAP);
    if (read(out[0], pids, sizeof(pid_t) * (size_t)n) != (ssize_t)(sizeof(pid_t) * (size_t)n)) {
        err = errno ? errno : EPIPE;
        close(out[0]);
        errno = err;
        fatal("start a session's spinners");
    }
    close(out[0]);
    for (i = 0; i < n; i++)
        keep(pids[i], KILL);
}

/** Measure the run time each of the `n` processes of `pids` gets over
 * `span` nanoseconds after `warm`, into `ns`. */
static void measure(const pid_t *pids, int n, long long warm, long long span, long long *ns)
{
    int i;

    nap(warm);
    for (i = 0; i < n; i++)
        ns[i] = exec_of(pids[i]);
    nap(span);
    for (i = 0; i < n; i++)
        ns[i] = exec_of(pids[i]) - ns[i];
}

/** Check that `got` is within `tol` of `want`, as a fraction of `want`;
 * `what` names the figure in the miss. */
static void near(const char *what, double got, double want, double tol)
{
    double off = got / want - 1.0;

    printf("schedsmp: figure: %s: %s got=%.4f want=%.4f off=%+.2f%%\n", report_part_id, what, got,
           want, off * 100.0);
    if (off > tol || off < -tol)
        miss("%s is %.4f, not %.4f within %.0f%%", what, got, want, tol * 100.0);
}

/** Check that every one of the `n` measured run times of `ns` is above 0. */
static void all_ran(const long long *ns, int n)
{
    int i;

    for (i = 0; i < n; i++)
        if (ns[i] <= 0)
            miss("load %d ran %lld ns", i, ns[i]);
}

/** nice 0 and nice 5 on processor 1 share it 1024:335. */
static void row_nice(void)
{
    pid_t p[2];
    long long ns[2];

    p[0] = spin(1u << 1, SCHED_OTHER, 0, 0);
    p[1] = spin(1u << 1, SCHED_OTHER, 0, 5);
    measure(p, 2, WARM_NS, SPAN_NS, ns);
    all_ran(ns, 2);
    if (ns[1] > 0)
        near("nice0:nice5", (double)ns[0] / (double)ns[1], WEIGHT_NICE0 / WEIGHT_NICE5, 0.05);
}

/** nice 19 and SCHED_IDLE on processor 2 share it 15:3. */
static void row_idle(void)
{
    pid_t p[2];
    long long ns[2];

    p[0] = spin(1u << 2, SCHED_OTHER, 0, 19);
    p[1] = spin(1u << 2, SCHED_IDLE, 0, 0);
    measure(p, 2, WARM_NS, SPAN_NS, ns);
    all_ran(ns, 2);
    if (ns[1] > 0)
        near("nice19:idle", (double)ns[0] / (double)ns[1], WEIGHT_NICE19 / WEIGHT_IDLE, 0.05);
}

/** Two sessions on processor 3, of one spinner and of four: half and half
 * with autogroup on, 1:4 with it off. The setting applies to the sessions
 * made after it, so it is written first. */
static void row_autogroup(int on)
{
    pid_t a[1], b[4], all[5];
    long long ns[5], sb;
    int i;

    if (saved_autogroup < 0) {
        miss("no /proc/sys/kernel/sched_autogroup_enabled");
        return;
    }
    if (write_int("/proc/sys/kernel/sched_autogroup_enabled", on) != 0)
        fatal("write sched_autogroup_enabled");
    session(1, 1u << 3, a);
    session(4, 1u << 3, b);
    all[0] = a[0];
    for (i = 0; i < 4; i++)
        all[i + 1] = b[i];
    measure(all, 5, WARM_NS, SPAN_NS, ns);
    all_ran(ns, 5);
    sb = ns[1] + ns[2] + ns[3] + ns[4];
    if (ns[0] > 0)
        near(on ? "four:one, autogroup on" : "four:one, autogroup off", (double)sb / (double)ns[0],
             on ? 1.0 : 4.0, 0.05);
}

/** The autogroup row with autogroup on. */
static void row_autogroup_on(void)
{
    row_autogroup(1);
}

/** The autogroup row with autogroup off. */
static void row_autogroup_off(void)
{
    row_autogroup(0);
}

/** A thread pinned to processor 1 that sleeps 1 ms and runs 0.1 ms, beside a
 * nice-0 spinner on each processor, waits on average no more than 25% above
 * the reference kernel's wait, or 0.5 ms above it, whichever is larger. */
static void row_wakes(void)
{
    int done[2], cpu, i;
    long long mean, bound;
    struct snap s;
    pid_t sleeper, parent = getpid();
    char c;

    saved_schedstats = (int)read_int("/proc/sys/kernel/sched_schedstats");
    if (saved_schedstats < 0 || write_int("/proc/sys/kernel/sched_schedstats", 1) != 0)
        fatal("set kernel.sched_schedstats to 1");
    for (cpu = 0; cpu < 4; cpu++)
        spin(1u << cpu, SCHED_OTHER, 0, 0);
    if (pipe(done) != 0)
        fatal("a pipe for the sleeper");
    sleeper = fork();
    if (sleeper == 0) {
        struct timespec t;

        close(done[0]);
        if (prctl(PR_SET_PDEATHSIG, SIGKILL) != 0 || getppid() != parent || pin(1u << 1) != 0)
            _exit(1);
        for (i = 0; i < WAKE_ROUNDS; i++) {
            t.tv_sec = 0;
            t.tv_nsec = WAKE_SLEEP_NS;
            /* interval: the sleeper's 1 ms sleep is the workload */
            nanosleep(&t, NULL);
            run_for(WAKE_RUN_NS);
        }
        if (write(done[1], "d", 1) != 1)
            _exit(1);
        pause();
        _exit(0);
    }
    close(done[1]);
    if (sleeper < 0)
        fatal("start the sleeper");
    keep(sleeper, KILL_REAP);
    if (read(done[0], &c, 1) != 1) {
        close(done[0]);
        fatal("the sleeper's rounds");
    }
    close(done[0]);
    if (sched_read(sleeper, 0, &s) != 0 || s.wait_count <= 0 || s.wait_sum < 0) {
        miss("no wait_sum and wait_count in the sleeper's /proc/PID/sched");
        return;
    }
    mean = s.wait_sum / s.wait_count;
    printf(
        "schedsmp: figure: wakes: wait_sum=%lld wait_count=%lld mean_ns=%lld reference_ns=%lld\n",
        s.wait_sum, s.wait_count, mean, ref_wait_ns);
    if (s.wait_count < WAKE_ROUNDS)
        miss("wait_count is %lld after %d wakes", s.wait_count, WAKE_ROUNDS);
    if (ref_wait_ns < 0) {
        miss("no reference wait given with -w");
        return;
    }
    bound = ref_wait_ns + ref_wait_ns / 4;
    if (bound < ref_wait_ns + WAKE_SLEEP_NS / 2)
        bound = ref_wait_ns + WAKE_SLEEP_NS / 2;
    if (mean > bound)
        miss("the sleeper's mean wait is %lld ns, past %lld ns", mean, bound);
}

/** sched_rr_get_interval and sched_rr_timeslice_ms give 100 ms, and two equal
 * SCHED_RR spinners on processor 3 share it in turns of 100 ms: each one's
 * run time over the times it took over from the other. A preemption by the
 * fair server or a kernel thread switches a spinner out without a turn, so
 * nr_involuntary_switches is not the count. */
static void row_rr(void)
{
    struct timespec ts;
    pid_t p[2];
    long long slice = read_int("/proc/sys/kernel/sched_rr_timeslice_ms"), ns[2];
    long before[2], took;
    int i;

    if (slice != 100)
        miss("sched_rr_timeslice_ms reads %lld", slice);
    turns = mmap(NULL, sizeof *turns, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (turns == MAP_FAILED) {
        turns = NULL;
        fatal("map the turns' record");
    }
    turns->last = -1;
    for (i = 0; i < 2; i++) {
        p[i] = spinner(1u << 3, SCHED_RR, RT_LOW, 0, i);
        if (p[i] < 0)
            fatal("start an RR spinner");
        keep(p[i], KILL_REAP);
    }
    if (sched_rr_get_interval(p[0], &ts) != 0)
        fatal("sched_rr_get_interval");
    if ((long long)ts.tv_sec * NS_PER_SEC + ts.tv_nsec != RR_SLICE_NS)
        miss("sched_rr_get_interval gives %lld.%09ld s", (long long)ts.tv_sec, ts.tv_nsec);
    nap(NS_PER_SEC / 2);
    for (i = 0; i < 2; i++) {
        before[i] = turns->count[i];
        ns[i] = exec_of(p[i]);
    }
    nap(2 * SHORT_NS);
    for (i = 0; i < 2; i++)
        ns[i] = exec_of(p[i]) - ns[i];
    for (i = 0; i < 2; i++) {
        took = turns->count[i] - before[i];
        printf("schedsmp: figure: rr-turns: spinner %d ran %lld ns in %ld turns\n", i, ns[i], took);
        if (ns[i] <= 0 || took <= 0) {
            miss("RR spinner %d ran %lld ns in %ld turns", i, ns[i], took);
            continue;
        }
        near(i ? "second spinner's turn, ns" : "first spinner's turn, ns",
             (double)ns[i] / (double)took, (double)RR_SLICE_NS, 0.10);
    }
    if (ns[1] > 0)
        near("RR share", (double)ns[0] / (double)ns[1], 1.0, 0.05);
    kill_kids();
    munmap(turns, sizeof *turns);
    turns = NULL;
}

/** A SCHED_FIFO spinner on processor 2 leaves a nice-0 spinner beside it 5%
 * of the processor, within one percentage point: the fair server's 50 ms of
 * each 1000 ms. */
static void row_fair_server(void)
{
    pid_t p[2];
    long long ns[2];
    double share;

    p[0] = spin(1u << 2, SCHED_OTHER, 0, 0);
    p[1] = spin(1u << 2, SCHED_FIFO, RT_LOW, 0);
    measure(p, 2, WARM_NS, SPAN_NS, ns);
    all_ran(ns, 2);
    if (ns[0] + ns[1] <= 0)
        return;
    share = (double)ns[0] / (double)(ns[0] + ns[1]);
    printf("schedsmp: figure: fair-server: ordinary=%lld fifo=%lld share=%.4f\n", ns[0], ns[1],
           share);
    if (share < 0.04 || share > 0.06)
        miss("the ordinary spinner had %.2f%% of the processor, not 5%% within one point",
             share * 100.0);
}

/* The priority-inheritance row's mutex and its threads' state. */
static pthread_mutex_t pi_lock;
static volatile int pi_locked, pi_waiting, pi_seen, pi_released, pi_acquired, pi_error;
static volatile pid_t pi_owner_tid;

/** The owner: an ordinary thread on processor 1 that takes the mutex, then
 * runs until the main thread has seen it boosted, or for PI_GIVEUP_NS of its
 * own time, and lets the mutex go. */
static void *pi_owner(void *arg)
{
    long long until;

    (void)arg;
    pi_owner_tid = (pid_t)syscall(SYS_gettid);
    if (pin(1u << 1) != 0 || pthread_mutex_lock(&pi_lock) != 0) {
        pi_error = errno ? errno : EINVAL;
        return NULL;
    }
    __atomic_store_n(&pi_locked, 1, __ATOMIC_RELEASE);
    while (!__atomic_load_n(&pi_waiting, __ATOMIC_ACQUIRE)) {
    }
    until = thread_ns() + PI_GIVEUP_NS;
    while (!__atomic_load_n(&pi_seen, __ATOMIC_ACQUIRE) && thread_ns() < until) {
    }
    __atomic_store_n(&pi_released, 1, __ATOMIC_RELEASE);
    pthread_mutex_unlock(&pi_lock);
    return NULL;
}

/** The waiter: a SCHED_FIFO thread on processor 1 above the spinner there,
 * which blocks on the owner's mutex. It takes its class before it moves, so
 * that the spinner does not hold it off. */
static void *pi_waiter(void *arg)
{
    (void)arg;
    if (set_class(SCHED_FIFO, RT_HIGH, 0) != 0 || pin(1u << 1) != 0) {
        pi_error = errno ? errno : EINVAL;
        __atomic_store_n(&pi_waiting, 1, __ATOMIC_RELEASE);
        return NULL;
    }
    __atomic_store_n(&pi_waiting, 1, __ATOMIC_RELEASE);
    if (pthread_mutex_lock(&pi_lock) != 0) {
        pi_error = errno ? errno : EINVAL;
        return NULL;
    }
    __atomic_store_n(&pi_acquired, 1, __ATOMIC_RELEASE);
    pthread_mutex_unlock(&pi_lock);
    return NULL;
}

/** An ordinary thread holding a priority-inheriting futex, held off processor
 * 1 by a SCHED_FIFO spinner, inherits the priority of a SCHED_FIFO waiter
 * above it, runs and lets the mutex go to the waiter. */
static void row_pi(void)
{
    pthread_mutexattr_t ma;
    pthread_t owner, waiter;
    struct snap s;
    long long boosted = -1, policy = -1;

    pi_locked = pi_waiting = pi_seen = pi_released = pi_acquired = pi_error = 0;
    if (pthread_mutexattr_init(&ma) != 0 ||
        pthread_mutexattr_setprotocol(&ma, PTHREAD_PRIO_INHERIT) != 0 ||
        pthread_mutex_init(&pi_lock, &ma) != 0)
        fatal("a priority-inheriting mutex");
    pthread_mutexattr_destroy(&ma);
    if (pthread_create(&owner, NULL, pi_owner, NULL) != 0)
        fatal("start the owner");
    while (!__atomic_load_n(&pi_locked, __ATOMIC_ACQUIRE) && !pi_error) {
        sched_yield();
    }
    spin(1u << 1, SCHED_FIFO, RT_LOW, 0);
    if (pthread_create(&waiter, NULL, pi_waiter, NULL) != 0)
        fatal("start the waiter");
    while (!__atomic_load_n(&pi_waiting, __ATOMIC_ACQUIRE)) {
        sched_yield();
    }
    while (!pi_error && !__atomic_load_n(&pi_released, __ATOMIC_ACQUIRE)) {
        if (sched_read(getpid(), pi_owner_tid, &s) == 0 && s.prio == KPRIO(RT_HIGH)) {
            boosted = s.prio;
            policy = s.policy;
            __atomic_store_n(&pi_seen, 1, __ATOMIC_RELEASE);
            break;
        }
        sched_yield();
    }
    pthread_join(waiter, NULL);
    pthread_join(owner, NULL);
    kill_kids();
    pthread_mutex_destroy(&pi_lock);
    printf("schedsmp: figure: pi-owner: prio=%lld policy=%lld seen=%d acquired=%d\n", boosted,
           policy, pi_seen, pi_acquired);
    if (pi_error) {
        errno = pi_error;
        fatal("the owner's or the waiter's setup");
    }
    if (!pi_seen)
        miss("the owner's prio never read %d while the waiter waited", KPRIO(RT_HIGH));
    else if (policy != SCHED_OTHER)
        miss("the boosted owner's policy reads %lld, not SCHED_OTHER", policy);
    if (!pi_acquired)
        miss("the waiter did not take the mutex");
}

/** Eight spinners started on processor 1 and then let run on all four spread
 * out: together they get four processors' worth of run time within 10%, each
 * gets within 25% of their mean, and six or more moved. */
static void row_migration(void)
{
    pid_t p[8];
    long long ns[8], total = 0, moved = 0, lo, hi;
    struct snap s;
    int i;

    for (i = 0; i < 8; i++)
        p[i] = spin(1u << 1, SCHED_OTHER, 0, 0);
    for (i = 0; i < 8; i++)
        if (pin_pid(p[i], 0xFu) != 0)
            fatal("widen a spinner's processors");
    measure(p, 8, WARM_NS, 2 * SHORT_NS, ns);
    all_ran(ns, 8);
    for (i = 0; i < 8; i++) {
        total += ns[i];
        if (sched_read(p[i], 0, &s) == 0 && s.migrations > 0)
            moved += s.migrations;
    }
    lo = hi = ns[0];
    for (i = 1; i < 8; i++) {
        lo = ns[i] < lo ? ns[i] : lo;
        hi = ns[i] > hi ? ns[i] : hi;
    }
    printf("schedsmp: figure: migration: total=%lld least=%lld most=%lld migrations=%lld\n", total,
           lo, hi, moved);
    near("processors' worth of run time", (double)total / (double)(2 * SHORT_NS), 4.0, 0.10);
    near("least run time over the mean", (double)lo * 8.0 / (double)total, 1.0, 0.25);
    near("most run time over the mean", (double)hi * 8.0 / (double)total, 1.0, 0.25);
    if (moved < 6)
        miss("the spinners migrated %lld times, fewer than the six that must leave", moved);
}

/** With processor 2 idle and its tick stopped, a spinner let run on
 * processors 1 and 2 beside one pinned to processor 1 is pulled to
 * processor 2: it migrates, and both then run at least 90% of the span,
 * which one processor could not give them both. */
static void row_nohz(void)
{
    pid_t p[2];
    long long ns[2];
    struct snap s;

    p[0] = spin(1u << 1, SCHED_OTHER, 0, 0);
    p[1] = spin(1u << 1, SCHED_OTHER, 0, 0);
    if (pin_pid(p[1], (1u << 1) | (1u << 2)) != 0)
        fatal("widen a spinner's processors");
    measure(p, 2, NS_PER_SEC, SHORT_NS, ns);
    all_ran(ns, 2);
    if (sched_read(p[1], 0, &s) != 0)
        fatal("read the moved spinner's /proc/PID/sched");
    printf("schedsmp: figure: nohz-pull: pinned=%lld movable=%lld migrations=%lld\n", ns[0], ns[1],
           s.migrations);
    if (s.migrations < 1)
        miss("the movable spinner never migrated");
    near("pinned spinner's share of the span", (double)ns[0] / (double)SHORT_NS, 1.0, 0.10);
    near("movable spinner's share of the span", (double)ns[1] / (double)SHORT_NS, 1.0, 0.10);
}

/** The seconds since the epoch of the struct rtc_time `tm`, in UTC. */
static time_t rtc_seconds(const int *tm)
{
    struct tm t;

    memset(&t, 0, sizeof t);
    t.tm_sec = tm[0];
    t.tm_min = tm[1];
    t.tm_hour = tm[2];
    t.tm_mday = tm[3];
    t.tm_mon = tm[4];
    t.tm_year = tm[5];
    return timegm(&t);
}

/** Set the wake alarm of /dev/rtc0, open as `fd`, `ahead` seconds from the
 * clock's time. Returns ioctl's result. */
static int wake_in(int fd, int ahead)
{
    struct wkalrm w;
    struct tm t;
    time_t at;

    memset(&w, 0, sizeof w);
    if (ioctl(fd, RTC_RD_TIME, w.tm) != 0)
        return -1;
    at = rtc_seconds(w.tm) + ahead;
    gmtime_r(&at, &t);
    w.flags[0] = 1;
    w.tm[0] = t.tm_sec;
    w.tm[1] = t.tm_min;
    w.tm[2] = t.tm_hour;
    w.tm[3] = t.tm_mday;
    w.tm[4] = t.tm_mon;
    w.tm[5] = t.tm_year;
    return ioctl(fd, RTC_WKALM_SET, &w);
}

/** A suspend to idle while two spinners share processor 1, one of them
 * queued, and a thread on processor 2 runs briefly between sleeps beside a
 * spinner, so that it sleeps delayed: no load runs while the processes are
 * held, and afterwards each runs again, the two on processor 1 half and
 * half. */
static void row_suspend(void)
{
    pid_t p[4], parent = getpid(), sleeper;
    long long before[4], during[4], ns[4];
    int rtc, state, i;

    p[0] = spin(1u << 1, SCHED_OTHER, 0, 0);
    p[1] = spin(1u << 1, SCHED_OTHER, 0, 0);
    p[2] = spin(1u << 2, SCHED_OTHER, 0, 0);
    sleeper = fork();
    if (sleeper == 0) {
        struct timespec t;

        if (prctl(PR_SET_PDEATHSIG, SIGKILL) != 0 || getppid() != parent || pin(1u << 2) != 0)
            _exit(1);
        for (;;) {
            run_for(5 * WAKE_RUN_NS);
            t.tv_sec = 0;
            t.tv_nsec = WAKE_SLEEP_NS / 5;
            /* interval: the delayed thread's sleep is the workload */
            nanosleep(&t, NULL);
        }
    }
    if (sleeper < 0)
        fatal("start the delayed thread");
    keep(sleeper, KILL_REAP);
    p[3] = sleeper;
    nap(NS_PER_SEC);
    rtc = open("/dev/rtc0", O_RDONLY);
    state = open("/sys/power/state", O_WRONLY);
    if (rtc < 0 || state < 0 || wake_in(rtc, WAKE_AHEAD_S) != 0)
        fatal("/dev/rtc0's wake alarm and /sys/power/state");
    for (i = 0; i < 4; i++)
        before[i] = exec_of(p[i]);
    if (write(state, "freeze", 6) != 6)
        fatal("write freeze to /sys/power/state");
    for (i = 0; i < 4; i++)
        during[i] = exec_of(p[i]) - before[i];
    close(state);
    close(rtc);
    printf("schedsmp: figure: suspend: held run ns %lld %lld %lld %lld\n", during[0], during[1],
           during[2], during[3]);
    for (i = 0; i < 4; i++)
        if (during[i] > FROZEN_RUN_NS)
            miss("load %d ran %lld ns while the suspend held it", i, during[i]);
    measure(p, 4, 0, SHORT_NS, ns);
    all_ran(ns, 4);
    if (ns[1] > 0)
        near("processor 1's halves after the resume", (double)ns[0] / (double)ns[1], 1.0, 0.10);
}

/** Processors 0 to 3 are allowed, the test runs as root, and /proc/self/sched
 * shows the run time. The main thread stays on processor 0. */
static void row_setup(void)
{
    struct utsname name;
    struct snap self;
    cpu_set_t set;
    int k;

    if (uname(&name) != 0)
        fatal("uname");
    printf("schedsmp: start: %s %s %s\n", name.sysname, name.release, name.machine);
    if (sched_getaffinity(0, sizeof set, &set) != 0)
        fatal("sched_getaffinity");
    for (k = 0; k < 4; k++)
        if (!CPU_ISSET(k, &set)) {
            errno = ENODEV;
            fatal("processors 0 to 3 are not all allowed");
        }
    if (geteuid() != 0) {
        errno = EPERM;
        fatal("the test needs root");
    }
    if (pin(1u << 0) != 0)
        fatal("pin the main thread to processor 0");
    if (sched_read(getpid(), 0, &self) != 0)
        fatal("read /proc/self/sched");
    saved_autogroup = (int)read_int("/proc/sys/kernel/sched_autogroup_enabled");
    printf("schedsmp: setup: autogroup=%d schedstats=%lld base_slice_ns=%lld\n", saved_autogroup,
           read_int("/proc/sys/kernel/sched_schedstats"),
           read_int("/sys/kernel/debug/sched/base_slice_ns"));
}

/** Every process the test made is gone and reaped, and the sysctls it
 * changed are put back. */
static void row_cleanup(void)
{
    cleanup();
    while (wait(NULL) > 0 || errno == EINTR) {
    }
    if (errno != ECHILD)
        miss("a child is left: %s", strerror(errno));
}

/** Each row's function, in the order of rows[]. */
static void (*const row_fns[NROWS])(void) = {
    row_setup, row_nice,    row_idle,        row_autogroup_on, row_autogroup_off,
    row_wakes, row_rr,      row_fair_server, row_pi,           row_migration,
    row_nohz,  row_suspend, row_cleanup};

/** Begin row `i` when it is selected; report it not run otherwise. */
static int row(int i)
{
    rows_done = i;
    if (!selected[i]) {
        report_part_end();
        report_notrun(rows[i], "not selected on the command line");
        return 0;
    }
    report_part(rows[i]);
    printf("schedsmp: row: %s\n", rows[i]);
    return 1;
}

/** Read the reference wait of -w and the rows named on the command line;
 * with none named, every row runs, and setup and cleanup always do. */
static void parse_args(int argc, char **argv)
{
    int i, k, opt, any = 0;

    while ((opt = getopt(argc, argv, "w:")) != -1) {
        if (opt != 'w') {
            errno = EINVAL;
            fatal("usage: schedsmpmusl [-w reference-wait-ns] [row...]");
        }
        ref_wait_ns = strtoll(optarg, NULL, 10);
    }
    for (i = optind; i < argc; i++) {
        for (k = 0; k < NROWS && strcmp(argv[i], rows[k]); k++) {
        }
        if (k == NROWS) {
            errno = EINVAL;
            fatal(argv[i]);
        }
        selected[k] = any = 1;
    }
    for (k = 0; k < NROWS; k++)
        selected[k] = !any || selected[k] || k == 0 || k == NROWS - 1;
}

int main(int argc, char **argv)
{
    struct sigaction sa;
    int i;

    setvbuf(stdout, NULL, _IOLBF, 0);
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = caught;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGHUP, &sa, NULL);
    report_begin(report_case(environ, "guest.sched4.fair"));
    parse_args(argc, argv);
    atexit(cleanup);
    for (i = 0; i < NROWS; i++) {
        if (row(i)) {
            row_fns[i]();
            kill_kids();
            report_progress("rows", i + 1);
        }
    }
    rows_done = NROWS;
    report_part_end();
    report_end();
    if (failed) {
        printf("schedsmp: result: %d checks missed\n", failed);
        return 1;
    }
    puts("SCHEDSMP OK");
    return 0;
}
