/* Check Wine's yield decision and resource usage across exit and reap. */
#define _GNU_SOURCE
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

static volatile int *state;
static pid_t child = -1;
static struct rusage thread_usage;

/** Reap the child on every exit, including a failed assertion. */
static void cleanup(void)
{
    if (child > 0) {
        kill(child, SIGKILL);
        waitpid(child, NULL, 0);
    }
    if (state != NULL && state != MAP_FAILED) {
        munmap((void *)state, sizeof(*state));
    }
}

/** Stop at a failed contract and let the registered cleanup reap the child. */
static void require(int ok, const char *what)
{
    if (!ok) {
        fprintf(stderr, "rusage-switch: %s\n", what);
        exit(1);
    }
}

/** Yield beside the main thread and retain this thread's final usage. */
static void *worker(void *unused)
{
    (void)unused;
    while (*state == 0) {
        sched_yield();
    }
    require(getrusage(RUSAGE_THREAD, &thread_usage) == 0, "worker usage");
    return NULL;
}

/** Force a switch on one CPU, then check group, child totals. */
int main(void)
{
    cpu_set_t mask;
    cpu_set_t one;
    struct rusage before, after, whole, children, reaped;
    pthread_t thread;
    int i, status;
    long yielded;

    atexit(cleanup);
    require(sched_getaffinity(0, sizeof(mask), &mask) == 0, "read affinity");
    CPU_ZERO(&one);
    for (i = 0; i < CPU_SETSIZE; i++) {
        if (CPU_ISSET(i, &mask)) {
            CPU_SET(i, &one);
            break;
        }
    }
    require(sched_setaffinity(0, sizeof(one), &one) == 0, "pin one CPU");
    state = mmap(NULL, sizeof(*state), PROT_READ | PROT_WRITE,
                 MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    require(state != MAP_FAILED, "shared state");
    require(pthread_create(&thread, NULL, worker, NULL) == 0, "start worker");
    require(getrusage(RUSAGE_THREAD, &before) == 0, "before yield");
    for (i = 0; i < 32; i++) {
        sched_yield();
    }
    require(getrusage(RUSAGE_THREAD, &after) == 0, "after yield");
    require(after.ru_nvcsw + after.ru_nivcsw > before.ru_nvcsw + before.ru_nivcsw,
            "Wine must observe a switch across yield");
    yielded = after.ru_nvcsw + after.ru_nivcsw - before.ru_nvcsw - before.ru_nivcsw;
    *state = 1;
    require(pthread_join(thread, NULL) == 0, "join worker");
    require(getrusage(RUSAGE_SELF, &whole) == 0, "whole process");
    require(whole.ru_nvcsw >= after.ru_nvcsw + thread_usage.ru_nvcsw &&
            whole.ru_nivcsw >= after.ru_nivcsw + thread_usage.ru_nivcsw,
            "ended thread is in process usage");
    require(getrusage(RUSAGE_CHILDREN, &before) == 0, "children before");
    child = fork();
    require(child >= 0, "fork");
    if (child == 0) {
        *state = 2;
        while (*state == 2) {
            sched_yield();
        }
        _exit(0);
    }
    while (*state != 2) {
        sched_yield();
    }
    for (i = 0; i < 32; i++) {
        sched_yield();
    }
    *state = 3;
    require(wait4(child, &status, 0, &reaped) == child, "wait4 child");
    child = -1;
    require(WIFEXITED(status) && WEXITSTATUS(status) == 0, "child status");
    require(reaped.ru_nvcsw + reaped.ru_nivcsw > 0, "wait4 switches");
    require(getrusage(RUSAGE_CHILDREN, &children) == 0, "children after");
    require(children.ru_nvcsw - before.ru_nvcsw == reaped.ru_nvcsw &&
            children.ru_nivcsw - before.ru_nivcsw == reaped.ru_nivcsw,
            "reap adds reported switches once");
    printf("rusage-switch: result: yield %ld, ended thread %ld, child %ld\n",
           yielded,
           thread_usage.ru_nvcsw + thread_usage.ru_nivcsw,
           reaped.ru_nvcsw + reaped.ru_nivcsw);
    return 0;
}
