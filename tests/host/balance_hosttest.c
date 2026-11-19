/**
 * Host test of the fair class's load tracking and balancing, proc/pelt.c and
 * proc/balance.c, over a model of the processors and their fair queues.
 *
 * host-deps: rach2/sys/lib/kstring.c rach2/sys/lib/kctypes.c rach2/sys/tick.c
 * host-cflags: -DRACH2_HOST_CPU
 *
 * PELT's sums and averages are the kernel's; the rows compare them with the
 * geometric series computed exactly in double precision. The processors,
 * their queues and the order of running are the model's, which charges the
 * running thread a tick at a time, picks the thread that has run least for
 * its weight, and calls PELT at each change as fair.c does; the domains,
 * placement and balancing are the kernel's, and the model applies the moves
 * they ask for. The rows count ticks, never the host's clock.
 */

#include "proc/pelt.c"
#include "proc/balance.c"

extern int dprintf(int fd, const char *format, ...);

struct fair_rq fair_rqs[NR_CPUS];
struct fair_hooks fair_hooks;
struct task_group root_task_group;
uint32_t sched_features;
int fair_schedstats;
volatile uint64_t kernel_ticks;
unsigned int host_cpu;

static int failures;

#define CHECK(test, message)                                       \
    do {                                                           \
        if (!(test)) {                                             \
            dprintf(2, "balance-hosttest: failed: %s\n", message); \
            failures++;                                            \
        }                                                          \
    } while (0)

/** A kernel thread has an entry of its own; the model's threads have none. */
int proc_is_kthread(const process_t *p)
{
    return p->kthread_entry != NULL;
}

/** Take `cfs_rq` off its processor's leaf list, as fair.c does. */
void fair_leaf_del(struct cfs_rq *cfs_rq)
{
    struct fair_rq *rq = &fair_rqs[cfs_rq->cpu];

    if (!cfs_rq->on_list) {
        return;
    }
    if (cfs_rq->leaf_prev) {
        cfs_rq->leaf_prev->leaf_next = cfs_rq->leaf_next;
    } else {
        rq->leaf_head = cfs_rq->leaf_next;
    }
    if (cfs_rq->leaf_next) {
        cfs_rq->leaf_next->leaf_prev = cfs_rq->leaf_prev;
    }
    cfs_rq->leaf_prev = cfs_rq->leaf_next = NULL;
    cfs_rq->on_list = 0;
}

/** |a - b| for doubles. */
static double fabs_d(double a)
{
    return a < 0 ? -a : a;
}

/** The square root of `x` > 0 by Newton's method, with no libm. */
static double sqrt_d(double x)
{
    double r = x > 1 ? x : 1;
    int i;

    for (i = 0; i < 100; i++) {
        r = 0.5 * (r + x / r);
    }
    return r;
}

/** y, with y^32 = 1/2: five square roots of 1/2. */
static double pelt_y(void)
{
    double y = 0.5;
    int i;

    for (i = 0; i < 5; i++) {
        y = sqrt_d(y);
    }
    return y;
}

/*
 * PELT, against the series it approximates (87-2a).
 */

/** `val` * y^n in double. */
static double decay_exact(double val, unsigned int n)
{
    double y = pelt_y();

    while (n--) {
        val *= y;
    }
    return val;
}

/** decay_load matches val * y^n within its fixed point, and reaches 0
 * past 63 half-lives. */
static void decay_rows(void)
{
    static const uint64_t vals[] = {1024, 47742, 1u << 20, 123456789u, (uint64_t)1 << 40};
    unsigned int i, n;
    double exact, got;
    int bad = 0;

    for (i = 0; i < sizeof(vals) / sizeof(vals[0]); i++) {
        for (n = 0; n <= 2016; n += 7) {
            exact = decay_exact((double)vals[i], n);
            got = (double)pelt_decay_load(vals[i], n);
            if (fabs_d(got - exact) > exact * 1e-6 + 2) {
                bad++;
            }
        }
    }
    CHECK(bad == 0, "decay_load gives val * y^n");
    /* y^0 is 0xffffffff / 2^32 in sched-pelt.h, so Linux gives 511. */
    CHECK(pelt_decay_load(1024, 32) == 511, "32 periods halve the value");
    CHECK(pelt_decay_load((uint64_t)1 << 40, 2017) == 0, "past 63 half-lives a sum is gone");
}

/**
 * Run `sa` from time 0 for `units` units of 1024 ns, running `on` units then
 * stopping `off` units in turn (always running when `off` is 0), updating at
 * each change as an entity on its queue does; return the util_avg of the
 * series, where a unit in period P counts y^(Pnow - P) at the end, in period
 * Pnow.
 */
