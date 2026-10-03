#include "../arch/arch.h"
#include "sched.h"
#include "ptrace.h"
#include "suspend.h"
#include "tgroup.h"
#include "../io/timerfd.h"
#include "../lib/ksignal.h"

#include "../lib/kirq.h"
#include "../lib/spinlock.h"
#include "smp.h"
#include "../usb/usb.h"
#include "../network/ip/ip.h"

/** Nonzero while preemptive scheduling is enabled; schedule() is a no-op
 * while it is zero. */
static volatile int _sched_enabled = 0;

/*
 * The run queues. A ready process is on the queue of the processor its
 * `cpu` names, and the queues are the process list read through that
 * field, so that a process is on one queue by construction and the list
 * stays the one order the selection rotates through. The lock covers the
 * fields below and a process's `cpu` and READY state as a wake or a
 * selection changes them; it is taken after the big lock.
 */
static struct spinlock sched_lock = SPINLOCK_INIT;

/** Each processor's last ordinary selection; a wake preference does not
 * move it. */
static process_t *_last_sched[NR_CPUS];

/** Nonzero when a processor's next selection may prefer a woken process; a
 * preferred selection clears it, so the one after it is ordinary. */
static int _wake_turn[NR_CPUS];

/** Move every processor's round-robin cursor off `p`, which is about to be
 * unlinked, to the record before it. */
void sched_forget(process_t *p)
{
    uint32_t flags = spin_lock_irqsave(&sched_lock);
    int c;

    for (c = 0; c < NR_CPUS; c++) {
        if (_last_sched[c] == p) {
            _last_sched[c] = p->prev;
        }
    }
    spin_unlock_irqrestore(&sched_lock, flags);
}

void init_sched(void)
{
    int c;

    _sched_enabled = 0;
    for (c = 0; c < NR_CPUS; c++) {
        _last_sched[c] = NULL;
        _wake_turn[c] = 1;
    }
}

void sched_enable(void)
{
    _sched_enabled = 1;
}

void sched_disable(void)
{
    _sched_enabled = 0;
}

int sched_is_enabled(void)
{
    return _sched_enabled;
}

/** Whether `p` may run on processor `cpu`: its affinity has it, and it is
 * started. */
static int sched_allowed(const process_t *p, int cpu)
{
    return cpu_table[cpu].online && ((p->affinity >> cpu) & 1u) != 0;
}

/** Whether processor `cpu` runs its idle task: it has nothing else to do. */
static int sched_cpu_idle(int cpu)
{
    return cpu_table[cpu].current == cpu_table[cpu].idle;
}

/**
 * Whether `p` may be selected on processor `cpu` now: ready, of those a
 * suspend to idle lets run (`suspend_lets_run`), with `cpu` in its
 * affinity, and not another processor's current process, whose stack and
 * registers are still in use there until that processor switches away.
 */
static int sched_may_run(const process_t *p, int cpu)
{
    return p->state == PROC_READY && ((p->affinity >> cpu) & 1u) != 0 && suspend_lets_run(p) &&
           (p->cpu == cpu || cpu_table[p->cpu].current != p);
}

/**
 * Find a process of processor `cpu`'s queue that may run there, after its
 * ordinary cursor, wrapping once.
 *
 * With `woken_only`, consider only wait-queue wakeups. The run queues' lock
 * is held.
 */
static process_t *sched_find(int cpu, int woken_only)
{
    process_t *start;
    process_t *p;

    start = _last_sched[cpu] && _last_sched[cpu]->next ? _last_sched[cpu]->next : proc_list;
    p = start;
    if (!p) {
        return NULL;
    }
    do {
        if (p->cpu == cpu && (!woken_only || p->woken) && sched_may_run(p, cpu)) {
            return p;
        }
        p = p->next ? p->next : proc_list;
    } while (p != start);
    return NULL;
}

/**
 * For processor `cpu`, which has nothing of its own to run, the ready
 * process of the busiest other queue that may run on it, moved to its
 * queue; NULL when there is none. The busiest queue is the one with the
 * most ready processes. The run queues' lock is held.
 */
