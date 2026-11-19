/*
 * sched.c: the run queues and the selection, the idle wait, and the
 * accounting of the timer's ticks, as sched.h describes them.
 */
#include "../arch/arch.h"
#include "sched.h"
#include "../idle_timer.h"
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
#include "../lib/kerrno.h"
#include "../lib/kstring.h"
#include "fair.h"
#include "autogroup.h"
#include "balance.h"
#include "pelt.h"

#include "../lib/kirq.h"
#include "../lib/rcu.h"
#include "../lib/spinlock.h"
#include "../mm/pressure.h"
#include "smp.h"
#include "../usb/usb.h"
#include "../network/ip/ip.h"

/* Nonzero while the scheduler is on (`sched_enable`). */
static volatile int _sched_enabled = 0;

/* The big lock precedes this lock; interrupts cannot see partial links. */
static struct spinlock sched_lock = SPINLOCK_INIT_NAMED("sched_lock");

/* Each processor's list of ready real-time threads; the ordinary threads
 * are on its fair queue (fair.h, `fair_rqs`). */
struct ready_queue {
    process_t *head, *tail;
    unsigned int count;    /* ready records, excluding the current and idle tasks */
    unsigned int rt_count; /* queued threads with an effective RT priority */
};
static struct ready_queue run_queues[NR_CPUS];
/* The queued threads with an effective RT priority on every queue, the sum
 * of the queues' `rt_count`, for the yield that takes no lock
 * (`sched_yield_alone`). */
static unsigned int rt_queued;
static uint32_t pick_count, pick_steps; /* selection attempts and entries examined, modulo 2^32 */
/* Each processor's run-queue schedstats (`sched_rq_debug`), counted while
 * `fair_schedstats` is set, each by its own processor with interrupts off. */
static struct {
    unsigned int yld_count, sched_count, sched_goidle, ttwu_count, ttwu_local;
} rq_stats[NR_CPUS];

/* The ordinary threads a suspend to idle holds off their fair queues,
 * linked by `held_next` (`sched_hold`). */
static process_t *held_threads;

/* The entity a thread that no fork made, a kernel thread or a processor's
 * first, takes its own from (`sched_entity_init`): an ordinary thread at
 * nice 0 in the root group. */
static process_t kernel_parent;

/** Whether `p` belongs to the fair class: it has no effective real-time
 * priority. SCHED_DEADLINE, which has no class of its own, is ordinary. */
static int sched_fair_class(const process_t *p)
{
    return sched_priority(p) == 0;
}

/** The ordinary threads runnable on processor `c`, its current one
 * included and delayed ones not, as Linux's rq->cfs.h_nr_runnable. */
static unsigned int sched_fair_runnable(int c)
{
    return fair_rqs[c].cfs.h_nr_runnable;
}

/** Whether `p` is processor `c`'s current fair thread and still counted
 * runnable on its queue. */
static int sched_fair_running(int c, const process_t *p)
{
    return p && fair_rqs[c].curr == p && p->se.on_rq && !p->se.sched_delayed;
}

/** The ordinary threads waiting on processor `c`'s fair queue: its
 * runnable ones but the current one. Read without the lock by
 * `sched_yield_alone`, where it may be stale. */
static unsigned int sched_fair_waiting(int c)
{
    unsigned int n = sched_fair_runnable(c);

    return n > 0 && sched_fair_running(c, fair_rqs[c].curr) ? n - 1u : n;
}

/** The threads waiting on processor `c`, of both classes. */
static unsigned int sched_queued(int c)
{
    return run_queues[c].count + sched_fair_waiting(c);
}

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

/**
 * Place `p`, on no queue, on `cpu`, marking its rseq area for the return to
 * user mode. An ordinary thread's entity moves with it, its lag and load
 * (`fair_migrate`), as Linux's set_task_cpu calls migrate_task_rq_fair
 * (kernel/sched/core.c:3352-3395); a real-time thread's entity only points
 * at its group's queue there (`fair_set_task_rq`). The scheduler's lock is
 * held, with the clock of `p`'s processor given to the class.
 */
