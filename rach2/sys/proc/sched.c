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

/**
 * Whether a queued real-time thread that selection on processor `cpu`
 * could take, from any processor's queue as `sched_find_rt` takes it,
 * outranks `cur`, the thread running there. An equal one does not: an RR
 * peer waits for the slice, as Linux requeues an RR thread only once its
 * time slice has run out (task_tick_rt, kernel/sched/rt.c:2586-2602), and
 * one queued behind a higher thread on another processor counts, as Linux
 * pushes it to a processor that runs lower work and has that one
 * reschedule (push_rt_task and find_lowest_rq, rt.c:1807, 1982-2092). A
 * wake places a real-time thread on such a processor at once
 * (`sched_rt_place`); this finds one queued behind a thread raised above
 * it, or one whose processor started lower work since.
 * Counts no selection work. The run queues' lock is held.
 */
static int sched_rt_outranks(int cpu, const process_t *cur)
{
    unsigned int prio = sched_priority(cur);
    process_t *q;
    int c;

    for (c = 0; c < NR_CPUS; c++) {
        if (run_queues[c].rt_count == 0) {
            continue;
        }
        for (q = run_queues[c].head; q; q = q->ready_next) {
            if (sched_priority(q) > prio && sched_may_run(q, cpu)) {
                return 1;
            }
        }
    }
    return 0;
}

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
 * Ask processor `cpu`, on whose queue `p` has just been put, to select now
 * when it should: return `cpu` when it is idle, or when `p` outranks the
 * thread it runs, whose `need_resched` is then set, as Linux's
 * wakeup_preempt calls resched_curr for a task of a higher class, and
 * wakeup_preempt_rt for a real-time task of a higher priority
 * (kernel/sched/core.c:2151-2156, rt.c:1657-1658); return -1 otherwise.
 * The interrupt switches in user mode only on that request
 * (`isr_local_router`), and a request to the caller's own processor, whose
 * interrupt does nothing, is taken at the call's exit (`sched_take_preempt`).
 * The run queues' lock is held.
 */
static int sched_preempt_cpu(const process_t *p, int cpu)
{
    process_t *cur = cpu_table[cpu].current;

    if (sched_cpu_idle(cpu)) {
        return cpu;
    }
    if (sched_priority(p) > sched_priority(cur)) {
        cur->need_resched = 1;
        return cpu;
    }
    return -1;
}

/** Whether `p` may run on one started processor only. The run queues' lock
 * is held. */
static int sched_pinned(const process_t *p)
{
    int c, n = 0;

    for (c = 0; c < NR_CPUS; c++) {
        n += sched_allowed(p, c);
    }
    return n < 2;
}

/**
 * The highest real-time priority on processor `cpu`: that of its current
 * thread or of a ready thread on its queue, whichever is higher, 0 when
 * neither is real-time, as Linux's rt.highest_prio.curr counts the running
 * task with the queued ones (kernel/sched/rt.c:1108-1144). The run queues'
 * lock is held.
 */
static unsigned int sched_cpu_rt_top(int cpu)
{
    unsigned int top = sched_priority(cpu_table[cpu].current);
    process_t *q;

    if (run_queues[cpu].rt_count == 0) {
        return top;
    }
    for (q = run_queues[cpu].head; q; q = q->ready_next) {
        if (q->state == PROC_READY && sched_priority(q) > top) {
            top = sched_priority(q);
        }
    }
    return top;
}

/**
 * The processor a woken real-time thread `p` goes to instead of `target`,
 * whose current thread is real-time and either ranks with `p` or above it,
 * or may run nowhere else, as Linux's select_task_rq_rt tests it
 * (kernel/sched/rt.c:1577-1597): of the started processors `p` may run on,
 * one whose highest real-time priority, of its current thread and its queue
 * (`sched_cpu_rt_top`), is lowest, `target` first among equals, then the
 * waker's, then the lowest number, as find_lowest_rq ranks each processor
 * through cpupri_find and prefers the task's last processor and then this
 * one (rt.c:1807-1880). That one is taken only when `p` outranks every
 * thread there, as select_task_rq_rt moves a task only when it outranks
 * the target queue's highest_prio.curr (rt.c:1594-1596); otherwise `target`
 * stays. The run queues' lock is held.
 */
