/* The kernel's sleeping locks: the mutex and the reader-writer semaphore. */
#ifndef _MUTEX_H_
#define _MUTEX_H_

#include "../lib/spinlock.h"
#include "wait_queue.h"

/*
 * A mutex has one holder, who may sleep while holding it, and a waiter
 * sleeps until it is free, as Linux's `struct mutex` (kernel/locking/
 * mutex.c). A rwsem has any number of readers or one writer, as Linux's
 * `struct rw_semaphore` (kernel/locking/rwsem.c). Each keeps its state in
 * an atomic word and its sleepers on a wait queue: a sleeper files itself
 * before it tests the lock (`wq_wait_event_nointr`), and the holder gives
 * the lock up in a full barrier before it looks for sleepers to wake, so
 * no wake is lost.
 * A sleep gives up the big lock, as every sleep does, and neither is taken
 * in an interrupt or under a spinlock. A record of zeroes is a free mutex.
 */
/*
 * `owner` is the holder's record, a thread's or a processor's before the
 * first thread, with MUTEX_HANDOFF in its low bit, and 0 while the mutex is
 * free, changed only by an atomic exchange, as Linux's `owner` holds its
 * task and flags (kernel/locking/mutex.c:52-78): a take that finds it free
 * is one compare-and-exchange that records the holder with it, and a
 * release that finds no flag is one more (mutex.c:166-182). Records are at
 * least two bytes aligned, so the low bit is free for the flag.
 *
 * A taker that finds the mutex held spins while the holder runs on another
 * processor and sleeps only when it does not, as Linux's
 * `mutex_optimistic_spin` (mutex.c:441-510). `owner_cpu` is the processor
 * the holder took the mutex on, a hint the spinner checks against what that
 * processor runs (`sched_spin_on`). The spinners queue on `osq`, each on a
 * record of its processor's, so that one at a time tries the lock word, as
 * Linux's `osq_lock` (kernel/locking/osq_lock.c). A waiter that slept and,
 * woken, finds the mutex taken again sets MUTEX_HANDOFF, and the holder's
 * release then gives the mutex to the oldest sleeper instead of freeing it,
 * as Linux's MUTEX_FLAG_HANDOFF (mutex.c:62-75, 920-976), so that spinners
 * do not starve the sleepers.
 */
#define MUTEX_HANDOFF 1u

struct mutex {
    volatile uintptr_t owner;  /* the holder's record and MUTEX_HANDOFF, 0 while free */
    volatile int owner_cpu;    /* the processor the holder took it on, -1 if not known */
    volatile unsigned int osq; /* the last spinner's processor + 1, 0 with none */
    wait_queue_t wq;           /* its sleepers */
#ifdef RACH2_LOCKDEP
    struct lockdep_map dep; /* its class, for the lock-order checker */
#endif
};

/*
 * `state` is one word, changed only by an atomic compare-and-exchange: the
 * readers that hold the lock in RWSEM_READERS, RWSEM_WRITER while a writer
 * holds it and RWSEM_WAITING while a writer waits. A take that finds the
 * lock free for it and a release that wakes nobody are one exchange each,
 * as Linux's fast paths are one atomic operation on `count`
 * (kernel/locking/rwsem.c:248-272, 1336-1349); the spinlock keeps only the
 * count of waiting writers and its bit. A reader waits while a writer
 * waits too, so that readers that keep coming do not keep a writer out, as
 * Linux's readers queue behind a waiting writer
 * (`rwsem_down_read_slowpath`).
 */
#define RWSEM_READERS 0x3FFFFFFFu
#define RWSEM_WRITER  0x40000000u
#define RWSEM_WAITING 0x80000000u

struct rwsem {
    struct spinlock spin; /* keeps `writers` and the RWSEM_WAITING bit */
    volatile unsigned int state;
    int writers;        /* writers waiting */
    const void *wowner; /* the writer's record while one holds it */
    wait_queue_t wq;
#ifdef RACH2_LOCKDEP
    struct lockdep_map dep;
#endif
};

/* Each init site of a mutex or a rwsem is a class of the lock-order
 * checker, named `name`, as Linux's `mutex_init` macro makes one
 * (include/linux/mutex.h). */