static double run_pattern(struct sched_avg *sa, unsigned long weight, uint64_t units, uint64_t on,
                          uint64_t off)
{
    static double count[4096];
    double y = pelt_y(), s = 0;
    uint64_t u, p, pnow = units / 1024;
    int running = 1, was;

    k_memset(sa, 0, sizeof(*sa));
    k_memset(count, 0, sizeof(count));
    for (u = 0; u < units; u++) {
        was = running;
        if (off) {
            running = (u % (on + off)) < on;
        }
        if (running != was) {
            pelt_update_sum(u << 10, sa, 1, was, was);
            pelt_update_avg(sa, weight);
        }
        count[u / 1024] += running;
    }
    pelt_update_sum(units << 10, sa, 1, running, running);
    pelt_update_avg(sa, weight);
    for (p = 0; p <= pnow; p++) {
        s = s * y + count[p];
    }
    return 1024.0 * s / (double)(PELT_MIN_DIVIDER + (units % 1024));
}

/** A steady, a half-busy and a sleeping entity, against the series. */
static void pelt_entity_rows(void)
{
    struct sched_avg sa;
    double exact;
    uint64_t t;
    unsigned long before;

    /* Steady: always running for a second. */
    exact = run_pattern(&sa, 1024, 1000000, 1, 0);
    CHECK(sa.util_avg >= 1015 && sa.util_avg <= 1024, "a steady entity's utilization is full");
    CHECK(sa.load_avg >= 1015 && sa.load_avg <= 1024,
          "a steady nice-0 entity's load is its weight");
    CHECK(sa.runnable_avg >= 1015 && sa.runnable_avg <= 1024, "a steady entity is runnable");
    CHECK(fabs_d((double)sa.util_avg - exact) <= exact * 0.01 + 2,
          "steady utilization matches the series");

    /* Ramp: 32 periods from nothing reach half. */
    exact = run_pattern(&sa, 1024, 32 * 1024, 1, 0);
    CHECK(sa.util_avg >= 500 && sa.util_avg <= 524, "32 periods of running reach half");
    CHECK(fabs_d((double)sa.util_avg - exact) <= exact * 0.01 + 2, "the ramp matches the series");

    /* Half-busy: 8 periods on, 8 off, for two seconds, ending on. */
    exact = run_pattern(&sa, 1024, 2047 * 1024 + 512, 8 * 1024, 8 * 1024);
    CHECK(fabs_d((double)sa.util_avg - exact) <= exact * 0.01 + 2,
          "a half-busy entity's utilization matches the series");
    CHECK(sa.util_avg > 400 && sa.util_avg < 650, "a half-busy entity is near half");

    /* Sleeping: a steady entity stops for 32 periods, then for long. */
    run_pattern(&sa, 1024, 1000 * 1024, 1, 0);
    before = sa.util_avg;
    t = sa.last_update_time + ((uint64_t)32 * 1024 << 10);
    pelt_update_sum(t, &sa, 0, 0, 0);
    pelt_update_avg(&sa, 1024);
    CHECK(sa.util_avg * 2 >= before - 12 && sa.util_avg * 2 <= before + 12,
          "32 periods asleep halve the utilization");
    t += (uint64_t)2100 * 1024 << 10;
    pelt_update_sum(t, &sa, 0, 0, 0);
    pelt_update_avg(&sa, 1024);
    CHECK(sa.util_avg == 0 && sa.load_avg == 0 && sa.util_sum == 0 && sa.load_sum == 0,
          "a long sleep decays every sum to 0");
}

/* A group of one queue on processor 0 for the group rows. */
static struct task_group tg1;
static struct sched_entity *tg1_se[NR_CPUS];
static struct cfs_rq *tg1_cfs[NR_CPUS];
static struct sched_entity gse;
static struct cfs_rq gcfs;

/** Queue the entity `se` of a thread or group on `cfs_rq` at `now`, as
 * enqueue_entity does for PELT and the counts. */
static void pelt_enqueue(uint64_t now, struct cfs_rq *cfs_rq, struct sched_entity *se)
{
    pelt_update_load_avg(now, cfs_rq, se, FAIR_LOAD_UPDATE_TG | FAIR_LOAD_ATTACH);
    if (se->my_q) {
        se->runnable_weight = se->my_q->h_nr_runnable;
    }
    cfs_rq->load.weight += se->load.weight;
    cfs_rq->nr_running++;
    se->on_rq = 1;
}

/** Take `se` off `cfs_rq` at `now` as a sleep does (dequeue_entity). */
static void pelt_dequeue(uint64_t now, struct cfs_rq *cfs_rq, struct sched_entity *se)
{
    pelt_update_load_avg(now, cfs_rq, se, FAIR_LOAD_UPDATE_TG);
    if (se->my_q) {
        se->runnable_weight = se->my_q->h_nr_runnable;
    }
    cfs_rq->load.weight -= se->load.weight;
    cfs_rq->nr_running--;
    se->on_rq = 0;
}

