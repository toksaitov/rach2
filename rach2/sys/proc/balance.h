/*
 * balance.h: the fair class's placement and load balancing, Linux 6.12's
 * select_task_rq_fair, sched_balance_rq and the idle balance of nohz, over
 * scheduling domains built from the processors' SMT, cache and package
 * masks (balance.c).
 */
#ifndef _BALANCE_H_
#define _BALANCE_H_

#include "fair.h"

struct process;

/* A domain's flags, Linux's SD_* (include/linux/sched/sd_flags.h), those an
 * x86 machine without NUMA or asymmetric cores uses. */
#define SD_BALANCE_NEWIDLE   0x0001 /* balance when a processor goes idle */
#define SD_BALANCE_EXEC      0x0002 /* place an exec by load */
#define SD_BALANCE_FORK      0x0004 /* place a fork by load */
#define SD_BALANCE_WAKE      0x0008 /* place a wake by load */
#define SD_WAKE_AFFINE       0x0010 /* a wake may go to the waker's processor */
#define SD_SHARE_CPUCAPACITY 0x0020 /* the members are SMT siblings */
#define SD_SHARE_LLC         0x0040 /* the members share a last-level cache */
#define SD_SERIALIZE         0x0080 /* one processor balances it at a time */
#define SD_PREFER_SIBLING    0x0100 /* spread threads to the sibling groups */

/* The flags that need two groups or more to mean anything
 * (SDF_NEEDS_GROUPS). */
#define SD_DEGENERATE_GROUPS_MASK                                               \
    (SD_BALANCE_NEWIDLE | SD_BALANCE_EXEC | SD_BALANCE_FORK | SD_BALANCE_WAKE | \
     SD_SHARE_CPUCAPACITY | SD_SHARE_LLC | SD_SERIALIZE | SD_PREFER_SIBLING)

/* The levels of the topology, bottom up (x86's x86_topology without
 * clusters): SMT siblings, the processors sharing a last-level cache, and
 * the package, which without NUMA is every processor. */
#define SD_LEVEL_SMT 0
#define SD_LEVEL_MC  1
#define SD_LEVEL_PKG 2
#define SD_LEVELS    3

/* What the balance does for a processor, Linux's enum cpu_idle_type. */
#define CPU_IDLE       0 /* idle at the tick, or balancing for an idle one */
#define CPU_NOT_IDLE   1 /* busy at the tick */
#define CPU_NEWLY_IDLE 2 /* about to go idle, its queue empty */

/* Why `balance_ops.kick` asks a processor to run the balance's code. */
#define BALANCE_KICK_NOHZ   1 /* run `fair_nohz_balance` for idle processors */
#define BALANCE_KICK_ACTIVE 2 /* switch, then run `fair_balance_active` */

/*
 * A group of processors in a domain, the span of one child domain, shared
 * by every processor of the domain (struct sched_group with its
 * sched_group_capacity). `capacity` is the sum of the members' capacities,
 * SCHED_CAPACITY_SCALE each less what real-time threads and interrupts take,
 * computed again at `next_update`, in ticks. `imbalance` is set when pinned
 * threads kept a child domain's balance from evening out. `flags` are the
 * child domain's flags, 0 for a group of one processor. `cores` counts the
 * SMT cores among the members.
 */
struct sched_group {
    struct sched_group *next;
    uint64_t span;
    unsigned int group_weight;
    unsigned int cores;
    int flags;
    unsigned long capacity;
    unsigned long min_capacity;
    unsigned long max_capacity;
    uint64_t next_update;
    int imbalance;
};

/*
 * What the processors sharing a last-level cache share (struct
 * sched_domain_shared): how many of them have their tick running, whether
 * an SMT core among them is all idle, and how many processors a wake's scan
 * looks at (SIS_UTIL), set by the balance from the cache domain's
 * utilization.
 */
struct sched_domain_shared {
    int nr_busy_cpus;
    int has_idle_cores;
    int nr_idle_scan;
};

/*
 * A processor's domain at one level (struct sched_domain). `span` is the
 * processors it covers, `groups` its groups, the processor's own first.
 * The intervals are in ms, `last_balance` in ticks; `nr_balance_failed`
 * counts the balances in a row that moved nothing. `max_newidle_lb_cost` is
 * the most a newly idle balance of it took, in ns, decaying by 1% a second
 * from `last_decay_max_lb_cost`, in ticks; `newidle_ratio` is the share of
 * newly idle balances that pulled a thread, out of 1024 (NI_RANDOM).
 */
struct sched_domain {
    struct sched_domain *parent;
    struct sched_domain *child;
    struct sched_group *groups;
    struct sched_domain_shared *shared;
    uint64_t span;
    unsigned int span_weight;
    unsigned int min_interval;
    unsigned int max_interval;
    unsigned int busy_factor;
    unsigned int imbalance_pct;
    unsigned int cache_nice_tries;
    int flags;
    int level;
    int nohz_idle;
    uint64_t last_balance;
    unsigned int balance_interval;
    unsigned int nr_balance_failed;
    uint64_t max_newidle_lb_cost;
    uint64_t last_decay_max_lb_cost;
    unsigned int newidle_call;
    unsigned int newidle_success;
    unsigned int newidle_ratio;
    const char *name;
};

