/**
 * Host test of the fair class, proc/fair.c, and its tree, lib/rbtree.c,
 * against Linux 6.12's formulas, on a model processor whose clock the test
 * moves.
 *
 * host-deps: rach2/sys/lib/rbtree.c rach2/sys/tick.c rach2/sys/lib/kstring.c
 * host-deps: rach2/sys/lib/kctypes.c
 *
 * The rows drive the class's hooks as the scheduler would: a pick, then the
 * put of the current thread and the set of the pick when they differ, a tick
 * each 100 us, and a selection whenever the class asks for one. Each pick
 * is checked against a pick computed from the definition, over every
 * queued entity, and the rows judge shares, lag and placement by Linux's
 * formulas, never by the host's speed.
 */

#include "proc/fair.c"

extern int dprintf(int fd, const char *format, ...);

static int failures;

#define CHECK(test, message)                                    \
    do {                                                        \
        if (!(test)) {                                          \
            dprintf(2, "fair-hosttest: failed: %s\n", message); \
            failures++;                                         \
        }                                                       \
    } while (0)

/* A linear congruential generator, so each run takes the same path. */
static uint32_t seed = 12345u;

/** The next pseudo-random number, 24 bits. */
static uint32_t next_random(void)
{
    seed = seed * 1103515245u + 12345u;
    return seed >> 8;
}

/* The tree: random inserts and erases, every invariant checked. */

struct node {
    struct rb_node rb;
    uint32_t key;
    uint32_t value;
    uint32_t min; /* least value of the subtree */
    int in;
};

#define NODES 512
static struct node nodes[NODES];
static struct rb_root_cached tree;

#define rb_node_of(n) rb_entry((n), struct node, rb)

/** Recompute a node's subtree minimum; whether it did not change. */
static int node_update(struct node *n)
{
    uint32_t old = n->min;

    n->min = n->value;
    if (n->rb.rb_left && rb_node_of(n->rb.rb_left)->min < n->min) {
        n->min = rb_node_of(n->rb.rb_left)->min;
    }
    if (n->rb.rb_right && rb_node_of(n->rb.rb_right)->min < n->min) {
        n->min = rb_node_of(n->rb.rb_right)->min;
    }
    return n->min == old;
}

/** Recompute the minimums from `rb` up to `stop`. */
static void node_propagate(struct rb_node *rb, struct rb_node *stop)
{
    while (rb != stop) {
        struct node *n = rb_node_of(rb);

        if (node_update(n)) {
            break;
        }
        rb = rb_parent(rb);
    }
}

/** Give the node taking `old`'s place its minimum. */
static void node_copy(struct rb_node *old, struct rb_node *new_)
{
    rb_node_of(new_)->min = rb_node_of(old)->min;
}

/** Fix the minimums of a rotated pair. */
static void node_rotate(struct rb_node *old, struct rb_node *new_)
{
    node_copy(old, new_);
    node_update(rb_node_of(old));
}

static const struct rb_augment_callbacks node_cb = {node_propagate, node_copy, node_rotate};

/** The test tree's order, by key. */
static int node_less(const struct rb_node *a, const struct rb_node *b)
{
    return rb_node_of(a)->key < rb_node_of(b)->key;
}

/** Check the subtree at `rb`: order, colours, parents and minimums.
 * Returns its black height, or -1 after a failure. */
static int tree_check(struct rb_node *rb, struct rb_node *parent, uint32_t lo, uint32_t hi,
                      int *count)
{
    struct node *n;
    int lh, rh;
    uint32_t min;

    if (!rb) {
        return 1;
    }
    n = rb_node_of(rb);
    (*count)++;
    if (rb_parent(rb) != parent || n->key < lo || n->key > hi || !n->in) {
        return -1;
    }
    if ((rb->rb_parent_color & 1u) == 0 && parent && (parent->rb_parent_color & 1u) == 0) {
        return -1;
    }
    lh = tree_check(rb->rb_left, rb, lo, n->key, count);
    rh = tree_check(rb->rb_right, rb, n->key, hi, count);
    if (lh < 0 || rh < 0 || lh != rh) {
        return -1;
    }
    min = n->value;
    if (rb->rb_left && rb_node_of(rb->rb_left)->min < min) {
        min = rb_node_of(rb->rb_left)->min;
    }
    if (rb->rb_right && rb_node_of(rb->rb_right)->min < min) {
        min = rb_node_of(rb->rb_right)->min;
    }
    if (min != n->min) {
        return -1;
    }
    return lh + (int)(rb->rb_parent_color & 1u);
}

