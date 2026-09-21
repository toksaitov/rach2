/** Run one opener until it waits, then resume it after the writer returns. */
#include <pthread.h>
#include <stdlib.h>

static pthread_t thread;
static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t condition = PTHREAD_COND_INITIALIZER;
static int stopped, resumed;
static void (*opener)(void);

/** Publish completion to the writer. */
static void *run(void *unused)
{
    (void)unused;
    opener();
    pthread_mutex_lock(&mutex);
    stopped = 1;
    pthread_cond_broadcast(&condition);
    pthread_mutex_unlock(&mutex);
    return NULL;
}

/** Start the opener and wait until it blocks or completes. */
void open_thread_start(void (*fn)(void))
{
    stopped = resumed = 0;
    opener = fn;
    if (pthread_create(&thread, NULL, run, NULL) != 0) {
        abort();
    }
    pthread_mutex_lock(&mutex);
    while (!stopped) {
        pthread_cond_wait(&condition, &mutex);
    }
    pthread_mutex_unlock(&mutex);
}

/** Suspend the opener inside the kernel's namespace-lock wait. */
void open_thread_wait(void)
{
    pthread_mutex_lock(&mutex);
    stopped = 1;
    pthread_cond_broadcast(&condition);
    while (!resumed) {
        pthread_cond_wait(&condition, &mutex);
    }
    pthread_mutex_unlock(&mutex);
}

/** Resume the opener and wait for its descriptor to be registered. */
void open_thread_finish(void)
{
    pthread_mutex_lock(&mutex);
    resumed = 1;
    pthread_cond_broadcast(&condition);
    pthread_mutex_unlock(&mutex);
    if (pthread_join(thread, NULL) != 0) {
        abort();
    }
}
