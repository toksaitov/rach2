/*
 * fair.h: Linux 6.12's fair scheduling class, EEVDF over a red-black tree of
 * virtual deadlines, with group scheduling: the structures and hooks that
 * fair.c, fair_group.c, pelt.c, balance.c, autogroup.c and sched.c share.
 */
#ifndef _FAIR_H_
#define _FAIR_H_

#include "../lib/kdef.h"
#include "../lib/kint.h"
#include "../lib/rbtree.h"
#include "percpu.h"

struct process;
struct task_group;
struct autogroup;

/*
 * Units and bounds. Every time below is in nanoseconds. A virtual time
 * (`vruntime`, `deadline`, `min_vruntime`, `zero_vruntime`) is a 64-bit count
 * that wraps; two are compared by the sign of their difference, as Linux
 * does, so any two compared are less than 2^63 apart.
 *
 * Weights are Linux's unscaled ones, NICE_0_LOAD 1024 at nice 0, as a 32-bit
 * Linux keeps them on both architectures; a 64-bit Linux scales them by
 * 2^10 and scales them down again before every product below, so the
 * results are the same. A thread's weight is at most 88761, under 2^17.
 *
 * The products that must fit in 64 bits, with the bound that keeps each:
 *
 *  - `cfs_rq.avg_vruntime`, the sum of key * weight over the queue, where a
 *    key is a virtual runtime less `zero_vruntime`. An entity's key stays
 *    within about two lag limits, and its lag limit times its weight is at
 *    most 2 * 100 ms * NICE_0_LOAD, under 2^38, so the sum stays under 2^62
 *    for fewer than 2^24 entities on one queue, as Linux argues beside
 *    `entity_key` (fair.c:554).
 *  - `place_entity`'s lag * (load + weight) and `reweight_eevdf`'s lag and
 *    slice times the old weight: a lag within its limit times a weight is
 *    under 2^38, and the queue's load over the entity's weight is under
 *    2^24 for the same count.
 *  - `__calc_delta`'s delta * fact >> shift, done by `fair_mul_u64_u32_shr`
 *    in 96 bits, so it never overflows for a delta under 2^64.
 *
 * A signed division rounds toward zero (`fair_div_s64`), as Linux's div_s64;
 * where Linux floors, as in `avg_vruntime`, the code says so beside it.
 */

/* The weight of nice 0, and SCHED_IDLE's weight and its inverse. */
#define NICE_0_LOAD      1024ul
#define NICE_0_SHIFT     10
#define WEIGHT_IDLEPRIO  3ul
#define WMULT_IDLEPRIO   1431655765u
#define FAIR_WMULT_CONST 0xFFFFFFFFu
#define FAIR_WMULT_SHIFT 32

/* The base slice before its scaling by the processor count (Linux's
 * normalized_sysctl_sched_base_slice), the bounds of a custom slice
 * (sched_setattr's sched_runtime), and the tick of HZ=250, the lag limit's
 * least span. */
#define FAIR_BASE_SLICE_NS 750000u
#define FAIR_SLICE_MIN_NS  100000u
#define FAIR_SLICE_MAX_NS  100000000u
#define FAIR_TICK_NS       4000000u

/* Linux's scheduler features, in the order /sys/kernel/debug/sched/features
 * lists them for Debian's configuration (features.h). `sched_features` holds
 * one bit for each, set for a feature that is on. */
enum fair_feature {
    FEAT_PLACE_LAG,
    FEAT_PLACE_DEADLINE_INITIAL,
    FEAT_PLACE_REL_DEADLINE,
    FEAT_RUN_TO_PARITY,
    FEAT_PREEMPT_SHORT,
    FEAT_NEXT_BUDDY,
    FEAT_PICK_BUDDY,
    FEAT_CACHE_HOT_BUDDY,
    FEAT_DELAY_DEQUEUE,
    FEAT_DELAY_ZERO,
    FEAT_WAKEUP_PREEMPTION,
    FEAT_HRTICK,
    FEAT_HRTICK_DL,
    FEAT_DOUBLE_TICK,
    FEAT_NONTASK_CAPACITY,
    FEAT_TTWU_QUEUE,
    FEAT_SIS_UTIL,
    FEAT_WARN_DOUBLE_CLOCK,
    FEAT_RT_PUSH_IPI,
    FEAT_RT_RUNTIME_SHARE,
    FEAT_LB_MIN,
    FEAT_ATTACH_AGE_LOAD,
    FEAT_WA_IDLE,
    FEAT_WA_WEIGHT,
    FEAT_WA_BIAS,
    FEAT_UTIL_EST,
    FEAT_LATENCY_WARN,
    FEAT_NI_RANDOM,
    FEAT_COUNT
};

