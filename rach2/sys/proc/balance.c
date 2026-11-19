/*
 * balance.c: the fair class's wake placement and load balancing, as Linux
 * 6.12's kernel/sched/fair.c and topology.c do them for an x86 machine
 * without NUMA or asymmetric cores.
 */
#include "balance.h"
#include "pelt.h"
#include "sched.h"
#include "../timer.h"
#include "../lib/kstring.h"

unsigned int sysctl_sched_migration_cost = 500000u;
unsigned int sysctl_sched_nr_migrate = 32u;

/* The threads a balance looks at before it lets other work in, Linux's
 * SCHED_NR_MIGRATE_BREAK without PREEMPT_RT; the longest a balance interval
 * grows to while every thread is pinned, in ms. */
#define SCHED_NR_MIGRATE_BREAK 32u
#define MAX_PINNED_INTERVAL    512u

/* The kicks of the idle balance (nohz_flags): balance the idle processors,
 * decay their blocked load, do that from a newly idle processor, and
 * compute the next balance's time. */
#define NOHZ_BALANCE_KICK 0x1u
#define NOHZ_STATS_KICK   0x2u
#define NOHZ_NEWILB_KICK  0x4u
#define NOHZ_NEXT_KICK    0x8u
#define NOHZ_KICK_MASK    (NOHZ_BALANCE_KICK | NOHZ_STATS_KICK | NOHZ_NEXT_KICK)

/* An answer of the processor scans meaning none found (nr_cpumask_bits). */
#define NO_CPU NR_CPUS

/*
 * What the balance keeps for each processor, the members of Linux's struct
 * rq it reads and writes. `sd` is the lowest domain, NULL before the
 * domains are built or for a processor that is not started; `sd_llc` the
 * highest one sharing the last-level cache, with its size and first
 * processor, and `sd_llc_shared` its shared record. Times in ticks are
 * `kernel_ticks`; `idle_stamp`, `avg_idle` and `max_idle_balance_cost` are in
 * ns of the monotonic clock. `idle_balance` is whether the processor was
 * idle at its last tick. `nohz_flags` are the NOHZ_* kicks it was sent.
 * `capacity` is what real-time threads and interrupts leave of it.
 */
struct balance_rq {
    struct sched_domain *sd;
    struct sched_domain *sd_llc;
    struct sched_domain_shared *sd_llc_shared;
    int sd_llc_size;
    int sd_llc_id;
    uint64_t next_balance;
    int active_balance;
    int push_cpu;
    int idle_balance;
    uint64_t idle_stamp;
    uint64_t avg_idle;
    uint64_t max_idle_balance_cost;
    int nohz_tick_stopped;
    int has_blocked_load;
    uint64_t last_blocked_load_update_tick;
    unsigned int nohz_flags;
    unsigned long capacity;
    uint32_t rnd;
};

/* The idle balance's state over every processor (nohz): the processors
 * with a stopped tick, how many, whether some have blocked load left, and
 * when the next balance and decay for them are due, in ticks. */
static struct {
    uint64_t idle_cpus_mask;
    int nr_cpus;
    int has_blocked;
    int needs_update;
    uint64_t next_balance;
    uint64_t next_blocked;
} nohz;

static const struct balance_ops *bops;
static struct balance_rq brq[NR_CPUS];

void balance_rq_debug(int cpu, uint64_t *next_balance, uint64_t *avg_idle,
                      uint64_t *max_idle_balance_cost)
{
    *next_balance = brq[cpu].next_balance;
    *avg_idle = brq[cpu].avg_idle;
    *max_idle_balance_cost = brq[cpu].max_idle_balance_cost;
}

/* The masks each processor recorded as it started, itself included. */
static uint64_t smt_mask[NR_CPUS];
static uint64_t llc_mask[NR_CPUS];
static uint64_t active_mask;
static int smt_active;
static uint64_t max_load_balance_interval = 1;

/* The domains and their groups and shared records, a set for each level:
 * the group of a level that a processor's child span makes is kept at that
 * span's first processor, as Linux's get_group keeps it in sdd->sg. */
static struct sched_domain sd_store[NR_CPUS][SD_LEVELS];
static struct sched_group sg_store[SD_LEVELS][NR_CPUS];
static struct sched_domain_shared sds_store[SD_LEVELS][NR_CPUS];

static const char *const sd_names[SD_LEVELS] = {"SMT", "MC", "PKG"};

/** The number of processors in `mask`. */
static unsigned int mask_weight(uint64_t mask)
{
    unsigned int n = 0;

    while (mask) {
        mask &= mask - 1;
        n++;
    }
    return n;
}

/** The lowest processor of `mask`, or NO_CPU for an empty one. */
static int mask_first(uint64_t mask)
{
    int c;

    for (c = 0; c < NR_CPUS; c++) {
        if ((mask >> c) & 1u) {
            return c;
        }
    }
    return NO_CPU;
}

/** Whether processor `cpu` is in `mask`. */
static int mask_test(uint64_t mask, int cpu)
{
    return cpu >= 0 && cpu < NR_CPUS && ((mask >> cpu) & 1u) != 0;
}

/** The bit of processor `cpu`. */
static uint64_t cpu_bit(int cpu)
{
    return (uint64_t)1 << cpu;
}

/** Whether tick count `a` is after `b`, across a wrap (time_after). */
static int time_after(uint64_t a, uint64_t b)
{
    return (int64_t)(b - a) < 0;
}

/** Whether tick count `a` is at or after `b` (time_after_eq). */
static int time_after_eq(uint64_t a, uint64_t b)
{
    return (int64_t)(a - b) >= 0;
}

/** `n` divided by `d`, 64 bits wide, with no libgcc helper. */
static uint64_t div_u64(uint64_t n, uint64_t d)
{
    if ((d >> 32) == 0) {
        return tick_div(n, (uint32_t)d, NULL);
    }
    return tick_div64(n, d, NULL);
}

/** The thread whose entity `se` is. */
static process_t *task_of(struct sched_entity *se)
{
    return (process_t *)(void *)((char *)se - offsetof(process_t, se));
}

/** Whether `p` may run on processor `cpu`: in its affinity and started. */
static int cpu_allowed(const process_t *p, int cpu)
{
    return mask_test(p->affinity & active_mask, cpu);
}

/** The processors `p` may run on now. */
static uint64_t cpus_ptr(const process_t *p)
{
    return p->affinity & active_mask;
}

/** The fair root queue of processor `cpu`. */
static struct cfs_rq *cpu_cfs(int cpu)
{
    return &fair_rqs[cpu].cfs;
}

/** idle_cpu: `cpu` runs its idle task, nothing queued, no wake coming. */
static int idle_cpu(int cpu)
{
    return bops->idle(cpu);
}

/** Whether `cpu` runs only SCHED_IDLE threads, at least one
 * (sched_idle_cpu). */
static int sched_idle_cpu(int cpu)
{
    unsigned int nr = bops->nr_running(cpu);

    return nr && nr == cpu_cfs(cpu)->idle_h_nr_running;
}

/** Whether every SMT sibling of `cpu` is idle (is_core_idle). */
static int is_core_idle(int cpu)
{
    uint64_t m = smt_mask[cpu] & ~cpu_bit(cpu);
    int c;

    for (c = 0; m; c++, m >>= 1) {
        if ((m & 1u) && !idle_cpu(c)) {
            return 0;
        }
    }
    return 1;
}

/** Whether processors `a` and `b` share a last-level cache
 * (cpus_share_cache). */
static int cpus_share_cache(int a, int b)
{
    return a == b || brq[a].sd_llc_id == brq[b].sd_llc_id;
}

unsigned long balance_capacity(int cpu)
{
    return brq[cpu].capacity;
}

/** The load of `cpu`'s fair threads, running and blocked (cpu_load). */
static unsigned long cpu_load(int cpu)
{
    return cpu_cfs(cpu)->avg.load_avg;
}

/** `p`'s load as its processor's root queue sees it (task_h_load). */
static unsigned long task_h_load(process_t *p)
{
    return pelt_task_h_load(&p->se, kernel_ticks);
}

/** The load of `cpu` without `p`'s share when `p` is counted there
 * (cpu_load_without). */
static unsigned long cpu_load_without(int cpu, process_t *p)
{
    unsigned long load, h;

    if (cpu != p->cpu || !p->se.avg.last_update_time) {
        return cpu_load(cpu);
    }
    load = cpu_cfs(cpu)->avg.load_avg;
    h = task_h_load(p);
    return load > h ? load - h : 0;
}

/** The runnable average of `cpu`'s fair threads (cpu_runnable). */
static unsigned long cpu_runnable(int cpu)
{
    return cpu_cfs(cpu)->avg.runnable_avg;
}

/** The same without `p`'s (cpu_runnable_without). */
static unsigned long cpu_runnable_without(int cpu, process_t *p)
{
    unsigned long runnable;

    if (cpu != p->cpu || !p->se.avg.last_update_time) {
        return cpu_runnable(cpu);
    }
    runnable = cpu_cfs(cpu)->avg.runnable_avg;
    return runnable > p->se.avg.runnable_avg ? runnable - p->se.avg.runnable_avg : 0;
}

/**
 * The utilization of `cpu`'s fair threads, with `p` added when it would move
 * to `cpu` (`dst_cpu` is `cpu`) or taken away when it would leave, the
 * runnable average as its floor when `boost` is set, the estimates as
 * another, and the processor's full capacity as its ceiling (cpu_util).
 */
static unsigned long cpu_util(int cpu, process_t *p, int dst_cpu, int boost)
{
    struct cfs_rq *cfs_rq = cpu_cfs(cpu);
    int add_task = p && p->cpu != cpu && dst_cpu == cpu;
    int sub_task = p && p->cpu == cpu && dst_cpu != cpu;
    unsigned long util = cfs_rq->avg.util_avg, runnable, est;

    if (add_task) {
        util += p->se.avg.util_avg;
    } else if (sub_task) {
        util = util > p->se.avg.util_avg ? util - p->se.avg.util_avg : 0;
    }
    if (boost) {
        runnable = cfs_rq->avg.runnable_avg;
        if (add_task) {
            runnable += p->se.avg.runnable_avg;
        } else if (sub_task) {
            runnable = runnable > p->se.avg.runnable_avg ? runnable - p->se.avg.runnable_avg : 0;
        }
        if (runnable > util) {
            util = runnable;
        }
    }
    if (sched_feat(UTIL_EST)) {
        unsigned long pest = p ? (p->se.avg.util_est & ~UTIL_AVG_UNCHANGED) : 0;

        est = cfs_rq->avg.util_est;
        if (dst_cpu == cpu) {
            est += pest;
        } else if (p && (p->se.on_rq || bops->curr(p->cpu) == p)) {
            est = est > pest ? est - pest : 0;
        }
        if (est > util) {
            util = est;
        }
    }
    return util < (unsigned long)SCHED_CAPACITY_SCALE ? util : (unsigned long)SCHED_CAPACITY_SCALE;
}

/** The utilization of `cpu`'s fair threads (cpu_util_cfs). */
static unsigned long cpu_util_cfs(int cpu)
{
    return cpu_util(cpu, NULL, -1, 0);
}

/** The same, raised to the runnable average (cpu_util_cfs_boost). */
static unsigned long cpu_util_cfs_boost(int cpu)
{
    return cpu_util(cpu, NULL, -1, 1);
}

/** The utilization of `cpu` without `p`'s (cpu_util_without). */
static unsigned long cpu_util_without(int cpu, process_t *p)
{
    if (cpu != p->cpu || !p->se.avg.last_update_time) {
        p = NULL;
    }
    return cpu_util(cpu, p, -1, 0);
}

/**
 * What real-time threads and interrupts leave of processor `cpu`'s capacity,
 * at least 1 (scale_rt_capacity): the free capacity scaled by the part of
 * the time interrupts leave.
 */
static unsigned long scale_rt_capacity(int cpu)
{
    unsigned long max = SCHED_CAPACITY_SCALE, irq, used, free;

    irq = pelt_cpu_util_irq(cpu);
    if (irq >= max) {
        return 1;
    }
    used = pelt_cpu_util_rt(cpu);
    if (used >= max) {
        return 1;
    }
    free = max - used;
    return free * (max - irq) / max;
}

