/* host_threads.c: host threads that start together, on the host's
 * <pthread.h>, which this file includes apart from the kernel's headers. */
#include <pthread.h>
#include <stdlib.h>

#include "host_threads.h"

/* One thread of `host_threads_run`: its handle, its call, and the flag it
 * waits on, 1 to run the call and -1 to return without it. */
struct host_thread {
    pthread_t id;
    void *(*fn)(void *);
    void *arg;
    int *go;
};

/** Start of a thread of `host_threads_run`: wait for its flag, then make
 * its call or return without it. */
static void *host_thread_start(void *p)
{
    struct host_thread *t = p;
    int go;

    while ((go = __atomic_load_n(t->go, __ATOMIC_ACQUIRE)) == 0) {
    }
    return go > 0 ? t->fn(t->arg) : NULL;
}

int host_threads_run(int n, void *(*fn)(void *), void *const *args)
{
    struct host_thread *t = calloc((size_t)n, sizeof(*t));
    int go = 0;
    int i, made = 0, joined = 0;

    if (!t) {
        return -1;
    }
    for (i = 0; i < n; i++) {
        t[i].fn = fn;
        t[i].arg = args[i];
        t[i].go = &go;
        if (pthread_create(&t[i].id, NULL, host_thread_start, &t[i]) != 0) {
            break;
        }
        made++;
    }
    __atomic_store_n(&go, made == n ? 1 : -1, __ATOMIC_RELEASE);
    for (i = 0; i < made; i++) {
        joined += pthread_join(t[i].id, NULL) == 0;
    }
    free(t);
    return made == n && joined == n ? 0 : -1;
}
