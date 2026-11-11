/* pingpong: the cost of a system call and of a futex hand-off between threads. */
#define _GNU_SOURCE
#include <linux/futex.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

static _Atomic int word[8];
static int rounds;

static double now(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
}

static void fwait(_Atomic int *w, int v)
{
    while (atomic_load(w) == v)
        syscall(SYS_futex, w, FUTEX_WAIT_PRIVATE, v, NULL, NULL, 0);
}

static void fwake(_Atomic int *w)
{
    syscall(SYS_futex, w, FUTEX_WAKE_PRIVATE, 1, NULL, NULL, 0);
}

/* Worker i waits for word[i] to turn odd, then even, answering on word[i+4]. */
static void *worker(void *arg)
{
    int i = (int)(long)arg, r;
    cpu_set_t set;

    if (getenv("PIN")) {
        CPU_ZERO(&set);
        CPU_SET(i % 3 + 1, &set);
        sched_setaffinity(0, sizeof set, &set);
    }

    for (r = 0; r < rounds; r++) {
        fwait(&word[i], 2 * r);
        atomic_store(&word[i + 4], r + 1);
        fwake(&word[i + 4]);
    }
    return NULL;
}

int main(int argc, char **argv)
{
    int n = argc > 1 ? atoi(argv[1]) : 1, i, r;
    pthread_t t[4];
    double t0;

    rounds = argc > 2 ? atoi(argv[2]) : 20000;
    double pause = argc > 3 ? atof(argv[3]) / 1e6 : 0;
    if (getenv("PIN")) {
        cpu_set_t set;
        CPU_ZERO(&set);
        CPU_SET(0, &set);
        sched_setaffinity(0, sizeof set, &set);
    }
    t0 = now();
    for (r = 0; r < 200000; r++)
        syscall(SYS_getpid);
    printf("getpid: %.3f us\n", (now() - t0) / 200000 * 1e6);
    t0 = now();
    for (r = 0; r < 200000; r++)
        sched_yield();
    printf("sched_yield: %.3f us\n", (now() - t0) / 200000 * 1e6);
    for (i = 0; i < n; i++)
        pthread_create(&t[i], NULL, worker, (void *)(long)i);
    t0 = now();
    double spent = 0;
    for (r = 0; r < rounds; r++) {
        double p0 = now();
        while (now() - p0 < pause)
            ;
        spent += now() - p0;
        for (i = 0; i < n; i++) {
            atomic_store(&word[i], 2 * r + 1);
            fwake(&word[i]);
        }
        for (i = 0; i < n; i++)
            fwait(&word[i + 4], r);
    }
    printf("handoff to %d threads and back after %.0f us: %.3f us a round\n", n, pause * 1e6,
           (now() - t0 - spent) / rounds * 1e6);
    for (i = 0; i < n; i++)
        pthread_join(t[i], NULL);
    return 0;
}
