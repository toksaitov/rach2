/**
 * Host test of group scheduling and autogroup, proc/fair_group.c and
 * proc/autogroup.c, over the real fair class and its load tracking.
 *
 * host-deps: rach2/sys/proc/fair.c rach2/sys/proc/fair_group.c
 * host-deps: rach2/sys/proc/autogroup.c rach2/sys/proc/pelt.c
 * host-deps: rach2/sys/lib/rbtree.c rach2/sys/lib/kstring.c
 * host-deps: rach2/sys/lib/kctypes.c rach2/sys/tick.c
 * host-deps: tests/host/compat/kheap_host.c
 * host-cflags: -DRACH2_HOST_CPU
 *
 * The rows run threads on modeled processors whose clocks only the rows
 * move: each step picks with `fair_pick`, charges the thread picked by
 * advancing its processor's clock, and selects again when the class asks.
 * The shares are read from the threads' `se.sum_exec_runtime`, the time
 * the class charged, never from the host's clock.
 */

#include "proc/autogroup.h"
#include "proc/fair.h"
#include "proc/pelt.h"
#include "proc/process.h"
#include "proc/tgroup.h"
#include "lib/kerrno.h"
#include "lib/kstring.h"
#include "timer.h"

extern int dprintf(int fd, const char *format, ...);

process_t *proc_list;
struct percpu cpu_table[NR_CPUS];
volatile uint64_t kernel_ticks;
unsigned int host_cpu;

static int failures;
static int privileged = 1;
static unsigned int lock_depth;
static uint64_t kicks;

#define CHECK(test, message)                                         \
    do {                                                             \
        if (!(test)) {                                               \
            dprintf(2, "autogroup-hosttest: failed: %s\n", message); \
            failures++;                                              \
        }                                                            \
    } while (0)

/** sched_lock, which the model only counts, to see that every take is let
 * go. */
uint32_t sched_lock_take(void)
{
    lock_depth++;
    return 0;
}

/** Let sched_lock go. */
void sched_lock_give(uint32_t flags)
{
    (void)flags;
    lock_depth--;
}

/** Record the processors a group change asks to select again. */
void sched_kick_cpus(uint64_t cpus)
{
    kicks |= cpus;
}

/** Whether the caller has privilege, which the rows set. */
int cred_privileged(void)
{
    return privileged;
}

/** Whether `p` may lower a nice value to `nice`: only with privilege, as a
 * zero RLIMIT_NICE allows. */
int proc_nice_allowed(const process_t *p, int32_t nice)
{
    (void)p;
    (void)nice;
    return privileged;
}

/* The modeled threads and their thread groups, and each processor's
 * monotonic clock in nanoseconds, which is also its task clock. */
#define THREADS 8
static process_t threads[THREADS];
static struct tgroup groups[THREADS];
static uint64_t clock_ns[NR_CPUS];

/* The thread running on each processor, NULL for none. */
static process_t *running[NR_CPUS];

/* The record a session's first thread is forked from: nice 0, in the root
 * group. */
static process_t forker;

/** Give processor `cpu` its clocks, as the scheduler does before a group
 * change (`fair_group_clock`). */
static void model_clock(int cpu)
{
    fair_clock(cpu, clock_ns[cpu], clock_ns[cpu]);
}

/** Start the model with `ncpus` processors online, no thread and the groups
 * on. */
static void setup(unsigned int ncpus)
{
    unsigned int c;
    int i;

    for (i = 0; i < THREADS; i++) {
        k_memset(&threads[i], 0, sizeof(threads[i]));
        k_memset(&groups[i], 0, sizeof(groups[i]));
    }
    proc_list = NULL;
    for (c = 0; c < NR_CPUS; c++) {
        cpu_table[c].online = c < ncpus;
        clock_ns[c] = 1000000000u;
        running[c] = NULL;
    }
    fair_init(ncpus);
    fair_group_init();
    pelt_init();
    fair_group_clock = model_clock;
    autogroup_set_enabled(1);
    for (c = 0; c < ncpus; c++) {
        model_clock((int)c);
    }
}

/** Make thread `i` of thread group `g`, on processor `cpu`, nice `nice`,
 * linked on the process list; the first thread of a group leads it. */
