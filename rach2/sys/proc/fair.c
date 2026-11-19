/*
 * fair.c: Linux 6.12's fair scheduling class: virtual runtime, the EEVDF
 * pick of the earliest eligible virtual deadline, lag kept across sleeps,
 * nice weights and the group hierarchy.
 */

#include "fair.h"
#include "process.h"
#include "sched.h"
#include "../lib/kstring.h"
#include "../timer.h"

/*
 * Each function below is the Linux 6.12.115 function of the same name in
 * kernel/sched/fair.c, less what Rach2 does not build: bandwidth control,
 * NUMA balancing, core scheduling, util_est and the high-resolution tick.
 * The fair server, load tracking, group shares and balancing are in
 * sched.c, pelt.c, fair_group.c and balance.c, called through `fair_hooks`
 * where Linux calls them inline.
 *
 * Linux's flags, which the hooks of fair.h translate to.
 */
#define ENQUEUE_WAKEUP   0x01
#define ENQUEUE_RESTORE  0x02
#define ENQUEUE_MIGRATED 0x04
#define ENQUEUE_INITIAL  0x08
#define DEQUEUE_SLEEP    0x01
#define DEQUEUE_SAVE     0x02
#define DEQUEUE_SPECIAL  0x04
#define DEQUEUE_DELAYED  0x08
#define DEQUEUE_MIGRATE  0x10 /* Linux's task_on_rq_migrating */

struct fair_rq fair_rqs[NR_CPUS];
struct fair_hooks fair_hooks;

static struct sched_entity *root_se[NR_CPUS];
static struct cfs_rq *root_cfs_rq[NR_CPUS];

struct task_group root_task_group = {root_se, root_cfs_rq, NICE_0_LOAD, 0, 0, NULL, NULL, NULL};

uint64_t sysctl_sched_base_slice = FAIR_BASE_SLICE_NS;
int fair_schedstats;
int fair_groups_enabled = 1;

#define FEAT_ON(f) (1ul << FEAT_##f)
uint32_t sched_features =
    FEAT_ON(PLACE_LAG) | FEAT_ON(PLACE_DEADLINE_INITIAL) | FEAT_ON(PLACE_REL_DEADLINE) |
    FEAT_ON(RUN_TO_PARITY) | FEAT_ON(PREEMPT_SHORT) | FEAT_ON(PICK_BUDDY) |
    FEAT_ON(CACHE_HOT_BUDDY) | FEAT_ON(DELAY_DEQUEUE) | FEAT_ON(DELAY_ZERO) |
    FEAT_ON(WAKEUP_PREEMPTION) | FEAT_ON(NONTASK_CAPACITY) | FEAT_ON(TTWU_QUEUE) |
    FEAT_ON(SIS_UTIL) | FEAT_ON(RT_PUSH_IPI) | FEAT_ON(ATTACH_AGE_LOAD) | FEAT_ON(WA_IDLE) |
    FEAT_ON(WA_WEIGHT) | FEAT_ON(WA_BIAS) | FEAT_ON(UTIL_EST) | FEAT_ON(NI_RANDOM);

const char *const sched_feature_names[FEAT_COUNT] = {"PLACE_LAG",
                                                     "PLACE_DEADLINE_INITIAL",
                                                     "PLACE_REL_DEADLINE",
                                                     "RUN_TO_PARITY",
                                                     "PREEMPT_SHORT",
                                                     "NEXT_BUDDY",
                                                     "PICK_BUDDY",
                                                     "CACHE_HOT_BUDDY",
                                                     "DELAY_DEQUEUE",
                                                     "DELAY_ZERO",
                                                     "WAKEUP_PREEMPTION",
                                                     "HRTICK",
                                                     "HRTICK_DL",
                                                     "DOUBLE_TICK",
                                                     "NONTASK_CAPACITY",
                                                     "TTWU_QUEUE",
                                                     "SIS_UTIL",
                                                     "WARN_DOUBLE_CLOCK",
                                                     "RT_PUSH_IPI",
                                                     "RT_RUNTIME_SHARE",
                                                     "LB_MIN",
                                                     "ATTACH_AGE_LOAD",
                                                     "WA_IDLE",
                                                     "WA_WEIGHT",
                                                     "WA_BIAS",
                                                     "UTIL_EST",
                                                     "LATENCY_WARN",
                                                     "NI_RANDOM"};

const unsigned long sched_prio_to_weight[40] = {
    88761, 71755, 56483, 46273, 36291, /* -20 */
    29154, 23254, 18705, 14949, 11916, /* -15 */
    9548,  7620,  6100,  4904,  3906,  /* -10 */
    3121,  2501,  1991,  1586,  1277,  /* -5 */
    1024,  820,   655,   526,   423,   /* 0 */
    335,   272,   215,   172,   137,   /* 5 */
    110,   87,    70,    56,    45,    /* 10 */
    36,    29,    23,    18,    15     /* 15 */
};

const uint32_t sched_prio_to_wmult[40] = {
    48388u,     59856u,     76040u,     92818u,     118348u,   /* -20 */
    147320u,    184698u,    229616u,    287308u,    360437u,   /* -15 */
    449829u,    563644u,    704093u,    875809u,    1099582u,  /* -10 */
    1376151u,   1717300u,   2157191u,   2708050u,   3363326u,  /* -5 */
    4194304u,   5237765u,   6557202u,   8165337u,   10153587u, /* 0 */
    12820798u,  15790321u,  19976592u,  24970740u,  31350126u, /* 5 */
    39045157u,  49367440u,  61356676u,  76695844u,  95443717u, /* 10 */
    119304647u, 148102320u, 186737708u, 238609294u, 286331153u /* 15 */
};

/* The arithmetic. */

uint64_t fair_mul_u64_u32_shr(uint64_t a, uint32_t mul, unsigned int shift)
{
    uint32_t ah = (uint32_t)(a >> 32), al = (uint32_t)a;
    uint64_t ret = ((uint64_t)al * mul) >> shift;

    /* shift is at most 32, so the high product moves left by 0 to 32 */
    if (ah) {
        ret += ((uint64_t)ah * mul) << (32 - shift);
    }
    return ret;
}

int64_t fair_div_s64(int64_t n, int64_t d)
{
    uint64_t un = n < 0 ? (uint64_t)0 - (uint64_t)n : (uint64_t)n;
    uint64_t ud = d < 0 ? (uint64_t)0 - (uint64_t)d : (uint64_t)d;
    uint64_t q = tick_div64(un, ud, NULL);

    return (n < 0) != (d < 0) ? (int64_t)((uint64_t)0 - q) : (int64_t)q;
}

/** The index of the highest set bit of `x`, from 1, or 0 for 0 (fls). */
static int fair_fls(uint32_t x)
{
    int r = 0;

    while (x) {
        x >>= 1;
        r++;
    }
    return r;
}

/** Compute `lw`'s inverse weight if it is not known (__update_inv_weight). */
static void update_inv_weight(struct load_weight *lw)
{
    uint64_t w;

    if (lw->inv_weight) {
        return;
    }
    w = lw->weight;
    if (w >= FAIR_WMULT_CONST) {
        lw->inv_weight = 1;
    } else if (!w) {
        lw->inv_weight = FAIR_WMULT_CONST;
    } else {
        lw->inv_weight = FAIR_WMULT_CONST / (uint32_t)w;
    }
}

/*
 * delta * weight / lw->weight, as (delta * (weight * lw->inv_weight)) >> 32,
 * with the factor's high bits shifted out first so that it fits 32 bits.
 * Either weight is NICE_0_LOAD and the inverse fits 32 bits, so the shift
 * stays at least 22, or weight is at most lw's, so the factor is at most
 * 2^32 and the shift stays positive.
 */
uint64_t fair_calc_delta(uint64_t delta, unsigned long weight, struct load_weight *lw)
{
    uint64_t fact = weight;
    uint32_t fact_hi = (uint32_t)(fact >> 32);
    int shift = FAIR_WMULT_SHIFT;
    int fs;

    update_inv_weight(lw);
    if (fact_hi) {
        fs = fair_fls(fact_hi);
        shift -= fs;
        fact >>= fs;
    }
    fact = (uint64_t)(uint32_t)fact * lw->inv_weight;
    fact_hi = (uint32_t)(fact >> 32);
    if (fact_hi) {
        fs = fair_fls(fact_hi);
        shift -= fs;
        fact >>= fs;
    }
    return fair_mul_u64_u32_shr(delta, (uint32_t)fact, (unsigned int)shift);
}

