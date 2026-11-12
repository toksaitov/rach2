/*
 * sched.c: the run queues and the selection, the idle wait, and the
 * accounting of the timer's ticks, as sched.h describes them.
 */
#include "../arch/arch.h"
#include "sched.h"
#include "ptrace.h"
#include "membarrier.h"
#include "rseq.h"
#include "suspend.h"
#include "tgroup.h"
#include "wait_queue.h"
#include "../io/timerfd.h"
#include "../timer.h"
#include "../watchdog.h"
#include "../klog.h"
#include "../rtc.h"
#include "../lib/ksignal.h"

#include "../lib/kirq.h"
#include "../lib/spinlock.h"
#include "../mm/pressure.h"
#include "smp.h"
#include "../usb/usb.h"
#include "../network/ip/ip.h"

/* Nonzero while the scheduler is on (`sched_enable`). */
static volatile int _sched_enabled = 0;

/* The big lock precedes this lock; interrupts cannot see partial links. */
static struct spinlock sched_lock = SPINLOCK_INIT_NAMED("sched_lock");

struct ready_queue {
    process_t *head, *tail;
    unsigned int count;    /* ready records, excluding the current and idle tasks */
    unsigned int rt_count; /* queued threads with an effective RT priority */
};
static struct ready_queue run_queues[NR_CPUS];
static uint32_t pick_count, pick_steps; /* selection attempts and entries examined, modulo 2^32 */

/** Nonzero when the next selection may prefer a wait-queue wake. */
static int _wake_turn[NR_CPUS];

/* Each remote affinity caller keeps its wait on its stack until completed.
 * The list is under sched_lock; a wait is completed after it leaves the
 * list, under its queue's lock (`wq_complete`), and its waiter may return
 * and its stack go the moment that lock is let go. */
struct affinity_wait {
    struct affinity_wait *next;
    process_t *task; /* No reference remains after the wait completes. */
    wait_queue_t queue;
    volatile int done;
};
static struct affinity_wait *affinity_waits;

/** Complete every pending move of `p`, after its switch away or when a
 * later affinity allows its processor. Interrupts are off and sched_lock
 * free: the waits are taken off the list under it, and completed after,
 * since a wake takes its queue's lock, which comes before it. A wait
 * for `p` is linked only while `p` runs, under sched_lock, so a caller that
 * comes after `p`'s switch away sees it without the lock. */
static void affinity_complete(process_t *p)
{
    struct affinity_wait **link, *done = NULL, *wait;

    if (!affinity_waits) {
        return;
    }
    spin_lock(&sched_lock);
    link = &affinity_waits;
    while (*link) {
        wait = *link;
        if (wait->task != p) {
            link = &wait->next;
            continue;
        }
        *link = wait->next;
        wait->task = NULL;
        wait->next = done;
        done = wait;
    }
    spin_unlock(&sched_lock);
    while (done) {
        wait = done;
        done = wait->next;
        wq_complete(&wait->queue, &wait->done);
    }
}

/** Whether a remote affinity wait has completed; never dereference its task. */
static int affinity_done(void *arg)
{
    return ((struct affinity_wait *)arg)->done;
}

/** Place `p` on `cpu`, marking its rseq area for the return to user mode.
 * The scheduler's lock is held. */
static void sched_move(process_t *p, int cpu)
{
    if (p->cpu != cpu) {
        rseq_migrate(p);
        p->cpu = cpu;
    }
}

/** Unlink a queued process in constant work, with sched_lock held. */
static void sched_remove(process_t *p)
{
    struct ready_queue *q;

    if (!p->ready_linked) {
        return;
    }
    q = &run_queues[p->cpu];
    if (p->ready_prev) {
        p->ready_prev->ready_next = p->ready_next;
    } else {
        q->head = p->ready_next;
    }
    if (p->ready_next) {
        p->ready_next->ready_prev = p->ready_prev;
    } else {
        q->tail = p->ready_prev;
    }
    q->count--;
    q->rt_count -= p->ready_rt;
    p->ready_rt = 0;
    p->ready_prev = p->ready_next = NULL;
    p->ready_linked = 0;
}

/** The monotonic clock in nanoseconds, 64 bits wide so that no wait on a
 * run queue wraps, as Linux's sched_info reads rq_clock. */
static uint64_t sched_clock_ns(void)
{
    int64_t sec, ns;

    rtc_monotonic(&sec, &ns);
    return (uint64_t)sec * 1000000000u + (uint64_t)ns;
}

/* Set while a suspend to idle holds every thread but the suspender: no wait
 * on a run queue is counted, as Linux's frozen tasks leave their queues. */
static int waits_held;

