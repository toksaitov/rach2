/*
 * fair_group.c: group scheduling for the fair class, a group's queue and
 * entity on each processor and the group entity's weight from its share
 * (Linux 6.12's calc_group_shares and update_cfs_group).
 */

#include "autogroup.h"

#include "fair.h"
#include "percpu.h"
#include "process.h"
#include "sched.h"
#include "../timer.h"
#include "../mm/kheap.h"

/* The least and the most share of a group, Linux's MIN_SHARES and
 * MAX_SHARES (sched.h:534), unscaled as fair.h keeps every weight. */
#define FAIR_MIN_SHARES 2ul
#define FAIR_MAX_SHARES (1ul << 18)

void (*fair_group_clock)(int cpu);

/**
 * The weight of a group's entity on one processor: the group's share split
 * by the load of its queue there, as Linux's calc_group_shares
 * (fair.c:4030), whose comment derives it:
 *
 *                  tg->shares * load
 *     weight = -------------------------------------------------
 *              tg->load_avg - cfs_rq->tg_load_avg_contrib + load
 *
 * with load = max(cfs_rq->load.weight, cfs_rq->avg.load_avg), clamped to
 * FAIR_MIN_SHARES and tg->shares. The denominator stands in for the sum of
 * the group's loads on every processor, this one's taken at its present
 * weight so that a group that starts on an idle machine gets its whole
 * share at once. The product is taken in 64 bits, since a share reaches
 * 88761 at nice -20 and a load 2^17 with a few threads, past a 32-bit long;
 * the division truncates, as Linux's does. A denominator of 0 leaves the
 * product undivided, as Linux's `if (tg_weight)`.
 */
static unsigned long calc_group_shares(struct cfs_rq *cfs_rq)
{
    struct task_group *tg = cfs_rq->tg;
    unsigned long tg_shares = tg->shares;
    unsigned long load = cfs_rq->load.weight;
    long tg_weight;
    uint64_t shares;

    if (cfs_rq->avg.load_avg > load) {
        load = cfs_rq->avg.load_avg;
    }
    tg_weight = tg->load_avg - (long)cfs_rq->tg_load_avg_contrib + (long)load;
    shares = (uint64_t)tg_shares * load;
    if (tg_weight > 0) {
        shares = tick_div64(shares, (uint64_t)tg_weight, NULL);
    }
    if (shares < FAIR_MIN_SHARES) {
        return FAIR_MIN_SHARES;
    }
    if (shares > tg_shares) {
        return tg_shares;
    }
    return (unsigned long)shares;
}

/** Give the group entity `se` the weight its group's share and its queue's
 * load call for, as Linux's update_cfs_group (fair.c:4069): the hook
 * `fair_hooks.group_update`. A thread's entity, and a group entity whose
 * queue is empty, which keeps its weight for DELAY_DEQUEUE, are left. With
 * sched_lock held. */
static void update_cfs_group(struct sched_entity *se)
{
    struct cfs_rq *gcfs_rq = se->my_q;
    unsigned long shares;

    if (!gcfs_rq || !gcfs_rq->load.weight) {
        return;
    }
    shares = calc_group_shares(gcfs_rq);
    if (se->load.weight != shares) {
        fair_reweight_entity(se->cfs_rq, se, shares);
    }
}

/** Set the group hook; the contract is in autogroup.h. */
void fair_group_init(void)
{
    fair_hooks.group_update = update_cfs_group;
}

/** Free what `tg_create` allocated for `tg`, which is on no list. */
static void tg_free_memory(struct task_group *tg)
{
    int c;

    if (tg->se && tg->cfs_rq) {
        for (c = 0; c < NR_CPUS; c++) {
            kfree(tg->cfs_rq[c]);
            kfree(tg->se[c]);
        }
    }
    kfree(tg->cfs_rq);
    kfree(tg->se);
    kfree(tg);
}

/** Set `tg`'s queue `cfs_rq` and entity `se` on `cpu`, as Linux's
 * init_tg_cfs_entry (fair.c:13557): the entity sits in the root queue of
 * `cpu` and owns `cfs_rq`, with nice 0's weight until its first share, so
 * that a group entity always has weight. */
static void tg_init_entry(struct task_group *tg, struct cfs_rq *cfs_rq, struct sched_entity *se,
                          int cpu)
{
    fair_cfs_rq_init(cfs_rq, tg, cpu);
    tg->cfs_rq[cpu] = cfs_rq;
    tg->se[cpu] = se;
    se->cfs_rq = &fair_rqs[cpu].cfs;
    se->depth = 0;
    se->parent = NULL;
    se->my_q = cfs_rq;
    se->load.weight = NICE_0_LOAD;
    se->load.inv_weight = 0;
}

/** A new group under the root group with nice 0's share, or NULL with no
 * memory; the contract is in autogroup.h. */
struct task_group *tg_create(void)
{
    struct task_group *tg = kzalloc(sizeof(*tg));
    struct cfs_rq *cfs_rq;
    struct sched_entity *se;
    uint32_t flags;
    int c;

