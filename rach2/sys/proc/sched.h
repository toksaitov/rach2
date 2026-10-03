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

/** Switch this processor to a READY process of its queue, alternating wake
 * preference and round-robin, or to its idle task; see sched.c for the
 * contract. */
void schedule(void);

/** Make a blocked or stopped process ready, on a processor's queue, and wake
 * that processor if it is idle. */
void sched_make_ready(process_t *p);

/** The idle task's body, which does not return. */
void sched_idle_loop(void);

/** From an interrupt handler: switch now, or at the current system call's
 * exit when the process is inside one; see sched.c. */
void sched_preempt(void);

/** Take the switch an interrupt asked for during the current system call,
 * if one did. Called with interrupts off, at the call's exit and from
 * `sched_service_point`. */
void sched_take_preempt(void);

/** Inside a long system call, between two of its pieces, take the switch
 * the tick asked for without waiting for the call's exit. */
void sched_service_point(void);

/** Move the scheduler's cursor off `p`, which is about to be unlinked and
 * freed. */
void sched_forget(process_t *p);

/*
 * The idle wait, and the two counters it feeds.
 *
 * Each processor's idle task waits here when nothing is ready, and so does a
 * process whose wait polls in place, or that blocked before the idle tasks
 * existed and had nothing to switch to. A tick cannot be classified by
 * looking at `current_proc`, which in the second case names a process that is
 * not running. What says so is a marker set around the halt and sampled by
 * the timer before any wakeup can change state.
 *
 * Every idle wait of a user process goes through one of these two, and the
 * style check refuses a `sti; hlt` written anywhere else: a site added later
 * left unmarked would report its idle time as busy, and nothing would say so.
 * They differ only in the interrupt flag they leave behind, which the callers
 * do not agree on: a wait that must re-check a condition with interrupts off
 * needs the `cli`, and one that returns to ordinary work does not.
 */
void idle_wait_irq_off(void); /* sti; hlt; cli */
void idle_wait_irq_on(void);  /* sti; hlt */

/** Ticks delivered while a processor was parked in an idle wait, and while
 * it was not, summed over the processors; each processor's own are in its
 * record (percpu.h). Counted rather than timed:
 * interrupts are off in several paths and a tick can be lost, so these are
 * what the timer actually delivered. */
uint32_t sched_idle_ticks(void);
uint32_t sched_busy_ticks(void);

/** Count one delivered tick as idle or busy, and charge a busy one to the
 * current process as user time when `user` is set and as system time when it
 * is not. Called by the timer alone. */
void sched_account_tick(int user);

/** Fold the active processes into the load averages every five seconds and
 * a tick (`sched_loadavg`). Called by the timer alone. */
void sched_load_tick(void);

/** The load averages over one, five and fifteen minutes into `avg`, in
 * fixed point with SCHED_LOAD_SHIFT bits of fraction, as Linux keeps them. */
#define SCHED_LOAD_SHIFT 11
void sched_loadavg(uint32_t avg[3]);

/** The processes running or ready to run, the idle process aside, and all
 * the processes, threads counted, into `*active` and `*total`. */
void sched_task_counts(uint32_t *active, uint32_t *total);

#endif
