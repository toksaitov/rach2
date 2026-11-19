/* sched_debug.h: the scheduler's files in /sys/kernel/debug/sched, Linux
 * 6.12's kernel/sched/debug.c. */
#ifndef _SCHED_DEBUG_H_
#define _SCHED_DEBUG_H_

/** Register /sys/kernel/debug/sched and its files, once, after the
 * processors have started; a file that cannot be registered is logged. */
void sched_debugfs_init(void);

#endif