static void sched_move(process_t *p, int cpu)
{
    if (p->cpu != cpu) {
        if (!p->se.cfs_rq) {
            /* No entity yet: `sched_entity_init` points it at `cpu`. */
        } else if (sched_fair_class(p) && p != cpu_table[p->cpu].idle) {
            fair_migrate(p, cpu);
        } else {
            fair_set_task_rq(p, cpu);
        }
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
    rt_queued -= p->ready_rt;
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

/* Each processor's task clock, Linux's rq->clock_task: the monotonic clock
 * less the time its interrupts' handlers took and the time the hypervisor
 * stole, as `sched_clock_task` moves it, under sched_lock. */
struct task_clock {
    uint64_t mono;     /* the monotonic time it reaches, ns */
    uint64_t task;     /* the task clock, ns */
    uint64_t irq_ns;   /* the interrupts' time taken out, ns */
    uint64_t steal_ns; /* the steal time last read, ns */
    int started;       /* 1 once the clock has been read */
};
static struct task_clock task_clocks[NR_CPUS];

/** Processor `c`'s time in its interrupts' handlers in nanoseconds, its
 * `irq_cycles` read whole within `irq_seq` and converted by the measured
 * rate of the cycle counter; 0 before the rate is known. */
static uint64_t sched_irq_ns(int c)
{
    const struct percpu *cpu = &cpu_table[c];
    uint32_t seq, per_us = timer_calibrate() / TICK_US;
    uint64_t cycles;

    if (per_us == 0) {
        return 0;
    }
    do {
        seq = cpu->irq_seq;
        cycles = cpu->irq_cycles;
    } while ((seq & 1u) || cpu->irq_seq != seq);
    return tick_div64(cycles * 1000u, per_us, NULL);
}

/**
 * Move processor `c`'s task clock to now and give it and the monotonic
 * clock to the fair class (`fair_clock`), as Linux's update_rq_clock_task
 * (kernel/sched/core.c:746-801): the time since the last move, less the
 * interrupts' time and the steal time that came in it, each taken at most
 * to what is left, the interrupts' rest kept for the next move and the
 * steal time's dropped, as Linux keeps and drops them; the time taken out
 * goes to the interrupts' average (`pelt_update_irq`), as Linux's
 * update_irq_load_avg takes it. Returns the task clock. The run queues'
 * lock is held.
 */
static uint64_t sched_clock_task(int c)
{
    struct task_clock *tc = &task_clocks[c];
    uint64_t now = sched_clock_ns(), delta, irq, steal, cut, lost;

    if (!tc->started) {
        tc->started = 1;
        tc->mono = now;
        tc->irq_ns = sched_irq_ns(c);
        tc->steal_ns = arch_cpu_steal_ns(c);
        fair_clock(c, tc->mono, tc->task);
        return tc->task;
    }
    if (now <= tc->mono) {
        fair_clock(c, tc->mono, tc->task);
        return tc->task;
    }
    delta = now - tc->mono;
    tc->mono = now;
    irq = sched_irq_ns(c);
    cut = irq - tc->irq_ns;
    if (cut > delta) {
        cut = delta;
    }
    tc->irq_ns += cut;
    delta -= cut;
    lost = cut;
    steal = arch_cpu_steal_ns(c);
    cut = steal - tc->steal_ns;
    if (cut > delta) {
        cut = delta;
    }
    tc->steal_ns = steal;
    delta -= cut;
    lost += cut;
    tc->task += delta;
    fair_clock(c, tc->mono, tc->task);
    if (lost) {
        (void)pelt_update_irq(c, now, lost);
    }
    return tc->task;
}

/*
 * The fair server, Linux 6.12's deferred deadline server for the fair
 * class (kernel/sched/deadline.c:1629-1860), one per processor: a budget
 * of `runtime` nanoseconds of every `period`, 50 ms of 1000 ms, which lets
 * the ordinary threads queued there run ahead of real-time threads. Every
 * ordinary thread's run there spends the budget, and while the server
 * waits, a budget spent by the ordinary threads on their own starts a new
 * period, so that the server runs only when they starve: at the latest
 * moment at which the rest of the budget still fits in the period (its
 * zero laxity), and in every period after that until no ordinary thread is
 * queued. Under sched_lock; the deadlines are of the monotonic clock and
 * the budget is spent on the task clock.
 */
#define FAIR_SERVER_RUNTIME_NS 50000000ull
#define FAIR_SERVER_PERIOD_NS  1000000000ull
#define FAIR_SERVER_PERIOD_MIN 100000ull           /* 100 us, debug.c:342 */
#define FAIR_SERVER_PERIOD_MAX (4194304ull * 1000) /* 2^22 us, debug.c:341 */

enum fair_server_state {
    SERVER_IDLE,     /* no ordinary thread queued: Linux's init */
    SERVER_DEFER,    /* waiting for its zero laxity: zero_laxity-wait */
    SERVER_RUN,      /* may run ordinary threads ahead of real-time ones */
    SERVER_THROTTLED /* budget spent, waiting for the deadline: replenish-wait */
};

struct fair_server {
    uint64_t runtime, period; /* the parameters, ns; runtime 0 turns it off */
    int64_t left;             /* the budget left in this period, ns */
    uint64_t deadline;        /* the end of this period, monotonic ns */
    uint64_t mark;            /* the task clock its spending reaches, ns */
    int state;                /* enum fair_server_state */
    int ran;                  /* it has run since its last deferred period */
};
static struct fair_server fair_servers[NR_CPUS];

/* The real-time sysctls, Linux's: RR's slice in ticks and as written in
 * milliseconds, and the real-time period and runtime in microseconds,
 * which throttle nothing, as on a kernel without CONFIG_RT_GROUP_SCHED.
 * Written under sched_lock. */
static int rr_timeslice = SCHED_RR_TIMESLICE_MS * TICK_HZ / 1000;
static int rr_timeslice_ms = SCHED_RR_TIMESLICE_MS;
static int rt_period_us = 1000000;
static int rt_runtime_us = 950000;

/** Start a new period of the server at the monotonic time `now`, with its
 * whole budget, as Linux's replenish_dl_new_period. */
static void fair_server_new_period(struct fair_server *fs, uint64_t now)
{
    fs->left = (int64_t)fs->runtime;
    fs->deadline = now + fs->period;
}

/** Whether the budget left would run at a higher rate than the server's
 * own over what is left of the period, as Linux's dl_entity_overflow
 * compares them, each value scaled down by 2^10 so that the products stay
 * within 64 bits. */
static int fair_server_overflow(const struct fair_server *fs, uint64_t now)
{
    uint64_t left = fs->left > 0 ? (uint64_t)fs->left : 0;

    return (left >> 10) * (fs->period >> 10) > (fs->runtime >> 10) * ((fs->deadline - now) >> 10);
}

/**
 * Bring server `fs` to the monotonic time `now`: a period that has ended
 * gives a running or throttled server its next budget, the deadline a
 * period on, or a period from now when it is further behind, as
 * replenish_dl_entity; and a deferred server reaches its zero laxity when
 * what is left of the period is no more than its budget.
 */
static void fair_server_advance(struct fair_server *fs, uint64_t now)
{
    if (fs->state == SERVER_IDLE || fs->runtime == 0) {
        return;
    }
    if (now >= fs->deadline) {
        if (fs->state == SERVER_DEFER || now >= fs->deadline + fs->period) {
            fair_server_new_period(fs, now);
        } else {
            fs->deadline += fs->period;
            fs->left = (int64_t)fs->runtime;
        }
        if (fs->state == SERVER_THROTTLED) {
            fs->state = SERVER_RUN;
        }
    }
    if (fs->state == SERVER_DEFER && now + (uint64_t)fs->left >= fs->deadline) {
        fs->state = SERVER_RUN;
        fs->ran = 1;
    }
}

/**
 * Spend processor `c`'s server budget by the time charged to its ordinary
 * threads since the last spend (`charged_ns`), as Linux's update_curr
 * charges every fair task's run to the fair server (kernel/sched/fair.c:
 * 1240-1250). A deferred server whose budget the ordinary threads spent on
 * their own needs no turn in this period and starts a new one; a running
 * one is throttled until its deadline. The run queues' lock is held.
 */
static void fair_server_spend(int c)
{
    struct fair_server *fs = &fair_servers[c];
    uint64_t delta = fair_rqs[c].charged_ns - fs->mark;

    fs->mark = fair_rqs[c].charged_ns;
    if (delta == 0 || fs->state == SERVER_IDLE || fs->runtime == 0) {
        return;
    }
    fs->left -= (int64_t)delta;
    if (fs->left > 0) {
        return;
    }
    if (fs->state == SERVER_DEFER) {
        fs->ran = 0;
        fair_server_new_period(fs, sched_clock_ns());
    } else if (fs->state == SERVER_RUN) {
        fs->state = SERVER_THROTTLED;
    }
}

/**
 * Start processor `c`'s server when `runnable`, the ordinary threads
 * runnable there, the current one included, is not 0, and stop it when it
 * is, as Linux's enqueue_task_fair and dequeue_entities start and stop it
 * (kernel/sched/fair.c:7118, 6163; deadline.c:1829-1870). A start whose
 * period has ended, or whose budget would overflow it, starts a new period
 * and waits for its zero laxity; one that has run since its last deferred
 * period runs at once.
 */
static void fair_server_track(int c, unsigned int runnable)
{
    struct fair_server *fs = &fair_servers[c];
    uint64_t now;

    if (runnable == 0) {
        fs->state = SERVER_IDLE;
        return;
    }
    if (fs->state != SERVER_IDLE || fs->runtime == 0) {
        return;
    }
    now = sched_clock_ns();
    fs->mark = fair_rqs[c].charged_ns;
    if (now >= fs->deadline || fair_server_overflow(fs, now)) {
        fs->ran = 0;
        fair_server_new_period(fs, now);
    }
    fs->state = fs->ran ? SERVER_RUN : SERVER_DEFER;
}

/** Whether processor `c`'s server runs its ordinary threads now ahead of
 * `rt`, the real-time thread selection would take: it has budget and has
 * reached its turn, and `rt` does not run on a priority a futex lends it,
 * which no budget holds back. The run queues' lock is held. */
static int fair_server_takes(int c, const process_t *rt)
{
    struct fair_server *fs = &fair_servers[c];

    fair_server_advance(fs, sched_clock_ns());
    return fs->state == SERVER_RUN && fs->left > 0 && rt->pi_priority <= rt->rt_priority;
}

/** The monotonic time at which processor `c`'s server next changes what
 * selection takes while a real-time thread runs and ordinary threads
 * wait: its zero laxity, its deadline when throttled, or the end of its
 * budget when it runs; 0 for none. The run queues' lock is held. */
static uint64_t fair_server_event(int c)
{
    struct fair_server *fs = &fair_servers[c];

    switch (fs->state) {
    case SERVER_DEFER:
        return fs->deadline - (uint64_t)fs->left;
    case SERVER_THROTTLED:
        return fs->deadline;
    case SERVER_RUN:
        return sched_clock_ns() + (uint64_t)(fs->left > 0 ? fs->left : 0);
    default:
        return 0;
    }
}

/* Linux's BW_SHIFT and BW_UNIT: a bandwidth is a runtime over its period
 * in units of 2^-20 of a processor. */
#define SCHED_BW_SHIFT 20
#define SCHED_BW_UNIT  ((uint64_t)1 << SCHED_BW_SHIFT)

/** Linux's to_ratio (kernel/sched/core.c): `runtime` over `period`, both
 * in nanoseconds, in units of 2^-20; 0 for a period of 0. The runtime is
 * below 2^44 ns, so the shift stays within 64 bits. */
static uint64_t sched_bw_ratio(uint64_t period, uint64_t runtime)
{
    return period == 0 ? 0 : tick_div64(runtime << SCHED_BW_SHIFT, period, NULL);
}

/** The bandwidth the real-time sysctls leave the deadline class, Linux's
 * dl_b->bw: the runtime over the period, or -1 for an unlimited runtime. */
static int64_t sched_rt_ratio(int period_us, int runtime_us)
{
    if (runtime_us < 0) {
        return -1;
    }
    return (int64_t)sched_bw_ratio((uint64_t)(uint32_t)period_us * 1000u,
                                   (uint64_t)(uint32_t)runtime_us * 1000u);
}

/** The fair servers' bandwidth summed over the online processors, and in
 * `cpus` their count: Linux's total_bw and dl_bw_cpus of the one root
 * domain, to which each processor's server adds its share at its first
 * start (dl_server_apply_params, kernel/sched/deadline.c:1895-1920). The
 * run queues' lock is held. */
static uint64_t sched_servers_bw(int *cpus)
{
    uint64_t total = 0;
    int c;

    *cpus = 0;
    for (c = 0; c < NR_CPUS; c++) {
        if (cpu_table[c].online) {
            ++*cpus;
            total += sched_bw_ratio(fair_servers[c].period, fair_servers[c].runtime);
        }
    }
    return total;
}

void sched_fair_server(int c, uint64_t *runtime, uint64_t *period)
{
    uint32_t flags = spin_lock_irqsave(&sched_lock);

    *runtime = fair_servers[c].runtime;
    *period = fair_servers[c].period;
    spin_unlock_irqrestore(&sched_lock, flags);
}

/** Linux's sched_fair_server_write (kernel/sched/debug.c:344-402): the
 * runtime at most the period, the period from 100 us to 2^22 us, the
 * processor online, and the servers' bandwidth with the new one within
 * what the real-time sysctls leave the deadline class on the online
 * processors (__dl_overflow in dl_server_apply_params,
 * kernel/sched/deadline.c:1895-1915); the server stops and starts again
 * with its new parameters, and a runtime of 0 turns it off. */
int sched_set_fair_server(int c, uint64_t runtime, uint64_t period)
{
    struct fair_server *fs;
    uint64_t total;
    int64_t bw;
    uint32_t flags;
    int cpus;

    if (runtime > period || period > FAIR_SERVER_PERIOD_MAX || period < FAIR_SERVER_PERIOD_MIN) {
        return -EINVAL;
    }
    if (c < 0 || c >= NR_CPUS || !cpu_table[c].online) {
        return -EBUSY;
    }
    fs = &fair_servers[c];
    flags = spin_lock_irqsave(&sched_lock);
    bw = sched_rt_ratio(rt_period_us, rt_runtime_us);
    total = sched_servers_bw(&cpus) - sched_bw_ratio(fs->period, fs->runtime);
    if (bw >= 0 && (uint64_t)bw * (uint32_t)cpus < total + sched_bw_ratio(period, runtime)) {
        spin_unlock_irqrestore(&sched_lock, flags);
        return -EBUSY;
    }
    fs->runtime = runtime;
    fs->period = period;
    fs->state = SERVER_IDLE;
    fs->ran = 0;
    fs->deadline = 0;
    fair_server_track(c, sched_fair_runnable(c));
    spin_unlock_irqrestore(&sched_lock, flags);
    if (runtime == 0) {
        klog(KLOG_WARN, "sched", "fair server: disabled on processor %d, which may starve", c);
    }
    return 0;
}

/* Set while a suspend to idle holds every thread but the suspender: no wait
 * on a run queue is counted, as Linux's frozen tasks leave their queues. */
static int waits_held;

static int sched_wake_preempt(process_t *p, int cpu, int wf);

/** Start counting the wait of `p`, ready and not running, on a run queue,
 * as Linux's sched_info_enqueue marks it: a move from one queue to another
 * keeps the moment its wait began, and a wait begun before the scheduler is
 * on, which has no clock to read, or while `waits_held` is set, is not
 * counted. With sched_lock held. */
static void sched_wait_begin(process_t *p)
{
    if (!p->run_queued && _sched_enabled && !waits_held) {
        p->run_queued = 1;
        p->queued_ns = sched_clock_ns();
    }
    waketime_state(p, 1, p->cpu);
}

/** Link a noncurrent ready real-time thread once on its processor's list,
 * at its head when `head` is set and at its tail otherwise, with sched_lock
 * held. */
static void sched_link(process_t *p, int head)
{
    struct ready_queue *q = &run_queues[p->cpu];

    if (p->ready_linked || p == cpu_table[p->cpu].idle) {
        return;
    }
    sched_wait_begin(p);
    if (head) {
        p->ready_prev = NULL;
        p->ready_next = q->head;
        if (q->head) {
            q->head->ready_prev = p;
        } else {
            q->tail = p;
        }
        q->head = p;
    } else {
        p->ready_prev = q->tail;
        p->ready_next = NULL;
        if (q->tail) {
            q->tail->ready_next = p;
        } else {
            q->head = p;
        }
        q->tail = p;
    }
    q->count++;
    p->ready_rt = sched_priority(p) != 0;
    q->rt_count += p->ready_rt;
    rt_queued += p->ready_rt;
    p->ready_linked = 1;
}

/** Put the ordinary thread `p` on the list of those a suspend to idle
 * holds, once. With sched_lock held. */
static void sched_hold(process_t *p)
{
    if (!p->sched_held) {
        p->sched_held = 1;
        p->held_next = held_threads;
        held_threads = p;
    }
}

/** Take `p` off the held list if it is on it. With sched_lock held. */
static void sched_unhold(process_t *p)
{
    process_t **link;

    if (!p->sched_held) {
        return;
    }
    for (link = &held_threads; *link && *link != p; link = &(*link)->held_next) {
    }
    if (*link) {
        *link = p->held_next;
    }
    p->held_next = NULL;
    p->sched_held = 0;
}

/**
 * Give `p`, which no fork made, its entity: a kernel thread's or a
 * processor's first thread's, taken from `kernel_parent`, an ordinary
 * thread at nice 0 in the root group, as Linux's kernel threads are forked
 * from kthreadd. A thread a fork made has its entity from `sched_fork`.
 * The run time a real-time thread was charged before (`sched_charge_rt`)
 * is kept. With sched_lock held.
 */
static void sched_entity_init(process_t *p)
{
    uint64_t ran = p->se.sum_exec_runtime;

    if (p->se.cfs_rq) {
        return;
    }
    kernel_parent.cpu = p->cpu;
    fair_set_task_rq(&kernel_parent, p->cpu);
    fair_fork(p, &kernel_parent);
    fair_set_task_rq(p, p->cpu);
    p->se.sum_exec_runtime = ran;
}

/**
 * Put `p`, ready and not current, on its processor's queue of its class: a
 * real-time thread on its list, at the head when `head` is set, as Linux
 * keeps a preempted one's place, else at the tail; an ordinary one on its
 * fair queue for `why`, FAIR_ENQ_* (`fair_enqueue`), a delayed one
 * requeued in place, or on the held list while a suspend to idle holds it.
 * The class has the clock of `p`'s processor; sched_lock is held.
 */
static void sched_enqueue(process_t *p, int why, int head)
{
    if (p == cpu_table[p->cpu].idle) {
        return;
    }
    if (!sched_fair_class(p)) {
        sched_link(p, head);
        return;
    }
    if (waits_held && !suspend_lets_run(p)) {
        if (p->se.on_rq) {
            (void)fair_dequeue(p, FAIR_DEQ_FORCED);
            fair_server_track(p->cpu, sched_fair_runnable(p->cpu));
        }
        sched_hold(p);
        waketime_state(p, 1, p->cpu);
        return;
    }
    sched_entity_init(p);
    if (!p->se.on_rq || p->se.sched_delayed) {
        fair_enqueue(p, why);
        fair_server_track(p->cpu, sched_fair_runnable(p->cpu));
    }
    sched_wait_begin(p);
}

/**
 * Take `p` off every queue it is on: its real-time list, the held list and
 * its fair queue, for `why`, a FAIR_DEQ_* (`fair_dequeue`). Returns 0 when
 * the fair dequeue is delayed, so that `p` stays queued, never running,
 * until a wake requeues it or a pick finishes the dequeue, and 1 otherwise.
 * The class has the clock of `p`'s processor; sched_lock is held.
 */
static int sched_dequeue(process_t *p, int why)
{
    int gone = 1;

    sched_remove(p);
    sched_unhold(p);
    if (p->se.on_rq) {
        gone = fair_dequeue(p, why);
        fair_server_track(p->cpu, sched_fair_runnable(p->cpu));
    }
    return gone;
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

static int sched_resched_cur(process_t *cur, int cpu);

/** Ask the thread running on each processor, other than an idle task and
 * the suspender, to switch away, so that a suspend to idle's hold takes it
 * at its switch (`sched_prepare_old`). Returns the mask of processors to
 * kick. sched_lock is held. */
static uint64_t sched_hold_running(void)
{
    uint64_t kicks = 0;
    process_t *p;
    int c, kick;

    for (c = 0; c < NR_CPUS; c++) {
        p = cpu_table[c].current;
        if (p && p != cpu_table[c].idle && !suspend_lets_run(p)) {
            kick = sched_resched_cur(p, c);
            if (kick >= 0) {
                kicks |= (uint64_t)1 << kick;
            }
        }
    }
    return kicks;
}

/**
 * A hold takes every ordinary thread but the suspender that is ready and
 * not running off its fair queue (FAIR_DEQ_FORCED) onto the held list, and
 * finishes the dequeue of every delayed one, as Linux's frozen tasks sleep
 * in a state that is never delayed; one running elsewhere is asked to
 * switch, as Linux's freezer sends every task a fake signal
 * (kernel/freezer.c, freeze_task), and joins the list at its switch away
 * (`schedule`), and one woken meanwhile at its wake (`sched_enqueue`).
 * Real-time threads stay on their lists, where selection passes over them
 * (`sched_may_run`). The end of the hold puts every held thread back as a
 * wake does, and asks its processor to select. Each ready thread's wait so
 * far is counted at the start, and starts again at the end. The caller
 * holds the big lock, which keeps `proc_list` still.
 */
void sched_hold_waits(int hold)
{
    uint32_t flags = spin_lock_irqsave(&sched_lock);
    uint64_t now = sched_clock_ns(), kicks = 0;
    process_t *p;
    int kick;

    waits_held = hold;
    for (p = proc_list; p; p = p->next) {
        if (p == cpu_table[p->cpu].idle || cpu_table[p->cpu].current == p) {
            continue;
        }
        if (hold && p->se.on_rq && sched_fair_class(p) && !suspend_lets_run(p)) {
            (void)sched_clock_task(p->cpu);
            if (p->se.sched_delayed) {
                (void)sched_dequeue(p, FAIR_DEQ_FORCED);
            } else if (p->state == PROC_READY) {
                (void)sched_dequeue(p, FAIR_DEQ_FORCED);
                sched_hold(p);
            }
        }
        if (p->state != PROC_READY) {
            continue;
        }
        if (hold && p->run_queued) {
            p->run_queued = 0;
            p->run_delay += now - p->queued_ns;
        } else if (!hold && !p->run_queued && _sched_enabled) {
            p->run_queued = 1;
            p->queued_ns = now;
        }
    }
    if (hold) {
        kicks = sched_hold_running();
    }
    while (!hold && held_threads) {
        p = held_threads;
        sched_unhold(p);
        (void)sched_clock_task(p->cpu);
        sched_enqueue(p, FAIR_ENQ_WAKE, 0);
        kick = sched_wake_preempt(p, p->cpu, FAIR_WF_TTWU);
        if (kick >= 0) {
            kicks |= (uint64_t)1 << kick;
        }
    }
    spin_unlock_irqrestore(&sched_lock, flags);
    if (kicks) {
        smp_kick_many(kicks);
    }
}

void sched_forget(process_t *p)
{
    uint32_t flags = spin_lock_irqsave(&sched_lock);

    if (p->se.on_rq) {
        (void)sched_clock_task(p->cpu);
    }
    (void)sched_dequeue(p, FAIR_DEQ_FORCED);
    spin_unlock(&sched_lock);
    affinity_complete(p);
    kirq_restore(flags);
}

/**
 * The record must name the processor it is current on: a switch away
 * queues it on that processor (`sched_enqueue`), and a wake before the
 * switch keeps it there (`sched_ready_locked`); and it runs here, so
 * another processor's switch to it waits for its own switch away
 * (`on_cpu`). Linux's init_idle sets an idle task's processor in the same
 * hold of the run queue's lock that makes it the queue's current
 * (kernel/sched/core.c:7782-7786). An ordinary `p` becomes the fair
 * class's current thread here, in place of the one that was.
 */
void sched_adopt(process_t *p)
{
    uint32_t flags = spin_lock_irqsave(&sched_lock);
    int me = (int)arch_cpu_id();
    process_t *was = fair_rqs[me].curr;

    if (p->se.on_rq) {
        (void)sched_clock_task(p->cpu);
    }
    (void)sched_dequeue(p, FAIR_DEQ_MOVE);
    (void)sched_clock_task(me);
    if (was && was != p) {
        fair_put_prev(was);
    }
    sched_move(p, me);
    p->state = PROC_RUNNING;
    p->on_cpu = 1;
    cpu_table[me].current = p;
    if (sched_fair_class(p) && p != cpu_table[me].idle) {
        sched_entity_init(p);
        fair_enqueue(p, FAIR_ENQ_RESTORE);
        fair_set_next(p);
        fair_server_track(me, sched_fair_runnable(me));
    }
    spin_unlock_irqrestore(&sched_lock, flags);
}

/**
 * Left queued, process 0 would be picked at the program's first sleep and
 * switched to on a context it never saved, since it never switched away.
 */
void sched_disown(process_t *p)
{
    uint32_t flags = spin_lock_irqsave(&sched_lock);
    int me = (int)arch_cpu_id();

    if (p->se.on_rq) {
        (void)sched_clock_task(p->cpu);
    }
    (void)sched_dequeue(p, FAIR_DEQ_FORCED);
    if (fair_rqs[me].curr == p) {
        fair_put_prev(p);
    }
    p->on_cpu = 0;
    spin_unlock_irqrestore(&sched_lock, flags);
}

/**
 * The new thread joins its thread group's autogroup (`autogroup_fork`)
 * and takes its entity from its parent's (`fair_fork`), its weight, slice
 * and averages, with its queue in its group on the processor it starts on,
 * as Linux's sched_fork and sched_cgroup_fork (kernel/sched/core.c:
 * 4730-4840). Its RR slice is whole.
 */
void sched_fork(process_t *child, process_t *parent)
{
    uint32_t flags;

    autogroup_fork(child, parent);
    flags = spin_lock_irqsave(&sched_lock);
    child->rr_used = 0;
    (void)sched_clock_task(parent->cpu);
    fair_fork(child, parent);
    fair_set_task_rq(child, child->cpu);
    spin_unlock_irqrestore(&sched_lock, flags);
}

void sched_set_slice(process_t *p, uint64_t ns)
{
    uint32_t flags = spin_lock_irqsave(&sched_lock);

    if (p->se.cfs_rq) {
        (void)sched_clock_task(p->cpu);
        fair_set_slice(p, ns);
    }
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

static void sched_balance_start(void);
static void sched_group_clock(int cpu);

/**
 * The fair class is set up for every processor the kernel may start, with
 * the base slice of one processor; `sched_cache_join` scales it as each
 * other one comes online, as Linux's sched_init_smp sets it from the
 * processors online (update_sysctl, kernel/sched/fair.c:191-220). The root
 * group, PELT's hooks and the balance's operations follow it, and a group
 * change charges by this file's clocks (`fair_group_clock`). Each
 * processor's fair server has Linux's 50 ms of every second.
 */
void init_sched(void)
{
    int c;

    if (current_proc) {
        current_proc->on_cpu = 1;
    }
    _sched_enabled = 0;
    waits_held = 0;
    held_threads = NULL;
    pick_count = pick_steps = 0;
    rt_queued = 0;
    fair_init(NR_CPUS);
    fair_group_init();
    pelt_init();
    sched_balance_start();
    fair_group_clock = sched_group_clock;
    sysctl_sched_base_slice = FAIR_BASE_SLICE_NS;
    k_memset(&kernel_parent, 0, sizeof(kernel_parent));
    kernel_parent.sched_policy = SCHED_OTHER;
    for (c = 0; c < NR_CPUS; c++) {
        run_queues[c].head = run_queues[c].tail = NULL;
        run_queues[c].count = run_queues[c].rt_count = 0;
        k_memset(&task_clocks[c], 0, sizeof(task_clocks[c]));
        k_memset(&fair_servers[c], 0, sizeof(fair_servers[c]));
        fair_servers[c].runtime = FAIR_SERVER_RUNTIME_NS;
        fair_servers[c].period = FAIR_SERVER_PERIOD_NS;
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
 * Whether processor `cpu` is idle with nothing on its way to it: it runs
 * its idle task, its queue is empty and no wake has asked it to select
 * (its idle task's `need_resched`), as Linux's idle_cpu requires an empty
 * queue and no pending wake besides the idle task
 * (kernel/sched/syscalls.c:202-218). Two wakes from one interrupt then go
 * to two processors, where the first one's queued thread left its
 * processor counted idle for the second. The run queues' lock is held.
 */
static int sched_cpu_free(int cpu)
{
    const process_t *idle = cpu_table[cpu].idle;

    return idle && cpu_table[cpu].current == idle && sched_queued(cpu) == 0 && !idle->need_resched;
}

/**
 * The scheduling domains are built again over the processors online each
 * time one starts, as Linux's sched_cpu_activate rebuilds them, from the
 * masks each records as it comes online (`balance_cpu_topology`): the boot
 * processor, which never calls this, learns its siblings from theirs.
 */
void sched_cache_join(uint64_t shared, uint64_t core)
{
    uint32_t flags = spin_lock_irqsave(&sched_lock);
    uint64_t me_bit, online = 0;
    int me = (int)arch_cpu_id(), c, n = 0, factor = 1;

    for (c = 0; c < NR_CPUS; c++) {
        if (cpu_table[c].online) {
            online |= (uint64_t)1 << c;
            n++;
        }
    }
    /* The base slice of the processors online: 1 + log2(min(n, 8)). */
    for (n = n < 8 ? n : 8; n > 1; n >>= 1) {
        factor++;
    }
    sysctl_sched_base_slice = (uint64_t)FAIR_BASE_SLICE_NS * (uint32_t)factor;
    me_bit = (uint64_t)1 << me;
    balance_cpu_topology(me, core | me_bit, shared | core | me_bit);
    balance_build_domains(online);
    spin_unlock_irqrestore(&sched_lock, flags);
}

/*
 * What the balance asks of the scheduler (`balance_ops`), each with
 * sched_lock held. `balance_active` is set for a processor the balance
 * asked to push a thread away (BALANCE_KICK_ACTIVE), which it does at its
 * next selection; `balance_stopped` is set on a processor while it does,
 * where Linux's stopper thread would be current, so that the thread that
 * ran there may move.
 */
static uint8_t balance_active[NR_CPUS];
static uint8_t balance_stopped[NR_CPUS];

/** The threads on processor `cpu`'s queues, a running one included and the
 * idle task left out (rq->nr_running). */
static unsigned int sched_balance_nr(int cpu)
{
    const process_t *cur = cpu_table[cpu].current;
    unsigned int n = run_queues[cpu].count + fair_rqs[cpu].cfs.h_nr_queued;

    if (cur && cur != cpu_table[cpu].idle && !sched_fair_class(cur)) {
        n++;
    }
    return n;
}

/** The thread running on processor `cpu`, NULL for its idle task or while
 * it pushes its thread away. */
static process_t *sched_balance_curr(int cpu)
{
    process_t *cur = cpu_table[cpu].current;

    if (!cur || cur == cpu_table[cpu].idle || balance_stopped[cpu]) {
        return NULL;
    }
    return cur;
}

/** Move `p`, queued and not running, to processor `to`, as detach_task and
 * attach_task do, and ask `to` to select if `p` should run there now. */
static void sched_balance_move(process_t *p, int to)
{
    int kick;

    (void)sched_dequeue(p, FAIR_DEQ_MOVE);
    sched_move(p, to);
    (void)sched_clock_task(to);
    sched_enqueue(p, FAIR_ENQ_MIGRATED, 0);
    kick = sched_wake_preempt(p, to, 0);
    if (kick >= 0) {
        smp_kick(kick);
    }
}

/** Ask processor `cpu` to run the balance's code: an idle one's loop runs
 * `fair_nohz_balance` (`sched_idle_balance`), and a busy one pushes a
 * thread away at its next selection (`sched_balance_push`). */
static void sched_balance_kick(int cpu, int why)
{
    process_t *cur = cpu_table[cpu].current;

    if (why == BALANCE_KICK_ACTIVE) {
        balance_active[cpu] = 1;
        if (cur && cur != cpu_table[cpu].idle && sched_resched_cur(cur, cpu) < 0) {
            return;
        }
    }
    smp_kick(cpu);
}

/** The monotonic clock in nanoseconds. */
static uint64_t sched_balance_clock(void)
{
    return sched_clock_ns();
}

/** Give processor `cpu` its clocks as of now. */
static void sched_group_clock(int cpu)
{
    (void)sched_clock_task(cpu);
}

static const struct balance_ops sched_balance_ops = {
    sched_balance_nr,   sched_balance_curr,  sched_cpu_free,    sched_balance_move,
    sched_balance_kick, sched_balance_clock, sched_group_clock,
};

/** Give the balance its operations, at boot. */
static void sched_balance_start(void)
{
    k_memset(balance_active, 0, sizeof(balance_active));
    k_memset(balance_stopped, 0, sizeof(balance_stopped));
    balance_init(&sched_balance_ops);
}

/**
 * Push a thread from processor `me`, which the balance asked to, now that
 * `old` stops running there, as Linux's stopper thread runs
 * active_load_balance_cpu_stop (kernel/sched/fair.c:11718-11800): an
 * ordinary `old` is put back on its queue and counted ready, so that it may
 * be the one moved; if it stays, it runs on. The run queues' lock is held.
 */
static void sched_balance_push(int me, process_t *old)
{
    int ran = old != cpu_table[me].idle && old->state == PROC_RUNNING && sched_fair_class(old);

    if (!balance_active[me]) {
        return;
    }
    balance_active[me] = 0;
    if (ran) {
        if (fair_rqs[me].curr == old) {
            fair_put_prev(old);
        }
        old->state = PROC_READY;
    }
    balance_stopped[me] = 1;
    fair_balance_active(me);
    balance_stopped[me] = 0;
    if (ran && old->cpu == me) {
        old->state = PROC_RUNNING;
    }
}

/** On processor `me`'s way into its idle wait: the idle balance it was
 * kicked for, or the decay a newly idle balance left it
 * (`fair_nohz_balance`), whatever that pulls selected at once. */
static void sched_idle_balance(void)
{
    uint32_t flags = spin_lock_irqsave(&sched_lock);
    int me = (int)arch_cpu_id();

    if (cpu_table[me].current == cpu_table[me].idle) {
        (void)sched_clock_task(me);
        fair_nohz_balance(me);
    }
    spin_unlock_irqrestore(&sched_lock, flags);
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
 * The processor a ready process of `p`'s affinity joins when it is moved
 * off one it may no longer run on: the one it ran on last, unless that one
 * is busy and another it may run on is idle with nothing on its way
 * (`sched_cpu_free`), where it then goes, as Linux's affine_move_task
 * takes any processor of the new mask (kernel/sched/core.c:3105-3170). The
 * run queues' lock is held.
 */
static int sched_choose(const process_t *p)
{
    int last = p->cpu;
    int c;

    if (sched_allowed(p, last) && sched_cpu_free(last)) {
        return last;
    }
    for (c = 0; c < NR_CPUS; c++) {
        if (sched_allowed(p, c) && sched_cpu_free(c)) {
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
 * Ask processor `cpu`, on whose queue `p` has just been put, to select now
 * when it should: return `cpu` when it is idle, or when `p` outranks the
 * thread it runs, whose `need_resched` is then set, as Linux's
 * wakeup_preempt calls resched_curr for a task of a higher class, and
 * wakeup_preempt_rt for a real-time task of a higher priority
 * (kernel/sched/core.c:2151-2156, rt.c:1657-1658); return -1 otherwise.
 * The interrupt switches in user mode only on that request
 * (`isr_local_router`), and a request to the caller's own processor, whose
 * interrupt does nothing, is taken at the call's exit (`sched_take_preempt`).
 *
 * An idle processor is asked once: its idle task's `need_resched` marks the
 * request, and a later wake before its selection, which clears it, asks
 * nothing, as Linux's resched_curr sends no interrupt to a task whose
 * TIF_NEED_RESCHED is set (kernel/sched/core.c:1101-1102). The selection
 * takes every thread queued meanwhile. Under KVM an interrupt takes
 * microseconds to reach a halted vCPU, and a second one came during the
 * switch the first had started, with interrupts off.
 * The run queues' lock is held.
 */
static int sched_preempt_cpu(const process_t *p, int cpu)
{
    process_t *cur = cpu_table[cpu].current;

    if (sched_cpu_idle(cpu)) {
        if (cur) {
            if (cur->need_resched) {
                return -1;
            }
            cur->need_resched = 1;
        }
        return cpu;
    }
    if (sched_priority(p) > sched_priority(cur)) {
        cur->need_resched = 1;
        proc_work_set(cur, WORK_RESCHED);
        return cpu;
    }
    return -1;
}

/** Ask the thread `cur`, running on processor `cpu`, to switch, and return
 * the processor to kick: none for a kernel thread, which takes the request
 * at its next sleep or service point. The run queues' lock is held. */
static int sched_resched_cur(process_t *cur, int cpu)
{
    if (cur->need_resched) {
        return -1;
    }
    cur->need_resched = 1;
    proc_work_set(cur, WORK_RESCHED);
    return cur->no_tick_switch ? -1 : cpu;
}

/** Take the fair class's request that processor `cpu` select again
 * (`fair_resched`), made by a hook on its queue, as Linux's resched_curr
 * from the class: ask its current thread, as `sched_resched_cur` does, or
 * an idle processor's idle task. Return the processor to kick, or -1. The
 * run queues' lock is held. */
static int sched_fair_kick(int cpu)
{
    process_t *cur = cpu_table[cpu].current;

    if (!fair_resched(cpu) || !cur) {
        return -1;
    }
    if (sched_cpu_idle(cpu)) {
        if (cur->need_resched) {
            return -1;
        }
        cur->need_resched = 1;
        return cpu;
    }
    return sched_resched_cur(cur, cpu);
}

/**
 * Ask processor `cpu`, on whose queue `p` has just been woken or started,
 * or moved by an affinity change, as Linux's move_queued_task calls
 * wakeup_preempt (kernel/sched/core.c:2449-2465), to select now, as
 * `sched_preempt_cpu` asks it, and besides when `p` and the thread running
 * there are both ordinary and EEVDF would pick `p` at the queue the two
 * share (`fair_wake_preempt`, Linux's check_preempt_wakeup_fair,
 * kernel/sched/fair.c:8862-8951), for the wake's reason `wf`, FAIR_WF_*.
 * A thread already asked to select is not asked again, since that request
 * has sent its interrupt.
 *
 * The request is the thread's `need_resched`: a thread in user mode
 * switches in the interrupt the kick sends (`isr_local_router`), one inside
 * a call at the call's exit, and a kernel thread at its next service point
 * or sleep (`sched_service_point`), where Linux's kernel thread without
 * CONFIG_PREEMPTION takes TIF_NEED_RESCHED at cond_resched or schedule;
 * so a kernel thread is sent no kick, whose interrupt would switch nothing.
 * Return the processor to kick, or -1. The class has the clock of `cpu`;
 * the run queues' lock is held.
 */
static int sched_wake_preempt(process_t *p, int cpu, int wf)
{
    process_t *cur = cpu_table[cpu].current;
    int kick = sched_preempt_cpu(p, cpu);

    if (kick >= 0 || sched_cpu_idle(cpu) || cur->need_resched || !sched_fair_class(p) ||
        !sched_fair_class(cur)) {
        return kick;
    }
    if (p->sched_held || !p->se.on_rq || fair_rqs[cpu].curr != cur ||
        !fair_wake_preempt(cur, p, wf)) {
        return sched_fair_kick(cpu);
    }
    (void)fair_resched(cpu);
    return sched_resched_cur(cur, cpu);
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

/* The wake reason's bit for a wake that moved the thread to another
 * processor, Linux's WF_MIGRATED, beside fair.h's FAIR_WF_ bits. */
#define SCHED_WF_MIGRATED 0x20

/** Count the wake of `p` onto processor `cpu`, made on this processor with
 * the reason `wf`, in the thread's and this processor's schedstats, as
 * Linux's ttwu_stat (kernel/sched/core.c:3633-3669) does for every wake but
 * a new thread's first. With sched_lock held, while `fair_schedstats` is
 * set. */
static void sched_ttwu_stat(process_t *p, int cpu, int wf)
{
    int me = (int)arch_cpu_id();

    if (!fair_schedstats || (wf & FAIR_WF_FORK)) {
        return;
    }
    if (cpu == me) {
        rq_stats[me].ttwu_local++;
        p->se.stats.nr_wakeups_local++;
    } else {
        p->se.stats.nr_wakeups_remote++;
    }
    if (wf & SCHED_WF_MIGRATED) {
        p->se.stats.nr_wakeups_migrate++;
    }
    rq_stats[me].ttwu_count++;
    p->se.stats.nr_wakeups++;
    if (wf & FAIR_WF_SYNC) {
        p->se.stats.nr_wakeups_sync++;
    }
}

/**
 * Make `p` ready and return the processor to interrupt so that it selects
 * now, as `sched_wake_preempt` gives it, or the one where `p` is halted in
 * place as current; -1 when none needs it. `wf` is the reason, FAIR_WF_*:
 * FAIR_WF_FORK for a thread never yet run, which is placed as Linux's
 * wake_up_new_task places it, half a slice ahead (FAIR_ENQ_NEW), and
 * FAIR_WF_TTWU for a wake. `sched_lock` is held.
 *
 * An ordinary thread goes to the processor Linux's select_task_rq_fair
 * gives it (`fair_select_cpu`), its entity moving there with its lag and
 * load (`sched_move`); a processor idle since its newly idle balance folds
 * that idle time into its average (`fair_balance_woken`). A real-time
 * thread stays where it ran, unless its affinity has left that processor,
 * or goes to a processor running and holding lower work when it does not
 * outrank the real-time thread running there, or that thread may run
 * nowhere else (`sched_rt_place`), as select_task_rq_rt
 * (kernel/sched/rt.c:1535-1600).
 *
 * A `p` still current on its processor, between its marking as blocked and
 * its switch away, finds itself ready at its own selection, which takes
 * this lock, and runs on, as Linux's ttwu_runnable makes a task that has
 * not left its queue running with no interrupt (kernel/sched/core.c:
 * 3756-3780). It halts in place as current only where its processor has no
 * idle task to switch to or the scheduler is off, and only then is it
 * kicked: a kick otherwise came during that processor's switch, with its
 * interrupts off, and cost an interrupt-window exit under KVM besides the
 * command write and the interrupt. A thread whose dequeue was delayed is
 * still queued and is requeued in place, as ttwu_runnable requeues it
 * (requeue_delayed_entity, kernel/sched/fair.c:6981), unless its affinity
 * has left that processor or a suspend to idle holds it.
 */
static int sched_ready_locked(process_t *p, int wf)
{
    int target, why = wf & FAIR_WF_FORK ? FAIR_ENQ_NEW : FAIR_ENQ_WAKE;

    sched_remove(p);
    p->state = PROC_READY;
    proc_timer_update(p);
    if (cpu_table[p->cpu].current == p) {
        sched_ttwu_stat(p, p->cpu, wf);
        waketime_state(p, 2, p->cpu);
        return cpu_table[p->cpu].idle && _sched_enabled ? -1 : p->cpu;
    }
    (void)sched_clock_task(p->cpu);
    if (p->se.sched_delayed) {
        if (sched_fair_class(p) && sched_allowed(p, p->cpu) &&
            !(waits_held && !suspend_lets_run(p))) {
            sched_enqueue(p, FAIR_ENQ_WAKE, 0);
            sched_ttwu_stat(p, p->cpu, wf);
            return sched_wake_preempt(p, p->cpu, wf);
        }
        (void)sched_dequeue(p, FAIR_DEQ_FORCED);
    }
    sched_entity_init(p);
    if (sched_fair_class(p)) {
        target = fair_select_cpu(p, p->cpu, wf);
        if (target < 0 || target >= NR_CPUS || !sched_allowed(p, target)) {
            target = sched_choose(p);
        }
    } else {
        target = sched_allowed(p, p->cpu) ? p->cpu : sched_choose(p);
        if (!sched_cpu_idle(target)) {
            target = sched_rt_place(p, target);
        }
    }
    if (target != p->cpu) {
        if (why == FAIR_ENQ_NEW) {
            /* A new thread has no lag or load to move, as __set_task_cpu. */
            fair_set_task_rq(p, target);
            rseq_migrate(p);
            p->cpu = target;
        } else {
            sched_move(p, target);
            why |= FAIR_ENQ_MIGRATED;
        }
    }
    (void)sched_clock_task(target);
    sched_enqueue(p, why, 0);
    fair_balance_woken(target);
    sched_ttwu_stat(p, target, (why & FAIR_ENQ_MIGRATED) ? wf | SCHED_WF_MIGRATED : wf);
    return sched_wake_preempt(p, target, wf);
}

void sched_make_ready(process_t *p)
{
    uint32_t flags = spin_lock_irqsave(&sched_lock);
    int kick = sched_ready_locked(p, p->pcount == 0 ? FAIR_WF_FORK : FAIR_WF_TTWU);

    spin_unlock_irqrestore(&sched_lock, flags);
    if (kick >= 0) {
        smp_kick(kick);
    }
}

/** Charge the real-time thread `p`, current on processor `c`, its run since
 * `se.exec_start` on the task clock, into `se.sum_exec_runtime`, and start
 * its next interval now, as Linux's update_curr_rt charges a real-time
 * task through update_curr_se (kernel/sched/rt.c:989-1000, fair.c:
 * 1150-1160); the fair class charges its own threads. The run queues' lock
 * is held and the processor's task clock is current. */
static void sched_charge_rt(process_t *p, int c)
{
    uint64_t now = fair_rqs[c].clock_task;

    if (now > p->se.exec_start) {
        p->se.sum_exec_runtime += now - p->se.exec_start;
    }
    p->se.exec_start = now;
}

/** The part of `sched_priority_changed` for `p`, running on processor `c`,
 * whose class is now the fair one when `fair` is set: it leaves or joins
 * the fair class as its current thread, its run time charged by the class
 * it leaves and counted from now by the one it joins, and asks itself to
 * switch when it
 * becomes ordinary or a queued real-time thread outranks it. Returns the
 * processor to kick, or -1. The run queues' lock is held. */
static int sched_class_running(process_t *p, int c, int fair)
{
    int kick = -1;

    if (!fair && fair_rqs[c].curr == p) {
        fair_put_prev(p);
        (void)sched_dequeue(p, FAIR_DEQ_MOVE);
        fair_server_spend(c);
        p->se.exec_start = fair_rqs[c].clock_task;
    } else if (fair && fair_rqs[c].curr != p) {
        sched_charge_rt(p, c);
        sched_entity_init(p);
        if (!p->se.on_rq) {
            fair_enqueue(p, FAIR_ENQ_RESTORE);
            fair_server_track(c, sched_fair_runnable(c));
        }
        fair_set_next(p);
        kick = sched_resched_cur(p, c);
    }
    if (!fair && sched_rt_outranks(c, p)) {
        kick = sched_resched_cur(p, c);
    }
    return kick >= 0 ? kick : sched_fair_kick(c);
}

/** The part of `sched_priority_changed` for `p`, ready on processor `c`'s
 * queues: it moves to the queue of its class, keeping its lag, and asks
 * the thread running there to switch when it should run first. Returns the
 * processor to kick, or -1. The run queues' lock is held. */
static int sched_class_queued(process_t *p, int c, int fair)
{
    struct ready_queue *q = &run_queues[c];

    if (p->ready_linked && fair) {
        sched_remove(p);
        sched_enqueue(p, FAIR_ENQ_RESTORE, 0);
    } else if (!p->ready_linked && !fair) {
        (void)sched_dequeue(p, FAIR_DEQ_MOVE);
        sched_enqueue(p, 0, 0);
    } else if (p->ready_linked) {
        q->rt_count -= p->ready_rt;
        rt_queued -= p->ready_rt;
        p->ready_rt = sched_priority(p) != 0;
        q->rt_count += p->ready_rt;
        rt_queued += p->ready_rt;
    }
    return fair ? sched_wake_preempt(p, c, 0) : sched_preempt_cpu(p, c);
}

/**
 * A change of effective class moves the thread between the classes' queues
 * with its accounting, as Linux's __sched_setscheduler and rt_mutex_setprio
 * dequeue it, change its class and enqueue it again (kernel/sched/
 * syscalls.c:695-760, core.c:7140-7260): an ordinary thread that becomes
 * real-time leaves its fair queue keeping its lag (FAIR_DEQ_MOVE) and stops
 * being the fair class's current one when it runs, and one that becomes
 * ordinary comes back with it (FAIR_ENQ_RESTORE) and becomes the current
 * one when it runs; a delayed one finishes its dequeue. An ordinary
 * thread's weight follows its policy and nice (`fair_reweight`).
 *
 * A queued thread asks the thread it now outranks on its processor to
 * switch, as Linux's prio_changed_rt and switched_to_rt call resched_curr
 * on the queue's current task (kernel/sched/rt.c:2486-2492, 2526-2533),
 * and an ordinary one asks as a wake does (`sched_wake_preempt`). A running
 * thread asks itself when a queued thread that selection on its processor
 * could take now outranks it, as prio_changed_rt reschedules a running
 * task once the queue's highest priority is above its own (rt.c:2519-
 * 2520), and when it becomes ordinary, as switched_to_fair does
 * (fair.c:13367-13368). A blocked thread asks nothing.
 */
void sched_priority_changed(process_t *p)
{
    uint32_t flags = spin_lock_irqsave(&sched_lock);
    int c = p->cpu, kick = -1, fair = sched_fair_class(p);
    int running = cpu_table[c].current == p, idle = p == cpu_table[c].idle;

    (void)sched_clock_task(c);
    if (fair && !idle && p->se.cfs_rq) {
        fair_reweight(p);
    }
    if (running && !idle) {
        kick = sched_class_running(p, c, fair);
    } else if (p->state == PROC_READY && (p->ready_linked || p->se.on_rq || p->sched_held)) {
        kick = sched_class_queued(p, c, fair);
    } else if (p->se.sched_delayed && !fair) {
        (void)sched_dequeue(p, FAIR_DEQ_FORCED);
    }
    spin_unlock_irqrestore(&sched_lock, flags);
    if (kick >= 0) {
        smp_kick(kick);
    }
}

/** Make the blocked thread `p` ready after a queue or deadline wake, and
 * count the wake if its wait asked for it. The run queues' lock is held.
 * Return the processor to interrupt, or -1, as `sched_ready_locked` does. */
static int sched_wait_ready_locked(process_t *p)
{
    if (p->wait_wakeups) {
        (*p->wait_wakeups)++;
    }
    p->woken = 1;
    return sched_ready_locked(p, FAIR_WF_TTWU);
}

int sched_wake_blocked(process_t *p)
{
    uint32_t flags = spin_lock_irqsave(&sched_lock);
    int kick = -1;

    if (p->state == PROC_BLOCKED) {
        kick = sched_wait_ready_locked(p);
    }
    spin_unlock_irqrestore(&sched_lock, flags);
    return kick;
}

void sched_wake_stopped(process_t *p)
{
    uint32_t flags = spin_lock_irqsave(&sched_lock);
    int kick = -1;

    if (p->state == PROC_STOPPED) {
        kick = sched_ready_locked(p, FAIR_WF_TTWU);
    }
    spin_unlock_irqrestore(&sched_lock, flags);
    if (kick >= 0) {
        smp_kick(kick);
    }
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
        kick = sched_ready_locked(p, FAIR_WF_TTWU);
    } else if (cpu_table[p->cpu].current == p) {
        kick = p->cpu;
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
    return sched_wait_ready_locked(p);
}

int sched_wake_time_locked(process_t *p)
{
    if (p->state != PROC_BLOCKED || p->wakeup_tick == 0) {
        return -1;
    }
    p->wakeup_tick = 0;
    return sched_wait_ready_locked(p);
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
    smp_kick_many(cpus);
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
    if (p->se.sched_delayed && !sched_allowed(p, p->cpu)) {
        (void)sched_clock_task(p->cpu);
        (void)sched_dequeue(p, FAIR_DEQ_FORCED);
    }
    if (p->state == PROC_READY && !running && !p->sched_held && !sched_allowed(p, p->cpu)) {
        (void)sched_clock_task(p->cpu);
        (void)sched_dequeue(p, FAIR_DEQ_MOVE);
        sched_move(p, sched_choose(p));
        (void)sched_clock_task(p->cpu);
        sched_enqueue(p, FAIR_ENQ_MIGRATED, 0);
        kick = sched_wake_preempt(p, p->cpu, 0);
    }
    if (remote) {
        p->need_resched = 1;
        proc_work_set(p, WORK_RESCHED);
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
 * `next`, or ties with it and has neither yielded nor, under SCHED_RR, run
 * out its slice (`sched_tick`), which both set `sched_yielded`. A running
 * real-time thread is at the head of its priority's list in Linux, which
 * moves it to the tail only then (requeue_task_rt, kernel/sched/rt.c:1516-
 * 1530, 2589-2604). */
static int sched_keeps(const process_t *old, const process_t *next)
{
    return sched_priority(old) != 0 &&
           (!next || sched_priority(old) > sched_priority(next) ||
            (sched_priority(old) == sched_priority(next) && !old->sched_yielded));
}

/**
 * Select the thread to run on processor `me` after `old`: the real-time
 * thread `sched_find_rt` gives, or `old` when it keeps the processor
 * (`sched_keeps`); ahead of it the ordinary thread EEVDF picks
 * (`fair_pick`) while the fair server's turn lets the ordinary threads run
 * (`fair_server_takes`); and with no real-time thread, that ordinary one.
 * A yield by an ordinary `old` moves its virtual runtime to its deadline
 * first (`fair_yield`). With nothing here and `old` leaving, an ordinary
 * thread pulled from a
 * busier processor (`fair_balance_newidle`), as Linux's pick_next_task_fair
 * balances a processor about to go idle (kernel/sched/fair.c:8981-9000).
 * The run queues' lock is held and `old`'s state already reflects
 * blocking, affinity and suspension. NULL means there is no candidate.
 */
static process_t *sched_select(int me, process_t *old)
{
    struct percpu *cpu = &cpu_table[me];
    process_t *rt = sched_find_rt(me), *next;

    if (old->sched_yielded && old->state == PROC_RUNNING && sched_fair_running(me, old)) {
        fair_yield(old);
    }
    if (old != cpu->idle && old->state == PROC_RUNNING && sched_keeps(old, rt)) {
        rt = old;
    }
    old->sched_yielded = 0;
    if (rt && !(sched_fair_runnable(me) != 0 && fair_server_takes(me, rt))) {
        return rt;
    }
    next = fair_pick(me);
    if (next || rt) {
        return next ? next : rt;
    }
    if ((old == cpu->idle || old->state != PROC_RUNNING) && fair_balance_newidle(me) != 0) {
        (void)sched_clock_task(me);
        next = fair_pick(me);
        if (!next) {
            next = sched_find_rt(me);
        }
    }
    return next;
}

/**
 * Bring the outgoing thread `old` to its state before selection: one that
 * blocked leaves its fair queue as a sleep, which EEVDF may delay, and one
 * stopped or ended leaves it at once; one whose affinity no longer has this
 * processor is made ready on one it has, its entity moved there; one a
 * suspend to idle holds goes to the held list, or for a real-time thread
 * stays ready where it is; and an ordinary one that runs but is on no fair
 * queue, as one the debug shell adopted, is queued. Returns the processor
 * a moved thread asks to select (`sched_wake_preempt`), to kick after
 * selection, or -1. The class has this processor's clock.
 */
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
    if (old == cpu->idle) {
        return -1;
    }
    if (old->state != PROC_RUNNING) {
        if (old->se.on_rq) {
            (void)sched_dequeue(old, old->state == PROC_BLOCKED ? FAIR_DEQ_SLEEP : FAIR_DEQ_FORCED);
        }
        return -1;
    }
    if (cpu->idle && !((old->affinity >> me) & 1u)) {
        old->state = PROC_READY;
        if (fair_rqs[me].curr == old) {
            fair_put_prev(old);
        }
        (void)sched_dequeue(old, FAIR_DEQ_MOVE);
        sched_move(old, sched_choose(old));
        (void)sched_clock_task(old->cpu);
        sched_enqueue(old, FAIR_ENQ_MIGRATED, 0);
        (void)sched_clock_task(me);
        return sched_wake_preempt(old, old->cpu, 0);
    }
    if (cpu->idle && !suspend_lets_run(old)) {
        old->state = PROC_READY;
        if (sched_fair_class(old)) {
            if (fair_rqs[me].curr == old) {
                fair_put_prev(old);
            }
            (void)sched_dequeue(old, FAIR_DEQ_FORCED);
            sched_hold(old);
        }
        return -1;
    }
    if (sched_fair_class(old) && !old->se.on_rq) {
        sched_entity_init(old);
        fair_enqueue(old, FAIR_ENQ_RESTORE);
        fair_server_track(me, sched_fair_runnable(me));
    }
    return kick;
}

/**
 * Give the whole ticks from `cpu`'s count to the last edge of a scheduler
 * period of `period` ticks at or before `now`. Linux charges a stopped
 * tick's idle span by the jiffies it crossed (tick_nohz_account_idle_time,
 * kernel/time/tick-sched.c:1395-1415), and a running tick charges each edge
 * to what it finds, so the time from the last edge before a halt to the
 * halt, and from the wake to the next edge, is the thread's that the next
 * tick finds. Counting to `now` would charge to idle the run of the last
 * thread after its last tick: two milliseconds a sleep on average at a
 * guest's 4 ms period, and all of a run shorter than a period that began
 * at a wake.
 */
static uint64_t sched_edge_ticks(const struct percpu *cpu, int64_t now, uint32_t period)
{
    uint64_t rem;
    int64_t gap;

    (void)tick_div64((uint64_t)now, (uint64_t)period * TICK_NS, &rem);
    gap = now - (int64_t)rem - cpu->accounted_ns + (int64_t)(TICK_NS / 2u);
    return gap < (int64_t)TICK_NS ? 0 : tick_div64((uint64_t)gap, TICK_NS, NULL);
}

/** Count `ticks` of this processor's as idle, less the whole ticks of the
 * hypervisor's steal time `stolen` not yet counted, which count as steal. */
static void sched_charge_idle(struct percpu *cpu, uint64_t ticks, uint64_t stolen)
{
    uint64_t take = tick_div64(stolen, TICK_NS, NULL);

    if (take > ticks) {
        take = ticks;
    }
    cpu->ticks_seq++;
    cpu->accounted_ns += (int64_t)ticks * (int64_t)TICK_NS;
    cpu->steal_ticks += take;
    cpu->steal_ns += take * TICK_NS;
    cpu->idle_ticks += ticks - take;
    cpu->ticks_seq++;
}

/** Charge an idle wait that has just ended, to the last scheduler edge
 * (`sched_edge_ticks`): none when the tick ran through it and charged each
 * edge, and each edge it passed when it stopped. IRQs are off. */
static void sched_idle_account(struct percpu *cpu)
{
    uint32_t period = cpu->tick_stopped ? cpu->tick_stop_period : timer_tick_period();

    sched_charge_idle(cpu, sched_edge_ticks(cpu, rtc_monotonic_ns(), period),
                      arch_steal_ns() - cpu->steal_ns);
}

/** Finish a stopped idle account and commit the incoming thread's timer.
 * The scheduler has published its new current thread and released its
 * lock. IRQs are off; no callback or global device service runs here. */
static void sched_idle_leave(struct percpu *cpu)
{
    if (cpu->tick_stopped) {
        sched_idle_account(cpu);
    }
    timer_idle_exit();
    if (cpu->tick_stopped) {
        cpu->ticks_seq++;
        cpu->tick_stopped = 0;
        cpu->ticks_seq++;
    }
}

/** Keep `old` on processor `me` when selection found nothing else: it runs
 * on, or halts in place where no idle task exists yet, in which case it is
 * no longer the fair class's current thread. The run queues' lock is held. */
static void sched_stay(process_t *old, int me)
{
    if (fair_rqs[me].curr == old && !sched_fair_running(me, old)) {
        fair_put_prev(old);
    }
    fair_server_spend(me);
    waketime_state(old, old->state == PROC_RUNNING ? 2 : 3, me);
    old->slice_ns = sched_clock_ns();
}

/** Make the fair class's current thread on processor `me` follow the
 * selection of `next`: the one that was is charged and put back
 * (`fair_put_prev`), and an ordinary `next` becomes current
 * (`fair_set_next`); the fair server spends what was charged. The
 * real-time average counts a real-time thread's run to the switch away
 * from it, and none before one starts (`pelt_update_rt`), as Linux's
 * put_prev_task_rt and set_next_task_rt; so does the outgoing real-time
 * thread's run time (`sched_charge_rt`), and an incoming one's starts
 * here. The run queues' lock is held. */
static void sched_follow(int me, process_t *next)
{
    process_t *old = cpu_table[me].current;
    uint64_t now = fair_rqs[me].clock_task;

    if (old != next && old != cpu_table[me].idle && !sched_fair_class(old)) {
        sched_charge_rt(old, me);
    }
    if (old != next && next != cpu_table[me].idle && !sched_fair_class(next)) {
        next->se.exec_start = now;
    }
    if (old != next && old != cpu_table[me].idle && !sched_fair_class(old)) {
        (void)pelt_update_rt(me, now, 1);
    } else if (old != next && next != cpu_table[me].idle && !sched_fair_class(next)) {
        (void)pelt_update_rt(me, now, 0);
    }
    if (fair_rqs[me].curr && fair_rqs[me].curr != next) {
        fair_put_prev(fair_rqs[me].curr);
    }
    if (next != cpu_table[me].idle && sched_fair_class(next) && fair_rqs[me].curr != next) {
        fair_set_next(next);
    }
    fair_server_spend(me);
}

/** Account the switch from `old` to `next` on processor `cpu`: the count of
 * switches, `old`'s voluntary or involuntary one and its `sched_out`, and
 * its place if it could still run: a preempted real-time thread at the head
 * of its list, one that yielded or ran out its slice at the tail, and an
 * ordinary one where it is, on its fair queue or the held list. The run
 * queues' lock is held. */
static void sched_switch_out(struct percpu *cpu, process_t *old, process_t *next, int yielded)
{
    int me = (int)(cpu - cpu_table);

    cpu->switches++;
    if (old->state == PROC_RUNNING && old != cpu->idle) {
        old->state = PROC_READY;
    }
    if (old != cpu->idle) {
        if (old->state == PROC_READY) {
            old->nivcsw++;
        } else {
            old->nvcsw++;
        }
        bkl_note_switch(old, next, yielded);
        if (old->sched_out) {
            old->sched_out(old->sched_out_arg, old->state == PROC_READY);
        }
    }
    if (old->state == PROC_READY && old != cpu->idle) {
        if (!sched_fair_class(old)) {
            sched_link(old, !yielded && old->cpu == me);
        } else {
            sched_wait_begin(old);
        }
    } else {
        waketime_state(old, 3, me);
    }
    rseq_preempt(old);
}

/**
 * The ordinary threads stay on their fair queue while they run, as Linux
 * keeps the current task queued: one that blocks leaves it as a sleep,
 * which EEVDF may delay until its lag is repaid, and the class's current
 * thread follows the selection (`fair_put_prev`, `fair_set_next`), which
 * charges the outgoing one. A yield moves the yielder's virtual runtime to
 * its deadline first (`fair_yield`). A processor pulls an ordinary thread
 * from a busier one only when its current process is its idle task or
 * cannot run on, and with nothing to pull, one that cannot run on gives way
 * to the idle task. A process that blocks or ends therefore always leaves
 * the processor.
 *
 * A current process whose affinity no longer has this processor is made
 * ready on a processor it has; that processor may select it before the
 * switch away from it is done, and waits for its `on_cpu` to clear before
 * it switches to it. A
 * current process a suspend to idle holds is made ready where it is, an
 * ordinary one on the held list: on
 * one processor the suspend's writer was the only one to run, and on
 * several the others stop what they run at its next switch: the tick's,
 * a call's exit, or a kernel thread's next sleep or service point.
 *
 * Selection and publication run with interrupts disabled and the run
 * queues' lock held, so a wake cannot race the selection or a timer
 * re-enter partially updated state. The outgoing thread's request
 * (`need_resched`) and the fair class's (`fair_resched`), which the
 * charge of the outgoing thread may raise during the selection, are
 * cleared in the same hold, once selection is done and before either
 * return without a switch, as Linux's __schedule clears it
 * (kernel/sched/core.c:6705-6707): a request made after the selection is
 * kept, and one the selection has answered is not taken again, by the
 * incoming thread's slice event above all, which would switch it out at
 * once.
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
    int me, kick, yielded;

    if (!_sched_enabled || !current_proc) {
        return;
    }
    lockdep_switch((uintptr_t)(const char *)__builtin_return_address(0));

    saved_flags = spin_lock_irqsave(&sched_lock);
    rcu_qs();
    me = (int)arch_cpu_id();
    cpu = &cpu_table[me];
    old = cpu->current;
    pick_count++;
    if (fair_schedstats) {
        rq_stats[me].sched_count++;
    }
    watchdog_sched();
    (void)sched_clock_task(me);
    kick = sched_prepare_old(old, cpu, me);
    sched_balance_push(me, old);
    yielded = old->sched_yielded;
    next = sched_select(me, old);
    old->need_resched = 0;
    (void)fair_resched(me);

    if (!next) {
        if (old == cpu->idle || old->state == PROC_RUNNING || !cpu->idle) {
            sched_stay(old, me);
            spin_unlock_irqrestore(&sched_lock, saved_flags);
            return;
        }
        next = cpu->idle;
    }
    if (fair_schedstats && next == cpu->idle) {
        rq_stats[me].sched_goidle++;
    }
    if (kick >= 0) {
        smp_kick(kick);
    }
    sched_follow(me, next);
    sched_remove(next);
    sched_arrive(next);
    waketime_state(next, 2, me);
    usb_stamp_dispatch(next->pid);
    next->woken = 0;
    if (next == old) {
        next->state = PROC_RUNNING;
        next->slice_ns = sched_clock_ns();
        spin_unlock_irqrestore(&sched_lock, saved_flags);
        return;
    }

    sched_switch_out(cpu, old, next, yielded);
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
    if (old == cpu->idle) {
        (void)arch_atomic_xchg(&cpu->idle_poll, CPU_POLL_NONE);
    }
    cpu->current = next;
    cpu->switch_prev = old;
    spin_unlock(&sched_lock);
    if (old == cpu->idle) {
        sched_idle_leave(cpu);
    }
    old->bkl_depth = bkl_drop();
    sched_wait_off_cpu(next);
    next->on_cpu = 1;

    switch_context(old, next);

    schedule_tail();
    membarrier_switch(old);
    kirq_restore(saved_flags);
}

/**
 * Arm the slice event, Linux's hrtick, at the next moment that changes what
 * this processor should run, or request selection when that moment has
 * passed: for an ordinary current thread, the monotonic time at which it,
 * or a group entity above it, reaches its deadline while something
 * competes (`fair_next_event_ns`), charged at that moment by the tick's
 * hook (`fair_tick`); and, while real-time and ordinary threads compete,
 * the fair server's next turn or the end of its budget
 * (`fair_server_event`). A real-time thread's slice is counted by the
 * tick alone (`sched_rr_tick`). Interrupts are off and no scheduler lock
 * is held.
 */
void sched_slice_rearm(void)
{
    struct percpu *cpu = this_cpu();
    process_t *p = cpu->current, *rt;
    uint32_t flags;
    int me = (int)arch_cpu_id(), due = 0;
    uint64_t at = 0, ev, now;

    if (!p || p == cpu->idle || p->need_resched) {
        return;
    }
    flags = spin_lock_irqsave(&sched_lock);
    (void)sched_clock_task(me);
    now = sched_clock_ns();
    if (sched_fair_running(me, p)) {
        /* The computation charges `p` first, which may end its slice. */
        at = fair_next_event_ns(p);
        due = fair_resched(me);
        if (!due && at != 0 && at <= now) {
            due = fair_tick(p);
            at = due ? 0 : fair_next_event_ns(p);
        }
        fair_server_spend(me);
        rt = sched_find_rt(me);
        if (!due && rt) {
            if (fair_server_takes(me, rt)) {
                ev = fair_server_event(me);
                at = at == 0 || ev < at ? ev : at;
            } else {
                due = 1;
            }
        }
    } else if (!sched_fair_class(p) && sched_fair_runnable(me) != 0) {
        if (fair_server_takes(me, p)) {
            due = 1;
        } else {
            at = fair_server_event(me);
        }
    }
    spin_unlock_irqrestore(&sched_lock, flags);
    if (due || (at != 0 && at <= now)) {
        p->need_resched = 1;
        proc_work_set(p, WORK_RESCHED);
    } else if (at != 0) {
        timer_wake_at((int64_t)at);
    }
}

/**
 * Whether a yield by the current thread would select it again, so that
 * sched_yield may return without `schedule`: the scheduler is on, the
 * thread is no idle task, has no request to switch (`need_resched`), may
 * still run here and is not held by a suspend to idle, its processor's
 * queue is empty and no real-time thread is queued anywhere, which
 * selection could take from another queue (`sched_find_rt`). Read without
 * the run queues' lock, as Linux's yield takes only its own processor's
 * queue's lock (do_sched_yield, kernel/sched/syscalls.c:1410-1425) and
 * returns at once from a queue that holds the yielder alone
 * (yield_task_fair, kernel/sched/fair.c:9109-9110), whose schedule then
 * picks it again: a thread queued here just after the look waits for the
 * next selection, as one queued just after a yield's selection does. Every
 * yield taking the one lock of every queue, as Wine's spin calls it, kept
 * that lock's line moving between the spinning processor and those whose
 * threads block and wake.
 */
int sched_yield_alone(void)
{
    uint32_t flags = kirq_save();
    int me = (int)arch_cpu_id();
    process_t *p = cpu_table[me].current;
    int alone = _sched_enabled && p && p != cpu_table[me].idle && !p->need_resched &&
                sched_queued(me) == 0 && rt_queued == 0 && ((p->affinity >> me) & 1u) &&
                suspend_lets_run(p);

    kirq_restore(flags);
    return alone;
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
    sched_slice_rearm();
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
 * Before each wait it runs the idle balance it was kicked for, or the decay
 * of blocked load a newly idle balance left it (`sched_idle_balance`), as
 * Linux's do_idle runs nohz_run_idle_balance.
 */
void sched_idle_loop(void)
{
    for (;;) {
        arch_irq_disable();
        schedule();
        sched_idle_balance();
        idle_wait_irq_off();
    }
}

void sched_idle_awake(void)
{
    struct percpu *cpu = this_cpu();

    if (cpu->idle && cpu->current == cpu->idle) {
        (void)arch_atomic_xchg(&cpu->idle_poll, CPU_POLL_ON);
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
        proc_work_set(current_proc, WORK_RESCHED);
        return;
    }
    schedule();
}

int sched_rr_timeslice_ms(void)
{
    return rr_timeslice_ms;
}

/** Linux's sched_rr_handler (kernel/sched/rt.c:2966-2987): a value at or
 * below 0 restores the default, and any other is kept as written and
 * counted in ticks rounded up, as msecs_to_jiffies rounds it. */
void sched_set_rr_timeslice_ms(int ms)
{
    uint32_t flags = spin_lock_irqsave(&sched_lock);

    if (ms <= 0) {
        rr_timeslice = SCHED_RR_TIMESLICE_MS * TICK_HZ / 1000;
        rr_timeslice_ms = SCHED_RR_TIMESLICE_MS;
    } else {
        rr_timeslice = (int)MS_TO_TICKS((uint32_t)ms);
        rr_timeslice_ms = ms;
    }
    spin_unlock_irqrestore(&sched_lock, flags);
}

uint64_t sched_rr_slice_ns(void)
{
    return (uint64_t)(uint32_t)rr_timeslice * TICK_NS;
}

/**
 * Linux's sched_rr_get_interval (kernel/sched/syscalls.c:1586-1616): a
 * SCHED_RR thread's slice; an ordinary thread's slice in whole ticks,
 * rounded down, while its processor's fair queue has load, and 0 on an
 * otherwise idle one, as get_rr_interval_fair (kernel/sched/fair.c:
 * 13716-13729); 0 for SCHED_FIFO and SCHED_DEADLINE.
 */
uint64_t sched_rr_interval_ns(const process_t *p)
{
    uint64_t slice;
    uint32_t flags;
    int loaded;

    if (p->sched_policy == SCHED_RR) {
        return sched_rr_slice_ns();
    }
    if (p->sched_policy == SCHED_FIFO || p->sched_policy == SCHED_DEADLINE) {
        return 0;
    }
    flags = spin_lock_irqsave(&sched_lock);
    loaded = fair_rqs[p->cpu].cfs.load.weight != 0;
    slice = p->se.slice;
    spin_unlock_irqrestore(&sched_lock, flags);
    return loaded ? tick_div64(slice, TICK_NS, NULL) * TICK_NS : 0;
}

uint64_t sched_slice_ns(const process_t *p)
{
    uint64_t slice;
    uint32_t flags = spin_lock_irqsave(&sched_lock);

    slice = p->se.cfs_rq ? p->se.slice : p->sched_slice ? p->sched_slice : sysctl_sched_base_slice;
    spin_unlock_irqrestore(&sched_lock, flags);
    return slice;
}

void sched_rt_bandwidth(int *period_us, int *runtime_us)
{
    *period_us = rt_period_us;
    *runtime_us = rt_runtime_us;
}

/** Linux's sched_rt_handler, sched_rt_global_validate and
 * sched_dl_global_validate (kernel/sched/rt.c:2906-2964,
 * kernel/sched/deadline.c:3392-3427): the period at least 1, the runtime
 * -1 for none or at most the period, and the new bandwidth on the online
 * processors no less than the fair servers', -EBUSY. Its ceiling on the
 * runtime, 2^44 - 1 ns, is above any int of microseconds. */
int sched_set_rt_bandwidth(int period_us, int runtime_us)
{
    uint64_t total;
    int64_t bw;
    uint32_t flags;
    int cpus;

    if (period_us < 1 || runtime_us < -1 || (runtime_us != -1 && runtime_us > period_us)) {
        return -EINVAL;
    }
    flags = spin_lock_irqsave(&sched_lock);
    bw = sched_rt_ratio(period_us, runtime_us);
    total = sched_servers_bw(&cpus);
    if (bw >= 0 && (uint64_t)bw * (uint32_t)cpus < total) {
        spin_unlock_irqrestore(&sched_lock, flags);
        return -EBUSY;
    }
    rt_period_us = period_us;
    rt_runtime_us = runtime_us;
    spin_unlock_irqrestore(&sched_lock, flags);
    return 0;
}

/**
 * Count a tick against the current thread `p`'s RR slice, and when the
 * slice has run out start a new one and return 1, to select now, if a peer
 * of its priority that selection here could take is queued, which it then
 * goes behind (`sched_yielded`), as Linux's task_tick_rt counts the slice
 * in ticks and requeues the task behind its peers
 * (kernel/sched/rt.c:2586-2604). A FIFO thread, and an ordinary one
 * raised by a priority-inheriting futex, has no slice. The run queues'
 * lock is held.
 */
static int sched_rr_tick(process_t *p, int me)
{
    process_t *q;
    int c;

    if (p->sched_policy != SCHED_RR || sched_priority(p) == 0) {
        return 0;
    }
    if (++p->rr_used < (uint32_t)rr_timeslice) {
        return 0;
    }
    p->rr_used = 0;
    for (c = 0; c < NR_CPUS; c++) {
        if (run_queues[c].rt_count == 0) {
            continue;
        }
        for (q = run_queues[c].head; q; q = q->ready_next) {
            if (sched_priority(q) == sched_priority(p) && sched_may_run(q, me)) {
                p->sched_yielded = 1;
                return 1;
            }
        }
    }
    return 0;
}

/**
 * The periodic tick's part, which the tick's handler alone calls, with
 * interrupts off: an RR slice, counted in ticks, an ordinary thread's
 * slice, the fair server's budget and the load balance are counted by the
 * tick alone, as Linux's sched_tick runs only from the tick
 * (kernel/sched/core.c:5614-5660). It asks for the switch
 * (`need_resched`), which the interrupt's end takes once the interrupt's
 * time is counted (`sched_irq_exit`).
 *
 * A real-time thread runs on until a queued real-time thread that
 * selection here could take outranks it (`sched_rt_outranks`), as
 * wakeup_preempt_rt asks a lower current task to switch
 * (kernel/sched/rt.c:1657-1658), its SCHED_RR slice runs out with a peer
 * of its priority queued (`sched_rr_tick`), or the fair server's turn
 * comes for the ordinary threads waiting (`fair_server_takes`). An
 * ordinary thread runs on until the class says otherwise (`fair_tick`,
 * Linux's task_tick_fair), or a real-time thread waits that the fair
 * server does not hold back. Then the periodic balance runs
 * (`fair_balance_tick`), which kicks a processor whose tick has stopped in
 * its idle wait to balance when this one is busy, as sched_tick calls
 * sched_balance_trigger
 * (kernel/sched/fair.c:12426-12540, 12976-12990).
 */
void sched_tick(void)
{
    process_t *p = current_proc, *rt;
    uint32_t flags;
    int me = (int)arch_cpu_id(), due = 0, kick;

    if (!p || !_sched_enabled) {
        return;
    }
    flags = spin_lock_irqsave(&sched_lock);
    (void)sched_clock_task(me);
    if (p == this_cpu()->idle) {
        /* An idle processor balances and selects what it pulled. */
    } else if (!sched_fair_class(p)) {
        sched_charge_rt(p, me);
        (void)pelt_update_rt(me, fair_rqs[me].clock_task, 1);
        due = sched_rr_tick(p, me) || sched_rt_outranks(me, p) ||
              (sched_fair_runnable(me) != 0 && fair_server_takes(me, p));
    } else {
        if (sched_fair_running(me, p)) {
            due = fair_tick(p);
        }
        fair_server_spend(me);
        rt = sched_find_rt(me);
        if (rt && !fair_server_takes(me, rt)) {
            due = 1;
        }
    }
    fair_balance_tick(me, p == this_cpu()->idle);
    kick = sched_fair_kick(me);
    if (kick == me && p != this_cpu()->idle) {
        due = 1;
    }
    spin_unlock_irqrestore(&sched_lock, flags);
    if (kick >= 0 && kick != me) {
        smp_kick(kick);
    }
    if (p == this_cpu()->idle) {
        return;
    }
    if (!due) {
        sched_slice_rearm();
        return;
    }
    p->need_resched = 1;
    proc_work_set(p, WORK_RESCHED);
}

/** A request made by a wake or a priority change on another processor is
 * taken where the interrupt it sent returns, and one this processor's own
 * handler made, by a wake of a device's reader, here: Linux's return from
 * an interrupt switches only on TIF_NEED_RESCHED (kernel/entry/common.c:
 * 104-105). */
void sched_irq_exit(void)
{
    process_t *p = current_proc;

    if (p && p != this_cpu()->idle && p->need_resched) {
        sched_preempt();
    }
}

/** `schedule` clears the request under the run queues' lock, after its
 * selection, so a request that arrives after the selection is the next
 * call's and none is lost by a clear after the switch. */
void sched_take_preempt(void)
{
    if (current_proc && current_proc != this_cpu()->idle && current_proc->need_resched) {
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
 * first to any processor that has claimed it (`bkl_yield`). The switch is
 * taken only on a request, the tick's at the end of a slice or a wake's
 * that should run first (`sched_wake_preempt`), as Linux's cond_resched
 * tests TIF_NEED_RESCHED alone (kernel/sched/core.c:7287-7292). The caller
 * is at a point where it could have slept, which the soft-lockup detector
 * counts as a schedule (`watchdog_sched`), as Linux's cond_resched would
 * switch to the stopper thread the watchdog queued.
 */
void sched_service_point(void)
{
    uint32_t flags = kirq_save();

    rcu_qs();
    watchdog_sched();
    bkl_yield();
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
    uint64_t pending, stolen;

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
        if (cpu->tick_stopped) {
            pending = sched_edge_ticks(cpu, rtc_monotonic_ns(), cpu->tick_stop_period);
            stolen = tick_div64(arch_cpu_steal_ns(c) - cpu->steal_ns, TICK_NS, NULL);
            if (stolen > pending) {
                stolen = pending;
            }
            *idle += pending - stolen;
            *steal += stolen;
        }
        if (cpu->ticks_seq == seq) {
            return;
        }
    }
}

/** Only this processor writes its `rq_stats`, so interrupts off keep the
 * count on it with no lock, which the yield that takes none needs. */
void sched_note_yield(void)
{
    uint32_t flags;

    if (fair_schedstats) {
        flags = kirq_save();
        rq_stats[arch_cpu_id()].yld_count++;
        kirq_restore(flags);
    }
}

void sched_rq_debug(int c, struct sched_rq_debug *out)
{
    const process_t *cur = cpu_table[c].current, *t;

    out->nr_running = sched_balance_nr(c);
    out->nr_switches = cpu_table[c].switches;
    out->nr_uninterruptible = 0;
    for (t = proc_list; t; t = t->next) {
        if (t->cpu == c && t->state == PROC_BLOCKED && t->uninterruptible) {
            out->nr_uninterruptible++;
        }
    }
    out->curr_pid = cur && cur != cpu_table[c].idle ? (int)cur->pid : 0;
    out->clock = fair_rqs[c].clock;
    out->clock_task = fair_rqs[c].clock_task;
    out->rt_nr_running = run_queues[c].rt_count;
    if (cur && cur != cpu_table[c].idle && !sched_fair_class(cur)) {
        out->rt_nr_running++;
    }
    out->yld_count = rq_stats[c].yld_count;
    out->sched_count = rq_stats[c].sched_count;
    out->sched_goidle = rq_stats[c].sched_goidle;
    out->ttwu_count = rq_stats[c].ttwu_count;
    out->ttwu_local = rq_stats[c].ttwu_local;
}

uint64_t sched_switches(void)
{
    uint64_t sum = 0;
    int c;

    for (c = 0; c < NR_CPUS; c++) {
        sum += cpu_table[c].switches;
    }
    return sum;
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
 * A thread on another processor's current record that has not halted runs
 * kernel or user code there, since a thread in the kernel keeps its
 * processor until it sleeps, and one that sleeps with nothing else ready
 * stays current and halts in an idle wait (`wq_wait`). This is Linux's
 * owner_on_cpu, which spins only while the owner is on its processor and
 * that processor's vCPU is not preempted (include/linux/sched.h:2200-2207),
 * and mutex_spin_on_owner's test of need_resched
 * (kernel/locking/mutex.c:373).
 */
int sched_spin_on(const process_t *owner, int cpu)
{
    const volatile struct percpu *c;

    if (!current_proc || current_proc->need_resched || !bkl_spin_ok(owner ? cpu : -1)) {
        return 0;
    }
    if (owner) {
        if (cpu < 0 || cpu >= NR_CPUS) {
            return 0;
        }
        c = &cpu_table[cpu];
        if (c->current != owner || c->idle_wait || arch_cpu_preempted(cpu)) {
            return 0;
        }
    }
    arch_relax();
    return 1;
}

int sched_running_cpu(const process_t *owner)
{
    const volatile struct percpu *c;
    int i;

    if (!owner) {
        return -1;
    }
    for (i = 0; i < NR_CPUS; i++) {
        c = &cpu_table[i];
        if (c->online && c->current == owner && !c->idle_wait) {
            return i;
        }
    }
    return -1;
}

int sched_spin_pinned(void)
{
    const process_t *p = current_proc;

    return p && (p->no_tick_switch || p->in_syscall || p->in_fault);
}

/**
 * Count a tick of `p`'s processor time against its process's timer `k`
 * when `counts`, and when the timer runs out arm it again for its interval,
 * less the part of the tick past its expiry, so that its signals come at
 * the interval's mean rate as Linux's `expires += incr` gives them, and
 * leave `signo` for a holder of the big lock to post
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
    if (tg->cpu_left[k] > TICK_NS) {
        tg->cpu_left[k] -= TICK_NS;
    } else if (tg->cpu_left[k] != 0) {
        uint64_t over = TICK_NS - tg->cpu_left[k];
        uint64_t next = tg->cpu_interval[k];

        tg->cpu_left[k] = next == 0 ? 0 : next > over ? next - over : 1u;
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
 * is still current. The tick goes into the process's times together with
 * its POSIX timers, under their lock (`ptimer_cpu_tick`).
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
        if (current_proc->tg) {
            cpu_timer_tick(current_proc, 0, user, SIGVTALRM);
            cpu_timer_tick(current_proc, 1, 1, SIGPROF);
            ptimer_cpu_tick(current_proc, user);
        } else if (user) {
            ++current_proc->utime;
        } else {
            ++current_proc->stime;
        }
    }
}

int sched_cycle_account;

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
 * A tick is charged to the mode it finds, as Linux's is (`user_mode`, from
 * update_process_times, kernel/time/timer.c:2519), and with the cycle
 * account off (`sched_cycle_account`) that is all: the Debian kernel 80-92
 * measured, without nohz_full, charges its tick so. One that comes while
 * interrupts are off is held and taken as they come back on. A system
 * call's handler runs with them on, and its entry runs with them off until
 * the dispatcher turns them on. Its exit turns them off after the handler
 * and keeps them off through the loopback drain and the preemption; the
 * signal delivery and rseq update of the return to user mode
 * (`deliver_signals` and `rseq_return`, in `isr_leave_work`) run with them
 * on, so a tick then is taken in kernel mode; and they are off again from
 * that work's end to the return to user mode. A held spinlock and an
 * interrupt's handler run with them off too. A tick held to the first user
 * instruction after the call, or to the start of an idle wait's halt, would
 * be charged to the program or to idle although it came in the kernel.
 * With the cycle account on, when the timer keeps the tick's time, `due`,
 * the ticks that would have come before that point (`user_cycles`, which
 * `isr_leave` notes, or `idle_cycles`) are charged where a tick that came
 * on time would have been taken: as system time, or as idle when the
 * stretch that held them began at the end of an idle wait
 * (`idle_end_cycles`) and not at an entry from user mode (`entry_cycles`),
 * since Linux's idle task loops there. The rest are charged to the mode
 * the tick found. A tick comes after its time by the delay of its
 * interrupt, which a hypervisor makes microseconds long, so the time a tick
 * would have come is its time plus the processor's average delay
 * (`tick_late_ns`), which ticks that came with interrupts on measure;
 * comparing the bare time would charge the ticks due in the kernel that
 * came in user mode to the kernel, which Linux charges to the program.
 */
void sched_account_tick(int user, int64_t due)
{
    struct percpu *cpu = this_cpu();
    int64_t now = rtc_monotonic_ns(), on = 0;
    int64_t gap = now - cpu->accounted_ns + (int64_t)(TICK_NS / 2u);
    uint64_t ticks, stolen;
    int found = cpu->idle_wait || cpu->tick_stopped ? ACCOUNT_IDLE
                : user                              ? ACCOUNT_USER
                                                    : ACCOUNT_SYSTEM;
    int held = found;

    if (due != 0 && cpu->idle_wait) {
        cpu->idle_short = 0;
    }
    if (gap < (int64_t)TICK_NS) {
        return;
    }
    ticks = tick_div64((uint64_t)gap, TICK_NS, NULL);
    if (due != 0 && found != ACCOUNT_SYSTEM && sched_cycle_account) {
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
    if (found == ACCOUNT_IDLE && (due == 0 || cpu->tick_stopped)) {
        if (cpu->tick_stopped) {
            ticks = sched_edge_ticks(cpu, now, cpu->tick_stop_period);
        }
        sched_charge_idle(cpu, ticks, stolen);
        return;
    }
    cpu->accounted_ns += (int64_t)ticks * (int64_t)TICK_NS;
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

/** Return the next load sample's deadline for the stopped idle tick. */
uint32_t sched_load_idle_ticks(void)
{
    uint32_t passed = (uint32_t)kernel_ticks - load_tick;
    uint32_t wait = passed >= load_wait ? 1u : load_wait - passed;

    return wait < TICK_HZ ? wait : TICK_HZ;
}

/** Stop the idle task's tick immediately before a halt, and leave the
 * processor to the idle balance of others (`fair_balance_idle_enter`).
 * IRQs are off and no lock is held; return zero when a wake requires
 * another selection. */
static int idle_stop_tick(void)
{
    struct percpu *cpu = this_cpu();
    int stopped;

    cpu->ticks_seq++;
    cpu->tick_stop_period = timer_tick_period();
    cpu->tick_stopped = 1;
    cpu->ticks_seq++;
    stopped = timer_idle_enter();
    cpu->ticks_seq++;
    cpu->tick_stopped = stopped;
    cpu->ticks_seq++;
    if (stopped) {
        spin_lock(&sched_lock);
        fair_balance_idle_enter((int)(cpu - cpu_table));
        spin_unlock(&sched_lock);
    }
    return !current_proc->need_resched;
}

/** Halt with the idle tick stopped for a long wait, or retained after a
 * short non-tick wake, as Linux's idle governors retain it for a predicted
 * interval below one tick. IRQs are off and no lock is held. Return one
 * after a halt, zero when ready work declines it, which the caller answers
 * through `idle_kicked_window`. A real scheduler tick
 * clears the prediction, so a quiet CPU stops at its next idle entry. */
static int idle_sleep(int stop_tick, uint32_t per_us)
{
    struct percpu *cpu = this_cpu();
    uint64_t begin = 0;
    int64_t tick = timer_tick_due();

    if (stop_tick && (cpu->tick_stopped || !cpu->idle_short) && !idle_stop_tick()) {
        return 0;
    }
    if (stop_tick && current_proc->need_resched) {
        return 0;
    }
    if (stop_tick && !cpu->tick_stopped) {
        timer_idle_retain();
    }
    if (stop_tick && per_us != 0) {
        begin = arch_cycles();
    }
    arch_idle_masked();
    if (stop_tick && per_us != 0) {
        cpu->idle_short = timer_wakes_between() && timer_tick_due() == tick &&
                          arch_cycles() - begin < (uint64_t)timer_tick_period() * TICK_US * per_us;
    }
    return 1;
}

/** Let a pending interrupt in for one instruction: `sti` takes effect after
 * the return that follows it. A wait that a kick, a declined sleep or ready
 * work found after the marker ends opens this while the processor is still
 * marked idle; a loopback drain, and ready work found before the marker,
 * open it before the marker is set. Either returns with interrupts off. */
static void idle_kicked_window(void)
{
    arch_irq_enable();
    arch_irq_disable();
}

/**
 * Wait with the big lock let go. The scheduler idle task polls with its
 * existing tick and interrupts on: the interrupt routers mark the poll kicked,
 * so its interrupt ends the poll before a halt can follow it. Only a poll
 * which finds no wake stops the tick for a halt. A stopped tick skips the
 * poll, committing its deferred events before another halt. No APIC register
 * read is needed. A kick received while awake ends this wait before polling
 * or halting; absorbed service kicks also install their published target.
 *
 * A kernel wait with its tick running polls with interrupts off: poll this
 * processor's `idle_poll` for `idle_poll_us`, then halt unless a wake turned
 * it to CPU_POLL_KICKED meanwhile (`smp_kick`). A kick that comes after the
 * poll finds it off and sends the interrupt that ends the halt. The poll
 * answers the requests other processors leave (`smp_service`), as a wait for
 * the big lock does, so that their interrupts, pending meanwhile, are taken
 * once at its end: taken one by one inside the poll, as Linux's poll_idle
 * takes them with interrupts on (drivers/cpuidle/poll_state.c:22-41), each
 * cost a guest's interrupt and its exits, and a churn of short processes,
 * whose exits flush every processor, ran 1.5 times as long. Linux sends no
 * such flush to an idle processor in lazy TLB mode (should_flush_tlb,
 * arch/x86/mm/tlb.c: 912-914). A device's completion or a timer's interrupt,
 * which Linux's poll would take at once, the poll looks for every
 * IDLE_POLL_CHECK_US (`arch_irq_waiting`), and halts on it, which takes it
 * while the processor is still marked idle: held to the poll's end, a disk's
 * completion made each of its random reads some 90 us longer, and a tick held
 * past a kick was charged to the thread the processor switched to. A kicked
 * wait likewise lets interrupts in for one instruction before it returns, so
 * that one pending then, seen or not, is taken while the processor is still
 * marked idle, not after its caller waited for the big lock or switched
 * threads.
 *
 * The length adapts as Linux 6.12.115's haltpoll governor adapts its own
 * (adjust_poll_limit, drivers/cpuidle/governors/haltpoll.c): a wait the halt
 * ended within IDLE_POLL_MAX_US doubles it, to at least IDLE_POLL_START_US,
 * and a longer one halves it, to 0 below IDLE_POLL_START_US. A processor whose
 * threads hand work back and forth, as a renderer's do, then takes each wake
 * in the poll, without an interrupt sent under the big lock, a halt and its
 * exit; one whose waits exceed the maximum soon halts at once again. As Linux
 * polls only in a guest, so does this: on a machine of its own an interrupt
 * and a halt cost a few microseconds, and a poll the power of a busy
 * processor. Without a measured counter there is no poll either.
 */
static void idle_halt(int stop_tick)
{
    struct percpu *cpu = this_cpu();
    uint32_t per_us = timer_calibrate() / TICK_US;
    uint64_t start, limit, now, look, gone;
    uint32_t grow;
    int open = stop_tick;
    int polling;

    timer_service_ipi();
    polling = per_us != 0 && arch_idle_poll() && !cpu->tick_stopped && !timer_idle_pending();
    if (arch_atomic_xchg(&cpu->idle_poll, CPU_POLL_NONE) == CPU_POLL_KICKED) {
        timer_service_ipi();
        idle_kicked_window();
        return;
    }
    start = polling ? arch_cycles() : 0;
    if (polling && cpu->idle_poll_us != 0) {
        limit = (uint64_t)cpu->idle_poll_us * per_us;
        look = (uint64_t)IDLE_POLL_CHECK_US * per_us;
        (void)arch_atomic_xchg(&cpu->idle_poll, CPU_POLL_ON);
        if (open) {
            arch_irq_enable();
        }
        for (;;) {
            now = arch_cycles() - start;
            if (cpu->idle_poll != CPU_POLL_ON || now >= limit) {
                break;
            }
            if (!open && now >= look) {
                if (arch_irq_waiting()) {
                    break;
                }
                look = now + (uint64_t)IDLE_POLL_CHECK_US * per_us;
            }
            if (!open) {
                smp_service();
            }
            arch_relax();
        }
        if (open) {
            arch_irq_disable();
        }
        if (arch_atomic_xchg(&cpu->idle_poll, CPU_POLL_NONE) == CPU_POLL_KICKED) {
            timer_service_ipi();
            idle_kicked_window();
            return;
        }
    }
    if (!idle_sleep(stop_tick, per_us)) {
        idle_kicked_window();
        return;
    }
    if (!polling) {
        return;
    }
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
 * Anything on the loopback backlog is delivered before the processor is parked,
 * and this returns without halting so the caller checks its condition again.
 * Every return without a halt, that one and a scheduler idle task's that
 * ready work cancels, lets a pending interrupt in for one instruction first
 * (`idle_kicked_window`), as a kicked halt does, so that it is not held
 * across the caller's next check, its selection or the incoming thread's
 * wait for the big lock, as Linux's `call_cpuidle` enables interrupts when
 * a reschedule request cancels the idle entry (kernel/sched/idle.c:144).
 * The drain's window, and that of ready work found before the marker, open
 * before the marker is set; that of ready work found once it is set opens
 * while the processor is still marked idle.
 * The drain needs the big lock: a caller without it, the idle task, tries
 * the lock, and when another processor holds it leaves the backlog to that
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
    int depth, scheduler_idle;

    if (ip_loop_pending()) {
        if (this_cpu()->bkl_depth > 0) {
            ip_loop_drain_from(IP_LOOP_FROM_IDLE);
            idle_kicked_window();
            return;
        }
        if (bkl_try_enter()) {
            ip_loop_drain_from(IP_LOOP_FROM_IDLE);
            bkl_exit();
            idle_kicked_window();
            return;
        }
    }
    scheduler_idle = sched_is_enabled() && current_proc && current_proc == this_cpu()->idle;
    if (scheduler_idle && current_proc->need_resched) {
        idle_kicked_window();
        return;
    }
    this_cpu()->idle_cycles = arch_cycles();
    this_cpu()->idle_wait = 1;
    depth = bkl_drop();
    if (!scheduler_idle || !current_proc->need_resched) {
        if (scheduler_idle) {
            rcu_idle_enter();
        }
        idle_halt(scheduler_idle);
        if (scheduler_idle) {
            rcu_idle_exit();
        }
        this_cpu()->idle_end_cycles = arch_cycles();
        if (scheduler_idle) {
            sched_idle_account(this_cpu());
            timer_idle_work();
        }
    } else {
        idle_kicked_window();
    }
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
    idle_halt(0);
    this_cpu()->idle_end_cycles = arch_cycles();
    bkl_take_as(depth, BKL_KEY_IDLE);
    this_cpu()->idle_wait = 0;
    kirq_restore(flags);
}