static process_t *sched_steal(int cpu)
{
    int ready[NR_CPUS];
    process_t *best = NULL;
    process_t *p;
    int c;

    for (c = 0; c < NR_CPUS; c++) {
        ready[c] = 0;
    }
    for (p = proc_list; p; p = p->next) {
        if (p->state == PROC_READY && p->cpu >= 0 && p->cpu < NR_CPUS) {
            ready[p->cpu]++;
        }
    }
    for (p = proc_list; p; p = p->next) {
        if (p->cpu != cpu && sched_may_run(p, cpu) && (!best || ready[p->cpu] > ready[best->cpu])) {
            best = p;
        }
    }
    if (best) {
        best->cpu = cpu;
    }
    return best;
}

/**
 * The processor whose queue a process made ready joins: the one it ran on
 * last, unless that one is busy and another it may run on is idle, where
 * it then goes. A process that is still a processor's current, woken
 * before its switch away, stays where it runs. The run queues' lock is
 * held.
 */
static int sched_place(const process_t *p)
{
    int last = p->cpu;
    int c;

    if (cpu_table[last].current == p) {
        return last;
    }
    if (sched_allowed(p, last) && sched_cpu_idle(last)) {
        return last;
    }
    for (c = 0; c < NR_CPUS; c++) {
        if (sched_allowed(p, c) && sched_cpu_idle(c)) {
            return c;
        }
    }
    if (sched_allowed(p, last)) {
        return last;
    }
    for (c = 0; c < NR_CPUS; c++) {
        if (sched_allowed(p, c)) {
            return c;
        }
    }
    return last;
}

/**
 * Make `p`, blocked or stopped, ready to run, on the queue `sched_place`
 * chooses, and wake that processor when it is idle, or halted in place
 * with `p` its current process, so that it looks now rather than at its
 * tick. Every wake of a process goes through here.
 */
void sched_make_ready(process_t *p)
{
    uint32_t flags = spin_lock_irqsave(&sched_lock);
    int target;
    int kick;

    p->state = PROC_READY;
    target = sched_place(p);
    p->cpu = target;
    kick = sched_cpu_idle(target) || cpu_table[target].current == p;
    spin_unlock_irqrestore(&sched_lock, flags);
    if (kick) {
        smp_kick(target);
    }
}

/**
 * Switch this processor to the next process of its queue, alternating a
 * wake preference with an ordinary round-robin selection.
 *
 * A preferred selection consumes its wake mark without moving the ordinary
 * cursor. At least every second selection advances that cursor, even when
 * sleepers wake repeatedly. A missing wake candidate takes an ordinary turn.
 * A processor whose own queue has nothing to run takes a process from the
 * busiest other queue (`sched_steal`), and with nothing there either it
 * goes on with its current process if that one can still run, or switches
 * to its idle task, which waits for work (`sched_idle_loop`). A process that
 * blocks or ends therefore always leaves the processor, and a woken one may
 * run on any processor it may use. Before the idle tasks exist, at boot, a
 * process with nothing to switch to stays, and its caller idles in place.
 *
 * Selection and publication run with interrupts disabled and the run
 * queues' lock held, so a wake cannot race the mark's consumption or a
 * timer re-enter partially updated state. A current process that has
 * already resumed after an idle wait loses its mark before yielding, so
 * that wake cannot prefer it again later.
 *
 * Returns without a switch when scheduling is disabled or there is no
 * current process. The switch runs with interrupts disabled: a timer IRQ
 * landing between publishing current_proc and the stack and CR3 swap inside
 * switch_context would see current_proc == next with next->in_syscall == 0,
 * pass `sched_preempt`'s test, and re-enter schedule() on the old stack with
 * the TLS descriptor already rewritten, corrupting next->regs.esp. So
 * schedule() saves the caller's IF, clears it across the switch, and
 * restores it afterward. Each process does this symmetrically and resumes
 * with its own IF; a process entered for the first time skips the restore:
 * a forked child takes its IF from iret, and a kernel thread's entry
 * enables interrupts itself.
 *
 * The big lock stays held across the switch, so no other processor sees
 * the outgoing process before its registers are saved; the depth this
 * processor had in it goes with the outgoing process, and the incoming one
 * gets its own back. The run queues' lock is let go before the switch,
 * since the incoming process does not return here to let it go.
 *
 * The floating-point registers are the one part of the processor
 * `switch_context` does not touch. They are saved into the outgoing record
 * and loaded from the incoming one here, inside the same masked window and
 * before the stack pointer changes; the kernel computes in none of them, so
 * nothing between the load and the switch can disturb the loaded state. An
 * idle task has none to keep, and the registers are left as they are
 * around it.
 */
