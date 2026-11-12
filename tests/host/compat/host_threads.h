/* host_threads.h: host threads that start together, for a host test whose
 * kernel headers stand in the way of <pthread.h> (host_threads.c). */
#ifndef HOST_THREADS_H
#define HOST_THREADS_H

/**
 * Run `fn(args[i])` for each `i` below `n`, each on a host thread of its
 * own, all at once: no call begins until every thread is made. Returns 0
 * once every thread was made and every one joined; -1 when one could not be
 * made, after joining those made, which then return without calling `fn`,
 * or when a join failed.
 */
int host_threads_run(int n, void *(*fn)(void *), void *const *args);

#endif
