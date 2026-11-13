/* Read-copy update's grace periods and callbacks (rcu.h, rcu.Readme.md). */
#include "rcu.h"
#include "kirq.h"
#include "spinlock.h"
#include "../arch/arch.h"
#include "../proc/percpu.h"
#ifndef RACH2_HOSTTEST
#include "kstring.h"
#include "../panic.h"
#include "../proc/process.h"
#include "../proc/sched.h"
#include "../proc/smp.h"
#include "../proc/wait_queue.h"
#include "../timer.h"
#endif

/*
 * A processor's quiescent-state count, alone in 64 bytes so that two
 * processors' counts never share a cache line. Only its own processor
 * writes it, with interrupts off, by an exchange that is a full barrier, so
 * the reads of a read section that follows cannot pass the write; the
 * grace-period thread reads it. It goes up by 2 at each quiescent state and
 * by 1 as an idle wait begins and again as it ends, so it is odd while the
 * processor is idle, as Linux's count of a processor's extended quiescent
 * states marks one (`rcu_watching_snap_in_eqs`, kernel/rcu/tree.c:306).
 */
union rcu_count {
    volatile unsigned int seq;
    char line[64];
};

static union rcu_count rcu_counts[NR_CPUS];

/*
 * A processor's callbacks, two lists in the order they were queued: `next`,
 * queued since the grace period under way began, which wait for the next
 * one to begin, and `wait`, those of the grace period under way, as Linux's
 * RCU_NEXT_TAIL and RCU_WAIT_TAIL segments. `lock` guards both; `call_rcu`
 * takes it on its own processor and the grace-period thread on each in
 * turn. `snap` and `watched` are the thread's alone: the count the grace
 * period began with, and whether the grace period still waits for the
 * processor.
 */
struct rcu_cpu {
    struct spinlock lock;
    struct rcu_head *next, *next_last;
    struct rcu_head *wait, *wait_last;
    unsigned int snap;
    int watched;
};

static struct rcu_cpu rcu_cpus[NR_CPUS];

/* The grace periods ended, which only the grace-period thread counts. */
static volatile uint32_t gp_done;

/** A full barrier: a locked exchange, as `mb_here` in proc/membarrier.c. */
static void rcu_mb(void)
{
    volatile unsigned int word = 0;

    (void)arch_atomic_xchg(&word, 0);
}

/** Add `n` to the running processor's count with interrupts off: no
 * interrupt on this processor comes between the read and the exchange. */
static void count_add(unsigned int n)
{
    volatile unsigned int *seq = &rcu_counts[arch_cpu_id()].seq;

    (void)arch_atomic_xchg(seq, *seq + n);
}

void rcu_qs(void)
{
    count_add(2u);
}

void rcu_tick(int user)
{
    if (user) {
        count_add(2u);
    }
}

void rcu_idle_enter(void)
{
    count_add(1u);
}

void rcu_idle_exit(void)
{
    count_add(1u);
}

uint32_t rcu_gp_completed(void)
{
    return gp_done;
}

/** Append the list from `first` to `last` to the list `*head`..`*tail`. */
static void list_join(struct rcu_head **head, struct rcu_head **tail, struct rcu_head *first,
                      struct rcu_head *last)
{
    if (!first) {
        return;
    }
    if (*tail) {
        (*tail)->next = first;
    } else {
        *head = first;
    }
    *tail = last;
}

#ifndef RACH2_HOSTTEST
static void gp_kick(void);
#else
/** The host tests run the grace periods themselves. */
static void gp_kick(void)
{}
#endif

void call_rcu(struct rcu_head *head, void (*func)(struct rcu_head *head))
{
    struct rcu_cpu *c = &rcu_cpus[arch_cpu_id()];
    uint32_t flags;

    head->func = func;
    head->next = NULL;
    flags = spin_lock_irqsave(&c->lock);
    list_join(&c->next, &c->next_last, head, head);
    spin_unlock_irqrestore(&c->lock, flags);
    gp_kick();
}

/**
 * The callbacks move under each processor's lock, so an updater's stores
 * before its `call_rcu`, the unlinking of what the callback frees among
 * them, are seen by every read below; the barrier keeps the counts' reads
 * after them, as Linux orders the start of a grace period after the
 * callbacks it covers (`rcu_gp_init`, kernel/rcu/tree.c:1800).
 */
int rcu_gp_begin(void)
{
    int c, any = 0;

    for (c = 0; c < NR_CPUS; c++) {
        struct rcu_cpu *r = &rcu_cpus[c];
        uint32_t flags = spin_lock_irqsave(&r->lock);

        if (r->next) {
            list_join(&r->wait, &r->wait_last, r->next, r->next_last);
            r->next = NULL;
            r->next_last = NULL;
            any = 1;
        }
        spin_unlock_irqrestore(&r->lock, flags);
    }
    if (!any) {
        return 0;
    }
    rcu_mb();
    for (c = 0; c < NR_CPUS; c++) {
        rcu_cpus[c].watched = cpu_table[c].online != 0;
        rcu_cpus[c].snap = rcu_counts[c].seq;
    }
    return 1;
}

/**
 * A processor has passed a quiescent state once its count has moved from
 * the grace period's first reading, or when that reading was odd, made in
 * an idle wait, in which it can be in no read section; one that has gone
 * offline is waited for no more, as Linux's `rcutree_report_cpu_dead`
 * takes a processor out of the grace periods (kernel/rcu/tree.c:5207).
 * The exchange that moved the count is a full barrier on that processor,
 * so its read sections before it have ended for every reader of the
 * count.
 */
