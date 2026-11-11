/*
 * The scheduler: each processor's run queue and its selection, the idle
 * wait, and the timer's accounting of busy and idle ticks and of the load.
 */
#ifndef _SCHED_H_
#define _SCHED_H_

#include "process.h"

/* The scheduling policies, Linux's numbers. FIFO and RR run by their
 * real-time priority, above every other thread; OTHER, BATCH and IDLE take
 * their turns among the ordinary threads, and so does DEADLINE, which has
 * no class of its own (syscall_proc.c). EXT is defined so that a call about
 * it answers as a Linux without sched_ext does. */
#define SCHED_OTHER    0
#define SCHED_FIFO     1
#define SCHED_RR       2
#define SCHED_BATCH    3
#define SCHED_IDLE     5
#define SCHED_DEADLINE 6
#define SCHED_EXT      7

/** Effective real-time priority, including a PI futex's donation. */
static __inline__ unsigned int sched_priority(const process_t *p)
{
    return p->pi_priority > p->rt_priority ? p->pi_priority : p->rt_priority;
}

/** Refresh a ready entry after its priority changes and request selection.
 * The caller holds the big lock, with interrupts masked. */
void sched_priority_changed(process_t *p);

/** Initialize empty ready lists before any process is made ready, with the
 * scheduler off and the next selection allowed to prefer a woken process. */
void init_sched(void);

/** Turn the scheduler on or off; while it is off, schedule() returns at
 * once, so nothing switches processes. */
void sched_enable(void);
void sched_disable(void);

/** Nonzero while the scheduler is on. */
int sched_is_enabled(void);

/**
 * Switch to the highest-priority eligible real-time thread, or the next
 * ordinary thread from this processor's ready-list head. Ordinary selection
 * prefers a wait-queue wake at most every second time. With no candidate, it
 * continues the current process if that can run; otherwise it tries other
 * queues by their ready counts, busiest first, and takes an eligible entry
 * or switches to its idle task.
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
 * outside the set moves to one inside it. A running process leaves before
 * this returns: the caller switches now, and a remote process is asked to
 * switch and waited for without signals ending the wait. A newer mask
 * permitting its current processor also ends the wait. The big lock is
 * held on entry and return; the call may sleep and the target may end
 * meanwhile, so its caller must not use `p` afterwards without a new lookup.
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

/** Remove `p` from its ready list before it is freed; the big lock is held. */
void sched_forget(process_t *p);

/** With `hold` set, add each queued thread's wait so far to its `run_delay`
 * and count no wait until called with `hold` clear, which starts each
 * queued thread's wait again: a suspend to idle's bounds, as Linux's frozen
 * tasks leave their run queues and their sched_info stops. */
void sched_hold_waits(int hold);

/** Read selection attempts (index 0) or ready entries examined (1), modulo
 * 2^32. Other indices return zero. The caller holds the big lock. */
uint32_t sched_work_count(unsigned int index);

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
 * check (tests/cases/host/style.sh) fails on a `sti; hlt`, or a call of
 * `arch_idle_masked`, anywhere else: a site added later left unmarked would
 * report its idle time as busy, and nothing would say so.
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

/** Nonzero when every processor online is halted in an idle wait, as each
 * marks itself for the tick (`idle_wait_irq_off`, `idle_wait_irq_on`). Read
 * by the boot processor's tick, with interrupts off. */
int sched_all_idle(void);

/** Ticks that found a processor parked in an idle wait, and those that did
 * not, summed over the processors in 64 bits; each processor's own are in
 * its record (percpu.h). Counted rather than timed: each processor counts
 * every global tick since it started, a lost one with the next
 * (`sched_account_tick`). */
uint64_t sched_idle_ticks(void);
uint64_t sched_busy_ticks(void);

/** Processor `c`'s ticks in user mode, busy and idle, as one reading. Its
 * own tick changes them within its `ticks_seq`, which is odd meanwhile, so
 * a reader on any processor reads them again until the count is even and
 * has not moved: on the i686 a 64-bit count is two loads. */
void sched_cpu_ticks(int c, uint64_t *user, uint64_t *busy, uint64_t *idle);

/** Count the global ticks this processor has not counted yet as idle or
 * busy, so that its idle and busy ticks add up to the uptime since it
 * started, and charge each busy one to the current process as user time
 * when `user` is set and as system time when it is not, and to its
 * process's virtual, profiling and POSIX CPU-time timers. Called by each
 * processor's timer tick alone. */
void sched_account_tick(int user);

/**
 * Whether a tick of this processor that found the big lock held must wait
 * for the lock: true at the `SCHED_TICK_PASSES`th such tick in a row with a
 * process on this processor's queue, which only the tick's switch would let
 * run. Called with interrupts off.
 */
int sched_tick_waits(void);

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
