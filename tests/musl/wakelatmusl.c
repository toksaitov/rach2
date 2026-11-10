/* Measure six timed waits beside pinned user and system-call workers. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/timerfd.h>
#include <sys/utsname.h>
#include <time.h>
#include <unistd.h>
#include "../probe_report.h"

extern char **environ;

#define CPUS               64
#define FIELDS             8
#define FUTEX_WAIT_PRIVATE 128
#define PERIOD_NS          5000000L

struct worker {
    pthread_t thread;
    int cpu;
    int calls;
    unsigned long rounds;
};

struct trace {
    unsigned int count;
    unsigned long long part[FIELDS];
    int present;
};

struct sample {
    long long late;
    struct trace trace;
};

static struct worker workers[CPUS * 2];
static int worker_count;
static int ready, start, stopped, worker_error;
static volatile sig_atomic_t interrupted;
static int epfd = -1, timer = -1;
static struct sample *samples;
static const char *const methods[] = {"nanosleep",  "clock_nanosleep", "poll",
                                      "epoll_wait", "timerfd",         "futex"};

/** Stop and join every created worker before releasing the probe's objects. */
static void cleanup(void)
{
    int i;

    __atomic_store_n(&stopped, 1, __ATOMIC_RELEASE);
    for (i = 0; i < worker_count; i++)
        pthread_join(workers[i].thread, NULL);
    worker_count = 0;
    if (epfd >= 0)
        close(epfd);
    if (timer >= 0)
        close(timer);
    epfd = timer = -1;
    free(samples);
    samples = NULL;
}

/** Arrange an orderly exit after a signal interrupts the measured wait. */
static void caught(int signo)
{
    interrupted = signo;
}

/** Report a call that did not satisfy its contract. */
static void fail(const char *what)
{
    fprintf(stderr, "wakelat: %s: %s\n", what, strerror(errno));
    report_stop(what);
    exit(1);
}

/** Bind the calling thread to one of the CPUs allowed at probe entry. */
static int bind_cpu(int cpu)
{
    cpu_set_t mask;

    CPU_ZERO(&mask);
    CPU_SET(cpu, &mask);
    return sched_setaffinity(0, sizeof(mask), &mask);
}

/** Exercise faults, mapping changes and short calls, as process startup does.
 * The private mapping is removed on every path. */
static int kernel_work(void)
{
    unsigned char *p;
    size_t i, size = 128u * 4096u;
    int rc;

    if (syscall(SYS_gettid) <= 0)
        return -1;
    p = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED)
        return -1;
    for (i = 0; i < size; i += 4096)
        p[i] = (unsigned char)(i >> 12);
    rc = mprotect(p, size, PROT_READ);
    if (munmap(p, size) != 0)
        rc = -1;
    return rc;
}

/** Keep one CPU busy in user code or system calls until cleanup stops it. */
static void *work(void *arg)
{
    struct worker *w = arg;
    volatile unsigned long value = 1;
    unsigned int i;

    if (bind_cpu(w->cpu) != 0)
        __atomic_store_n(&worker_error, errno, __ATOMIC_RELEASE);
    __atomic_add_fetch(&ready, 1, __ATOMIC_RELEASE);
    while (!__atomic_load_n(&start, __ATOMIC_ACQUIRE) &&
           !__atomic_load_n(&stopped, __ATOMIC_ACQUIRE))
        sched_yield();
    while (!__atomic_load_n(&stopped, __ATOMIC_ACQUIRE)) {
        if (w->calls) {
            if (kernel_work() != 0) {
                __atomic_store_n(&worker_error, errno, __ATOMIC_RELEASE);
                break;
            }
        } else {
            for (i = 0; i < 4096; i++)
                value = value * 1664525u + 1013904223u;
        }
        __atomic_add_fetch(&w->rounds, 1, __ATOMIC_RELEASE);
    }
    return NULL;
}

/** Start the selected load on every allowed CPU, after every worker is ready.
 * Load bits select arithmetic (1), system calls (2), or both (3). */
static void load_start(const cpu_set_t *allowed, int load)
{
    int c, kind, rc;

    for (c = 0; c < CPU_SETSIZE; c++) {
        if (!CPU_ISSET(c, allowed))
            continue;
        for (kind = 1; kind <= 2; kind++) {
            struct worker *w;

            if (!(load & kind))
                continue;
            if (worker_count == CPUS * 2) {
                errno = E2BIG;
                fail("more than 64 allowed CPUs");
            }
            w = &workers[worker_count];
            w->cpu = c;
            w->calls = kind == 2;
            rc = pthread_create(&w->thread, NULL, work, w);
            if (rc) {
                errno = rc;
                fail("pthread_create");
            }
            worker_count++;
        }
    }
    while (__atomic_load_n(&ready, __ATOMIC_ACQUIRE) != worker_count && !interrupted)
        sched_yield();
    __atomic_store_n(&start, 1, __ATOMIC_RELEASE);
    for (c = 0; c < worker_count; c++) {
        while (!__atomic_load_n(&workers[c].rounds, __ATOMIC_ACQUIRE) && !interrupted &&
               !__atomic_load_n(&worker_error, __ATOMIC_ACQUIRE))
            sched_yield();
    }
}