int rcu_gp_ended(void)
{
    int c;

    for (c = 0; c < NR_CPUS; c++) {
        struct rcu_cpu *r = &rcu_cpus[c];

        if (!r->watched) {
            continue;
        }
        if (!cpu_table[c].online || (r->snap & 1u) || rcu_counts[c].seq != r->snap) {
            r->watched = 0;
            continue;
        }
        return 0;
    }
    return 1;
}

/**
 * Every processor's list is run, an offline processor's too, so a callback
 * queued on a processor that went offline is not lost, as Linux moves such
 * a processor's callbacks to a live one (`rcutree_migrate_callbacks`,
 * kernel/rcu/tree.c:5248). The
 * barrier orders the frees after the counts' reads that ended the grace
 * period.
 */
void rcu_gp_finish(void)
{
    int c;

    rcu_mb();
    gp_done++;
    for (c = 0; c < NR_CPUS; c++) {
        struct rcu_cpu *r = &rcu_cpus[c];
        struct rcu_head *h, *n;
        uint32_t flags = spin_lock_irqsave(&r->lock);

        h = r->wait;
        r->wait = NULL;
        r->wait_last = NULL;
        spin_unlock_irqrestore(&r->lock, flags);
        for (; h; h = n) {
            n = h->next;
            h->func(h);
        }
    }
}

#ifndef RACH2_HOSTTEST

/* The grace-period thread, and the queue it sleeps on for callbacks and
 * for the ticks of a grace period under way. */
static process_t *gp_thread;
static wait_queue_t gp_wait;

/** Wake the grace-period thread for a callback; a wake it does not need
 * costs it one look. */
static void gp_kick(void)
{
    if (gp_thread) {
        wq_wake(&gp_wait, 0);
    }
}

/** Whether a callback waits on some processor for a grace period to begin;
 * read without the locks, since `call_rcu` wakes the thread after its
 * callback is on its list. */
static int gp_wanted(void *arg)
{
    int c;

    (void)arg;
    for (c = 0; c < NR_CPUS; c++) {
        if (*(struct rcu_head *const volatile *)&rcu_cpus[c].next) {
            return 1;
        }
    }
    return 0;
}

/** Whether the tick `*arg` has come. */
static int gp_tick_due(void *arg)
{
    return tick_count() >= *(const uint64_t *)arg;
}

/**
 * The grace-period thread, Linux's `rcu_gp_kthread`
 * (kernel/rcu/tree.c:2229): sleep until a callback is queued, begin a grace
 * period, look at each tick whether it has ended, and run its callbacks. It
 * gives up the big lock it starts with, since no step needs it. Its own
 * sleeps are its processor's quiescent states.
 */
static void gp_main(void)
{
    uint64_t due;
    uint32_t flags;

    bkl_exit();
    arch_irq_enable();
    for (;;) {
        (void)wq_wait_event_idle(&gp_wait, gp_wanted, NULL);
        if (!rcu_gp_begin()) {
            continue;
        }
        while (!rcu_gp_ended()) {
            flags = kirq_save();
            due = tick_count() + 1u;
            current_proc->wakeup_tick = due;
            kirq_restore(flags);
            (void)wq_wait_event_idle(&gp_wait, gp_tick_due, &due);
            current_proc->wakeup_tick = 0;
        }
        rcu_gp_finish();
    }
}

/* A `synchronize_rcu` under way: the callback marks it done and wakes the
 * caller, whose stack holds it. */
struct rcu_sync {
    struct rcu_head head; /* first, so that the callback's head is the record */
    wait_queue_t wq;
    volatile int done;
};

/** The callback of `synchronize_rcu`: `wq_complete` lets go of the record
 * before its caller can return. */
static void sync_done(struct rcu_head *head)
{
    struct rcu_sync *s = (struct rcu_sync *)head;

    wq_complete(&s->wq, &s->done);
}

/** Whether the grace period of `synchronize_rcu`'s record `arg` has ended. */
static int sync_ended(void *arg)
{
    return ((const struct rcu_sync *)arg)->done;
}

/** A grace period is a callback's: the caller sleeps until it runs, as
 * Linux's `wait_rcu_gp`. Before the thread starts only the boot processor
 * runs. */
void synchronize_rcu(void)
{
    struct rcu_sync s;

    if (!gp_thread || smp_cpu_count() < 2) {
        return;
    }
    k_memset(&s, 0, sizeof(s));
    wait_queue_init(&s.wq);
    call_rcu(&s.head, sync_done);
    (void)wq_wait_event_nointr(&s.wq, sync_ended, &s);
}

#endif /* !RACH2_HOSTTEST */

/** The thread is made as the lockup detector's are (watchdog.c); a boot
 * that cannot make it stops, since no callback would ever run. */
void rcu_init(void)
{
    int c;
#ifndef RACH2_HOSTTEST
    process_t *p;
    uintptr_t base;
    uint32_t flags;
#endif

    for (c = 0; c < NR_CPUS; c++) {
        spin_lock_init(&rcu_cpus[c].lock, "rcu_cpu->lock");
    }
#ifndef RACH2_HOSTTEST
    wait_queue_init(&gp_wait);
    flags = kirq_save();
    base = kstack_alloc();
    p = base ? alloc_process() : NULL;
    if (!p) {
        if (base) {
            kstack_free(base + KERNEL_STACK_SIZE);
        }
        kirq_restore(flags);
        k_panic("rcu: rcu_sched: no record or stack for it");
    }
    p->kernel_stack = base + KERNEL_STACK_SIZE;
    p->space_root = 0;
    p->page_dir = NULL;
    p->parent_pid = 0;
    proc_set_comm(p, "rcu_sched");
    setup_process_stack(p, gp_main);
    gp_thread = p;
    sched_make_ready(p);
    kirq_restore(flags);
#endif
}