static process_t *thread_new(int i, int g, int cpu, int nice)
{
    process_t *p = &threads[i];

    p->pid = (uint32_t)i + 100u;
    p->tgid = groups[g].leader ? groups[g].leader->pid : p->pid;
    p->tg = &groups[g];
    if (!groups[g].leader) {
        groups[g].leader = p;
        groups[g].refs = 1;
    }
    groups[g].live++;
    p->state = PROC_READY;
    p->cpu = cpu;
    p->nice = nice;
    p->next = proc_list;
    proc_list = p;
    return p;
}

/** Queue thread `p`, made from `parent` as a fork or a clone makes it,
 * on its processor for the first time; a session's first thread, which
 * is its own `parent`, takes its entity from `forker`. */
static void thread_start(process_t *p, process_t *parent)
{
    lock_depth++;
    model_clock(p->cpu);
    autogroup_fork(p, parent);
    fair_fork(p, parent == p ? &forker : parent);
    fair_set_task_rq(p, p->cpu);
    fair_reweight(p);
    fair_enqueue(p, FAIR_ENQ_NEW);
    lock_depth--;
}

/** Select on `cpu`, putting back its current thread, as the scheduler's
 * switch does. */
static void select_on(int cpu)
{
    process_t *next = fair_pick(cpu);

    if (next == running[cpu]) {
        return;
    }
    if (running[cpu]) {
        fair_put_prev(running[cpu]);
    }
    if (next) {
        fair_set_next(next);
    }
    running[cpu] = next;
}

/** Run processor `cpu` for `ns` nanoseconds in steps of `step`: charge the
 * running thread at each step, as the tick and the slice event do, and
 * select again when the class asks. */
static void run_cpu(int cpu, uint64_t ns, uint64_t step)
{
    uint64_t end = clock_ns[cpu] + ns;
    int sel;

    lock_depth++;
    model_clock(cpu);
    select_on(cpu);
    while (clock_ns[cpu] < end) {
        clock_ns[cpu] += step;
        model_clock(cpu);
        sel = running[cpu] ? fair_tick(running[cpu]) : 0;
        if (fair_resched(cpu) || sel) {
            select_on(cpu);
        }
    }
    lock_depth--;
}

/** Put the current thread of `cpu` back, so that the rows may change the
 * threads with none current. */
static void stop_cpu(int cpu)
{
    lock_depth++;
    model_clock(cpu);
    if (running[cpu]) {
        fair_put_prev(running[cpu]);
        running[cpu] = NULL;
    }
    lock_depth--;
}

/** End every thread, as each thread's exit does, and see that every group
 * went with them. */
static void teardown(void)
{
    process_t *p;
    int c;

    for (c = 0; c < NR_CPUS; c++) {
        if (cpu_table[c].online) {
            stop_cpu(c);
        }
    }
    for (p = proc_list; p; p = p->next) {
        if (p->futex_exit_state == 0) {
            p->futex_exit_state = 2;
            p->tg->live--;
        }
        autogroup_exit(p);
    }
    CHECK(autogroup_count() == 0, "every group goes with the threads");
    CHECK(lock_depth == 0, "every take of sched_lock is let go");
}

/** Whether `a` / `b` is within 5% of `num` / `den`. */
static int ratio_near(uint64_t a, uint64_t b, uint64_t num, uint64_t den)
{
    double got = (double)a / (double)b;
    double want = (double)num / (double)den;

    return got > want * 0.95 && got < want * 1.05;
}

/**
 * Two sessions on one processor, one with one spinner and one with four,
 * as the Done's shares row: half and half with the groups on, 1:4 with
 * them off, over 10 s after 2 s of warm-up, by the time each thread was
 * charged.
 */
