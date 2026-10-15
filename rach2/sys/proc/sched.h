/*
 * The scheduler: each processor's run queue and its selection, the idle
 * wait, and the timer's accounting of busy and idle ticks and of the load.
 */
#ifndef _SCHED_H_
#define _SCHED_H_

#include "process.h"

/** Turn the scheduler off, with every processor's round-robin cursor at the
 * start of the list and its next selection one that may prefer a woken
 * process. */
void init_sched(void);

/** Turn the scheduler on or off; while it is off, schedule() returns at
 * once, so nothing switches processes. */
void sched_enable(void);
void sched_disable(void);

/** Nonzero while the scheduler is on. */
int sched_is_enabled(void);

/**
 * Switch this processor to the next process of its run queue that may run,
 * after its round-robin cursor; at most every second selection, a process a
 * wait queue woke is preferred (`wq_wake`). With nothing on its queue, the
 * processor takes a ready process from the busiest other queue, and with
 * nothing there either it goes on with the current process if that can
 * still run, or switches to its idle task.
 *
 * The current process leaves when it is not RUNNING; when its affinity no
 * longer has this processor, made ready on a processor it has; and when a
 * suspend to idle holds it, made ready where it is. Returns at once while
 * the scheduler is off or there is no current process, and, before the
 * idle tasks exist, when nothing else may run. After a switch the call
 * returns in the calling process once a later switch comes back to it,
 * with its interrupt flag as it was. The caller holds the big lock, which
 * stays held across the switch.
 */
void schedule(void);

/**
 * Make `p`, blocked or stopped, ready to run, on the queue of the processor
 * it last ran on, unless that one is busy and another it may run on is
 * idle, or its affinity no longer has it; a process still current on its
 * processor, woken before its switch away, stays there. That processor is
 * sent an interrupt when it is idle, or halted in place with `p` current,
 * so that it selects now rather than at its tick. Every wake of a process
 * goes through here, an interrupt handler's among them.
 */
void sched_make_ready(process_t *p);

/**
 * Bind `p` to the processors of `mask`, of which one is started at least,
 * as sched_setaffinity(2) asks. A ready process on the queue of a processor
 * outside the set moves to one inside it; a process running on one outside
 * it leaves at that processor's next selection (`schedule`), which for the
 * caller is now.
 */
void sched_set_affinity(process_t *p, uint64_t mask);

/**
 * The body of each processor's idle task, which does not return: switch to
 * whatever its queue, or the busiest other queue, has to run, and with
 * nothing, wait for an interrupt with the big lock let go
 * (`idle_wait_irq_off`). Entered with the big lock held at depth 1.
 */
void sched_idle_loop(void);

/**
 * From an interrupt handler: switch this processor now, unless it runs its
 * idle task, which selects once the handler returns, or a process inside a
 * system call, which is asked to switch at the call's exit
 * (`sched_take_preempt`).
 */
void sched_preempt(void);

/** Take the switch an interrupt asked for during the current system call,
 * if one did, clearing the request before the switch. Called with
 * interrupts off, at the call's exit and from `sched_service_point`. */
void sched_take_preempt(void);

/** Inside a long system call, between two of its pieces, with nothing half
 * done, take the switch the tick asked for without waiting for the call's
 * exit. The process is still inside its call when it runs again. */
void sched_service_point(void);

/** Move every processor's round-robin cursor off `p`, which is about to be
 * unlinked and freed, to the record before it. */
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
 * Every idle wait in the kernel goes through one of these two, and the style
 * check (tools/verify/check.sh) fails on a `sti; hlt`, or a call of
 * `arch_idle` or `arch_idle_masked`, anywhere else: a site added later left
 * unmarked would report its idle time as busy, and nothing would say so.
 * They differ only in the interrupt flag they leave behind, which the callers
 * do not agree on: a wait that must re-check a condition with interrupts off
 * needs the `cli`, and one that returns to ordinary work does not.
 */

/** Wait for an interrupt, with the big lock let go, and return with
 * interrupts off, as `sti; hlt; cli` does. The caller enters with them off,
 * having tested its condition. When the loopback ring has datagrams, they
 * are delivered instead and the call returns without the wait. */
void idle_wait_irq_off(void);

/** Wait for an interrupt, with the big lock let go, and return with the
 * interrupt flag as it was, which is set for every caller, as `sti; hlt`
 * leaves it. The caller tests its condition again in a loop, since a wake
 * that comes between its test and the halt ends no wait. */
void idle_wait_irq_on(void);

/** Ticks that found a processor parked in an idle wait, and those that did
 * not, summed over the processors in 64 bits; each processor's own are in
 * its record (percpu.h). Counted rather than timed: each processor counts
 * every global tick since it started, a lost one with the next
 * (`sched_account_tick`). */
uint64_t sched_idle_ticks(void);
uint64_t sched_busy_ticks(void);

/** Count the global ticks this processor has not counted yet as idle or
 * busy, so that its idle and busy ticks add up to the uptime since it
 * started, and charge each busy one to the current process as user time
 * when `user` is set and as system time when it is not, and to its
 * process's virtual, profiling and POSIX CPU-time timers. Called by each
 * processor's timer tick alone. */
void sched_account_tick(int user);

/** Fold the processes running or ready to run into the load averages every
 * five seconds and a tick (`sched_loadavg`). Called by the boot
 * processor's timer tick alone. */
void sched_load_tick(void);

/** The load averages over one, five and fifteen minutes into `avg`, in
 * fixed point with SCHED_LOAD_SHIFT bits of fraction, as Linux keeps them. */
#define SCHED_LOAD_SHIFT 11
void sched_loadavg(uint32_t avg[3]);

/** The processes running or ready to run, the idle process aside, and all
 * the processes, threads counted, into `*active` and `*total`. */
void sched_task_counts(uint32_t *active, uint32_t *total);

#endif