void schedule(void)
{
    process_t *next = NULL;
    process_t *old;
    struct percpu *cpu;
    uint32_t saved_flags;
    int me;

    if (!_sched_enabled) {
        return;
    }

    if (!current_proc) {
        return;
    }

    saved_flags = spin_lock_irqsave(&sched_lock);
    me = (int)arch_cpu_id();
    cpu = &cpu_table[me];
    old = cpu->current;
    if (old->state != PROC_READY) {
        old->woken = 0;
    }
    if (_wake_turn[me]) {
        next = sched_find(me, 1);
    }
    if (next) {
        _wake_turn[me] = 0;
    } else {
        next = sched_find(me, 0);
        if (next) {
            _last_sched[me] = next;
            _wake_turn[me] = 1;
        } else if (old == cpu->idle || old->state != PROC_RUNNING) {
            next = sched_steal(me);
        }
    }

    if (!next) {
        if (old == cpu->idle || old->state == PROC_RUNNING || !cpu->idle) {
            spin_unlock_irqrestore(&sched_lock, saved_flags);
            return;
        }
        next = cpu->idle;
    }

    usb_stamp_dispatch(next->pid);
    next->woken = 0;
    if (next == old) {
        next->state = PROC_RUNNING;
        spin_unlock_irqrestore(&sched_lock, saved_flags);
        return;
    }

    if (old->state == PROC_RUNNING && old != cpu->idle) {
        old->state = PROC_READY;
    }

    next->state = PROC_RUNNING;
    next->cpu = me;

    if (old != cpu->idle) {
        fpu_save(old->fpu);
    }
    if (next != cpu->idle) {
        fpu_restore(next->fpu);
    }
    arch_debug_switch(old, next);
    old->bkl_depth = cpu->bkl_depth;
    cpu->bkl_depth = next->bkl_depth;
    cpu->current = next;
    spin_unlock(&sched_lock);

    switch_context(old, next);

    kirq_restore(saved_flags);
}

/**
 * The idle task's body, on every processor: switch to whatever its queue,
 * or the busiest other queue, has to run, and with nothing, wait for an
 * interrupt with the big lock let go (`idle_wait_irq_off`). A wake that
 * puts a process on this processor's queue sends it the interprocessor
 * interrupt that ends the wait (`sched_make_ready`). Entered with the big
 * lock held at depth 1, as a started processor takes it and as the first
 * switch to the boot processor's idle task gives it.
 */
void sched_idle_loop(void)
{
    for (;;) {
        arch_irq_disable();
        schedule();
        idle_wait_irq_off();
    }
}

/**
 * Preempt from an interrupt handler.
 *
 * A process interrupted in user mode, the idle process and a kernel thread
 * are switched at once. A process inside a system call is not: the call's
 * code runs with interrupts on while its own state is half changed, and
 * `sys_exit`, for one, publishes the zombie its parent may reap, kernel
 * stack and all, before it calls `schedule`. The request is kept on the
 * process, and `sched_take_preempt` takes it at the call's exit, so a
 * process that is always inside a call still gives way once per call.
 */
void sched_preempt(void)
{
    if (current_proc && current_proc->in_syscall) {
        current_proc->need_resched = 1;
        return;
    }
    schedule();
}

/**
 * Take the switch an interrupt asked for during the current system call.
 * Called with interrupts off, at the call's exit with the trapframe complete
 * and from `sched_service_point`; the request is cleared before the switch,
 * so a request that arrives during the switch is the next call's.
 */
void sched_take_preempt(void)
{
    if (current_proc && current_proc->need_resched) {
        current_proc->need_resched = 0;
        schedule();
    }
}

