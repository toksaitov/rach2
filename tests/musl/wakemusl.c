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
 * The set rows hold IDLE eventfds that never become ready, as a server's
 * idle connections: the cost of one epoll_wait with a zero timeout on a set
 * of them and one eventfd that is ready, and of one poll of all of them;
 * and the eventfd wake row again with the idle ones in the waiter's set.
 * Linux's wait looks only at the ready registration, and its poll at every
 * descriptor.
 *
 * With the third argument `load`, two more threads on processors 2 and 3
 * hand a futex back and forth between them for the whole run, so that
 * every row runs while other processors block and wake, as the loop's
 * other threads do; that needs four processors.
 *
 * With the word `calls` among the arguments after PAUSE, on Rach2, the
 * woken thread, named `wakewaiter`, is the one thread debugfs's bkl/calls
 * counts during the wake rows (`thread wakewaiter`, debugfs(5)), and the
 * file is printed after them, each line after "calls: "; debugfs is
 * mounted on /sys/kernel/debug first when it is not there. Linux has no
 * such file, and the word does nothing there.
 *
 * With the word `busy`, it runs two other rows in place of all of these,
 * for a wake that finds the processors busy; they need four processors.
 * In the first, a thread spins in user mode on processor 1, where the
 * waiter sleeps, and the futex wake's time to run is that of the
 * scheduler's preemption of the spinner. In the second, two threads that
 * may run anywhere sleep on one futex, and one FUTEX_WAKE wakes both, as
 * the GPU's one interrupt wakes WineD3D's command thread and the i915
 * worker. Before each round a thread pinned to each processor of 1 to 3
 * that either sleeper ran on last spins until both have run, as the main
 * thread spins on processor 0, so that each wake must find a processor of
 * its own; each woken thread spins 20 us before it answers, so that one
 * placed behind the other waits for it. The row gives the wake-to-run of
 * the first and of the second woken in each round, the rounds in which
 * both ran on one processor, and those in which either ran on a processor
 * whose spinner it had to preempt.
 *
 * Arguments: [ROUNDS [PAUSE [load] [calls] [busy]]], 20000 wakes a row and
 * 50 us by default. Prints one line for each row and "WAKEMUSL OK", or
 * "WAKEMUSL FAIL (detail)" and exits 1. It needs at least two processors.
 */

#define _GNU_SOURCE
#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/mount.h>
#include <sys/prctl.h>
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

#define CALLS     200000
/* The idle eventfds of the set rows, and the calls of their per-call rows. */
#define IDLE      500
#define SET_CALLS 20000

enum sibling { SIB_IDLE, SIB_SPIN, SIB_YIELD };
enum waker { WAKE_FUTEX, WAKE_EVENTFD, WAKE_PIPE, WAKE_PINGPONG, WAKE_EVENTFD_SET };

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
static int idle[IDLE];            /* the set rows' eventfds, never written */
static volatile int load_word[2]; /* the load's hand-off, one word a side */
static int load_side[2] = {0, 1}; /* each load thread's argument */
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

/** Open the IDLE eventfds of the set rows. */
static void idle_open(void)
{
    int i;

    for (i = 0; i < IDLE; i++) {
        idle[i] = eventfd(0, 0);
        if (idle[i] < 0) {
            fail("eventfd for the set rows");
        }
    }
}

/** Add every idle eventfd to the set `ep`, watched for input. */
static void idle_add(int ep)
{
    struct epoll_event ev;
    int i;

    memset(&ev, 0, sizeof ev);
    ev.events = EPOLLIN;
    for (i = 0; i < IDLE; i++) {
        if (epoll_ctl(ep, EPOLL_CTL_ADD, idle[i], &ev) != 0) {
            fail("epoll_ctl of an idle eventfd");
        }
    }
}

/** Print the mean cost of one epoll_wait with a zero timeout on a set of
 * the idle eventfds and one ready, and of one poll of them all. */