/** 87-1b: insert and erase at random, checking the tree after each. */
static void rbtree_rows(void)
{
    int i, op, bad = 0, count, expect = 0, least;
    struct rb_node *first;

    for (op = 0; op < 20000 && !bad; op++) {
        struct node *n = &nodes[next_random() % NODES];

        if (n->in) {
            rb_erase_augmented_cached(&n->rb, &tree, &node_cb);
            n->in = 0;
            expect--;
        } else {
            n->key = next_random() % 1000u;
            n->value = next_random();
            n->min = n->value;
            rb_add_augmented_cached(&n->rb, &tree, node_less, &node_cb);
            n->in = 1;
            expect++;
        }
        count = 0;
        if (tree.rb_root.rb_node && (tree.rb_root.rb_node->rb_parent_color & 1u) == 0) {
            bad = 1;
        }
        if (tree_check(tree.rb_root.rb_node, NULL, 0, ~0u, &count) < 0 || count != expect) {
            bad = 1;
        }
        first = rb_first(&tree.rb_root);
        if (first != tree.rb_leftmost) {
            bad = 1;
        }
        least = -1;
        for (i = 0; i < NODES; i++) {
            if (nodes[i].in && (least < 0 || nodes[i].key < nodes[least].key)) {
                least = i;
            }
        }
        if (first && rb_node_of(first)->key != nodes[least].key) {
            bad = 1;
        }
    }
    CHECK(!bad, "the tree keeps its order, colours, parents, leftmost and subtree minimums");
    /* walk the tree in order, forward and back */
    count = 0;
    for (first = rb_first(&tree.rb_root); first; first = rb_next(first)) {
        struct rb_node *nx = rb_next(first);

        if (nx && rb_node_of(nx)->key < rb_node_of(first)->key) {
            bad = 1;
        }
        if (nx && rb_prev(nx) != first) {
            bad = 1;
        }
        count++;
    }
    CHECK(!bad && count == expect, "rb_next and rb_prev walk the tree in order");
}

/* The model processor. */

#define NPROC 8
static struct process procs[NPROC];
static struct process *cur;
static uint64_t now;
static int blocked_calls;
static struct process *blocked_last;

/** Count the finished delayed dequeues the class reports. */
static void blocked_hook(struct process *p)
{
    blocked_calls++;
    blocked_last = p;
}

/** A fresh class on one processor, the clock at 1 s. */
static void reset(void)
{
    int i;

    fair_init(1);
    fair_hooks.blocked = blocked_hook;
    for (i = 0; i < NPROC; i++) {
        k_memset(&procs[i], 0, sizeof(procs[i]));
        procs[i].cpu = 0;
    }
    cur = NULL;
    now = 1000000000u;
    blocked_calls = 0;
    blocked_last = NULL;
    fair_clock(0, now, now);
}

/** Give `p` its entity as a fork would and queue it as new. */
static void spawn(struct process *p, int nice, int policy)
{
    p->nice = nice;
    p->sched_policy = (uint8_t)policy;
    fair_fork(p, NULL);
    fair_clock(0, now, now);
    fair_enqueue(p, FAIR_ENQ_NEW);
}

/*
 * The pick from the definition: among the queued entities of `cfs_rq`, the
 * current one included, those whose lag is not negative, the earliest
 * deadline; the current one while eligible and protected; the only one when
 * it is alone. The tree's pick must agree.
 */
static struct sched_entity *brute_pick(struct cfs_rq *cfs_rq)
{
    struct sched_entity *all[NPROC + 1], *best = NULL, *curr = cfs_rq->curr;
    int n = 0, i;
    struct rb_node *rb;
    int64_t avg = 0, load = 0;

