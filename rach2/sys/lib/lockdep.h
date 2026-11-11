/* The lock-order checker of the debug build, after Linux's lockdep. */
#ifndef _LOCKDEP_H_
#define _LOCKDEP_H_

#include "kint.h"
#include "kuser.h"

/*
 * A kernel built with `KEXTRA=-DRACH2_LOCKDEP` (the `rach2-lockdep`
 * component) learns the order in which its locks are taken and reports,
 * once for each pair of locks, an order that could deadlock: a cycle, a
 * lock taken twice, a lock taken in an interrupt and elsewhere with
 * interrupts on, a sleep or sleeping lock under a spinlock or with
 * interrupts off, and the big lock taken under a spinlock. The boot goes
 * on after a report. lockdep.Readme.md describes the classes, the checks
 * and what they print.
 *
 * Without the switch every hook below is an empty macro and a lock carries
 * no map, so the release kernel's code and data do not change.
 */

/* An init site's key: a lock initialised at it belongs to the class of the
 * site, whose key is this object's address (Linux's lock_class_key). */
struct lockdep_key {
    char unused; /* C89 has no empty structure */
};

/* What the checker knows of one lock. */
struct lockdep_map {
    const char *name;              /* the class's name, or NULL for its address */
    const struct lockdep_key *key; /* the init site's key, or NULL */
    unsigned short cls;            /* class plus one, for subclass 0; 0 until known */
    unsigned short flags;          /* LOCKDEP_STATIC */
};

/* The map's class is keyed by the map's own address, as a lock in the
 * kernel's static data is, with this flag or without (`static_obj`,
 * kernel/locking/lockdep.c:999). A map outside it with neither this nor a
 * key was never initialised: it is reported once and keyed by its
 * address. */
#define LOCKDEP_STATIC 1u

/* A static lock's map, named `name` (a string constant, or 0). */
#define LOCKDEP_MAP_STATIC(name) {(name), 0, 0, LOCKDEP_STATIC}

/* The deepest nesting of locks one context holds. */
#define LOCKDEP_DEPTH 32

/* One lock a context holds. */
struct lockdep_held_lock {
    struct lockdep_map *map;
    uintptr_t ip;           /* where it was taken */
    unsigned short cls;     /* its class's index */
    unsigned char flags;    /* LOCKDEP_HELD_ */
    unsigned char subclass; /* the subclass it was taken at */
};

#define LOCKDEP_HELD_TRY   1u /* taken by a trylock, which adds no order */
#define LOCKDEP_HELD_IRQ   2u /* taken in an interrupt */
#define LOCKDEP_HELD_SLEEP 4u /* a sleeping lock */

/* The locks one context holds, outermost first. */
struct lockdep_held {
    unsigned int depth;
    struct lockdep_held_lock locks[LOCKDEP_DEPTH];
};

/* How `lockdep_acquire` takes the lock. */
#define LOCKDEP_SPIN     0 /* a spinlock, waited for */
#define LOCKDEP_TRY      1 /* a spinlock taken by a trylock that succeeded */
#define LOCKDEP_SLEEPING 2 /* a sleeping lock: a mutex, rwsem or sleep_lock */

#ifdef RACH2_LOCKDEP

/** Give `map` the class of the init site whose key is `key`, named `name`.
 * The lock is not held. */
void lockdep_init_map(struct lockdep_map *map, const char *name, const struct lockdep_key *key);

/**
 * Note that the running context takes the lock of `map` at `subclass`,
 * the way `how` says, from the instruction `ip`, and check the order it
 * makes with the locks the context holds. Called before the lock is
 * waited for, as Linux's lock_acquire, so that a deadlock is reported
 * before it happens; a trylock is noted after it succeeds. Interrupts may
 * be on or off.
 */
void lockdep_acquire(struct lockdep_map *map, unsigned int subclass, int how, uintptr_t ip);

/** Note that the running context lets the lock of `map` go, at `ip`. */
void lockdep_release(struct lockdep_map *map, uintptr_t ip);

/** Report, once for each site, a sleep at `ip` under a spinlock, with
 * interrupts off or in an interrupt (`might_sleep`). */
void lockdep_might_sleep(uintptr_t ip);

/** Report, once for each site, a switch at `ip` while this processor holds
 * a spinlock, which `schedule` checks with interrupts on or off: a switch
 * with them off is how every sleep ends, so only the spinlock is wrong. */
void lockdep_switch(uintptr_t ip);

/** Report, once for each site, the big lock taken at `ip` while this
 * processor holds a spinlock. A trylock of the big lock does not wait and
 * is not checked. */
void lockdep_bkl_acquire(uintptr_t ip);

/** Note that interrupts come on at `ip`: each spinlock this processor
 * holds is then held with interrupts on, which an interrupt that takes it
 * would deadlock on (`mark_held_locks`, lockdep.c:4344). */
void lockdep_irqs_on(uintptr_t ip);

/** 1 when the running context holds the lock of `map`, 0 when not, and 1
 * when the checker is off and cannot tell. */
int lockdep_is_held(const struct lockdep_map *map);

/** Report, once for each site, a caller that does not hold the lock of
 * `map` (`lockdep_assert_held`, include/linux/lockdep.h:285). */
void lockdep_assert_held_map(const struct lockdep_map *map);

/** Panic when this processor does not hold the big lock: code that runs
 * without it reached code that still needs it. */
void bkl_assert_held_here(void);

/** Write /proc/lockdep's text, the classes and their orders, into `buf` of
 * `cap` bytes, ended with a NUL; the length, without the NUL. */
int lockdep_proc_text(char *buf, int cap);

/** Write /proc/lockdep_stats's text into `buf` of `cap` bytes, as
 * `lockdep_proc_text` does. */
int lockdep_stats_text(char *buf, int cap);

/** Write debugfs's lockdep/inject: the names of its triggers, a line each. */
int32_t lockdep_inject_show(void *data, char *buf, uint32_t cap);

/**
 * Run the trigger that `buf` names, "abba" for an order that closes a cycle
 * or "irq" for an interrupt inversion, each on locks of its own, so that a
 * test sees the checker report both; -EINVAL for any other word.
 */
int32_t lockdep_inject_store(void *data, const char __user *buf, uint32_t len);

/** Report a missing hold of the spinlock (or any lock with a `dep` map) at
 * `lock`. */
#define lockdep_assert_held(lock) lockdep_assert_held_map(&(lock)->dep)

/** Panic when this processor does not hold the big lock. */
#define bkl_assert_held() bkl_assert_held_here()

#else /* !RACH2_LOCKDEP */

#define lockdep_init_map(map, name, key)        ((void)0)
#define lockdep_acquire(map, subclass, how, ip) ((void)0)
#define lockdep_release(map, ip)                ((void)0)
#define lockdep_might_sleep(ip)                 ((void)0)
#define lockdep_switch(ip)                      ((void)0)
#define lockdep_bkl_acquire(ip)                 ((void)0)
#define lockdep_irqs_on(ip)                     ((void)0)
#define lockdep_assert_held(lock)               ((void)(lock))
#define bkl_assert_held()                       ((void)0)

#endif /* RACH2_LOCKDEP */

#endif /* _LOCKDEP_H_ */