/** Append a noncurrent ready process once, with sched_lock held. A move
 * from one queue to another keeps the moment its wait began; a wait begun
 * before the scheduler is on, which has no clock to read, or while
 * `waits_held` is set, is not counted. */
static void sched_append(process_t *p)
{
    struct ready_queue *q = &run_queues[p->cpu];

    if (p->ready_linked || p == cpu_table[p->cpu].idle) {
        return;
    }
    if (!p->run_queued && _sched_enabled && !waits_held) {
        p->run_queued = 1;
        p->queued_ns = sched_clock_ns();
    }
    waketime_state(p, 1, p->cpu);
    p->ready_prev = q->tail;
    p->ready_next = NULL;
    if (q->tail) {
        q->tail->ready_next = p;
    } else {
        q->head = p;
    }
    q->tail = p;
    q->count++;
    p->ready_rt = sched_priority(p) != 0;
    q->rt_count += p->ready_rt;
    p->ready_linked = 1;
}

/** Count the wait of `p`, which now runs, on the run queues since it was
 * made ready, as Linux's sched_info_arrive adds it to run_delay, with
 * sched_lock held. */
static void sched_arrive(process_t *p)
{
    if (p->run_queued) {
        p->run_queued = 0;
        p->run_delay += sched_clock_ns() - p->queued_ns;
    }
}

void sched_hold_waits(int hold)
{
    uint32_t flags = spin_lock_irqsave(&sched_lock);
    uint64_t now = sched_clock_ns();
    process_t *p;
    int c;

    waits_held = hold;
    for (c = 0; c < NR_CPUS; c++) {
        for (p = run_queues[c].head; p; p = p->ready_next) {
            if (hold && p->run_queued) {
                p->run_queued = 0;
                p->run_delay += now - p->queued_ns;
            } else if (!hold && !p->run_queued && _sched_enabled) {
                p->run_queued = 1;
                p->queued_ns = now;
            }
        }
    }
    spin_unlock_irqrestore(&sched_lock, flags);
}

void sched_forget(process_t *p)
{
    uint32_t flags = spin_lock_irqsave(&sched_lock);

    sched_remove(p);
    spin_unlock(&sched_lock);
    affinity_complete(p);
    kirq_restore(flags);
}

/**
 * The record must name the processor it is current on: a switch away
 * appends it to that processor's queue (`sched_append`), and a wake before
 * the switch keeps it there (`sched_place`); and it runs here, so another
 * processor's switch to it waits for its own switch away (`on_cpu`). Linux's
 * init_idle sets an idle task's processor in the same hold of the run
 * queue's lock that makes it the queue's current
 * (kernel/sched/core.c:7782-7786).
 */
void sched_adopt(process_t *p)
{
    uint32_t flags = spin_lock_irqsave(&sched_lock);
    int me = (int)arch_cpu_id();

    sched_remove(p);
    sched_move(p, me);
    p->state = PROC_RUNNING;
    p->on_cpu = 1;
    cpu_table[me].current = p;
    spin_unlock_irqrestore(&sched_lock, flags);
}

static int sched_may_run(const process_t *p, int cpu);

void sched_priority_changed(process_t *p)
{
    uint32_t flags = spin_lock_irqsave(&sched_lock);

    if (p->ready_linked) {
        struct ready_queue *q = &run_queues[p->cpu];

        q->rt_count -= p->ready_rt;
        p->ready_rt = sched_priority(p) != 0;
        q->rt_count += p->ready_rt;
    }
    p->need_resched = 1;
    spin_unlock_irqrestore(&sched_lock, flags);
    smp_kick(p->cpu);
}

/** Find the highest-priority eligible RT thread; equal priorities keep
 * queue order. Ordinary queues need no scan. The run queue lock is held. */
static process_t *sched_find_rt(int cpu)
{
    process_t *best = NULL, *p;
    int c;

    for (c = 0; c < NR_CPUS; c++) {
        if (run_queues[c].rt_count == 0) {
            continue;
        }
        for (p = run_queues[c].head; p; p = p->ready_next) {
            pick_steps++;
            if (sched_may_run(p, cpu) && sched_priority(p) != 0 &&
                (!best || sched_priority(p) > sched_priority(best))) {
                best = p;
            }
        }
    }
    return best;
}

/* The ticks in a row a processor passes without the big lock while a process
 * waits on its queue, in ms: a slice's end may wait this long for the lock. */
#define SCHED_TICK_PASSES 4u

uint32_t sched_work_count(unsigned int index)
{
    return index == 0 ? pick_count : index == 1 ? pick_steps : 0;
}

