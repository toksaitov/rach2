/*
 * The scheduler: each processor's run queue and its selection, the idle
 * wait, and the timer's accounting of busy and idle ticks and of the load.
 */
#ifndef _SCHED_H_
#define _SCHED_H_

#include "process.h"
#include "../arch/arch.h"

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

/** Refresh a ready entry after its priority changes, and ask the thread
 * that should give way to switch: the current thread of a queued one's
 * processor when the queued one now outranks it, or a running one when a
 * queued thread now outranks it. The caller holds the big lock, with
 * interrupts masked. */
void sched_priority_changed(process_t *p);

/** Initialize empty ready lists before any process is made ready, with the
 * scheduler off and the next selection allowed to prefer a woken process,
 * and mark the boot's current process, process 0, as on its processor. */
void init_sched(void);

/** Turn the scheduler on or off; while it is off, schedule() returns at
 * once, so nothing switches processes. */
void sched_enable(void);
void sched_disable(void);

/** Nonzero while the scheduler is on. */
int sched_is_enabled(void);

/**
 * Switch to the highest-priority eligible real-time thread, or, ahead of it
 * while the fair server's turn lasts and without one, the ordinary thread
 * EEVDF picks on this processor's fair queue (fair.h). With no candidate,
 * it continues the current process if that can run; otherwise it pulls an
 * ordinary thread from a busier processor or switches to its idle task.
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
 * Make `p`, new, blocked or stopped, ready to run: an ordinary thread on the
 * fair queue of the processor Linux's select_task_rq_fair gives it
 * (`fair_select_cpu`), a new one placed as wake_up_new_task places it, a
 * real-time one on the list of the processor it last ran on, or of one its
 * affinity has. A process still current on its processor, woken before its
 * switch away, stays there, and one whose dequeue EEVDF delayed is
 * requeued where it is. A real-time `p` that does not outrank the
 * real-time thread running there, or that thread may run nowhere else, goes
 * instead to the processor it may run on whose highest real-time priority,
 * of its current thread and its queue, is lowest, when it outranks every
 * thread there. Its processor is kicked so that it selects now rather than
 * at its tick when it is idle, halted in place with `p` current, or runs a
 * thread `p` outranks, or runs an ordinary thread that EEVDF would preempt
 * for ordinary `p` (`fair_wake_preempt`). The thread there has its
 * `need_resched` set, once, so that the interrupt switches it in user mode
 * and a call on that processor switches at its exit; a kernel thread is
 * sent no kick and
 * switches at its next sleep or service point. An idle target records its
 * reschedule request for the idle loop, which checks it before halting.
 * The state is not tested again: this is for a new thread, and for a
 * stopped one, which only the big lock's holder stops and makes ready. A
 * blocked thread, which wakers without the big lock may race for, is woken
 * by `sched_wake_blocked`, `sched_wake_signal` or `sched_wake_due`, which
 * test the state under the run queues' lock, so that one of them makes it
 * ready.
 */
void sched_make_ready(process_t *p);

/** Whether the current thread's yield would select it again, read without
 * the run queues' lock, so that sched_yield returns at once. Any context
 * with a current thread; the answer may be stale by the return. */
int sched_yield_alone(void);

/**
 * Make `p` ready as `sched_make_ready` does, and mark it woken, if it is
 * blocked; a thread in any other state is left as it is. This is a wait
 * queue's wake, and a child's exit's wake of its parent. The state is
 * tested under the run queues' lock, as Linux's `try_to_wake_up` tests it
 * under the task's `pi_lock` (kernel/sched/core.c:4191-4193), so that of two
 * wakers on two queues of one poller, or a queue's and a signal's or a
 * deadline's, one makes it ready. Returns the processor to interrupt, which
 * the caller passes to `smp_kick`, or to `sched_kick_cpus` in a set, once it
 * has let its own spinlocks go, or -1 when none needs it. Interrupts may be
 * on or off.
 */