void balance_init(const struct balance_ops *ops)
{
    int c;

    for (c = 0; c < NR_CPUS; c++) {
        brq[c].capacity = SCHED_CAPACITY_SCALE;
        brq[c].avg_idle = 2u * (uint64_t)sysctl_sched_migration_cost;
        brq[c].max_idle_balance_cost = sysctl_sched_migration_cost;
        brq[c].sd_llc_size = 1;
        brq[c].sd_llc_id = c;
        brq[c].rnd = 0x9e3779b9u * (uint32_t)(c + 1);
        if (!smt_mask[c]) {
            smt_mask[c] = cpu_bit(c);
        }
        if (!llc_mask[c]) {
            llc_mask[c] = cpu_bit(c);
        }
    }
    bops = ops;
}

void balance_cpu_topology(int cpu, uint64_t smt, uint64_t llc)
{
    int c;

    smt |= cpu_bit(cpu);
    llc |= smt;
    smt_mask[cpu] = smt;
    llc_mask[cpu] = llc;
    /* A sibling recorded before this one learns of it too. */
    for (c = 0; c < NR_CPUS; c++) {
        if (c != cpu && mask_test(smt, c)) {
            smt_mask[c] |= cpu_bit(cpu) | smt;
        }
        if (c != cpu && mask_test(llc, c)) {
            llc_mask[c] |= cpu_bit(cpu) | llc;
        }
    }
    if (mask_weight(smt) > 1) {
        smt_active = 1;
    }
}

/** The span of level `level` around processor `cpu` (the topology table's
 * masks), over the processors of `online`. */
static uint64_t level_mask(int level, int cpu, uint64_t online)
{
    switch (level) {
    case SD_LEVEL_SMT:
        return smt_mask[cpu] & online;
    case SD_LEVEL_MC:
        return llc_mask[cpu] & online;
    default:
        return online;
    }
}

/** The flags a level adds to the defaults (cpu_smt_flags, cpu_core_flags). */
static int level_flags(int level)
{
    switch (level) {
    case SD_LEVEL_SMT:
        return SD_SHARE_CPUCAPACITY | SD_SHARE_LLC;
    case SD_LEVEL_MC:
        return SD_SHARE_LLC;
    default:
        return 0;
    }
}

/**
 * Give `sd` of processor `cpu` at `level` Linux's defaults and the level's
 * behaviour (sd_init): intervals of one and two ms per processor spanned,
 * busy factor 16, an imbalance of 117%, 110% between SMT siblings, and one
 * failed balance a cache-hot thread survives above SMT.
 */
static void sd_init(struct sched_domain *sd, int level, uint64_t span, int cpu,
                    struct sched_domain *child)
{
    unsigned int weight = mask_weight(span);
    int first = mask_first(span);

    sd->parent = NULL;
    sd->child = child;
    sd->groups = NULL;
    sd->shared = NULL;
    sd->span = span;
    sd->span_weight = weight;
    sd->min_interval = weight;
    sd->max_interval = 2u * weight;
    sd->busy_factor = 16;
    sd->imbalance_pct = 117;
    sd->cache_nice_tries = 0;
    sd->flags = SD_BALANCE_NEWIDLE | SD_BALANCE_EXEC | SD_BALANCE_FORK | SD_WAKE_AFFINE |
                SD_PREFER_SIBLING | level_flags(level);
    sd->level = level;
    sd->nohz_idle = 0;
    sd->last_balance = kernel_ticks;
    sd->balance_interval = weight;
    sd->nr_balance_failed = 0;
    sd->newidle_call = 512;
    sd->newidle_success = 256;
    sd->newidle_ratio = 512;
    sd->max_newidle_lb_cost = 0;
    sd->last_decay_max_lb_cost = kernel_ticks;
    sd->name = sd_names[level];
    if (sd->flags & SD_SHARE_CPUCAPACITY) {
        sd->imbalance_pct = 110;
    } else {
        sd->imbalance_pct = 117;
        sd->cache_nice_tries = 1;
    }
    if (sd->flags & SD_SHARE_LLC) {
        sd->shared = &sds_store[level][first];
        sd->shared->nr_busy_cpus = (int)weight;
    }
    (void)cpu;
}

/**
 * Link the groups of `sd`, processor `cpu`'s domain at `level`: one per
 * child span covering the domain, or one per processor at the bottom,
 * starting with the processor's own (build_sched_groups, get_group,
 * init_sched_groups_capacity).
 */
static void build_groups(struct sched_domain *sd, int level, int cpu)
{
    struct sched_group *first = NULL, *last = NULL, *sg;
    uint64_t covered = 0, span;
    int i, c, g;

    for (i = 0; i < NR_CPUS; i++) {
        c = (cpu + i) % NR_CPUS;
        if (!mask_test(sd->span, c) || mask_test(covered, c)) {
            continue;
        }
        span = sd->child ? sd_store[c][level - 1].span : cpu_bit(c);
        g = mask_first(span);
        sg = &sg_store[level][g];
        sg->span = span;
        sg->group_weight = mask_weight(span);
        sg->flags = sd->child ? sd_store[c][level - 1].flags : 0;
        sg->capacity = SCHED_CAPACITY_SCALE * sg->group_weight;
        sg->min_capacity = SCHED_CAPACITY_SCALE;
        sg->max_capacity = SCHED_CAPACITY_SCALE;
        sg->next_update = kernel_ticks;
        sg->imbalance = 0;
        sg->cores = 0;
        while (span) {
            int s = mask_first(span);

            sg->cores++;
            span &= ~smt_mask[s];
        }
        covered |= sg->span;
        if (last) {
            last->next = sg;
        } else {
            first = sg;
        }
        last = sg;
    }
    if (last) {
        last->next = first;
    }
    sd->groups = first;
}

/** Whether `sd` does nothing for balancing (sd_degenerate). */
static int sd_degenerate(const struct sched_domain *sd)
{
    if (sd->span_weight == 1) {
        return 1;
    }
    if ((sd->flags & SD_DEGENERATE_GROUPS_MASK) && sd->groups != sd->groups->next) {
        return 0;
    }
    if (sd->flags & SD_WAKE_AFFINE) {
        return 0;
    }
    return 1;
}

/** Whether `parent` adds nothing to `sd` below it (sd_parent_degenerate). */
static int sd_parent_degenerate(const struct sched_domain *sd, const struct sched_domain *parent)
{
    int cflags = sd->flags, pflags = parent->flags;

    if (sd_degenerate(parent)) {
        return 1;
    }
    if (sd->span != parent->span) {
        return 0;
    }
    if (parent->groups == parent->groups->next) {
        pflags &= ~SD_DEGENERATE_GROUPS_MASK;
    }
    if (~cflags & pflags) {
        return 0;
    }
    return 1;
}

/** Drop the domains of `cpu` that add nothing, from `sd` its lowest, and
 * return the lowest left (cpu_attach_domain). */
static struct sched_domain *attach_domain(struct sched_domain *sd)
{
    struct sched_domain *tmp, *parent;

    for (tmp = sd; tmp;) {
        parent = tmp->parent;
        if (!parent) {
            break;
        }
        if (sd_parent_degenerate(tmp, parent)) {
            tmp->parent = parent->parent;
            if (parent->parent) {
                parent->parent->child = tmp;
                parent->parent->groups->flags = tmp->flags;
            }
            /* The spans match, so the property passes down. */
            if (parent->flags & SD_PREFER_SIBLING) {
                tmp->flags |= SD_PREFER_SIBLING;
            }
        } else {
            tmp = tmp->parent;
        }
    }
    if (sd && sd_degenerate(sd)) {
        sd = sd->parent;
        if (sd) {
            /* Linux's loop here clears the first group's flags only. */
            sd->groups->flags = 0;
            sd->child = NULL;
        }
    }
    return sd;
}

/** Note the highest domain of `cpu` sharing its cache
 * (update_top_cache_domain). */
static void update_top_cache_domain(int cpu)
{
    struct sched_domain *sd, *llc = NULL;

    for (sd = brq[cpu].sd; sd; sd = sd->parent) {
        if (sd->flags & SD_SHARE_LLC) {
            llc = sd;
        }
    }
    brq[cpu].sd_llc = llc;
    brq[cpu].sd_llc_shared = llc ? llc->shared : NULL;
    brq[cpu].sd_llc_size = llc ? (int)llc->span_weight : 1;
    brq[cpu].sd_llc_id = llc ? mask_first(llc->span) : cpu;
}

void balance_build_domains(uint64_t online)
{
    static int top[NR_CPUS];
    struct sched_domain *sd, *child;
    uint64_t span;
    int c, level;

    active_mask = online;
    /* Every processor's domains first, since a group is a child's span. */
    for (c = 0; c < NR_CPUS; c++) {
        brq[c].sd = NULL;
        top[c] = -1;
        if (!mask_test(online, c)) {
            continue;
        }
        child = NULL;
        for (level = 0; level < SD_LEVELS; level++) {
            span = level_mask(level, c, online);
            if (child && (child->span & ~span)) {
                span |= child->span;
            }
            sd = &sd_store[c][level];
            sd_init(sd, level, span, c, child);
            if (child) {
                child->parent = sd;
            }
            child = sd;
            top[c] = level;
            if (span == online) {
                break;
            }
        }
    }
    for (c = 0; c < NR_CPUS; c++) {
        for (level = 0; level <= top[c]; level++) {
            build_groups(&sd_store[c][level], level, c);
        }
    }
    for (c = 0; c < NR_CPUS; c++) {
        if (mask_test(online, c)) {
            brq[c].sd = attach_domain(&sd_store[c][0]);
        } else {
            brq[c].nohz_tick_stopped = 0;
        }
        update_top_cache_domain(c);
    }
    nohz.idle_cpus_mask &= online;
    nohz.nr_cpus = (int)mask_weight(nohz.idle_cpus_mask);
    max_load_balance_interval = (uint64_t)TICK_HZ * mask_weight(online) / 10u;
    if (!max_load_balance_interval) {
        max_load_balance_interval = 1;
    }
}

struct sched_domain *balance_domain(int cpu)
{
    return brq[cpu].sd;
}

/*
 * Wake placement.
 */

/** A pseudo-random number for processor `cpu` (sched_rng), xorshift32. */
static uint32_t sched_rng(int cpu)
{
    uint32_t x = brq[cpu].rnd;

    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    brq[cpu].rnd = x;
    return x;
}

/** Note that the waker `cur` woke `p`, counting a change of wakee, halved
 * once a second (record_wakee). */
static void record_wakee(process_t *cur, process_t *p)
{
    if (!cur) {
        return;
    }
    if (time_after(kernel_ticks, cur->se.wakee_flip_decay_ts + TICK_HZ)) {
        cur->se.wakee_flips >>= 1;
        cur->se.wakee_flip_decay_ts = kernel_ticks;
    }
    if (cur->se.last_wakee != p) {
        cur->se.last_wakee = p;
        cur->se.wakee_flips++;
    }
}

/**
 * Whether the waker `cur` and `p` wake many partners each, more than share
 * a cache, so that the wake should spread rather than come to the waker
 * (wake_wide).
 */
static int wake_wide(process_t *cur, process_t *p, int this_cpu)
{
    unsigned int master = cur ? cur->se.wakee_flips : 0, slave = p->se.wakee_flips;
    unsigned int factor = (unsigned int)brq[this_cpu].sd_llc_size, t;

    if (master < slave) {
        t = master;
        master = slave;
        slave = t;
    }
    if (slave < factor || master < slave * factor) {
        return 0;
    }
    return 1;
}

/** The waker's processor when it is idle and shares the cache, or will be
 * idle after a sync wake, else the last one when it is idle; NO_CPU when
 * neither answers (wake_affine_idle). */
static int wake_affine_idle(int this_cpu, int prev_cpu, int sync)
{
    if (idle_cpu(this_cpu) && cpus_share_cache(this_cpu, prev_cpu)) {
        return idle_cpu(prev_cpu) ? prev_cpu : this_cpu;
    }
    if (sync && bops->nr_running(this_cpu) - cpu_cfs(this_cpu)->h_nr_delayed == 1) {
        return this_cpu;
    }
    if (idle_cpu(prev_cpu)) {
        return prev_cpu;
    }
    return NO_CPU;
}

/**
 * The waker's processor when its load with `p` added is below the last
 * one's without it, the last one's raised by half the domain's imbalance
 * (WA_BIAS), and, for a sync wake, without the waker's own load; NO_CPU
 * otherwise (wake_affine_weight). Capacities are equal, so they cancel.
 */