    for (rb = rb_first(&cfs_rq->tasks_timeline.rb_root); rb; rb = rb_next(rb)) {
        all[n++] = node_to_se(rb);
    }
    if (curr && curr->on_rq) {
        all[n++] = curr;
    } else {
        curr = NULL;
    }
    if (n == 1) {
        return all[0];
    }
    for (i = 0; i < n; i++) {
        avg += entity_key(cfs_rq, all[i]) * (int64_t)all[i]->load.weight;
        load += (int64_t)all[i]->load.weight;
    }
    if (curr && avg >= entity_key(cfs_rq, curr) * load && protect_slice(curr)) {
        return curr;
    }
    for (i = 0; i < n; i++) {
        if (avg < entity_key(cfs_rq, all[i]) * load) {
            continue;
        }
        if (!best || entity_before(all[i], best) ||
            (!entity_before(best, all[i]) && all[i] != curr && best == curr)) {
            best = all[i];
        }
    }
    return best;
}

static int pick_mismatch, pick_compared;

/** Select on the model processor as the scheduler does. */
static void schedule_model(void)
{
    struct process *n;
    struct sched_entity *want;

    fair_clock(0, now, now);
    if (fair_rqs[0].cfs.nr_running && !fair_rqs[0].cfs.next) {
        /* the definition's pick, before the pick charges the current */
        if (fair_rqs[0].cfs.curr && fair_rqs[0].cfs.curr->on_rq) {
            update_curr(&fair_rqs[0].cfs);
        }
        want = brute_pick(&fair_rqs[0].cfs);
        if (want && want->sched_delayed) {
            want = NULL;
        }
        n = fair_pick(0);
        if (want && n) {
            pick_compared++;
            if (&n->se != want) {
                pick_mismatch++;
            }
        }
    } else {
        n = fair_pick(0);
    }
    if (n != cur) {
        if (cur) {
            fair_put_prev(cur);
        }
        if (n) {
            fair_set_next(n);
        }
        cur = n;
    }
    fair_resched(0);
}

/** Run the model for `ns`, ticking each 100 us. */
static void run_for(uint64_t ns)
{
    uint64_t end = now + ns;

    while (now < end) {
        uint64_t step = end - now < 100000u ? end - now : 100000u;

        now += step;
        fair_clock(0, now, now);
        if (cur && fair_tick(cur)) {
            schedule_model();
        }
    }
}

/** The lag w * (V - v) of a queued entity, in weighted nanoseconds. */
static int64_t lag_of(struct cfs_rq *cfs_rq, struct sched_entity *se)
{
    return (int64_t)(fair_avg_vruntime(cfs_rq) - se->vruntime);
}

/** The ratio a / b against `want`, within `tolerance` of it. */
static int ratio_within(uint64_t a, uint64_t b, double want, double tolerance)
{
    double r = (double)a / (double)b;

    return r > want * (1.0 - tolerance) && r < want * (1.0 + tolerance);
}

/** Shares by weight within 1% over 10,000 slices. */
static void share_rows(void)
{
    uint64_t a0, b0, c0;

    reset();
    spawn(&procs[0], 0, SCHED_OTHER);
    spawn(&procs[1], 5, SCHED_OTHER);
    schedule_model();
    run_for(100000000u);
    a0 = procs[0].se.sum_exec_runtime;
    b0 = procs[1].se.sum_exec_runtime;
    run_for(7500000000u);
    CHECK(ratio_within(procs[0].se.sum_exec_runtime - a0, procs[1].se.sum_exec_runtime - b0,
                       1024.0 / 335.0, 0.01),
          "nice 0 against nice 5 shares 1024:335 within 1%");
    CHECK(pick_mismatch == 0, "every pick of the shares rows is EEVDF's by definition");

    reset();
    spawn(&procs[0], 0, SCHED_OTHER);
    spawn(&procs[1], -5, SCHED_OTHER);
    spawn(&procs[2], 19, SCHED_OTHER);
    schedule_model();
    run_for(100000000u);
    a0 = procs[0].se.sum_exec_runtime;
    b0 = procs[1].se.sum_exec_runtime;
    c0 = procs[2].se.sum_exec_runtime;
    run_for(10000000000u);
    CHECK(ratio_within(procs[1].se.sum_exec_runtime - b0, procs[0].se.sum_exec_runtime - a0,
                       3121.0 / 1024.0, 0.01),
          "nice -5 against nice 0 shares 3121:1024 within 1%");
    CHECK(ratio_within(procs[0].se.sum_exec_runtime - a0, procs[2].se.sum_exec_runtime - c0,
                       1024.0 / 15.0, 0.01),
          "nice 0 against nice 19 shares 1024:15 within 1%");

    reset();
    spawn(&procs[0], 19, SCHED_OTHER);
    spawn(&procs[1], 0, SCHED_IDLE);
    schedule_model();
    run_for(100000000u);
    a0 = procs[0].se.sum_exec_runtime;
    b0 = procs[1].se.sum_exec_runtime;
    run_for(10000000000u);
    CHECK(ratio_within(procs[0].se.sum_exec_runtime - a0, procs[1].se.sum_exec_runtime - b0,
                       15.0 / 3.0, 0.01),
          "nice 19 against SCHED_IDLE shares 15:3 within 1%");
    CHECK(pick_mismatch == 0, "every pick of the weighted rows is EEVDF's by definition");
}