int sched_wake_blocked(process_t *p);

/** Turn interrupts off and take the run queues' lock, for a caller that
 * must keep the records it wakes from being freed across several wakes, as
 * the timer pass does (`proc_wake_sleepers`): `free_process` takes the lock
 * before it frees. Returns the flags `sched_lock_give` restores. */
uint32_t sched_lock_take(void);

/** Let the run queues' lock go and restore the interrupt flag `flags`. */
void sched_lock_give(uint32_t flags);

/**
 * Make `p` ready, clearing its deadline, if it is blocked, as a signal or
 * `PTRACE_INTERRUPT` wakes it, or interrupt the processor it runs on, so
 * that its return to user mode takes the work the caller marked: Linux's
 * `signal_wake_up_state` wakes through `try_to_wake_up`'s test and kicks
 * a thread that was not asleep (`kick_process`, kernel/signal.c:759-773,
 * kernel/sched/core.c). Without the kick a thread running user code on
 * another processor took the signal only at its next entry to the kernel,
 * and a call it made then, such as a read of a pipe, ended with EINTR. The
 * test and the clearing are under the run queues' lock, as
 * `sched_wake_blocked`'s; a wait that goes on arms its own deadline again.
 * The thread is marked woken, as `sched_wake_blocked` marks it, so that the
 * current thread's slice gives way to it (`sched_tick`). The interrupt is
 * sent before this returns, so the caller holds no spinlock. Interrupts
 * may be on or off.
 */
void sched_wake_signal(process_t *p);

/**
 * Make `p` ready, recording its expiry (`waketime_expire`) and clearing its
 * deadline, if it is blocked with a deadline at or before the tick `now`,
 * as Linux's `hrtimer_wakeup` wakes a sleeper through `wake_up_process`
 * (kernel/time/hrtimer.c:1981-1992). The test is under the run queues' lock,
 * as `sched_wake_blocked`'s, and the thread is marked woken as there. The
 * interrupt is sent before this returns, so the caller holds no spinlock.
 */
void sched_wake_due(process_t *p, uint64_t now);

/** `sched_wake_due` for a caller that holds the run queues' lock
 * (`sched_lock_take`): the processor to interrupt, which the caller passes
 * to `sched_kick_cpus` once it has let the lock go, or -1. */
int sched_wake_due_locked(process_t *p, uint64_t now);

/** Make `p` ready, clearing its deadline, and mark it woken, if it is
 * blocked with a deadline, for a timed wait whose end has come by another
 * measure than its tick: its time of the monotonic clock, or a setting of
 * the real-time clock that its wait must count again from (`rt_steps`). The
 * caller holds the run queues' lock (`sched_lock_take`) and passes the
 * processor returned, or -1, to `sched_kick_cpus` once it has let it go. */
int sched_wake_time_locked(process_t *p);

/** `smp_kick` each processor whose bit is set in `cpus`: the interrupts a
 * waker's `sched_wake_blocked` calls asked for, sent after its locks, as
 * Linux's `wake_up_q` wakes after the futex bucket's lock
 * (kernel/futex/waitwake.c:198-199). */
void sched_kick_cpus(uint64_t cpus);

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
 * whatever its queues have to run, or a busier processor's fair queue
 * gives, and with nothing, wait for an interrupt with the big lock let go
 * (`idle_wait_irq_off`), which stops the tick until the next event.
 * Entered holding no big lock, which the idle task never holds but to drain
 * the loopback backlog.
 */
void sched_idle_loop(void);

/**
 * Mark this processor's idle task awake when an interrupt ends its halt:
 * from then until it switches to a thread or halts again it runs with
 * interrupts off, and a wake needs no interrupt to reach it, since its
 * selection comes next. A kick then turns `idle_poll` to CPU_POLL_KICKED
 * and sends none (`smp_kick`), as Linux's idle task keeps its polling bit
 * set outside the halt itself while it is current (kernel/sched/idle.c:257,
 * 282-290). The switch to a thread clears the mark under the run queues'
 * lock, and the next halt clears it first and, when a kick came, lets a
 * pending interrupt in and does not halt (`idle_halt`). From the interrupt
 * that ended the halt, with interrupts off; a thread halted in place is not
 * marked.
 */