static int wake_affine_weight(struct sched_domain *sd, process_t *cur, process_t *p, int this_cpu,
                              int prev_cpu, int sync)
{
    int64_t this_eff_load, prev_eff_load;
    unsigned long task_load, current_load;

    this_eff_load = (int64_t)cpu_load(this_cpu);
    if (sync && cur) {
        current_load = task_h_load(cur);
        if ((int64_t)current_load > this_eff_load) {
            return this_cpu;
        }
        this_eff_load -= (int64_t)current_load;
    }
    task_load = task_h_load(p);
    this_eff_load += (int64_t)task_load;
    if (sched_feat(WA_BIAS)) {
        this_eff_load *= 100;
    }
    this_eff_load *= (int64_t)balance_capacity(prev_cpu);
    prev_eff_load = (int64_t)cpu_load(prev_cpu);
    prev_eff_load -= (int64_t)task_load;
    if (sched_feat(WA_BIAS)) {
        prev_eff_load *= 100 + (sd->imbalance_pct - 100) / 2;
    }
    prev_eff_load *= (int64_t)balance_capacity(this_cpu);
    /* A sync wake that ties may stack the wakee on the waker. */
    if (sync) {
        prev_eff_load += 1;
    }
    return this_eff_load < prev_eff_load ? this_cpu : NO_CPU;
}

/** The waker's processor or the last one, whichever a wake of `p` should
 * aim for (wake_affine). */
static int wake_affine(struct sched_domain *sd, process_t *cur, process_t *p, int this_cpu,
                       int prev_cpu, int sync)
{
    int target = NO_CPU;

    if (sched_feat(WA_IDLE)) {
        target = wake_affine_idle(this_cpu, prev_cpu, sync);
    }
    if (sched_feat(WA_WEIGHT) && target == NO_CPU) {
        target = wake_affine_weight(sd, cur, p, this_cpu, prev_cpu, sync);
    }
    if (fair_schedstats) {
        p->se.stats.nr_wakeups_affine_attempts++;
    }
    if (target != this_cpu) {
        return prev_cpu;
    }
    if (fair_schedstats) {
        p->se.stats.nr_wakeups_affine++;
    }
    return target;
}

/** Note that `cpu`'s core is all idle in its cache domain's record, as
 * `cpu` goes idle (__update_idle_core). */
static void update_idle_core(int cpu)
{
    struct sched_domain_shared *sds = brq[cpu].sd_llc_shared;

    if (!smt_active || !sds || sds->has_idle_cores) {
        return;
    }
    if (is_core_idle(cpu)) {
        sds->has_idle_cores = 1;
    }
}

/** `cpu` when it can take `p` at once, idle or running only SCHED_IDLE
 * threads; else -1 (__select_idle_cpu). */
static int select_idle_cpu1(int cpu)
{
    if (idle_cpu(cpu) || sched_idle_cpu(cpu)) {
        return cpu;
    }
    return -1;
}

/**
 * `core` when all its SMT siblings are idle; else -1, taking the core out of
 * `cpus` and keeping in `*idle_cpu` the first sibling of `cpus` that can
 * take a thread (select_idle_core).
 */
static int select_idle_core(int core, uint64_t *cpus, int *idle_cpu_found)
{
    int idle = 1, c;

    for (c = 0; c < NR_CPUS; c++) {
        if (!mask_test(smt_mask[core], c)) {
            continue;
        }
        if (!idle_cpu(c)) {
            idle = 0;
            if (*idle_cpu_found == -1) {
                if (sched_idle_cpu(c) && mask_test(*cpus, c)) {
                    *idle_cpu_found = c;
                    break;
                }
                continue;
            }
            break;
        }
        if (*idle_cpu_found == -1 && mask_test(*cpus, c)) {
            *idle_cpu_found = c;
        }
    }
    if (idle) {
        return core;
    }
    *cpus &= ~smt_mask[core];
    return -1;
}

/** A sibling of `target` in `sd`'s span that `p` may use and that can take
 * it at once, or -1 (select_idle_smt). */
static int select_idle_smt(process_t *p, struct sched_domain *sd, int target)
{
    int c;

    for (c = 0; c < NR_CPUS; c++) {
        if (c == target || !mask_test(smt_mask[target] & cpus_ptr(p), c)) {
            continue;
        }
        if (!mask_test(sd->span, c)) {
            continue;
        }
        if (idle_cpu(c) || sched_idle_cpu(c)) {
            return c;
        }
    }
    return -1;
}

/**
 * Scan the cache domain `sd` from the processor after `target` for one that
 * can take `p`: with an idle core in it, the first all-idle core, else the
 * first idle processor met; without, the first idle processor within the
 * scan's length that SIS_UTIL sets (select_idle_cpu). -1 when none.
 */
static int select_idle_cpu(process_t *p, struct sched_domain *sd, int has_idle_core, int target)
{
    uint64_t cpus = sd->span & cpus_ptr(p);
    int i, c, idle = -1, nr = 0x7fffffff, r;
    struct sched_domain_shared *share;

    if (sched_feat(SIS_UTIL)) {
        share = brq[target].sd_llc_shared;
        if (share) {
            nr = share->nr_idle_scan + 1;
            if (nr == 1) {
                return -1;
            }
        }
    }
    for (i = 1; i <= NR_CPUS; i++) {
        c = (target + i) % NR_CPUS;
        if (!mask_test(cpus, c)) {
            continue;
        }
        if (has_idle_core) {
            r = select_idle_core(c, &cpus, &idle);
            if (r >= 0) {
                return r;
            }
        } else {
            if (--nr <= 0) {
                return -1;
            }
            idle = select_idle_cpu1(c);
            if (idle >= 0) {
                break;
            }
        }
    }
    if (has_idle_core && brq[target].sd_llc_shared) {
        brq[target].sd_llc_shared->has_idle_cores = 0;
    }
    return idle;
}

/**
 * A processor that takes a wake of `p` at once without a scan: the target,
 * the last one sharing its cache, a per-processor kernel thread's own, or
 * the one before the last; -1 when none (the start of select_idle_sibling).
 */
static int select_idle_quick(process_t *cur, process_t *p, int prev, int target, int this_cpu)
{
    int recent_used_cpu;

    if (idle_cpu(target) || sched_idle_cpu(target)) {
        return target;
    }
    if (prev != target && cpus_share_cache(prev, target) &&
        (idle_cpu(prev) || sched_idle_cpu(prev))) {
        return prev;
    }
    /* A per-processor kernel thread waking a thread that last ran on its
     * processor, which has nothing else queued, as when I/O completes:
     * the wakee likely runs next there. Linux also asks that the waker
     * not be an interrupt (in_task), which the caller cannot tell here. */
    if (cur && proc_is_kthread(cur) && mask_weight(cur->affinity) == 1 && prev == this_cpu &&
        bops->nr_running(this_cpu) <= 1) {
        return prev;
    }
    recent_used_cpu = p->se.recent_used_cpu;
    p->se.recent_used_cpu = prev;
    if (recent_used_cpu != prev && recent_used_cpu != target && recent_used_cpu >= 0 &&
        recent_used_cpu < NR_CPUS && cpus_share_cache(recent_used_cpu, target) &&
        (idle_cpu(recent_used_cpu) || sched_idle_cpu(recent_used_cpu)) &&
        cpu_allowed(p, recent_used_cpu)) {
        return recent_used_cpu;
    }
    return -1;
}

/**
 * The processor near `target`, the processor `wake_affine` chose for `p`,
 * last on `prev`, that can take it at once: one `select_idle_quick` finds,
 * an idle SMT sibling of the last one, or one of the cache domain; else the
 * target (select_idle_sibling).
 */
static int select_idle_sibling(process_t *cur, process_t *p, int prev, int target, int this_cpu)
{
    struct sched_domain *sd;
    int has_idle_core = 0, i;

    i = select_idle_quick(cur, p, prev, target, this_cpu);
    if (i >= 0) {
        return i;
    }
    sd = brq[target].sd_llc;
    if (!sd) {
        return target;
    }
    if (smt_active) {
        has_idle_core = brq[target].sd_llc_shared && brq[target].sd_llc_shared->has_idle_cores;
        if (!has_idle_core && cpus_share_cache(prev, target)) {
            i = select_idle_smt(p, sd, prev);
            if (i >= 0) {
                return i;
            }
        }
    }
    i = select_idle_cpu(p, sd, has_idle_core, target);
    if (i >= 0) {
        return i;
    }
    return target;
}

/*
 * Group statistics, shared by placement by load and by the balance.
 */

/* A group's state, from the least to the most in need of balancing
 * (enum group_type); misfit, asymmetric packing and SMT balance, which need
 * asymmetric cores or ITMT, never come up here but for SMT balance. */
enum group_type {
    group_has_spare = 0,
    group_fully_busy,
    group_misfit_task,
    group_smt_balance,
    group_asym_packing,
    group_imbalanced,
    group_overloaded
};

/* What a balance moves to even the domain out (enum migration_type). */
enum migration_type { migrate_load = 0, migrate_util, migrate_task, migrate_misfit };

/* A group's statistics (struct sg_lb_stats). */
struct sg_lb_stats {
    unsigned long avg_load;        /* load per capacity, for a busy group */
    unsigned long group_load;      /* the members' load */
    unsigned long group_capacity;  /* the members' capacity */
    unsigned long group_util;      /* the members' utilization */
    unsigned long group_runnable;  /* the members' runnable average */
    unsigned int sum_nr_running;   /* threads of every class */
    unsigned int sum_h_nr_running; /* fair threads */
    unsigned int idle_cpus;
    unsigned int group_weight;
    enum group_type group_type;
    unsigned int group_smt_balance;
    unsigned long group_misfit_task_load;
};

/** Whether a group has room for more: fewer threads than processors, or a
 * utilization and runnable average within its capacity by the domain's
 * imbalance (group_has_capacity). */
static int group_has_capacity(unsigned int imbalance_pct, const struct sg_lb_stats *sgs)
{
    if (sgs->sum_nr_running < sgs->group_weight) {
        return 1;
    }
    if (sgs->group_capacity * imbalance_pct < sgs->group_runnable * 100) {
        return 0;
    }
    if (sgs->group_capacity * 100 > sgs->group_util * imbalance_pct) {
        return 1;
    }
    return 0;
}

/** Whether a group has more threads than processors and its utilization or
 * runnable average beyond its capacity (group_is_overloaded). */
static int group_is_overloaded(unsigned int imbalance_pct, const struct sg_lb_stats *sgs)
{
    if (sgs->sum_nr_running <= sgs->group_weight) {
        return 0;
    }
    if (sgs->group_capacity * 100 < sgs->group_util * imbalance_pct) {
        return 1;
    }
    if (sgs->group_capacity * imbalance_pct < sgs->group_runnable * 100) {
        return 1;
    }
    return 0;
}

/** A group's state (group_classify). */
static enum group_type group_classify(unsigned int imbalance_pct, const struct sched_group *group,
                                      const struct sg_lb_stats *sgs)
{
    if (group_is_overloaded(imbalance_pct, sgs)) {
        return group_overloaded;
    }
    if (group->imbalance) {
        return group_imbalanced;
    }
    if (sgs->group_smt_balance) {
        return group_smt_balance;
    }
    if (sgs->group_misfit_task_load) {
        return group_misfit_task;
    }
    if (!group_has_capacity(imbalance_pct, sgs)) {
        return group_fully_busy;
    }
    return group_has_spare;
}

/** Whether `p` counts on `cpu`'s queue: its processor, attached and queued
 * (task_running_on_cpu). */
static unsigned int task_running_on_cpu(int cpu, process_t *p)
{
    if (cpu != p->cpu || !p->se.avg.last_update_time) {
        return 0;
    }
    return p->se.on_rq != 0;
}

/** Whether `cpu` would be idle without `p` (idle_cpu_without). */
static int idle_cpu_without(int cpu, process_t *p)
{
    process_t *cur = bops->curr(cpu);

    if (cur && cur != p) {
        return 0;
    }
    return 1;
}

/** The statistics of `group` for placing `p`, its own share left out
 * (update_sg_wakeup_stats). */
static void update_sg_wakeup_stats(struct sched_domain *sd, struct sched_group *group,
                                   struct sg_lb_stats *sgs, process_t *p)
{
    unsigned int local, nr_running;
    int i;