/** A thread in a group: its load reaches the root through the group
 * entity, its hierarchical load is the root's, and the group's load is its
 * queue's; asleep, every queue decays and leaves the leaf list. */
static void pelt_group_rows(void)
{
    struct cfs_rq *root = &fair_rqs[0].cfs;
    struct sched_entity se;
    uint64_t now = 1000000000u, t;
    unsigned long h;
    int i, left;

    k_memset(&se, 0, sizeof(se));
    k_memset(&fair_rqs[0], 0, sizeof(fair_rqs[0]));
    root->tg = &root_task_group;
    root_task_group.se = NULL;
    tg1.se = tg1_se;
    tg1.cfs_rq = tg1_cfs;
    tg1.parent = &root_task_group;
    tg1.shares = 1024;
    tg1_se[0] = &gse;
    tg1_cfs[0] = &gcfs;
    gcfs.tg = &tg1;
    gse.my_q = &gcfs;
    gse.cfs_rq = root;
    gse.load.weight = 1024;
    pelt_init_entity(&gse);
    se.cfs_rq = &gcfs;
    se.parent = &gse;
    se.depth = 1;
    se.load.weight = 1024;
    pelt_init_entity(&se);
    /* The leaf list, child first. */
    fair_rqs[0].leaf_head = &gcfs;
    gcfs.leaf_next = root;
    root->leaf_prev = &gcfs;
    gcfs.on_list = root->on_list = 1;

    gcfs.h_nr_queued = gcfs.h_nr_runnable = 1;
    root->h_nr_queued = root->h_nr_runnable = 1;
    pelt_enqueue(now, &gcfs, &se);
    CHECK(gcfs.propagate == 1, "an attach marks the group's queue for propagation");
    pelt_enqueue(now, root, &gse);
    CHECK(gcfs.propagate == 0 && root->propagate == 1,
          "the group entity's update carries it to the parent");
    gcfs.curr = &se;
    root->curr = &gse;
    for (i = 1; i <= 1000; i++) {
        now = 1000000000u + (uint64_t)i * 1000000u;
        pelt_update_load_avg(now, &gcfs, &se, FAIR_LOAD_UPDATE_TG);
        pelt_update_load_avg(now, root, &gse, FAIR_LOAD_UPDATE_TG);
    }
    CHECK(se.avg.util_avg >= 1000, "the thread runs all the time");
    CHECK(gse.avg.util_avg >= 1000 && root->avg.util_avg >= 1000,
          "the group entity and the root take the thread's utilization");
    CHECK(root->avg.load_avg >= 1000 && root->avg.load_avg <= 1024,
          "the root's load is the group entity's weight");
    h = pelt_task_h_load(&se, 1);
    CHECK(h >= 990 && h <= 1024, "the thread's hierarchical load is the root's");
    CHECK(tg1.load_avg >= 1000 && tg1.load_avg <= 1024, "the group's load is its queue's");

    /* Asleep: off both queues, then the blocked decay. */
    gcfs.curr = NULL;
    root->curr = NULL;
    gcfs.h_nr_queued = gcfs.h_nr_runnable = 0;
    root->h_nr_queued = root->h_nr_runnable = 0;
    pelt_dequeue(now, &gcfs, &se);
    pelt_dequeue(now, root, &gse);
    t = now + (uint64_t)32 * 1024 * 1024;
    left = pelt_update_blocked(0, t, 0);
    CHECK(left, "blocked load is left after 32 periods");
    CHECK(gcfs.avg.util_avg >= 490 && gcfs.avg.util_avg <= 530,
          "the group queue's utilization halves");
    CHECK(root->avg.util_avg >= 490 && root->avg.util_avg <= 530, "the root's utilization halves");
    t += (uint64_t)2100 * 1024 * 1024;
    pelt_update_blocked(0, t, 0);
    t += (uint64_t)1024 * 1024 * 1024;
    left = pelt_update_blocked(0, t, 0);
    CHECK(!left, "no blocked load is left after a long sleep");
    CHECK(gcfs.avg.load_avg == 0 && root->avg.load_avg == 0 && gse.avg.load_avg == 0,
          "every average decays to 0");
    CHECK(tg1.load_avg == 0, "the group's load goes with its queue's");
    CHECK(!gcfs.on_list && !root->on_list, "decayed queues leave the leaf list");
}

/** An attach and a detach leave the queue as it was; a removal is taken at
 * the queue's next update. */