/* The features, a bit for each `enum fair_feature`, and their names. */
extern uint32_t sched_features;
extern const char *const sched_feature_names[FEAT_COUNT];
#define sched_feat(f) ((sched_features >> FEAT_##f) & 1u)

/* A weight and its inverse, 2^32 / weight, or 0 until `__update_inv_weight`
 * computes it. */
struct load_weight {
    unsigned long weight;
    uint32_t inv_weight;
};

/*
 * Per-entity load tracking (pelt.c), Linux's struct sched_avg: geometric
 * sums over periods of 1024 units of 1024 ns of the task clock, decaying by
 * half in 32 periods, and the averages derived from them. `last_update_time`
 * is the task clock at the last update, 0 for an entity not attached to a
 * queue's sums.
 */
struct sched_avg {
    uint64_t last_update_time;
    uint64_t load_sum;
    uint64_t runnable_sum;
    uint32_t util_sum;
    uint32_t period_contrib;
    unsigned long load_avg;
    unsigned long runnable_avg;
    unsigned long util_avg;
    unsigned int util_est;
};

/* The schedstats of an entity, Linux's struct sched_statistics: kept while
 * `fair_schedstats` is set, and read by /proc/PID/sched. A `*_start` is the
 * monotonic clock at which the state began, 0 when not in it; a migrating
 * thread's `wait_start` holds the wait so far until its enqueue. */
struct sched_statistics {
    uint64_t wait_start;
    uint64_t wait_max;
    uint64_t wait_count;
    uint64_t wait_sum;
    uint64_t iowait_count;
    uint64_t iowait_sum;
    uint64_t sleep_start;
    uint64_t sleep_max;
    int64_t sum_sleep_runtime;
    uint64_t block_start;
    uint64_t block_max;
    int64_t sum_block_runtime;
    int64_t exec_max;
    uint64_t slice_max;
    uint64_t nr_migrations_cold;
    uint64_t nr_failed_migrations_affine;
    uint64_t nr_failed_migrations_running;
    uint64_t nr_failed_migrations_hot;
    uint64_t nr_forced_migrations;
    uint64_t nr_wakeups;
    uint64_t nr_wakeups_sync;
    uint64_t nr_wakeups_migrate;
    uint64_t nr_wakeups_local;
    uint64_t nr_wakeups_remote;
    uint64_t nr_wakeups_affine;
    uint64_t nr_wakeups_affine_attempts;
    uint64_t nr_wakeups_passive;
    uint64_t nr_wakeups_idle;
};

/*
 * A schedulable entity: a thread, or a group's share of one processor.
 * Every member changes under `sched_lock` with interrupts off.
 *
 * In the tree of its queue, ordered by `deadline`, while it is queued and
 * not its queue's `curr`; `min_vruntime` and `min_slice` are the least
 * `vruntime` and `slice` of its subtree there. `vlag` is V - vruntime saved
 * at a dequeue, clamped to the lag limit, and while the entity is current
 * the deadline it was picked with (RUN_TO_PARITY's protection, Linux's
 * set_protect_slice). `rel_deadline` marks a `deadline` kept relative to
 * `vruntime` across a dequeue that was not a sleep. `custom_slice` marks a
 * slice that sched_setattr set, or a group entity's slice taken from its
 * queue.
 *
 * `exec_start` is the task clock at which charging last stopped, 0 for an
 * entity that migrated and is cache-cold. `sum_exec_runtime` is the charged
 * time since the thread began; `prev_sum_exec_runtime` its value at the last
 * pick.
 *
 * `parent` is the group entity above, NULL for an entity of a root queue;
 * `depth` counts the levels above it. `cfs_rq` is the queue it is on or
 * goes on; `my_q` the queue a group entity owns, NULL for a thread.
 * `runnable_weight` is a group entity's count of runnable threads below,
 * cached for PELT. `tasks_prev` and `tasks_next` link a queued thread on
 * its processor's `cfs_tasks`.
 */
