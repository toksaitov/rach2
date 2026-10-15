/**
 * smpmusl: check the processors as a program sees and uses them through musl.
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
 * the boot processor's tick, must not end early: a thread on the first
 * processor reads the clock before it lets each sleep begin and again once
 * it sees the sleep end, since a clock read wrong on the last processor
 * would time its own sleeps wrong too, and each must last its request less
 * the slack of the clock's reading. A sleep ends at a tick, so each begins
 * after a spin of its own length, at a different place in the tick.
 *
 * Prints "SMPMUSL OK (N processors)", or "(1 processor)", on success and
 * "SMPMUSL FAIL (detail)" otherwise.
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

/* The sleeps row: how many, and how much shorter than its request a sleep
 * may seem, for the clock's reading. */
#define SLEEPS      60
#define SLEEP_SLACK 100000LL

/* The processors the boot should have, from the argument, 1 to 64. */
static int cpus;
/* What a thread returns when it could not be bound to its processor. */
static char unbound;
/* This program's path, which the exec row runs. */
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

/** Count the lines of `path` that begin with `prefix`, or -1. */
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

/**
 * Check the processors seen each way a program asks, as the file comment
 * lists, and that a process bound to each runs there; the affinity is then
 * set back to every processor. Returns 0, or 1 after printing the failure.
 */
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

/**
 * Fork, for each processor, a child bound there that forks and execs this
 * program EXECS_PER_CPU times and counts the exits with CHILD_STATUS. Returns
 * 0 when every child counted every exit, or 1 after printing the failure.
 */
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

/**
 * Pass a counter back and forth through two pipes between this process, bound
 * to the first processor, and a child bound to the last, each checking every
 * value it reads. Returns 0, or 1 after printing the failure.
 */
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

/* The futex the turns pass through: even for the first thread's turn, odd
 * for the last's. */
static int futex_word;

/* The waits that slept until a wake, and the calls that failed otherwise than
 * a wait does when the word has changed or a signal came. */
static int futex_slept, futex_errors;

/**
 * Take FUTEX_TURNS turns on `futex_word`: wait until its low bit is `parity`,
 * add one, and wake the other thread. Counts a wait that slept until a wake
 * in `futex_slept` and a failed call in `futex_errors`.
 */
static void futex_turns(int parity)
{
    int i;

    for (i = 0; i < FUTEX_TURNS; i++) {
        int v;

        while (((v = __atomic_load_n(&futex_word, __ATOMIC_SEQ_CST)) & 1) != parity) {
            if (syscall(SYS_futex, &futex_word, FUTEX_WAIT, v, NULL, NULL, 0) == 0) {
                __atomic_add_fetch(&futex_slept, 1, __ATOMIC_SEQ_CST);
            } else if (errno != EAGAIN && errno != EINTR) {
                __atomic_add_fetch(&futex_errors, 1, __ATOMIC_SEQ_CST);
            }
        }
        __atomic_add_fetch(&futex_word, 1, __ATOMIC_SEQ_CST);
        if (syscall(SYS_futex, &futex_word, FUTEX_WAKE, 1, NULL, NULL, 0) < 0) {
            __atomic_add_fetch(&futex_errors, 1, __ATOMIC_SEQ_CST);
        }
    }
}

/** Take the odd turns on the last processor; `&unbound` if it cannot bind. */
static void *futex_last(void *arg)
{
    (void)arg;
    if (bind_to(cpus - 1) != 0) {
        return &unbound;
    }
    futex_turns(1);
    return NULL;
}

/**
 * Check two threads on the first and the last processor taking turns through
 * a futex word, which must end at twice the turns, with no call failed and
 * some of the waits asleep until the other's wake: threads that only polled
 * the word would end it at twice the turns too. Returns 0, or 1 after printing
 * the failure.
 */
static int futexes(void)
{
    pthread_t t;
    void *rc;

    futex_word = futex_slept = futex_errors = 0;
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
    if (futex_errors != 0 || futex_slept == 0) {
        return fail("a futex call failed, or no wait slept until a wake");
    }
    return 0;
}

/* The unmap row: the page, the flags the reader and the unmapper set
 * (`reading` once the reader has read it, `unmapped` once the munmap has
 * returned, `faulted` once the reader took SIGSEGV), the address of the
 * fault, and where the handler returns the reader to. */
static volatile int *page;
static volatile int reading, unmapped, faulted;
static volatile void *fault_at;
static sigjmp_buf reader_env;