static void pelt_attach_rows(void)
{
    struct cfs_rq *cfs = &fair_rqs[1].cfs;
    struct sched_entity a, b;
    uint64_t now = 1000000000u;
    unsigned long load;
    int i;

    k_memset(&fair_rqs[1], 0, sizeof(fair_rqs[1]));
    cfs->tg = &root_task_group;
    cfs->cpu = 1;
    k_memset(&a, 0, sizeof(a));
    k_memset(&b, 0, sizeof(b));
    a.cfs_rq = b.cfs_rq = cfs;
    a.load.weight = b.load.weight = 1024;
    pelt_init_entity(&a);
    pelt_init_entity(&b);
    cfs->h_nr_queued = cfs->h_nr_runnable = 2;
    pelt_enqueue(now, cfs, &a);
    pelt_enqueue(now, cfs, &b);
    for (i = 1; i <= 500; i++) {
        now = 1000000000u + (uint64_t)i * 1000000u;
        cfs->curr = (i & 1) ? &a : &b;
        pelt_update_load_avg(now, cfs, &a, FAIR_LOAD_UPDATE_TG);
        pelt_update_load_avg(now, cfs, &b, FAIR_LOAD_UPDATE_TG);
    }
    CHECK(cfs->avg.load_avg >= 2000 && cfs->avg.load_avg <= 2048,
          "two runnable entities load the queue twice");
    CHECK(a.avg.util_avg >= 480 && a.avg.util_avg <= 540, "two sharing a processor use half each");
    load = cfs->avg.load_avg;
    cfs->curr = NULL;
    pelt_update_load_avg(now, cfs, &b, FAIR_LOAD_UPDATE_TG | FAIR_LOAD_DETACH);
    CHECK(cfs->avg.load_avg + b.avg.load_avg == load, "a detach takes the entity's load out");
    pelt_update_load_avg(now, cfs, &b, FAIR_LOAD_SKIP_AGE);
    b.avg.last_update_time = 0;
    pelt_update_load_avg(now, cfs, &b, FAIR_LOAD_ATTACH);
    CHECK(cfs->avg.load_avg >= load - 2 && cfs->avg.load_avg <= load + 2, "an attach puts it back");
    pelt_remove_entity(&a);
    CHECK(cfs->removed.nr == 1, "a removal is noted for the queue");
    pelt_update_cfs_rq_load(now + 2000000u, cfs);
    CHECK(cfs->removed.nr == 0 && cfs->avg.load_avg < load * 3 / 4, "the queue takes the removal");
}

/*
 * The processor model for the balance rows (87-2b to 87-2e).
 */

#define NTASK         8
#define NICE19_WEIGHT 15 /* sched_prio_to_weight[39] */
#define TICKNS        ((uint64_t)TICK_NS)

static process_t tasks[NTASK];
static int ntasks, ncpus;
static uint64_t now_ns;
static process_t *mcurr[NR_CPUS];
static int tick_stopped[NR_CPUS];
static unsigned int kick_nohz[NR_CPUS], kick_active[NR_CPUS];
static unsigned int moves;
static int model_balancing;

/** The threads queued on `cpu`, the running one included. */
static unsigned int m_nr_running(int cpu)
{
    unsigned int n = 0;
    int i;

    for (i = 0; i < ntasks; i++) {
        n += tasks[i].cpu == cpu && tasks[i].se.on_rq;
    }
    return n;
}

/** The thread running on `cpu`, NULL when it idles. */
static struct process *m_curr(int cpu)
{
    return mcurr[cpu];
}

/** Whether `cpu` idles with nothing queued. */
static int m_idle(int cpu)
{
    return mcurr[cpu] == NULL && m_nr_running(cpu) == 0;
}

/** The model's monotonic clock in ns. */
static uint64_t m_clock(void)
{
    return now_ns;
}

/** Give `cpu` the model's clock as its clock and task clock. */
static void m_update_clock(int cpu)
{
    fair_rqs[cpu].clock = now_ns;
    fair_rqs[cpu].clock_task = now_ns;
}

/** Record a kick of `cpu` for `why`, which `model_tick` serves. */
static void m_kick(int cpu, int why)
{
    if (why == BALANCE_KICK_NOHZ) {
        kick_nohz[cpu]++;
    } else {
        kick_active[cpu]++;
    }
}

/** Put `p` at the head of its processor's `cfs_tasks` (fair.c's
 * cfs_tasks_add). */
static void list_push(process_t *p)
{
    struct fair_rq *rq = &fair_rqs[p->cpu];

    p->se.tasks_prev = NULL;
    p->se.tasks_next = rq->cfs_tasks;
    if (rq->cfs_tasks) {
        rq->cfs_tasks->tasks_prev = &p->se;
    }
    rq->cfs_tasks = &p->se;
}

/** Take `p` off its processor's `cfs_tasks` (cfs_tasks_del). */
static void list_del(process_t *p)
{
    struct fair_rq *rq = &fair_rqs[p->cpu];

    if (p->se.tasks_prev) {
        p->se.tasks_prev->tasks_next = p->se.tasks_next;
    } else if (rq->cfs_tasks == &p->se) {
        rq->cfs_tasks = p->se.tasks_next;
    }
    if (p->se.tasks_next) {
        p->se.tasks_next->tasks_prev = p->se.tasks_prev;
    }
    p->se.tasks_prev = p->se.tasks_next = NULL;
}

