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

/** From an interrupt handler: switch now, or at the current system call's
 * exit when the process is inside one; see sched.c. */
void sched_preempt(void);

/** At a system call's exit, interrupts off: the switch an interrupt asked
 * for during the call, if one did. */
void sched_take_preempt(void);

/** A record is about to go: the scheduler must not start its next search
 * from it. */
void sched_forget(process_t *p);

/*
 * The idle wait, and the two counters it feeds.
 *
 * There is no idle task here: `schedule()` returns with the blocked process
 * still current when nothing else is ready, and the caller parks the processor
 * with `sti; hlt` inside its own syscall. So a tick cannot be classified by
 * looking at `current_proc`, which names a process that is not running. What
 * says so is a marker set around the halt and sampled by the timer before any
 * wakeup can change state.
 *
 * Every halt in the kernel goes through one of these two, and the style
 * check refuses a bare `sti; hlt` written anywhere else: a site added later
 * left unmarked would report its idle time as busy, and nothing would say so.
 * They differ only in the interrupt flag they leave behind, which the callers
 * do not agree on: a wait that must re-check a condition with interrupts off
 * needs the `cli`, and one that returns to ordinary work does not.
 */
void idle_wait_irq_off(void); /* sti; hlt; cli */
void idle_wait_irq_on(void);  /* sti; hlt */

/** Ticks delivered while the processor was parked in an idle wait, and while
 * it was not. Counted rather than timed: interrupts are off in several paths
 * and a tick can be lost, so these are what the timer actually delivered. */
uint32_t sched_idle_ticks(void);
uint32_t sched_busy_ticks(void);

/** Whether the processor is parked in an idle wait right now; the timer asks
 * this at entry, before a wakeup can change any process's state. */
int sched_in_idle_wait(void);

/** Count one delivered tick as idle or busy. Called by the timer alone. */
void sched_account_tick(void);

#endif