/** Read a monotonic timestamp as nanoseconds. */
static long long now_ns(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        fail("clock_gettime");
    return (long long)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

/** Read the current thread's latest completed wake, if this kernel has it.
 * Linux has no TimedWake fields, and still runs the identical measured load. */
static struct trace trace_read(void)
{
    struct trace t;
    FILE *f;
    char line[512];
    int count_seen = 0, last_seen = 0;

    memset(&t, 0, sizeof(t));
    f = fopen("/proc/self/status", "r");
    if (!f)
        fail("open status");
    while (fgets(line, sizeof(line), f)) {
        if (sscanf(line, "TimedWakeCount: %u", &t.count) == 1)
            count_seen = 1;
        if (sscanf(line, "TimedWakeLast: %llu %llu %llu %llu %llu %llu %llu %llu", &t.part[0],
                   &t.part[1], &t.part[2], &t.part[3], &t.part[4], &t.part[5], &t.part[6],
                   &t.part[7]) == FIELDS)
            last_seen = 1;
    }
    if (ferror(f)) {
        fclose(f);
        fail("read status");
    }
    fclose(f);
    if (count_seen != last_seen) {
        errno = EINVAL;
        fail("incomplete timed wake record");
    }
    t.present = count_seen;
    return t;
}

/** Print the kernel's lock counters at a load boundary, when present.
 * Linux has none of these lines; its measured workload is unchanged. */
static void lock_stats(const char *where)
{
    FILE *f = fopen("/proc/stat", "r");
    char line[512];

    if (!f)
        fail("open stat");
    while (fgets(line, sizeof(line), f)) {
        if (!strncmp(line, "bkl_", 4))
            printf("wakelat: lock: %s %s", where, line);
    }
    if (ferror(f)) {
        fclose(f);
        fail("read stat");
    }
    fclose(f);
}

/** Wait once through the selected interface; return 0 only for its timeout.
 * The five-millisecond interval is the measured workload. */
static int wait_once(int method, long long due)
{
    struct timespec relative = {0, PERIOD_NS}, absolute;
    struct itimerspec setting;
    struct epoll_event event;
    uint64_t expirations;
    int word = 0, rc;

    absolute.tv_sec = (time_t)(due / 1000000000LL);
    absolute.tv_nsec = (long)(due % 1000000000LL);
    switch (method) {
    case 0:
        /* interval: the measured 5 ms deadline is the workload */
        return nanosleep(&relative, NULL);
    case 1:
        /* interval: the measured 5 ms deadline is the workload */
        rc = clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &absolute, NULL);
        if (rc)
            errno = rc;
        return rc ? -1 : 0;
    case 2:
        return poll(NULL, 0, 5);
    case 3:
        return epoll_wait(epfd, &event, 1, 5);
    case 4:
        memset(&setting, 0, sizeof(setting));
        setting.it_value = absolute;
        if (timerfd_settime(timer, TFD_TIMER_ABSTIME, &setting, NULL) != 0)
            return -1;
        if (read(timer, &expirations, sizeof(expirations)) != sizeof(expirations))
            return -1;
        return expirations == 1 ? 0 : -1;
    case 5:
        rc = (int)syscall(SYS_futex, &word, FUTEX_WAIT_PRIVATE, 0, &relative, NULL, 0);
        return rc == -1 && errno == ETIMEDOUT ? 0 : -1;
    default:
        errno = EINVAL;
        return -1;
    }
}

/** Order lateness samples, retaining their associated kernel record. */
static int compare(const void *left, const void *right)
{
    const struct sample *a = left, *b = right;

    return (a->late > b->late) - (a->late < b->late);
}

/** Print the columns of a kernel record, in the proc file's order. */
static void print_parts(const unsigned long long part[FIELDS])
{
    int i;

    for (i = 0; i < FIELDS; i++)
        printf(" %llu", part[i]);
    putchar('\n');
}

/** Measure a fixed number of deadlines and print percentiles and attribution.
 * Time is evidence; the assertions judge the calls and the accounting. */
