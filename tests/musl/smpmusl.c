/**
 * The processors, as a program sees and uses them through musl.
 *
 * The argument is the number of processors the boot should have. That many
 * are listed by /proc/cpuinfo, are in the caller's affinity, are counted by
 * `sysconf(_SC_NPROCESSORS_ONLN)`, are in /sys/devices/system/cpu/online as
 * `0` or `0-N`, have a `cpuN` directory there, each of its own inode, and a
 * `cpuN` line in /proc/stat, and none past the last has either. A process
 * bound to each processor in turn runs there, as `getcpu` says.
 *
 * Then each part of a stress across the processors is counted. On each
 * processor a child bound there forks and execs this program twenty times,
 * and each exec's exit status is the one the program gives. Two processes
 * on two processors, the first and the last, pass a counter back and forth
 * through two pipes, each checking every value it reads. Two threads on the
 * two take turns through a futex word, which ends at twice the turns. A
 * thread on the first reads a page in a loop while a thread on the last
 * unmaps it, and the reader must take SIGSEGV at that page: a translation
 * the reader's processor kept would let it read on, which the row bounds by
 * a count of reads after the unmap returned. On one processor the two of
 * each pair share it, and each row still runs.
 *
 * Last, sleeps on the last processor, whose own timer runs out of step with
 * the boot processor's tick, must not end early: each is timed by the
 * time-stamp counter, measured against the clock on the first processor,
 * since a clock read wrong on the last processor would time its own sleeps
 * wrong too, and must last its request less the slack of the clock's
 * reading. A sleep ends at a tick, so each begins after a spin of its own
 * length, at a different place in the tick.
 *
 * Prints "SMPMUSL OK (N processors)" on success and "SMPMUSL FAIL (detail)"
 * otherwise.
 */

#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <setjmp.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* The futex operations, which musl's headers do not name. */
#define FUTEX_WAIT 0
#define FUTEX_WAKE 1

#define EXECS_PER_CPU 20
#define PIPE_TURNS    2000
#define FUTEX_TURNS   2000

/* The reads the munmap row allows after the unmap returned, before it
 * calls the page still readable. */
#define READS_AFTER_UNMAP 2000000L

/* The status an exec of this program with "child" exits with. */
#define CHILD_STATUS 7

/* The sleeps row: how many, how long the counter is measured for, and how
 * much shorter than its request a sleep may seem, for the clock's reading
 * and the measurement's own. */
#define SLEEPS      60
#define MEASURE_NS  200000000LL
#define SLEEP_SLACK 100000LL

static int cpus;
/* What a thread returns when it could not be bound to its processor. */
static char unbound;
static const char *self;

/** Report one failed check and return 1, the probe's failure status. */
static int fail(const char *what)
{
    printf("SMPMUSL FAIL (%s: errno %d)\n", what, errno);
    return 1;
}

/** Bind the calling thread to processor `cpu`: 0, or -1. */
static int bind_to(int cpu)
{
    cpu_set_t set;

    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    return sched_setaffinity(0, sizeof(set), &set);
}

/** The processor the caller runs on, or -1. */
static int running_on(void)
{
    unsigned cpu = 0;

    return syscall(SYS_getcpu, &cpu, NULL, NULL) == 0 ? (int)cpu : -1;
}

/** The lines of `path` that begin with `prefix`, or -1. */
static int count_lines(const char *path, const char *prefix)
{
    char line[256];
    int n = 0;
    FILE *f = fopen(path, "r");

    if (!f) {
        return -1;
    }
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, prefix, strlen(prefix)) == 0) {
            n++;
        }
    }
    fclose(f);
    return n;
}

/** Whether /proc/stat has a line that begins with `name` and a space. */
static int stat_has(const char *name)
{
    char prefix[16];

    snprintf(prefix, sizeof(prefix), "%s ", name);
    return count_lines("/proc/stat", prefix) == 1;
}

