/**
 * mutexmusl: measure a kernel mutex held briefly by several processors at
 * once, for comparison with Linux on the same machine.
 *
 * The same static binary runs under Rach2 and under Linux. Each thread,
 * pinned to a processor of its own, calls recv(MSG_DONTWAIT) on one end of
 * a Unix stream socketpair with nothing queued, which on both systems takes
 * the socket's receive mutex, finds no message and gives it back with
 * EAGAIN (Linux's u->iolock in unix_stream_read_generic, Rach2's `unix_io`
 * in ipc/unix.c). With one thread the mutex is never held by another; with
 * several, a caller often finds it held by a thread on another processor
 * for a few hundred nanoseconds. A taker that sleeps there instead of
 * spinning is a voluntary switch, which getrusage(RUSAGE_THREAD) counts.
 *
 * Each row gives the mean time of a call, read from the time-stamp counter
 * calibrated against CLOCK_MONOTONIC, and the voluntary and involuntary
 * switches of all its threads over the row. The threads start together on
 * a spin, so the row times only their calls.
 *
 * Arguments: [CALLS], 200000 calls a thread by default. Prints one line a
 * row and "MUTEXMUSL OK", or "MUTEXMUSL FAIL (detail)" and exits 1. Run as
 * process 1, the init of a private root, it waits for the console to send
 * what it wrote and powers the machine off, so that a boot of it ends by
 * itself on either system.
 */

#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/reboot.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#ifndef RUSAGE_THREAD
#define RUSAGE_THREAD 1
#endif

#define MAX_THREADS 4

static double cycles_per_us;
static long calls = 200000;
static int sv[2] = {-1, -1};

static volatile int ready;   /* threads at the start line */
static volatile int go;      /* set once all are there */
static volatile int row_err; /* errno of a call that did not fail EAGAIN */

/* One thread's result over a row. */
struct result {
    int cpu;
    uint64_t cycles;
    long voluntary;
    long involuntary;
};

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

/** Exit with `status`, or as process 1 let the console drain and power the
 * machine off, since the kernel would stop at the exit of its init. */
static void finish(int status)
{
    if (getpid() == 1) {
        fflush(stdout);
        tcdrain(STDOUT_FILENO);
        sync();
        reboot(RB_POWER_OFF);
    }
    exit(status);
}

/** Print the failure of `what` with errno and exit 1. */
static void fail(const char *what)
{
    printf("MUTEXMUSL FAIL (%s: errno %d)\n", what, errno);
    finish(1);
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

/** A thread of a row: wait at the start line, then make `calls` empty
 * receives, timing them and counting its switches. */
static void *caller(void *arg)
{
    struct result *r = arg;
    struct rusage before, after;
    uint64_t c0;
    char byte;
    long i;

    pin(r->cpu);
    __atomic_add_fetch(&ready, 1, __ATOMIC_SEQ_CST);
    while (!go) {
    }
    getrusage(RUSAGE_THREAD, &before);
    c0 = tsc();
    for (i = 0; i < calls; i++) {
        if (recv(sv[0], &byte, 1, MSG_DONTWAIT) != -1 || errno != EAGAIN) {
            row_err = errno ? errno : -1;
            break;
        }
    }
    r->cycles = tsc() - c0;
    getrusage(RUSAGE_THREAD, &after);
    r->voluntary = after.ru_nvcsw - before.ru_nvcsw;
    r->involuntary = after.ru_nivcsw - before.ru_nivcsw;
    return NULL;
}

/** Run a row of `n` threads on processors 0 to n - 1 and print it. */
static void row(int n)
{
    pthread_t t[MAX_THREADS];
    struct result r[MAX_THREADS];
    double cycles = 0;
    long vol = 0, invol = 0;
    int i;

    ready = 0;
    go = 0;
    for (i = 0; i < n; i++) {
        r[i].cpu = i;
        if (pthread_create(&t[i], NULL, caller, &r[i]) != 0) {
            fail("pthread_create");
        }
    }
    while (ready < n) {
    }
    go = 1;
    for (i = 0; i < n; i++) {
        pthread_join(t[i], NULL);
        cycles += r[i].cycles;
        vol += r[i].voluntary;
        invol += r[i].involuntary;
    }
    if (row_err) {
        errno = row_err;
        fail("recv");
    }
    printf("mutexmusl: %d thread%s, %ld calls each: %.3f us a call, %ld voluntary switches "
           "(%.2f per 1000 calls), %ld involuntary\n",
           n, n == 1 ? "" : "s", calls, cycles / n / cycles_per_us / calls, vol,
           1000.0 * vol / ((double)calls * n), invol);
}

int main(int argc, char **argv)
{
    double u0, u1;
    uint64_t c0, c1;
    long ncpu = sysconf(_SC_NPROCESSORS_ONLN);
    int n;

    if (argc > 1) {
        calls = atol(argv[1]);
    }
    if (calls <= 0) {
        printf("MUTEXMUSL FAIL (CALLS must be positive)\n");
        finish(1);
    }
    if (ncpu < 2) {
        printf("MUTEXMUSL FAIL (%ld processor; two are needed)\n", ncpu);
        finish(1);
    }
    if (ncpu > MAX_THREADS) {
        ncpu = MAX_THREADS;
    }
    u0 = now_us();
    c0 = tsc();
    while (now_us() - u0 < 200000) {
    }
    u1 = now_us();
    c1 = tsc();
    cycles_per_us = (c1 - c0) / (u1 - u0);
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) {
        fail("socketpair");
    }
    printf("mutexmusl: %ld processors, %.1f cycles a microsecond\n", ncpu, cycles_per_us);
    for (n = 1; n <= ncpu; n++) {
        row(n);
    }
    close(sv[0]);
    close(sv[1]);
    printf("MUTEXMUSL OK\n");
    finish(0);
    return 0;
}