struct sched_entity {
    struct load_weight load;
    struct rb_node run_node;
    uint64_t deadline;
    uint64_t min_vruntime;
    uint64_t min_slice;
    uint8_t on_rq;
    uint8_t sched_delayed;
    uint8_t rel_deadline;
    uint8_t custom_slice;
    uint64_t exec_start;
    uint64_t sum_exec_runtime;
    uint64_t prev_sum_exec_runtime;
    uint64_t vruntime;
    int64_t vlag;
    uint64_t slice;
    uint64_t nr_migrations;
    int depth;
    struct sched_entity *parent;
    struct cfs_rq *cfs_rq;
    struct cfs_rq *my_q;
    unsigned long runnable_weight;
    struct sched_entity *tasks_prev, *tasks_next;
    struct sched_avg avg;
    struct sched_statistics stats;
};

/*
 * A fair run queue: the root queue of a processor, or a group's queue on
 * one. Every member changes under `sched_lock` with interrupts off.
 *
 * `load` is the sum of the queued entities' weights, current and delayed
 * ones included; `nr_running` counts those entities. `h_nr_queued` counts
 * the threads queued at or below this queue, delayed ones included, and
 * `h_nr_runnable` those not delayed; `h_nr_delayed` the delayed ones;
 * `idle_nr_running` and `idle_h_nr_running` the SCHED_IDLE ones.
 *
 * `avg_vruntime` is the sum over the tree's entities, the current one
 * excluded, of (vruntime - zero_vruntime) * weight, and `avg_load` the sum
 * of their weights, from which `avg_vruntime()` gives V, the weighted
 * average virtual runtime. `zero_vruntime` follows V at each computation.
 *
 * `curr` is the entity running from this queue, out of the tree, or NULL;
 * `next` the buddy a sleeping group member nominated (PICK_BUDDY).
 *
 * `avg`, `removed` and the members to `h_load_next` are PELT's (pelt.c),
 * `removed` the load of entities that left this queue by migration or
 * death, to be taken from `avg` at its next update. `tg_load_avg_contrib`
 * is this queue's share in its group's `load_avg`.
 *
 * `on_list`, `leaf_prev` and `leaf_next` keep the queue on its processor's
 * list of queues that hold load, child queues before their parents, which
 * the blocked load's update walks. `tg` is the group owning the queue,
 * `root_task_group` for a root queue; `idle` the group's SCHED_IDLE mark.
 */
struct cfs_rq {
    struct load_weight load;
    unsigned int nr_running;
    unsigned int h_nr_queued;
    unsigned int h_nr_runnable;
    unsigned int idle_nr_running;
    unsigned int idle_h_nr_running;
    unsigned int h_nr_delayed;
    int64_t avg_vruntime;
    uint64_t avg_load;
    uint64_t zero_vruntime;
    struct rb_root_cached tasks_timeline;
    struct sched_entity *curr;
    struct sched_entity *next;
    struct sched_avg avg;
    struct {
        int nr;
        unsigned long load_avg;
        unsigned long util_avg;
        unsigned long runnable_avg;
    } removed;
    uint64_t last_update_tg_load_avg;
    unsigned long tg_load_avg_contrib;
    long propagate;
    long prop_runnable_sum;
    unsigned long h_load;
    uint64_t last_h_load_update;
    struct sched_entity *h_load_next;
    int cpu;
    int on_list;
    struct cfs_rq *leaf_prev, *leaf_next;
    struct task_group *tg;
    int idle;
};

/*
 * A group of threads scheduled as one entity on each processor, Linux's
 * struct task_group for the fair class: the root group, which owns the
 * processors' root queues and has no entities, and the autogroups
 * (autogroup.c). `se[cpu]` and `cfs_rq[cpu]` are the group's entity and
 * queue on each processor, `se[cpu]` NULL for the root group. `shares` is
 * the group's weight, `load_avg` the sum of its queues'
 * `tg_load_avg_contrib`. `idle` is positive for a SCHED_IDLE group.
 * `autogroup` is the autogroup the group serves, NULL for the root.
 * `list_next` links every group but the root, for walks that visit all.
 */