/*
 * What the balance needs from the scheduler (sched.c), each called with
 * `sched_lock` held and interrupts off, and none taking it.
 *
 * `nr_running`: the threads on processor `cpu`'s queues of every class, the
 * running one and delayed fair ones included and its idle task left out
 * (rq->nr_running). `curr`: the thread running on `cpu`, NULL while its idle
 * task does. `idle`: whether `cpu` runs its idle task with nothing queued
 * and no wake on its way (idle_cpu; available_idle_cpu is the same here).
 * `move`: move `p`, a queued fair thread of another processor that is not
 * running, to processor `to`, as detach_task and attach_task do: dequeue it
 * for FAIR_DEQ_MOVE, `fair_migrate`, set its processor, enqueue it with
 * FAIR_ENQ_MIGRATED and ask `to` to select if `p` should run there now.
 * `kick`: ask processor `cpu` to run the balance's code for `why`, a
 * BALANCE_KICK_*, once the lock is let go. `clock`: the monotonic clock in
 * ns, read now, for the cost of a newly idle balance. `update_clock`: give
 * processor `cpu` its clocks as of now by `fair_clock` (update_rq_clock),
 * before the balance reads or moves another processor's threads.
 */
struct balance_ops {
    unsigned int (*nr_running)(int cpu);
    struct process *(*curr)(int cpu);
    int (*idle)(int cpu);
    void (*move)(struct process *p, int to);
    void (*kick)(int cpu, int why);
    uint64_t (*clock)(void);
    void (*update_clock)(int cpu);
};

/* The domains' tunables, /sys/kernel/debug/sched/'s migration_cost_ns and
 * nr_migrate: how long since it last ran a thread stays cache-hot, in ns of
 * the task clock, and how many threads one balance looks at. */
extern unsigned int sysctl_sched_migration_cost;
extern unsigned int sysctl_sched_nr_migrate;

/** Take the scheduler's operations; until then every entry below does
 * nothing and places a thread where it was. Once, at boot. */
void balance_init(const struct balance_ops *ops);

/** Record processor `cpu`'s SMT siblings `smt` and the processors sharing
 * its last-level cache `llc`, itself included in each, as it starts. */
void balance_cpu_topology(int cpu, uint64_t smt, uint64_t llc);

/**
 * Build every processor's domains over the processors of `online`, from
 * the masks `balance_cpu_topology` recorded (build_sched_domains with
 * x86_topology), dropping a level that adds nothing to the one below
 * (cpu_attach_domain). Called with `sched_lock` held, whenever the set of
 * started processors changes.
 */
void balance_build_domains(uint64_t online);

/** Processor `cpu`'s lowest domain, NULL when it has none. */
struct sched_domain *balance_domain(int cpu);

/**
 * The processor a thread `p` should be queued on, last on `prev`, for
 * `why`: FAIR_WF_TTWU for a wake, with FAIR_WF_SYNC when the waker sleeps at
 * once; FAIR_WF_FORK for a new thread; FAIR_WF_EXEC for an exec
 * (select_task_rq_fair). The caller is the waker, on its own processor.
 */
int fair_select_cpu(struct process *p, int prev, int why);

/**
 * Pull threads to processor `cpu`, whose queue has just emptied, from the
 * busiest processors near it, while its expected idle time pays for the
 * balance (sched_balance_newidle). Returns 1 when a fair thread is now
 * queued there, -1 when a thread of another class is, and 0 otherwise.
 */
int fair_balance_newidle(int cpu);

/**
 * The tick's balance on processor `cpu`, `idle` nonzero when its idle task
 * ran at the tick: balance its domains whose interval has passed and ask an
 * idle processor with a stopped tick to balance for the others when there is
 * work to spread (sched_balance_trigger and sched_balance_softirq).
 */
void fair_balance_tick(int cpu, int idle);

/**
 * Whether processors with a stopped tick should be balanced for, from busy
 * processor `cpu`'s tick, and if so ask one of them by `balance_ops.kick`
 * (nohz_balancer_kick); returns the processor asked, or -1.
 */
int fair_nohz_kick(int cpu);

/**
 * On idle processor `cpu`, kicked by `fair_nohz_kick`, balance and decay the
 * blocked load for every idle processor with a stopped tick whose time has
 * come, and its own domains (nohz_idle_balance and sched_balance_softirq);
 * called also as `cpu` enters its idle wait, where it decays the blocked load
 * that a newly idle balance left for it (nohz_run_idle_balance).
 */
void fair_nohz_balance(int cpu);

/** Processor `cpu` stops its tick as it idles (nohz_balance_enter_idle). */
void fair_balance_idle_enter(int cpu);

/** A thread is woken onto processor `cpu`, idle since its newly idle
 * balance: fold that idle time into its average (ttwu_do_activate's
 * avg_idle). */
void fair_balance_woken(int cpu);

/**
 * On processor `cpu`, kicked for an active balance, after a switch, with
 * only its current thread running there: move one queued thread to the
 * processor that asked (active_load_balance_cpu_stop), which may move a
 * cache-hot thread.
 */
void fair_balance_active(int cpu);

/** The capacity of processor `cpu` left to fair threads, out of
 * SCHED_CAPACITY_SCALE (capacity_of). */
unsigned long balance_capacity(int cpu);

#endif /* _BALANCE_H_ */