static void row_two_sessions(int on)
{
    uint64_t a0, b0, a, b;
    int i;

    setup(1);
    thread_new(0, 0, 0, 0);
    for (i = 1; i <= 4; i++) {
        thread_new(i, 1, 0, 0);
    }
    autogroup_setsid(&threads[0]);
    autogroup_setsid(&threads[1]);
    for (i = 0; i <= 4; i++) {
        thread_start(&threads[i], &threads[i == 0 ? 0 : 1]);
    }
    CHECK(autogroup_count() == 2, "two sessions make two groups");
    autogroup_set_enabled(on);
    run_cpu(0, 2000000000u, 100000u);
    a0 = threads[0].se.sum_exec_runtime;
    b0 = 0;
    for (i = 1; i <= 4; i++) {
        b0 += threads[i].se.sum_exec_runtime;
    }
    run_cpu(0, 10000000000ull, 100000u);
    stop_cpu(0);
    a = threads[0].se.sum_exec_runtime - a0;
    b = 0;
    for (i = 1; i <= 4; i++) {
        b += threads[i].se.sum_exec_runtime;
        CHECK(threads[i].se.sum_exec_runtime > 0, "every spinner of the larger session ran");
    }
    b -= b0;
    if (on) {
        CHECK(ratio_near(a, b, 1, 1), "groups on: the sessions share half and half");
    } else {
        CHECK(ratio_near(a, b, 1, 4), "groups off: the threads share 1:4");
    }
    CHECK(a + b > 9900000000ull && a + b <= 10000000000ull, "the processor's time is all charged");
    if (failures) {
        dprintf(2,
                "autogroup-hosttest: groups %s: one-thread session %llu ns, four-thread %llu ns\n",
                on ? "on" : "off", (unsigned long long)a, (unsigned long long)b);
    }
    teardown();
}

/**
 * setsid in a thread group of three: the running thread and a queued one
 * move into the new group's queues, with the group's entity current in the
 * root queue, and a thread that has begun to end stays in the root group.
 * A second setsid, after one of the group's threads has begun to end,
 * moves only the live one, and the first group goes at that thread's exit.
 */
static void row_setsid_threads(void)
{
    struct task_group *tg;
    struct autogroup *first;
    int i;

    setup(1);
    thread_new(0, 0, 0, 0);
    thread_new(1, 0, 0, 0);
    thread_new(2, 0, 0, 0);
    thread_new(3, 1, 0, 0);
    thread_start(&threads[0], &threads[0]);
    thread_start(&threads[1], &threads[0]);
    thread_start(&threads[2], &threads[0]);
    thread_start(&threads[3], &threads[3]);
    run_cpu(0, 20000000u, 100000u);
    for (i = 0; i < 100 && running[0] != &threads[0] && running[0] != &threads[1]; i++) {
        run_cpu(0, 100000u, 100000u);
    }
    CHECK(running[0] == &threads[0] || running[0] == &threads[1],
          "a live thread of the first process runs");
    threads[2].futex_exit_state = 2;

    kicks = 0;
    autogroup_setsid(running[0] ? running[0] : &threads[0]);
    first = groups[0].autogroup;
    CHECK(first != NULL && first->refs == 1, "setsid gives the thread group a group");
    tg = first ? first->tg : NULL;
    CHECK(tg && threads[0].sched_task_group == tg && threads[1].sched_task_group == tg,
          "both live threads join the group");
    CHECK(tg && threads[0].se.cfs_rq == tg->cfs_rq[0] && threads[1].se.cfs_rq == tg->cfs_rq[0],
          "both live threads are on the group's queue");
    CHECK(tg && threads[0].se.parent == tg->se[0] && threads[0].se.depth == 1,
          "the group's entity is their parent");
    CHECK(threads[2].sched_task_group == NULL && threads[2].se.cfs_rq == &fair_rqs[0].cfs,
          "a thread that has begun to end stays in the root group");
    CHECK(threads[3].se.cfs_rq == &fair_rqs[0].cfs, "another process stays in the root group");
    CHECK(first && first->members == 2, "the group counts its two threads");
    if (tg && running[0] && running[0]->sched_task_group == tg) {
        CHECK(tg->cfs_rq[0]->curr == &running[0]->se && fair_rqs[0].cfs.curr == tg->se[0],
              "the running thread and its group's entity are current");
    }
    CHECK(tg && tg->cfs_rq[0]->h_nr_queued == 2, "the group's queue holds both threads");
    CHECK(lock_depth == 0, "every take of sched_lock is let go");
    run_cpu(0, 20000000u, 100000u);

    stop_cpu(0);
    threads[1].futex_exit_state = 2;
    autogroup_setsid(&threads[0]);
    CHECK(groups[0].autogroup != first, "a second setsid makes another group");
    CHECK(threads[0].sched_task_group != tg, "the live thread moves to the new group");
    CHECK(threads[1].sched_task_group == tg, "a thread that has begun to end keeps its group");
    CHECK(autogroup_count() == 2, "the old group waits for the thread still in it");
    groups[0].live--;
    autogroup_exit(&threads[2]);
    CHECK(autogroup_count() == 2, "an ended thread outside the old group frees nothing");
    groups[0].live--;
    autogroup_exit(&threads[1]);
    CHECK(autogroup_count() == 1, "the old group goes with the last thread in it");
    CHECK(threads[1].se.cfs_rq == &fair_rqs[0].cfs, "the ended thread is back in the root group");
    run_cpu(0, 20000000u, 100000u);
    stop_cpu(0);
    teardown();
}