struct task_group {
    struct sched_entity **se;
    struct cfs_rq **cfs_rq;
    unsigned long shares;
    long load_avg;
    int idle;
    struct task_group *parent;
    struct autogroup *autogroup;
    struct task_group *list_next;
};

/*
 * The fair class's record of one processor, Linux's struct rq for this
 * class. `cfs` is the root queue. `clock` and `clock_task` are the monotonic
 * clock and the task clock (the monotonic clock less the steal and interrupt
 * time) at the caller's last `fair_clock`, which the hooks charge and place
 * by. `curr` is the fair thread `fair_set_next` made current here, NULL
 * when none is. `leaf_head` is the first queue on the processor's leaf list.
 * `cfs_tasks` lists the threads queued here, the most recently picked
 * first, which the load balancer walks (Linux's rq->cfs_tasks).
 * `resched` is set when the class asks the processor to select again, and
 * read and cleared by `fair_resched`. `charged_ns` is the task-clock time
 * charged to fair threads here, for the fair server.
 */
struct fair_rq {
    struct cfs_rq cfs;
    uint64_t clock;
    uint64_t clock_task;
    struct process *curr;
    struct cfs_rq *leaf_head;
    struct sched_entity *cfs_tasks;
    uint64_t charged_ns;
    int resched;
    int cpu;
};

extern struct fair_rq fair_rqs[NR_CPUS];
extern struct task_group root_task_group;

/* The base slice the scheduler uses, sched_getattr reports and
 * base_slice_ns shows: FAIR_BASE_SLICE_NS times 1 + log2 of the processor
 * count capped at 8, Linux's SCHED_TUNABLESCALING_LOG. */
extern uint64_t sysctl_sched_base_slice;

/* Whether the schedstats are kept: kernel.sched_schedstats. */
extern int fair_schedstats;

/* Whether a thread's autogroup decides its queue: when 0, every thread is
 * queued in the root group, each keeping `sched_task_group` for when it is
 * 1 again. Set by autogroup.c from kernel.sched_autogroup_enabled. */
extern int fair_groups_enabled;

/* Linux's sched_prio_to_weight and sched_prio_to_wmult, by nice + 20. */
extern const unsigned long sched_prio_to_weight[40];
extern const uint32_t sched_prio_to_wmult[40];

/*
 * Why a thread is enqueued, a bitwise or. WAKE: it woke from a sleep, its
 * dequeue maybe delayed. NEW: its first enqueue after a fork, placed with
 * half a slice (PLACE_DEADLINE_INITIAL). MIGRATED: it comes from another
 * processor's queue, after `fair_migrate`, and is cache-cold. RESTORE: it
 * comes back after a FAIR_DEQ_MOVE on this processor, keeping its relative
 * deadline.
 */
#define FAIR_ENQ_WAKE     0x01
#define FAIR_ENQ_NEW      0x02
#define FAIR_ENQ_MIGRATED 0x04
#define FAIR_ENQ_RESTORE  0x08

/*
 * Why a thread is dequeued, one of these. SLEEP: it blocks, and an entity
 * that is not eligible stays queued, delayed, until it is (DELAY_DEQUEUE).
 * FORCED: it blocks in a state that must not be delayed, a stop, an exit or
 * suspend's hold. MOVE: it leaves for another queue, another class or a
 * change of group, and comes back with FAIR_ENQ_RESTORE or
 * FAIR_ENQ_MIGRATED, keeping its lag and its deadline relative to its
 * virtual runtime; its load average leaves the queue's sums and joins the
 * next queue's at that enqueue. A FORCED or MOVE dequeue of a delayed
 * thread finishes the delayed dequeue instead, so the thread leaves the
 * queue whole.
 */
#define FAIR_DEQ_SLEEP  1
#define FAIR_DEQ_FORCED 2
#define FAIR_DEQ_MOVE   3

/* The reason of a wake, for placement and wake preemption: Linux's WF_SYNC,
 * WF_FORK, WF_EXEC and WF_TTWU. */
#define FAIR_WF_SYNC 0x01
#define FAIR_WF_FORK 0x02
#define FAIR_WF_EXEC 0x04
#define FAIR_WF_TTWU 0x08