/** A sleeper among four spinners: eligible and earliest, it is the next
 * pick, and it waits at most a slice for the processor. */
static void sleeper_rows(void)
{
    struct process *s = &procs[4];
    uint64_t woke, worst = 0;
    int i, cycles, wrong_pick = 0;

    reset();
    for (i = 0; i < 4; i++) {
        spawn(&procs[i], 0, SCHED_OTHER);
    }
    spawn(s, 0, SCHED_OTHER);
    schedule_model();
    run_for(50000000u);
    for (cycles = 0; cycles < 2000; cycles++) {
        /* run until the sleeper runs, then 100 us of it, then 1 ms asleep */
        while (cur != s) {
            run_for(100000u);
        }
        run_for(100000u);
        if (cur == s) {
            fair_dequeue(s, FAIR_DEQ_SLEEP);
            schedule_model();
        }
        run_for(1000000u);
        if (s->se.sched_delayed || !s->se.on_rq) {
            fair_clock(0, now, now);
            fair_enqueue(s, FAIR_ENQ_WAKE);
            if (cur) {
                fair_wake_preempt(cur, s, FAIR_WF_TTWU);
            }
        }
        woke = now;
        /* at the next pick, the definition's choice is the class's; when
         * the sleeper is eligible and earliest, that is the sleeper */
        while (cur != s) {
            run_for(100000u);
        }
        if (now - woke > worst) {
            worst = now - woke;
        }
    }
    (void)wrong_pick;
    CHECK(pick_mismatch == 0, "with a sleeper among spinners every pick is EEVDF's");
    CHECK(worst <= sysctl_sched_base_slice + 2 * 100000u,
          "a woken sleeper among four spinners runs within one slice");
}