    k_memset(sgs, 0, sizeof(*sgs));
    for (i = 0; i < NR_CPUS; i++) {
        if (!mask_test(group->span & cpus_ptr(p), i)) {
            continue;
        }
        sgs->group_load += cpu_load_without(i, p);
        sgs->group_util += cpu_util_without(i, p);
        sgs->group_runnable += cpu_runnable_without(i, p);
        local = task_running_on_cpu(i, p);
        sgs->sum_h_nr_running += cpu_cfs(i)->h_nr_queued - local;
        nr_running = bops->nr_running(i) - local;
        sgs->sum_nr_running += nr_running;
        if (!nr_running && idle_cpu_without(i, p)) {
            sgs->idle_cpus++;
        }
    }
    sgs->group_capacity = group->capacity;
    sgs->group_weight = group->group_weight;
    sgs->group_type = group_classify(sd->imbalance_pct, group, sgs);
    if (sgs->group_type == group_fully_busy || sgs->group_type == group_overloaded) {
        sgs->avg_load = sgs->group_load * SCHED_CAPACITY_SCALE / sgs->group_capacity;
    }
}

/** Whether `group` is less busy than `idlest` (update_pick_idlest). */
static int update_pick_idlest(const struct sched_group *idlest,
                              const struct sg_lb_stats *idlest_sgs, const struct sched_group *group,
                              const struct sg_lb_stats *sgs)
{
    if (sgs->group_type < idlest_sgs->group_type) {
        return 1;
    }
    if (sgs->group_type > idlest_sgs->group_type) {
        return 0;
    }
    switch (sgs->group_type) {
    case group_overloaded:
    case group_fully_busy:
        if (idlest_sgs->avg_load <= sgs->avg_load) {
            return 0;
        }
        break;
    case group_imbalanced:
    case group_asym_packing:
    case group_smt_balance:
        return 0;
    case group_misfit_task:
        if (idlest->max_capacity >= group->max_capacity) {
            return 0;
        }
        break;
    case group_has_spare:
        if (idlest_sgs->idle_cpus > sgs->idle_cpus) {
            return 0;
        }
        if (idlest_sgs->idle_cpus == sgs->idle_cpus && idlest_sgs->group_util <= sgs->group_util) {
            return 0;
        }
        break;
    default:
        break;
    }
    return 1;
}

/**
 * The least busy group of `sd` for a new or exec'ing `p`, or NULL when the
 * group of `this_cpu` is as good, within the domain's imbalance
 * (sched_balance_find_dst_group).
 */
static struct sched_group *find_dst_group(struct sched_domain *sd, process_t *p, int this_cpu)
{
    struct sched_group *idlest = NULL, *local = NULL, *group = sd->groups;
    struct sg_lb_stats local_sgs, tmp_sgs, idlest_sgs, *sgs;
    unsigned long imbalance;
    int local_group;

    k_memset(&local_sgs, 0, sizeof(local_sgs));
    k_memset(&idlest_sgs, 0, sizeof(idlest_sgs));
    idlest_sgs.avg_load = 0xffffffffu;
    idlest_sgs.group_type = group_overloaded;
    do {
        if (!(group->span & cpus_ptr(p))) {
            continue;
        }
        local_group = mask_test(group->span, this_cpu);
        if (local_group) {
            sgs = &local_sgs;
            local = group;
        } else {
            sgs = &tmp_sgs;
        }
        update_sg_wakeup_stats(sd, group, sgs, p);
        if (!local_group && update_pick_idlest(idlest, &idlest_sgs, group, sgs)) {
            idlest = group;
            idlest_sgs = *sgs;
        }
    } while ((group = group->next) != sd->groups);

    if (!idlest) {
        return NULL;
    }
    if (!local) {
        return idlest;
    }
    if (local_sgs.group_type < idlest_sgs.group_type) {
        return NULL;
    }
    if (local_sgs.group_type > idlest_sgs.group_type) {
        return idlest;
    }
    switch (local_sgs.group_type) {
    case group_overloaded:
    case group_fully_busy:
        imbalance = NICE_0_LOAD * (sd->imbalance_pct - 100) / 100;
        if (idlest_sgs.avg_load >= local_sgs.avg_load + imbalance) {
            return NULL;
        }
        if (100 * local_sgs.avg_load <= sd->imbalance_pct * idlest_sgs.avg_load) {
            return NULL;
        }
        break;
    case group_imbalanced:
    case group_asym_packing:
    case group_smt_balance:
        return NULL;
    case group_misfit_task:
        if (local->max_capacity >= idlest->max_capacity) {
            return NULL;
        }
        break;
    case group_has_spare:
        if (local_sgs.idle_cpus >= idlest_sgs.idle_cpus) {
            return NULL;
        }
        break;
    default:
        break;
    }
    return idlest;
}

/**
 * The processor of `group` for `p`: one running only SCHED_IDLE threads, else
 * the idle one that went idle last, its cache the warmest, else the least
 * loaded (sched_balance_find_dst_group_cpu, with no idle states to compare).
 */
static int find_dst_group_cpu(struct sched_group *group, process_t *p, int this_cpu)
{
    unsigned long load, min_load = (unsigned long)-1;
    uint64_t latest_idle_timestamp = 0;
    int least_loaded_cpu = this_cpu, shallowest_idle_cpu = -1, i;

    if (group->group_weight == 1) {
        return mask_first(group->span);
    }
    for (i = 0; i < NR_CPUS; i++) {
        if (!mask_test(group->span & cpus_ptr(p), i)) {
            continue;
        }
        if (sched_idle_cpu(i)) {
            return i;
        }
        if (idle_cpu(i)) {
            if (brq[i].idle_stamp > latest_idle_timestamp) {
                latest_idle_timestamp = brq[i].idle_stamp;
                shallowest_idle_cpu = i;
            }
        } else if (shallowest_idle_cpu == -1) {
            load = cpu_load(i);
            if (load < min_load) {
                min_load = load;
                least_loaded_cpu = i;
            }
        }
    }
    return shallowest_idle_cpu != -1 ? shallowest_idle_cpu : least_loaded_cpu;
}

/**
 * The processor for a new or exec'ing `p` by load, down from domain `sd` of
 * `cpu` through the levels with `sd_flag` (sched_balance_find_dst_cpu).
 */
static int find_dst_cpu(struct sched_domain *sd, process_t *p, int cpu, int prev_cpu, int sd_flag)
{
    struct sched_group *group;
    struct sched_domain *tmp;
    unsigned int weight;
    int new_cpu = cpu;

    if (!(sd->span & cpus_ptr(p))) {
        return prev_cpu;
    }
    if (!(sd_flag & SD_BALANCE_FORK)) {
        pelt_sync_entity(&p->se);
    }
    while (sd) {
        if (!(sd->flags & sd_flag)) {
            sd = sd->child;
            continue;
        }
        group = find_dst_group(sd, p, cpu);
        if (!group) {
            sd = sd->child;
            continue;
        }
        new_cpu = find_dst_group_cpu(group, p, cpu);
        if (new_cpu == cpu) {
            sd = sd->child;
            continue;
        }
        /* Go on below at the new processor's levels under this one. */
        cpu = new_cpu;
        weight = sd->span_weight;
        sd = NULL;
        for (tmp = brq[cpu].sd; tmp; tmp = tmp->parent) {
            if (weight <= tmp->span_weight) {
                break;
            }
            if (tmp->flags & sd_flag) {
                sd = tmp;
            }
        }
    }
    return new_cpu;
}

/** A processor `p` may run on, `prev` first, for a choice it may not take
 * (select_fallback_rq). */
static int fallback_cpu(process_t *p, int prev)
{
    if (cpu_allowed(p, prev)) {
        return prev;
    }
    if (cpus_ptr(p)) {
        return mask_first(cpus_ptr(p));
    }
    return prev;
}

int fair_select_cpu(struct process *p, int prev, int why)
{
    int sync = (why & FAIR_WF_SYNC) != 0, cpu, new_cpu = prev, want_affine = 0, sd_flag = 0;
    struct sched_domain *tmp, *sd = NULL;
    process_t *cur;

    if (why & FAIR_WF_FORK) {
        p->se.recent_used_cpu = prev; /* wake_up_new_task */
    }
    if (!bops || !brq[prev].sd || mask_weight(cpus_ptr(p)) <= 1) {
        return fallback_cpu(p, prev);
    }
    cpu = (int)arch_cpu_id();
    cur = bops->curr(cpu);
    if (why & FAIR_WF_FORK) {
        sd_flag |= SD_BALANCE_FORK;
    }
    if (why & FAIR_WF_EXEC) {
        sd_flag |= SD_BALANCE_EXEC;
    }
    if (why & FAIR_WF_TTWU) {
        sd_flag |= SD_BALANCE_WAKE;
        record_wakee(cur, p);
        want_affine = !wake_wide(cur, p, cpu) && cpu_allowed(p, cpu);
    }
    for (tmp = brq[cpu].sd; tmp; tmp = tmp->parent) {
        /* The waker's and the last processor share this domain: aim for
         * one of them, and place by load no further. */
        if (want_affine && (tmp->flags & SD_WAKE_AFFINE) && mask_test(tmp->span, prev)) {
            if (cpu != prev) {
                new_cpu = wake_affine(tmp, cur, p, cpu, prev, sync);
            }
            sd = NULL;
            break;
        }
        if (tmp->flags & sd_flag) {
            sd = tmp;
        } else if (!want_affine) {
            break;
        }
    }
    if (sd) {
        new_cpu = find_dst_cpu(sd, p, cpu, prev, sd_flag);
    } else if (why & FAIR_WF_TTWU) {
        new_cpu = select_idle_sibling(cur, p, prev, new_cpu, cpu);
    }
    if (!cpu_allowed(p, new_cpu)) {
        new_cpu = fallback_cpu(p, prev);
    }
    return new_cpu;
}

/*
 * The load balance.
 */

/* A balance's flags (LBF_*): every thread looked at was pinned elsewhere;
 * stop to let other work in; a thread could go to another processor of the
 * local group; some thread was pinned; an active balance. */
#define LBF_ALL_PINNED  0x01u
#define LBF_NEED_BREAK  0x02u
#define LBF_DST_PINNED  0x04u
#define LBF_SOME_PINNED 0x08u
#define LBF_ACTIVE_LB   0x10u

/* One balance of a domain (struct lb_env). */
struct lb_env {
    struct sched_domain *sd;
    int src_cpu;
    int dst_cpu;
    int dst_core_idle;
    uint64_t dst_grpmask;
    int new_dst_cpu;
    int idle;
    long imbalance;
    uint64_t cpus;
    unsigned int flags;
    unsigned int loop;
    unsigned int loop_break;
    unsigned int loop_max;
    enum migration_type migration_type;
};

/* A domain's statistics (struct sd_lb_stats). */
struct sd_lb_stats {
    struct sched_group *busiest;
    struct sched_group *local;
    unsigned long total_load;
    unsigned long total_capacity;
    unsigned long avg_load;
    unsigned int prefer_sibling;
    struct sg_lb_stats busiest_stat;
    struct sg_lb_stats local_stat;
};

/** Compute what real-time threads and interrupts leave of `cpu` and give it
 * to its group of one in `sd` (update_cpu_capacity). */
static void update_cpu_capacity(struct sched_domain *sd, int cpu)
{
    unsigned long capacity = scale_rt_capacity(cpu);
    struct sched_group *sdg = sd->groups;

    if (!capacity) {
        capacity = 1;
    }
    brq[cpu].capacity = capacity;
    sdg->capacity = capacity;
    sdg->min_capacity = capacity;
    sdg->max_capacity = capacity;
}

/** Sum the capacities of the child's groups into `sd`'s local group, or
 * compute a processor's at the bottom (update_group_capacity). */
static void update_group_capacity(struct sched_domain *sd, int cpu)
{
    struct sched_domain *child = sd->child;
    struct sched_group *group, *sdg = sd->groups;
    unsigned long capacity = 0, min_capacity = (unsigned long)-1, max_capacity = 0;
    uint64_t interval;

    interval = MS_TO_TICKS(sd->balance_interval);
    if (interval < 1) {
        interval = 1;
    }
    if (interval > max_load_balance_interval) {
        interval = max_load_balance_interval;
    }
    sdg->next_update = kernel_ticks + interval;
    if (!child) {
        update_cpu_capacity(sd, cpu);
        return;
    }
    group = child->groups;
    do {
        capacity += group->capacity;
        if (group->min_capacity < min_capacity) {
            min_capacity = group->min_capacity;
        }
        if (group->max_capacity > max_capacity) {
            max_capacity = group->max_capacity;
        }
        group = group->next;
    } while (group != child->groups);
    sdg->capacity = capacity;
    sdg->min_capacity = min_capacity;
    sdg->max_capacity = max_capacity;
}