void sched_idle_awake(void);

/**
 * The tail of a switch, run by the incoming thread on its own stack with
 * interrupts off: where `schedule` returns past `switch_context`, at a
 * forked child's first return (`ret_from_fork`, isr_handler.s) and at a
 * kernel thread's first entry (`setup_process_stack`). It clears the
 * outgoing thread's `on_cpu` and takes the big lock at the incoming
 * thread's own depth.
 */
void schedule_tail(void);

/** A full memory barrier: no load or store of this processor moves across
 * it, as Linux's smp_mb. A waker that stores its condition and then reads a
 * sleeper's state passes one between, as a sleeper does between its state
 * and its test (`sched_set_current_state`). */
static __inline__ void sched_mb(void)
{
    volatile unsigned int fence = 0;

    (void)arch_atomic_xchg(&fence, 0u);
}

/**
 * Set the current thread's state to `state`, PROC_BLOCKED or PROC_STOPPED
 * before a sleep, with a full barrier after the store, as Linux's
 * set_current_state (include/linux/sched.h:190-195). A sleep sets it, and
 * files itself on its queue, before it tests its condition: a waker that
 * changes the condition after the test finds the thread blocked and makes
 * it ready, and `schedule` then keeps it running.
 */
static __inline__ void sched_set_current_state(proc_state_t state)
{
    current_proc->state = state;
    sched_mb();
}

/** Wait until thread `p` is on no processor: the switch away from it has
 * saved its registers, so its stack may be freed or another processor may
 * run it. Returns at once for a thread that is not running. */
void sched_wait_off_cpu(const process_t *p);

/**
 * From an interrupt handler: switch this processor now, unless it runs its
 * idle task, which selects once the handler returns, a process inside a
 * system call or a fault, which is asked to switch at the call's exit or
 * the fault's end (`sched_take_preempt`), a kernel thread
 * (`no_tick_switch`), which is asked to switch at its next sleep or
 * service point, or a thread inside klog.
 */
void sched_preempt(void);

/** From the end of the periodic tick's handler alone, with interrupts off:
 * count the tick against the current thread's slice and ask for a switch
 * (`need_resched`), which `sched_irq_exit` takes, when the slice has run
 * out or a queued thread should run first (sched.c). */
void sched_tick(void);

/** From the end of a device's interrupt, or of the timer's interrupt
 * between ticks, with interrupts off: switch this processor as
 * `sched_preempt` does when a wake or a priority change asked it to
 * (`need_resched`). */
void sched_irq_exit(void);

/* Linux's RR_TIMESLICE, the default slice of a SCHED_RR thread. */
#define SCHED_RR_TIMESLICE_MS 100

/** /proc/sys/kernel/sched_rr_timeslice_ms: the value read, and a write,
 * of which one at or below 0 restores the default. */
int sched_rr_timeslice_ms(void);
void sched_set_rr_timeslice_ms(int ms);

/** A SCHED_RR thread's slice in nanoseconds, a whole number of ticks. */
uint64_t sched_rr_slice_ns(void);

/** The slice sched_rr_get_interval reports for `p`, in nanoseconds. Takes
 * the run queues' lock. */
uint64_t sched_rr_interval_ns(const process_t *p);

/** The slice sched_getattr reports for the ordinary thread `p`, in
 * nanoseconds: its entity's, as Linux's get_params reads se.slice, which
 * the base slice renews at each deadline and placement unless the thread
 * asked for its own; before the thread has an entity, the slice it asked
 * for or the base slice. Takes the run queues' lock. */
uint64_t sched_slice_ns(const process_t *p);

