/* PI futex words, requeue and pthread PI mutexes through musl or glibc.
 * Compile with -DPI_NO_REPORT for the GNU row, which judges the exit status.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#ifndef PI_NO_REPORT
#include "../probe_report.h"
extern char **environ;
#endif

#define LOCK_PI         6
#define UNLOCK_PI       7
#define TRYLOCK_PI      8
#define WAIT_REQUEUE_PI 11
#define CMP_REQUEUE_PI  12
#define LOCK_PI2        13
#define PRIVATE         128
#define REALTIME        256

static pthread_mutex_t mutex;
static pthread_cond_t condition = PTHREAD_COND_INITIALIZER;
static int ready;
static uint32_t pi_word, cond_word;
static int thread_result;
static const char *const rows[] = {"raw",    "timeouts",       "signals",   "requeue", "mutex",
                                   "robust", "robust-handoff", "condition", "shared"};

/* The native futex syscall's timespec uses longs, including on i386. */
struct pi_timespec {
    long sec, nsec;
};

/** Make a raw futex call, returning its result or a negative errno. */
static long fx(uint32_t *word, int op, uint32_t val, const void *time, uint32_t *word2,
               uint32_t cmp)
{
    long rc = syscall(SYS_futex, word, op, val, time, word2, cmp);

    return rc < 0 ? -errno : rc;
}

/** Fail the current report row and end the process, whose threads end too. */
static void require(int ok, const char *detail)
{
    if (!ok) {
#ifndef PI_NO_REPORT
        report_stop(detail);
#endif
        fprintf(stderr, "pi-futex: fail: %s\n", detail);
        exit(1);
    }
}

/** Finish one report row after all its checks have passed. */
static void held(void)
{
#ifndef PI_NO_REPORT
    report_held();
#endif
}

/** Raw acquisition, recursive lock and nonowner unlock errors. */
static void raw_rows(void)
{
    require(fx(&pi_word, LOCK_PI | PRIVATE, 0, NULL, NULL, 0) == 0, "lock PI");
    require((pi_word & 0x3fffffffu) == (uint32_t)syscall(SYS_gettid), "owner TID");
    require(fx(&pi_word, TRYLOCK_PI | PRIVATE, 0, NULL, NULL, 0) == -EDEADLK, "recursive trylock");
    require(fx(&pi_word, LOCK_PI2 | PRIVATE, 0, NULL, NULL, 0) == -EDEADLK, "recursive lock PI2");
    require(fx(&pi_word, UNLOCK_PI | PRIVATE, 0, NULL, NULL, 0) == 0, "unlock PI");
    require(pi_word == 0, "unlocked word");
    require(fx(&pi_word, UNLOCK_PI | PRIVATE, 0, NULL, NULL, 0) == -EPERM, "nonowner unlock");
    held();
}

/** Contended lock deadlines on both clocks and both PI lock operations. */
static void *timeout_thread(void *arg)
{
    struct timespec now;
    struct pi_timespec ts;
    int op = (int)(intptr_t)arg;
    int clk = op == LOCK_PI || (op & REALTIME) ? CLOCK_REALTIME : CLOCK_MONOTONIC;

    if (clock_gettime(clk, &now) != 0) {
        thread_result = 1;
        return NULL;
    }
    ts.sec = now.tv_sec;
    ts.nsec = now.tv_nsec;
    thread_result = fx(&pi_word, op | PRIVATE, 0, &ts, NULL, 0) == -ETIMEDOUT ? 0 : 1;
    return NULL;
}

/** Run the timed contenders while this thread holds the raw lock. */
static void timeout_rows(void)
{
    pthread_t t;
    int ops[] = {LOCK_PI, LOCK_PI2, LOCK_PI2 | REALTIME};
    unsigned int i;

    require(fx(&pi_word, LOCK_PI | PRIVATE, 0, NULL, NULL, 0) == 0, "timeout owner lock");
    for (i = 0; i < sizeof(ops) / sizeof(ops[0]); i++) {
        require(pthread_create(&t, NULL, timeout_thread, (void *)(intptr_t)ops[i]) == 0,
                "start timed contender");
        require(pthread_join(t, NULL) == 0 && thread_result == 0, "PI absolute timeout");
    }
    require(fx(&pi_word, UNLOCK_PI | PRIVATE, 0, NULL, NULL, 0) == 0, "timeout owner unlock");
    held();
}