/** The processors seen, each way a program asks. */
static int counts(void)
{
    char want[16], got[32], name[64];
    ino_t inos[65];
    cpu_set_t set;
    struct stat st;
    FILE *f;
    int c, k;

    if (count_lines("/proc/cpuinfo", "processor\t:") != cpus) {
        return fail("/proc/cpuinfo does not list the processors");
    }
    if (sched_getaffinity(0, sizeof(set), &set) != 0 || CPU_COUNT(&set) != cpus) {
        return fail("the affinity is not every processor");
    }
    if (sysconf(_SC_NPROCESSORS_ONLN) != cpus) {
        return fail("sysconf does not count the processors");
    }
    if (cpus == 1) {
        snprintf(want, sizeof(want), "0\n");
    } else {
        snprintf(want, sizeof(want), "0-%d\n", cpus - 1);
    }
    f = fopen("/sys/devices/system/cpu/online", "r");
    if (!f || !fgets(got, sizeof(got), f) || strcmp(got, want) != 0) {
        return fail("/sys/devices/system/cpu/online does not list the processors");
    }
    fclose(f);
    for (c = 0; c <= cpus; c++) {
        snprintf(name, sizeof(name), "/sys/devices/system/cpu/cpu%d", c);
        if ((stat(name, &st) == 0 && S_ISDIR(st.st_mode)) != (c < cpus)) {
            return fail("a cpuN directory is missing, or one past the last is there");
        }
        for (k = 0; c < cpus && k < c; k++) {
            if (inos[k] == st.st_ino) {
                return fail("two cpuN directories have one inode");
            }
        }
        inos[c] = st.st_ino;
        snprintf(name, sizeof(name), "cpu%d", c);
        if (stat_has(name) != (c < cpus)) {
            return fail("a cpuN line of /proc/stat is missing, or one past the last is there");
        }
    }
    for (c = 0; c < cpus; c++) {
        if (bind_to(c) != 0 || running_on() != c) {
            return fail("a process bound to a processor does not run there");
        }
    }
    CPU_ZERO(&set);
    for (c = 0; c < cpus; c++) {
        CPU_SET(c, &set);
    }
    if (sched_setaffinity(0, sizeof(set), &set) != 0) {
        return fail("the affinity of every processor was refused");
    }
    return 0;
}

/** On each processor, a child bound there that forks and execs this
 * program EXECS_PER_CPU times and counts the exits. */
static int execs(void)
{
    int c, status;

    for (c = 0; c < cpus; c++) {
        pid_t pid = fork();

        if (pid < 0) {
            return fail("fork");
        }
        if (pid == 0) {
            int i, done = 0;

            if (bind_to(c) != 0) {
                _exit(2);
            }
            for (i = 0; i < EXECS_PER_CPU; i++) {
                pid_t kid = fork();

                if (kid == 0) {
                    execl(self, self, "child", (char *)0);
                    _exit(3);
                }
                if (kid > 0 && waitpid(kid, &status, 0) == kid && WIFEXITED(status) &&
                    WEXITSTATUS(status) == CHILD_STATUS) {
                    done++;
                }
            }
            _exit(done == EXECS_PER_CPU ? 0 : 1);
        }
    }
    for (c = 0; c < cpus; c++) {
        if (wait(&status) < 0 || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
            return fail("an exec on a processor did not exit as the program does");
        }
    }
    return 0;
}

/** A counter passed back and forth through two pipes by a process on the
 * first processor and one on the last, each checking every value. */
static int pipes(void)
{
    int there[2], back[2], status, i, v;
    pid_t pid;

    if (pipe(there) != 0 || pipe(back) != 0) {
        return fail("pipe");
    }
    pid = fork();
    if (pid < 0) {
        return fail("fork");
    }
    if (pid == 0) {
        if (bind_to(cpus - 1) != 0) {
            _exit(2);
        }
        for (i = 0; i < PIPE_TURNS; i++) {
            if (read(there[0], &v, sizeof(v)) != sizeof(v) || v != 2 * i) {
                _exit(1);
            }
            v++;
            if (write(back[1], &v, sizeof(v)) != sizeof(v)) {
                _exit(1);
            }
        }
        _exit(0);
    }
    if (bind_to(0) != 0) {
        return fail("bind to the first processor");
    }
    for (i = 0; i < PIPE_TURNS; i++) {
        v = 2 * i;
        if (write(there[1], &v, sizeof(v)) != sizeof(v) ||
            read(back[0], &v, sizeof(v)) != sizeof(v) || v != 2 * i + 1) {
            return fail("a value through the pipes was not the one sent");
        }
    }
    if (waitpid(pid, &status, 0) != pid || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        return fail("the pipes' other end did not read every value");
    }
    close(there[0]);
    close(there[1]);
    close(back[0]);
    close(back[1]);
    return 0;
}

static int futex_word;

static void futex_turns(int parity)
{
    int i;

    for (i = 0; i < FUTEX_TURNS; i++) {
        int v;

        while (((v = __atomic_load_n(&futex_word, __ATOMIC_SEQ_CST)) & 1) != parity) {
            syscall(SYS_futex, &futex_word, FUTEX_WAIT, v, NULL, NULL, 0);
        }
        __atomic_add_fetch(&futex_word, 1, __ATOMIC_SEQ_CST);
        syscall(SYS_futex, &futex_word, FUTEX_WAKE, 1, NULL, NULL, 0);
    }
}

static void *futex_last(void *arg)
{
    (void)arg;
    if (bind_to(cpus - 1) != 0) {
        return &unbound;
    }
    futex_turns(1);
    return NULL;
}

/** Two threads on the first and the last processor taking turns through a
 * futex word, which ends at twice the turns. */
static int futexes(void)
{
    pthread_t t;
    void *rc;

    futex_word = 0;
    if (pthread_create(&t, NULL, futex_last, NULL) != 0) {
        return fail("pthread_create");
    }
    if (bind_to(0) != 0) {
        return fail("bind to the first processor");
    }
    futex_turns(0);
    if (pthread_join(t, &rc) != 0 || rc != NULL) {
        return fail("the futex's other thread");
    }
    if (futex_word != 2 * FUTEX_TURNS) {
        return fail("the futex word is not twice the turns");
    }
    return 0;
}

