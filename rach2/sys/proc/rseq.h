/* rseq(2): restartable sequences, a thread's record of the processor it
 * runs on and the critical section that a preemption aborts. */
#ifndef _RSEQ_H_
#define _RSEQ_H_

#include "../irq.h"

struct process;

/* The bytes of struct rseq this kernel keeps, offsetof(struct rseq, end),
 * and the area's alignment, which a new program's auxiliary vector gives
 * as AT_RSEQ_FEATURE_SIZE and AT_RSEQ_ALIGN, as Linux 6.12's does. */
#define RSEQ_FEATURE_SIZE 28u
#define RSEQ_ALIGN        32u

/* The events that abort a critical section, Linux's rseq_event_mask bits:
 * the thread was switched away from its processor, took a signal, or moved. */
#define RSEQ_EVENT_PREEMPT 0x1u
#define RSEQ_EVENT_SIGNAL  0x2u
#define RSEQ_EVENT_MIGRATE 0x4u

/** rseq(rseq, rseq_len, flags, sig): register or unregister the
 * calling thread's area; 0 or -errno. */
long sys_rseq(isr_params_t *params);

/** Note that `p` is being switched away from its processor, for the
 * scheduler: with an area registered, the next return to user mode aborts
 * a critical section it is in and gives it its processor again. Called
 * with the scheduler's lock held; touches only `p`'s record. */
void rseq_preempt(struct process *p);

/** Mark a move of `p` to another processor, under the scheduler's lock.
 * The next return to user mode aborts its section and updates its area. */
void rseq_migrate(struct process *p);

/** The current thread's part of a return to user mode to the frame
 * `params`, after its signals (`isr_leave`): when an event or a
 * registration asked for it, abort the critical section it is in and
 * write its processor into its area. An area that cannot be read or
 * written, or names a section that is not valid, is SIGSEGV, delivered at
 * once, as Linux's force_sigsegv. Interrupts are off and the big lock
 * held. */
void rseq_return(isr_params_t *params);

/** The current thread's part of the delivery of `signo` to a handler, before
 * its frame records `params`: a critical section it is in is aborted, so
 * that the handler returns to the section's abort handler, and the area
 * gets the processor. A failure posts SIGSEGV, with SIGSEGV's handler
 * reset first when `signo` is SIGSEGV, as Linux's force_sigsegv. */
void rseq_signal(isr_params_t *params, int signo);

/** Give the child `child` the registration of `parent`, as Linux's
 * rseq_fork: none when the child shares the parent's memory (`shares_vm`),
 * a thread or a vfork's child, and otherwise the same area, which the
 * child's first return to user mode updates. */
void rseq_fork(struct process *child, const struct process *parent, int shares_vm);

/** Forget `p`'s registration, at an exec, which leaves its area behind. */
void rseq_exec(struct process *p);

#endif /* _RSEQ_H_ */
