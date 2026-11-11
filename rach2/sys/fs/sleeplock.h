#ifndef _SLEEPLOCK_H_
#define _SLEEPLOCK_H_

/*
 * The sleeping lock of a mount or a file: one holder, who may sleep while
 * it holds it, waiters served in the order they asked, and a record of the
 * longest hold that crossed a disk wait.
 *
 * A sleeping lock keeps what must stay unchanged across a sleep, such as a
 * file's block map while its blocks are read; it is held across the sleep,
 * and every other caller that wants it waits. Its own state is kept under a
 * spinlock of its own, so that callers with and without the big lock take
 * it alike.
 */

#include "../lib/kint.h"
#include "../lib/spinlock.h"
#include "../proc/wait_queue.h"

/*
 * The longest hold across a disk wait of one kind of lock, such as every
 * mount's lock or every inode's, and who held it. A hold crossed a disk
 * wait when its holder finished a wait in the block layer while it held
 * the lock (`disk_waits` in process.h). The kinds are listed from
 * `sleep_lock_records`, each once, from the first lock initialised with it.
 */
struct sleep_lock_record {
    const char *kind;      /* the name a listing shows */
    uint32_t crossings;    /* holds that crossed a disk wait, modulo 2^32 */
    uint32_t longest_us;   /* the longest of them, in microseconds */
    uint32_t longest_pid;  /* its holder's thread ID */
    char longest_comm[16]; /* its holder's name, NUL-padded */
    struct sleep_lock_record *next;
    int listed; /* nonzero once on the list */
#ifdef RACH2_LOCKDEP
    struct lockdep_key key; /* the checker's class of the kind's locks */
#endif
};

/* A record with nothing counted yet, of the kind named `kind`. */
#ifdef RACH2_LOCKDEP
#define SLEEP_LOCK_RECORD(kind) {(kind), 0, 0, 0, {0}, 0, 0, {0}}
#else
#define SLEEP_LOCK_RECORD(kind) {(kind), 0, 0, 0, {0}, 0, 0}
#endif

/* A lock that counts in its holder's `file_locks` (process.h): a node's
 * lock and a mount's allocator lock, under which reclaim must not enter a
 * file system. */
#define SLEEP_LOCK_FILE 1

/*
 * One lock. `holder` is the holder's thread ID, 0 for a holder before the
 * first process, and -1 when the lock is free; `depth` counts the holder's
 * takes. A caller takes a place in line (`next`) and waits until the lock
 * is free and its place is the one served (`serving`), so a process that
 * gives the lock back and asks again waits behind everyone who asked
 * meanwhile. A hold lasts from the first take to the last give;
 * `taken_waits` and `taken_at` are its start, by the holder's `disk_waits`
 * and by `arch_cycles` where `timer_cycles_agree` says the processors'
 * counters can time it (`taken_by_cycles`), so that a take reads the
 * counter and no clock, as Linux's mutex_lock reads none, and by
 * `timer_now_us` elsewhere. `spin` keeps every field but `wq`.
 */
struct sleep_lock {
    struct spinlock spin;
    int holder;
    int depth;
    uint32_t next;
    uint32_t serving;
    uint64_t taken_at;
    int taken_by_cycles;
    uint32_t taken_waits;
    int flags;                        /* SLEEP_LOCK_FILE or 0 */
    struct sleep_lock_record *record; /* its kind's, or NULL */
    wait_queue_t wq;
#ifdef RACH2_LOCKDEP
    struct lockdep_map dep; /* its class: its kind's, for the checker */
#endif
};

/* Take the next sleeping lock the calling thread takes at subclass `n` of
 * its class, for the checker: a rename's second directory and the nodes
 * under its two, of one class with the first, as Linux's
 * `inode_lock_nested` names them (include/linux/fs.h). Nothing without the
 * checker. */
#ifdef RACH2_LOCKDEP
#define sleep_lock_subclass(n)                    \
    do {                                          \
        if (current_proc) {                       \
            current_proc->lockdep_subclass = (n); \
        }                                         \
    } while (0)
#else
#define sleep_lock_subclass(n) ((void)0)
#endif

/** Make `l` a free lock of the kind `record`, which may be NULL for a lock
 * whose holds are not recorded, with `flags` SLEEP_LOCK_FILE or 0. The
 * record is listed the first time a lock names it. */
void sleep_lock_init(struct sleep_lock *l, struct sleep_lock_record *record, int flags);

/**
 * Take `l`, sleeping until it is free and the caller's place is served; a
 * signal does not end the wait, since a waiter that retried without
 * sleeping would never let the holder run. The holder takes it again
 * without a place. From a context that may sleep unless the caller holds
 * `l` already, with or without the big lock.
 */
void sleep_lock_take(struct sleep_lock *l);

/**
 * Give back one take of `l`. The last lets the next waiter in, and when
 * the hold crossed a disk wait, counts it in the lock's record and keeps it
 * there if it was the longest. Panics when the caller does not hold `l`.
 */
void sleep_lock_give(struct sleep_lock *l);

/** Whether the calling thread holds `l`. */
int sleep_lock_held(const struct sleep_lock *l);

/** Panic with `message` unless the calling thread holds `l`. A change that
 * must be made under the lock calls it first, so that a caller without the
 * lock is found on its first change, not by a race. */
void sleep_lock_assert(const struct sleep_lock *l, const char *message);

/** The first record of the kinds of lock, in the order they were first
 * initialised; each links to the next by `next`, the last to NULL. */
const struct sleep_lock_record *sleep_lock_records(void);

#endif
