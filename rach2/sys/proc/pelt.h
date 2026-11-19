/*
 * pelt.h: per-entity load tracking, Linux 6.12's PELT (kernel/sched/pelt.c
 * and the load-average half of fair.c), for the fair class's entities,
 * queues and groups and for each processor's real-time and interrupt time.
 */
#ifndef _PELT_H_
#define _PELT_H_

#include "fair.h"

/*
 * Time is the task clock in nanoseconds, counted in units of 1024 ns; a
 * period is 1024 units, 1,048,576 ns. A sum decays by y each period, with
 * y^32 = 1/2 (`LOAD_AVG_PERIOD`), and `LOAD_AVG_MAX` is the sum of
 * 1024 * y^n over all n, the most a sum reaches (sched-pelt.h).
 */
#define LOAD_AVG_PERIOD  32
#define LOAD_AVG_MAX     47742
#define PELT_MIN_DIVIDER (LOAD_AVG_MAX - 1024)

/* The full capacity of a processor, Linux's SCHED_CAPACITY_SCALE: a
 * utilization of 1024 is a processor busy all the time. */
#define SCHED_CAPACITY_SHIFT 10
#define SCHED_CAPACITY_SCALE (1L << SCHED_CAPACITY_SHIFT)

/* util_est's ewma weight, 1/4, and the bit that marks an estimate whose
 * util_avg has not changed since the last dequeue (include/linux/sched.h). */
#define UTIL_EST_WEIGHT_SHIFT 2
#define UTIL_AVG_UNCHANGED    0x80000000u
#define UTIL_EST_MARGIN       (SCHED_CAPACITY_SCALE / 100)

/**
 * The divider that turns a sum into an average: the most a sum reaches at
 * the averages' place in the current period, LOAD_AVG_MAX - 1024 plus the
 * part of the period passed (pelt.h's get_pelt_divider).
 */
static __inline__ uint32_t pelt_divider(const struct sched_avg *sa)
{
    return PELT_MIN_DIVIDER + sa->period_contrib;
}

/** `val` times y^n, in constant work: whole halvings by shift, the rest
 * from the table (decay_load). 0 past 63 half-lives. */
uint64_t pelt_decay_load(uint64_t val, uint64_t n);

/**
 * Age the sums of `sa` to the task clock `now`, in ns, with `load`,
 * `runnable` and `running` the entity's state over the time since its last
 * update, and return 1 when a period boundary passed, so that the averages
 * must be computed again by `pelt_update_avg` (___update_load_sum). A clock
 * that went back moves only the update time.
 */
int pelt_update_sum(uint64_t now, struct sched_avg *sa, unsigned long load, unsigned long runnable,
                    int running);

/** Compute the averages of `sa` from its sums, the load weighted by
 * `load` (___update_load_avg). */
void pelt_update_avg(struct sched_avg *sa, unsigned long load);

/*
 * The entity and queue updates of pelt.c. Each runs under sched_lock with
 * interrupts off, `now` being the processor's task clock, as Linux's run
 * under the run queue's lock.
 */

/** Age a sleeping entity's averages to `now` (__update_load_avg_blocked_se). */
int pelt_update_blocked_se(uint64_t now, struct sched_entity *se);

/** Age an entity of `cfs_rq` to `now`, runnable while it is on the queue and
 * running while it is the queue's current (__update_load_avg_se). */
int pelt_update_se(uint64_t now, struct cfs_rq *cfs_rq, struct sched_entity *se);

/** Age a queue's own sums to `now` (__update_load_avg_cfs_rq). */
int pelt_update_cfs_rq(uint64_t now, struct cfs_rq *cfs_rq);

/**
 * Age processor `cpu`'s real-time average to `now`, with `running` nonzero
 * while a real-time or deadline thread ran there since the last call
 * (update_rt_rq_load_avg), so that the balancer leaves the capacity real-time
 * threads take out of the fair class's (`pelt_cpu_util_rt`).
 */
int pelt_update_rt(int cpu, uint64_t now, int running);

/**
 * Add `running` ns of interrupt time to processor `cpu`'s interrupt average,
 * with the monotonic clock at `clock` (update_irq_load_avg): the time is
 * taken to have come just before `clock`, the rest as idle.
 */
int pelt_update_irq(int cpu, uint64_t clock, uint64_t running);

/** The utilization of real-time threads and of interrupts on `cpu`, 0 to
 * SCHED_CAPACITY_SCALE. */
unsigned long pelt_cpu_util_rt(int cpu);
unsigned long pelt_cpu_util_irq(int cpu);

/** Clear an entity's averages for its first enqueue, a task's load at its
 * full weight (init_entity_runnable_average). */
void pelt_init_entity(struct sched_entity *se);