/* The flags of `fair_hooks.load_update`, Linux's update_load_avg flags. */
#define FAIR_LOAD_UPDATE_TG 0x1
#define FAIR_LOAD_SKIP_AGE  0x2
#define FAIR_LOAD_ATTACH    0x4
#define FAIR_LOAD_DETACH    0x8

/*
 * What fair.c calls in the files that build on it, set by their
 * initialisation; a NULL member is skipped. Each runs under `sched_lock`
 * with interrupts off.
 *
 * `load_update`: PELT's update_load_avg of `se` on `cfs_rq` at the task
 * clock, with FAIR_LOAD_* flags, and se_update_runnable. `load_reweight`:
 * after `se->load` changed, take the entity's old load average out of
 * `cfs_rq`'s, compute it again from the new weight, and add it back
 * (reweight_entity's dequeue_load_avg and enqueue_load_avg). `load_remove`:
 * the entity leaves its queue's sums without a queue update, by migration
 * or death (remove_entity_load_avg). `load_init`: a new thread's averages
 * (init_entity_runnable_average and post_init_entity_util_avg).
 *
 * `group_update`: a group entity's weight from its group's share
 * (update_cfs_group), which calls `fair_reweight_entity`.
 *
 * `blocked`: a delayed thread's dequeue finished outside `fair_dequeue`,
 * in `fair_pick`, `fair_migrate` or `fair_finish_delayed`, so the thread
 * is off every fair queue; the scheduler stops counting it as queued.
 */
struct fair_hooks {
    void (*load_update)(struct cfs_rq *cfs_rq, struct sched_entity *se, int flags);
    void (*load_reweight)(struct cfs_rq *cfs_rq, struct sched_entity *se);
    void (*load_remove)(struct sched_entity *se);
    void (*load_init)(struct sched_entity *se);
    void (*group_update)(struct sched_entity *se);
    void (*blocked)(struct process *p);
};

extern struct fair_hooks fair_hooks;

/*
 * The hooks the scheduler calls. Each runs with `sched_lock` held and
 * interrupts off, on the task clock that the caller gave the thread's
 * processor by `fair_clock` since it took the lock. A thread's processor is
 * its `cpu`. None sleeps or allocates.
 */

/** Set up the root queues of processors 0 to `ncpus` - 1 and the base slice
 * for `ncpus` processors. Once, at boot, before the first enqueue. */
void fair_init(unsigned int ncpus);

/** Give processor `cpu` its monotonic clock `now` and task clock `task`,
 * neither going back. */
void fair_clock(int cpu, uint64_t now, uint64_t task);

/** Queue `p`, which is on no fair queue or delayed on one, on its
 * processor's queue in its group, for `why`, FAIR_ENQ_* flags. A delayed
 * thread woken is requeued in place (requeue_delayed_entity). */
void fair_enqueue(struct process *p, int why);

/** Take `p` off its fair queue for `why`, a FAIR_DEQ_*. Returns 1 when the
 * thread left the queue, 0 when its dequeue is delayed and it stays queued
 * with `se.sched_delayed` set, never running, until a wake requeues it or a
 * pick finishes the dequeue. `p` may be current. */
int fair_dequeue(struct process *p, int why);

/** The thread EEVDF picks on processor `cpu`, down the hierarchy, or NULL
 * for an empty queue. It charges the current entities first. A delayed
 * thread it meets has its dequeue finished and `fair_hooks.blocked`
 * called, and the pick retries. The caller follows with `fair_put_prev` of
 * the current fair thread and `fair_set_next` of the pick, and calls
 * neither when the pick is the current thread, which keeps its slice. */
struct process *fair_pick(int cpu);

/** Make `p`, picked on its processor, the current entity at every level,
 * taking it and its group entities out of the trees, and start its slice's
 * protection (RUN_TO_PARITY). */
void fair_set_next(struct process *p);

/** Stop `p` being current on its processor, charging it, and put it and its
 * group entities back in the trees if still queued. */
void fair_put_prev(struct process *p);

/** The tick for `cur`, the current fair thread: charge it and its group
 * entities. Returns nonzero when the processor should select now. */
int fair_tick(struct process *cur);