#ifdef RACH2_LOCKDEP
#define mutex_init(m, name)                       \
    do {                                          \
        static struct lockdep_key mutex_key_;     \
        mutex_init_key((m), (name), &mutex_key_); \
    } while (0)
#define rwsem_init(s, name)                       \
    do {                                          \
        static struct lockdep_key rwsem_key_;     \
        rwsem_init_key((s), (name), &rwsem_key_); \
    } while (0)
#else
#define mutex_init(m, name) mutex_init_key((m), (name), 0)
#define rwsem_init(s, name) rwsem_init_key((s), (name), 0)
#endif

/** Make `m` a free mutex of the checker's class `key`, named `name`;
 * `mutex_init` passes its site's key. */
void mutex_init_key(struct mutex *m, const char *name, const struct lockdep_key *key);

/**
 * Take `m`, spinning while its holder runs on another processor and
 * sleeping until it is free or handed over otherwise; a signal does not end
 * the wait, as Linux's `mutex_lock` sleeps in TASK_UNINTERRUPTIBLE
 * (mutex.c:281). From a context that may sleep, never with `m` held
 * already.
 */
void mutex_lock(struct mutex *m);

/** Take `m` as `mutex_lock` does, as the `subclass`th of two or more
 * mutexes of one class held at once, in an order the caller keeps, as
 * Linux's `mutex_lock_nested`: an epoll set's lock under the lock of the
 * set it is in, at the depth it is looked into. */
void mutex_lock_nested(struct mutex *m, unsigned int subclass);

/** Take `m` as `mutex_lock` does, but end the wait at a signal, as
 * Linux's `mutex_lock_interruptible`: 0 once held, or -EINTR with `m` not
 * taken. The wait tests the signal-work flag; with no mutex held it
 * judges a signal under the big lock, which can discard an ignored one.
 * A caller without the big lock holds no lock ordered after it. */
int mutex_lock_interruptible(struct mutex *m);

/** Take `m` as `mutex_lock` does, but end the wait at SIGKILL pending for
 * the thread or its process, as Linux's `mutex_lock_killable`
 * (kernel/locking/mutex.c:1014): 0 once held, or -EINTR with `m` not taken.
 * Any other signal leaves the wait asleep. From a thread. */
int mutex_lock_killable(struct mutex *m);

/** Take `m` if it is free and return 1, or return 0 at once. Any context
 * but an interrupt. */
int mutex_trylock(struct mutex *m);

/** Give `m` back and wake its waiters; a panic when the caller does not
 * hold it. */
void mutex_unlock(struct mutex *m);

/** Whether the calling thread holds `m`. */
int mutex_held(const struct mutex *m);

/** Make `s` a free rwsem of the checker's class `key`, as `mutex_init_key`. */
void rwsem_init_key(struct rwsem *s, const char *name, const struct lockdep_key *key);

/** Take `s` to read, sleeping while a writer holds it or waits for it, as
 * Linux's `down_read` (rwsem.c:1521). */
void down_read(struct rwsem *s);

/** Take `s` to read if no writer holds it or waits for it and return 1, or
 * return 0 at once. */
int down_read_trylock(struct rwsem *s);

/** Take `s` to write, sleeping until no reader or writer holds it, as
 * Linux's `down_write` (rwsem.c:1574). */
void down_write(struct rwsem *s);

/** `down_write` that SIGKILL pending for the thread or its process ends,
 * as Linux's `down_write_killable` (rwsem.c:1586): 0 once held, or -EINTR
 * with `s` not held. */
int down_write_killable(struct rwsem *s);

/** Take `s` to write if nobody holds it and return 1, or return 0 at once,
 * as Linux's `down_write_trylock` (rwsem.c:1603). */
int down_write_trylock(struct rwsem *s);

/** Give back a read hold of `s`; the last reader wakes the waiters. */
void up_read(struct rwsem *s);

/** Give back the write hold of `s` and wake the waiters; a panic when the
 * caller does not hold it to write. */
void up_write(struct rwsem *s);

#endif /* _MUTEX_H_ */