/** Whether real-time threads or interrupts take a noticeable part of `cpu`
 * (check_cpu_capacity). */
static int check_cpu_capacity(int cpu, const struct sched_domain *sd)
{
    return brq[cpu].capacity * sd->imbalance_pct < SCHED_CAPACITY_SCALE * 100;
}

/** Whether SMT siblings and lone processors are compared
 * (smt_vs_nonsmt_groups). */
static int smt_vs_nonsmt_groups(const struct sched_group *sg1, const struct sched_group *sg2)
{
    if (!sg1 || !sg2) {
        return 0;
    }
    return (sg1->flags & SD_SHARE_CPUCAPACITY) != (sg2->flags & SD_SHARE_CPUCAPACITY);
}

/** Whether an idle balance should move a thread off busy SMT siblings
 * (smt_balance). */
static int smt_balance(const struct lb_env *env, const struct sg_lb_stats *sgs,
                       const struct sched_group *group)
{
    if (env->idle == CPU_NOT_IDLE) {
        return 0;
    }
    return (group->flags & SD_SHARE_CPUCAPACITY) && sgs->sum_h_nr_running > 1;
}

/**
 * The threads to move so that the busiest and local groups run as many per
 * core, for an idle balance (sibling_imbalance).
 */
static long sibling_imbalance(const struct lb_env *env, const struct sd_lb_stats *sds,
                              const struct sg_lb_stats *busiest, const struct sg_lb_stats *local)
{
    long ncores_busiest, ncores_local, imbalance;

    if (env->idle == CPU_NOT_IDLE || !busiest->sum_nr_running) {
        return 0;
    }
    ncores_busiest = (long)sds->busiest->cores;
    ncores_local = (long)sds->local->cores;
    if (ncores_busiest == ncores_local) {
        imbalance = (long)busiest->sum_nr_running - (long)local->sum_nr_running;
        return imbalance > 0 ? imbalance : 0;
    }
    /* Balance by threads per core, rounded to the nearest. */
    imbalance =
        ncores_local * (long)busiest->sum_nr_running - ncores_busiest * (long)local->sum_nr_running;
    if (imbalance < 0) {
        imbalance = 0;
    }
    imbalance = 2 * imbalance + ncores_local + ncores_busiest;
    imbalance /= ncores_local + ncores_busiest;
    if (imbalance <= 1 && local->sum_nr_running == 0 && busiest->sum_nr_running > 1) {
        imbalance = 2;
    }
    return imbalance;
}

/** Whether `cpu` runs one fair thread and loses capacity to others
 * (sched_reduced_capacity). */
static int sched_reduced_capacity(int cpu, const struct sched_domain *sd)
{
    if (cpu_cfs(cpu)->h_nr_queued != 1) {
        return 0;
    }
    return check_cpu_capacity(cpu, sd);
}

/** A group's statistics for the balance, noting a processor with two
 * threads or more in `*overloaded` (update_sg_lb_stats). */
static void update_sg_lb_stats(struct lb_env *env, struct sd_lb_stats *sds,
                               struct sched_group *group, struct sg_lb_stats *sgs, int *overloaded)
{
    unsigned int nr_running;
    unsigned long load;
    int i, local_group = group == sds->local;

    k_memset(sgs, 0, sizeof(*sgs));
    for (i = 0; i < NR_CPUS; i++) {
        if (!mask_test(group->span & env->cpus, i)) {
            continue;
        }
        load = cpu_load(i);
        sgs->group_load += load;
        sgs->group_util += cpu_util_cfs(i);
        sgs->group_runnable += cpu_runnable(i);
        sgs->sum_h_nr_running += cpu_cfs(i)->h_nr_queued;
        nr_running = bops->nr_running(i);
        sgs->sum_nr_running += nr_running;
        if (nr_running > 1) {
            *overloaded = 1;
        }
        if (!nr_running && idle_cpu(i)) {
            sgs->idle_cpus++;
            continue;
        }
        if (local_group) {
            continue;
        }
        /* A processor whose one thread loses capacity to others is a
         * misfit for an idle balance. */
        if (env->idle != CPU_NOT_IDLE && sched_reduced_capacity(i, env->sd)) {
            if (sgs->group_misfit_task_load < load) {
                sgs->group_misfit_task_load = load;
            }
        }
    }
    sgs->group_capacity = group->capacity;
    sgs->group_weight = group->group_weight;
    if (!local_group && smt_balance(env, sgs, group)) {
        sgs->group_smt_balance = 1;
    }
    sgs->group_type = group_classify(env->sd->imbalance_pct, group, sgs);
    if (sgs->group_type == group_overloaded) {
        sgs->avg_load = sgs->group_load * SCHED_CAPACITY_SCALE / sgs->group_capacity;
    }
}

/** Whether `sg` is busier than the busiest group so far
 * (update_sd_pick_busiest). */
static int update_sd_pick_busiest(struct lb_env *env, struct sd_lb_stats *sds,
                                  struct sched_group *sg, struct sg_lb_stats *sgs)
{
    struct sg_lb_stats *busiest = &sds->busiest_stat;

    (void)env;
    if (!sgs->sum_h_nr_running) {
        return 0;
    }
    if (sgs->group_type > busiest->group_type) {
        return 1;
    }
    if (sgs->group_type < busiest->group_type) {
        return 0;
    }
    switch (sgs->group_type) {
    case group_overloaded:
        return sgs->avg_load > busiest->avg_load;
    case group_imbalanced:
        return 0;
    case group_asym_packing:
        return 0;
    case group_misfit_task:
        return sgs->group_misfit_task_load > busiest->group_misfit_task_load;
    case group_smt_balance:
        if (sgs->idle_cpus != 0 || busiest->idle_cpus != 0) {
            goto has_spare;
        }
        /* fall through */
    case group_fully_busy:
        if (sgs->avg_load < busiest->avg_load) {
            return 0;
        }
        if (sgs->avg_load == busiest->avg_load && (sds->busiest->flags & SD_SHARE_CPUCAPACITY)) {
            return 0;
        }
        break;
    case group_has_spare:
        /* Prefer to empty SMT siblings onto a lone processor. */
        if (smt_vs_nonsmt_groups(sds->busiest, sg)) {
            if ((sg->flags & SD_SHARE_CPUCAPACITY) && sgs->sum_h_nr_running <= 1) {
                return 0;
            }
            return 1;
        }
    has_spare:
        if (sgs->idle_cpus > busiest->idle_cpus) {
            return 0;
        }
        if (sgs->idle_cpus == busiest->idle_cpus &&
            sgs->sum_nr_running <= busiest->sum_nr_running) {
            return 0;
        }
        break;
    default:
        break;
    }
    return 1;
}

/**
 * Set how many processors of the cache domain a wake scans, from its
 * utilization: llc_weight * (1 - (util * pct / 100)^2) with util a share of
 * capacity (update_idle_cpu_scan).
 */
static void update_idle_cpu_scan(struct lb_env *env, unsigned long sum_util)
{
    struct sched_domain_shared *share;
    uint64_t x, y, tmp;
    unsigned int llc_weight, pct;

    if (!sched_feat(SIS_UTIL) || env->idle == CPU_NEWLY_IDLE) {
        return;
    }
    llc_weight = (unsigned int)brq[env->dst_cpu].sd_llc_size;
    if (env->sd->span_weight != llc_weight) {
        return;
    }
    share = brq[env->dst_cpu].sd_llc_shared;
    if (!share) {
        return;
    }
    x = div_u64(sum_util, llc_weight);
    pct = env->sd->imbalance_pct;
    tmp = div_u64(x * x * pct * pct, 10000u * SCHED_CAPACITY_SCALE);
    if (tmp > (uint64_t)SCHED_CAPACITY_SCALE) {
        tmp = SCHED_CAPACITY_SCALE;
    }
    y = (uint64_t)SCHED_CAPACITY_SCALE - tmp;
    y = div_u64(y * llc_weight, SCHED_CAPACITY_SCALE);
    share->nr_idle_scan = (int)y;
}

/** The statistics of every group of the domain (update_sd_lb_stats). */
static void update_sd_lb_stats(struct lb_env *env, struct sd_lb_stats *sds)
{
    struct sched_group *sg = env->sd->groups;
    struct sg_lb_stats *local = &sds->local_stat, tmp_sgs, *sgs;
    unsigned long sum_util = 0;
    int overloaded = 0, local_group;

    env->dst_core_idle = !smt_active || is_core_idle(env->dst_cpu);
    do {
        sgs = &tmp_sgs;
        local_group = mask_test(sg->span, env->dst_cpu);
        if (local_group) {
            sds->local = sg;
            sgs = local;
            if (env->idle != CPU_NEWLY_IDLE || time_after_eq(kernel_ticks, sg->next_update)) {
                update_group_capacity(env->sd, env->dst_cpu);
            }
        }
        update_sg_lb_stats(env, sds, sg, sgs, &overloaded);
        if (!local_group && update_sd_pick_busiest(env, sds, sg, sgs)) {
            sds->busiest = sg;
            sds->busiest_stat = *sgs;
        }
        sds->total_load += sgs->group_load;
        sds->total_capacity += sgs->group_capacity;
        sum_util += sgs->group_util;
        sg = sg->next;
    } while (sg != env->sd->groups);
    if (sds->busiest) {
        sds->prefer_sibling = (sds->busiest->flags & SD_PREFER_SIBLING) != 0;
    }
    update_idle_cpu_scan(env, sum_util);
}

/** How much to move, and of what, to even out the busiest and local groups
 * (calculate_imbalance). */
static void calculate_imbalance(struct lb_env *env, struct sd_lb_stats *sds)
{
    struct sg_lb_stats *local = &sds->local_stat, *busiest = &sds->busiest_stat;
    unsigned long a, b;

    if (busiest->group_type == group_misfit_task) {
        env->migration_type = migrate_load;
        env->imbalance = (long)busiest->group_misfit_task_load;
        return;
    }
    if (busiest->group_type == group_smt_balance || busiest->group_type == group_imbalanced) {
        env->migration_type = migrate_task;
        env->imbalance = 1;
        return;
    }
    if (local->group_type == group_has_spare) {
        if (busiest->group_type > group_fully_busy && !(env->sd->flags & SD_SHARE_LLC)) {
            /* Fill the local group's spare capacity by utilization. */
            env->migration_type = migrate_util;
            env->imbalance =
                (long)((local->group_capacity > local->group_util ? local->group_capacity
                                                                  : local->group_util) -
                       local->group_util);
            if (env->idle != CPU_NOT_IDLE && env->imbalance == 0) {
                env->migration_type = migrate_task;
                env->imbalance = 1;
            }
            return;
        }
        if (busiest->group_weight == 1 || sds->prefer_sibling) {
            env->migration_type = migrate_task;
            env->imbalance = sibling_imbalance(env, sds, busiest, local);
        } else {
            env->migration_type = migrate_task;
            env->imbalance = (long)local->idle_cpus - (long)busiest->idle_cpus;
            if (env->imbalance < 0) {
                env->imbalance = 0;
            }
        }
        env->imbalance >>= 1;
        return;
    }
    if (local->group_type < group_overloaded) {
        local->avg_load = local->group_load * SCHED_CAPACITY_SCALE / local->group_capacity;
        if (local->avg_load >= busiest->avg_load) {
            env->imbalance = 0;
            return;
        }
        sds->avg_load = sds->total_load * SCHED_CAPACITY_SCALE / sds->total_capacity;
        if (local->avg_load >= sds->avg_load) {
            env->imbalance = 0;
            return;
        }
    }
    /* Both groups overloaded, or the local one about to be: move load to
     * bring both to the domain's average, no further. */
    env->migration_type = migrate_load;
    a = (busiest->avg_load - sds->avg_load) * busiest->group_capacity;
    b = (sds->avg_load - local->avg_load) * local->group_capacity;
    env->imbalance = (long)((a < b ? a : b) / SCHED_CAPACITY_SCALE);
}

/** The busiest group of the domain when the balance should move threads
 * from it, else NULL (sched_balance_find_src_group). */
static struct sched_group *find_src_group(struct lb_env *env)
{
    struct sg_lb_stats *local, *busiest;
    struct sd_lb_stats sds;

