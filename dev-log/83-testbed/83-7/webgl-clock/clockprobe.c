/* clockprobe: threads read a clock and check it never goes backwards,
 * within a thread or against the latest value any thread published. */
#define _GNU_SOURCE
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

static clockid_t clk;
static int secs;
static _Atomic long long best;
static _Atomic long reads, back_self, back_cross, worst_self, worst_cross;

static long long now(void)
{
    struct timespec ts;
    clock_gettime(clk, &ts);
    return ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

static void maxl(_Atomic long *a, long v)
{
    long o = atomic_load(a);
    while (v > o && !atomic_compare_exchange_weak(a, &o, v))
        ;
}

static void *run(void *arg)
{
    long long end = now() + secs * 1000000000LL, last = 0, g, t;
    long n = 0;
    (void)arg;
    for (;;) {
        g = atomic_load(&best);
        t = now();
        n++;
        if (t < last) {
            atomic_fetch_add(&back_self, 1);
            maxl(&worst_self, (long)(last - t));
        }
        if (t < g) {
            atomic_fetch_add(&back_cross, 1);
            maxl(&worst_cross, (long)(g - t));
        }
        last = t;
        g = atomic_load(&best);
        while (t > g && !atomic_compare_exchange_weak(&best, &g, t))
            ;
        if (t > end)
            break;
    }
    atomic_fetch_add(&reads, n);
    return 0;
}

int main(int argc, char **argv)
{
    static const struct { const char *name; clockid_t id; } c[] = {
        {"monotonic", CLOCK_MONOTONIC}, {"realtime", CLOCK_REALTIME},
        {"boottime", CLOCK_BOOTTIME}, {"monotonic_raw", CLOCK_MONOTONIC_RAW},
        {"monotonic_coarse", CLOCK_MONOTONIC_COARSE}};
    int nt = argc > 1 ? atoi(argv[1]) : 8, i, k;
    pthread_t th[64];
    struct timespec res;
    secs = argc > 2 ? atoi(argv[2]) : 3;
    for (k = 0; k < 5; k++) {
        clk = c[k].id;
        atomic_store(&best, 0);
        atomic_store(&reads, 0);
        atomic_store(&back_self, 0);
        atomic_store(&back_cross, 0);
        atomic_store(&worst_self, 0);
        atomic_store(&worst_cross, 0);
        for (i = 0; i < nt; i++)
            pthread_create(&th[i], 0, run, 0);
        for (i = 0; i < nt; i++)
            pthread_join(th[i], 0);
        clock_getres(clk, &res);
        printf("clockprobe: %s: res %ld ns reads %ld back_self %ld worst %ld ns back_cross %ld worst %ld ns\n",
               c[k].name, res.tv_nsec, atomic_load(&reads), atomic_load(&back_self),
               atomic_load(&worst_self), atomic_load(&back_cross), atomic_load(&worst_cross));
    }
    return 0;
}