static int sched_rt_place(const process_t *p, int target)
{
    process_t *cur = cpu_table[target].current;
    int me = (int)arch_cpu_id();
    int best = -1, c;
    unsigned int top, best_top = 0, rank, best_rank = 0;

    if (sched_priority(p) == 0 || sched_priority(cur) == 0 ||
        (sched_priority(cur) < sched_priority(p) && !sched_pinned(cur))) {
        return target;
    }
    for (c = 0; c < NR_CPUS; c++) {
        if (!sched_allowed(p, c)) {
            continue;
        }
        /* Priority first, then target, the waker's and the lowest number. */
        top = sched_cpu_rt_top(c);
        rank = top * 4u + (c == target ? 0u : c == me ? 1u : 2u);
        if (best < 0 || rank < best_rank) {
            best = c;
            best_top = top;
            best_rank = rank;
        }
    }
    if (best >= 0 && sched_priority(p) > best_top) {
        return best;
    }
    return target;
}

/**
 * Make `p` ready, on the queue `sched_place` gives it, or for a real-time
 * thread that does not outrank the real-time thread running there, or that
 * thread may run nowhere else, on a processor running and holding lower
 * work (`sched_rt_place`), and return the
 * processor to interrupt so that it selects now, as `sched_preempt_cpu`
 * gives it, or the one where `p` is halted in place as current; -1 when
 * none needs it. `sched_lock` is held.
 */