/** Queue `p` on its processor, as enqueue_task_fair does for PELT. */
static void model_enqueue(process_t *p)
{
    struct cfs_rq *cfs = &fair_rqs[p->cpu].cfs;

    m_update_clock(p->cpu);
    p->se.cfs_rq = cfs;
    pelt_util_est_enqueue(cfs, &p->se);
    pelt_update_load_avg(now_ns, cfs, &p->se, FAIR_LOAD_UPDATE_TG | FAIR_LOAD_ATTACH);
    cfs->load.weight += p->se.load.weight;
    cfs->nr_running++;
    cfs->h_nr_queued++;
    cfs->h_nr_runnable++;
    p->se.on_rq = 1;
    list_push(p);
}

/** Take `p` off its processor's queue, detaching it when it migrates. */
static void model_dequeue(process_t *p, int migrate)
{
    struct cfs_rq *cfs = &fair_rqs[p->cpu].cfs;

    m_update_clock(p->cpu);
    pelt_util_est_dequeue(cfs, &p->se);
    pelt_update_load_avg(now_ns, cfs, &p->se,
                         FAIR_LOAD_UPDATE_TG | (migrate ? FAIR_LOAD_DETACH : 0));
    cfs->load.weight -= p->se.load.weight;
    cfs->nr_running--;
    cfs->h_nr_queued--;
    cfs->h_nr_runnable--;
    p->se.on_rq = 0;
    list_del(p);
}

/** Move `p`, queued and not running, to `to` (detach_task, attach_task). */
static void m_move(struct process *p, int to)
{
    CHECK(mcurr[p->cpu] != p, "the balance moves no running thread");
    model_dequeue(p, 1);
    p->se.avg.last_update_time = 0;
    p->cpu = to;
    model_enqueue(p);
    moves++;
}

static const struct balance_ops model_ops = {
    m_nr_running, m_curr, m_idle, m_move, m_kick, m_clock, m_update_clock,
};

/** Start the model with `n` processors, domains from `smt` and `llc` masks
 * given per processor. */
static void model_start(int n, const uint64_t *smt, const uint64_t *llc)
{
    int c;

    k_memset(tasks, 0, sizeof(tasks));
    k_memset(fair_rqs, 0, sizeof(fair_rqs));
    k_memset(mcurr, 0, sizeof(mcurr));
    k_memset(tick_stopped, 0, sizeof(tick_stopped));
    k_memset(brq, 0, sizeof(brq));
    k_memset(&nohz, 0, sizeof(nohz));
    k_memset(smt_mask, 0, sizeof(smt_mask));
    k_memset(llc_mask, 0, sizeof(llc_mask));
    smt_active = 0;
    ntasks = 0;
    ncpus = n;
    moves = 0;
    model_balancing = 1;
    sysctl_sched_migration_cost = 500000u;
    now_ns = 1000000000u;
    kernel_ticks = 250;
    for (c = 0; c < NR_CPUS; c++) {
        fair_rqs[c].cpu = c;
        fair_rqs[c].cfs.cpu = c;
        fair_rqs[c].cfs.tg = &root_task_group;
        kick_nohz[c] = kick_active[c] = 0;
    }
    for (c = 0; c < n; c++) {
        balance_cpu_topology(c, smt ? smt[c] : 0, llc ? llc[c] : 0);
    }
    balance_init(&model_ops);
    balance_build_domains(((uint64_t)1 << n) - 1);
}

/** Add a spinner of `weight` on `cpu`, may run on `affinity`. */
static process_t *model_spinner(int cpu, unsigned long weight, uint64_t affinity)
{
    process_t *p = &tasks[ntasks++];

    p->state = PROC_READY;
    p->cpu = cpu;
    p->affinity = affinity;
    p->se.load.weight = weight;
    p->se.recent_used_cpu = cpu;
    pelt_init_entity(&p->se);
    p->se.cfs_rq = &fair_rqs[cpu].cfs;
    model_enqueue(p);
    pelt_post_init_entity(&p->se);
    return p;
}

/** The queued thread of `cpu` that has run least for its weight. */
static process_t *model_pick(int cpu)
{
    process_t *best = NULL;
    uint64_t v, bv = 0;
    int i;

    for (i = 0; i < ntasks; i++) {
        process_t *p = &tasks[i];

        if (p->cpu != cpu || !p->se.on_rq) {
            continue;
        }
        v = p->se.sum_exec_runtime * 1024u / p->se.load.weight;
        if (!best || v < bv) {
            best = p;
            bv = v;
        }
    }
    return best;
}

