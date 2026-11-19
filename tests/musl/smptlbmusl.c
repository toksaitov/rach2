/*
 * smptlbmusl: count the TLB shootdowns that page faults, an unmap and an
 * mprotect cost while a second thread runs on another processor, and the
 * timer passes other processors make while the boot processor's tick waits
 * for the big lock.
 *
 * The main thread runs on processor 0 and a spinner on processor 1, so that
 * every change of an entry this address space had present must be dropped
 * there too. The rows read `/proc/stat`'s `tlb_shootdowns` and
 * `tlb_shootdown_ipis` around each step, through a buffer made before the
 * first, so that the reading itself maps nothing: 64 pages faulted in cost
 * no shootdown, as in Linux, since x86 caches no translation of an entry
 * that is not present; an unmap of 64 pages costs one, which drops every
 * user translation past Linux's ceiling of 33 pages, and interrupts the
 * spinner's processor once; an mprotect of 8 pages costs one, and so does
 * their unmap. On one processor no row sends any. A step counts only when
 * the spinner ran on processor 1 throughout it, as the kernel's count of
 * the times it was switched in shows: a processor that runs another
 * address space or a kernel thread is not interrupted, as in Linux, so a
 * step during which processor 1 ran something else, such as a kernel
 * thread's periodic work, is made again, at most TLB_TRIES times, and the
 * row prints the tries it took.
 *
 * With three processors or more, a holder on processor 1 forks a process of
 * 64 MiB of pages over and over, a copy the big lock covers whole, while a
 * waiter on processor 2 sleeps 200 times on a futex with a 2 ms timeout and
 * reads a timerfd of 2 ms 200 times. Each wait must end by its timeout, and
 * the run prints `timer_passes_late`, the passes other processors made, and
 * the most a wait overran its deadline, which are the machine's and not
 * judged.
 *
 * With two processors or more, a child's thread spins on processor 1 while
 * debugfs's cputime_test has set its user time 25 ticks short of 2^32 ticks
 * and has its tick at the carry wait half a tick, so that processor 0's
 * tick at the same edge is done, write the count's lower half, wait for a
 * read of that thread's clock or its process's to begin, and write the
 * upper half a millisecond later; such a read that begins while the carry
 * is due waits for the halves to come apart. The child's main thread on
 * processor 0 reads the thread's clock and the process's over and over
 * until the count is past 2^32 and a carry had a read under way: each
 * reading is no less than the one before and less than 2^31 ticks past it,
 * as a count read whole gives, where one read with its halves apart is 2^32
 * ticks off, and one of those reads, not another process's, began while the
 * halves were apart. A carry that no read begins in within five ticks,
 * since a processor of the virtual machine did not run, is given up and
 * made again by the next tick, and a reading may move by many ticks at once
 * after a processor ran with interrupts off; the run prints the carries
 * made and given up, which are the machine's and not judged.
 *
 * Prints "SMPTLBMUSL OK" and exits 0, or "SMPTLBMUSL FAIL: " and the reason
 * and exits 1.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/timerfd.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include "../probe_report.h"

#define PAGE               4096u
#define FUTEX_WAIT_PRIVATE 128 /* FUTEX_WAIT with FUTEX_PRIVATE_FLAG */
#define TLB_TRIES          20
#define BIG                (64u << 20)
#define WAITS              200
#define WAIT_NS            2000000L
#define NKEYS              4
#define SHOOT              0
#define IPIS               1
#define LATE               2
#define PASSES             3

static const char *keys[NKEYS] = {"tlb_shootdowns", "tlb_shootdown_ipis", "timer_passes_late",
                                  "process_timer_passes"};
/* The reading of /proc/stat, made before the first row. */
static char text[16384];
static volatile int spin_stop, spin_ready, hold_stop;
/* The spinner's thread id, and the turns of its loop, which move only
 * while it runs. */
static volatile pid_t spin_tid;
static volatile unsigned long spin_turns;
static int futex_word;
/* What a thread returns when it fails, by its address. */
static char thread_failed;

