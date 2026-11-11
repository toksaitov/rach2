/* The kernel's sleeping locks: the mutex and the reader-writer semaphore. */
#ifndef _MUTEX_H_
#define _MUTEX_H_

#include "../lib/spinlock.h"
#include "wait_queue.h"

/*
 * A mutex has one holder, who may sleep while holding it, and a waiter
 * sleeps until it is free, as Linux's `struct mutex` (kernel/locking/
 * mutex.c). A rwsem has any number of readers or one writer, as Linux's
 * `struct rw_semaphore` (kernel/locking/rwsem.c). Each keeps its state
 * under a spinlock of its own and its sleepers on a wait queue: a sleeper
 * files itself before it tests the lock (`wq_wait_event_nointr`), and the
 * holder gives the lock up under the spinlock before it wakes them, so no
 * wake is lost. A sleep gives up the big lock, as every sleep does, and
 * neither is taken in an interrupt or under a spinlock.
 */
struct mutex {
    struct spinlock spin; /* keeps `owner` */
    const void *owner;    /* the holder's record, NULL while free */
    wait_queue_t wq;      /* its waiters */
};

/*
 * `count` is the readers that hold the lock, or -1 while a writer holds it;
 * a reader waits while a writer waits too (`writers`), so that readers that
 * keep coming do not keep a writer out, as Linux's readers queue behind a
 * waiting writer (`rwsem_down_read_slowpath`).
 */
struct rwsem {
    struct spinlock spin;
    int count;
    int writers;        /* writers waiting */
    const void *wowner; /* the writer's record while one holds it */
    wait_queue_t wq;
};

/** Make `m` a free mutex. */
void mutex_init(struct mutex *m);

/**
 * Take `m`, sleeping until it is free; a signal does not end the wait, as
 * Linux's `mutex_lock` sleeps in TASK_UNINTERRUPTIBLE (mutex.c:281). From
 * a context that may sleep, never with `m` held already.
 */
void mutex_lock(struct mutex *m);

/** Take `m` if it is free and return 1, or return 0 at once. Any context
 * but an interrupt. */
int mutex_trylock(struct mutex *m);

/** Give `m` back and wake its waiters; a panic when the caller does not
 * hold it. */
void mutex_unlock(struct mutex *m);

/** Whether the calling thread holds `m`. */
int mutex_held(const struct mutex *m);

/** Make `s` a free rwsem. */
void rwsem_init(struct rwsem *s);

/** Take `s` to read, sleeping while a writer holds it or waits for it, as
 * Linux's `down_read` (rwsem.c:1521). */
void down_read(struct rwsem *s);

/** Take `s` to read if no writer holds it or waits for it and return 1, or
 * return 0 at once. */
int down_read_trylock(struct rwsem *s);

/** Take `s` to write, sleeping until no reader or writer holds it, as
 * Linux's `down_write` (rwsem.c:1574). */
void down_write(struct rwsem *s);

/** Give back a read hold of `s`; the last reader wakes the waiters. */
void up_read(struct rwsem *s);

/** Give back the write hold of `s` and wake the waiters; a panic when the
 * caller does not hold it to write. */
void up_write(struct rwsem *s);

#endif /* _MUTEX_H_ */