/** Make `p`, or nothing, run on `cpu` from now, charging the one before. */
static void model_switch(int cpu, process_t *p)
{
    struct cfs_rq *cfs = &fair_rqs[cpu].cfs;
    process_t *prev = mcurr[cpu];

    if (prev) {
        prev->se.exec_start = now_ns;
    }
    mcurr[cpu] = p;
    cfs->curr = p ? &p->se : NULL;
    if (p) {
        p->se.exec_start = now_ns;
        list_del(p);
        list_push(p);
    }
}

/** Age every queued thread of `cpu` and its queue to now. */
static void model_age(int cpu)
{
    struct cfs_rq *cfs = &fair_rqs[cpu].cfs;
    int i;

    m_update_clock(cpu);
    for (i = 0; i < ntasks; i++) {
        if (tasks[i].cpu == cpu && tasks[i].se.on_rq) {
            pelt_update_load_avg(now_ns, cfs, &tasks[i].se, FAIR_LOAD_UPDATE_TG);
        }
    }
    pelt_update_cfs_rq_load(now_ns, cfs);
}

/** Pick again on `cpu`: the least run queued thread, or a newly idle
 * balance and the idle wait when there is none. */
static void model_select(int cpu)
{
    process_t *next = model_pick(cpu);

    if (!next && model_balancing) {
        host_cpu = (unsigned int)cpu;
        mcurr[cpu] = NULL;
        fair_rqs[cpu].cfs.curr = NULL;
        if (fair_balance_newidle(cpu) > 0) {
            next = model_pick(cpu);
        }
    }
    model_switch(cpu, next);
    if (!next && model_balancing && !tick_stopped[cpu]) {
        tick_stopped[cpu] = 1;
        fair_balance_idle_enter(cpu);
        fair_nohz_balance(cpu);
    }
    if (next) {
        tick_stopped[cpu] = 0;
    }
}

/** One tick of the model on every processor: charge, age, select, then the
 * tick's balance and the kicks it sent. */
static void model_tick(void)
{
    int c;

    now_ns += TICKNS;
    kernel_ticks++;
    for (c = 0; c < ncpus; c++) {
        if (mcurr[c]) {
            mcurr[c]->se.sum_exec_runtime += TICKNS;
        }
        model_age(c);
        if (mcurr[c] || !tick_stopped[c] || m_nr_running(c)) {
            model_select(c);
        }
    }
    if (!model_balancing) {
        return;
    }
    for (c = 0; c < ncpus; c++) {
        if (tick_stopped[c]) {
            continue;
        }
        host_cpu = (unsigned int)c;
        fair_balance_tick(c, m_idle(c));
    }
    for (c = 0; c < ncpus; c++) {
        host_cpu = (unsigned int)c;
        if (kick_active[c]) {
            kick_active[c] = 0;
            model_switch(c, NULL);
            model_select(c);
            fair_balance_active(c);
        }
        if (kick_nohz[c]) {
            kick_nohz[c] = 0;
            fair_nohz_balance(c);
        }
        if (!mcurr[c] && m_nr_running(c)) {
            model_select(c);
        }
    }
}

/** Run the model for `ticks` ticks. */
static void model_run(int ticks)
{
    while (ticks-- > 0) {
        model_tick();
    }
}

/*
 * The domains (87-2b).
 */

/** SMT pairs under one cache: an SMT level of each pair below a cache
 * level of four, whose groups are the cores, and no package level. */
static void domain_smt_rows(void)
{
    static const uint64_t smt_pairs[4] = {0x3, 0x3, 0xc, 0xc};
    static const uint64_t one_llc[4] = {0xf, 0xf, 0xf, 0xf};
    struct sched_domain *sd;

    model_start(4, smt_pairs, one_llc);
    sd = balance_domain(0);
    if (!sd || !sd->parent) {
        CHECK(0, "SMT pairs make two levels");
        return;
    }
    CHECK(sd->span == 0x3 && (sd->flags & SD_SHARE_CPUCAPACITY) && (sd->flags & SD_SHARE_LLC),
          "SMT siblings form the lowest domain");
    CHECK(sd->imbalance_pct == 110 && sd->cache_nice_tries == 0, "SMT's imbalance is 110%");
    CHECK(sd->groups->span == 0x1 && sd->groups->next->span == 0x2,
          "SMT's groups are its processors");
    sd = sd->parent;
    CHECK(sd->span == 0xf && sd->imbalance_pct == 117 && sd->cache_nice_tries == 1,
          "the cache domain spans the package");
    CHECK(sd->groups->span == 0x3 && sd->groups->next->span == 0xc && sd->groups->cores == 1,
          "the cache domain's groups are the cores");
    CHECK(sd->parent == NULL, "a package of one cache has no package level");
    CHECK(brq[0].sd_llc == sd && brq[0].sd_llc_size == 4 && brq[3].sd_llc_id == 0,
          "the cache domain is the top sharing the cache");
    CHECK(sd->min_interval == 4 && sd->max_interval == 8 && sd->busy_factor == 16,
          "intervals are one and two ms per processor");
}