static void set_calls(void)
{
    static struct pollfd fds[IDLE + 1];
    struct epoll_event ev;
    uint64_t one = 1, c0;
    int ep = epoll_create1(0);
    int ready = eventfd(0, 0);
    int i;

    if (ep < 0 || ready < 0 || write(ready, &one, sizeof one) != sizeof one) {
        fail("setup of the set rows");
    }
    idle_add(ep);
    memset(&ev, 0, sizeof ev);
    ev.events = EPOLLIN;
    if (epoll_ctl(ep, EPOLL_CTL_ADD, ready, &ev) != 0) {
        fail("epoll_ctl of the ready eventfd");
    }
    c0 = tsc();
    for (i = 0; i < SET_CALLS; i++) {
        if (epoll_wait(ep, &ev, 1, 0) != 1) {
            fail("epoll_wait of the set row");
        }
    }
    printf("wakemusl: epoll_wait, %d idle and 1 ready: %.3f us a call\n", IDLE,
           (tsc() - c0) / cycles_per_us / SET_CALLS);
    for (i = 0; i < IDLE; i++) {
        fds[i].fd = idle[i];
        fds[i].events = POLLIN;
    }
    fds[IDLE].fd = ready;
    fds[IDLE].events = POLLIN;
    c0 = tsc();
    for (i = 0; i < SET_CALLS; i++) {
        if (poll(fds, IDLE + 1, 0) != 1) {
            fail("poll of the set row");
        }
    }
    printf("wakemusl: poll, %d idle and 1 ready: %.3f us a call\n", IDLE,
           (tsc() - c0) / cycles_per_us / SET_CALLS);
    close(ready);
    close(ep);
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
    prctl(PR_SET_NAME, "wakewaiter", 0, 0, 0);
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
        if ((wake_kind == WAKE_EVENTFD || wake_kind == WAKE_EVENTFD_SET) &&
            read(efd, &value, sizeof value) != sizeof value) {
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

/** One side of the load, on processor 2 + the side `arg` points to: wait
 * for its word to change, then change and wake the other side's. */
static void *load(void *arg)
{
    int me = *(const int *)arg;
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

#define CALLS_FILE "/sys/kernel/debug/bkl/calls"

/** Write `text` to bkl/calls, mounting debugfs first when the file is not
 * there: 0, or -1 where there is no such file, as on Linux. */
static int calls_write(const char *text)
{
    FILE *f;

    if (access(CALLS_FILE, F_OK) != 0) {
        mount("debugfs", "/sys/kernel/debug", "debugfs", 0, NULL);
    }
    f = fopen(CALLS_FILE, "w");
    if (!f) {
        return -1;
    }
    fputs(text, f);
    return fclose(f) == 0 ? 0 : -1;
}

/** Print bkl/calls, each line after "calls: ". */
static void calls_print(void)
{
    char line[256];
    FILE *f = fopen(CALLS_FILE, "r");

    if (!f) {
        fail("open bkl/calls");
    }
    while (fgets(line, sizeof line, f)) {
        printf("calls: %s", line);
    }
    fclose(f);
}

/** qsort's order of two counter values, smallest first. */
static int by_value(const void *a, const void *b)
{
    uint64_t x = *(const uint64_t *)a;
    uint64_t y = *(const uint64_t *)b;

    return x < y ? -1 : x > y;
}

/** Make the set, and the eventfd or the pipe in it, that the waiter of a
 * wake row of the kind `kind` sleeps on; a futex row needs none. */
static void wake_set_up(int kind)
{
    struct epoll_event ev;

    if (kind != WAKE_EVENTFD && kind != WAKE_EVENTFD_SET && kind != WAKE_PIPE) {
        return;
    }
    epfd = epoll_create1(0);
    if (epfd < 0) {
        fail("epoll_create1");
    }
    if (kind == WAKE_EVENTFD_SET) {
        idle_add(epfd);
    }
    if (kind != WAKE_PIPE) {
        efd = eventfd(0, 0);
        if (efd < 0) {
            fail("eventfd");
        }
    } else if (pipe(pfd) != 0) {
        fail("pipe");
    }
    memset(&ev, 0, sizeof ev);
    ev.events = EPOLLIN;
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, kind == WAKE_PIPE ? pfd[0] : efd, &ev) != 0) {
        fail("epoll_ctl");
    }
}

/** Close what `wake_set_up` made. */
static void wake_tear_down(void)
{
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

/** Run one wake row of the kind `kind` and print it. */
static void wake_row(const char *name, int kind)
{
    pthread_t t;
    uint64_t pause = (uint64_t)(pause_us * cycles_per_us);
    uint64_t call = 0;
    uint64_t total = 0;
    uint64_t start;
    uint64_t one = 1;
    int r;

    wake_kind = kind;
    word = 0;
    back = 0;
    answered = 0;
    wake_set_up(kind);
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
        } else if (kind == WAKE_EVENTFD || kind == WAKE_EVENTFD_SET) {
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
    wake_tear_down();
}

/* The busy rows' two sleepers and the spinners on processors 1 to 3. */
#define PAIR_WORK_US 20
static volatile int pair_word;     /* the futex both sleepers wait on */
static volatile int pair_answered; /* the sleepers that ran this round */
static volatile int pair_last[2];  /* each sleeper's processor last round */
static volatile int pair_stop;
static uint64_t *pair_run[2];           /* each round's wake-to-run, by sleeper */
static int *pair_cpu[2];                /* each round's processor, by sleeper */
static volatile int hot[4];             /* a spinner sleeps on it while 0; -1 ends it */
static volatile int hot_running[4];     /* set by a spinner that runs */
static int hot_index[4] = {0, 1, 2, 3}; /* each spinner's argument */

/** A spinner of the busy rows, on processor `arg` points to: spin while its
 * word is set, sleep on it while it is 0. */
static void *hot_spinner(void *arg)
{
    int c = *(const int *)arg;

    pin(c);
    while (!pair_stop) {
        while (hot[c] == 0 && !pair_stop) {
            futex(&hot[c], FUTEX_WAIT_PRIVATE, 0);
        }
        __atomic_store_n(&hot_running[c], 1, __ATOMIC_SEQ_CST);
        while (hot[c] != 0 && !pair_stop) {
        }
        __atomic_store_n(&hot_running[c], 0, __ATOMIC_SEQ_CST);
    }
    return NULL;
}

/** One of the busy row's two sleepers, which may run anywhere; `arg`
 * points to its index. */
static void *pair_sleeper(void *arg)
{
    int me = *(const int *)arg;
    uint64_t work = (uint64_t)(PAIR_WORK_US * cycles_per_us), t;
    cpu_set_t set;
    int r;

    /* The main thread's pin to processor 0 is inherited: undo it. */
    CPU_ZERO(&set);
    for (r = 0; r < 4; r++) {
        CPU_SET(r, &set);
    }
    if (sched_setaffinity(0, sizeof set, &set) != 0) {
        fail("sched_setaffinity");
    }
    for (r = 0; r < rounds; r++) {
        while (pair_word == r) {
            futex(&pair_word, FUTEX_WAIT_PRIVATE, r);
        }
        t = tsc();
        pair_run[me][r] = t - sent;
        pair_cpu[me][r] = sched_getcpu();
        while (tsc() - t < work) {
        }
        pair_last[me] = pair_cpu[me][r];
        __atomic_add_fetch(&pair_answered, 1, __ATOMIC_SEQ_CST);
    }
    return NULL;
}

/** Print the mean, median, p90 and p99 of `n` counter values, which it
 * sorts, after `name`. */
static void print_spread(const char *name, uint64_t *v, int n)
{
    uint64_t sum = 0;
    int i;

    for (i = 0; i < n; i++) {
        sum += v[i];
    }
    qsort(v, (size_t)n, sizeof v[0], by_value);
    printf("wakemusl: %s: mean %.3f us, median %.3f, p90 %.3f, p99 %.3f\n", name,
           sum / cycles_per_us / n, v[n / 2] / cycles_per_us, v[n * 9 / 10] / cycles_per_us,
           v[n * 99 / 100] / cycles_per_us);
}

/** The first busy row: a futex wake onto processor 1 while a thread spins
 * there in user mode. */
static void spin_row(void)
{
    pthread_t t;

    sibling_mode = SIB_SPIN;
    sibling_stop = 0;
    sibling_ready = 0;
    if (pthread_create(&t, NULL, sibling, NULL) != 0) {
        fail("pthread_create");
    }
    while (!sibling_ready) {
    }
    wake_row("futex wake onto a spinning thread's processor", WAKE_FUTEX);
    sibling_stop = 1;
    pthread_join(t, NULL);
}

/** Set the spinners of the processors in `busy` spinning, or with `on` 0
 * stop them all, and wait until each has done so. */
static void hot_set(const int *busy, int on)
{
    int c;

    for (c = 1; c < 4; c++) {
        if (busy[c] || !on) {
            __atomic_store_n(&hot[c], on, __ATOMIC_SEQ_CST);
            if (on) {
                futex(&hot[c], FUTEX_WAKE_PRIVATE, 1);
            }
        }
    }
    for (c = 1; c < 4; c++) {
        while ((busy[c] || !on) && __atomic_load_n(&hot_running[c], __ATOMIC_SEQ_CST) != on) {
        }
    }
}

/** Whether processor `cpu`, which a sleeper ran on, had a spinner to
 * preempt: one of 1 to 3 set spinning in `busy`. */
static int was_busy(const int *busy, int cpu)
{
    return cpu > 0 && cpu < 4 && busy[cpu];
}

/** Round `r` of the second busy row: the spinners of the sleepers' last
 * processors set spinning, the pause, the one wake of both, and the wait
 * for both; the first to run's time goes in pair_run[0]. Adds the round to
 * `shared`, `preempted` and `where`. */
static void pair_round(int r, int *shared, int *preempted, int *where)
{
    uint64_t pause = (uint64_t)(pause_us * cycles_per_us), start, x;
    int busy[4] = {1, 0, 0, 0}, i;

    for (i = 0; i < 2; i++) {
        busy[pair_last[i]] = 1;
    }
    hot_set(busy, 1);
    start = tsc();
    while (tsc() - start < pause) {
    }
    __atomic_store_n(&pair_answered, 0, __ATOMIC_SEQ_CST);
    sent = tsc();
    __atomic_store_n(&pair_word, r + 1, __ATOMIC_SEQ_CST);
    futex(&pair_word, FUTEX_WAKE_PRIVATE, 2);
    while (__atomic_load_n(&pair_answered, __ATOMIC_SEQ_CST) < 2) {
    }
    hot_set(busy, 0);
    for (i = 0; i < 2; i++) {
        where[pair_cpu[i][r] >= 0 && pair_cpu[i][r] < 4 ? pair_cpu[i][r] : 4]++;
    }
    *shared += pair_cpu[0][r] == pair_cpu[1][r];
    *preempted += was_busy(busy, pair_cpu[0][r]) || was_busy(busy, pair_cpu[1][r]);
    if (pair_run[0][r] > pair_run[1][r]) {
        x = pair_run[0][r];
        pair_run[0][r] = pair_run[1][r];
        pair_run[1][r] = x;
    }
}

/** The second busy row: two wakes in one call whose sleepers' last
 * processors are busy. */
static void pair_row(void)
{
    pthread_t t[2], spinners[3];
    int side[2] = {0, 1}, r, c, i, shared = 0, preempted = 0;
    int where[5] = {0, 0, 0, 0, 0}; /* the wakes run on processors 0 to 3, or elsewhere */

    for (i = 0; i < 2; i++) {
        pair_run[i] = calloc((size_t)rounds, sizeof pair_run[i][0]);
        pair_cpu[i] = calloc((size_t)rounds, sizeof pair_cpu[i][0]);
        if (!pair_run[i] || !pair_cpu[i]) {
            fail("calloc");
        }
    }
    for (c = 1; c < 4; c++) {
        if (pthread_create(&spinners[c - 1], NULL, hot_spinner, &hot_index[c]) != 0) {
            fail("pthread_create");
        }
    }
    for (i = 0; i < 2; i++) {
        if (pthread_create(&t[i], NULL, pair_sleeper, &side[i]) != 0) {
            fail("pthread_create");
        }
    }
    for (r = 0; r < rounds; r++) {
        pair_round(r, &shared, &preempted, where);
    }
    for (i = 0; i < 2; i++) {
        pthread_join(t[i], NULL);
    }
    pair_stop = 1;
    for (c = 1; c < 4; c++) {
        /* A spinner that read the old stop flag then waits on 0 and fails. */
        __atomic_store_n(&hot[c], -1, __ATOMIC_SEQ_CST);
        futex(&hot[c], FUTEX_WAKE_PRIVATE, 1);
        pthread_join(spinners[c - 1], NULL);
    }
    printf("wakemusl: two futex wakes in one call, last processors busy, after %.0f us: "
           "%d of %d rounds on one processor, %d with a spinner preempted\n",
           pause_us, shared, rounds, preempted);
    printf("wakemusl: two wakes, processors run on: 0 %d, 1 %d, 2 %d, 3 %d, other %d\n", where[0],
           where[1], where[2], where[3], where[4]);
    print_spread("two wakes, first to run", pair_run[0], rounds);
    print_spread("two wakes, second to run", pair_run[1], rounds);
    for (i = 0; i < 2; i++) {
        free(pair_run[i]);
        free(pair_cpu[i]);
    }
}

int main(int argc, char **argv)
{
    double t0;
    uint64_t c0;
    long cpus = sysconf(_SC_NPROCESSORS_ONLN);
    int loaded = 0, calls = 0, busy = 0, i;
    pthread_t loaders[2];
    double l0;

    setvbuf(stdout, NULL, _IOLBF, 0);
    if (argc > 1) {
        rounds = atoi(argv[1]);
    }
    if (argc > 2) {
        pause_us = atof(argv[2]);
    }
    for (i = 3; i < argc; i++) {
        loaded |= strcmp(argv[i], "load") == 0;
        calls |= strcmp(argv[i], "calls") == 0;
        busy |= strcmp(argv[i], "busy") == 0;
    }
    if (rounds < 100 || cpus < (loaded || busy ? 4 : 2)) {
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
    printf("wakemusl: %ld processors, %.1f cycles a microsecond, %d rounds%s\n", cpus,
           cycles_per_us, rounds, loaded ? ", under load" : "");
    if (busy) {
        spin_row();
        pair_row();
        printf("WAKEMUSL OK\n");
        return 0;
    }
    l0 = now_us();
    if (loaded && (pthread_create(&loaders[0], NULL, load, &load_side[0]) != 0 ||
                   pthread_create(&loaders[1], NULL, load, &load_side[1]) != 0)) {
        fail("pthread_create");
    }
    per_call("getrusage(RUSAGE_THREAD)", SIB_IDLE);
    per_call("getrusage(RUSAGE_THREAD)", SIB_SPIN);
    per_call("getrusage(RUSAGE_THREAD)", SIB_YIELD);
    per_call("sched_yield", SIB_IDLE);
    per_call("sched_yield", SIB_SPIN);
    per_call("sched_yield", SIB_YIELD);
    per_call("yield turn (getrusage, sched_yield, getrusage)", SIB_SPIN);
    idle_open();
    set_calls();
    if (calls && calls_write("thread wakewaiter\n") != 0) {
        printf("wakemusl: no bkl/calls\n");
        calls = 0;
    }
    wake_row("futex wake", WAKE_FUTEX);
    wake_row("eventfd write to epoll_pwait2", WAKE_EVENTFD);
    wake_row("pipe write to epoll_pwait2", WAKE_PIPE);
    wake_row("eventfd write to epoll_pwait2 over 500 idle", WAKE_EVENTFD_SET);
    wake_row("futex hand-off and back", WAKE_PINGPONG);
    if (calls) {
        calls_print();
        if (calls_write("all\n") != 0) {
            fail("write bkl/calls");
        }
    }
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
