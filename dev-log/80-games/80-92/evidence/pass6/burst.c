/* Spin BURST us, sleep SLEEP us, for SECS s in each of 4 threads; print the
 * busy time each measured by CLOCK_MONOTONIC and the ticks charged to it. */
#define _GNU_SOURCE
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/syscall.h>

static long burst_us, sleep_us;
static double secs;

static double now(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
}

struct arg { double busy; long tid; long ut, st; };

static void ticks(long tid, long *ut, long *st)
{
    char p[64], b[1024], *s;
    FILE *f;
    snprintf(p, sizeof p, "/proc/self/task/%ld/stat", tid);
    f = fopen(p, "r");
    *ut = *st = -1;
    if (!f) return;
    if (fgets(b, sizeof b, f) && (s = strrchr(b, ')')))
        sscanf(s + 2, "%*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %ld %ld", ut, st);
    fclose(f);
}

static void *run(void *v)
{
    struct arg *a = v;
    double end = now() + secs, t0, t;
    struct timespec z = {0, sleep_us * 1000};
    a->tid = syscall(SYS_gettid);
    while (now() < end) {
        t0 = now();
        do t = now(); while (t - t0 < burst_us / 1e6);
        a->busy += t - t0;
        if (sleep_us) nanosleep(&z, 0);
    }
    ticks(a->tid, &a->ut, &a->st);
    return 0;
}

int main(int c, char **v)
{
    pthread_t th[4];
    struct arg a[4];
    double busy = 0;
    long ut = 0, st = 0;
    int i;
    burst_us = atol(v[1]); sleep_us = atol(v[2]); secs = atof(v[3]);
    memset(a, 0, sizeof a);
    for (i = 0; i < 4; i++) pthread_create(&th[i], 0, run, &a[i]);
    for (i = 0; i < 4; i++) pthread_join(th[i], 0);
    for (i = 0; i < 4; i++) { busy += a[i].busy; ut += a[i].ut; st += a[i].st; }
    printf("burst %ld us sleep %ld us: busy %.2f s by the clock, charged %.2f s user + %.2f s system (%ld per second), ratio %.2f\n",
           burst_us, sleep_us, busy, ut / (double)sysconf(_SC_CLK_TCK), st / (double)sysconf(_SC_CLK_TCK),
           sysconf(_SC_CLK_TCK), (ut + st) / (double)sysconf(_SC_CLK_TCK) / busy);
    return 0;
}