/** /sys/kernel/debug/sched/fair_server/cpuN/runtime and period: processor
 * `c`'s fair server's budget and period in nanoseconds, and a write of
 * both, -EINVAL unless the runtime is at most the period and the period is
 * from 100 us to 2^22 us, -EBUSY for a processor not online or when the
 * servers would take more than the real-time runtime's share of the online
 * processors. */
void sched_fair_server(int c, uint64_t *runtime, uint64_t *period);
int sched_set_fair_server(int c, uint64_t runtime, uint64_t period);

/** /proc/sys/kernel/sched_rt_period_us and sched_rt_runtime_us: the values
 * read, and a write of both, -EINVAL unless the period is at least 1 and
 * the runtime is -1 or between 0 and the period, -EBUSY when the runtime's
 * share of the online processors would be less than the fair servers'.
 * Nothing is throttled by them; the fair server keeps ordinary threads
 * their share. */
void sched_rt_bandwidth(int *period_us, int *runtime_us);
int sched_set_rt_bandwidth(int period_us, int runtime_us);

/** Arm the slice event at the next moment that changes what this processor
 * should run, the current ordinary thread's deadline or the fair server's
 * turn, or request selection once it has passed. Interrupts are off; no
 * scheduler lock is held. */
void sched_slice_rearm(void);

/** Take the switch an interrupt asked for (`need_resched`), if one did;
 * `schedule` clears the request at its selection. Called with interrupts
 * off, at a call's exit, a fault's end and from `sched_service_point`. */
void sched_take_preempt(void);

/** Inside a long system call, between two of its pieces, or in a kernel
 * thread, between two completed works, with nothing half done: take the
 * switch the tick asked for without waiting for the call's exit or the
 * thread's sleep. A process is still inside its call when it runs again. */
void sched_service_point(void);

/**
 * Give way inside a long walk, between two bounded batches of its work, as
 * Linux's cond_resched: the service point (`sched_service_point`), which
 * lets a processor that has claimed the big lock have it and takes the
 * switch the tick asked for, when interrupts are on outside an interrupt
 * handler; nothing otherwise, as Linux's __cond_resched tests
 * irqs_disabled (kernel/sched/core.c:7287-7292). Every spinlock is taken
 * with interrupts off (lib/spinlock.h), so interrupts on means no spinlock
 * is held. The caller holds no pointer that another thread could free or
 * change while the big lock is let go.
 */
void sched_give_way(void);

/** Remove `p` from its ready list before it is freed; the big lock is held. */
void sched_forget(process_t *p);

/** Make `p` this processor's current process without a switch, as the
 * debug shell's process 0 takes back the processor a program it ran ended
 * on, whichever that is: off any ready list, running, and placed on this
 * processor. */
void sched_adopt(process_t *p);

/** With `hold` set, add each queued thread's wait so far to its `run_delay`
 * and count no wait until called with `hold` clear, which starts each
 * queued thread's wait again: a suspend to idle's bounds, as Linux's frozen
 * tasks leave their run queues and their sched_info stops. */
void sched_hold_waits(int hold);

/** Give `child`, a thread fork or clone has just made from `parent` and
 * placed on `child->cpu`, its scheduling group and its fair entity, once
 * the fork can no longer fail and before the child is made ready. The big
 * lock is held; sched_lock is not. */
void sched_fork(process_t *child, process_t *parent);

/** Give the ordinary thread `p` the slice `ns` sched_setattr asked for, or
 * the base slice for 0 (`fair_set_slice`). */
void sched_set_slice(process_t *p, uint64_t ns);

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
 * having tested its condition. When the loopback backlog has datagrams, they
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

/**
 * Pause once while `owner`, the holder of a sleeping lock the running
 * thread waits for, is processor `cpu`'s current thread and runs there: the
 * processor is not halted in an idle wait and the hypervisor has not
 * preempted it (`arch_cpu_preempted`). With `owner` NULL, the lock is
 * passing to a waiter ahead, and only the caller's own state counts.
 * Returns 1 after the pause, and 0 at once, for the caller to sleep
 * instead, when the owner does not run so, when the caller needs a switch,
 * or when its big lock may hold up the owner or another processor
 * (`bkl_spin_ok`). `owner` is compared, never read, so an owner that has
 * since given the lock up and exited does no harm. Interrupts on.
 */