/** Read the counts of `keys` into `v`; 0, or -1 when one is missing. */
static int counts(uint32_t v[NKEYS])
{
    int fd = open("/proc/stat", O_RDONLY), found = 0, i;
    ssize_t n, got = 0;
    char *line;

    if (fd < 0) {
        return -1;
    }
    while ((n = read(fd, text + got, sizeof(text) - 1 - (size_t)got)) > 0) {
        got += n;
    }
    close(fd);
    text[got] = '\0';
    for (line = text; line && *line; line = strchr(line, '\n') ? strchr(line, '\n') + 1 : NULL) {
        for (i = 0; i < NKEYS; i++) {
            size_t k = strlen(keys[i]);

            if (strncmp(line, keys[i], k) == 0 && line[k] == ' ') {
                v[i] = (uint32_t)strtoul(line + k + 1, NULL, 10);
                found |= 1 << i;
            }
        }
    }
    return found == (1 << NKEYS) - 1 ? 0 : -1;
}

/** Run the caller on processor `cpu` alone; 0 or -1. */
static int pin(int cpu)
{
    cpu_set_t set;

    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    return sched_setaffinity(0, sizeof(set), &set);
}

/** Spin in user mode on processor 1, counting the turns, until told to
 * stop. */
static void *spinner(void *arg)
{
    (void)arg;
    if (pin(1) != 0) {
        return &thread_failed;
    }
    spin_tid = (pid_t)syscall(SYS_gettid);
    spin_ready = 1;
    while (!spin_stop) {
        spin_turns++;
    }
    return NULL;
}

/** The times the spinner was switched in, the third number of its
 * `schedstat`, or 0 when it cannot be read; the reading maps nothing. */
static unsigned long long spin_switches(void)
{
    char path[48], line[96];
    unsigned long long run = 0, wait = 0, in = 0;
    int fd;
    ssize_t n;

    snprintf(path, sizeof path, "/proc/self/task/%d/schedstat", (int)spin_tid);
    fd = open(path, O_RDONLY);
    n = fd < 0 ? -1 : read(fd, line, sizeof line - 1);
    if (fd >= 0) {
        close(fd);
    }
    line[n > 0 ? n : 0] = 0;
    return sscanf(line, "%llu %llu %llu", &run, &wait, &in) == 3 ? in : 0;
}

/** Wait until the spinner's loop turns, which it does only while it runs. */
static void spin_running(void)
{
    unsigned long seen = spin_turns, now;

    do {
        __asm__ __volatile__("" ::: "memory");
        now = spin_turns;
    } while (now == seen);
}

/** Fork this process, with its BIG bytes of pages, on processor 1 until
 * told to stop; each child exits at once. */
static void *holder(void *arg)
{
    pid_t pid;

    (void)arg;

    if (pin(1) != 0) {
        return &thread_failed;
    }
    while (!hold_stop) {
        pid = (pid_t)syscall(SYS_clone, SIGCHLD, 0, 0, 0, 0);
        if (pid == 0) {
            syscall(SYS_exit, 0);
        }
        if (pid < 0 || waitpid(pid, NULL, 0) != pid) {
            return &thread_failed;
        }
    }
    return NULL;
}

/** Nanoseconds from `a` to `b`. */
static long long ns_between(const struct timespec *a, const struct timespec *b)
{
    return (long long)(b->tv_sec - a->tv_sec) * 1000000000LL + (b->tv_nsec - a->tv_nsec);
}

/** Wait WAITS times on a futex and WAITS times on a timerfd, each for
 * WAIT_NS, on processor 2; the most a wait overran its deadline goes in
 * `*over`. 0 when each ended by its timeout, -1 otherwise. */
static int waits(long long *over)
{
    struct timespec rel = {0, WAIT_NS}, t0, t1;
    struct itimerspec its;
    uint64_t fired;
    int i, fd;

    *over = 0;
    if (pin(2) != 0) {
        return -1;
    }
    for (i = 0; i < WAITS; i++) {
        clock_gettime(CLOCK_MONOTONIC, &t0);
        if (syscall(SYS_futex, &futex_word, FUTEX_WAIT_PRIVATE, 0, &rel, 0, 0) != -1 ||
            errno != ETIMEDOUT) {
            return -1;
        }
        clock_gettime(CLOCK_MONOTONIC, &t1);
        if (ns_between(&t0, &t1) - WAIT_NS > *over) {
            *over = ns_between(&t0, &t1) - WAIT_NS;
        }
    }
    fd = timerfd_create(CLOCK_MONOTONIC, 0);
    if (fd < 0) {
        return -1;
    }
    memset(&its, 0, sizeof(its));
    its.it_value.tv_nsec = WAIT_NS;
    for (i = 0; i < WAITS; i++) {
        clock_gettime(CLOCK_MONOTONIC, &t0);
        if (timerfd_settime(fd, 0, &its, NULL) != 0 || read(fd, &fired, 8) != 8 || fired < 1) {
            close(fd);
            return -1;
        }
        clock_gettime(CLOCK_MONOTONIC, &t1);
        if (ns_between(&t0, &t1) - WAIT_NS > *over) {
            *over = ns_between(&t0, &t1) - WAIT_NS;
        }
    }
    close(fd);
    return 0;
}