/**
 * Give a new task's entity, already on its queue's processor, half the spare
 * capacity of that processor as its utilization, or its share of the
 * queue's utilization by weight when that is less (post_init_entity_util_avg).
 */
void pelt_post_init_entity(struct sched_entity *se);

/** Add an entity's averages to its queue's (enqueue_load_avg). */
void pelt_enqueue_load(struct cfs_rq *cfs_rq, struct sched_entity *se);

/** Take an entity's averages out of its queue's (dequeue_load_avg). */
void pelt_dequeue_load(struct cfs_rq *cfs_rq, struct sched_entity *se);

/**
 * Age the entity `se` of `cfs_rq` and the queue to the task clock `now`,
 * fold in a group's pending change, and attach or detach the entity as
 * `flags` asks (update_load_avg). An entity whose `avg.last_update_time` is
 * 0 has migrated and is attached on FAIR_LOAD_ATTACH.
 */
void pelt_update_load_avg(uint64_t now, struct cfs_rq *cfs_rq, struct sched_entity *se, int flags);

/** Age a queue to `now` and take out what left it since its last update
 * (update_cfs_rq_load_avg); 1 when its load changed. */
int pelt_update_cfs_rq_load(uint64_t now, struct cfs_rq *cfs_rq);

/** Join an entity to its queue's averages, aligned to the queue's period
 * (attach_entity_load_avg); the queue must be aged to now first. */
void pelt_attach_entity(struct cfs_rq *cfs_rq, struct sched_entity *se);

/** Take an entity out of its queue's averages (detach_entity_load_avg). */
void pelt_detach_entity(struct cfs_rq *cfs_rq, struct sched_entity *se);

/** Age a sleeping entity to its queue's last update (sync_entity_load_avg). */
void pelt_sync_entity(struct sched_entity *se);

/**
 * Note a sleeping entity's averages for its queue to take out at its next
 * update, as a task does that leaves the processor asleep or dies
 * (remove_entity_load_avg). Linux's `removed.lock` is sched_lock here.
 */
void pelt_remove_entity(struct sched_entity *se);

/**
 * Age a sleeping task's entity, leaving a processor where no fair thread
 * runs, to that processor's task clock, so that it takes no stale load with
 * it (migrate_se_pelt_lag). The caller has updated that clock.
 */
void pelt_migrate_lag(struct sched_entity *se);

/** Add a task's estimated utilization to its queue's on enqueue, and take it
 * out on dequeue (util_est_enqueue, util_est_dequeue). */
void pelt_util_est_enqueue(struct cfs_rq *cfs_rq, struct sched_entity *se);
void pelt_util_est_dequeue(struct cfs_rq *cfs_rq, struct sched_entity *se);

/** Update a task's ewma of its utilization at a sleep (`sleep` nonzero), the
 * only dequeue that changes it (util_est_update). */
void pelt_util_est_update(struct cfs_rq *cfs_rq, struct sched_entity *se, int sleep);

/** A task's utilization, its estimate at least (task_util_est). */
unsigned long pelt_task_util_est(const struct sched_entity *se);

/** Pass a group queue's load average to its group's sum when it has moved
 * by more than 1/64 and at most once per ms (update_tg_load_avg). */
void pelt_update_tg_load(struct cfs_rq *cfs_rq, uint64_t now);

/** Take a group queue's whole share out of its group's sum, as the queue's
 * processor goes offline (clear_tg_load_avg). */
void pelt_clear_tg_load(struct cfs_rq *cfs_rq, uint64_t now);

/** Whether a queue has nothing left to decay, no load and no child on the
 * leaf list, and may leave that list (cfs_rq_is_decayed). */
int pelt_cfs_rq_decayed(const struct cfs_rq *cfs_rq);

/** Whether a queue still holds blocked load or utilization. */
int pelt_cfs_rq_has_blocked(const struct cfs_rq *cfs_rq);

/**
 * Decay the blocked averages of processor `cpu`'s queues, its real-time and
 * interrupt averages and every queue on its leaf list, to its task clock
 * `now`, `rt_running` nonzero while a real-time thread runs there
 * (sched_balance_update_blocked_averages); 1 while load is still left to
 * decay.
 */
int pelt_update_blocked(int cpu, uint64_t now, int rt_running);

/** A task's load as its processor's root queue sees it, through its groups'
 * shares (task_h_load); `tick` is the tick count, by which the hierarchy's
 * loads are computed once per tick. */
unsigned long pelt_task_h_load(struct sched_entity *se, uint64_t tick);

/**
 * Fill `fair_hooks`' load members with PELT's: the update, the reweight,
 * the removal and a new thread's averages. Once, at boot, after `fair_init`.
 */
void pelt_init(void);

#endif /* _PELT_H_ */