    k_memset(&sds, 0, sizeof(sds));
    sds.busiest_stat.idle_cpus = (unsigned int)-1;
    sds.busiest_stat.group_type = group_has_spare;
    update_sd_lb_stats(env, &sds);
    if (!sds.busiest) {
        goto out_balanced;
    }
    busiest = &sds.busiest_stat;
    if (busiest->group_type == group_misfit_task) {
        goto force_balance;
    }
    if (busiest->group_type == group_imbalanced) {
        goto force_balance;
    }
    local = &sds.local_stat;
    if (local->group_type > busiest->group_type) {
        goto out_balanced;
    }
    if (local->group_type == group_overloaded) {
        if (local->avg_load >= busiest->avg_load) {
            goto out_balanced;
        }
        sds.avg_load = sds.total_load * SCHED_CAPACITY_SCALE / sds.total_capacity;
        if (local->avg_load >= sds.avg_load) {
            goto out_balanced;
        }
        if (100 * busiest->avg_load <= env->sd->imbalance_pct * local->avg_load) {
            goto out_balanced;
        }
    }
    if (sds.prefer_sibling && local->group_type == group_has_spare &&
        sibling_imbalance(env, &sds, busiest, local) > 1) {
        goto force_balance;
    }
    if (busiest->group_type != group_overloaded) {
        if (env->idle == CPU_NOT_IDLE) {
            /* A busy processor pulls only from an overloaded group. */
            goto out_balanced;
        }
        if (busiest->group_type == group_smt_balance &&
            smt_vs_nonsmt_groups(sds.local, sds.busiest)) {
            goto force_balance;
        }
        if (busiest->group_weight > 1 && local->idle_cpus <= busiest->idle_cpus + 1) {
            goto out_balanced;
        }
        if (busiest->sum_h_nr_running == 1) {
            goto out_balanced;
        }
    }
force_balance:
    calculate_imbalance(env, &sds);
    return env->imbalance ? sds.busiest : NULL;
out_balanced:
    env->imbalance = 0;
    return NULL;
}

/** The busiest processor of `group` by the balance's migration type
 * (sched_balance_find_src_rq), or -1. */
static int find_src_rq(struct lb_env *env, struct sched_group *group)
{
    unsigned long busiest_util = 0, busiest_load = 0, busiest_capacity = 1, capacity, load, util;
    unsigned int busiest_nr = 0, nr_running;
    int busiest = -1, i;

    for (i = 0; i < NR_CPUS; i++) {
        if (!mask_test(group->span & env->cpus, i)) {
            continue;
        }
        nr_running = cpu_cfs(i)->h_nr_queued;
        if (!nr_running) {
            continue;
        }
        capacity = balance_capacity(i);
        switch (env->migration_type) {
        case migrate_load:
            /* One thread heavier than the imbalance is left, unless its
             * processor loses capacity to others. */
            load = cpu_load(i);
            if (nr_running == 1 && load > (unsigned long)env->imbalance &&
                !check_cpu_capacity(i, env->sd)) {
                break;
            }
            if ((uint64_t)load * busiest_capacity > (uint64_t)busiest_load * capacity) {
                busiest_load = load;
                busiest_capacity = capacity;
                busiest = i;
            }
            break;
        case migrate_util:
            util = cpu_util_cfs_boost(i);
            if (nr_running <= 1) {
                continue;
            }
            if (busiest_util < util) {
                busiest_util = util;
                busiest = i;
            }
            break;
        case migrate_task:
            if (busiest_nr < nr_running) {
                busiest_nr = nr_running;
                busiest = i;
            }
            break;
        case migrate_misfit:
        default:
            break;
        }
    }
    return busiest;
}

/**
 * Whether `p` ran on its processor within migration_cost_ns, or is the
 * buddy a busy destination would run next, so that moving it loses its cache
 * (task_hot). SCHED_IDLE threads and moves between SMT siblings are never
 * hot.
 */
static int task_hot(process_t *p, struct lb_env *env)
{
    int64_t delta;

    if (p->sched_policy == SCHED_IDLE) {
        return 0;
    }
    if (env->sd->flags & SD_SHARE_CPUCAPACITY) {
        return 0;
    }
    if (sched_feat(CACHE_HOT_BUDDY) && bops->nr_running(env->dst_cpu) &&
        &p->se == p->se.cfs_rq->next) {
        return 1;
    }
    if (sysctl_sched_migration_cost == 0) {
        return 0;
    }
    delta = (int64_t)(fair_rqs[env->src_cpu].clock_task - p->se.exec_start);
    return delta < (int64_t)sysctl_sched_migration_cost;
}

/**
 * Whether the balance may move `p` from the source to the destination
 * (can_migrate_task): its affinity allows the destination, or else another
 * processor of the local group, noted for a retry; it is not running; and it
 * is cold, or the domain has failed to balance more than cache_nice_tries
 * times, or the balance is active. A delayed thread, asleep though still
 * queued, is not moved: `balance_ops.move` would finish its dequeue.
 */
static int can_migrate_task(process_t *p, struct lb_env *env)
{
    int hot, c;

    p->se.sched_task_hot = 0;
    if (!cpu_allowed(p, env->dst_cpu)) {
        if (fair_schedstats) {
            p->se.stats.nr_failed_migrations_affine++;
        }
        env->flags |= LBF_SOME_PINNED;
        if (env->idle == CPU_NEWLY_IDLE || (env->flags & (LBF_DST_PINNED | LBF_ACTIVE_LB))) {
            return 0;
        }
        for (c = 0; c < NR_CPUS; c++) {
            if (mask_test(env->dst_grpmask & env->cpus, c) && cpu_allowed(p, c)) {
                env->flags |= LBF_DST_PINNED;
                env->new_dst_cpu = c;
                break;
            }
        }
        return 0;
    }
    env->flags &= ~LBF_ALL_PINNED;
    if (bops->curr(env->src_cpu) == p || p->se.sched_delayed) {
        if (fair_schedstats) {
            p->se.stats.nr_failed_migrations_running++;
        }
        return 0;
    }
    if (env->flags & LBF_ACTIVE_LB) {
        return 1;
    }
    hot = task_hot(p, env);
    if (!hot || env->sd->nr_balance_failed > env->sd->cache_nice_tries) {
        if (hot) {
            p->se.sched_task_hot = 1;
        }
        return 1;
    }
    if (fair_schedstats) {
        p->se.stats.nr_failed_migrations_hot++;
    }
    return 0;
}

/** Move `p` to the balance's destination (detach_task and attach_task). */
static void move_task(process_t *p, struct lb_env *env)
{
    if (p->se.sched_task_hot) {
        p->se.sched_task_hot = 0;
        if (fair_schedstats) {
            p->se.stats.nr_forced_migrations++;
        }
    }
    bops->move(p, env->dst_cpu);
}

/** The last of processor `cpu`'s fair threads, the least recently picked,
 * or NULL when none is queued there. */
static struct sched_entity *tasks_last(int cpu)
{
    struct sched_entity *se = fair_rqs[cpu].cfs_tasks;

    while (se && se->tasks_next) {
        se = se->tasks_next;
    }
    return se;
}

/** Move `se` to the head of processor `cpu`'s `cfs_tasks`, as a thread the
 * balance skipped (list_move). */
static void tasks_move_head(int cpu, struct sched_entity *se)
{
    struct fair_rq *rq = &fair_rqs[cpu];

    if (rq->cfs_tasks == se) {
        return;
    }
    if (se->tasks_prev) {
        se->tasks_prev->tasks_next = se->tasks_next;
    }
    if (se->tasks_next) {
        se->tasks_next->tasks_prev = se->tasks_prev;
    }
    se->tasks_prev = NULL;
    se->tasks_next = rq->cfs_tasks;
    if (rq->cfs_tasks) {
        rq->cfs_tasks->tasks_prev = se;
    }
    rq->cfs_tasks = se;
}

/** Move one movable thread from the source, the least recently run first,
 * for an active balance (detach_one_task); 1 when one moved. */
static int move_one_task(struct lb_env *env)
{
    struct sched_entity *se;
    process_t *p;

    for (se = tasks_last(env->src_cpu); se; se = se->tasks_prev) {
        p = task_of(se);
        if (!can_migrate_task(p, env)) {
            continue;
        }
        move_task(p, env);
        return 1;
    }
    return 0;
}

/** A thread's share of the imbalance, halved for each failed balance
 * (shr_bound). */
static unsigned long shr_bound(unsigned long val, unsigned int shift)
{
    return shift < 8 * sizeof(val) ? val >> shift : 0;
}

/**
 * Move threads from the source to the destination until the imbalance is
 * met, looking at no more than `loop_max`, the least recently run first, and
 * stopping every SCHED_NR_MIGRATE_BREAK to let other work in (detach_tasks
 * and attach_tasks). Return how many moved.
 */
static int move_tasks(struct lb_env *env)
{
    struct sched_entity *se, *prev, *first;
    unsigned long util, load;
    process_t *p;
    int moved = 0;

    if (bops->nr_running(env->src_cpu) <= 1) {
        env->flags &= ~LBF_ALL_PINNED;
        return 0;
    }
    if (env->imbalance <= 0) {
        return 0;
    }
    /* From the tail back to the head as it was, so that a thread moved to
     * the head is not looked at again. */
    first = fair_rqs[env->src_cpu].cfs_tasks;
    for (se = tasks_last(env->src_cpu); se; se = se == first ? NULL : prev) {
        prev = se->tasks_prev;
        /* Leave an idle balance's source its last thread. */
        if (env->idle != CPU_NOT_IDLE && bops->nr_running(env->src_cpu) <= 1) {
            break;
        }
        env->loop++;
        if (env->loop > env->loop_max) {
            break;
        }
        if (env->loop > env->loop_break) {
            env->loop_break += SCHED_NR_MIGRATE_BREAK;
            env->flags |= LBF_NEED_BREAK;
            break;
        }
        p = task_of(se);
        if (!can_migrate_task(p, env)) {
            goto next;
        }
        switch (env->migration_type) {
        case migrate_load:
            load = task_h_load(p);
            if (load < 1) {
                load = 1;
            }
            if (sched_feat(LB_MIN) && load < 16 && !env->sd->nr_balance_failed) {
                goto next;
            }
            /* A thread far heavier than the imbalance is moved only
             * after balances fail. */
            if (shr_bound(load, env->sd->nr_balance_failed) > (unsigned long)env->imbalance) {
                goto next;
            }
            env->imbalance -= (long)load;
            break;
        case migrate_util:
            util = pelt_task_util_est(&p->se);
            if (shr_bound(util, env->sd->nr_balance_failed) > (unsigned long)env->imbalance) {
                goto next;
            }
            env->imbalance -= (long)util;
            break;
        case migrate_task:
            env->imbalance--;
            break;
        case migrate_misfit:
        default:
            env->imbalance = 0;
            break;
        }
        move_task(p, env);
        moved++;
        /* A newly idle balance moves one thread, as with preemption. */
        if (env->idle == CPU_NEWLY_IDLE) {
            break;
        }
        if (env->imbalance <= 0) {
            break;
        }
        continue;
    next:
        tasks_move_head(env->src_cpu, se);
    }
    return moved;
}

/** Whether a balance that moved nothing should push the source's running
 * thread instead (need_active_balance): after cache_nice_tries + 2 failures
 * to move a thread, or when the source's one thread loses capacity to
 * others that the destination has. */
static int need_active_balance(struct lb_env *env)
{
    struct sched_domain *sd = env->sd;

    if (env->migration_type == migrate_task && sd->nr_balance_failed > sd->cache_nice_tries + 2) {
        return 1;
    }
    if (env->idle != CPU_NOT_IDLE && cpu_cfs(env->src_cpu)->h_nr_queued == 1) {
        if (check_cpu_capacity(env->src_cpu, sd) &&
            balance_capacity(env->src_cpu) * sd->imbalance_pct <
                balance_capacity(env->dst_cpu) * 100) {
            return 1;
        }
    }
    return 0;
}

/**
 * Whether the destination is the processor of its group that should balance
 * the domain (should_we_balance): a newly idle one with nothing queued
 * always; else the first idle processor of the group, one of an idle core
 * first, or the group's first when none is idle.
 */
