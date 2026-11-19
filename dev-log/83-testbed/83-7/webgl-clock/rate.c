/* rate: calls a second of clock_gettime(CLOCK_MONOTONIC) through the
 * library, of the bare system call, and of getpid, in N threads. */
#define _GNU_SOURCE
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

static int mode;
static long counts[64];

static double secs(void)
{
    struct timespec t;
    syscall(SYS_clock_gettime, CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
}

static void *run(void *arg)
{
    long i = (long)arg, n = 0;
    double end = secs() + 1.0;
    struct timespec t;
    while (1) {
        int k;
        for (k = 0; k < 100; k++) {
            if (mode == 0)
                clock_gettime(CLOCK_MONOTONIC, &t);
            else if (mode == 1)
                syscall(SYS_clock_gettime, CLOCK_MONOTONIC, &t);
            else
                syscall(SYS_getpid);
        }
        n += 100;
        if (secs() > end)
            break;
    }
    counts[i] = n;
    return 0;
}

int main(void)
{
    static const char *names[] = {"vdso", "syscall", "getpid"};
    static const int ths[] = {1, 4, 8};
    pthread_t th[64];
    int t, i;
    for (mode = 0; mode < 3; mode++)
        for (t = 0; t < 3; t++) {
            long sum = 0;
            for (i = 0; i < ths[t]; i++)
                pthread_create(&th[i], 0, run, (void *)(long)i);
            for (i = 0; i < ths[t]; i++) {
                pthread_join(th[i], 0);
                sum += counts[i];
            }
            printf("rate: %s threads %d calls_per_s %ld us_per_call_per_thread %.2f\n", names[mode],
                   ths[t], sum, sum ? 1e6 * ths[t] / sum : 0.0);
        }
    return 0;
}