/** Whether `p`, just woken or placed on `cur`'s processor for the reason
 * `wake_flags`, FAIR_WF_*, should preempt `cur`, the current fair thread
 * there (check_preempt_wakeup_fair). May nominate `p` as the buddy. */
int fair_wake_preempt(struct process *cur, struct process *p, int wake_flags);

/** `cur` yields: its virtual runtime moves to its deadline and it takes a
 * new deadline (yield_task_fair). */
void fair_yield(struct process *cur);

/** Give `p` the weight of its policy and nice, keeping its lag, queued or
 * not (reweight_task_fair). */
void fair_reweight(struct process *p);

/** Set `p`'s slice to `ns` from sched_setattr, or to the base slice for 0,
 * clamped to FAIR_SLICE_MIN_NS and FAIR_SLICE_MAX_NS. */
void fair_set_slice(struct process *p, uint64_t ns);

/** Give `child`, not yet queued, its entity from `parent`'s: weight, slice,
 * group and averages (task_fork_fair, sched_fork's part). Its first
 * enqueue, with FAIR_ENQ_NEW, places it. */
void fair_fork(struct process *child, struct process *parent);

/** Move `p`, on no fair queue, to processor `to`: the only owner of a move's
 * lag and averages (migrate_task_rq_fair). It sets `p`'s queue on `to` but
 * not its `cpu`, which the caller sets, and the caller then enqueues it
 * with FAIR_ENQ_MIGRATED. A delayed thread finishes its dequeue first. */
void fair_migrate(struct process *p, int to);

/** The monotonic nanosecond at which `cur`, the current fair thread, or one
 * of its group entities reaches its deadline, so that the slice event
 * should fire; 0 when nothing competes with it at any level. */
uint64_t fair_next_event_ns(struct process *cur);

/** Whether the class asked processor `cpu` to select again since the last
 * call, clearing the request. */
int fair_resched(int cpu);

/** Point `p`'s entity at its group's queue and entity on processor `cpu`
 * (set_task_rq), for a thread on no fair queue. */
void fair_set_task_rq(struct process *p, int cpu);

/** The group whose queue `p` joins: its autogroup's while
 * `fair_groups_enabled` is set, else the root group. */
struct task_group *fair_task_group(struct process *p);

/** Set a queue to empty, owned by `tg` on processor `cpu`. */
void fair_cfs_rq_init(struct cfs_rq *cfs_rq, struct task_group *tg, int cpu);

/** Give `se`, queued on `cfs_rq` or not, the weight `weight`, keeping its lag
 * and its deadline's distance in real time (reweight_entity). */
void fair_reweight_entity(struct cfs_rq *cfs_rq, struct sched_entity *se, unsigned long weight);

/** Finish the delayed dequeue of `se`, a delayed entity, up the hierarchy
 * (dequeue_entities with DEQUEUE_DELAYED), as a group's teardown needs. */
void fair_finish_delayed(struct sched_entity *se);

/** Put `cfs_rq` on its processor's leaf list, with its parents, a child
 * before its parent; and take it off. */
void fair_leaf_add(struct cfs_rq *cfs_rq);
void fair_leaf_del(struct cfs_rq *cfs_rq);

/** The process an entity belongs to, or NULL for a group entity. */
struct process *fair_task_of(struct sched_entity *se);

/** The weighted average virtual runtime V of `cfs_rq`, moving
 * `zero_vruntime` to it (avg_vruntime). */
uint64_t fair_avg_vruntime(struct cfs_rq *cfs_rq);

/** Whether `se` on `cfs_rq` is owed service, V >= its virtual runtime
 * (entity_eligible). */
int fair_entity_eligible(struct cfs_rq *cfs_rq, struct sched_entity *se);

/** `delta` times `mul`, shifted right by `shift`, in 96 bits
 * (mul_u64_u32_shr). */
uint64_t fair_mul_u64_u32_shr(uint64_t delta, uint32_t mul, unsigned int shift);

/** `delta` scaled by `weight` over `lw`'s weight (__calc_delta). */
uint64_t fair_calc_delta(uint64_t delta, unsigned long weight, struct load_weight *lw);

/** `n` divided by `d`, rounding toward zero, for any signs (div_s64). `d` is
 * not 0. */
int64_t fair_div_s64(int64_t n, int64_t d);

#endif