static volatile sig_atomic_t signal_seen;

/** Record delivery without SA_RESTART. */
static void pi_signal(int signo)
{
    (void)signo;
    signal_seen = 1;
}

/** Lock despite a handled signal, then release the acquired word. */
static void *signal_thread(void *arg)
{
    (void)arg;
    thread_result = (int)fx(&pi_word, LOCK_PI2 | PRIVATE, 0, NULL, NULL, 0);
    if (!thread_result) {
        thread_result = (int)fx(&pi_word, UNLOCK_PI | PRIVATE, 0, NULL, NULL, 0);
    }
    return NULL;
}

/** PI acquisition restarts after a handler without SA_RESTART, as Linux's
 * ERESTARTNOINTR does; WAITERS proves the contender entered the kernel. */
static void signal_rows(void)
{
    struct sigaction action = {0}, old;
    pthread_t t;

    action.sa_handler = pi_signal;
    sigemptyset(&action.sa_mask);
    require(sigaction(SIGUSR1, &action, &old) == 0, "PI signal handler");
    require(fx(&pi_word, LOCK_PI | PRIVATE, 0, NULL, NULL, 0) == 0, "signal owner lock");
    require(pthread_create(&t, NULL, signal_thread, NULL) == 0, "signal contender");
    while (!(__atomic_load_n(&pi_word, __ATOMIC_ACQUIRE) & 0x80000000u)) {
        sched_yield();
    }
    require(pthread_kill(t, SIGUSR1) == 0, "signal blocked PI contender");
    while (!signal_seen) {
        sched_yield();
    }
    require(fx(&pi_word, UNLOCK_PI | PRIVATE, 0, NULL, NULL, 0) == 0, "signal owner unlock");
    require(pthread_join(t, NULL) == 0 && !thread_result, "PI lock restarts without SA_RESTART");
    require(sigaction(SIGUSR1, &old, NULL) == 0, "restore signal handler");
    held();
}

/** Wait to be requeued, then release the mutex the requeue acquired. */
static void *requeue_thread(void *arg)
{
    (void)arg;
    thread_result = (int)fx(&cond_word, WAIT_REQUEUE_PI | PRIVATE, 0, NULL, &pi_word, 0);
    if (!thread_result) {
        thread_result = (int)fx(&pi_word, UNLOCK_PI | PRIVATE, 0, NULL, NULL, 0);
    }
    return NULL;
}

/** Requeue a sleeping condition waiter onto an owned PI mutex. */
static void requeue_rows(void)
{
    pthread_t t;
    long count;

    require(fx(&pi_word, LOCK_PI | PRIVATE, 0, NULL, NULL, 0) == 0, "requeue owner lock");
    require(pthread_create(&t, NULL, requeue_thread, NULL) == 0, "start requeue waiter");
    do {
        count = fx(&cond_word, CMP_REQUEUE_PI | PRIVATE, 1, NULL, &pi_word, 0);
        require(count >= 0, "compare requeue PI");
        if (!count) {
            sched_yield();
        }
    } while (!count);
    require(count == 1, "one requeued waiter");
    require(fx(&pi_word, UNLOCK_PI | PRIVATE, 0, NULL, NULL, 0) == 0, "requeue handoff");
    require(pthread_join(t, NULL) == 0 && thread_result == 0, "requeued waiter owns mutex");
    held();
}

/** A pthread contender that acquires and releases the PI mutex. */
static void *mutex_thread(void *arg)
{
    (void)arg;
    thread_result = pthread_mutex_lock(&mutex);
    if (!thread_result) {
        thread_result = pthread_mutex_unlock(&mutex);
    }
    return NULL;
}