static int sched_ready_locked(process_t *p)
{
    int target;

    sched_remove(p);
    p->state = PROC_READY;
    proc_timer_update(p);
    target = sched_place(p);
    if (cpu_table[target].current == p) {
        waketime_state(p, 2, target);
        return target;
    }
    if (!sched_cpu_idle(target)) {
        target = sched_rt_place(p, target);
    }
    sched_move(p, target);
    sched_append(p);
    return sched_preempt_cpu(p, target);
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

/**
 * A queued thread asks the thread it now outranks on its processor to
 * switch, as Linux's prio_changed_rt and switched_to_rt call resched_curr
 * on the queue's current task (kernel/sched/rt.c:2486-2492, 2526-2533). A
 * running thread asks itself when a queued thread that selection on its
 * processor could take now outranks it, as prio_changed_rt reschedules a
 * running task once the queue's highest priority is above its own
 * (rt.c:2519-2520); one that left the real-time policy with only ordinary
 * threads queued gives way at the tick once its slice has run, where
 * Linux's switched_to_fair asks at once (fair.c:13367-13368). A blocked
 * thread asks nothing.
 */
void sched_priority_changed(process_t *p)
{
    uint32_t flags = spin_lock_irqsave(&sched_lock);
    int kick = -1;

    if (p->ready_linked) {
        struct ready_queue *q = &run_queues[p->cpu];

        q->rt_count -= p->ready_rt;
        p->ready_rt = sched_priority(p) != 0;
        q->rt_count += p->ready_rt;
    }
    if (cpu_table[p->cpu].current == p) {
        if (sched_rt_outranks(p->cpu, p)) {
            p->need_resched = 1;
            kick = p->cpu;
        }
    } else if (p->ready_linked) {
        kick = sched_preempt_cpu(p, p->cpu);
    }
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

uint32_t sched_lock_take(void)
{
    return spin_lock_irqsave(&sched_lock);
}

void sched_lock_give(uint32_t flags)
{
    spin_unlock_irqrestore(&sched_lock, flags);
}

void sched_wake_signal(process_t *p)
{
    uint32_t flags = spin_lock_irqsave(&sched_lock);
    int kick = -1;

    if (p->state == PROC_BLOCKED) {
        p->wakeup_tick = 0;
        p->woken = 1;
        kick = sched_ready_locked(p);
    }
    spin_unlock_irqrestore(&sched_lock, flags);
    if (kick >= 0) {
        smp_kick(kick);
    }
}

int sched_wake_due_locked(process_t *p, uint64_t now)
{
    if (p->state != PROC_BLOCKED || p->wakeup_tick == 0 || now < p->wakeup_tick) {
        return -1;
    }
    waketime_expire(p, p->wakeup_tick);
    p->wakeup_tick = 0;
    p->woken = 1;
    return sched_ready_locked(p);
}

int sched_wake_time_locked(process_t *p)
{
    if (p->state != PROC_BLOCKED || p->wakeup_tick == 0) {
        return -1;
    }
    p->wakeup_tick = 0;
    p->woken = 1;
    return sched_ready_locked(p);
}

void sched_wake_due(process_t *p, uint64_t now)
{
    uint32_t flags = spin_lock_irqsave(&sched_lock);
    int kick = sched_wake_due_locked(p, now);

    spin_unlock_irqrestore(&sched_lock, flags);
    if (kick >= 0) {
        smp_kick(kick);
    }
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
        kick = sched_preempt_cpu(p, p->cpu);
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
 * Returns the processor a moved task asks to select (`sched_preempt_cpu`),
 * to kick after selection, or -1. */
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
        kick = sched_preempt_cpu(old, old->cpu);
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
 * A switch away from a process calls its `sched_out` in the same hold of
 * the run queues' lock as the selection, saying whether the process could
 * still run, so that what it marks is true of the switch made: KVM's steal
 * time says a vCPU was preempted exactly when it was, as Linux's
 * kvm_sched_out says it, and KVM gives the host back the processor's state
 * that a vCPU's run left the guest's at every switch, as Linux's
 * kvm_arch_vcpu_put.
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
        } else {
            old->nvcsw++;
        }
        if (old->sched_out) {
            old->sched_out(old->sched_out_arg, old->state == PROC_READY);
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
    next->slice_ns = sched_clock_ns();

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
 * A wake that puts a process on this processor's queue kicks it, which
 * ends the wait's poll or sends the interprocessor interrupt that ends its
 * halt (`sched_ready_locked` asks for it, and the waker kicks once its locks
 * are let go). The
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

/* The least a thread runs once switched onto a processor before an
 * interrupt gives the processor to another thread of its queue that no wake
 * marked: a chosen value, under the 1 ms tick, so that a thread switched in
 * at a tick still gives way at the next one. Linux's base slice is 0.70 ms
 * times 1 + ilog2(min(online processors, 8)), 2.1 ms with four
 * (kernel/sched/fair.c:76, 191-220). */
#define SCHED_SLICE_NS 750000u

/**
 * A switch made between two ticks gives the incoming thread only what is
 * left of the tick, and the next tick would hand the processor back to a
 * thread that yielded to it, as KVM's steal_time selftest yields to a
 * thread that runs for 200 us and waits for its own run_delay to grow by
 * as much. So the tick and a device's interrupt end a thread's turn only
 * once it has run SCHED_SLICE_NS since its switch in, as Linux's tick ends
 * one only once its slice has run (update_deadline and update_curr,
 * kernel/sched/fair.c:1022-1045, 1271-1274); Linux keeps the deadline
 * across switches where this measures from each switch in.
 *
 * A real-time thread that selection could take here, from any processor's
 * queue, and that outranks the current thread (`sched_rt_outranks`) lets
 * the interrupt select within the slice all the same, as Linux's
 * wakeup_preempt_rt asks a lower current task to switch
 * (kernel/sched/rt.c:1657-1658); an equal one waits for the slice, as
 * task_tick_rt requeues an RR task only once its time slice has run out
 * (rt.c:2586-2602). Under an ordinary current thread, a thread a wake
 * marked (`woken`) waiting on this processor's queue does too; under a
 * real-time one it does not, since selection would not take it, and a
 * real-time thread gives way within its slice only to one that outranks
 * it. A woken thread is then one candidate: selection alternates between a
 * woken thread and the head of the queue (`_wake_turn`), so the woken
 * reader may run only at the next selection.
 *
 * Linux ends an ordinary turn within the slice in more cases than the
 * deadline. Its tick reschedules once the current thread has lost its
 * slice's protection and is no longer eligible (did_preempt_short,
 * fair.c:1180-1188, 1271-1274). A wake preempts the current thread when the
 * woken one is the most eligible (check_preempt_wakeup_fair,
 * fair.c:8862-8951, 8944), and within the current thread's protected slice
 * (RUN_TO_PARITY in pick_eevdf, fair.c:943, set at set_next_entity,
 * fair.c:5633) only when the current thread is no longer eligible, which
 * drops it from the pick, protection and all (fair.c:940-941), when it is
 * of the idle policy and the woken one is not (fair.c:8911-8917), or when
 * the woken thread's shorter custom slice cancels the protection
 * (fair.c:8938-8939). Here every wake lets the interrupt select, so that a
 * reader runs soon after its data comes.
 *
 * Called with interrupts off, at the end of an interrupt's handler.
 */
void sched_tick(void)
{
    process_t *p = current_proc, *q;
    uint32_t flags;
    int due;

    if (p && p != this_cpu()->idle && sched_clock_ns() - p->slice_ns < SCHED_SLICE_NS) {
        flags = spin_lock_irqsave(&sched_lock);
        q = run_queues[arch_cpu_id()].head;
        due = sched_rt_outranks((int)arch_cpu_id(), p);
        for (; q && !due && sched_priority(p) == 0; q = q->ready_next) {
            due = q->woken;
        }
        spin_unlock_irqrestore(&sched_lock, flags);
        if (!due) {
            return;
        }
    }
    sched_preempt();
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
 * first to any processor that has claimed it (`bkl_yield`). The tick asks
 * a thread inside a call to switch at each tick (`sched_preempt`); what was
 * queued here since counts as a request too, so a thread made ready
 * between two ticks waits only for the next service point. The caller
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
 * Each processor's idle marker and four tick counters are in its record
 * (percpu.h). They are `volatile` because the timer handler reads the marker
 * from an interrupt that arrives in the middle of the halt it describes. The
 * counters are 64 bits wide, so they do not wrap, and are read whole within
 * the record's `ticks_seq` (`sched_cpu_ticks`), whose wait answers this
 * processor's requests, since the reader may have interrupts off. The
 * totals are summed over every record, of which those of processors never
 * started are zero.
 */
void sched_cpu_ticks(int c, uint64_t *user, uint64_t *busy, uint64_t *idle, uint64_t *steal)
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
        *steal = cpu->steal_ticks;
        if (cpu->ticks_seq == seq) {
            return;
        }
    }
}

uint64_t sched_idle_ticks(void)
{
    uint64_t sum = 0, user, busy, idle, steal;
    int i;

    for (i = 0; i < NR_CPUS; i++) {
        sched_cpu_ticks(i, &user, &busy, &idle, &steal);
        sum += idle;
    }
    return sum;
}

uint64_t sched_busy_ticks(void)
{
    uint64_t sum = 0, user, busy, idle, steal;
    int i;

    for (i = 0; i < NR_CPUS; i++) {
        sched_cpu_ticks(i, &user, &busy, &idle, &steal);
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

/**
 * Count a tick of `p`'s processor time against its process's timer `k`
 * when `counts`, and when the timer runs out arm it again for its interval
 * and leave `signo` for a holder of the big lock to post
 * (`proc_tick_signal`). The group's timers are under its `cpu_lock`, since
 * the ticks of its threads on every processor count them, as Linux's
 * `check_cpu_itimer` runs under the signal handler's lock
 * (kernel/time/posix-cpu-timers.c:972-994).
 */
static void cpu_timer_tick(process_t *p, int k, int counts, int signo)
{
    struct tgroup *tg = p->tg;
    int fired = 0;

    if (!counts) {
        return;
    }
    spin_lock(&tg->cpu_lock);
    if (tg->cpu_left[k] != 0 && --tg->cpu_left[k] == 0) {
        tg->cpu_left[k] = tg->cpu_interval[k];
        fired = 1;
    }
    spin_unlock(&tg->cpu_lock);
    if (fired) {
        proc_tick_signal(p, signo);
    }
}

/* The modes a tick is charged to (`sched_account_one`). */
#define ACCOUNT_SYSTEM 0
#define ACCOUNT_USER   1
#define ACCOUNT_IDLE   2

/**
 * Count one tick of processor `cpu` as idle or busy, and charge a busy one
 * to the process it interrupted, to its process's virtual and profiling
 * timers, and to the POSIX timers that count its processor time.
 *
 * The process charged is `current_proc`, whose code, system call or
 * interrupt handler the tick interrupted, and `mode` is ACCOUNT_USER when
 * that was ring 3's code; Linux charges an interrupt handler's time to the
 * process it interrupted too. An ACCOUNT_IDLE tick, one an idle wait
 * took, is charged to no one, although the process whose call is waiting
 * is still current.
 */
static void sched_account_one(struct percpu *cpu, int mode)
{
    int idle = mode == ACCOUNT_IDLE, user = mode == ACCOUNT_USER;

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
 * Each tick is counted as `sched_account_one` counts one. A processor
 * counts the ticks of its own clock, the monotonic time since its last
 * count in whole ticks, rounded to the nearest, as Linux charges each
 * processor's tick to that processor alone (account_process_tick,
 * kernel/sched/cputime.c:474-500, from update_process_times,
 * kernel/time/timer.c:2514-2519, in 6.12.115). Its interrupt normally
 * comes once a tick and counts one; a tick lost while interrupts were off
 * for longer than a tick is charged to what the processor was doing when
 * the next one came. The count does not follow the boot processor's tick,
 * which a stretch with interrupts off there makes late: a count of the
 * global ticks would charge another processor none while it is late and
 * two at its next tick after, to whatever that processor runs then. Each
 * processor writes only its own counts and its current thread's times, so
 * this needs no lock but the group's for its CPU timers
 * (`cpu_timer_tick`).
 *
 * A tick's worth of the hypervisor's steal time not yet counted makes a
 * tick stolen, counted as neither idle nor busy and charged to no one, as
 * Linux takes the steal time out of the tick it charges
 * (steal_account_process_time and account_process_tick,
 * kernel/sched/cputime.c:253-269 and 486-492); the rest carries to the
 * next tick.
 *
 * Linux's kernel runs a system call with interrupts on, so its tick is
 * taken when it comes and charged to the mode it finds (`user_mode`, from
 * update_process_times, kernel/time/timer.c:2519). Rach2's runs one with
 * interrupts off, so a tick that comes in the call is held and taken as
 * interrupts come back on: at the first user instruction after the call, or
 * as an idle wait's halt begins. When the timer keeps the tick's time,
 * `due`, the ticks that would have come before that point (`user_cycles`,
 * which `isr_leave` notes, or `idle_cycles`) are charged where Linux would
 * have taken them: as system time, or as idle when the stretch that held
 * them began at the end of an idle wait (`idle_end_cycles`) and not at an
 * entry from user mode (`entry_cycles`), since Linux's idle task loops
 * there. The rest are charged to the mode the tick found. A tick comes
 * after its time by the delay of its interrupt, which a hypervisor makes
 * microseconds long, so the time a tick would have come is its time plus
 * the processor's average delay (`tick_late_ns`), which ticks that came
 * with interrupts on measure; comparing the bare time would charge the
 * ticks due in the kernel that came in user mode to the kernel, which
 * Linux charges to the program.
 */
void sched_account_tick(int user, int64_t due)
{
    struct percpu *cpu = this_cpu();
    int64_t now = rtc_monotonic_ns(), on = 0;
    int64_t gap = now - cpu->accounted_ns + (int64_t)(TICK_NS / 2u);
    uint64_t ticks, stolen;
    int found = cpu->idle_wait ? ACCOUNT_IDLE : user ? ACCOUNT_USER : ACCOUNT_SYSTEM;
    int held = found;

    if (gap < (int64_t)TICK_NS) {
        return;
    }
    ticks = tick_div64((uint64_t)gap, TICK_NS, NULL);
    cpu->accounted_ns += (int64_t)ticks * (int64_t)TICK_NS;
    if (due != 0 && found != ACCOUNT_SYSTEM) {
        on = timer_cycles_when(user ? cpu->user_cycles : cpu->idle_cycles, arch_cycles(), now);
        if (due >= on && now >= due && now - due < (int64_t)TICK_NS) {
            cpu->tick_late_ns += (now - due - cpu->tick_late_ns) / 8;
        }
        on -= cpu->tick_late_ns;
        held = ACCOUNT_SYSTEM;
        if (!user && cpu->idle_end_cycles > cpu->entry_cycles) {
            held = ACCOUNT_IDLE;
        }
    }
    stolen = arch_steal_ns() - cpu->steal_ns;
    while (ticks-- > 0) {
        if (stolen >= TICK_NS) {
            stolen -= TICK_NS;
            cpu->steal_ns += TICK_NS;
            cpu->ticks_seq++;
            ++cpu->steal_ticks;
            cpu->ticks_seq++;
        } else {
            sched_account_one(cpu, due - (int64_t)ticks * (int64_t)TICK_NS >= on ? found : held);
        }
    }
}

void sched_account_start(void)
{
    struct percpu *cpu = this_cpu();

    cpu->accounted_ns = rtc_monotonic_ns();
    cpu->steal_ns = arch_steal_ns();
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
 * Wait for a wake with interrupts off and the big lock let go: poll this
 * processor's `idle_poll` for `idle_poll_us`, then halt unless a wake
 * turned it to CPU_POLL_KICKED meanwhile (`smp_kick`). A kick that comes
 * after the poll finds it off and sends the interrupt that ends the halt.
 * The poll answers the requests other processors leave (`smp_service`), as
 * a wait for the big lock does, so that their interrupts, pending
 * meanwhile, are taken once at its end: taken one by one inside the poll,
 * as Linux's poll_idle takes them with interrupts on
 * (drivers/cpuidle/poll_state.c:22-41), each cost a guest's interrupt and
 * its exits, and a churn of short processes, whose exits flush every
 * processor, ran 1.5 times as long. Linux sends no such flush to an idle
 * processor in lazy TLB mode (should_flush_tlb, arch/x86/mm/tlb.c:
 * 912-914). A device's completion or a timer's interrupt, which Linux's
 * poll would take at once, the poll looks for every IDLE_POLL_CHECK_US
 * (`arch_irq_waiting`), and halts on it, which takes it while the processor
 * is still marked idle: held to the poll's end, a disk's completion made
 * each of its random reads some 90 us longer, and a tick held past a kick
 * was charged to the thread the processor switched to. A kicked poll
 * likewise lets interrupts in for one instruction before it returns, so
 * that one pending then, seen or not, is taken while the processor is
 * still marked idle, not after its caller waited for the big lock or
 * switched threads.
 *
 * The length adapts as Linux 6.12.115's haltpoll governor adapts its own
 * (adjust_poll_limit, drivers/cpuidle/governors/haltpoll.c): a wait the
 * halt ended within IDLE_POLL_MAX_US doubles it, to at least
 * IDLE_POLL_START_US, and a longer one halves it, to 0 below
 * IDLE_POLL_START_US. A processor whose threads
 * hand work back and forth, as a renderer's do, then takes each wake in the
 * poll, without an interrupt sent under the big lock, a halt and its exit;
 * one that stays idle, which its tick wakes every millisecond, soon halts at
 * once again. As Linux polls only in a guest, so does this: on a machine
 * of its own an interrupt and a halt cost a few microseconds, and a poll
 * the power of a busy processor. Without a measured counter there is no
 * poll either.
 */
static void idle_halt(void)
{
    struct percpu *cpu = this_cpu();
    uint32_t per_us = timer_calibrate() / TICK_US;
    uint64_t start, limit, now, look, gone;
    uint32_t grow;

    if (per_us == 0 || !arch_guest()) {
        arch_idle_masked();
        return;
    }
    start = arch_cycles();
    if (cpu->idle_poll_us != 0) {
        limit = (uint64_t)cpu->idle_poll_us * per_us;
        look = (uint64_t)IDLE_POLL_CHECK_US * per_us;
        (void)arch_atomic_xchg(&cpu->idle_poll, CPU_POLL_ON);
        for (;;) {
            now = arch_cycles() - start;
            if (cpu->idle_poll != CPU_POLL_ON || now >= limit) {
                break;
            }
            if (now >= look) {
                if (arch_irq_waiting()) {
                    break;
                }
                look = now + (uint64_t)IDLE_POLL_CHECK_US * per_us;
            }
            smp_service();
            arch_relax();
        }
        if (arch_atomic_xchg(&cpu->idle_poll, CPU_POLL_NONE) == CPU_POLL_KICKED) {
            arch_irq_enable(); /* `sti` lets a pending interrupt in after the return */
            arch_irq_disable();
            return;
        }
    }
    arch_idle_masked();
    gone = tick_div(arch_cycles() - start, per_us, NULL);
    if (gone > IDLE_POLL_MAX_US) {
        cpu->idle_poll_us /= 2u;
        if (cpu->idle_poll_us < IDLE_POLL_START_US) {
            cpu->idle_poll_us = 0;
        }
    } else if (gone > cpu->idle_poll_us) {
        grow = cpu->idle_poll_us * 2u;
        if (grow < IDLE_POLL_START_US) {
            grow = IDLE_POLL_START_US;
        }
        cpu->idle_poll_us = grow > IDLE_POLL_MAX_US ? IDLE_POLL_MAX_US : grow;
    }
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
 * that comes between the two is not lost either: the waker kicks this
 * processor (`smp_kick`), which ends a poll before the halt or else sends an
 * interrupt that is pending until the `sti` and ends the halt.
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
    this_cpu()->idle_cycles = arch_cycles();
    this_cpu()->idle_wait = 1;
    depth = bkl_drop();
    idle_halt();
    this_cpu()->idle_end_cycles = arch_cycles();
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

    this_cpu()->idle_cycles = arch_cycles();
    this_cpu()->idle_wait = 1;
    depth = bkl_drop();
    idle_halt();
    this_cpu()->idle_end_cycles = arch_cycles();
    bkl_take_as(depth, BKL_KEY_IDLE);
    this_cpu()->idle_wait = 0;
    kirq_restore(flags);
}