    if (!tg) {
        return NULL;
    }
    tg->se = kzalloc(NR_CPUS * sizeof(*tg->se));
    tg->cfs_rq = kzalloc(NR_CPUS * sizeof(*tg->cfs_rq));
    if (!tg->se || !tg->cfs_rq) {
        tg_free_memory(tg);
        return NULL;
    }
    tg->shares = NICE_0_LOAD;
    tg->parent = &root_task_group;
    for (c = 0; c < NR_CPUS; c++) {
        if (!cpu_table[c].online) {
            continue;
        }
        cfs_rq = kzalloc(sizeof(*cfs_rq));
        se = kzalloc(sizeof(*se));
        if (!cfs_rq || !se) {
            kfree(cfs_rq);
            kfree(se);
            tg_free_memory(tg);
            return NULL;
        }
        tg_init_entry(tg, cfs_rq, se, c);
    }
    /* As Linux's alloc_fair_sched_group and online_fair_sched_group
     * (fair.c:13464, 13505): each entity's averages start, and join its
     * root queue's. */
    flags = sched_lock_take();
    for (c = 0; c < NR_CPUS; c++) {
        se = tg->se[c];
        if (!se) {
            continue;
        }
        if (fair_group_clock) {
            fair_group_clock(c);
        }
        if (fair_hooks.load_init) {
            fair_hooks.load_init(se);
        }
        if (fair_hooks.load_update) {
            fair_hooks.load_update(se->cfs_rq, se, FAIR_LOAD_ATTACH);
        }
    }
    tg->list_next = root_task_group.list_next;
    root_task_group.list_next = tg;
    sched_lock_give(flags);
    return tg;
}

/** Take `tg`, which no thread is in, off every queue and free it; the
 * contract is in autogroup.h. */
void tg_free(struct task_group *tg)
{
    struct task_group **link;
    struct sched_entity *se;
    uint32_t flags;
    int c;

    /* As Linux's sched_release_group and unregister_fair_sched_group
     * (core.c:8960, fair.c:13523). */
    flags = sched_lock_take();
    for (link = &root_task_group.list_next; *link; link = &(*link)->list_next) {
        if (*link == tg) {
            *link = tg->list_next;
            break;
        }
    }
    for (c = 0; c < NR_CPUS; c++) {
        se = tg->se[c];
        if (!se) {
            continue;
        }
        if (fair_group_clock) {
            fair_group_clock(c);
        }
        if (se->sched_delayed) {
            fair_finish_delayed(se);
        }
        if (tg->cfs_rq[c]->on_list) {
            fair_leaf_del(tg->cfs_rq[c]);
        }
        if (fair_hooks.load_remove && se->avg.last_update_time) {
            fair_hooks.load_remove(se);
        }
    }
    sched_lock_give(flags);
    tg_free_memory(tg);
}

/** Set `tg`'s share and give its entities their weights again; the
 * contract is in autogroup.h. */
void tg_set_shares(struct task_group *tg, unsigned long shares)
{
    struct sched_entity *se;
    uint32_t flags;
    int c;

    if (shares < FAIR_MIN_SHARES) {
        shares = FAIR_MIN_SHARES;
    }
    if (shares > FAIR_MAX_SHARES) {
        shares = FAIR_MAX_SHARES;
    }
    flags = sched_lock_take();
    if (tg->shares != shares) {
        tg->shares = shares;
        /* As Linux's __sched_group_set_shares (fair.c:13590): the
         * averages up to now, then each level's weight. */
        for (c = 0; c < NR_CPUS; c++) {
            if (!tg->se[c]) {
                continue;
            }
            if (fair_group_clock) {
                fair_group_clock(c);
            }
            for (se = tg->se[c]; se; se = se->parent) {
                if (fair_hooks.load_update) {
                    fair_hooks.load_update(se->cfs_rq, se, FAIR_LOAD_UPDATE_TG);
                }
                update_cfs_group(se);
            }
        }
    }
    sched_lock_give(flags);
}

/**
 * Move `p`'s entity to the queue of the group `fair_task_group` names, on
 * its processor, as Linux's sched_move_task (core.c:9012) with
 * task_change_group_fair (fair.c:13430). A thread not yet placed is left
 * for its first `fair_set_task_rq`. A queued entity is dequeued and
 * enqueued again, except one whose dequeue was delayed, which the move
 * finishes, so that the thread is blocked; the running one is put back and
 * set again, and its processor asked to select, since its new group may
 * owe it less. A fair thread's averages leave one queue's and join the
 * other's; a real-time thread's entity is only pointed at the new queue,
 * as Linux's set_task_rq does for a class without task_change_group.
 */
void tg_move_task(struct process *p, struct task_group *tg)
{
    struct sched_entity *se = &p->se;
    int queued, delayed, running, fair, cpu = p->cpu;
    uint64_t kick = 0;
    uint32_t flags;

    flags = sched_lock_take();
    p->sched_task_group = tg;
    if (!se->cfs_rq || se->cfs_rq->tg == fair_task_group(p)) {
        sched_lock_give(flags);
        return;
    }
    if (fair_group_clock) {
        fair_group_clock(cpu);
    }
    queued = se->on_rq;
    delayed = se->sched_delayed;
    running = se->cfs_rq->curr == se;
    fair = sched_priority(p) == 0;
    if (queued) {
        fair_dequeue(p, FAIR_DEQ_MOVE);
        if (delayed && fair_hooks.blocked) {
            fair_hooks.blocked(p);
        }
    }
    if (running) {
        fair_put_prev(p);
    }
    if (fair && se->avg.last_update_time && fair_hooks.load_update) {
        fair_hooks.load_update(se->cfs_rq, se, FAIR_LOAD_DETACH);
    }
    if (fair) {
        se->avg.last_update_time = 0;
    }
    fair_set_task_rq(p, cpu);
    if (fair && fair_hooks.load_update) {
        fair_hooks.load_update(se->cfs_rq, se, FAIR_LOAD_ATTACH);
    }
    if (queued && !delayed) {
        fair_enqueue(p, FAIR_ENQ_RESTORE);
    }
    if (running) {
        fair_set_next(p);
        fair_rqs[cpu].resched = 1;
        if (cpu != (int)arch_cpu_id()) {
            kick = (uint64_t)1 << cpu;
        }
    }
    sched_lock_give(flags);
    if (kick) {
        sched_kick_cpus(kick);
    }
}
