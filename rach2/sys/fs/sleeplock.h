#ifndef _SLEEPLOCK_H_
#define _SLEEPLOCK_H_

/*
 * The sleeping lock of a mount or a file: one holder, who may sleep while
 * it holds it, waiters served in the order they asked, and a record of the
 * longest hold that crossed a disk wait.
 *
 * A sleeping lock keeps what must stay unchanged across a sleep, such as a
 * file's block map while its blocks are read; it is held across the sleep,
 * and every other caller that wants it waits. Its line is two counters
 * changed by atomic operations, so that callers with and without the big
 * lock take it alike, and a take and a give with no one waiting pass no
 * spinlock.
 */

#include "../lib/kint.h"
#include "../lib/spinlock.h"
#include "../proc/wait_queue.h"

struct sleep_waiter;

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

/* kernel.lock_stat: nonzero while the holds of the sleeping locks are
 * timed for their records, as Linux's lock_stat sysctl turns its record of
 * lock holds on (kernel/locking/lockdep.c); 0 at boot, since a stamp costs
 * each take a read of the clock. A hold counts only when this was on at its
 * take. */
extern int sleep_lock_stat;

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

/* The states of a lock (`state` in `struct sleep_lock`): free, held with
 * no waiter listed, and held with waiters that may be listed, which makes
 * the last give take the list's lock and hand the lock on. */
#define SLEEP_LOCK_FREE   0u
#define SLEEP_LOCK_HELD   1u
#define SLEEP_LOCK_QUEUED 2u

/*
 * One lock. `holder` is the holder's thread ID, 0 for a holder before the
 * first process, and -1 when the lock is free or handed to a waiter that
 * has not yet run; `depth` counts the holder's takes. A taker that finds
 * `state` free makes it held by one compare-and-exchange and has the lock.
 * Any other files a record (`struct sleep_waiter`, on its own stack) at the
 * end of the list from `first` to `last`, under `wait_lock`, and the last
 * give of a hold hands the lock to the first record on the list, so the
 * waiters have it in the order they asked, and a process that gives the
 * lock back and asks again waits behind everyone who asked meanwhile. A
 * killed taker takes its record off the list (`sleep_lock_take_killable`),
 * which needs no memory. A hold lasts from the first take to the last give.
 * A hold taken while `sleep_lock_stat` is on is `timed`: `taken_waits` and
 * `taken_at` are its start, by the holder's `disk_waits` and by
 * `arch_cycles` where `timer_cycles_agree` says the processors' counters
 * can time it (`taken_by_cycles`), and by `timer_now_us` elsewhere. Any
 * other hold reads neither, as Linux's mutex_lock reads no clock. `owner`
 * and `owner_cpu` are the holder's record and the processor it took the
 * lock on, which a waiter compares with what that processor runs, to spin
 * while the holder runs (`sched_spin_on`). The holder alone writes `holder`,
 * `depth`, `owner`, `owner_cpu` and the stamps; `state` changes by atomic
 * operations, from free to held without `wait_lock` and otherwise under it,
 * but for the last give of a hold with no waiter, which makes held free.
 */
struct sleep_lock {
    int holder;
    int depth;
    const struct process *volatile owner; /* the holder's record, NULL with none */
    volatile int owner_cpu;
    volatile unsigned int state; /* SLEEP_LOCK_FREE, _HELD or _QUEUED */
    struct spinlock wait_lock;   /* the list's, taken with interrupts off */
    struct sleep_waiter *first;  /* the waiter the next give hands to, or NULL */
    struct sleep_waiter *last;   /* the waiter that asked last, or NULL */
    uint64_t taken_at;
    int taken_by_cycles;
    int timed; /* the hold is stamped, for its record */
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
 * Take `l`, waiting until the lock is handed to the caller, spinning while
 * its holder runs on another processor and sleeping otherwise; a signal
 * does not end the wait, since a waiter that retried without sleeping
 * would never let the holder run. The holder takes it again without a wait.
 * From a context that may sleep unless the caller holds `l` already, with
 * or without the big lock.
 */
void sleep_lock_take(struct sleep_lock *l);

/**
 * `sleep_lock_take` whose wait SIGKILL ends, pending for the calling thread
 * or its process, as Linux's `mutex_lock_killable`: 0 with `l` held, or
 * -EINTR without it. A killed waiter takes its record off the lock's list,
 * so the waiters behind it keep their order, as Linux's killed waiter
 * leaves its mutex's list (kernel/locking/mutex.c:630-699); one that the
 * lock was handed to as it left hands it on and holds nothing. Leaving
 * needs no memory, so every killed waiter leaves at once. A take by the
 * holder, or with no process, never ends early. The caller tests SIGKILL
 * itself after a take that needed no wait.
 */
int sleep_lock_take_killable(struct sleep_lock *l);

/**
 * Give back one take of `l`. The last lets the next waiter in, and when
 * the hold was timed (`sleep_lock_stat`) and crossed a disk wait, counts it
 * in the lock's record and keeps it there if it was the longest. Panics
 * when the caller does not hold `l`.
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