int sched_spin_on(const process_t *owner, int cpu);

/** The processor whose current thread is `owner` and that is not halted in
 * an idle wait, or -1 when none is: where a sleeping lock's holder runs,
 * for a waiter whose note of the holder's processor is stale. `owner` is
 * compared, never read. Interrupts on or off. */
int sched_running_cpu(const process_t *owner);

/** Nonzero when no interrupt switches the running thread off its processor,
 * since it is a kernel thread or inside a system call or a fault
 * (`sched_preempt`): the processor stays its own until it sleeps or reaches
 * a service point, as Linux's preempt_disable keeps it for a spin on a
 * sleeping lock's owner. */
int sched_spin_pinned(void);

/** Ticks that found a processor parked in an idle wait, and those that did
 * not, summed over the processors in 64 bits; each processor's own are in
 * its record (percpu.h). Counted rather than timed: each processor counts
 * every tick of its own clock since it started, a lost one with the next
 * (`sched_account_tick`). */
uint64_t sched_idle_ticks(void);
uint64_t sched_busy_ticks(void);

/** The switches from one thread to another on every processor since boot,
 * the idle task's among them, for /proc/stat's `ctxt`, as Linux sums each
 * run queue's `nr_switches` (`nr_context_switches`, kernel/sched/core.c).
 * Each processor counts its own; a 64-bit count is two loads on the i686,
 * which are read with no lock, as Linux reads them. */
uint64_t sched_switches(void);

/** Processor `c`'s ticks in user mode, busy, idle and stolen by the
 * hypervisor, which are neither busy nor idle, as one reading. Its own tick
 * changes them within its `ticks_seq`, which is odd meanwhile, so a reader
 * on any processor reads them again until the count is even and has not
 * moved: on the i686 a 64-bit count is two loads. */
void sched_cpu_ticks(int c, uint64_t *user, uint64_t *busy, uint64_t *idle, uint64_t *steal);

/** Count the ticks of this processor's clock it has not counted yet as idle,
 * busy or stolen, so that the three add up to the uptime since it started,
 * and charge each busy one to the current process as user time
 * when `user` is set and as system time when it is not, and to its
 * process's virtual, profiling and POSIX CPU-time timers, whose signals it
 * leaves for a holder of the big lock to post; with the cycle account on
 * (`sched_cycle_account`), a tick found in user mode that was due, at `due`
 * on the monotonic clock, before the processor's last return to user mode
 * is charged as system time, and `due` is 0 when the timer keeps no such
 * time. Called by each processor's timer tick alone, without the big lock
 * and with interrupts off. */
void sched_account_tick(int user, int64_t due);

/* Nonzero for the cycle account, `rach2.cycleaccount` on the command line:
 * each entry from user mode and each return to it reads the cycle count
 * (`entry_cycles`, `user_cycles`), so that a tick held while interrupts
 * were off is charged where it was due (`sched_account_tick`), and each
 * system call's time counts in bkl/calls (`smp_call_end`). Off, a tick is
 * charged to the mode it finds, as Linux's tick accounting does, and the
 * entry and return read no clock. Set once at boot. */
extern int sched_cycle_account;

/** Start this processor's counts at the clock's time now: an application
 * processor's, as it comes online, before its first tick. */
void sched_account_start(void);

/** Record that this processor, now online, shares its last-level cache
 * with the processors of `shared` and its core with those of `core`, its
 * SMT siblings, a bit each, and they with it, and scale the fair class's
 * base slice to the processors online. Called by each application
 * processor as it comes online; the boot processor learns its peers from
 * theirs. */
void sched_cache_join(uint64_t shared, uint64_t core);

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
