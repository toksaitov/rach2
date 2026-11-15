/* threadscaling: count process, VMA and selection work beside blocked
 * threads. */
#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "../probe_report.h"

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t started = PTHREAD_COND_INITIALIZER;
static pthread_cond_t release = PTHREAD_COND_INITIALIZER;
static unsigned int waiting;
static int go;
static int runner_stop;
static const char *keys[] = {"process_pid_steps", "process_uid_steps",    "process_timer_steps",
                             "vm_gap_steps",      "process_timer_passes", "scheduler_picks",
                             "scheduler_steps"};

/** Read the seven modulo-2^32 work counts; -1 if any field is absent. */
static int counts(uint32_t value[7])
{
    FILE *f = fopen("/proc/stat", "r");
    char line[256], key[80];
    unsigned long n;
    unsigned int i, found = 0;

    if (!f) {
        return -1;
    }
    while (fgets(line, sizeof(line), f)) {
        if (sscanf(line, "%79s %lu", key, &n) != 2) {
            continue;
        }
        for (i = 0; i < 7; i++) {
            if (strcmp(key, keys[i]) == 0) {
                value[i] = (uint32_t)n;
                found |= 1u << i;
            }
        }
    }
    fclose(f);
    return found == 127 ? 0 : -1;
}

/** Announce entry and wait under the mutex until the parent releases all. */
static void *worker(void *arg)
{
    (void)arg;
    pthread_mutex_lock(&lock);
    waiting++;
    pthread_cond_signal(&started);
    while (!go) {
        pthread_cond_wait(&release, &lock);
    }
    pthread_mutex_unlock(&lock);
    return NULL;
}

/** Keep a second thread runnable until the parent ends the sample. */
static void *runner(void *arg)
{
    (void)arg;
    while (!__atomic_load_n(&runner_stop, __ATOMIC_SEQ_CST)) {
        sched_yield();
    }
    return NULL;
}

/** Count selection work beside n blocked threads with two runnable
 * threads, the caller and a runner, both kept on the caller's processor so
 * that each yield selects: a yield with nothing else ready on its processor
 * returns without a selection. Restore the caller's affinity and join the
 * runner before returning. Return -1 when the affinity, the runner or the
 * bound fails, and zero otherwise. */
static int selections(unsigned int n)
{
    pthread_t running;
    uint32_t before[7], after[7];
    cpu_set_t all, one;
    unsigned int i;
    int failed = 0, running_made = 0, cpu = sched_getcpu();

    CPU_ZERO(&one);
    if (cpu < 0 || cpu >= CPU_SETSIZE || sched_getaffinity(0, sizeof(all), &all) != 0) {
        fprintf(stderr, "threadscaling: cannot read the processor or its affinity\n");
        return -1;
    }
    CPU_SET(cpu, &one);
    if (sched_setaffinity(0, sizeof(one), &one) != 0) {
        fprintf(stderr, "threadscaling: cannot keep to processor %d\n", cpu);
        return -1;
    }
    runner_stop = 0;
    /* the runner inherits the caller's single processor */
    if (pthread_create(&running, NULL, runner, NULL) == 0) {
        running_made = 1;
    } else {
        failed = 1;
    }
    if (counts(before) != 0) {
        failed = 1;
    } else {
        for (i = 0; i < 1024; i++) {
            sched_yield();
        }
        if (counts(after) != 0) {
            failed = 1;
        } else {
            uint32_t picks = after[5] - before[5];
            uint32_t steps = after[6] - before[6];

            printf("threadscaling: held %u: picks %u entries %u\n", n, picks, steps);
            if (picks < 1024 || (uint64_t)steps > 8u * (uint64_t)picks + 64u) {
                failed = 1;
            }
        }
    }
    __atomic_store_n(&runner_stop, 1, __ATOMIC_SEQ_CST);
    if (running_made && pthread_join(running, NULL) != 0) {
        failed = 1;
    }
    if (sched_setaffinity(0, sizeof(all), &all) != 0) {
        failed = 1;
    }
    return failed ? -1 : 0;
}

/** Count process, VMA and timer work with n blocked threads, then the
 * selection work beside them (`selections`). Release and join all created
 * threads on success or failure. Return -1 when creation, joining or a
 * work bound fails, and zero otherwise. */
static int batch(unsigned int n)
{
    pthread_t threads[256];
    uint32_t before[7], after[7], idle[7];
    struct timespec pause = {0, 20000000};
    unsigned int made = 0, i;
    int failed = 0, rc;

    waiting = 0;
    go = 0;
    if (counts(before) != 0) {
        return -1;
    }
    while (made < n) {
        rc = pthread_create(&threads[made], NULL, worker, NULL);
        if (rc != 0) {
            fprintf(stderr, "threadscaling: pthread_create %u: %s\n", made, strerror(rc));
            failed = 1;
            break;
        }
        made++;
    }
    pthread_mutex_lock(&lock);
    while (waiting != made) {
        pthread_cond_wait(&started, &lock);
    }
    pthread_mutex_unlock(&lock);
    for (i = 0; i < made; i++) {
        if (pthread_kill(threads[i], 0) != 0) {
            failed = 1;
        }
    }
    if (counts(after) != 0) {
        failed = 1;
    } else {
        printf("threadscaling: held %u: pid %u uid %u timers %u gaps %u\n", made,
               (uint32_t)(after[0] - before[0]), (uint32_t)(after[1] - before[1]),
               (uint32_t)(after[2] - before[2]), (uint32_t)(after[3] - before[3]));
        if ((uint32_t)(after[0] - before[0]) > 32u * n + 64u ||
            (uint32_t)(after[1] - before[1]) > 8u * n + 64u ||
            (uint32_t)(after[3] - before[3]) > 8u * n + 64u) {
            failed = 1;
        }
        do {
            /* interval: the held threads sleep through 20 ms of ticks */
            rc = nanosleep(&pause, &pause);
        } while (rc != 0 && errno == EINTR);
        if (rc != 0) {
            failed = 1;
        }
        if (counts(idle) != 0 ||
            (uint32_t)(idle[2] - after[2]) > 16u * ((uint32_t)(idle[4] - after[4]) + 1u)) {
            failed = 1;
        }
        if (selections(n) != 0) {
            failed = 1;
        }
    }
    pthread_mutex_lock(&lock);
    go = 1;
    pthread_cond_broadcast(&release);
    pthread_mutex_unlock(&lock);
    for (i = 0; i < made; i++) {
        if (pthread_join(threads[i], NULL) != 0) {
            failed = 1;
        }
    }
    return failed ? -1 : 0;
}

/** Exercise the unprivileged process limit path with 16, 64 and 256 threads. */
int main(void)
{
    static const char *assertions[] = {"identity", "threads-16", "threads-64", "threads-256"};
    static const unsigned int sizes[] = {16, 64, 256};
    unsigned int i;

    report_begin(report_case(environ, "guest.default.threadscaling"));
    for (i = 0; i < sizeof(assertions) / sizeof(assertions[0]); i++) {
        report_plan(assertions[i]);
    }
    if (getuid() == 0 && setuid(1000) != 0) {
        report_stop("setuid failed");
        return 1;
    }
    if (getuid() == 0) {
        report_stop("real uid is still root");
        return 1;
    }
    report_held();
    for (i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
        if (batch(sizes[i]) != 0) {
            report_stop("thread creation, lookup, joining or work count failed");
            return 1;
        }
        report_held();
    }
    report_end();
    puts("THREADSCALING OK");
    return 0;
}