/** Without SMT the level of one processor is dropped; with two caches the
 * package level spans them, its groups the caches. */
static void domain_llc_rows(void)
{
    static const uint64_t one_llc[4] = {0xf, 0xf, 0xf, 0xf};
    static const uint64_t two_llc[4] = {0x3, 0x3, 0xc, 0xc};
    struct sched_domain *sd;

    model_start(4, NULL, one_llc);
    sd = balance_domain(2);
    CHECK(sd && sd->span == 0xf && sd->child == NULL && !(sd->flags & SD_SHARE_CPUCAPACITY),
          "without SMT the level of one processor is dropped");
    CHECK(sd && sd->groups->span == 0x4, "a processor's own group comes first");

    model_start(4, NULL, two_llc);
    sd = balance_domain(1);
    if (!sd || !sd->parent) {
        CHECK(0, "two caches make two levels");
        return;
    }
    CHECK(sd->span == 0x3 && (sd->flags & SD_SHARE_LLC), "each cache is a domain");
    sd = sd->parent;
    CHECK(sd->span == 0xf && !(sd->flags & SD_SHARE_LLC) && sd->name == sd_names[SD_LEVEL_PKG],
          "the package spans both caches");
    CHECK(sd->groups->span == 0x3 && sd->groups->next->span == 0xc,
          "the package's groups are the caches");
    CHECK((sd->flags & SD_PREFER_SIBLING) && sd->imbalance_pct == 117,
          "the package prefers siblings");
}

/*
 * Wake placement (87-2c).
 */

/** A wake goes to an idle last processor, to an idle processor of the
 * cache when the last one is busy, and stays put when every one is busy. */
static void placement_rows(void)
{
    static const uint64_t one_llc[3] = {0x7, 0x7, 0x7};
    process_t *waker, *busy, *p;
    int cpu;

    model_start(3, NULL, one_llc);
    model_balancing = 0;
    waker = model_spinner(0, 1024, 0x7);
    busy = model_spinner(1, 1024, 0x7);
    model_run(1);
    p = &tasks[ntasks++];
    p->state = PROC_READY;
    p->affinity = 0x7;
    p->se.load.weight = 1024;
    p->cpu = 2;
    p->se.recent_used_cpu = 2;
    pelt_init_entity(&p->se);
    p->se.cfs_rq = &fair_rqs[2].cfs;
    host_cpu = 0;
    cpu = fair_select_cpu(p, 2, FAIR_WF_TTWU);
    CHECK(cpu == 2, "a wake goes to its idle last processor");
    p->cpu = 1;
    p->se.recent_used_cpu = 1;
    brq[0].sd_llc_shared->nr_idle_scan = 3; /* set by the periodic balance */
    cpu = fair_select_cpu(p, 1, FAIR_WF_TTWU);
    CHECK(cpu == 2, "a wake whose last processor is busy goes to an idle one of its cache");
    model_spinner(2, 1024, 0x7);
    model_run(1);
    cpu = fair_select_cpu(p, 1, FAIR_WF_TTWU);
    CHECK(cpu == 1 || cpu == 0, "with every processor busy a wake goes to the waker's or its own");
    p->affinity = 0x4;
    cpu = fair_select_cpu(p, 1, FAIR_WF_TTWU);
    CHECK(cpu == 2, "a wake goes only where its affinity allows");
    (void)waker;
    (void)busy;
}

/*
 * Balancing (87-2d and 87-2e).
 */

/**
 * Two nice-0 spinners and one nice-19 spinner, all started on processor 0
 * of two, end with the nice-0 ones on separate processors by load, and stay.
 * The nice-19 one goes on moving: its load, 15, exceeds the imbalance of
 * about 7 that it makes on either side, and detach_tasks halves the load it
 * compares for each failed balance, so every second balance moves it.
 */
static void spread_rows(void)
{
    static const uint64_t llc[2] = {0x3, 0x3};
    process_t *a, *b, *c;
    int bcpu, ccpu, i, stayed = 1;
    unsigned int settled;

    model_start(2, NULL, llc);
    a = model_spinner(0, NICE19_WEIGHT, 0x3);
    b = model_spinner(0, 1024, 0x3);
    c = model_spinner(0, 1024, 0x3);
    model_run(500);
    CHECK(b->cpu != c->cpu, "the two nice-0 spinners run on separate processors");
    bcpu = b->cpu;
    ccpu = c->cpu;
    settled = moves;
    for (i = 0; i < 500; i++) {
        model_tick();
        stayed &= b->cpu == bcpu && c->cpu == ccpu;
    }
    CHECK(stayed, "and stay where they are");
    CHECK(moves - settled >= 2, "the nice-19 spinner goes on moving");
    (void)a;
}