static void measure(int method, int rounds, int load)
{
    struct trace previous = trace_read();
    unsigned long long sums[FIELDS] = {0};
    long long sum = 0;
    int i, j, over = 0, traced = 0, kernel_trace = previous.present;

    for (i = 0; i < rounds; i++) {
        long long begin = now_ns();
        long long due = begin + PERIOD_NS;
        long long end;

        if (wait_once(method, due) != 0)
            fail(methods[method]);
        end = now_ns();
        samples[i].late = end - due;
        samples[i].trace = trace_read();
        if (samples[i].trace.present && samples[i].trace.count != previous.count) {
            if (samples[i].trace.count - previous.count != 1 ||
                samples[i].trace.part[6] > samples[i].trace.part[3] ||
                samples[i].trace.part[7] > samples[i].trace.part[4]) {
                errno = EINVAL;
                fail("timed wake accounting");
            }
            traced++;
            for (j = 0; j < FIELDS; j++)
                sums[j] += samples[i].trace.part[j];
        } else {
            samples[i].trace.present = 0;
        }
        previous = samples[i].trace;
        sum += samples[i].late;
        over += samples[i].late > 10000000LL;
        if ((i + 1) % 32 == 0) {
            printf("wakelat: progress: %s %d/%d\n", methods[method], i + 1, rounds);
            report_progress("samples", method * rounds + i + 1);
        }
        if (interrupted || __atomic_load_n(&worker_error, __ATOMIC_ACQUIRE)) {
            errno = interrupted ? EINTR : __atomic_load_n(&worker_error, __ATOMIC_ACQUIRE);
            fail("worker or signal");
        }
    }
    qsort(samples, (size_t)rounds, sizeof(*samples), compare);
    printf("wakelat: result: %s load=%d n=%d min_ns=%lld p50_ns=%lld p95_ns=%lld "
           "p99_ns=%lld max_ns=%lld mean_ns=%lld over_10ms=%d traced=%d\n",
           methods[method], load, rounds, samples[0].late, samples[(rounds - 1) / 2].late,
           samples[(rounds - 1) * 95 / 100].late, samples[(rounds - 1) * 99 / 100].late,
           samples[rounds - 1].late, sum / rounds, over, traced);
    printf("wakelat: sum_us: %s", methods[method]);
    print_parts(sums);
    for (i = rounds > 8 ? rounds - 8 : 0; i < rounds; i++) {
        printf("wakelat: sample: %s late_ns=%lld trace=%d parts_us", methods[method],
               samples[i].late, samples[i].trace.present);
        print_parts(samples[i].trace.part);
    }
    if (kernel_trace && !traced) {
        errno = EINVAL;
        fail("no completed timed wakes");
    }
}

/** Find a sleeper CPU in the caller's allowed set and report its size.
 * A negative `cpu` selects the first one. Fail if the set exceeds CPUS. */
static int allowed_cpu(cpu_set_t *allowed, int cpu, int *count)
{
    int i;

    *count = 0;
    if (sched_getaffinity(0, sizeof(*allowed), allowed) != 0)
        fail("sched_getaffinity");
    for (i = 0; i < CPU_SETSIZE; i++) {
        if (CPU_ISSET(i, allowed)) {
            (*count)++;
            if (cpu < 0)
                cpu = i;
        }
    }
    if (cpu < 0 || cpu >= CPU_SETSIZE || !CPU_ISSET(cpu, allowed) || *count > CPUS) {
        errno = EINVAL;
        fail("sleeper CPU or CPU count");
    }
    return cpu;
}

/** Run 64 deadlines per interface by default; arguments select count, load
 * bits and sleeper CPU. No lateness bound is inferred from the host clock. */
int main(int argc, char **argv)
{
    cpu_set_t allowed;
    struct sigaction sa;
    struct utsname name;
    int rounds = argc > 1 ? atoi(argv[1]) : 64;
    int load = argc > 2 ? atoi(argv[2]) : 3;
    int cpu = argc > 3 ? atoi(argv[3]) : -1;
    int i, ncpu = 0;

    atexit(cleanup);
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = caught;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGHUP, &sa, NULL);
    setvbuf(stdout, NULL, _IOLBF, 0);
    if (argc > 4 || rounds < 1 || rounds > 65536 || load < 0 || load > 3) {
        errno = EINVAL;
        fail("usage: wakelatmusl [rounds [load-bits [sleeper-cpu]]]");
    }
    report_begin(report_case(environ, "guest.wakelat.mixed"));
    report_plan("setup");
    for (i = 0; i < 6; i++)
        report_plan(methods[i]);
    report_plan("workers");
    if (uname(&name) != 0)
        fail("uname");
    if (!strcmp(name.sysname, "Rach2") && !trace_read().present) {
        errno = EINVAL;
        fail("missing timed wake counters");
    }
    cpu = allowed_cpu(&allowed, cpu, &ncpu);
    samples = calloc((size_t)rounds, sizeof(*samples));
    epfd = epoll_create1(EPOLL_CLOEXEC);
    timer = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC);
    if (!samples || epfd < 0 || timer < 0)
        fail("prepare samples and descriptors");
    printf("wakelat: start: cpus=%d sleeper=%d rounds=%d load=%d period_ns=%ld\n", ncpu, cpu,
           rounds, load, PERIOD_NS);
    printf("wakelat: columns_us: irq timer_bkl timer_work queue return blocked "
           "queue_bkl return_bkl\n");
    lock_stats("before");
    load_start(&allowed, load);
    if (bind_cpu(cpu) != 0)
        fail("bind sleeper");
    report_held();
    for (i = 0; i < 6; i++) {
        measure(i, rounds, load);
        lock_stats(methods[i]);
        report_held();
    }
    for (i = 0; i < worker_count; i++)
        printf("wakelat: worker: cpu=%d calls=%d rounds=%lu\n", workers[i].cpu, workers[i].calls,
               __atomic_load_n(&workers[i].rounds, __ATOMIC_ACQUIRE));
    cleanup();
    lock_stats("after");
    report_held();
    report_end();
    puts("WAKELAT OK");
    return 0;
}
