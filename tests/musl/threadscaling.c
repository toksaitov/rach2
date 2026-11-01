/* threadscaling: count process-index and VMA work while creating held threads. */
#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
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
static const char *keys[] = {"process_pid_steps", "process_uid_steps", "process_timer_steps",
                             "vm_gap_steps", "process_timer_passes"};

/** Read the five modulo-2^32 work counts; -1 if any field is absent. */
static int counts(uint32_t value[5])
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
        for (i = 0; i < 5; i++) {
            if (strcmp(key, keys[i]) == 0) {
                value[i] = (uint32_t)n;
                found |= 1u << i;
            }
        }
    }
    fclose(f);
    return found == 31 ? 0 : -1;
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

/** Create n held threads, count the work, then release and join every one.
 * Every creation result is checked; failures still release and join all
 * threads already made. Return -1 if creation, joining or a work bound fails.
 */
static int batch(unsigned int n)
{
    pthread_t threads[256];
    uint32_t before[5], after[5], idle[5];
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
            rc = nanosleep(&pause, &pause);
        } while (rc != 0 && errno == EINTR);
        if (rc != 0) {
            failed = 1;
        }
        if (counts(idle) != 0 ||
            (uint32_t)(idle[2] - after[2]) > 16u * ((uint32_t)(idle[4] - after[4]) + 1u)) {
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