void init_sched(void)
{
    int c;

    if (current_proc) {
        current_proc->on_cpu = 1;
    }
    _sched_enabled = 0;
    waits_held = 0;
    pick_count = pick_steps = 0;
    for (c = 0; c < NR_CPUS; c++) {
        run_queues[c].head = run_queues[c].tail = NULL;
        run_queues[c].count = run_queues[c].rt_count = 0;
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
 * Find an eligible entry on queue `owner` for processor `cpu`, optionally
 * requiring a wake mark. Drop a record whose state changed before its
 * removal; sched_lock is held.
 */
static process_t *sched_find(int owner, int cpu, int woken_only)
{
    process_t *p = run_queues[owner].head;

    while (p) {
        process_t *next = p->ready_next;

        pick_steps++;
        if (p->state != PROC_READY) {
            sched_remove(p);
        } else if ((!woken_only || p->woken) && sched_may_run(p, cpu)) {
            return p;
        }
        p = next;
    }
    return NULL;
}

/**
 * Take the first eligible entry from the busiest other queue, trying each
 * donor at most once. Counts choose donors without visiting blocked records.
 * The returned process is unlinked before its cpu changes; sched_lock is held.
 */
static process_t *sched_steal(int cpu)
{
    uint64_t tried = (uint64_t)1 << cpu;
    process_t *p;
    unsigned int most;
    int c, donor;

    for (;;) {
        donor = -1;
        most = 0;
        for (c = 0; c < NR_CPUS; c++) {
            if (!((tried >> c) & 1u) && run_queues[c].count > most) {
                donor = c;
                most = run_queues[c].count;
            }
        }
        if (donor < 0) {
            return NULL;
        }
        tried |= (uint64_t)1 << donor;
        p = sched_find(donor, cpu, 0);
        if (p) {
            sched_remove(p);
            sched_move(p, cpu);
            return p;
        }
    }
}

/**
 * The processor a ready process of `p`'s affinity joins: the one it ran on
 * last, unless that one is busy and another it may run on is idle, where
 * it then goes. The run queues' lock is held.
 */
static int sched_choose(const process_t *p)
{
    int last = p->cpu;
    int c;

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
 * The processor whose queue a process made ready joins, as `sched_choose`
 * gives it; a process that is still a processor's current, woken before its
 * switch away, stays where it runs. The run queues' lock is held.
 */
static int sched_place(const process_t *p)
{
    return cpu_table[p->cpu].current == p ? p->cpu : sched_choose(p);
}

/**
 * Make `p` ready, on the queue `sched_place` gives it, and return the
 * processor to interrupt so that it selects now: one that is idle, or
 * halted in place with `p` current, or runs a thread `p` outranks; -1 when
 * none needs it. `sched_lock` is held.
 */
static int sched_ready_locked(process_t *p)
{
    int target;

    sched_remove(p);
    p->state = PROC_READY;
    proc_timer_update(p);
    target = sched_place(p);
    sched_move(p, target);
    if (cpu_table[target].current != p) {
        sched_append(p);
    } else {
        waketime_state(p, 2, target);
    }
    if (sched_cpu_idle(target) || cpu_table[target].current == p ||
        sched_priority(p) > sched_priority(cpu_table[target].current)) {
        return target;
    }
    return -1;
}

void sched_make_ready(process_t *p)
{
    uint32_t flags = spin_lock_irqsave(&sched_lock);
    int kick = sched_ready_locked(p);

    spin_unlock_irqrestore(&sched_lock, flags);
    if (kick >= 0) {
        smp_kick(kick);
    }
}

int sched_wake_blocked(process_t *p)
{
    uint32_t flags = spin_lock_irqsave(&sched_lock);
    int kick = -1;

    if (p->state == PROC_BLOCKED) {
        p->woken = 1;
        kick = sched_ready_locked(p);
    }
    spin_unlock_irqrestore(&sched_lock, flags);
    return kick;
}

void sched_kick_cpus(uint64_t cpus)
{
    int c;

    for (c = 0; cpus != 0; c++, cpus >>= 1) {
        if (cpus & 1u) {
            smp_kick(c);
        }
    }
}

void sched_set_affinity(process_t *p, uint64_t mask)
{
    uint32_t flags = spin_lock_irqsave(&sched_lock);
    struct affinity_wait wait;
    int running = cpu_table[p->cpu].current == p;
    int remote = running && p != current_proc && !((mask >> p->cpu) & 1u);
    int kick = -1;

    p->affinity = mask;
    if (remote) {
        wait_queue_init(&wait.queue);
        wait.task = p;
        wait.done = 0;
        wait.next = affinity_waits;
        affinity_waits = &wait;
    }
    if (p->state == PROC_READY && !running && !sched_allowed(p, p->cpu)) {
        sched_remove(p);
        sched_move(p, sched_choose(p));
        sched_append(p);
        if (sched_cpu_idle(p->cpu)) {
            kick = p->cpu;
        }
    }
    if (remote) {
        p->need_resched = 1;
        kick = p->cpu;
    }
    spin_unlock(&sched_lock);
    if (!remote) {
        affinity_complete(p);
    }
    if (kick >= 0) {
        smp_kick(kick);
    }
    if (remote) {
        (void)wq_wait_event_nointr(&wait.queue, affinity_done, &wait);
    } else if (p == current_proc && !((mask >> arch_cpu_id()) & 1u)) {
        schedule();
    }
    kirq_restore(flags);
}

/** Whether selection keeps `old`, a running thread, over `next`, the best
 * queued real-time candidate or NULL: `old` is real-time and outranks
 * `next`, or ties with it and is neither SCHED_RR nor yielding. */
static int sched_keeps(const process_t *old, const process_t *next)
{
    return sched_priority(old) != 0 && (!next || sched_priority(old) > sched_priority(next) ||
                                        (sched_priority(old) == sched_priority(next) &&
                                         old->sched_policy != SCHED_RR && !old->sched_yielded));
}

/** Select a real-time thread first, then apply ordinary wake preference.
 * The run queue lock is held and old's state already reflects blocking,
 * affinity and suspension. NULL means there is no queued candidate. */
static process_t *sched_select(int me, process_t *old)
{
    process_t *next = sched_find_rt(me);
    struct percpu *cpu = &cpu_table[me];
    if (old != cpu->idle && old->state == PROC_RUNNING && sched_keeps(old, next)) {
        old->sched_yielded = 0;
        return old;
    }
    old->sched_yielded = 0;
    if (!next) {
        if (_wake_turn[me]) {
            next = sched_find(me, me, 1);
        }
        if (next) {
            _wake_turn[me] = 0;
        } else {
            next = sched_find(me, me, 0);
            if (next) {
                _wake_turn[me] = 1;
            } else if (old == cpu->idle || old->state != PROC_RUNNING) {
                next = sched_steal(me);
            }
        }
    }
    return next;
}

/** Refresh the outgoing task's state and placement before selection.
 * Returns an idle processor to kick after selection, or -1. */
static int sched_prepare_old(process_t *old, struct percpu *cpu, int me)
{
    int kick = -1;

    sched_remove(old);
    sched_arrive(old);
    proc_timer_update(old);
    old->woken = 0;
    if (old->state == PROC_READY) {
        old->state = PROC_RUNNING;
    }
    if (cpu->idle && old != cpu->idle && (old->state == PROC_RUNNING || old->state == PROC_READY) &&
        !((old->affinity >> me) & 1u)) {
        old->state = PROC_READY;
        sched_move(old, sched_choose(old));
        if (sched_cpu_idle(old->cpu)) {
            kick = old->cpu;
        }
    }
    if (cpu->idle && old != cpu->idle && old->state == PROC_RUNNING && !suspend_lets_run(old)) {
        old->state = PROC_READY;
    }
    return kick;
}

/**
 * A preferred selection consumes its wake mark without passing over earlier
 * ready entries, and the next selection is ordinary, so the head advances at
 * least every second selection even when sleepers wake repeatedly; a turn
 * with no woken candidate is ordinary. A processor steals from another
 * queue (`sched_steal`) only when its current process is its idle task or
 * cannot run on, and with nothing to steal, one that cannot run on gives
 * way to the idle task. A process that blocks or ends therefore always
 * leaves the processor, and a woken one may run on any processor it may
 * use.
 *
 * A current process whose affinity no longer has this processor is made
 * ready on a processor it has; that processor may select it before the
 * switch away from it is done, and waits for its `on_cpu` to clear before
 * it switches to it. A
 * current process a suspend to idle holds is made ready where it is: on
 * one processor the suspend's writer was the only one to run, and on
 * several the others stop what they run at its next switch: the tick's,
 * a call's exit, or a kernel thread's next sleep or service point.
 *
 * Selection and publication run with interrupts disabled and the run
 * queues' lock held, so a wake cannot race the mark's consumption or a
 * timer re-enter partially updated state. A current process that has
 * already resumed after an idle wait loses its mark before yielding, so
 * that wake cannot prefer it again later. The outgoing thread's request
 * (`need_resched`) is cleared in the same hold, once selection is done
 * and before either return without a switch, as Linux's __schedule
 * clears it (kernel/sched/core.c:6705-6707): a request made after the
 * selection is kept, and one the selection has answered is not taken
 * again.
 *
 * The switch runs with interrupts disabled: a timer interrupt between the
 * publication of the new current process and the change of stacks inside
 * switch_context would find `next` current and outside a system call, pass
 * `sched_preempt`'s test, and enter schedule() again on the old stack,
 * saving that stack's pointer in next's record. So schedule() saves the
 * caller's IF, clears it across the switch, and restores it afterward. Each
 * process does this symmetrically and resumes with its own IF; a process
 * entered for the first time skips the restore: a forked child takes its
 * IF from iret, and a kernel thread's entry enables interrupts itself.
 *
 * A switch away from a process that could still run calls its `sched_out`
 * in the same hold of the run queues' lock as the selection, so that what
 * it marks is true of the switch made: KVM's steal time says a vCPU was
 * preempted exactly when it was, as Linux's kvm_sched_out says it.
 *
 * The switch is Linux's protocol (kernel/sched/core.c:4972-4990, 3798):
 * the incoming thread's `on_cpu` is set before the switch, and the
 * outgoing one's is cleared by the incoming thread's tail once the switch
 * has saved its registers (`schedule_tail`). A thread woken while it still
 * switches away on another processor may be selected here meanwhile; the
 * switch to it waits for its `on_cpu` to clear first, outside the run
 * queues' lock, which the tail on that processor may take. The run queues'
 * lock is let go before the switch, since the incoming process does not
 * return here to let it go.
 *
 * The big lock does not cross the switch. The outgoing thread's depth is
 * its own: it is saved in its record and the lock let go, and the tail
 * takes the lock back at the incoming thread's own depth, none for the
 * idle task or a thread switched away outside the lock. So a processor
 * that goes idle or runs a thread that holds nothing keeps the lock from
 * no one, as Linux 2.6's `release_kernel_lock` and `reacquire_kernel_lock`
 * gave it up around the switch.
 *
 * The floating-point registers are the one part of the processor
 * `switch_context` does not touch. They are saved into the outgoing record
 * and loaded from the incoming one here, inside the same masked window and
 * before the stack pointer changes; the kernel computes in none of them, so
 * nothing between the load and the switch can disturb the loaded state. An
 * idle task has none to keep, and the registers are left as they are
 * around it.
 *
 * Each selection runs the soft-lockup detector's function when its timer
 * queued it (`watchdog_sched`), where Linux's stopper thread would run.
 */
void schedule(void)
{
    process_t *next = NULL;
    process_t *old;
    struct percpu *cpu;
    uint32_t saved_flags;
    int me;
    int kick = -1;

    if (!_sched_enabled || !current_proc) {
        return;
    }
    lockdep_switch((uintptr_t)(const char *)__builtin_return_address(0));

    saved_flags = spin_lock_irqsave(&sched_lock);
    me = (int)arch_cpu_id();
    cpu = &cpu_table[me];
    old = cpu->current;
    pick_count++;
    watchdog_sched();
    kick = sched_prepare_old(old, cpu, me);
    next = sched_select(me, old);
    old->need_resched = 0;

    if (!next) {
        if (old == cpu->idle || old->state == PROC_RUNNING || !cpu->idle) {
            waketime_state(old, old->state == PROC_RUNNING ? 2 : 3, me);
            spin_unlock_irqrestore(&sched_lock, saved_flags);
            return;
        }
        next = cpu->idle;
    }
    if (kick >= 0) {
        smp_kick(kick);
    }

    sched_remove(next);
    sched_arrive(next);
    waketime_state(next, 2, me);
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
    if (old != cpu->idle) {
        if (old->state == PROC_READY) {
            old->nivcsw++;
            if (old->sched_out) {
                old->sched_out(old->sched_out_arg);
            }
        } else {
            old->nvcsw++;
        }
    }

    if (old->state == PROC_READY && old != cpu->idle) {
        sched_append(old);
    } else {
        waketime_state(old, 3, me);
    }
    rseq_preempt(old);
    next->state = PROC_RUNNING;
    sched_move(next, me);
    next->pcount++;

    if (old != cpu->idle) {
        fpu_save(old->fpu);
    }
    if (next != cpu->idle) {
        fpu_restore(next->fpu);
    }
    arch_debug_switch(next);
    cpu->current = next;
    cpu->switch_prev = old;
    spin_unlock(&sched_lock);
    old->bkl_depth = bkl_drop();
    sched_wait_off_cpu(next);
    next->on_cpu = 1;

    switch_context(old, next);

    schedule_tail();
    membarrier_switch(old);
    kirq_restore(saved_flags);
}

/**
 * The outgoing thread's registers are saved once `switch_context` returns
 * here, so its pending moves complete (`affinity_complete`) and its `on_cpu`
 * clears, with the exchange's full barrier, before this thread takes the
 * big lock: a holder may be waiting for that `on_cpu` to free the thread's
 * stack (`sched_wait_off_cpu`). A switch made outside `schedule`, which
 * leaves no outgoing thread and keeps the processor's depth, takes
 * nothing.
 */
void schedule_tail(void)
{
    struct percpu *cpu = this_cpu();
    process_t *prev = cpu->switch_prev;
    process_t *self = cpu->current;

    if (prev) {
        cpu->switch_prev = NULL;
        affinity_complete(prev);
        (void)arch_atomic_xchg(&prev->on_cpu, 0u);
    }
    if (self->bkl_depth > 0 && cpu->bkl_depth == 0) {
        bkl_take_as(self->bkl_depth, BKL_KEY_SWITCH);
    }
}

void sched_wait_off_cpu(const process_t *p)
{
    uint32_t flags;

    if (!p->on_cpu) {
        return;
    }
    flags = kirq_save();
    while (p->on_cpu) {
        smp_service();
        arch_relax();
    }
    kirq_restore(flags);
}

/**
 * A wake that puts a process on this processor's queue sends it the
 * interprocessor interrupt that ends the wait (`sched_make_ready`). The
 * idle task holds no big lock: its selection and its halt need none, and a
 * thread it switches to takes the lock in its tail when it holds it.
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
 * A process interrupted in user mode and process 0 are switched at once.
 * A processor's idle task is not: the interrupt ended its halt, and its
 * loop selects as soon as the handler returns, with the idle marker of the
 * halt cleared first, which a switch from inside the handler would leave
 * set for the process switched to. A process inside a system call or a
 * fault is not either: the call's code runs with interrupts on while its
 * own state is half changed, and `sys_exit`, for one, publishes the zombie
 * its parent may reap, kernel stack and all, before it calls `schedule`.
 * Nor is a kernel thread (`no_tick_switch`), whose work changes state that
 * only the big lock guards, which the switch lets go: Linux without
 * CONFIG_PREEMPTION only sets TIF_NEED_RESCHED from the tick
 * (kernel/sched/core.c:1094) and switches a kernel thread where it calls
 * schedule or cond_resched (:7287), never on an interrupt's return to
 * kernel mode (kernel/entry/common.c:356). Nor is a thread inside klog
 * (`klog_inside`), in a record or a console write, as process 0 is while
 * the debug shell prints a line: klog's count of its users would stay up
 * while the big lock passed on, as Linux writes each console record with
 * interrupts off and gives way only between records
 * (kernel/printk/printk.c:3074-3089).
 *
 * Each of these keeps the request in its record (`need_resched`): a call
 * takes it at its exit and a fault at its end, so a process that is always
 * inside a call still gives way once per call; a kernel thread takes it at
 * its next sleep or service point (`sched_service_point`), and a thread in
 * klog at a later tick, once it has left klog.
 */
void sched_preempt(void)
{
    if (current_proc && current_proc == this_cpu()->idle) {
        return;
    }
    if (current_proc && (current_proc->in_syscall || current_proc->in_fault ||
                         current_proc->no_tick_switch || klog_inside())) {
        current_proc->need_resched = 1;
        return;
    }
    schedule();
}

/** `schedule` clears the request under the run queues' lock, after its
 * selection, so a request that arrives after the selection is the next
 * call's and none is lost by a clear after the switch. */
void sched_take_preempt(void)
{
    if (current_proc && current_proc->need_resched) {
        schedule();
    }
}

/**
 * A call that loops over a large buffer keeps the processor for the whole
 * loop, since the tick's switch is taken at the call's exit; a reader woken
 * by a keystroke then waits for the loop. A kernel thread keeps it until it
 * sleeps (`no_tick_switch`), and calls this between two completed works, as
 * Linux's worker calls cond_resched between works (kernel/workqueue.c:3290-
 * 3299). The switch is taken here as the exit would take it, with
 * interrupts off; `schedule` clears the request.
 *
 * `schedule` passes the big lock on only past a real switch, and a call on
 * a processor with nothing else queued has none to make, so the lock goes
 * first to any processor that has claimed it (`bkl_yield`). The tick skips
 * a processor while another holds the lock, so a call that shares the lock
 * would seldom see a request: what is queued here counts as one. The caller
 * is at a point where it could have slept, which the soft-lockup detector
 * counts as a schedule (`watchdog_sched`), as Linux's cond_resched would
 * switch to the stopper thread the watchdog queued.
 */
void sched_service_point(void)
{
    uint32_t flags = kirq_save();

    watchdog_sched();
    bkl_yield();
    if (current_proc && current_proc != this_cpu()->idle) {
        uint32_t held = spin_lock_irqsave(&sched_lock);

        if (run_queues[arch_cpu_id()].count > 0) {
            current_proc->need_resched = 1;
        }
        spin_unlock_irqrestore(&sched_lock, held);
    }
    sched_take_preempt();
    kirq_restore(flags);
}

void sched_give_way(void)
{
    if (kirq_enabled() && !in_interrupt()) {
        sched_service_point();
    }
}

/*
 * Each processor's idle marker and three tick counters are in its record
 * (percpu.h). They are `volatile` because the timer handler reads the marker
 * from an interrupt that arrives in the middle of the halt it describes. The
 * counters are 64 bits wide, so they do not wrap, and are read whole within
 * the record's `ticks_seq` (`sched_cpu_ticks`), whose wait answers this
 * processor's requests, since the reader may have interrupts off. The
 * totals are summed over every record, of which those of processors never
 * started are zero.
 */
void sched_cpu_ticks(int c, uint64_t *user, uint64_t *busy, uint64_t *idle)
{
    const struct percpu *cpu = &cpu_table[c];
    uint32_t seq;

    for (;;) {
        seq = cpu->ticks_seq;
        if (seq & 1u) {
            uint32_t flags = kirq_save();

            smp_service();
            kirq_restore(flags);
            arch_relax();
            continue;
        }
        *user = cpu->user_ticks;
        *busy = cpu->busy_ticks;
        *idle = cpu->idle_ticks;
        if (cpu->ticks_seq == seq) {
            return;
        }
    }
}

uint64_t sched_idle_ticks(void)
{
    uint64_t sum = 0, user, busy, idle;
    int i;

    for (i = 0; i < NR_CPUS; i++) {
        sched_cpu_ticks(i, &user, &busy, &idle);
        sum += idle;
    }
    return sum;
}

uint64_t sched_busy_ticks(void)
{
    uint64_t sum = 0, user, busy, idle;
    int i;

    for (i = 0; i < NR_CPUS; i++) {
        sched_cpu_ticks(i, &user, &busy, &idle);
        sum += busy;
    }
    return sum;
}

int sched_all_idle(void)
{
    int c;

    for (c = 0; c < NR_CPUS; c++) {
        if (cpu_table[c].online && !cpu_table[c].idle_wait) {
            return 0;
        }
    }
    return 1;
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
 * Count one tick of processor `cpu` as idle or busy, and charge a busy one
 * to the process it interrupted, to its process's virtual and profiling
 * timers, and to the POSIX timers that count its processor time.
 *
 * The process charged is `current_proc`, whose code, system call or
 * interrupt handler the tick interrupted, and `user` says whether the
 * interrupted code was ring 3's; Linux charges an interrupt handler's time to
 * the process it interrupted too. A tick that finds the processor in an idle
 * wait is charged to no one, although the process whose call is waiting is
 * still current.
 */
static void sched_account_one(struct percpu *cpu, int user)
{
    int idle = cpu->idle_wait;

    cpu->ticks_seq++;
    if (idle) {
        ++cpu->idle_ticks;
    } else {
        ++cpu->busy_ticks;
        if (user) {
            ++cpu->user_ticks;
        }
    }
    cpu->ticks_seq++;
    if (!idle && current_proc) {
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

/**
 * Each tick is counted as `sched_account_one` counts one. The boot
 * processor calls this after it has counted its tick and any the
 * clock found lost, and so charges all of them. Another processor's tick is
 * out of step with the boot processor's and charges the global ticks since
 * its last, none or more than one. A tick lost while an interrupt was held
 * off, which the big lock can hold for longer than a tick, is charged to
 * what the processor was doing when the next one came.
 */
void sched_account_tick(int user)
{
    struct percpu *cpu = this_cpu();
    uint32_t now = (uint32_t)kernel_ticks;
    uint32_t ticks = now - cpu->accounted_tick;

    cpu->ticks_passed = 0;
    cpu->accounted_tick = now;
    while (ticks-- > 0) {
        sched_account_one(cpu, user);
    }
}

int sched_tick_waits(void)
{
    struct percpu *cpu = this_cpu();
    uint32_t flags = spin_lock_irqsave(&sched_lock);
    int queued = run_queues[arch_cpu_id()].count != 0;

    spin_unlock_irqrestore(&sched_lock, flags);
    if (!queued) {
        cpu->ticks_passed = 0;
        return 0;
    }
    return ++cpu->ticks_passed >= SCHED_TICK_PASSES;
}

/* The load averages, Linux's: in fixed point, sampled every LOAD_FREQ
 * ticks, five seconds and one, and decayed by e^(-5/60), e^(-5/300) and
 * e^(-5/900) at each sample. */
#define LOAD_ONE  (1u << SCHED_LOAD_SHIFT)
#define LOAD_FREQ (5u * TICK_HZ + 1u)
static const uint32_t load_decay[3] = {1884u, 2014u, 2037u};
static uint32_t load_avg[3];
static uint32_t load_wait = LOAD_FREQ;
static uint32_t load_tick; /* the tick count at the last call */

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
 * `decay` to the power `n` in the averages' fixed point, rounded at each
 * product, as Linux's `fixed_power_int` takes it: the decay of `n` samples
 * at once.
 */
static uint32_t sched_load_power(uint32_t decay, uint32_t n)
{
    uint32_t result = LOAD_ONE;

    while (n != 0) {
        if (n & 1u) {
            result = (result * decay + LOAD_ONE / 2u) >> SCHED_LOAD_SHIFT;
        }
        n >>= 1;
        decay = (decay * decay + LOAD_ONE / 2u) >> SCHED_LOAD_SHIFT;
    }
    return result;
}

/**
 * Each average is folded as Linux's `calc_load` folds it: the new average is
 * the old one decayed, plus the count times what the decay took off, rounded
 * up while the count is above it. Linux counts the uninterruptible sleepers
 * as well, and this kernel does not. The products are taken in 64 bits,
 * since 4096 processes in fixed point times a decay pass 32. The period is
 * measured by the tick count, since a boot processor's tick that found the
 * big lock held ends without calling this, and the next counts its ticks:
 * a sample made late keeps the period's phase, and the samples a long gap
 * passed over are folded at once with the decay to their power, as Linux's
 * `calc_global_nohz` and `calc_load_n` fold them.
 */
void sched_load_tick(void)
{
    uint32_t active, total, now = (uint32_t)kernel_ticks, passed = now - load_tick, n;
    int i;

    load_tick = now;
    if (passed < load_wait) {
        load_wait -= passed;
        return;
    }
    passed -= load_wait;
    n = 1u + passed / LOAD_FREQ;
    load_wait = LOAD_FREQ - passed % LOAD_FREQ;
    sched_task_counts(&active, &total);
    active *= LOAD_ONE;
    for (i = 0; i < 3; i++) {
        uint32_t decay = n == 1u ? load_decay[i] : sched_load_power(load_decay[i], n);
        uint64_t next = (uint64_t)load_avg[i] * decay + (uint64_t)active * (LOAD_ONE - decay);

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
 * Every caller of this one enters with IF already clear, having checked its
 * condition there, and that is what makes the wait free of a lost wakeup:
 * `sti; hlt` issued from IF=0 is atomic against a wake that became pending
 * during the check, because the interrupt fires the instant after `hlt` and
 * never before it. The marker is set while interrupts are still off, so no
 * tick can be delivered between setting it and the halt it describes.
 *
 * Anything on the loopback ring is delivered before the processor is parked,
 * and this returns without halting so the caller checks its condition again.
 * The drain needs the big lock: a caller without it, the idle task, tries
 * the lock, and when another processor holds it leaves the ring to that
 * holder, whose call's exit drains it, or to the next tick that has the
 * lock, and halts.
 * Those datagrams are this machine's own, a delivery can be what makes a
 * process runnable, and nothing else is going to run to deliver them. A
 * call that sleeps inside, a blocking connect or a send that waits for the
 * peer's window, has put datagrams there that no system call's exit has
 * delivered yet, and they would otherwise wait for the timer. Interrupts are
 * off here, which is the discipline the drain needs, and a caller that is
 * about to halt holds nothing and is between operations, which is the state
 * the timer's drain relies on too.
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
        if (this_cpu()->bkl_depth > 0) {
            ip_loop_drain_from(IP_LOOP_FROM_IDLE);
            return;
        }
        if (bkl_try_enter()) {
            ip_loop_drain_from(IP_LOOP_FROM_IDLE);
            bkl_exit();
            return;
        }
    }
    this_cpu()->idle_wait = 1;
    depth = bkl_drop();
    arch_idle_masked();
    bkl_take_as(depth, BKL_KEY_IDLE);
    this_cpu()->idle_wait = 0;
}

/**
 * The callers run with IF set, so a wake that comes between a caller's test
 * of its condition and this call is taken at once and leaves the halt to
 * wait for the next interrupt; that costs at most one more tick of waiting,
 * which is why every such caller tests its condition again in a loop rather
 * than assuming the halt ended for its own reason. From the marker to the
 * `sti; hlt` interrupts are off, so the tick that ends the halt is the
 * first the marker can catch.
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
    arch_idle_masked();
    bkl_take_as(depth, BKL_KEY_IDLE);
    this_cpu()->idle_wait = 0;
    kirq_restore(flags);
}