static int should_we_balance(struct lb_env *env)
{
    struct sched_group *sg = env->sd->groups;
    uint64_t swb_cpus;
    int cpu, idle_smt = -1;

    if (!mask_test(env->cpus, env->dst_cpu)) {
        return 0;
    }
    if (env->idle == CPU_NEWLY_IDLE) {
        return bops->nr_running(env->dst_cpu) == 0;
    }
    swb_cpus = sg->span;
    for (cpu = 0; cpu < NR_CPUS; cpu++) {
        if (!mask_test(swb_cpus & env->cpus, cpu)) {
            continue;
        }
        if (!idle_cpu(cpu)) {
            continue;
        }
        if (!(env->sd->flags & SD_SHARE_CPUCAPACITY) && !is_core_idle(cpu)) {
            if (idle_smt == -1) {
                idle_smt = cpu;
            }
            swb_cpus &= ~smt_mask[cpu];
            continue;
        }
        return cpu == env->dst_cpu;
    }
    if (idle_smt != -1) {
        return idle_smt == env->dst_cpu;
    }
    return mask_first(sg->span) == env->dst_cpu;
}

/**
 * Move threads from the busiest processor `busiest` as the balance `env`
 * asks, retrying with another destination of the local group for threads
 * pinned away from this one; return how many moved (the more_balance part
 * of sched_balance_rq).
 */
static int balance_pass(struct lb_env *env, int busiest)
{
    int moved = 0;

    env->loop_max = sysctl_sched_nr_migrate;
    if (env->loop_max > bops->nr_running(busiest)) {
        env->loop_max = bops->nr_running(busiest);
    }
    bops->update_clock(busiest);
    for (;;) {
        moved += move_tasks(env);
        if (env->flags & LBF_NEED_BREAK) {
            env->flags &= ~LBF_NEED_BREAK;
            continue;
        }
        /* Threads pinned away from the destination may go to another
         * processor of its group. */
        if ((env->flags & LBF_DST_PINNED) && env->imbalance > 0) {
            env->cpus &= ~cpu_bit(env->dst_cpu);
            env->dst_cpu = env->new_dst_cpu;
            env->flags &= ~LBF_DST_PINNED;
            env->loop = 0;
            env->loop_break = SCHED_NR_MIGRATE_BREAK;
            continue;
        }
        return moved;
    }
}

/**
 * After a balance that moved nothing, ask `busiest` to push a thread to
 * `this_cpu` when `need_active_balance` says so. Return 1 when the push was
 * asked, 0 when none was needed or one is on its way, and -1 when the
 * busiest processor's running thread may not run on `this_cpu`.
 */
static int start_active_balance(struct lb_env *env, int this_cpu, int busiest)
{
    process_t *cur;

    if (!need_active_balance(env)) {
        return 0;
    }
    cur = bops->curr(busiest);
    if (cur && !cpu_allowed(cur, this_cpu)) {
        return -1;
    }
    env->flags &= ~LBF_ALL_PINNED;
    if (brq[busiest].active_balance) {
        return 0;
    }
    brq[busiest].active_balance = 1;
    brq[busiest].push_cpu = this_cpu;
    bops->kick(busiest, BALANCE_KICK_ACTIVE);
    return 1;
}

/**
 * Balance domain `sd` of processor `this_cpu`, idle as `idle` says: find the
 * busiest group and processor, move threads from it, retry around pinned
 * ones, start an active balance after repeated failures, and adjust the
 * domain's interval (sched_balance_rq). Returns how many threads moved;
 * clears `*continue_balancing` when another processor of the group should
 * balance instead.
 */
static int sched_balance_rq(int this_cpu, struct sched_domain *sd, int idle,
                            int *continue_balancing)
{
    struct sched_domain *sd_parent = sd->parent;
    struct sched_group *group;
    struct lb_env env;
    int ld_moved = 0, active_balance = 0, busiest;

    k_memset(&env, 0, sizeof(env));
    env.sd = sd;
    env.dst_cpu = this_cpu;
    env.dst_grpmask = sd->groups->span;
    env.idle = idle;
    env.loop_break = SCHED_NR_MIGRATE_BREAK;
    env.cpus = sd->span & active_mask;
redo:
    if (!should_we_balance(&env)) {
        *continue_balancing = 0;
        goto out_balanced;
    }
    group = find_src_group(&env);
    if (!group) {
        goto out_balanced;
    }
    busiest = find_src_rq(&env, group);
    if (busiest < 0) {
        goto out_balanced;
    }
    env.src_cpu = busiest;
    env.flags |= LBF_ALL_PINNED;
    if (bops->nr_running(busiest) > 1) {
        ld_moved = balance_pass(&env, busiest);
        /* Pinned threads kept this level from balancing: let the parent
         * know its group is imbalanced. */
        if (sd_parent && (env.flags & LBF_SOME_PINNED) && env.imbalance > 0) {
            sd_parent->groups->imbalance = 1;
        }
        if (env.flags & LBF_ALL_PINNED) {
            env.cpus &= ~cpu_bit(busiest);
            if (env.cpus & ~env.dst_grpmask) {
                env.loop = 0;
                env.loop_break = SCHED_NR_MIGRATE_BREAK;
                goto redo;
            }
            goto out_all_pinned;
        }
    }
    if (!ld_moved) {
        if (idle != CPU_NEWLY_IDLE) {
            sd->nr_balance_failed++;
        }
        active_balance = start_active_balance(&env, this_cpu, busiest);
        if (active_balance < 0) {
            goto out_one_pinned;
        }
    } else {
        sd->nr_balance_failed = 0;
    }
    if (!active_balance || need_active_balance(&env)) {
        sd->balance_interval = sd->min_interval;
    }
    return ld_moved;

out_balanced:
    if (sd_parent && !(env.flags & LBF_ALL_PINNED) && sd_parent->groups->imbalance) {
        sd_parent->groups->imbalance = 0;
    }
out_all_pinned:
    sd->nr_balance_failed = 0;
out_one_pinned:
    if (env.idle == CPU_NEWLY_IDLE) {
        return 0;
    }
    if (((env.flags & LBF_ALL_PINNED) && sd->balance_interval < MAX_PINNED_INTERVAL) ||
        sd->balance_interval < sd->max_interval) {
        sd->balance_interval *= 2;
    }
    return 0;
}

void fair_balance_active(int cpu)
{
    struct sched_domain *sd;
    struct lb_env env;
    int target = brq[cpu].push_cpu, moved = 0;

    if (!bops || !brq[cpu].active_balance) {
        return;
    }
    if (!mask_test(active_mask, cpu) || !mask_test(active_mask, target) ||
        bops->nr_running(cpu) <= 1 || target == cpu) {
        goto out;
    }
    for (sd = brq[target].sd; sd; sd = sd->parent) {
        if (mask_test(sd->span, cpu)) {
            break;
        }
    }
    if (sd) {
        k_memset(&env, 0, sizeof(env));
        env.sd = sd;
        env.dst_cpu = target;
        env.src_cpu = cpu;
        env.idle = CPU_IDLE;
        env.flags = LBF_ACTIVE_LB;
        bops->update_clock(cpu);
        moved = move_one_task(&env);
        if (moved) {
            sd->nr_balance_failed = 0;
        }
    }
out:
    brq[cpu].active_balance = 0;
}

/** A domain's interval in ticks, longer on a busy processor
 * (get_sd_balance_interval). */
static uint64_t get_sd_balance_interval(const struct sched_domain *sd, int cpu_busy)
{
    uint64_t interval = sd->balance_interval;

    if (cpu_busy) {
        interval *= sd->busy_factor;
    }
    interval = MS_TO_TICKS(interval);
    if (cpu_busy && interval > 0) {
        interval -= 1;
    }
    if (interval < 1) {
        interval = 1;
    }
    if (interval > max_load_balance_interval) {
        interval = max_load_balance_interval;
    }
    return interval;
}

/** Bring `*next_balance` forward to `sd`'s next idle balance
 * (update_next_balance). */
static void update_next_balance(const struct sched_domain *sd, uint64_t *next_balance)
{
    uint64_t next = sd->last_balance + get_sd_balance_interval(sd, 0);

    if (time_after(*next_balance, next)) {
        *next_balance = next;
    }
}

/** Count a newly idle balance of `sd` and whether it pulled
 * (update_newidle_stats). */
static void update_newidle_stats(struct sched_domain *sd, unsigned int success)
{
    sd->newidle_call++;
    sd->newidle_success += success;
    if (sd->newidle_call >= 1024) {
        sd->newidle_ratio = sd->newidle_success;
        sd->newidle_call /= 2;
        sd->newidle_success /= 2;
    }
}

/** Keep the most a newly idle balance of `sd` cost, decaying it by 1% a
 * second; 1 when it decayed (update_newidle_cost). */
static int update_newidle_cost(struct sched_domain *sd, uint64_t cost, unsigned int success)
{
    uint64_t next_decay = sd->last_decay_max_lb_cost + TICK_HZ;

    if (cost) {
        update_newidle_stats(sd, success);
    }
    if (cost > sd->max_newidle_lb_cost) {
        sd->max_newidle_lb_cost = cost;
        sd->last_decay_max_lb_cost = kernel_ticks;
    } else if (time_after(kernel_ticks, next_decay)) {
        sd->max_newidle_lb_cost = div_u64(sd->max_newidle_lb_cost * 253u, 256u);
        sd->last_decay_max_lb_cost = kernel_ticks;
        return 1;
    }
    return 0;
}

/** Balance each domain of `cpu` whose interval has passed, idle as `idle`
 * says (sched_balance_domains). */
static void sched_balance_domains(int cpu, int idle)
{
    int continue_balancing = 1, busy, need_decay = 0, update = 0;
    uint64_t next_balance = kernel_ticks + 60u * TICK_HZ, interval, max_cost = 0;
    struct sched_domain *sd;

    busy = idle != CPU_IDLE && !sched_idle_cpu(cpu);
    for (sd = brq[cpu].sd; sd; sd = sd->parent) {
        need_decay = update_newidle_cost(sd, 0, 0);
        max_cost += sd->max_newidle_lb_cost;
        if (!continue_balancing) {
            if (need_decay) {
                continue;
            }
            break;
        }
        interval = get_sd_balance_interval(sd, busy);
        if (time_after_eq(kernel_ticks, sd->last_balance + interval)) {
            if (sched_balance_rq(cpu, sd, idle, &continue_balancing)) {
                /* Threads came: the processor may no longer be idle. */
                idle = idle_cpu(cpu) ? CPU_IDLE : CPU_NOT_IDLE;
                busy = idle != CPU_IDLE && !sched_idle_cpu(cpu);
            }
            sd->last_balance = kernel_ticks;
            interval = get_sd_balance_interval(sd, busy);
        }
        if (time_after(next_balance, sd->last_balance + interval)) {
            next_balance = sd->last_balance + interval;
            update = 1;
        }
    }
    if (need_decay) {
        brq[cpu].max_idle_balance_cost = max_cost > sysctl_sched_migration_cost
                                             ? max_cost
                                             : (uint64_t)sysctl_sched_migration_cost;
    }
    if (update) {
        brq[cpu].next_balance = next_balance;
    }
}

/** Decay `cpu`'s blocked load and note whether some is left
 * (sched_balance_update_blocked_averages). */
static void update_blocked_averages(int cpu)
{
    process_t *cur;
    int rt_running;

    brq[cpu].last_blocked_load_update_tick = kernel_ticks;
    bops->update_clock(cpu);
    cur = bops->curr(cpu);
    rt_running = cur && sched_priority(cur) > 0;
    if (!pelt_update_blocked(cpu, fair_rqs[cpu].clock_task, rt_running)) {
        brq[cpu].has_blocked_load = 0;
    }
}

/*
 * The idle balance of processors whose tick is stopped (nohz), and the
 * entries the scheduler calls.
 */

/** Count `cpu` busy again in its cache domain's record
 * (set_cpu_sd_state_busy). */
static void set_cpu_sd_state_busy(int cpu)
{
    struct sched_domain *sd = brq[cpu].sd_llc;

    if (!sd || !sd->nohz_idle) {
        return;
    }
    sd->nohz_idle = 0;
    sd->shared->nr_busy_cpus++;
}

/** Count `cpu` idle in its cache domain's record (set_cpu_sd_state_idle). */
static void set_cpu_sd_state_idle(int cpu)
{
    struct sched_domain *sd = brq[cpu].sd_llc;

    if (!sd || sd->nohz_idle) {
        return;
    }
    sd->nohz_idle = 1;
    sd->shared->nr_busy_cpus--;
}

/** `cpu`'s tick runs again: take it out of the idle balance's set
 * (nohz_balance_exit_idle). */
