/**
 * wakemusl: measure the calls of Wine's yield spin and the time from a wake
 * to the woken thread's running, for comparison with Linux on the same
 * machine.
 *
 * The same static binary runs under Rach2 and under Linux. Every time is
 * read from the time-stamp counter, calibrated against CLOCK_MONOTONIC at
 * the start, so that no clock call adds its own cost to a measurement.
 *
 * The first rows give the cost of one getrusage(RUSAGE_THREAD), one
 * sched_yield and one turn of Wine's NtYieldExecution (getrusage, yield,
 * getrusage) on processor 0, while a sibling thread on processor 1 is idle,
 * spins in user mode or calls sched_yield itself.
 *
 * The wake rows have a thread on processor 1 asleep in a futex wait, or in
 * an epoll_pwait2 on an eventfd or a pipe, and the main thread on processor
 * 0 wake it after a busy pause of PAUSE microseconds: by FUTEX_WAKE, or by a
 * write to the eventfd or the pipe. The woken thread reads the counter as
 * soon as its call returns; the row gives the time from just before the
 * waker's call to then, and the waker's call's own time. The main thread
 * waits for the answer by spinning, so that each round measures one wake.
 * The last row is the futex hand-off of 80-27's pingpong: a wake and the
 * woken thread's wake back, the round's time.
 *
 * With the third argument `load`, two more threads on processors 2 and 3
 * hand a futex back and forth between them for the whole run, so that
 * every row runs while other processors block and wake, as the loop's
 * other threads do; that needs four processors.
 *
 * Arguments: [ROUNDS [PAUSE [load]]], 20000 wakes a row and 50 us by
 * default. Prints one line for each row and "WAKEMUSL OK", or "WAKEMUSL
 * FAIL (detail)" and exits 1. It needs at least two processors.
 */

#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#ifndef RUSAGE_THREAD
#define RUSAGE_THREAD 1
#endif
#define FUTEX_WAIT_PRIVATE 128
#define FUTEX_WAKE_PRIVATE 129
#ifndef SYS_epoll_pwait2
#define SYS_epoll_pwait2 441
#endif

#define CALLS 200000

enum sibling { SIB_IDLE, SIB_SPIN, SIB_YIELD };
enum waker { WAKE_FUTEX, WAKE_EVENTFD, WAKE_PIPE, WAKE_PINGPONG };

static double cycles_per_us;
static int rounds = 20000;
static double pause_us = 50;

static volatile int sibling_mode;
static volatile int sibling_stop;
static volatile int sibling_ready;

static volatile int word;      /* the futex the waiter sleeps on */
static volatile int back;      /* the futex of the pingpong's answer */
static volatile int answered;  /* the round the waiter has answered */
static volatile uint64_t sent; /* the counter just before the wake */
static uint64_t *latency;      /* each round's wake-to-run, in cycles */
static int wake_kind;
static int efd = -1;
static int pfd[2] = {-1, -1};
static int epfd = -1;
static volatile int load_word[2]; /* the load's hand-off, one word a side */
static volatile int load_stop;
static unsigned long load_rounds;

/** The time-stamp counter, ordered with the loads and stores around it. */
static uint64_t tsc(void)
{
    uint32_t lo;
    uint32_t hi;

    __asm__ volatile("lfence; rdtsc; lfence" : "=a"(lo), "=d"(hi) : : "memory");
    return (uint64_t)hi << 32 | lo;
}

/** CLOCK_MONOTONIC in microseconds, for the counter's calibration alone. */
static double now_us(void)
{
    struct timespec t;

    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1e6 + t.tv_nsec / 1e3;
}

/** Print the failure of `what` with errno and exit 1. */
static void fail(const char *what)
{
    printf("WAKEMUSL FAIL (%s: errno %d)\n", what, errno);
    exit(1);
}

/** Keep the calling thread on processor `cpu`. */
static void pin(int cpu)
{
    cpu_set_t set;

    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    if (sched_setaffinity(0, sizeof set, &set) != 0) {
        fail("sched_setaffinity");
    }
}

/** The futex call `op` on `w` with the value `v` and no timeout. */
static long futex(volatile int *w, int op, int v)
{
    return syscall(SYS_futex, w, op, v, NULL, NULL, 0);
}

/** The sibling of the per-call rows, on processor 1. */
static void *sibling(void *arg)
{
    (void)arg;
    pin(1);
    sibling_ready = 1;
    while (!sibling_stop) {
        if (sibling_mode == SIB_YIELD) {
            sched_yield();
        }
    }
    return NULL;
}