/** `delta` of real time as virtual time for `se`: delta * 1024 / weight. */
static uint64_t calc_delta_fair(uint64_t delta, struct sched_entity *se)
{
    if (se->load.weight != NICE_0_LOAD) {
        delta = fair_calc_delta(delta, NICE_0_LOAD, &se->load);
    }
    return delta;
}

/** Add `inc` to a weight, its inverse to be computed again. */
static void update_load_add(struct load_weight *lw, unsigned long inc)
{
    lw->weight += inc;
    lw->inv_weight = 0;
}

/** Take `dec` from a weight, its inverse to be computed again. */
static void update_load_sub(struct load_weight *lw, unsigned long dec)
{
    lw->weight -= dec;
    lw->inv_weight = 0;
}

/** Set a weight, its inverse to be computed again. */
static void update_load_set(struct load_weight *lw, unsigned long w)
{
    lw->weight = w;
    lw->inv_weight = 0;
}

/* The hierarchy. */

#define entity_is_task(se) ((se)->my_q == NULL)
#define rq_of(cfs_rq)      (&fair_rqs[(cfs_rq)->cpu])

struct process *fair_task_of(struct sched_entity *se)
{
    if (!entity_is_task(se)) {
        return NULL;
    }
    return (struct process *)(void *)((char *)se - offsetof(struct process, se));
}

/** Whether `p`'s policy is SCHED_IDLE (task_has_idle_policy). */
static int task_has_idle_policy(struct process *p)
{
    return p->sched_policy == SCHED_IDLE;
}

/** Whether `se` is a SCHED_IDLE thread or a SCHED_IDLE group's entity. */
static int se_is_idle(struct sched_entity *se)
{
    if (entity_is_task(se)) {
        return task_has_idle_policy(fair_task_of(se));
    }
    return se->my_q->idle > 0;
}

/** The queue both `*se` and `*pse` are on, walking each up to it
 * (find_matching_se). */
static void find_matching_se(struct sched_entity **se, struct sched_entity **pse)
{
    int se_depth = (*se)->depth, pse_depth = (*pse)->depth;

    while (se_depth > pse_depth) {
        se_depth--;
        *se = (*se)->parent;
    }
    while (pse_depth > se_depth) {
        pse_depth--;
        *pse = (*pse)->parent;
    }
    while ((*se)->cfs_rq != (*pse)->cfs_rq) {
        *se = (*se)->parent;
        *pse = (*pse)->parent;
    }
}

/** Ask processor `rq`'s current thread to select again (resched_curr). */
static void resched_curr(struct fair_rq *rq)
{
    rq->resched = 1;
}

/* The hooks of the files that build on this one. */

/** PELT's update of `se` on `cfs_rq` (update_load_avg), when pelt.c is in. */
static void update_load_avg(struct cfs_rq *cfs_rq, struct sched_entity *se, int flags)
{
    if (fair_hooks.load_update) {
        fair_hooks.load_update(cfs_rq, se, flags);
    }
}

/** A group entity's count of runnable threads below (se_update_runnable). */
static void se_update_runnable(struct sched_entity *se)
{
    if (!entity_is_task(se)) {
        se->runnable_weight = se->my_q->h_nr_queued - se->my_q->h_nr_delayed;
    }
}

/** A group entity's weight from its share (update_cfs_group), when
 * fair_group.c is in; nothing for a thread. */
static void update_cfs_group(struct sched_entity *se)
{
    if (!entity_is_task(se) && fair_hooks.group_update) {
        fair_hooks.group_update(se);
    }
}

/* The leaf list, child queues before their parents. */

/** The queue above `cfs_rq` on its processor, NULL for a root queue. */
static struct cfs_rq *parent_cfs_rq(struct cfs_rq *cfs_rq)
{
    struct task_group *tg = cfs_rq->tg;

    if (!tg || !tg->parent) {
        return NULL;
    }
    return tg->parent->cfs_rq[cfs_rq->cpu];
}

void fair_leaf_add(struct cfs_rq *cfs_rq)
{
    struct fair_rq *rq = rq_of(cfs_rq);
    struct cfs_rq *parent, *tail;

    if (cfs_rq->on_list) {
        return;
    }
    parent = parent_cfs_rq(cfs_rq);
    if (parent) {
        fair_leaf_add(parent);
        cfs_rq->leaf_next = parent;
        cfs_rq->leaf_prev = parent->leaf_prev;
        if (parent->leaf_prev) {
            parent->leaf_prev->leaf_next = cfs_rq;
        } else {
            rq->leaf_head = cfs_rq;
        }
        parent->leaf_prev = cfs_rq;
    } else {
        tail = rq->leaf_head;
        while (tail && tail->leaf_next) {
            tail = tail->leaf_next;
        }
        cfs_rq->leaf_prev = tail;
        cfs_rq->leaf_next = NULL;
        if (tail) {
            tail->leaf_next = cfs_rq;
        } else {
            rq->leaf_head = cfs_rq;
        }
    }
    cfs_rq->on_list = 1;
}