/* The steps of the TLB rows (`tlb_step`). */
#define STEP_FAULT 0
#define STEP_UNMAP 1
#define STEP_MPROT 2
#define STEP_SPAN  3

/**
 * Make the step `step` of the TLB rows, its pages set up first, and read
 * the counts around it into `a` and `b`: 64 pages faulted in, those 64
 * unmapped, 8 pages made read-only, or those 8 unmapped. With the spinner,
 * `smp`, the step is made again until the spinner was not switched in
 * between a reading of its count just before the step and one just after
 * it, both taken while it ran and inside the readings of the counts, so
 * that it ran on processor 1 throughout the step, at most TLB_TRIES times;
 * the tries, or -1 when a mapping failed.
 */
static int tlb_step(int step, int smp, uint32_t a[NKEYS], uint32_t b[NKEYS])
{
    size_t pages = step <= STEP_UNMAP ? 64 : 8;
    unsigned long long before = 0, after = 0;
    unsigned int i;
    char *p;
    int tries = 0;

    do {
        p = mmap(NULL, pages * PAGE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (p == MAP_FAILED) {
            return -1;
        }
        for (i = 0; step != STEP_FAULT && i < pages; i++) {
            p[i * PAGE] = 1;
        }
        if (step == STEP_SPAN) {
            mprotect(p, pages * PAGE, PROT_READ);
        }
        counts(a);
        if (smp) {
            spin_running();
            before = spin_switches();
        }
        if (step == STEP_FAULT) {
            for (i = 0; i < pages; i++) {
                p[i * PAGE] = 1;
            }
        } else if (step == STEP_MPROT) {
            mprotect(p, pages * PAGE, PROT_READ);
        } else {
            munmap(p, pages * PAGE);
        }
        if (smp) {
            spin_running();
            after = spin_switches();
        }
        counts(b);
        if (step == STEP_FAULT || step == STEP_MPROT) {
            munmap(p, pages * PAGE);
        }
        tries++;
    } while (smp && after != before && tries < TLB_TRIES);
    return tries;
}

/** Report `id` held when the step `step` cost `shoot` shootdowns and
 * `ipis` interrupts, failed otherwise; 0 when it held. */
static int tlb_row(const char *id, int step, int smp, uint32_t shoot, uint32_t ipis)
{
    uint32_t a[NKEYS], b[NKEYS], s, n;
    int tries = tlb_step(step, smp, a, b);

    if (tries < 0) {
        report_fail(id, "a mapping failed");
        return -1;
    }
    s = b[SHOOT] - a[SHOOT];
    n = b[IPIS] - a[IPIS];
    printf("smptlbmusl: %s: shootdowns %u interrupts %u, tries %d\n", id, s, n, tries);
    if (s != shoot || n != ipis) {
        report_fail(id, "the shootdowns or their interrupts differ");
        return -1;
    }
    report_pass(id);
    return 0;
}

/** The TLB rows with a spinner on processor 1, or with none on one
 * processor, where `smp` is 0 and every count must stay 0. */
static int tlb_rows(int smp)
{
    uint32_t a[NKEYS];
    int failed = 0;

    if (counts(a) != 0) {
        return -1;
    }
    failed |= tlb_row("fault", STEP_FAULT, smp, 0, 0);
    failed |= tlb_row("unmap", STEP_UNMAP, smp, smp ? 1 : 0, smp ? 1 : 0);
    failed |= tlb_row("mprotect", STEP_MPROT, smp, smp ? 1 : 0, smp ? 1 : 0);
    failed |= tlb_row("unmap-span", STEP_SPAN, smp, smp ? 1 : 0, smp ? 1 : 0);
    return failed ? -1 : 0;
}

/* The id of the thread whose count `untorn` carries, once it runs, and
 * the word that tells it to stop. */
static volatile pid_t carry_tid;
static volatile int carry_stop;

/** Spin in user mode on processor 1, with this thread's id in `carry_tid`,
 * until told to stop. */
static void *carrier(void *arg)
{
    (void)arg;
    if (pin(1) != 0) {
        return &thread_failed;
    }
    carry_tid = (pid_t)syscall(SYS_gettid);
    while (!carry_stop) {
        __asm__ __volatile__("" ::: "memory");
    }
    return NULL;
}

/** The clock `clock` in nanoseconds, or -1. */
static long long clock_ns(clockid_t clock)
{
    struct timespec ts;

    if (clock_gettime(clock, &ts) != 0) {
        return -1;
    }
    return (long long)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

/** Whether the reading `now` of a clock of processor time of `tick`
 * nanoseconds a tick follows `before` as a count read whole does: no less,
 * and less than 2^31 ticks past, half what a count read with its halves
 * apart is off by. */
static int follows(long long before, long long now, long long tick)
{
    return now >= before && now - before < ((long long)1 << 31) * tick;
}

/** Read the carries of debugfs's cputime_test that a read of the carried
 * thread's clock or its process's was under way in, the fourth of its
 * numbers, or -1; a read of another process's clock counts for nothing. */
static long contested(void)
{
    char line[96];
    long v[4];
    int fd = open("/sys/kernel/debug/cputime_test", O_RDONLY);
    ssize_t n = fd < 0 ? -1 : read(fd, line, sizeof line - 1);

    if (fd >= 0) {
        close(fd);
    }
    if (n <= 0) {
        return -1;
    }
    line[n] = 0;
    return sscanf(line, "%ld %ld %ld %ld", &v[0], &v[1], &v[2], &v[3]) == 4 ? v[3] : -1;
}

/** Print the record of `untorn_child`: the carried thread's last reading
 * in ticks against the `need` it had to pass, the milliseconds the reads
 * took, and debugfs's cputime_test line, whose third, fourth and fifth
 * numbers are the carries, the carries a read of the thread's clock or its
 * process's was under way in, and the carries given up for want of one. */
static void untorn_counts(long long ticks, long long need, long long ms)
{
    char line[96];
    int fd = open("/sys/kernel/debug/cputime_test", O_RDONLY);
    ssize_t n = fd < 0 ? -1 : read(fd, line, sizeof line - 1);

    if (fd >= 0) {
        close(fd);
    }
    line[n > 0 ? n : 0] = 0;
    printf("smptlbmusl: untorn: thread %lld ticks of %lld after %lld ms; cputime_test %s", ticks,
           need, ms, n > 0 ? line : "unread\n");
    fflush(stdout);
}

/**
 * The child of `untorn`: carry the count of a thread spinning on processor
 * 1 past 2^32 ticks and read its clock and the process's on processor 0
 * meanwhile. Exits 0 when every reading followed the one before and a read
 * was under way while the carry's halves were apart, one that began then or
 * began before and waited for the carry's lock, 2 for a reading that did not
 * follow, 3 when within a minute of monotonic time the count was not seen
 * past 2^32 with a read under way in a carry, and 4 when the thread or
 * cputime_test failed. The thread's clock counts its system time as well as
 * the user time that carries, so it can pass 2^32 ticks before the carry;
 * the reads go on until cputime_test has counted a carry with a read.
 */
static void untorn_child(void)
{
    struct timespec res, start, at;
    long long tick, end, thread_before = 0, process_before = 0, thread_now, process_now;
    clockid_t clock;
    pthread_t carry;
    char line[32];
    int fd, len, torn = 0;

    if (clock_getres(CLOCK_PROCESS_CPUTIME_ID, &res) != 0 || pin(0) != 0 ||
        pthread_create(&carry, NULL, carrier, NULL) != 0 ||
        pthread_getcpuclockid(carry, &clock) != 0) {
        _exit(4);
    }
    tick = res.tv_nsec;
    end = (((long long)1 << 32) + 10) * tick;
    while (carry_tid == 0) {
        sched_yield();
    }
    len = snprintf(line, sizeof line, "spl %d\n", (int)carry_tid);
    fd = open("/sys/kernel/debug/cputime_test", O_WRONLY);
    if (fd < 0 || write(fd, line, (size_t)len) != len) {
        _exit(4);
    }
    close(fd);
    clock_gettime(CLOCK_MONOTONIC, &start);
    do {
        thread_now = clock_ns(clock);
        process_now = clock_ns(CLOCK_PROCESS_CPUTIME_ID);
        if ((thread_before != 0 && !follows(thread_before, thread_now, tick)) ||
            (process_before != 0 && !follows(process_before, process_now, tick))) {
            printf("smptlbmusl: untorn: thread %lld after %lld, process %lld after %lld\n",
                   thread_now, thread_before, process_now, process_before);
            torn = 1;
            break;
        }
        thread_before = thread_now;
        process_before = process_now;
        clock_gettime(CLOCK_MONOTONIC, &at);
    } while ((thread_now < end || contested() < 1) && ns_between(&start, &at) < 60000000000LL);
    carry_stop = 1;
    pthread_join(carry, NULL);
    untorn_counts(thread_now / tick, end / tick, ns_between(&start, &at) / 1000000);
    if (torn) {
        _exit(2);
    }
    if (thread_now < end || contested() < 1) {
        _exit(3);
    }
    _exit(0);
}

/** Run `untorn_child` in a child, whose clocks it moves past 2^32 ticks,
 * and report the row `untorn`; 0 when it held. */
static int untorn(void)
{
    pid_t child;
    int status;

    fflush(stdout);
    child = fork();
    if (child == 0) {
        untorn_child();
    }
    if (child < 0 || waitpid(child, &status, 0) != child || !WIFEXITED(status)) {
        report_fail("untorn", "the child did not run to its end");
        return -1;
    }
    if (WEXITSTATUS(status) != 0) {
        report_fail("untorn", WEXITSTATUS(status) == 2 ? "a CPU clock read a torn count"
                              : WEXITSTATUS(status) == 3
                                  ? "the count did not carry past 2^32 under a read"
                                  : "the thread or cputime_test failed");
        return -1;
    }
    report_pass("untorn");
    return 0;
}

/** Run the rows; 0 when each held. */
int main(void)
{
    pthread_t spin, hold;
    uint32_t a[NKEYS], b[NKEYS];
    long cpus = sysconf(_SC_NPROCESSORS_ONLN);
    long long over = 0;
    void *rc = NULL;
    char *big;
    int failed = 0, smp = cpus >= 2;

    report_begin(report_case(environ, "guest.default.smptlbmusl"));
    if (counts(a) != 0) {
        report_fail("counters", "/proc/stat lacks a count");
        report_end();
        puts("SMPTLBMUSL FAIL: /proc/stat lacks a count");
        return 1;
    }
    report_pass("counters");
    if (smp && (pin(0) != 0 || pthread_create(&spin, NULL, spinner, NULL) != 0)) {
        report_end();
        puts("SMPTLBMUSL FAIL: the spinner");
        return 1;
    }
    while (smp && !spin_ready) {
        sched_yield();
    }
    failed |= tlb_rows(smp);
    if (smp) {
        spin_stop = 1;
        pthread_join(spin, &rc);
        failed |= rc != NULL;
    }
    if (cpus < 3) {
        report_notrun("timeouts", "fewer than three processors");
    } else {
        big = mmap(NULL, BIG, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE,
                   -1, 0);
        counts(a);
        if (big == MAP_FAILED || pthread_create(&hold, NULL, holder, big) != 0) {
            report_fail("timeouts", "the holder");
            failed = 1;
        } else {
            int w = waits(&over);

            hold_stop = 1;
            pthread_join(hold, &rc);
            counts(b);
            printf("smptlbmusl: timeouts: late passes %u of %u, most overrun %lld us\n",
                   b[LATE] - a[LATE], b[PASSES] - a[PASSES], over / 1000);
            if (w != 0 || rc != NULL) {
                report_fail("timeouts", "a wait did not end by its timeout");
                failed = 1;
            } else {
                report_pass("timeouts");
            }
        }
    }
    if (!smp) {
        report_notrun("untorn", "one processor");
    } else {
        failed |= untorn();
    }
    report_end();
    puts(failed ? "SMPTLBMUSL FAIL: a row failed" : "SMPTLBMUSL OK");
    return failed ? 1 : 0;
}