/** Print the mean cost of one call of `what` on processor 0. */
static void per_call(const char *what, int mode)
{
    pthread_t t;
    struct rusage ru;
    uint64_t c0;
    int i;
    int threaded = mode != SIB_IDLE;
    static const char *const names[] = {"sibling idle", "sibling spins", "sibling yields"};

    sibling_mode = mode;
    sibling_stop = 0;
    sibling_ready = 0;
    if (threaded && pthread_create(&t, NULL, sibling, NULL) != 0) {
        fail("pthread_create");
    }
    while (threaded && !sibling_ready) {
    }
    c0 = tsc();
    for (i = 0; i < CALLS; i++) {
        if (what[0] == 'g') {
            getrusage(RUSAGE_THREAD, &ru);
        } else if (what[0] == 's') {
            sched_yield();
        } else {
            getrusage(RUSAGE_THREAD, &ru);
            sched_yield();
            getrusage(RUSAGE_THREAD, &ru);
        }
    }
    printf("wakemusl: %s, %s: %.3f us a call\n", what, names[mode],
           (tsc() - c0) / cycles_per_us / CALLS);
    sibling_stop = 1;
    if (threaded) {
        pthread_join(t, NULL);
    }
}

/** The thread woken in the wake rows, on processor 1. */
static void *waiter(void *arg)
{
    struct epoll_event ev;
    uint64_t value;
    char byte;
    int r;

    (void)arg;
    pin(1);
    for (r = 0; r < rounds; r++) {
        if (wake_kind == WAKE_FUTEX || wake_kind == WAKE_PINGPONG) {
            while (word == r) {
                futex(&word, FUTEX_WAIT_PRIVATE, r);
            }
        } else {
            while (syscall(SYS_epoll_pwait2, epfd, &ev, 1, NULL, NULL, 0) != 1) {
            }
        }
        latency[r] = tsc() - sent;
        if (wake_kind == WAKE_EVENTFD && read(efd, &value, sizeof value) != sizeof value) {
            fail("read eventfd");
        }
        if (wake_kind == WAKE_PIPE && read(pfd[0], &byte, 1) != 1) {
            fail("read pipe");
        }
        if (wake_kind == WAKE_PINGPONG) {
            __atomic_store_n(&back, r + 1, __ATOMIC_SEQ_CST);
            futex(&back, FUTEX_WAKE_PRIVATE, 1);
        } else {
            __atomic_store_n(&answered, r + 1, __ATOMIC_SEQ_CST);
        }
    }
    return NULL;
}

/** One side of the load, on processor 2 + `arg`: wait for its word to
 * change, then change and wake the other side's. */
static void *load(void *arg)
{
    int me = (int)(long)arg;
    int seen = 0;
    int first = me == 0;

    pin(2 + me);
    while (!load_stop) {
        if (!first) {
            while (load_word[me] == seen && !load_stop) {
                futex(&load_word[me], FUTEX_WAIT_PRIVATE, seen);
            }
            seen = load_word[me];
        }
        first = 0;
        __atomic_store_n(&load_word[!me], load_word[!me] + 1, __ATOMIC_SEQ_CST);
        futex(&load_word[!me], FUTEX_WAKE_PRIVATE, 1);
        if (me == 0) {
            load_rounds++;
        }
    }
    __atomic_store_n(&load_word[!me], load_word[!me] + 1, __ATOMIC_SEQ_CST);
    futex(&load_word[!me], FUTEX_WAKE_PRIVATE, 1);
    return NULL;
}

/** qsort's order of two counter values, smallest first. */
static int by_value(const void *a, const void *b)
{
    uint64_t x = *(const uint64_t *)a;
    uint64_t y = *(const uint64_t *)b;

    return x < y ? -1 : x > y;
}