void fair_leaf_del(struct cfs_rq *cfs_rq)
{
    struct fair_rq *rq = rq_of(cfs_rq);

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

/* The processor's list of queued threads. */

/** Put a thread at the head of its processor's `cfs_tasks`. */
static void cfs_tasks_add(struct fair_rq *rq, struct sched_entity *se)
{
    se->tasks_prev = NULL;
    se->tasks_next = rq->cfs_tasks;
    if (rq->cfs_tasks) {
        rq->cfs_tasks->tasks_prev = se;
    }
    rq->cfs_tasks = se;
}

/** Take a thread off its processor's `cfs_tasks`. */
static void cfs_tasks_del(struct fair_rq *rq, struct sched_entity *se)
{
    if (se->tasks_prev) {
        se->tasks_prev->tasks_next = se->tasks_next;
    } else if (rq->cfs_tasks == se) {
        rq->cfs_tasks = se->tasks_next;
    }
    if (se->tasks_next) {
        se->tasks_next->tasks_prev = se->tasks_prev;
    }
    se->tasks_prev = se->tasks_next = NULL;
}

/* The tree and virtual time. */

/** Whether `a`'s deadline is before `b`'s (entity_before). */
static int entity_before(const struct sched_entity *a, const struct sched_entity *b)
{
    return (int64_t)(a->deadline - b->deadline) < 0;
}

/** `se`'s virtual runtime relative to its queue's `zero_vruntime`. */
static int64_t entity_key(struct cfs_rq *cfs_rq, struct sched_entity *se)
{
    return (int64_t)(se->vruntime - cfs_rq->zero_vruntime);
}

#define node_to_se(node) rb_entry((node), struct sched_entity, run_node)

/** Add an entity entering the tree to its queue's weighted sum. */
static void avg_vruntime_add(struct cfs_rq *cfs_rq, struct sched_entity *se)
{
    unsigned long weight = se->load.weight;

    cfs_rq->avg_vruntime += entity_key(cfs_rq, se) * (int64_t)weight;
    cfs_rq->avg_load += weight;
}

/** Take an entity leaving the tree from its queue's weighted sum. */
static void avg_vruntime_sub(struct cfs_rq *cfs_rq, struct sched_entity *se)
{
    unsigned long weight = se->load.weight;

    cfs_rq->avg_vruntime -= entity_key(cfs_rq, se) * (int64_t)weight;
    cfs_rq->avg_load -= weight;
}

/** Move `zero_vruntime` by `delta`, keeping the sum relative to it. */
static void update_zero_vruntime(struct cfs_rq *cfs_rq, int64_t delta)
{
    cfs_rq->avg_vruntime -= (int64_t)cfs_rq->avg_load * delta;
    cfs_rq->zero_vruntime += (uint64_t)delta;
}

/*
 * V, the weighted average of the virtual runtimes of the queue's entities,
 * the current one included:
 *
 *     V = \Sum (v_i - v0) * w_i / \Sum w_i + v0
 *
 * with v0 `zero_vruntime`, which then moves to V. The division floors, so
 * that V + 0 is eligible for any entity at V.
 */
uint64_t fair_avg_vruntime(struct cfs_rq *cfs_rq)
{
    struct sched_entity *curr = cfs_rq->curr;
    int64_t weight = (int64_t)cfs_rq->avg_load;
    int64_t delta = 0;

    if (curr && !curr->on_rq) {
        curr = NULL;
    }
    if (weight) {
        int64_t runtime = cfs_rq->avg_vruntime;

        if (curr) {
            unsigned long w = curr->load.weight;

            runtime += entity_key(cfs_rq, curr) * (int64_t)w;
            weight += (int64_t)w;
        }
        /* rounding toward zero floors only for a positive sum */
        if (runtime < 0) {
            runtime -= weight - 1;
        }
        delta = fair_div_s64(runtime, weight);
    } else if (curr) {
        delta = (int64_t)(curr->vruntime - cfs_rq->zero_vruntime);
    }
    update_zero_vruntime(cfs_rq, delta);
    return cfs_rq->zero_vruntime;
}

/*
 * The virtual lag V - v of `se`, clamped to the virtual time of twice its
 * slice or a tick, whichever is longer, since adding, removing and
 * reweighting entities moves V and could grow a lag past what EEVDF bounds
 * it to.
 */
static int64_t entity_lag(uint64_t avruntime, struct sched_entity *se)
{
    int64_t vlag, limit;
    uint64_t span = 2 * se->slice > FAIR_TICK_NS ? 2 * se->slice : FAIR_TICK_NS;

    vlag = (int64_t)(avruntime - se->vruntime);
    limit = (int64_t)calc_delta_fair(span, se);
    if (vlag < -limit) {
        return -limit;
    }
    if (vlag > limit) {
        return limit;
    }
    return vlag;
}

/** Save `se`'s clamped virtual lag before its dequeue. */
static void update_entity_lag(struct cfs_rq *cfs_rq, struct sched_entity *se)
{
    se->vlag = entity_lag(fair_avg_vruntime(cfs_rq), se);
}

/*
 * Whether an entity of virtual runtime `vruntime` is owed service, lag >= 0,
 * that is V >= vruntime, compared without V's division:
 *
 *     \Sum (v_i - v0) * w_i >= (vruntime - v0) * \Sum w_i
 */
static int vruntime_eligible(struct cfs_rq *cfs_rq, uint64_t vruntime)
{
    struct sched_entity *curr = cfs_rq->curr;
    int64_t avg = cfs_rq->avg_vruntime;
    int64_t load = (int64_t)cfs_rq->avg_load;

    if (curr && curr->on_rq) {
        unsigned long weight = curr->load.weight;

        avg += entity_key(cfs_rq, curr) * (int64_t)weight;
        load += (int64_t)weight;
    }
    return avg >= (int64_t)(vruntime - cfs_rq->zero_vruntime) * load;
}

int fair_entity_eligible(struct cfs_rq *cfs_rq, struct sched_entity *se)
{
    return vruntime_eligible(cfs_rq, se->vruntime);
}

/** The root entity of the queue's tree, NULL for an empty one. */
static struct sched_entity *pick_root_entity(struct cfs_rq *cfs_rq)
{
    struct rb_node *root = cfs_rq->tasks_timeline.rb_root.rb_node;

    return root ? node_to_se(root) : NULL;
}

/** The entity of the earliest deadline in the tree, NULL for an empty one. */
static struct sched_entity *pick_first_entity(struct cfs_rq *cfs_rq)
{
    struct rb_node *left = cfs_rq->tasks_timeline.rb_leftmost;

    return left ? node_to_se(left) : NULL;
}

/** The least slice on the queue, the current entity's included, or ~0. */
static uint64_t cfs_rq_min_slice(struct cfs_rq *cfs_rq)
{
    struct sched_entity *root = pick_root_entity(cfs_rq);
    struct sched_entity *curr = cfs_rq->curr;
    uint64_t min_slice = ~(uint64_t)0;

    if (curr && curr->on_rq) {
        min_slice = curr->slice;
    }
    if (root && root->min_slice < min_slice) {
        min_slice = root->min_slice;
    }
    return min_slice;
}

/** The tree's order: by virtual deadline. */
static int entity_less(const struct rb_node *a, const struct rb_node *b)
{
    return entity_before(node_to_se(a), node_to_se(b));
}

/** Recompute `se`'s subtree minimums from its own values and its
 * children's. Returns whether neither changed, which ends a propagation. */
static int min_vruntime_update(struct sched_entity *se)
{
    uint64_t old_min_vruntime = se->min_vruntime;
    uint64_t old_min_slice = se->min_slice;
    struct rb_node *node[2];
    struct sched_entity *rse;
    int i;

    node[0] = se->run_node.rb_right;
    node[1] = se->run_node.rb_left;
    se->min_vruntime = se->vruntime;
    se->min_slice = se->slice;
    for (i = 0; i < 2; i++) {
        if (!node[i]) {
            continue;
        }
        rse = node_to_se(node[i]);
        if ((int64_t)(se->min_vruntime - rse->min_vruntime) > 0) {
            se->min_vruntime = rse->min_vruntime;
        }
        if (rse->min_slice < se->min_slice) {
            se->min_slice = rse->min_slice;
        }
    }
    return se->min_vruntime == old_min_vruntime && se->min_slice == old_min_slice;
}

/** Recompute the subtree minimums from `rb` up to `stop`, stopping
 * where one does not change. */
static void min_vruntime_cb_propagate(struct rb_node *rb, struct rb_node *stop)
{
    while (rb != stop) {
        struct sched_entity *se = node_to_se(rb);

        if (min_vruntime_update(se)) {
            break;
        }
        rb = rb_parent(&se->run_node);
    }
}

/** Give the node taking `rb_old`'s place its subtree minimums. */
static void min_vruntime_cb_copy(struct rb_node *rb_old, struct rb_node *rb_new)
{
    struct sched_entity *old = node_to_se(rb_old);
    struct sched_entity *new_ = node_to_se(rb_new);

    new_->min_vruntime = old->min_vruntime;
    new_->min_slice = old->min_slice;
}

/** After a rotation, give the subtree's new root the old root's
 * minimums and recompute the old root's. */
static void min_vruntime_cb_rotate(struct rb_node *rb_old, struct rb_node *rb_new)
{
    min_vruntime_cb_copy(rb_old, rb_new);
    min_vruntime_update(node_to_se(rb_old));
}

static const struct rb_augment_callbacks min_vruntime_cb = {
    min_vruntime_cb_propagate, min_vruntime_cb_copy, min_vruntime_cb_rotate};

/** Put `se` in its queue's tree and weighted sum (__enqueue_entity). */
static void enqueue_tree(struct cfs_rq *cfs_rq, struct sched_entity *se)
{
    avg_vruntime_add(cfs_rq, se);
    se->min_vruntime = se->vruntime;
    se->min_slice = se->slice;
    rb_add_augmented_cached(&se->run_node, &cfs_rq->tasks_timeline, entity_less, &min_vruntime_cb);
}

/** Take `se` out of its queue's tree and weighted sum (__dequeue_entity). */
static void dequeue_tree(struct cfs_rq *cfs_rq, struct sched_entity *se)
{
    rb_erase_augmented_cached(&se->run_node, &cfs_rq->tasks_timeline, &min_vruntime_cb);
    RB_CLEAR_NODE(&se->run_node);
    avg_vruntime_sub(cfs_rq, se);
}

/*
 * RUN_TO_PARITY's protection: the deadline at the pick, kept in `vlag`,
 * which no one reads until the dequeue. The current entity runs on while
 * it is eligible and its deadline is the one it was picked with.
 */
static void set_protect_slice(struct sched_entity *se)
{
    se->vlag = (int64_t)se->deadline;
}

/** Whether the current entity still runs on the deadline it was picked
 * with. */
static int protect_slice(struct sched_entity *se)
{
    return se->vlag == (int64_t)se->deadline;
}

/** End the current entity's protection. */
static void cancel_protect_slice(struct sched_entity *se)
{
    if (protect_slice(se)) {
        se->vlag = (int64_t)(se->deadline + 1);
    }
}

/*
 * Earliest Eligible Virtual Deadline First: among the entities owed
 * service, the one of the earliest virtual deadline. The tree is ordered by
 * deadline, and each node keeps its subtree's least virtual runtime, so a
 * subtree with no eligible entity is skipped and the search takes
 * O(log n).
 */
static struct sched_entity *pick_eevdf(struct cfs_rq *cfs_rq)
{
    struct rb_node *node = cfs_rq->tasks_timeline.rb_root.rb_node;
    struct sched_entity *se = pick_first_entity(cfs_rq);
    struct sched_entity *curr = cfs_rq->curr;
    struct sched_entity *best = NULL;

    if (cfs_rq->nr_running == 1) {
        return curr && curr->on_rq ? curr : se;
    }
    if (curr && (!curr->on_rq || !fair_entity_eligible(cfs_rq, curr))) {
        curr = NULL;
    }
    if (sched_feat(RUN_TO_PARITY) && curr && protect_slice(curr)) {
        return curr;
    }
    if (se && fair_entity_eligible(cfs_rq, se)) {
        best = se;
    } else {
        while (node) {
            struct rb_node *left = node->rb_left;

            /* an eligible entity on the left has an earlier deadline */
            if (left && vruntime_eligible(cfs_rq, node_to_se(left)->min_vruntime)) {
                node = left;
                continue;
            }
            se = node_to_se(node);
            if (fair_entity_eligible(cfs_rq, se)) {
                best = se;
                break;
            }
            node = node->rb_right;
        }
    }
    if (!best || (curr && entity_before(curr, best))) {
        best = curr;
    }
    /* V is the weighted average, so some entity is eligible; a rounding
     * that leaves none takes the earliest deadline rather than none */
    if (!best) {
        best = pick_first_entity(cfs_rq);
    }
    return best;
}

static void clear_buddies(struct cfs_rq *cfs_rq, struct sched_entity *se);

/**
 * Give `se` a new deadline once it has run its request, vd = ve + r / w, the
 * request being the base slice unless a custom one is set (update_deadline).
 * Returns whether it did, so the processor should select again.
 */
static int update_deadline(struct cfs_rq *cfs_rq, struct sched_entity *se)
{
    if ((int64_t)(se->vruntime - se->deadline) < 0) {
        return 0;
    }
    if (!se->custom_slice) {
        se->slice = sysctl_sched_base_slice;
    }
    se->deadline = se->vruntime + calc_delta_fair(se->slice, se);
    fair_avg_vruntime(cfs_rq);
    return 1;
}

/* Charging. */

/** Charge the current entity the task clock since `exec_start`. Returns the
 * charged time, or a value not above 0 for none. */
static int64_t update_curr_se(struct fair_rq *rq, struct sched_entity *curr)
{
    int64_t delta_exec = (int64_t)(rq->clock_task - curr->exec_start);

    if (delta_exec <= 0) {
        return delta_exec;
    }
    curr->exec_start = rq->clock_task;
    curr->sum_exec_runtime += (uint64_t)delta_exec;
    if (fair_schedstats && delta_exec > curr->stats.exec_max) {
        curr->stats.exec_max = delta_exec;
    }
    return delta_exec;
}

/** Whether the current entity lost its eligibility while unprotected, so a
 * shorter-slice entity's wake should take over (PREEMPT_SHORT). */
static int did_preempt_short(struct cfs_rq *cfs_rq, struct sched_entity *curr)
{
    if (!sched_feat(PREEMPT_SHORT)) {
        return 0;
    }
    if (protect_slice(curr)) {
        return 0;
    }
    return !fair_entity_eligible(cfs_rq, curr);
}

/** Whether `pse`, of a shorter slice than `se` and eligible, should end
 * `se`'s protection. */
static int do_preempt_short(struct cfs_rq *cfs_rq, struct sched_entity *pse,
                            struct sched_entity *se)
{
    if (!sched_feat(PREEMPT_SHORT)) {
        return 0;
    }
    if (pse->slice >= se->slice) {
        return 0;
    }
    if (!fair_entity_eligible(cfs_rq, pse)) {
        return 0;
    }
    if (entity_before(pse, se)) {
        return 1;
    }
    if (!fair_entity_eligible(cfs_rq, se)) {
        return 1;
    }
    return 0;
}

/** Charge the queue's current entity its run time, weighted, and give it a
 * new deadline when it reaches its own. */
static void update_curr(struct cfs_rq *cfs_rq)
{
    struct sched_entity *curr = cfs_rq->curr;
    struct fair_rq *rq = rq_of(cfs_rq);
    int64_t delta_exec;
    int resched;

    if (!curr) {
        return;
    }
    delta_exec = update_curr_se(rq, curr);
    if (delta_exec <= 0) {
        return;
    }
    curr->vruntime += calc_delta_fair((uint64_t)delta_exec, curr);
    resched = update_deadline(cfs_rq, curr);
    if (entity_is_task(curr)) {
        rq->charged_ns += (uint64_t)delta_exec;
    }
    if (cfs_rq->nr_running == 1) {
        return;
    }
    if (resched || did_preempt_short(cfs_rq, curr)) {
        resched_curr(rq);
        clear_buddies(cfs_rq, curr);
    }
}

/* The schedstats, on the monotonic clock. */

/** Start counting `se`'s wait on its queue. */
static void update_stats_wait_start(struct cfs_rq *cfs_rq, struct sched_entity *se)
{
    uint64_t wait_start = rq_of(cfs_rq)->clock;
    uint64_t prev = se->stats.wait_start;

    if (!fair_schedstats) {
        return;
    }
    /* a migrating thread brought its wait so far in `wait_start` */
    if (entity_is_task(se) && wait_start > prev) {
        wait_start -= prev;
    }
    se->stats.wait_start = wait_start;
}

/** End `se`'s wait, adding it to the sums; a migrating thread instead
 * keeps the wait so far in `wait_start` for its next queue. */
static void update_stats_wait_end(struct cfs_rq *cfs_rq, struct sched_entity *se, int migrating)
{
    uint64_t delta;

    if (!fair_schedstats || !se->stats.wait_start) {
        return;
    }
    delta = rq_of(cfs_rq)->clock - se->stats.wait_start;
    if (entity_is_task(se) && migrating) {
        se->stats.wait_start = delta;
        return;
    }
    if (delta > se->stats.wait_max) {
        se->stats.wait_max = delta;
    }
    se->stats.wait_count++;
    se->stats.wait_sum += delta;
    se->stats.wait_start = 0;
}

/** End a woken thread's sleep or block, adding it to the sums. */
static void update_stats_enqueue_sleeper(struct cfs_rq *cfs_rq, struct sched_entity *se)
{
    uint64_t now = rq_of(cfs_rq)->clock;
    uint64_t delta;

    if (se->stats.sleep_start) {
        delta = now - se->stats.sleep_start;
        if ((int64_t)delta < 0) {
            delta = 0;
        }
        if (delta > se->stats.sleep_max) {
            se->stats.sleep_max = delta;
        }
        se->stats.sleep_start = 0;
        se->stats.sum_sleep_runtime += (int64_t)delta;
    }
    if (se->stats.block_start) {
        delta = now - se->stats.block_start;
        if ((int64_t)delta < 0) {
            delta = 0;
        }
        if (delta > se->stats.block_max) {
            se->stats.block_max = delta;
        }
        se->stats.block_start = 0;
        se->stats.sum_sleep_runtime += (int64_t)delta;
        se->stats.sum_block_runtime += (int64_t)delta;
    }
}

/** The schedstats of an enqueue. */
static void update_stats_enqueue(struct cfs_rq *cfs_rq, struct sched_entity *se, int flags)
{
    if (!fair_schedstats) {
        return;
    }
    if (se != cfs_rq->curr) {
        update_stats_wait_start(cfs_rq, se);
    }
    if (flags & ENQUEUE_WAKEUP) {
        update_stats_enqueue_sleeper(cfs_rq, se);
    }
}

/** The schedstats of a dequeue: the wait ends, and a sleep or block
 * starts. */
static void update_stats_dequeue(struct cfs_rq *cfs_rq, struct sched_entity *se, int flags)
{
    struct process *p;

    if (!fair_schedstats) {
        return;
    }
    if (se != cfs_rq->curr) {
        update_stats_wait_end(cfs_rq, se, (flags & DEQUEUE_MIGRATE) != 0);
    }
    if ((flags & DEQUEUE_SLEEP) && entity_is_task(se)) {
        p = fair_task_of(se);
        if (p->uninterruptible) {
            se->stats.block_start = rq_of(cfs_rq)->clock;
        } else {
            se->stats.sleep_start = rq_of(cfs_rq)->clock;
        }
    }
}

/* Enqueue and dequeue of one entity. */

/** Count an entity on its queue: weight, entities and threads. */
static void account_entity_enqueue(struct cfs_rq *cfs_rq, struct sched_entity *se)
{
    update_load_add(&cfs_rq->load, se->load.weight);
    if (entity_is_task(se)) {
        cfs_tasks_add(rq_of(cfs_rq), se);
    }
    cfs_rq->nr_running++;
    if (se_is_idle(se)) {
        cfs_rq->idle_nr_running++;
    }
}

/** Count an entity off its queue. */
static void account_entity_dequeue(struct cfs_rq *cfs_rq, struct sched_entity *se)
{
    update_load_sub(&cfs_rq->load, se->load.weight);
    if (entity_is_task(se)) {
        cfs_tasks_del(rq_of(cfs_rq), se);
    }
    cfs_rq->nr_running--;
    if (se_is_idle(se)) {
        cfs_rq->idle_nr_running--;
    }
}

/**
 * Place `se` on `cfs_rq` at V less its lag, with a deadline a slice of
 * virtual time on (place_entity).
 *
 * Adding an entity of virtual lag vl moves V to V - w * vl / (W + w), which
 * would leave it only vl - w * vl / (W + w) of lag; so with PLACE_LAG the
 * lag is first inflated to (W + w) * vl / W, which leaves exactly vl after
 * placement. With PLACE_REL_DEADLINE, a deadline kept relative across a
 * dequeue that was not a sleep is restored as it was; with
 * PLACE_DEADLINE_INITIAL, a new thread (ENQUEUE_INITIAL in `flags`) starts
 * with half a slice, as the others are on average halfway through theirs.
 */
static void place_entity(struct cfs_rq *cfs_rq, struct sched_entity *se, int flags)
{
    uint64_t vslice, vruntime = fair_avg_vruntime(cfs_rq);
    int64_t lag = 0;

    if (!se->custom_slice) {
        se->slice = sysctl_sched_base_slice;
    }
    vslice = calc_delta_fair(se->slice, se);
    if (sched_feat(PLACE_LAG) && cfs_rq->nr_running) {
        struct sched_entity *curr = cfs_rq->curr;
        int64_t load = (int64_t)cfs_rq->avg_load;

        lag = se->vlag;
        if (curr && curr->on_rq) {
            load += (int64_t)curr->load.weight;
        }
        lag *= load + (int64_t)se->load.weight;
        if (!load) {
            load = 1;
        }
        lag = fair_div_s64(lag, load);
    }
    se->vruntime = vruntime - (uint64_t)lag;
    if (sched_feat(PLACE_REL_DEADLINE) && se->rel_deadline) {
        se->deadline += se->vruntime;
        se->rel_deadline = 0;
        return;
    }
    if (sched_feat(PLACE_DEADLINE_INITIAL) && (flags & ENQUEUE_INITIAL)) {
        vslice /= 2;
    }
    se->deadline = se->vruntime + vslice;
}

/** Queue one entity on `cfs_rq`, placed by its lag (enqueue_entity). */
static void enqueue_entity(struct cfs_rq *cfs_rq, struct sched_entity *se, int flags)
{
    int curr = cfs_rq->curr == se;

    /* the current entity is placed before its charge, which reads V */
    if (curr) {
        place_entity(cfs_rq, se, flags);
    }
    update_curr(cfs_rq);
    update_load_avg(cfs_rq, se, FAIR_LOAD_UPDATE_TG | FAIR_LOAD_ATTACH);
    se_update_runnable(se);
    update_cfs_group(se);
    if (!curr) {
        place_entity(cfs_rq, se, flags);
    }
    account_entity_enqueue(cfs_rq, se);
    if (flags & ENQUEUE_MIGRATED) {
        se->exec_start = 0;
    }
    update_stats_enqueue(cfs_rq, se, flags);
    if (!curr) {
        enqueue_tree(cfs_rq, se);
    }
    se->on_rq = 1;
    if (cfs_rq->nr_running == 1) {
        fair_leaf_add(cfs_rq);
    }
}

/** End the buddy nominations of `se` and its ancestors. */
static void clear_buddies_next(struct sched_entity *se)
{
    for (; se; se = se->parent) {
        struct cfs_rq *cfs_rq = se->cfs_rq;

        if (cfs_rq->next != se) {
            break;
        }
        cfs_rq->next = NULL;
    }
}

/** End `se`'s buddy nomination, if it holds one. */
static void clear_buddies(struct cfs_rq *cfs_rq, struct sched_entity *se)
{
    if (cfs_rq->next == se) {
        clear_buddies_next(se);
    }
}

/** Nominate `se` and its ancestors as their queues' buddies, up to an
 * idle one (set_next_buddy). */
static void set_next_buddy(struct sched_entity *se)
{
    for (; se; se = se->parent) {
        if (!se->on_rq || se_is_idle(se)) {
            return;
        }
        se->cfs_rq->next = se;
    }
}

/** Mark a thread's entity delayed, moving it from the runnable counts to
 * the delayed ones up the hierarchy; a group entity's counts follow its
 * threads'. */
static void set_delayed(struct sched_entity *se)
{
    se->sched_delayed = 1;
    if (!entity_is_task(se)) {
        return;
    }
    for (; se; se = se->parent) {
        struct cfs_rq *cfs_rq = se->cfs_rq;

        cfs_rq->h_nr_runnable--;
        cfs_rq->h_nr_delayed++;
    }
}

/** Clear a thread's delay, moving it back to the runnable counts. */
static void clear_delayed(struct sched_entity *se)
{
    se->sched_delayed = 0;
    if (!entity_is_task(se)) {
        return;
    }
    for (; se; se = se->parent) {
        struct cfs_rq *cfs_rq = se->cfs_rq;

        cfs_rq->h_nr_runnable++;
        cfs_rq->h_nr_delayed--;
    }
}

/** Clear the delay of an entity whose dequeue finished, dropping a positive
 * lag it would otherwise keep (DELAY_ZERO). */
static void finish_delayed_dequeue_entity(struct sched_entity *se)
{
    clear_delayed(se);
    if (sched_feat(DELAY_ZERO) && se->vlag > 0) {
        se->vlag = 0;
    }
}

/*
 * Take `se` off `cfs_rq`. A sleep of an entity that is not eligible is
 * delayed instead: it stays queued, competing, until it is picked while
 * eligible or woken, so that its negative lag is repaid in place. Returns
 * whether the entity left the queue.
 */
static int dequeue_entity(struct cfs_rq *cfs_rq, struct sched_entity *se, int flags)
{
    int sleep = (flags & DEQUEUE_SLEEP) != 0;
    int action = FAIR_LOAD_UPDATE_TG;

    update_curr(cfs_rq);
    clear_buddies(cfs_rq, se);
    if (!(flags & DEQUEUE_DELAYED)) {
        /* a delay relies on spurious wakes, which a special state, a stop
         * or an exit, must not have */
        int delay = sleep && !(flags & DEQUEUE_SPECIAL);

        if (sched_feat(DELAY_DEQUEUE) && delay && !fair_entity_eligible(cfs_rq, se)) {
            update_load_avg(cfs_rq, se, 0);
            set_delayed(se);
            return 0;
        }
    }
    if (entity_is_task(se) && (flags & DEQUEUE_MIGRATE)) {
        action |= FAIR_LOAD_DETACH;
    }
    update_load_avg(cfs_rq, se, action);
    se_update_runnable(se);
    update_stats_dequeue(cfs_rq, se, flags);
    update_entity_lag(cfs_rq, se);
    if (sched_feat(PLACE_REL_DEADLINE) && !sleep) {
        se->deadline -= se->vruntime;
        se->rel_deadline = 1;
    }
    if (se != cfs_rq->curr) {
        dequeue_tree(cfs_rq, se);
    }
    se->on_rq = 0;
    account_entity_dequeue(cfs_rq, se);
    update_cfs_group(se);
    if (flags & DEQUEUE_DELAYED) {
        finish_delayed_dequeue_entity(se);
    }
    return 1;
}

/** Make `se` its queue's current entity, out of the tree, and start its
 * slice's protection. */
static void set_next_entity(struct cfs_rq *cfs_rq, struct sched_entity *se)
{
    struct fair_rq *rq = rq_of(cfs_rq);

    clear_buddies(cfs_rq, se);
    if (se->on_rq) {
        update_stats_wait_end(cfs_rq, se, 0);
        dequeue_tree(cfs_rq, se);
        update_load_avg(cfs_rq, se, FAIR_LOAD_UPDATE_TG);
        set_protect_slice(se);
    }
    se->exec_start = rq->clock_task;
    cfs_rq->curr = se;
    /* the longest slice, counted only while the processor's load is at
     * least twice this entity's weight */
    if (fair_schedstats && rq->cfs.load.weight >= 2 * se->load.weight) {
        uint64_t ran = se->sum_exec_runtime - se->prev_sum_exec_runtime;

        if (ran > se->stats.slice_max) {
            se->stats.slice_max = ran;
        }
    }
    se->prev_sum_exec_runtime = se->sum_exec_runtime;
}

/** Stop `prev` being its queue's current entity, putting it back in the
 * tree if still queued. */
static void put_prev_entity(struct cfs_rq *cfs_rq, struct sched_entity *prev)
{
    if (prev->on_rq) {
        update_curr(cfs_rq);
        update_stats_wait_start(cfs_rq, prev);
        enqueue_tree(cfs_rq, prev);
        update_load_avg(cfs_rq, prev, 0);
    }
    cfs_rq->curr = NULL;
}

/** The tick of one current entity: charge it and update its load. */
static void entity_tick(struct cfs_rq *cfs_rq, struct sched_entity *curr)
{
    update_curr(cfs_rq);
    update_load_avg(cfs_rq, curr, FAIR_LOAD_UPDATE_TG);
    update_cfs_group(curr);
}

/* Weights. */

/*
 * The new virtual runtime and deadline of a queued entity reweighted from
 * `se->load.weight` to `weight` at V `avruntime`. Lag w * (V - v) is kept,
 * which leaves V unchanged, so v' = V - (V - v) * w / w'; the deadline
 * keeps its distance in real time, d' = V + (d - V) * w / w'.
 */
static void reweight_eevdf(struct sched_entity *se, uint64_t avruntime, unsigned long weight)
{
    unsigned long old_weight = se->load.weight;
    int64_t vlag, vslice;

    if (avruntime != se->vruntime) {
        vlag = entity_lag(avruntime, se);
        vlag = fair_div_s64(vlag * (int64_t)old_weight, (int64_t)weight);
        se->vruntime = avruntime - (uint64_t)vlag;
    }
    vslice = (int64_t)(se->deadline - avruntime);
    vslice = fair_div_s64(vslice * (int64_t)old_weight, (int64_t)weight);
    se->deadline = avruntime + (uint64_t)vslice;
}

void fair_reweight_entity(struct cfs_rq *cfs_rq, struct sched_entity *se, unsigned long weight)
{
    int curr = cfs_rq && cfs_rq->curr == se;
    unsigned long old_weight = se->load.weight;
    uint64_t avruntime = 0;

    if (se->on_rq) {
        update_curr(cfs_rq);
        avruntime = fair_avg_vruntime(cfs_rq);
        if (!curr) {
            dequeue_tree(cfs_rq, se);
        }
        update_load_sub(&cfs_rq->load, se->load.weight);
    }
    if (se->on_rq) {
        reweight_eevdf(se, avruntime, weight);
    } else {
        /* `vlag` is V - v while the lag is w * (V - v), so it scales */
        se->vlag = fair_div_s64(se->vlag * (int64_t)se->load.weight, (int64_t)weight);
    }
    update_load_set(&se->load, weight);
    if (cfs_rq && fair_hooks.load_reweight) {
        fair_hooks.load_reweight(cfs_rq, se, old_weight);
    }
    if (se->on_rq) {
        update_load_add(&cfs_rq->load, se->load.weight);
        if (!curr) {
            enqueue_tree(cfs_rq, se);
        }
    }
}

/** The weight and inverse of `p`'s policy and nice (set_load_weight). */
static void task_load_weight(struct process *p, struct load_weight *lw)
{
    int prio = p->nice + 20;

    if (task_has_idle_policy(p)) {
        lw->weight = WEIGHT_IDLEPRIO;
        lw->inv_weight = WMULT_IDLEPRIO;
        return;
    }
    if (prio < 0) {
        prio = 0;
    } else if (prio > 39) {
        prio = 39;
    }
    lw->weight = sched_prio_to_weight[prio];
    lw->inv_weight = sched_prio_to_wmult[prio];
}

/* Threads through the hierarchy. */

/** Finish the delayed dequeue of `se`'s levels, or dequeue them, counting a
 * thread off its queues; for DEQUEUE_DELAYED of a thread, `notify` calls
 * `fair_hooks.blocked`. Returns -1 when the thread's own dequeue was
 * delayed, else 1. */
static int dequeue_entities(struct sched_entity *se, int flags, int notify)
{
    int task_sleep = (flags & DEQUEUE_SLEEP) != 0;
    int task_delayed = (flags & DEQUEUE_DELAYED) != 0;
    struct process *p = NULL;
    unsigned int idle_h_nr_running = 0, h_nr_queued = 0, h_nr_delayed = 0;
    struct cfs_rq *cfs_rq;
    uint64_t slice = 0;

    if (entity_is_task(se)) {
        p = fair_task_of(se);
        h_nr_queued = 1;
        idle_h_nr_running = (unsigned int)task_has_idle_policy(p);
        if (!task_sleep && !task_delayed) {
            h_nr_delayed = se->sched_delayed ? 1u : 0u;
        }
    }
    for (; se; se = se->parent) {
        cfs_rq = se->cfs_rq;
        if (!dequeue_entity(cfs_rq, se, flags)) {
            if (p && &p->se == se) {
                return -1;
            }
            slice = cfs_rq_min_slice(cfs_rq);
            break;
        }
        if (!h_nr_delayed) {
            cfs_rq->h_nr_runnable -= h_nr_queued;
        }
        cfs_rq->h_nr_queued -= h_nr_queued;
        cfs_rq->idle_h_nr_running -= idle_h_nr_running;
        cfs_rq->h_nr_delayed -= h_nr_delayed;
        if (cfs_rq->idle > 0) {
            idle_h_nr_running = h_nr_queued;
        }
        /* a parent with other entities stays queued */
        if (cfs_rq->load.weight) {
            slice = cfs_rq_min_slice(cfs_rq);
            se = se->parent;
            /* bias the pick to this group, as p sleeps within its slice */
            if (task_sleep && se) {
                set_next_buddy(se);
            }
            break;
        }
        flags |= DEQUEUE_SLEEP;
        flags &= ~(DEQUEUE_DELAYED | DEQUEUE_SPECIAL);
    }
    for (; se; se = se->parent) {
        cfs_rq = se->cfs_rq;
        update_load_avg(cfs_rq, se, FAIR_LOAD_UPDATE_TG);
        se_update_runnable(se);
        update_cfs_group(se);
        se->slice = slice;
        if (se != cfs_rq->curr) {
            min_vruntime_cb_propagate(&se->run_node, NULL);
        }
        slice = cfs_rq_min_slice(cfs_rq);
        if (!h_nr_delayed) {
            cfs_rq->h_nr_runnable -= h_nr_queued;
        }
        cfs_rq->h_nr_queued -= h_nr_queued;
        cfs_rq->idle_h_nr_running -= idle_h_nr_running;
        cfs_rq->h_nr_delayed -= h_nr_delayed;
        if (cfs_rq->idle > 0) {
            idle_h_nr_running = h_nr_queued;
        }
    }
    if (p && task_delayed && notify && fair_hooks.blocked) {
        fair_hooks.blocked(p);
    }
    return 1;
}

/*
 * Requeue a delayed entity that woke: it competes on from where it was,
 * unless its lag turned positive while it slept in the tree, which
 * DELAY_ZERO drops by placing it at V.
 */
static void requeue_delayed_entity(struct sched_entity *se)
{
    struct cfs_rq *cfs_rq = se->cfs_rq;

    if (sched_feat(DELAY_ZERO)) {
        update_entity_lag(cfs_rq, se);
        if (se->vlag > 0) {
            cfs_rq->nr_running--;
            if (se != cfs_rq->curr) {
                dequeue_tree(cfs_rq, se);
            }
            se->vlag = 0;
            place_entity(cfs_rq, se, 0);
            if (se != cfs_rq->curr) {
                enqueue_tree(cfs_rq, se);
            }
            cfs_rq->nr_running++;
        }
    }
    update_load_avg(cfs_rq, se, 0);
    clear_delayed(se);
}

/** Queue a thread's entity and the group entities above it not yet queued,
 * giving each group entity its queue's least slice (enqueue_task_fair). */
static void enqueue_task_fair(struct process *p, int flags)
{
    struct sched_entity *se = &p->se;
    unsigned int idle_h_nr_running = (unsigned int)task_has_idle_policy(p);
    struct cfs_rq *cfs_rq;
    uint64_t slice = 0;

    for (; se; se = se->parent) {
        if (se->on_rq) {
            if (se->sched_delayed) {
                requeue_delayed_entity(se);
            }
            break;
        }
        cfs_rq = se->cfs_rq;
        /* a group entity takes its queue's least slice, so the group can
         * serve its entities in their requested time */
        if (slice) {
            se->slice = slice;
            se->custom_slice = 1;
        }
        enqueue_entity(cfs_rq, se, flags);
        slice = cfs_rq_min_slice(cfs_rq);
        cfs_rq->h_nr_runnable++;
        cfs_rq->h_nr_queued++;
        cfs_rq->idle_h_nr_running += idle_h_nr_running;
        if (cfs_rq->idle > 0) {
            idle_h_nr_running = 1;
        }
        flags = ENQUEUE_WAKEUP;
    }
    for (; se; se = se->parent) {
        cfs_rq = se->cfs_rq;
        update_load_avg(cfs_rq, se, FAIR_LOAD_UPDATE_TG);
        se_update_runnable(se);
        update_cfs_group(se);
        se->slice = slice;
        if (se != cfs_rq->curr) {
            min_vruntime_cb_propagate(&se->run_node, NULL);
        }
        slice = cfs_rq_min_slice(cfs_rq);
        cfs_rq->h_nr_runnable++;
        cfs_rq->h_nr_queued++;
        cfs_rq->idle_h_nr_running += idle_h_nr_running;
        if (cfs_rq->idle > 0) {
            idle_h_nr_running = 1;
        }
    }
}

/** The entity to run on `cfs_rq`: the buddy when eligible, else EEVDF's
 * pick; NULL when the pick was delayed and its dequeue is now finished. */
static struct sched_entity *pick_next_entity(struct cfs_rq *cfs_rq)
{
    struct sched_entity *se;

    if (sched_feat(PICK_BUDDY) && cfs_rq->next && fair_entity_eligible(cfs_rq, cfs_rq->next)) {
        return cfs_rq->next;
    }
    se = pick_eevdf(cfs_rq);
    if (se && se->sched_delayed) {
        dequeue_entities(se, DEQUEUE_SLEEP | DEQUEUE_DELAYED, 1);
        return NULL;
    }
    return se;
}

/* The hooks of fair.h. */

void fair_cfs_rq_init(struct cfs_rq *cfs_rq, struct task_group *tg, int cpu)
{
    k_memset(cfs_rq, 0, sizeof(*cfs_rq));
    cfs_rq->zero_vruntime = (uint64_t)0 - ((uint64_t)1 << 20);
    cfs_rq->tg = tg;
    cfs_rq->cpu = cpu;
}

void fair_init(unsigned int ncpus)
{
    unsigned int cpus = ncpus < 8 ? ncpus : 8;
    unsigned int factor = (unsigned int)fair_fls(cpus ? cpus : 1);
    int cpu;

    /* 1 + log2(cpus), Linux's SCHED_TUNABLESCALING_LOG */
    sysctl_sched_base_slice = (uint64_t)factor * FAIR_BASE_SLICE_NS;
    for (cpu = 0; cpu < NR_CPUS; cpu++) {
        struct fair_rq *rq = &fair_rqs[cpu];

        fair_cfs_rq_init(&rq->cfs, &root_task_group, cpu);
        root_cfs_rq[cpu] = &rq->cfs;
        root_se[cpu] = NULL;
        rq->clock = rq->clock_task = 0;
        rq->curr = NULL;
        rq->leaf_head = NULL;
        rq->cfs_tasks = NULL;
        rq->charged_ns = 0;
        rq->resched = 0;
        rq->cpu = cpu;
    }
}

void fair_clock(int cpu, uint64_t now, uint64_t task)
{
    struct fair_rq *rq = &fair_rqs[cpu];

    if ((int64_t)(now - rq->clock) > 0) {
        rq->clock = now;
    }
    if ((int64_t)(task - rq->clock_task) > 0) {
        rq->clock_task = task;
    }
}

struct task_group *fair_task_group(struct process *p)
{
    if (fair_groups_enabled && p->sched_task_group) {
        return p->sched_task_group;
    }
    return &root_task_group;
}

void fair_set_task_rq(struct process *p, int cpu)
{
    struct task_group *tg = fair_task_group(p);
    struct sched_entity *se = &p->se;

    se->cfs_rq = tg->cfs_rq[cpu];
    se->parent = tg->se[cpu];
    se->depth = se->parent ? se->parent->depth + 1 : 0;
}

void fair_enqueue(struct process *p, int why)
{
    struct sched_entity *se = &p->se;
    int flags = 0;

    if (se->sched_delayed) {
        requeue_delayed_entity(se);
        return;
    }
    if (se->on_rq) {
        return;
    }
    fair_set_task_rq(p, p->cpu);
    if (why & FAIR_ENQ_WAKE) {
        flags |= ENQUEUE_WAKEUP;
    }
    if (why & FAIR_ENQ_NEW) {
        flags |= ENQUEUE_INITIAL;
    }
    if (why & FAIR_ENQ_MIGRATED) {
        flags |= ENQUEUE_MIGRATED;
    }
    if (why & FAIR_ENQ_RESTORE) {
        flags |= ENQUEUE_RESTORE;
    }
    enqueue_task_fair(p, flags);
}

int fair_dequeue(struct process *p, int why)
{
    struct sched_entity *se = &p->se;
    int flags;

    if (!se->on_rq) {
        return 1;
    }
    if (se->sched_delayed) {
        /* a forced removal or a move of a sleeper ends its delay */
        dequeue_entities(se, DEQUEUE_SLEEP | DEQUEUE_DELAYED, 0);
        if (why == FAIR_DEQ_MOVE) {
            se->avg.last_update_time = 0;
        }
        return 1;
    }
    switch (why) {
    case FAIR_DEQ_SLEEP:
        flags = DEQUEUE_SLEEP;
        break;
    case FAIR_DEQ_FORCED:
        flags = DEQUEUE_SLEEP | DEQUEUE_SPECIAL;
        break;
    case FAIR_DEQ_MOVE:
        flags = DEQUEUE_SAVE | DEQUEUE_MIGRATE;
        break;
    default:
        flags = DEQUEUE_SLEEP | DEQUEUE_SPECIAL;
        break;
    }
    if (dequeue_entities(se, flags, 0) < 0) {
        return 0;
    }
    /* the load left the queue's sums; the next enqueue attaches it */
    if (why == FAIR_DEQ_MOVE) {
        se->avg.last_update_time = 0;
    }
    return 1;
}

struct process *fair_pick(int cpu)
{
    struct fair_rq *rq = &fair_rqs[cpu];
    struct sched_entity *se;
    struct cfs_rq *cfs_rq;

again:
    cfs_rq = &rq->cfs;
    if (!cfs_rq->nr_running) {
        return NULL;
    }
    do {
        if (cfs_rq->curr && cfs_rq->curr->on_rq) {
            update_curr(cfs_rq);
        }
        se = pick_next_entity(cfs_rq);
        if (!se) {
            goto again;
        }
        cfs_rq = se->my_q;
    } while (cfs_rq);
    return fair_task_of(se);
}

void fair_set_next(struct process *p)
{
    struct sched_entity *se = &p->se;
    struct fair_rq *rq = rq_of(se->cfs_rq);

    if (se->on_rq) {
        /* the most recently picked thread heads the list */
        cfs_tasks_del(rq, se);
        cfs_tasks_add(rq, se);
    }
    for (; se; se = se->parent) {
        set_next_entity(se->cfs_rq, se);
    }
    rq->curr = p;
}

void fair_put_prev(struct process *p)
{
    struct sched_entity *se = &p->se;
    struct fair_rq *rq = rq_of(se->cfs_rq);

    for (; se; se = se->parent) {
        put_prev_entity(se->cfs_rq, se);
    }
    if (rq->curr == p) {
        rq->curr = NULL;
    }
}

int fair_resched(int cpu)
{
    int r = fair_rqs[cpu].resched;

    fair_rqs[cpu].resched = 0;
    return r;
}

int fair_tick(struct process *cur)
{
    struct sched_entity *se = &cur->se;
    struct fair_rq *rq = rq_of(se->cfs_rq);

    for (; se; se = se->parent) {
        entity_tick(se->cfs_rq, se);
    }
    return fair_resched(rq->cpu);
}

/** Ask `rq` to select again when `p` should preempt `cur` there
 * (check_preempt_wakeup_fair). */
static void check_preempt_wakeup(struct fair_rq *rq, struct process *cur, struct process *p,
                                 int wake_flags)
{
    struct sched_entity *se = &cur->se, *pse = &p->se;
    struct cfs_rq *cfs_rq;
    int cse_is_idle, pse_is_idle;

    if (se == pse) {
        return;
    }
    if (sched_feat(NEXT_BUDDY) && !(wake_flags & FAIR_WF_FORK) && !pse->sched_delayed) {
        set_next_buddy(pse);
    }
    /* a reschedule already asked for stands */
    if (rq->resched || !sched_feat(WAKEUP_PREEMPTION)) {
        return;
    }
    find_matching_se(&se, &pse);
    cse_is_idle = se_is_idle(se);
    pse_is_idle = se_is_idle(pse);
    /* a non-idle entity preempts an idle one, never the reverse */
    if (cse_is_idle && !pse_is_idle) {
        cancel_protect_slice(se);
        resched_curr(rq);
        return;
    }
    if (cse_is_idle != pse_is_idle) {
        return;
    }
    /* SCHED_BATCH and SCHED_IDLE threads do not preempt */
    if (p->sched_policy != SCHED_OTHER) {
        return;
    }
    cfs_rq = se->cfs_rq;
    update_curr(cfs_rq);
    /* an eligible wakee of a shorter slice ends the current protection,
     * even if it is not then the pick */
    if (do_preempt_short(cfs_rq, pse, se)) {
        cancel_protect_slice(se);
    }
    if (pick_eevdf(cfs_rq) == pse) {
        resched_curr(rq);
    }
}

int fair_wake_preempt(struct process *cur, struct process *p, int wake_flags)
{
    struct fair_rq *rq = rq_of(p->se.cfs_rq);

    check_preempt_wakeup(rq, cur, p, wake_flags);
    return fair_resched(rq->cpu);
}

void fair_yield(struct process *cur)
{
    struct sched_entity *se = &cur->se;
    struct cfs_rq *cfs_rq = se->cfs_rq;

    if (rq_of(cfs_rq)->cfs.h_nr_queued <= 1) {
        return;
    }
    clear_buddies(cfs_rq, se);
    update_curr(cfs_rq);
    /* only an eligible yielder forfeits the rest of its request, so a
     * picked ineligible one cannot run its virtual runtime away */
    if (fair_entity_eligible(cfs_rq, se)) {
        se->vruntime = se->deadline;
        update_deadline(cfs_rq, se);
    }
}

void fair_reweight(struct process *p)
{
    struct sched_entity *se = &p->se;
    struct load_weight lw;
    unsigned long old = se->load.weight;
    struct fair_rq *rq;

    task_load_weight(p, &lw);
    if (lw.weight == old && se->load.inv_weight == lw.inv_weight) {
        return;
    }
    if (!se->cfs_rq) {
        se->load = lw;
        return;
    }
    fair_reweight_entity(se->cfs_rq, se, lw.weight);
    se->load.inv_weight = lw.inv_weight;
    /* prio_changed_fair: a thread that lost weight while running selects
     * again; a queued one that gained may preempt the current */
    rq = rq_of(se->cfs_rq);
    if (!se->on_rq || rq->cfs.nr_running == 1) {
        return;
    }
    if (rq->curr == p) {
        if (lw.weight < old) {
            resched_curr(rq);
        }
    } else if (rq->curr) {
        check_preempt_wakeup(rq, rq->curr, p, 0);
    }
}

void fair_set_slice(struct process *p, uint64_t ns)
{
    struct sched_entity *se = &p->se;
    int queued = se->on_rq && se->cfs_rq && se->cfs_rq->curr != se;

    if (queued) {
        dequeue_tree(se->cfs_rq, se);
    }
    if (ns) {
        if (ns < FAIR_SLICE_MIN_NS) {
            ns = FAIR_SLICE_MIN_NS;
        } else if (ns > FAIR_SLICE_MAX_NS) {
            ns = FAIR_SLICE_MAX_NS;
        }
        se->custom_slice = 1;
        se->slice = ns;
    } else {
        se->custom_slice = 0;
        se->slice = sysctl_sched_base_slice;
    }
    if (queued) {
        enqueue_tree(se->cfs_rq, se);
    }
}

void fair_fork(struct process *child, struct process *parent)
{
    struct sched_entity *se = &child->se;

    (void)parent;
    k_memset(se, 0, sizeof(*se));
    RB_CLEAR_NODE(&se->run_node);
    task_load_weight(child, &se->load);
    fair_set_slice(child, child->sched_slice);
    fair_set_task_rq(child, child->cpu);
    if (fair_hooks.load_init) {
        fair_hooks.load_init(se);
    }
}

void fair_migrate(struct process *p, int to)
{
    struct sched_entity *se = &p->se;

    if (se->sched_delayed) {
        dequeue_entities(se, DEQUEUE_SLEEP | DEQUEUE_DELAYED, 1);
    }
    /* a sleeper's load leaves its old queue here; a queued thread's left
     * at its FAIR_DEQ_MOVE, which zeroed `last_update_time` */
    if (se->avg.last_update_time && fair_hooks.load_remove) {
        fair_hooks.load_remove(se);
    }
    se->avg.last_update_time = 0;
    se->nr_migrations++;
    fair_set_task_rq(p, to);
}

uint64_t fair_next_event_ns(struct process *cur)
{
    struct sched_entity *se = &cur->se;
    struct fair_rq *rq = rq_of(se->cfs_rq);
    uint64_t best = ~(uint64_t)0, real;
    int64_t vleft;
    int any = 0;

    for (; se; se = se->parent) {
        struct cfs_rq *cfs_rq = se->cfs_rq;

        update_curr(cfs_rq);
        if (cfs_rq->nr_running <= 1) {
            continue;
        }
        any = 1;
        vleft = (int64_t)(se->deadline - se->vruntime);
        if (vleft <= 0) {
            return rq->clock;
        }
        /* virtual time back to real: v * weight / 1024, rounded up */
        real = fair_mul_u64_u32_shr((uint64_t)vleft, (uint32_t)se->load.weight, NICE_0_SHIFT) + 1;
        if (real < best) {
            best = real;
        }
    }
    if (!any) {
        return 0;
    }
    return rq->clock + best;
}

void fair_finish_delayed(struct sched_entity *se)
{
    dequeue_entities(se, DEQUEUE_SLEEP | DEQUEUE_DELAYED, 1);
}