static volatile int *page;
static volatile int reading, unmapped, faulted;
static volatile void *fault_at;
static sigjmp_buf reader_env;

static void segv(int signo, siginfo_t *info, void *context)
{
    (void)signo;
    (void)context;
    fault_at = info->si_addr;
    faulted = 1;
    siglongjmp(reader_env, 1);
}

static void *reader(void *arg)
{
    long after = 0;
    int sum = 0;

    (void)arg;
    if (bind_to(0) != 0) {
        return &unbound;
    }
    if (sigsetjmp(reader_env, 1) != 0) {
        return NULL;
    }
    sum += page[0];
    reading = 1;
    for (;;) {
        sum += page[0];
        if (unmapped && ++after > READS_AFTER_UNMAP) {
            return (void *)(long)(sum | 2);
        }
    }
}

/** A page read on the first processor and unmapped on the last: the reader
 * takes SIGSEGV at it. */
static int unmaps(void)
{
    struct sigaction sa;
    pthread_t t;
    void *rc;

    page = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (page == MAP_FAILED) {
        return fail("mmap");
    }
    page[0] = 1;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = segv;
    sa.sa_flags = SA_SIGINFO;
    if (sigaction(SIGSEGV, &sa, NULL) != 0) {
        return fail("sigaction");
    }
    if (bind_to(cpus - 1) != 0) {
        return fail("bind to the last processor");
    }
    if (pthread_create(&t, NULL, reader, NULL) != 0) {
        return fail("pthread_create");
    }
    while (!reading) {
        sched_yield();
    }
    if (munmap((void *)page, 4096) != 0) {
        return fail("munmap");
    }
    unmapped = 1;
    if (pthread_join(t, &rc) != 0 || rc == &unbound) {
        return fail("the reader's thread");
    }
    if (!faulted) {
        return fail("a page unmapped on one processor was still read on another");
    }
    if (fault_at != (void *)page) {
        return fail("the reader's fault was not at the unmapped page");
    }
    return 0;
}

/** The time-stamp counter. */
static unsigned long long tsc(void)
{
    unsigned lo, hi;

    __asm__ __volatile__("rdtsc" : "=a"(lo), "=d"(hi));
    return ((unsigned long long)hi << 32) | lo;
}

/** CLOCK_MONOTONIC in nanoseconds. */
static long long mono_ns(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

/*
 * The counter's rate, from the clock on the first processor over 200 ms;
 * then on the last, sleeps of 1 to 16 ms, each timed by the counter and
 * begun after a spin of 0 to 10 ms.
 */
static int sleeps(void)
{
    struct timespec req;
    unsigned long long t0, t1;
    long long c0, c1, want, took;
    double per_ns;
    int i;

    if (bind_to(0) != 0) {
        return fail("bind to the first processor");
    }
    req.tv_sec = 0;
    req.tv_nsec = (long)MEASURE_NS;
    t0 = tsc();
    c0 = mono_ns();
    nanosleep(&req, NULL);
    t1 = tsc();
    c1 = mono_ns();
    if (c1 <= c0 || t1 <= t0) {
        return fail("the counter or the clock did not move");
    }
    per_ns = (double)(t1 - t0) / (double)(c1 - c0);
    if (bind_to(cpus - 1) != 0) {
        return fail("bind to the last processor");
    }
    for (i = 0; i < SLEEPS; i++) {
        want = 1000000LL + (i * 2770000LL) % 15000000LL;
        req.tv_nsec = (long)want;
        t0 = tsc() + (unsigned long long)((double)((i * 3700000LL) % 10000000LL) * per_ns);
        while (tsc() < t0) {
        }
        t0 = tsc();
        nanosleep(&req, NULL);
        t1 = tsc();
        took = (long long)((double)(t1 - t0) / per_ns);
        if (took + SLEEP_SLACK < want) {
            printf("SMPMUSL FAIL (a sleep of %lld us on the last processor took %lld us)\n",
                   want / 1000, took / 1000);
            return 1;
        }
    }
    return 0;
}

int main(int argc, char **argv)
{
    if (argc == 2 && strcmp(argv[1], "child") == 0) {
        return CHILD_STATUS;
    }
    if (argc != 2 || atoi(argv[1]) < 1 || atoi(argv[1]) > 64) {
        printf("SMPMUSL FAIL (usage: smpmusl N, the processors the boot has)\n");
        return 2;
    }
    cpus = atoi(argv[1]);
    self = argv[0];
    if (counts() || execs() || pipes() || futexes() || unmaps() || sleeps()) {
        return 1;
    }
    printf("SMPMUSL OK (%d processor%s)\n", cpus, cpus == 1 ? "" : "s");
    return 0;
}