/** A cache-hot thread stays while the balance fails up to cache_nice_tries
 * times, and moves on the balance after. */
static void hot_rows(void)
{
    static const uint64_t llc[2] = {0x3, 0x3};
    struct sched_domain *sd;
    process_t *a, *b;
    int cpu1_moves = 0, i;

    model_start(2, NULL, llc);
    model_balancing = 0;
    a = model_spinner(0, 1024, 0x3);
    b = model_spinner(0, 1024, 0x3);
    model_run(50);
    model_balancing = 1;
    sysctl_sched_migration_cost = 4000000000u;
    fair_schedstats = 1;
    sd = balance_domain(1);
    host_cpu = 1;
    for (i = 0; i < 3; i++) {
        kernel_ticks += 4;
        now_ns += 4 * TICKNS;
        brq[1].next_balance = 0;
        sd->last_balance = 0;
        brq[1].idle_balance = 1;
        balance_softirq(1);
        if (i < 2) {
            CHECK(a->cpu == 0 && b->cpu == 0, "a cache-hot thread stays");
            CHECK(sd->nr_balance_failed == (unsigned int)i + 1, "each failure is counted");
        } else {
            cpu1_moves = (a->cpu == 1) + (b->cpu == 1);
        }
    }
    CHECK(cpu1_moves == 1, "after cache_nice_tries failures the hot thread moves");
    CHECK(a->se.stats.nr_forced_migrations + b->se.stats.nr_forced_migrations == 1,
          "the move is counted as forced");
    CHECK(sd->nr_balance_failed == 0, "a move clears the failures");
    fair_schedstats = 0;
}

/** An idle processor with a stopped tick pulls a movable thread within one
 * balance interval, while the pinned ones stay. */
static void nohz_rows(void)
{
    static const uint64_t llc[4] = {0xf, 0xf, 0xf, 0xf};
    process_t *p1, *p2, *m;
    int c;

    model_start(4, NULL, llc);
    model_balancing = 0;
    p1 = model_spinner(0, 1024, 0x1);
    p2 = model_spinner(0, 1024, 0x1);
    m = model_spinner(0, 1024, 0xf);
    model_run(100);
    /* The idle processors stop their ticks. */
    for (c = 1; c < 4; c++) {
        tick_stopped[c] = 1;
        host_cpu = (unsigned int)c;
        fair_balance_idle_enter(c);
    }
    model_balancing = 1;
    CHECK(nohz.nr_cpus == 3, "three processors have stopped ticks");
    CHECK(brq[0].sd_llc_shared->nr_busy_cpus == 1, "one processor of the cache is busy");
    model_run(1);
    CHECK(m->cpu != 0, "a stopped-tick processor pulls the movable thread in one interval");
    CHECK(p1->cpu == 0 && p2->cpu == 0, "the pinned threads stay");
    model_run(250);
    CHECK(p1->cpu == 0 && p2->cpu == 0 && m->cpu != 0, "and nothing moves back");
}

/** Four spinners on two processors of one cache, with an idle cache beside
 * it, spread across both caches, one to a processor. */
static void llc_rows(void)
{
    static const uint64_t llc[4] = {0x3, 0x3, 0xc, 0xc};
    unsigned int per_cpu[4] = {0, 0, 0, 0};
    int i;

    model_start(4, NULL, llc);
    model_spinner(0, 1024, 0xf);
    model_spinner(0, 1024, 0xf);
    model_spinner(1, 1024, 0xf);
    model_spinner(1, 1024, 0xf);
    model_run(750);
    for (i = 0; i < ntasks; i++) {
        per_cpu[tasks[i].cpu]++;
    }
    CHECK(per_cpu[0] == 1 && per_cpu[1] == 1 && per_cpu[2] == 1 && per_cpu[3] == 1,
          "a domain spanning two caches balances one thread to each processor");
}

int main(void)
{
    sched_features = (1u << FEAT_CACHE_HOT_BUDDY) | (1u << FEAT_SIS_UTIL) | (1u << FEAT_WA_IDLE) |
                     (1u << FEAT_WA_WEIGHT) | (1u << FEAT_WA_BIAS) | (1u << FEAT_UTIL_EST) |
                     (1u << FEAT_NI_RANDOM) | (1u << FEAT_ATTACH_AGE_LOAD);
    pelt_init();
    decay_rows();
    pelt_entity_rows();
    pelt_group_rows();
    pelt_attach_rows();
    domain_smt_rows();
    domain_llc_rows();
    placement_rows();
    spread_rows();
    hot_rows();
    nohz_rows();
    llc_rows();
    if (failures) {
        dprintf(2, "balance-hosttest: %d failures\n", failures);
        return failures > 255 ? 255 : failures;
    }
    dprintf(1, "balance-hosttest: passed\n");
    return 0;
}