/**
 * A group's entity kept in the root queue by DELAY_DEQUEUE after its only
 * thread slept, is taken off before the group is freed: the root queue
 * holds only the other thread once the session's last thread has ended.
 */
static void row_free_after_queues(void)
{
    struct task_group *tg;
    int left;

    setup(1);
    thread_new(0, 0, 0, 0);
    thread_new(1, 1, 0, 0);
    autogroup_setsid(&threads[0]);
    thread_start(&threads[0], &threads[0]);
    thread_start(&threads[1], &threads[1]);
    tg = threads[0].sched_task_group;
    CHECK(tg != NULL, "the session has a group");
    lock_depth++;
    model_clock(0);
    if (running[0] != &threads[0]) {
        if (running[0]) {
            fair_put_prev(running[0]);
        }
        fair_set_next(&threads[0]);
        running[0] = &threads[0];
    }
    clock_ns[0] += 3000000u;
    model_clock(0);
    (void)fair_tick(&threads[0]);
    left = fair_dequeue(&threads[0], FAIR_DEQ_SLEEP);
    fair_put_prev(&threads[0]);
    running[0] = NULL;
    threads[0].state = PROC_BLOCKED;
    lock_depth--;
    CHECK(left == 1, "the thread alone in its group leaves the group's queue");
    CHECK(tg && tg->se[0]->sched_delayed && tg->se[0]->on_rq,
          "the group's entity, run past its share, stays delayed in the root queue");

    threads[0].futex_exit_state = 2;
    groups[0].live = 0;
    autogroup_exit(&threads[0]);
    CHECK(autogroup_count() == 0, "the group goes with the session's last thread");
    CHECK(fair_rqs[0].cfs.nr_running == 1 && fair_rqs[0].cfs.load.weight == NICE_0_LOAD,
          "the delayed group entity left the root queue before the group was freed");
    run_cpu(0, 20000000u, 100000u);
    CHECK(running[0] == &threads[1], "the other thread runs on");
    stop_cpu(0);
    teardown();
}

/** A group's nice value gives its share, and the checks of
 * /proc/PID/autogroup's write hold. */
static void row_nice(void)
{
    unsigned long id;
    int nice;

    setup(1);
    thread_new(0, 0, 0, 0);
    CHECK(autogroup_show(&threads[0], &id, &nice) == 0, "the root group shows nothing");
    autogroup_setsid(&threads[0]);
    thread_start(&threads[0], &threads[0]);
    CHECK(autogroup_show(&threads[0], &id, &nice) == 1 && nice == 0, "a new group has nice 0");
    CHECK(autogroup_nice(&threads[0], 20) == -EINVAL, "nice 20 is refused");
    CHECK(autogroup_nice(&threads[0], 5) == 0, "a privileged caller sets nice 5");
    CHECK(threads[0].sched_task_group->shares == sched_prio_to_weight[25],
          "nice 5 gives its weight as the share");
    privileged = 0;
    CHECK(autogroup_nice(&threads[0], -1) == -EPERM,
          "a caller without RLIMIT_NICE may not lower it");
    CHECK(autogroup_nice(&threads[0], 6) == -EAGAIN, "a second change within 100 ms waits");
    kernel_ticks += TICK_HZ / 10;
    CHECK(autogroup_nice(&threads[0], 6) == 0, "a change after 100 ms is taken");
    privileged = 1;
    CHECK(autogroup_show(&threads[0], &id, &nice) == 1 && nice == 6,
          "the group shows its nice value");
    groups[0].live = 0;
    threads[0].futex_exit_state = 2;
    autogroup_exit(&threads[0]);
    CHECK(autogroup_count() == 0, "the group goes with its last thread");
    teardown();
}

/** Run every row. */
int main(void)
{
    row_two_sessions(1);
    row_two_sessions(0);
    row_setsid_threads();
    row_free_after_queues();
    row_nice();
    if (!failures) {
        dprintf(1, "autogroup-hosttest: pass\n");
    }
    return failures != 0;
}