/** Lag kept across a sleep, and a negative lag repaid by delayed dequeue. */
static void lag_rows(void)
{
    struct process *a = &procs[0], *b = &procs[1], *c = &procs[2];
    struct cfs_rq *rq = &fair_rqs[0].cfs;
    int64_t vlag, after;

    /* an eligible thread leaves with its lag and comes back with it */
    reset();
    spawn(a, 0, SCHED_OTHER);
    spawn(b, 0, SCHED_OTHER);
    spawn(c, 0, SCHED_OTHER);
    schedule_model();
    run_for(20000000u);
    while (cur == c || !fair_entity_eligible(rq, &c->se) || lag_of(rq, &c->se) <= 0) {
        run_for(100000u);
    }
    CHECK(fair_dequeue(c, FAIR_DEQ_SLEEP) == 1, "an eligible sleeper leaves the queue at once");
    vlag = c->se.vlag;
    CHECK(vlag > 0, "an eligible sleeper keeps a positive lag");
    run_for(5000000u);
    fair_clock(0, now, now);
    fair_enqueue(c, FAIR_ENQ_WAKE);
    after = lag_of(rq, &c->se);
    CHECK(after - vlag <= 1 && vlag - after <= 1,
          "the woken thread's lag after placement is the lag it left with");

    /* an ineligible thread's sleep is delayed until its lag is repaid */
    reset();
    spawn(a, 0, SCHED_OTHER);
    spawn(b, 0, SCHED_OTHER);
    spawn(c, 0, SCHED_OTHER);
    schedule_model();
    run_for(20000000u);
    while (cur == NULL || fair_entity_eligible(rq, &cur->se)) {
        run_for(100000u);
    }
    {
        struct process *d = cur;
        unsigned int runnable = rq->h_nr_runnable;

        CHECK(fair_dequeue(d, FAIR_DEQ_SLEEP) == 0, "an ineligible sleeper's dequeue is delayed");
        CHECK(d->se.sched_delayed && d->se.on_rq, "the delayed thread stays queued");
        CHECK(rq->h_nr_runnable == runnable - 1 && rq->h_nr_delayed == 1,
              "the delayed thread counts as delayed, not runnable");
        schedule_model();
        CHECK(cur != d, "a delayed thread is not picked to run");
        while (d->se.on_rq) {
            run_for(100000u);
            CHECK(cur != d, "a delayed thread never runs");
            if (cur == d) {
                break;
            }
        }
        CHECK(blocked_calls == 1 && blocked_last == d,
              "the pick that meets the repaid thread finishes its dequeue and says so");
        CHECK(d->se.vlag <= 0, "the finished dequeue keeps no positive lag (DELAY_ZERO)");
        CHECK(rq->h_nr_delayed == 0 && rq->h_nr_queued == 2, "the counts drop the thread");
    }

    /* a wake of a delayed thread requeues it in place */
    reset();
    spawn(a, 0, SCHED_OTHER);
    spawn(b, 0, SCHED_OTHER);
    schedule_model();
    run_for(20000000u);
    while (cur == NULL || fair_entity_eligible(rq, &cur->se)) {
        run_for(100000u);
    }
    {
        struct process *d = cur;
        uint64_t v = d->se.vruntime;

        fair_dequeue(d, FAIR_DEQ_SLEEP);
        schedule_model();
        fair_enqueue(d, FAIR_ENQ_WAKE);
        CHECK(!d->se.sched_delayed && d->se.on_rq && d->se.vruntime == v && rq->h_nr_runnable == 2,
              "a woken delayed thread competes on from where it was");
    }

    /* a forced removal finishes a delayed dequeue */
    reset();
    spawn(a, 0, SCHED_OTHER);
    spawn(b, 0, SCHED_OTHER);
    schedule_model();
    run_for(20000000u);
    while (cur == NULL || fair_entity_eligible(rq, &cur->se)) {
        run_for(100000u);
    }
    {
        struct process *d = cur;

        fair_dequeue(d, FAIR_DEQ_SLEEP);
        schedule_model();
        CHECK(fair_dequeue(d, FAIR_DEQ_FORCED) == 1 && !d->se.on_rq && !d->se.sched_delayed &&
                  rq->h_nr_queued == 1 && rq->nr_running == 1,
              "a forced removal of a delayed thread takes it off whole");
        CHECK(fair_dequeue(b == d ? a : b, FAIR_DEQ_FORCED) == 1, "a stop is never delayed");
    }
}

/** A reweight keeps lag: w * (V - v) before equals w' * (V' - v') after,
 * and V does not move. */
static void reweight_rows(void)
{
    struct cfs_rq *rq = &fair_rqs[0].cfs;
    struct process *a = &procs[0], *b = &procs[1], *c = &procs[2];
    int64_t before, after;
    uint64_t v0, v1;

    reset();
    spawn(a, 0, SCHED_OTHER);
    spawn(b, 0, SCHED_OTHER);
    spawn(c, 0, SCHED_OTHER);
    schedule_model();
    run_for(13300000u);
    while (cur == c || lag_of(rq, &c->se) == 0) {
        run_for(100000u);
    }
    v0 = fair_avg_vruntime(rq);
    before = lag_of(rq, &c->se) * (int64_t)c->se.load.weight;
    c->nice = 5;
    fair_reweight(c);
    v1 = fair_avg_vruntime(rq);
    after = lag_of(rq, &c->se) * (int64_t)c->se.load.weight;
    CHECK(c->se.load.weight == 335, "nice 5 weighs 335");
    CHECK((int64_t)(v1 - v0) <= 1 && (int64_t)(v0 - v1) <= 1, "a reweight leaves V where it was");
    CHECK(after - before <= 2 * 1024 && before - after <= 2 * 1024,
          "a reweight keeps the queued thread's lag");
    CHECK(pick_mismatch == 0, "every pick of the reweight rows is EEVDF's by definition");

    /* a sleeping thread's virtual lag scales by w / w' */
    fair_dequeue(c, FAIR_DEQ_FORCED);
    before = c->se.vlag;
    c->nice = 0;
    fair_reweight(c);
    CHECK(c->se.vlag == fair_div_s64(before * 335, 1024), "a sleeper's virtual lag scales");
}

