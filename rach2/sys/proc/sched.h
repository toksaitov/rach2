#ifndef _SCHED_H_
#define _SCHED_H_

#include "process.h"

/** Reset the scheduler to its initial disabled state. */
void init_sched(void);

/** Enable or disable preemptive scheduling; while disabled schedule() is a
 * no-op. */
void sched_enable(void);
void sched_disable(void);

/** Return nonzero while preemptive scheduling is enabled. */
int sched_is_enabled(void);

/** Round-robin: switch to the next READY process; see sched.c for the
 * contract. */
void schedule(void);

/** Voluntarily give up the CPU to the next READY process. */
void sched_yield(void);

#endif
