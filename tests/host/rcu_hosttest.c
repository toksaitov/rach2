/*
 * rcu_hosttest.c: the grace periods and callbacks of rach2/sys/lib/rcu.c:
 * a grace period held by a processor in a read section, the order in which
 * callbacks run, an idle processor and one that goes offline.
 *
 * host-deps: tests/host/compat/spinlock_host.c tests/host/compat/kirq_none.c
 * host-deps: tests/host/compat/atomic_host.c
 * host-cflags: -DRACH2_HOST_CPU
 *
 * One host thread plays every processor: `host_cpu` is the one that runs,
 * and each row moves it to make that processor's calls. The rows drive the
 * grace-period thread's steps (`rcu_gp_begin`, `rcu_gp_ended`,
 * `rcu_gp_finish`) themselves.
 */

#include "lib/rcu.c"

extern int printf(const char *fmt, ...);

unsigned int host_cpu;
struct percpu cpu_table[NR_CPUS];

static int fails;

/* What the callbacks ran, in order: each record's tag. */
static char ran[16];
static int nran;

struct tagged {
    struct rcu_head head; /* first, so the callback's head is the record */
    char tag;
};

/** Count a failed check and print `what`. */
static void check(int ok, const char *what)
{
    if (!ok) {
        printf("rcu_hosttest: fail: %s\n", what);
        fails++;
    }
}

/** The model's panic: none of these rows reaches one. */
void k_panic(const char *message)
{
    printf("rcu_hosttest: panic: %s\n", message);
    fails++;
    for (;;) {
    }
}

/** A callback: note the record's tag. */
static void note(struct rcu_head *head)
{
    if (nran < (int)sizeof(ran) - 1) {
        ran[nran++] = ((struct tagged *)head)->tag;
        ran[nran] = 0;
    }
}

/** Queue `t`, tagged `tag`, on processor `cpu`. */
static void queue_on(int cpu, struct tagged *t, char tag)
{
    host_cpu = (unsigned int)cpu;
    t->tag = tag;
    call_rcu(&t->head, note);
    host_cpu = 0;
}

/** A quiescent state of processor `cpu`, as its `schedule` makes one. */
static void qs_on(int cpu)
{
    host_cpu = (unsigned int)cpu;
    rcu_qs();
    host_cpu = 0;
}

/** Whether `s` is what the callbacks ran. */
static int ran_is(const char *s)
{
    int i;

    for (i = 0; s[i] && ran[i] == s[i]; i++) {
    }
    return s[i] == 0 && ran[i] == 0;
}

/** Bring four processors online, none in an idle wait, and forget what
 * ran. */
static void setup(void)
{
    int c;

    for (c = 0; c < 4; c++) {
        cpu_table[c].online = 1;
    }
    nran = 0;
    ran[0] = 0;
}

/**
 * Processor 2 is inside a read section from before the grace period
 * begins: the others pass quiescent states, its tick comes in the kernel,
 * and the grace period waits for it, so the callback does not run. Once its
 * section ends and it switches, the grace period ends and the callback
 * runs. A tick in user mode is a quiescent state too.
 */
static void reader_rows(void)
{
    static struct tagged a;
    uint32_t done;

    setup();
    queue_on(1, &a, 'a');
    check(rcu_gp_begin() == 1, "a callback begins a grace period");
    qs_on(0);
    qs_on(1);
    qs_on(3);
    host_cpu = 2;
    rcu_read_lock();
    rcu_tick(0);
    host_cpu = 0;
    check(!rcu_gp_ended(), "a reader holds the grace period");
    check(nran == 0, "no callback runs while a reader may use what it frees");
    host_cpu = 2;
    rcu_read_unlock();
    rcu_tick(1);
    host_cpu = 0;
    check(rcu_gp_ended(), "a tick in user mode ends the reader's hold");
    done = rcu_gp_completed();
    rcu_gp_finish();
    check(ran_is("a") && rcu_gp_completed() == done + 1, "the callback runs at the end");
    check(rcu_gp_begin() == 0, "no grace period begins with nothing queued");
}

/**
 * Callbacks queued on one processor run in their order, and one queued
 * after a grace period began waits for the next, though every processor
 * has passed a quiescent state since it was queued.
 */
static void order_rows(void)
{
    static struct tagged a, b, c, d;
    int i;

    setup();
    queue_on(0, &a, 'a');
    queue_on(0, &b, 'b');
    queue_on(1, &c, 'c');
    check(rcu_gp_begin() == 1, "the first grace period begins");
    queue_on(0, &d, 'd');
    for (i = 0; i < 4; i++) {
        qs_on(i);
    }
    check(rcu_gp_ended(), "every processor passed a quiescent state");
    rcu_gp_finish();
    check(ran_is("abc"), "the first grace period runs a, b, then c");
    check(rcu_gp_begin() == 1, "the late callback begins a second grace period");
    check(!rcu_gp_ended(), "the second grace period waits again");
    for (i = 0; i < 4; i++) {
        qs_on(i);
    }
    check(rcu_gp_ended(), "the second grace period ends");
    rcu_gp_finish();
    check(ran_is("abcd"), "the late callback runs at the second's end");
}

/**
 * A processor in an idle wait when the grace period begins is not waited
 * for, nor is one that enters one after; one that leaves one and runs
 * without a quiescent state is. A processor that goes offline is waited
 * for no more, and a callback queued on it before still runs.
 */
static void idle_offline_rows(void)
{
    static struct tagged a, b;

    setup();
    host_cpu = 3;
    rcu_idle_enter();
    host_cpu = 0;
    queue_on(2, &a, 'a');
    check(rcu_gp_begin() == 1, "the grace period begins");
    qs_on(0);
    host_cpu = 1;
    rcu_idle_enter();
    host_cpu = 0;
    check(!rcu_gp_ended(), "processor 2 holds the grace period");
    host_cpu = 3;
    rcu_idle_exit();
    host_cpu = 0;
    cpu_table[2].online = 0;
    check(rcu_gp_ended(), "the idle and the offline processors are not waited for");
    rcu_gp_finish();
    check(ran_is("a"), "the offline processor's callback runs");
    host_cpu = 1;
    rcu_idle_exit();
    host_cpu = 0;
    queue_on(0, &b, 'b');
    check(rcu_gp_begin() == 1, "a grace period begins with processor 3 out of idle");
    qs_on(0);
    qs_on(1);
    check(!rcu_gp_ended(), "a processor out of its idle wait is waited for");
    qs_on(3);
    check(rcu_gp_ended(), "its quiescent state ends the grace period");
    rcu_gp_finish();
    check(ran_is("ab"), "the second callback runs");
}

int main(void)
{
    rcu_init();
    reader_rows();
    order_rows();
    idle_offline_rows();
    if (fails) {
        return 1;
    }
    printf("rcu_hosttest: ok\n");
    return 0;
}