/** Pthread PI attributes, contention, trylock and robust owner recovery. */
static void mutex_rows(void)
{
    pthread_mutexattr_t attr;
    pthread_t t;

    require(pthread_mutexattr_init(&attr) == 0, "mutex attributes");
    require(pthread_mutexattr_setprotocol(&attr, PTHREAD_PRIO_INHERIT) == 0, "PI protocol");
    require(pthread_mutex_init(&mutex, &attr) == 0, "PI mutex init");
    require(pthread_mutex_lock(&mutex) == 0, "pthread owner lock");
    require(pthread_mutex_trylock(&mutex) == EBUSY, "pthread trylock busy");
    require(pthread_create(&t, NULL, mutex_thread, NULL) == 0, "pthread contender");
    require(pthread_mutex_unlock(&mutex) == 0, "pthread owner unlock");
    require(pthread_join(t, NULL) == 0 && !thread_result, "pthread contender completes");
    require(pthread_mutex_destroy(&mutex) == 0, "destroy PI mutex");
    require(pthread_mutexattr_destroy(&attr) == 0, "destroy mutex attributes");
    held();
}

/** End with a robust PI mutex still owned. */
static void *robust_thread(void *arg)
{
    pthread_mutex_t *m = arg;

    thread_result = pthread_mutex_lock(m);
    return NULL;
}

/** Recover a robust PI mutex after its owning thread has exited. */
static void robust_rows(void)
{
    pthread_mutexattr_t attr;
    pthread_t t;

    require(pthread_mutexattr_init(&attr) == 0, "robust attributes");
    require(pthread_mutexattr_setprotocol(&attr, PTHREAD_PRIO_INHERIT) == 0, "robust PI protocol");
    require(pthread_mutexattr_setrobust(&attr, PTHREAD_MUTEX_ROBUST) == 0, "robust flag");
    require(pthread_mutex_init(&mutex, &attr) == 0, "robust PI mutex init");
    require(pthread_create(&t, NULL, robust_thread, &mutex) == 0, "robust owner");
    require(pthread_join(t, NULL) == 0 && !thread_result, "robust owner exits");
    require(pthread_mutex_lock(&mutex) == EOWNERDEAD, "robust PI owner death");
    require(pthread_mutex_consistent(&mutex) == 0, "robust PI consistency");
    require(pthread_mutex_unlock(&mutex) == 0, "robust PI recovered unlock");
    require(pthread_mutex_destroy(&mutex) == 0, "destroy robust PI mutex");
    pthread_mutexattr_destroy(&attr);
    held();
}

/* The robust list's head and one PI-tagged entry, in native pointer width. */
struct pi_robust_head {
    uintptr_t next;
    long offset;
    uintptr_t pending;
};
struct pi_robust_node {
    uintptr_t next;
    uint32_t word;
};
static struct pi_robust_head death_head;
static struct pi_robust_node death_node;
static int death_ready, death_exit, death_result;

/** Register a native robust PI list and end while holding its word. */
static void *death_owner(void *arg)
{
    (void)arg;
    death_head.next = (uintptr_t)&death_node | 1u;
    death_head.offset = (long)((char *)&death_node.word - (char *)&death_node);
    death_head.pending = 0;
    death_node.next = (uintptr_t)&death_head;
    death_result = (int)syscall(SYS_set_robust_list, &death_head, sizeof(death_head));
    if (!death_result) {
        death_result = (int)fx(&death_node.word, LOCK_PI | PRIVATE, 0, NULL, NULL, 0);
    }
    __atomic_store_n(&death_ready, 1, __ATOMIC_RELEASE);
    while (!__atomic_load_n(&death_exit, __ATOMIC_ACQUIRE)) {
        sched_yield();
    }
    return NULL;
}

/** Acquire the dead owner's word and confirm the handoff carries OWNER_DIED. */
static void *death_waiter(void *arg)
{
    (void)arg;
    thread_result = (int)fx(&death_node.word, LOCK_PI | PRIVATE, 0, NULL, NULL, 0);
    if (!thread_result && !(death_node.word & 0x40000000u)) {
        thread_result = 1;
    }
    if (!thread_result) {
        thread_result = (int)fx(&death_node.word, UNLOCK_PI | PRIVATE, 0, NULL, NULL, 0);
    }
    return NULL;
}

