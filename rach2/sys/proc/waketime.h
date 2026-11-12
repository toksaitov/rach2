/* Accounting from a timed wake's deadline to its return to user mode. */
#ifndef _WAKETIME_H_
#define _WAKETIME_H_

#include "../lib/kint.h"

/* Microseconds in disjoint stages; the two BKL fields are subsets. */
#define WAKE_IRQ        0
#define WAKE_TIMER_BKL  1
#define WAKE_TIMER      2
#define WAKE_QUEUE      3
#define WAKE_RETURN     4
#define WAKE_BLOCKED    5
#define WAKE_QUEUE_BKL  6
#define WAKE_RETURN_BKL 7
#define WAKE_FIELDS     8

struct wake_record {
    uint32_t phase;              /* 0: none, 1: queued, 2: running, 3: blocked, 4: firing */
    uint32_t since;              /* stage start, microseconds modulo 2^32 */
    uint32_t queue_bkl;          /* destination CPU's lock wait at queue entry */
    int cpu;                     /* queue whose lock wait is being counted */
    uint32_t part[WAKE_FIELDS];  /* the wake still on its way to user mode */
    uint32_t last[WAKE_FIELDS];  /* the most recent completed wake */
    uint32_t worst[WAKE_FIELDS]; /* the completed wake with greatest total */
    uint64_t sum[WAKE_FIELDS];   /* completed wakes, microseconds */
    uint32_t count;              /* completed wakes, modulo 2^32 */
};

struct process;

/** A cheap microsecond clock, modulo 2^32, from the calibrated TSC, or
 * the platform timer when no TSC was calibrated. Interrupts are off. */
uint32_t waketime_clock(void);

/** Start a timed wake of blocked user thread `p` at tick `due`.
 * A kernel thread has no user return and starts no record.
 * The caller holds the run queues' lock or the waiter's queue's lock, with
 * interrupts off. An unfinished wake keeps
 * its first deadline, including a later block before returning to user. */
void waketime_expire(struct process *p, uint64_t due);

/** Account a change to phase 1 (queued), 2 (running), or 3 (blocked again).
 * For a queued thread `cpu` is its destination. The big lock is held with
 * interrupts off; a thread with no timed wake costs no clock reading. */
void waketime_state(struct process *p, unsigned int phase, int cpu);

/** Finish the current thread's wake just before its return to user mode,
 * with the big lock held and interrupts off. */
void waketime_return(void);

/** Charge a completed big-lock acquisition to the running timed wake.
 * The caller has just taken the big lock with interrupts off. */
void waketime_lock(uint32_t waited);

#endif