static void nohz_balance_exit_idle(int cpu)
{
    if (!brq[cpu].nohz_tick_stopped) {
        return;
    }
    brq[cpu].nohz_tick_stopped = 0;
    nohz.idle_cpus_mask &= ~cpu_bit(cpu);
    nohz.nr_cpus--;
    set_cpu_sd_state_busy(cpu);
}

void fair_balance_idle_enter(int cpu)
{
    if (!bops || !mask_test(active_mask, cpu)) {
        return;
    }
    brq[cpu].has_blocked_load = 1;
    if (!brq[cpu].nohz_tick_stopped) {
        if (!brq[cpu].sd) {
            return;
        }
        brq[cpu].nohz_tick_stopped = 1;
        nohz.idle_cpus_mask |= cpu_bit(cpu);
        nohz.nr_cpus++;
        set_cpu_sd_state_idle(cpu);
        nohz.needs_update = 1;
    }
    nohz.has_blocked = 1;
}

/** An idle processor of the stopped-tick set, other than `this_cpu`, to
 * balance for the others, or -1 (find_new_ilb). */
static int find_new_ilb(int this_cpu)
{
    int c;

    for (c = 0; c < NR_CPUS; c++) {
        if (c != this_cpu && mask_test(nohz.idle_cpus_mask, c) && idle_cpu(c)) {
            return c;
        }
    }
    return -1;
}

/** Ask an idle processor to run the idle balance for `flags` (kick_ilb);
 * the processor asked, or -1. */
static int kick_ilb(int this_cpu, unsigned int flags)
{
    unsigned int old;
    int ilb;

    if (flags & NOHZ_BALANCE_KICK) {
        nohz.next_balance = kernel_ticks + 1;
    }
    ilb = find_new_ilb(this_cpu);
    if (ilb < 0) {
        return -1;
    }
    if ((brq[ilb].nohz_flags & flags) == flags) {
        return -1;
    }
    old = brq[ilb].nohz_flags;
    brq[ilb].nohz_flags |= flags;
    /* A kick already on its way carries these too. */
    if (old & NOHZ_KICK_MASK) {
        return -1;
    }
    bops->kick(ilb, BALANCE_KICK_NOHZ);
    return ilb;
}

int fair_nohz_kick(int cpu)
{
    struct sched_domain_shared *sds;
    unsigned int flags = 0;

    if (!bops || brq[cpu].idle_balance) {
        return -1;
    }
    nohz_balance_exit_idle(cpu);
    if (!nohz.nr_cpus) {
        return -1;
    }
    if (nohz.has_blocked && time_after(kernel_ticks, nohz.next_blocked)) {
        flags = NOHZ_STATS_KICK;
    }
    if (time_after(nohz.next_balance, kernel_ticks)) {
        goto out;
    }
    /* Two threads here: an idle processor can take one. */
    if (bops->nr_running(cpu) >= 2) {
        flags = NOHZ_STATS_KICK | NOHZ_BALANCE_KICK;
        goto out;
    }
    if (brq[cpu].sd && cpu_cfs(cpu)->h_nr_queued >= 1 && check_cpu_capacity(cpu, brq[cpu].sd)) {
        flags = NOHZ_STATS_KICK | NOHZ_BALANCE_KICK;
        goto out;
    }
    /* Another busy processor shares the cache: one may spread. */
    sds = brq[cpu].sd_llc_shared;
    if (sds && sds->nr_busy_cpus > 1) {
        flags = NOHZ_STATS_KICK | NOHZ_BALANCE_KICK;
    }
out:
    if (nohz.needs_update) {
        flags |= NOHZ_NEXT_KICK;
    }
    if (flags) {
        return kick_ilb(cpu, flags);
    }
    return -1;
}

/** Decay a stopped-tick processor's blocked load once a tick at most; 1
 * while some is left (update_nohz_stats). */
static int update_nohz_stats(int cpu)
{
    if (!brq[cpu].has_blocked_load) {
        return 0;
    }
    if (!mask_test(nohz.idle_cpus_mask, cpu)) {
        return 0;
    }
    if (!time_after(kernel_ticks, brq[cpu].last_blocked_load_update_tick)) {
        return 1;
    }
    update_blocked_averages(cpu);
    return brq[cpu].has_blocked_load;
}

/**
 * On `this_cpu`, for each idle processor with a stopped tick, from the one
 * after it: decay its blocked load and balance its domains when due, as
 * `flags` asks, stopping when work comes for `this_cpu` (_nohz_idle_balance).
 */
static void nohz_idle_balance_run(int this_cpu, unsigned int flags)
{
    uint64_t now = kernel_ticks, next_balance = now + 60u * TICK_HZ;
    int has_blocked_load = 0, update = 0, i, c;

    if (flags & NOHZ_STATS_KICK) {
        nohz.has_blocked = 0;
    }
    if (flags & NOHZ_NEXT_KICK) {
        nohz.needs_update = 0;
    }
    for (i = 1; i <= NR_CPUS; i++) {
        c = (this_cpu + i) % NR_CPUS;
        if (!mask_test(nohz.idle_cpus_mask, c) || !idle_cpu(c)) {
            continue;
        }
        if (!idle_cpu(this_cpu)) {
            if (flags & NOHZ_STATS_KICK) {
                has_blocked_load = 1;
            }
            if (flags & NOHZ_NEXT_KICK) {
                nohz.needs_update = 1;
            }
            goto abort;
        }
        if (flags & NOHZ_STATS_KICK) {
            has_blocked_load |= update_nohz_stats(c);
        }
        if (time_after_eq(kernel_ticks, brq[c].next_balance)) {
            bops->update_clock(c);
            if (flags & NOHZ_BALANCE_KICK) {
                sched_balance_domains(c, CPU_IDLE);
            }
        }
        if (time_after(next_balance, brq[c].next_balance)) {
            next_balance = brq[c].next_balance;
            update = 1;
        }
    }
    if (update) {
        nohz.next_balance = next_balance;
    }
    if (flags & NOHZ_STATS_KICK) {
        nohz.next_blocked = now + MS_TO_TICKS(LOAD_AVG_PERIOD);
    }
abort:
    if (has_blocked_load) {
        nohz.has_blocked = 1;
    }
}

/** The balance work of a processor's tick or kick (sched_balance_softirq):
 * the idle balance for the others when kicked for it, else its own. */
static void balance_softirq(int cpu)
{
    int idle = brq[cpu].idle_balance ? CPU_IDLE : CPU_NOT_IDLE;
    unsigned int flags = brq[cpu].nohz_flags & NOHZ_KICK_MASK;

    if (flags) {
        brq[cpu].nohz_flags &= ~NOHZ_KICK_MASK;
        if (idle == CPU_IDLE) {
            nohz_idle_balance_run(cpu, flags);
            return;
        }
    }
    update_blocked_averages(cpu);
    sched_balance_domains(cpu, idle);
}

void fair_nohz_balance(int cpu)
{
    unsigned int flags;

    if (!bops || !brq[cpu].sd) {
        return;
    }
    flags = brq[cpu].nohz_flags;
    if (flags & NOHZ_KICK_MASK) {
        /* The kick of `fair_nohz_kick`: balance while idle
         * (nohz_csd_func). */
        brq[cpu].idle_balance = idle_cpu(cpu);
        if (!brq[cpu].idle_balance) {
            brq[cpu].nohz_flags &= ~NOHZ_KICK_MASK;
            return;
        }
        balance_softirq(cpu);
        return;
    }
    /* On the way into the idle wait: the blocked load a newly idle
     * balance asked to decay (nohz_run_idle_balance). */
    brq[cpu].nohz_flags &= ~NOHZ_NEWILB_KICK;
    if (flags == NOHZ_NEWILB_KICK && idle_cpu(cpu)) {
        nohz_idle_balance_run(cpu, NOHZ_STATS_KICK);
    }
}

void fair_balance_tick(int cpu, int idle)
{
    if (!bops || !brq[cpu].sd || !mask_test(active_mask, cpu)) {
        return;
    }
    brq[cpu].idle_balance = idle != 0;
    if (time_after_eq(kernel_ticks, brq[cpu].next_balance)) {
        balance_softirq(cpu);
    }
    fair_nohz_kick(cpu);
}

/** Whether any started processor has two threads or more, so that a newly
 * idle balance may find one to pull (the root domain's overloaded mark). */
static int rd_overloaded(void)
{
    int c;

    for (c = 0; c < NR_CPUS; c++) {
        if (mask_test(active_mask, c) && bops->nr_running(c) > 1) {
            return 1;
        }
    }
    return 0;
}

/** Ask for the idle balance's decay of the blocked load from a processor
 * going idle for long enough (nohz_newidle_balance). */
static void nohz_newidle_balance(int cpu)
{
    if (brq[cpu].avg_idle < sysctl_sched_migration_cost) {
        return;
    }
    if (!nohz.has_blocked || time_after(nohz.next_blocked, kernel_ticks)) {
        return;
    }
    brq[cpu].nohz_flags |= NOHZ_NEWILB_KICK;
}

int fair_balance_newidle(int cpu)
{
    uint64_t next_balance = kernel_ticks + TICK_HZ, t0, t1, curr_cost = 0, domain_cost;
    struct sched_domain *sd;
    int continue_balancing = 1, pulled_task = 0;
    unsigned int weight, d1k;

    if (!bops) {
        return 0;
    }
    brq[cpu].idle_stamp = bops->clock();
    if (!mask_test(active_mask, cpu)) {
        return 0;
    }
    sd = brq[cpu].sd;
    if (!sd) {
        goto out;
    }
    if (!rd_overloaded() || brq[cpu].avg_idle < sd->max_newidle_lb_cost) {
        update_next_balance(sd, &next_balance);
        goto out;
    }
    t0 = bops->clock();
    update_blocked_averages(cpu);
    for (sd = brq[cpu].sd; sd; sd = sd->parent) {
        update_next_balance(sd, &next_balance);
        if (brq[cpu].avg_idle < curr_cost + sd->max_newidle_lb_cost) {
            break;
        }
        if (sd->flags & SD_BALANCE_NEWIDLE) {
            weight = 1;
            /* Skip at random by the domain's past success (NI_RANDOM),
             * and weigh a success by the skipping. */
            if (sched_feat(NI_RANDOM)) {
                d1k = sched_rng(cpu) % 1024u;
                weight = 1 + sd->newidle_ratio;
                if (d1k > weight) {
                    update_newidle_stats(sd, 0);
                    continue;
                }
                weight = (1024 + weight / 2) / weight;
            }
            pulled_task = sched_balance_rq(cpu, sd, CPU_NEWLY_IDLE, &continue_balancing);
            t1 = bops->clock();
            domain_cost = t1 - t0;
            curr_cost += domain_cost;
            t0 = t1;
            update_newidle_cost(sd, domain_cost, weight * (pulled_task != 0));
        }
        if (pulled_task || !continue_balancing) {
            break;
        }
    }
    if (curr_cost > brq[cpu].max_idle_balance_cost) {
        brq[cpu].max_idle_balance_cost = curr_cost;
    }
    if (cpu_cfs(cpu)->h_nr_queued && !pulled_task) {
        pulled_task = 1;
    }
    if (bops->nr_running(cpu) != cpu_cfs(cpu)->h_nr_queued) {
        pulled_task = -1;
    }
out:
    if (time_after(brq[cpu].next_balance, next_balance)) {
        brq[cpu].next_balance = next_balance;
    }
    if (pulled_task) {
        brq[cpu].idle_stamp = 0;
    } else {
        nohz_newidle_balance(cpu);
        update_idle_core(cpu);
    }
    return pulled_task;
}

void fair_balance_woken(int cpu)
{
    uint64_t delta, max;

    if (!bops || !brq[cpu].idle_stamp) {
        return;
    }
    delta = bops->clock() - brq[cpu].idle_stamp;
    max = 2u * brq[cpu].max_idle_balance_cost;
    /* avg += (sample - avg) / 8, rounding toward zero (update_avg). */
    if (delta >= brq[cpu].avg_idle) {
        brq[cpu].avg_idle += (delta - brq[cpu].avg_idle) >> 3;
    } else {
        brq[cpu].avg_idle -= (brq[cpu].avg_idle - delta) >> 3;
    }
    if (brq[cpu].avg_idle > max) {
        brq[cpu].avg_idle = max;
    }
    brq[cpu].idle_stamp = 0;
}