/**
 * Give way inside a system call that does not return for a while.
 *
 * A call that loops over a large buffer keeps the processor for the whole
 * loop, since the tick's switch is taken at the call's exit; a reader woken
 * by a keystroke then waits for the loop. Between two pieces of such a
 * loop, with nothing half done, the call takes the switch here as its exit
 * would: interrupts off, the request cleared before the switch, and the
 * process still inside its call.
 */
void sched_service_point(void)
{
    uint32_t flags = kirq_save();

    sched_take_preempt();
    kirq_restore(flags);
}

/*
 * Each processor's idle marker and two tick counters are in its record
 * (percpu.h). They are `volatile` because the timer handler reads the marker
 * from an interrupt that arrives in the middle of the halt it describes. The
 * counters are 32-bit, one aligned store, so a reader never sees half of
 * one; at 100 Hz they wrap after about 497 days, the bound `kernel_ticks`
 * already has. The totals are summed over every record, of which those of
 * processors never started are zero.
 */
uint32_t sched_idle_ticks(void)
{
    uint32_t sum = 0;
    int i;

    for (i = 0; i < NR_CPUS; i++) {
        sum += cpu_table[i].idle_ticks;
    }
    return sum;
}

uint32_t sched_busy_ticks(void)
{
    uint32_t sum = 0;
    int i;

    for (i = 0; i < NR_CPUS; i++) {
        sum += cpu_table[i].busy_ticks;
    }
    return sum;
}

/** Count a tick of `p`'s processor time against its process's timer `k`
 * when `counts`, and post `signo` when the timer runs out, arming it again
 * for its interval. */
static void cpu_timer_tick(process_t *p, int k, int counts, int signo)
{
    struct tgroup *tg = p->tg;

    if (counts && tg->cpu_left[k] != 0 && --tg->cpu_left[k] == 0) {
        tg->cpu_left[k] = tg->cpu_interval[k];
        signal_post(p, signo);
    }
}

/**
 * Count one delivered tick as idle or busy, and charge a busy one to the
 * process it interrupted, to its process's virtual and profiling timers,
 * and to the POSIX timers that count its processor time.
 *
 * The process charged is `current_proc`, whose code, system call or
 * interrupt handler the tick interrupted, and `user` says whether the
 * interrupted code was ring 3's; Linux charges an interrupt handler's time to
 * the process it interrupted too. A tick that finds the processor in an idle
 * wait is charged to no one, although the process whose call is waiting is
 * still current.
 */
void sched_account_tick(int user)
{
    struct percpu *cpu = this_cpu();

    if (cpu->idle_wait) {
        ++cpu->idle_ticks;
        return;
    }
    ++cpu->busy_ticks;
    if (user) {
        ++cpu->user_ticks;
    }
    if (current_proc) {
        if (user) {
            ++current_proc->utime;
        } else {
            ++current_proc->stime;
        }
        if (current_proc->tg) {
            cpu_timer_tick(current_proc, 0, user, SIGVTALRM);
            cpu_timer_tick(current_proc, 1, 1, SIGPROF);
            ptimer_cpu_tick(current_proc, user);
        }
    }
}

/* The load averages, Linux's: in fixed point, sampled every LOAD_FREQ
 * ticks, five seconds and one, and decayed by e^(-5/60), e^(-5/300) and
 * e^(-5/900) at each sample. */
#define LOAD_ONE  (1u << SCHED_LOAD_SHIFT)
#define LOAD_FREQ (5u * 100u + 1u)
static const uint32_t load_decay[3] = {1884u, 2014u, 2037u};
static uint32_t load_avg[3];
static uint32_t load_wait = LOAD_FREQ;

void sched_task_counts(uint32_t *active, uint32_t *total)
{
    const process_t *p;

    *active = 0;
    *total = 0;
    for (p = proc_list; p; p = p->next) {
        if (p == proc_idle || p->state == PROC_UNUSED) {
            continue;
        }
        ++*total;
        if (p->state == PROC_RUNNING || p->state == PROC_READY) {
            ++*active;
        }
    }
}