/** Force exit with a PI waiter queued on a native or compat robust list. */
static void robust_handoff_rows(void)
{
    pthread_t owner_thread, waiter_thread;

    require(pthread_create(&owner_thread, NULL, death_owner, NULL) == 0, "dying PI owner");
    while (!__atomic_load_n(&death_ready, __ATOMIC_ACQUIRE)) {
        sched_yield();
    }
    require(!death_result, "robust PI list registration and owner lock");
    require(pthread_create(&waiter_thread, NULL, death_waiter, NULL) == 0, "death PI waiter");
    while (!(__atomic_load_n(&death_node.word, __ATOMIC_ACQUIRE) & 0x80000000u)) {
        sched_yield();
    }
    __atomic_store_n(&death_exit, 1, __ATOMIC_RELEASE);
    require(pthread_join(owner_thread, NULL) == 0, "dying owner ends");
    require(pthread_join(waiter_thread, NULL) == 0 && !thread_result, "robust PI exit handoff");
    held();
}

/** Signal a condition while holding the associated PI mutex. */
static void *condition_thread(void *arg)
{
    (void)arg;
    thread_result = pthread_mutex_lock(&mutex);
    if (!thread_result) {
        ready = 1;
        thread_result = pthread_cond_signal(&condition);
        if (!thread_result) {
            thread_result = pthread_mutex_unlock(&mutex);
        }
    }
    return NULL;
}

/** A pthread condition variable releases and reacquires a PI mutex. */
static void condition_rows(void)
{
    pthread_mutexattr_t attr;
    pthread_t t;

    require(pthread_mutexattr_init(&attr) == 0 &&
                pthread_mutexattr_setprotocol(&attr, PTHREAD_PRIO_INHERIT) == 0 &&
                pthread_mutex_init(&mutex, &attr) == 0,
            "condition PI mutex");
    require(pthread_mutex_lock(&mutex) == 0, "condition owner");
    require(pthread_create(&t, NULL, condition_thread, NULL) == 0, "condition signaler");
    while (!ready) {
        require(pthread_cond_wait(&condition, &mutex) == 0, "condition PI wait");
    }
    require(pthread_mutex_unlock(&mutex) == 0, "condition unlock");
    require(pthread_join(t, NULL) == 0 && !thread_result, "condition signaler finishes");
    pthread_mutex_destroy(&mutex);
    pthread_cond_destroy(&condition);
    pthread_mutexattr_destroy(&attr);
    held();
}

/** Recover a process-shared robust PI mutex whose owner was another process. */
static void shared_rows(void)
{
    pthread_mutex_t *m;
    pthread_mutexattr_t attr;
    pid_t child;
    int status;

    m = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    require(m != MAP_FAILED, "shared mutex mapping");
    require(pthread_mutexattr_init(&attr) == 0 &&
                pthread_mutexattr_setprotocol(&attr, PTHREAD_PRIO_INHERIT) == 0 &&
                pthread_mutexattr_setrobust(&attr, PTHREAD_MUTEX_ROBUST) == 0 &&
                pthread_mutexattr_setpshared(&attr, PTHREAD_PROCESS_SHARED) == 0 &&
                pthread_mutex_init(m, &attr) == 0,
            "shared robust PI mutex");
    child = fork();
    require(child >= 0, "fork shared mutex owner");
    if (!child) {
        _exit(pthread_mutex_lock(m) == 0 ? 0 : 1);
    }
    require(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0,
            "shared owner exits");
    require(pthread_mutex_lock(m) == EOWNERDEAD, "shared PI owner died");
    require(pthread_mutex_consistent(m) == 0 && pthread_mutex_unlock(m) == 0, "shared PI recovery");
    pthread_mutex_destroy(m);
    pthread_mutexattr_destroy(&attr);
    munmap(m, 4096);
    held();
}

/** Run the PI futex and libc rows, ending at process exit. */
int main(void)
{
#ifndef PI_NO_REPORT
    unsigned int i;

    report_begin(report_case(environ, "guest.default.pifutexmusl"));
    for (i = 0; i < sizeof(rows) / sizeof(rows[0]); i++) {
        report_plan(rows[i]);
    }
#else
    (void)rows;
#endif
    raw_rows();
    timeout_rows();
    signal_rows();
    requeue_rows();
    mutex_rows();
    robust_rows();
    robust_handoff_rows();
    condition_rows();
    shared_rows();
#ifndef PI_NO_REPORT
    report_end();
#endif
    puts("PIFUTEXMUSL OK");
    return 0;
}