/** A child is placed at V with half a slice (PLACE_DEADLINE_INITIAL). */
static void fork_rows(void)
{
    struct cfs_rq *rq = &fair_rqs[0].cfs;
    struct process *child = &procs[3];
    uint64_t v;

    reset();
    spawn(&procs[0], 0, SCHED_OTHER);
    spawn(&procs[1], 0, SCHED_OTHER);
    schedule_model();
    run_for(7700000u);
    fair_clock(0, now, now);
    update_curr(rq);
    v = fair_avg_vruntime(rq);
    child->nice = 0;
    fair_fork(child, cur);
    fair_enqueue(child, FAIR_ENQ_NEW);
    CHECK((int64_t)(child->se.vruntime - v) <= 1 && (int64_t)(v - child->se.vruntime) <= 1,
          "a new thread starts at V");
    CHECK(child->se.deadline - child->se.vruntime == sysctl_sched_base_slice / 2,
          "a new thread's first deadline is half a slice on");
    CHECK(child->se.slice == sysctl_sched_base_slice && !child->se.custom_slice,
          "a new thread takes the base slice");

    child->sched_slice = 300000u;
    fair_dequeue(child, FAIR_DEQ_FORCED);
    fair_fork(child, cur);
    CHECK(child->se.custom_slice && child->se.slice == 300000u,
          "a fork keeps the custom slice sched_setattr gave");
    fair_init(4);
    CHECK(sysctl_sched_base_slice == 2250000u, "four processors scale the base slice to 2.25 ms");
    fair_init(64);
    CHECK(sysctl_sched_base_slice == 3000000u, "the scaling stops at eight processors");
}

/** RUN_TO_PARITY's protection, and PREEMPT_SHORT's cancel of it. */
static void parity_rows(void)
{
    struct cfs_rq *rq = &fair_rqs[0].cfs;
    struct process *a = &procs[0], *b = &procs[1], *w = &procs[2];

    reset();
    spawn(a, 0, SCHED_OTHER);
    spawn(b, 0, SCHED_OTHER);
    schedule_model();
    run_for(30000000u);
    /* just after a pick, the current thread is protected */
    while (cur->se.sum_exec_runtime != cur->se.prev_sum_exec_runtime) {
        run_for(100000u);
    }
    CHECK(protect_slice(&cur->se), "a picked thread starts protected");
    w->nice = 0;
    fair_fork(w, cur);
    w->se.vlag = (int64_t)calc_delta_fair(sysctl_sched_base_slice, &w->se);
    fair_enqueue(w, FAIR_ENQ_WAKE);
    CHECK(fair_entity_eligible(rq, &w->se) && entity_before(&w->se, &cur->se),
          "the wakee is eligible and earlier");
    CHECK(!fair_wake_preempt(cur, w, FAIR_WF_TTWU),
          "an equal-slice wakee does not end the protection (RUN_TO_PARITY)");
    CHECK(protect_slice(&cur->se), "the protection stands");

    fair_dequeue(w, FAIR_DEQ_FORCED);
    fair_set_slice(w, 100000u);
    w->se.vlag = (int64_t)calc_delta_fair(sysctl_sched_base_slice, &w->se);
    fair_enqueue(w, FAIR_ENQ_WAKE);
    CHECK(fair_wake_preempt(cur, w, FAIR_WF_TTWU),
          "an eligible wakee of a shorter slice preempts (PREEMPT_SHORT)");
    CHECK(!protect_slice(&cur->se), "the shorter slice ended the protection");
    schedule_model();
    CHECK(cur == w, "the short-slice wakee runs next");
    CHECK(pick_mismatch == 0, "every pick of the parity rows is EEVDF's by definition");
}

