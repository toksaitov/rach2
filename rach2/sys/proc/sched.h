#ifndef _SCHED_H_
#define _SCHED_H_

#include "process.h"

/*
 * Initialize the scheduler
 */
void init_sched(void);

/*
 * Enable/disable preemptive scheduling
 * When disabled, schedule() does nothing
 */
void sched_enable(void);
void sched_disable(void);

/*
 * Check if scheduler is enabled
 */
int sched_is_enabled(void);

/*
 * Round-robin scheduler
 * Called from timer interrupt to switch to next ready process
 * Does nothing if scheduler is disabled or only one runnable process
 */
void schedule(void);

/*
 * Yield the CPU to the next ready process
 * Can be called voluntarily by a process
 */
void sched_yield(void);

#endif
