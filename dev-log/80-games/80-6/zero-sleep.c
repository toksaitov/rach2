/* Measure zero relative sleeps beside a runnable thread on one processor. */
#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <sys/resource.h>
#include <time.h>

static int ready, stop;
static unsigned long steps;

/** Count work on the caller's processor until stop is published. */
static void *worker(void *unused)
{
    (void)unused;
    __atomic_store_n(&ready, 1, __ATOMIC_SEQ_CST);
    while (!__atomic_load_n(&stop, __ATOMIC_SEQ_CST))
        __atomic_add_fetch(&steps, 1, __ATOMIC_SEQ_CST);
    return NULL;
}

/** Print voluntary switches and worker progress for each relative sleep. */
int main(void)
{
    cpu_set_t cpus;
    pthread_t thread;
    struct rusage before, after;
    struct timespec zero = {0, 0};
    unsigned long first, last;
    int cpu, mode, i, rc;

    if (sched_getaffinity(0, sizeof(cpus), &cpus) != 0) return 1;
    for (cpu = 0; cpu < CPU_SETSIZE && !CPU_ISSET(cpu, &cpus); cpu++) {}
    if (cpu == CPU_SETSIZE) return 1;
    CPU_ZERO(&cpus);
    CPU_SET(cpu, &cpus);
    if (sched_setaffinity(0, sizeof(cpus), &cpus) != 0) return 1;
    if (pthread_create(&thread, NULL, worker, NULL) != 0) return 1;
    while (!__atomic_load_n(&ready, __ATOMIC_SEQ_CST)) sched_yield();
    for (mode = 0; mode != 3; mode++) {
        first = __atomic_load_n(&steps, __ATOMIC_SEQ_CST);
        if (getrusage(RUSAGE_THREAD, &before) != 0) return 1;
        for (i = 0; i < 16; i++) {
            if (mode == 0) rc = nanosleep(&zero, NULL);
            else rc = clock_nanosleep(mode == 1 ? CLOCK_REALTIME : CLOCK_MONOTONIC, 0, &zero, NULL);
            if (rc != 0) { fprintf(stderr, "zero-sleep: mode %d: error %d errno %d\n", mode, rc, errno); return 1; }
        }
        if (getrusage(RUSAGE_THREAD, &after) != 0) return 1;
        last = __atomic_load_n(&steps, __ATOMIC_SEQ_CST);
        printf("zero-sleep: mode %d calls 16 voluntary %ld worker %lu\n", mode,
               after.ru_nvcsw - before.ru_nvcsw, last - first);
    }
    __atomic_store_n(&stop, 1, __ATOMIC_SEQ_CST);
    return pthread_join(thread, NULL) != 0;
}