/* A group on the model processor, its entity weighing NICE_0_LOAD. */
static struct task_group group;
static struct sched_entity *group_se_slot[NR_CPUS];
static struct cfs_rq *group_rq_slot[NR_CPUS];
static struct sched_entity group_se;
static struct cfs_rq group_rq;

/** Make the group, its entity on the root queue and its queue. */
static void group_setup(void)
{
    group.se = group_se_slot;
    group.cfs_rq = group_rq_slot;
    group.shares = NICE_0_LOAD;
    group.parent = &root_task_group;
    group_se_slot[0] = &group_se;
    group_rq_slot[0] = &group_rq;
    fair_cfs_rq_init(&group_rq, &group, 0);
    k_memset(&group_se, 0, sizeof(group_se));
    RB_CLEAR_NODE(&group_se.run_node);
    group_se.load.weight = NICE_0_LOAD;
    group_se.my_q = &group_rq;
    group_se.cfs_rq = &fair_rqs[0].cfs;
    group_se.slice = sysctl_sched_base_slice;
}

/** Wakes in the same group and across groups, judged at the queue the
 * two entities share. */
static void group_rows(void)
{
    struct process *a = &procs[0], *b = &procs[1], *c = &procs[2], *d = &procs[3];
    struct cfs_rq *root = &fair_rqs[0].cfs;

    reset();
    group_setup();
    a->sched_task_group = &group;
    b->sched_task_group = &group;
    c->sched_task_group = &group;
    spawn(a, 0, SCHED_OTHER);
    spawn(b, 0, SCHED_OTHER);
    spawn(d, 0, SCHED_OTHER);
    CHECK(a->se.parent == &group_se && a->se.depth == 1 && a->se.cfs_rq == &group_rq,
          "a grouped thread is queued below its group's entity");
    CHECK(root->nr_running == 2 && root->h_nr_queued == 3 && group_rq.nr_running == 2,
          "the root holds the group's entity and the ungrouped thread");
    schedule_model();
    run_for(40000000u);
    CHECK(a->se.sum_exec_runtime + b->se.sum_exec_runtime > 15000000u &&
              d->se.sum_exec_runtime > 15000000u,
          "the group and the lone thread share the processor half and half");

    /* same group: the wake is judged inside the group's queue */
    while (cur != a && cur != b) {
        run_for(100000u);
    }
    while (cur->se.sum_exec_runtime != cur->se.prev_sum_exec_runtime) {
        run_for(100000u);
    }
    /* placed at V, the wakee leaves V and the current's eligibility as
     * they were */
    c->nice = 0;
    fair_fork(c, cur);
    fair_enqueue(c, FAIR_ENQ_WAKE);
    CHECK(c->se.cfs_rq == &group_rq, "the grouped wakee joins its group's queue");
    CHECK(fair_entity_eligible(&group_rq, &cur->se), "the current sibling is still eligible");
    CHECK(!fair_wake_preempt(cur, c, FAIR_WF_TTWU),
          "an equal-slice wakee in the same group waits for the protected current");
    fair_dequeue(c, FAIR_DEQ_FORCED);
    fair_set_slice(c, 100000u);
    c->se.vlag = 0;
    fair_enqueue(c, FAIR_ENQ_WAKE);
    CHECK(fair_wake_preempt(cur, c, FAIR_WF_TTWU),
          "a short-slice wakee in the same group preempts its sibling");
    schedule_model();
    CHECK(cur == c || cur == d, "the pick takes the wakee once its group wins at the root");
    fair_dequeue(c, FAIR_DEQ_FORCED);
    schedule_model();

    /* across groups: the wake is judged between the group entity and the
     * wakee at the root */
    while (cur != a && cur != b) {
        run_for(100000u);
    }
    while (cur->se.sum_exec_runtime != cur->se.prev_sum_exec_runtime) {
        run_for(100000u);
    }
    fair_dequeue(d, FAIR_DEQ_FORCED);
    fair_set_slice(d, 100000u);
    d->se.vlag = (int64_t)calc_delta_fair(sysctl_sched_base_slice, &d->se);
    fair_enqueue(d, FAIR_ENQ_WAKE);
    CHECK(group_rq.next == NULL, "no buddy is nominated without NEXT_BUDDY");
    CHECK(fair_wake_preempt(cur, d, FAIR_WF_TTWU),
          "a short-slice wakee at the root preempts the group's entity");
    schedule_model();
    CHECK(cur == d, "the root's wakee runs next");

    /* a sleeping group member nominates its group as the buddy */
    while (cur != a && cur != b) {
        run_for(100000u);
    }
    {
        struct process *s = cur;

        fair_dequeue(s, FAIR_DEQ_FORCED);
        CHECK(root->next == &group_se, "a member's sleep nominates its group (PICK_BUDDY)");
        schedule_model();
        CHECK(cur == (s == a ? b : a), "the buddy group's other member runs next");
    }

    /* SCHED_BATCH does not preempt, and anything preempts SCHED_IDLE */
    reset();
    spawn(a, 0, SCHED_OTHER);
    schedule_model();
    run_for(1000000u);
    b->nice = 0;
    b->sched_policy = SCHED_BATCH;
    fair_fork(b, a);
    fair_set_slice(b, 100000u);
    b->se.vlag = (int64_t)calc_delta_fair(sysctl_sched_base_slice, &b->se);
    fair_enqueue(b, FAIR_ENQ_WAKE);
    CHECK(!fair_wake_preempt(cur, b, FAIR_WF_TTWU), "a SCHED_BATCH wakee does not preempt");
    reset();
    spawn(a, 0, SCHED_IDLE);
    schedule_model();
    run_for(1000000u);
    spawn(b, 19, SCHED_OTHER);
    CHECK(fair_wake_preempt(cur, b, FAIR_WF_TTWU), "an ordinary wakee preempts SCHED_IDLE");
}