/** Run one wake row of the kind `kind` and print it. */
static void wake_row(const char *name, int kind)
{
    pthread_t t;
    uint64_t pause = (uint64_t)(pause_us * cycles_per_us);
    uint64_t call = 0;
    uint64_t total = 0;
    uint64_t start;
    uint64_t one = 1;
    struct epoll_event ev;
    int r;

    wake_kind = kind;
    word = 0;
    back = 0;
    answered = 0;
    if (kind == WAKE_EVENTFD || kind == WAKE_PIPE) {
        epfd = epoll_create1(0);
        if (epfd < 0) {
            fail("epoll_create1");
        }
        if (kind == WAKE_EVENTFD) {
            efd = eventfd(0, 0);
            if (efd < 0) {
                fail("eventfd");
            }
        } else if (pipe(pfd) != 0) {
            fail("pipe");
        }
        memset(&ev, 0, sizeof ev);
        ev.events = EPOLLIN;
        if (epoll_ctl(epfd, EPOLL_CTL_ADD, kind == WAKE_EVENTFD ? efd : pfd[0], &ev) != 0) {
            fail("epoll_ctl");
        }
    }
    if (pthread_create(&t, NULL, waiter, NULL) != 0) {
        fail("pthread_create");
    }
    for (r = 0; r < rounds; r++) {
        start = tsc();
        while (tsc() - start < pause) {
        }
        sent = tsc();
        if (kind == WAKE_FUTEX || kind == WAKE_PINGPONG) {
            __atomic_store_n(&word, r + 1, __ATOMIC_SEQ_CST);
            futex(&word, FUTEX_WAKE_PRIVATE, 1);
        } else if (kind == WAKE_EVENTFD) {
            if (write(efd, &one, sizeof one) != sizeof one) {
                fail("write eventfd");
            }
        } else if (write(pfd[1], "x", 1) != 1) {
            fail("write pipe");
        }
        call += tsc() - sent;
        if (kind == WAKE_PINGPONG) {
            while (back == r) {
                futex(&back, FUTEX_WAIT_PRIVATE, r);
            }
            total += tsc() - sent;
        } else {
            while (__atomic_load_n(&answered, __ATOMIC_SEQ_CST) == r) {
            }
        }
    }
    pthread_join(t, NULL);
    if (kind == WAKE_PINGPONG) {
        printf("wakemusl: %s after %.0f us: %.3f us a round\n", name, pause_us,
               total / cycles_per_us / rounds);
    } else {
        uint64_t sum = 0;

        for (r = 0; r < rounds; r++) {
            sum += latency[r];
        }
        qsort(latency, (size_t)rounds, sizeof latency[0], by_value);
        printf("wakemusl: %s after %.0f us: wake to run mean %.3f us, median %.3f, "
               "p90 %.3f, p99 %.3f; waker's call %.3f us\n",
               name, pause_us, sum / cycles_per_us / rounds, latency[rounds / 2] / cycles_per_us,
               latency[rounds * 9 / 10] / cycles_per_us, latency[rounds * 99 / 100] / cycles_per_us,
               call / cycles_per_us / rounds);
    }
    if (epfd >= 0) {
        close(epfd);
        epfd = -1;
    }
    if (efd >= 0) {
        close(efd);
        efd = -1;
    }
    if (pfd[0] >= 0) {
        close(pfd[0]);
        close(pfd[1]);
        pfd[0] = pfd[1] = -1;
    }
}

int main(int argc, char **argv)
{
    double t0;
    uint64_t c0;
    long cpus = sysconf(_SC_NPROCESSORS_ONLN);
    int loaded = argc > 3 && strcmp(argv[3], "load") == 0;
    pthread_t loaders[2];
    double l0;

    setvbuf(stdout, NULL, _IOLBF, 0);
    if (argc > 1) {
        rounds = atoi(argv[1]);
    }
    if (argc > 2) {
        pause_us = atof(argv[2]);
    }
    if (rounds < 100 || cpus < (loaded ? 4 : 2)) {
        errno = 0;
        fail(rounds < 100 ? "fewer than 100 rounds" : "too few processors");
    }
    latency = calloc((size_t)rounds, sizeof latency[0]);
    if (!latency) {
        fail("calloc");
    }
    pin(0);
    t0 = now_us();
    c0 = tsc();
    while (now_us() - t0 < 200000) {
    }
    cycles_per_us = (tsc() - c0) / (now_us() - t0);
    printf("wakemusl: %ld processors, %.1f cycles a microsecond, %d rounds\n", cpus, cycles_per_us,
           rounds);
    per_call("getrusage(RUSAGE_THREAD)", SIB_IDLE);
    per_call("getrusage(RUSAGE_THREAD)", SIB_SPIN);
    per_call("getrusage(RUSAGE_THREAD)", SIB_YIELD);
    per_call("sched_yield", SIB_IDLE);
    per_call("sched_yield", SIB_SPIN);
    per_call("sched_yield", SIB_YIELD);
    per_call("yield turn (getrusage, sched_yield, getrusage)", SIB_SPIN);
    wake_row("futex wake", WAKE_FUTEX);
    wake_row("eventfd write to epoll_pwait2", WAKE_EVENTFD);
    wake_row("pipe write to epoll_pwait2", WAKE_PIPE);
    wake_row("futex hand-off and back", WAKE_PINGPONG);
    if (loaded) {
        load_stop = 1;
        pthread_join(loaders[0], NULL);
        pthread_join(loaders[1], NULL);
        printf("wakemusl: load: %lu hand-offs, %.3f us each\n", load_rounds,
               (now_us() - l0) / (load_rounds ? load_rounds : 1));
    }
    printf("WAKEMUSL OK\n");
    return 0;
}