/**
 * Fold the processes running or ready to run into the three averages, once
 * every LOAD_FREQ ticks, as Linux's `calc_load` does: the new average is the
 * old one decayed, plus the count times what the decay took off, rounded up
 * while the count is above it. Linux counts the uninterruptible sleepers
 * as well, which this kernel does not tell apart from the others. The
 * products are taken in 64 bits, since 4096 processes in fixed point times
 * a decay pass 32.
 */
void sched_load_tick(void)
{
    uint32_t active, total;
    int i;

    if (--load_wait != 0) {
        return;
    }
    load_wait = LOAD_FREQ;
    sched_task_counts(&active, &total);
    active *= LOAD_ONE;
    for (i = 0; i < 3; i++) {
        uint64_t next =
            (uint64_t)load_avg[i] * load_decay[i] + (uint64_t)active * (LOAD_ONE - load_decay[i]);

        if (active >= load_avg[i]) {
            next += LOAD_ONE - 1u;
        }
        load_avg[i] = (uint32_t)(next >> SCHED_LOAD_SHIFT);
    }
}

void sched_loadavg(uint32_t avg[3])
{
    uint32_t flags = kirq_save();

    avg[0] = load_avg[0];
    avg[1] = load_avg[1];
    avg[2] = load_avg[2];
    kirq_restore(flags);
}

/**
 * Park the processor until an interrupt, and return with interrupts disabled.
 *
 * Every caller of this one enters with IF already clear, having checked its
 * condition there, and that is what makes the wait free of a lost wakeup:
 * `sti; hlt` issued from IF=0 is atomic against a wake that became pending
 * during the check, because the interrupt fires the instant after `hlt` and
 * never before it. The marker is set while interrupts are still off, so no
 * tick can be delivered between setting it and the halt it describes.
 *
 * Anything on the loopback ring is delivered before the processor is parked,
 * and this returns without halting so the caller checks its condition again.
 * Those datagrams are this machine's own, a delivery can be what makes a
 * process runnable, and nothing else is going to run to deliver them. A
 * system call's exit delivers only once the ring has three or more, since
 * one or two may be the crossing SYNs of a simultaneous open; a single
 * datagram, which is what a window update or a bare acknowledgement is, would
 * otherwise wait for the timer, and a local connection would turn over one
 * window every ten milliseconds. Interrupts are off here, which is the
 * discipline the drain needs, and a caller that is about to halt holds nothing
 * and is between operations, which is the state the timer's drain relies on
 * too.
 *
 * The big lock is let go for the halt and taken again after it, so that the
 * other processors run kernel code meanwhile. A wake from another processor
 * that comes between the two is not lost either: it sends this processor an
 * interrupt (`sched_make_ready`), which is pending until the `sti` and ends
 * the halt.
 */
void idle_wait_irq_off(void)
{
    int depth;

    if (ip_loop_pending()) {
        ip_loop_drain_from(IP_LOOP_FROM_IDLE);
        return;
    }
    this_cpu()->idle_wait = 1;
    depth = bkl_drop();
    arch_idle_masked();
    bkl_take(depth);
    this_cpu()->idle_wait = 0;
}

/**
 * Park the processor until an interrupt, and return with interrupts enabled.
 *
 * The callers of this one are already running with IF set, so an interrupt can
 * arrive at any instruction and the `sti` changes nothing; a wake that lands
 * between the marker and the `hlt` costs one more tick of waiting, which is why
 * every such caller re-checks its condition in a loop rather than assuming the
 * halt ended for its own reason. The tick the marker catches in that window
 * is counted idle, which is what the processor is about to be; the error is
 * at most one tick per wait.
 *
 * The big lock is let go for the halt as in `idle_wait_irq_off`, with
 * interrupts off while it is let go and taken, since an interrupt handler
 * in between would take it at a depth that is not yet the processor's.
 */
void idle_wait_irq_on(void)
{
    uint32_t flags = kirq_save();
    int depth;

    this_cpu()->idle_wait = 1;
    depth = bkl_drop();
    arch_idle();
    arch_irq_disable();
    bkl_take(depth);
    this_cpu()->idle_wait = 0;
    kirq_restore(flags);
}