/** A yield moves an eligible yielder's virtual runtime to its deadline. */
static void yield_rows(void)
{
    struct cfs_rq *rq = &fair_rqs[0].cfs;
    uint64_t deadline;

    reset();
    spawn(&procs[0], 0, SCHED_OTHER);
    spawn(&procs[1], 0, SCHED_OTHER);
    schedule_model();
    run_for(10000000u);
    while (cur->se.sum_exec_runtime != cur->se.prev_sum_exec_runtime ||
           !fair_entity_eligible(rq, &cur->se)) {
        run_for(100000u);
    }
    deadline = cur->se.deadline;
    fair_yield(cur);
    CHECK(cur->se.vruntime == deadline,
          "an eligible yielder's virtual runtime moves to its deadline");
    CHECK(cur->se.deadline == deadline + sysctl_sched_base_slice,
          "the yielder takes a new deadline a slice on");
    {
        struct process *y = cur;

        schedule_model();
        CHECK(cur != y, "the other thread runs after the yield");
    }
}

/** The slice event: the monotonic time at which the current thread reaches
 * its deadline, 0 when nothing competes. */
static void event_rows(void)
{
    uint64_t at;

    reset();
    spawn(&procs[0], 0, SCHED_OTHER);
    schedule_model();
    CHECK(fair_next_event_ns(cur) == 0, "a lone thread needs no slice event");
    spawn(&procs[1], 5, SCHED_OTHER);
    run_for(100000u);
    at = fair_next_event_ns(cur);
    CHECK(at > now && at <= now + sysctl_sched_base_slice + 1,
          "the event falls within the current slice");
    now = at;
    fair_clock(0, now, now);
    CHECK(fair_tick(cur), "at the event the thread has reached its deadline");
}

int main(void)
{
    rbtree_rows();
    share_rows();
    sleeper_rows();
    lag_rows();
    reweight_rows();
    fork_rows();
    parity_rows();
    group_rows();
    yield_rows();
    event_rows();
    CHECK(pick_compared > 10000, "the rows compared over ten thousand picks with the definition's");
    if (!failures) {
        dprintf(1, "fair-hosttest: pass\n");
    }
    return failures != 0;
}