/** The SIGSEGV handler: record the address, and jump back into `reader`. */
static void segv(int signo, siginfo_t *info, void *context)
{
    (void)signo;
    (void)context;
    fault_at = info->si_addr;
    faulted = 1;
    siglongjmp(reader_env, 1);
}

/**
 * Read `page` in a loop on the first processor until the read faults, and
 * return NULL then. A reader still reading READS_AFTER_UNMAP reads after the
 * unmap stops and returns a value that is neither NULL nor `&unbound`; one
 * that cannot bind returns `&unbound`.
 */
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

/**
 * Check that a page read on the first processor and unmapped on the last
 * makes the reader take SIGSEGV at that page. Returns 0, or 1 after printing
 * the failure.
 */
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

/** CLOCK_MONOTONIC in nanoseconds. */
static long long mono_ns(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

/* The sleeps row's handshake: the sleeper sets `sleep_ready` after its spin,
 * or with `sleep_unbound` when it could not be bound; the thread on the
 * first processor sets `sleep_go` once it has read the clock; and the
 * sleeper sets `sleep_done` when its sleep returns. */
static volatile int sleep_ready, sleep_go, sleep_done, sleep_unbound;

/** Wait until `*flag` is set, yielding so that one processor can run both. */
static void await(volatile int *flag)
{
    while (!__atomic_load_n(flag, __ATOMIC_SEQ_CST)) {
        sched_yield();
    }
}

/**
 * Sleep SLEEPS times on the last processor, for 1 to 16 ms, each sleep begun
 * after a spin of 0 to 10 ms on the clock there. The thread on the first
 * processor reads the clock before `sleep_go` and after `sleep_done`, so the
 * handshake only lengthens what it measures. Returns NULL, or `&unbound` when
 * it cannot bind.
 */
static void *sleeper(void *arg)
{
    struct timespec req;
    long long until;
    int i;

    (void)arg;
    if (bind_to(cpus - 1) != 0) {
        __atomic_store_n(&sleep_unbound, 1, __ATOMIC_SEQ_CST);
        __atomic_store_n(&sleep_ready, 1, __ATOMIC_SEQ_CST);
        return &unbound;
    }
    req.tv_sec = 0;
    for (i = 0; i < SLEEPS; i++) {
        until = mono_ns() + (i * 3700000LL) % 10000000LL;
        while (mono_ns() < until) {
        }
        req.tv_nsec = (long)(1000000LL + (i * 2770000LL) % 15000000LL);
        __atomic_store_n(&sleep_ready, 1, __ATOMIC_SEQ_CST);
        await(&sleep_go);
        __atomic_store_n(&sleep_go, 0, __ATOMIC_SEQ_CST);
        nanosleep(&req, NULL);
        __atomic_store_n(&sleep_done, 1, __ATOMIC_SEQ_CST);
    }
    return NULL;
}

/**
 * Time each of the sleeper's sleeps by the clock on the first processor, and
 * check that none is shorter than its request less SLEEP_SLACK. Returns 0, or
 * 1 after printing the failure.
 */
static int sleeps(void)
{
    pthread_t t;
    void *rc;
    long long c0, c1, want;
    int i;

    sleep_ready = sleep_go = sleep_done = sleep_unbound = 0;
    if (bind_to(0) != 0) {
        return fail("bind to the first processor");
    }
    if (pthread_create(&t, NULL, sleeper, NULL) != 0) {
        return fail("pthread_create");
    }
    for (i = 0; i < SLEEPS; i++) {
        want = 1000000LL + (i * 2770000LL) % 15000000LL;
        await(&sleep_ready);
        if (sleep_unbound) {
            break;
        }
        __atomic_store_n(&sleep_ready, 0, __ATOMIC_SEQ_CST);
        c0 = mono_ns();
        __atomic_store_n(&sleep_go, 1, __ATOMIC_SEQ_CST);
        await(&sleep_done);
        c1 = mono_ns();
        __atomic_store_n(&sleep_done, 0, __ATOMIC_SEQ_CST);
        if (c1 - c0 + SLEEP_SLACK < want) {
            printf("SMPMUSL FAIL (a sleep of %lld us on the last processor took %lld us)\n",
                   want / 1000, (c1 - c0) / 1000);
            return 1;
        }
    }
    if (pthread_join(t, &rc) != 0 || rc != NULL) {
        return fail("the sleeping thread");
    }
    return 0;
}

/**
 * Run the rows for the number of processors the argument gives, 1 to 64, or,
 * with the argument `child`, exit with CHILD_STATUS for the exec row. A bad
 * argument exits with 2.
 */
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
