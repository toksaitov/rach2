/**
 * proc_ref_hosttest.c: the lookup of a record by its id without a lock
 * (`get_process_ref`), the record's references (`proc_ref_get`,
 * `proc_ref_put`) and the free a grace period after the last of them, of
 * rach2/sys/proc/process.c, against a reap that runs at once on another
 * thread.
 *
 * host-deps: rach2/sys/lib/kstring.c rach2/sys/lib/kctypes.c
 * host-deps: tests/host/compat/spinlock_host.c
 * host-deps: tests/host/compat/host_threads.c
 * host-cflags: -Wno-int-to-pointer-cast -Wno-pointer-to-int-cast
 * host-cflags: -pthread
 *
 * The functions `process.c` exports that the rows do not call are made
 * private before the source is included, so at -O1 the compiler discards
 * those that nothing under test calls. The heap is the host's, counted, and
 * the sanitizer the host suite builds with stops the test at a read of
 * freed memory. A record's descriptor table and the rest of what the rows
 * do not look at are stubs.
 *
 * `call_rcu` is the test's model of a grace period: a callback runs only
 * once the reader thread, the one processor here that reads without a lock,
 * has passed a quiescent state since the callback was queued, which it does
 * between two lookups and never inside one; while no reader runs, the next
 * `grace` runs every callback queued.
 *
 * The rows: a lookup finds a record by its id, takes a reference and finds
 * no record for an id nobody has; a record reaped while a reference is held
 * leaves the index and the list at once, so neither lookup finds it, but
 * its memory and its thread group stay until the reference goes and a grace
 * period passes after it; a record found in the index whose count has
 * reached 0 is not taken; a growth of the pid index keeps every record
 * findable and frees the old index only after a grace period; an exchange
 * of ids at exec gives the lookup of each id the other record; a lookup
 * that finds nothing while a move of records between chains is under way
 * walks again once it ends, and finds the record. Then a reader thread
 * looks up, reads and lets go of records while the main thread makes,
 * reaps, exchanges and grows them, and the records the reader may find
 * stay readable through each of its references, while those the main
 * thread keeps are always found.
 */
#include "lib/kint.h"
#include "compat/waketime_none.h"
struct process;
struct tgroup;
struct k_siginfo;
static uint32_t tasklist_read_lock(void);
static void tasklist_read_unlock(uint32_t flags);
static uint32_t proc_work_count(unsigned int index);
static void proc_set_parent(struct process *p, uint32_t ppid);
static void proc_set_tracer(struct process *p, uint32_t tgid);
static struct process *proc_children(uint32_t ppid);
static struct process *proc_tracees(uint32_t tgid);
static void proc_set_group(struct process *p, uint32_t sid, uint32_t pgid);
static uint32_t proc_user_records(const struct process *p);
static void proc_tick_signal(struct process *p, int signo);
static int proc_tick_signals_due(void);
static void proc_alarm_set(struct process *p, uint64_t tick, uint64_t interval);
static void proc_alarm_set_ns(struct process *p, uint64_t ns, uint64_t interval);
static uint64_t proc_alarm_read_ns(const struct process *p, uint64_t *interval);
static uint64_t proc_alarm_due_tick(const struct process *p);
static void proc_alarm_take(struct process *to, const struct process *from);
static void proc_alarm_taken(struct process *p);
static void proc_tick_signals_post(void);
static void proc_tick_signals_own(void);
static int proc_in_syscall(void);
static uint32_t proc_last_pid(void);
static void proc_set_last_pid(uint32_t pid);
static void init_proc(void);
static uint32_t process_forks_since_boot(void);
static uint32_t process_sleeps_cut_short(void);
static void process_count_sleep_cut_short(void);
static void proc_reclaim_boot_pids(void);
static struct process *alloc_process_fork(const struct process *parent, int thread, int share_fs,
                                          int32_t *error);
static void proc_wake_sleepers(uint64_t now);
static void proc_wake_due(void);
static int64_t proc_wait_set(uint64_t tick, int64_t ns, uint32_t steps, int fresh);
static uint64_t proc_alarm_at(uint64_t now, uint64_t ticks);
static void proc_cpu_time(const struct process *p, uint64_t *user, uint64_t *sys);
static void thread_cpu_time(const struct process *t, uint64_t *user, uint64_t *sys);
static void tgroup_cpu_time_locked(const struct tgroup *tg, uint64_t *user, uint64_t *sys);
static void tgroup_cpu_time(const struct tgroup *tg, uint64_t *user, uint64_t *sys);
static void proc_faults(const struct process *p, unsigned long *minor, unsigned long *major);
static void proc_switches(const struct process *p, unsigned long *voluntary,
                          unsigned long *involuntary);
static void proc_thread_fold(struct process *t);
static int proc_tid_gone(const struct process *t);
static int proc_is_kthread(const struct process *p);
static void reparent_children(uint32_t dead_pid);
static void proc_set_comm(struct process *p, const char *path);
static void setup_process_stack(struct process *proc, void (*entry_point)(void));
static void proc_child_info(const struct process *child, const struct process *receiver,
                            struct k_siginfo *info);
#include "proc/process.c"
#include "ns_first.h"
#include "host_threads.h"

extern void *calloc(unsigned long count, unsigned long size);
extern void free(void *ptr);
extern int printf(const char *format, ...);

volatile uint32_t init_pid;
volatile uint64_t kernel_ticks;

static int failures;

/** Count a failed check and say which. */
static void check(int ok, const char *what)
{
    if (!ok) {
        printf("proc-ref-test: fail: %s\n", what);
        __sync_fetch_and_add(&failures, 1);
    }
}

/* The blocks the heap has given and not taken back, and the last block
 * taken back. */
static volatile int allocations;
static void *volatile last_freed;

/** Zeroed host memory, counted. */
void *kzalloc(size_t size)
{
    void *ptr = calloc(1, size);

    if (ptr) {
        __sync_fetch_and_add(&allocations, 1);
    }
    return ptr;
}

/** `kzalloc`: the record's fields are all set by `process_alloc`. */
void *kmalloc(size_t size)
{
    return kzalloc(size);
}

/** Take back a block, counted. */
void kfree(void *ptr)
{
    if (ptr) {
        __sync_fetch_and_sub(&allocations, 1);
        last_freed = ptr;
    }
    free(ptr);
}

/** The floating-point area, from the counted heap. */
void *fpu_area_alloc(void)
{
    return kzalloc(512);
}

/** Take back the floating-point area. */
void fpu_area_free(void *area)
{
    kfree(area);
}

/* The one descriptor table every record of the rows joins; none of them
 * reads it. */
static fd_table_t table;

/** The table every standalone record joins. */
fd_table_t *fd_table_new(void)
{
    return &table;
}

/** Join `p` to `t`. */
void fd_table_join(fd_table_t *t, struct process *p)
{
    p->fds = t;
}

/** No standard descriptions in the model. */
int fd_init_stdio(fd_table_t *t, uint32_t hangups)
{
    (void)t;
    (void)hangups;
    return 0;
}

/** A failed allocation would release the descriptions; none fails here. */
int fd_free(fd_table_t *t, int fd)
{
    (void)t;
    (void)fd;
    return 0;
}

/** The shared table is never the last record's. */
int fd_table_put(struct process *p)
{
    (void)p;
    return 0;
}

/** Never reached: `fd_table_put` keeps the table. */
void fd_table_delete(fd_table_t *t)
{
    (void)t;
}

/** No console in the model. */
uint32_t tty_console_vhangups(void)
{
    return 0;
}

/** The tick count a new record starts at. */
uint64_t tick_count(void)
{
    return kernel_ticks;
}

/** No interrupt between ticks to arm. */
void timer_wake_at(int64_t ns)
{
    (void)ns;
}

/** No tick to arm. */
void timer_wake_tick(uint64_t tick)
{
    (void)tick;
}

/** No queue of the model has a waiter. */
void wait_queue_init(wait_queue_t *wq)
{
    (void)wq;
}

/** No signalfd is registered on a group's queue. */
void wq_wake_free(wait_queue_t *wq)
{
    (void)wq;
}

/** No pidfd names a record. */
void pidfd_id_gone(struct process *p)
{
    (void)p;
}

/** The records never reach a run queue. */
void sched_forget(struct process *p)
{
    (void)p;
}

/** No seccomp filter. */
void seccomp_release(struct process *p)
{
    (void)p;
}

/** No path pages. */
void path_bufs_free(struct process *p)
{
    (void)p;
}

/** No TLB shootdown reads a record here. */
void smp_record_quiesce(void)
{}

/** No interrupt to mask in the model. */
uint32_t kirq_save(void)
{
    return 0;
}

/** Nothing to restore. */
void kirq_restore(uint32_t flags)
{
    (void)flags;
}

/* The model's grace periods: the callbacks queued and not yet run, pushed
 * by either thread; the reader's count of quiescent states; and whether a
 * reader runs. */
static struct rcu_head *volatile rcu_queued;
static volatile unsigned long reader_qs;
static volatile int reader_on;

/** Queue `head` for `func` after a grace period, from either thread. */
void call_rcu(struct rcu_head *head, void (*func)(struct rcu_head *head))
{
    struct rcu_head *old;

    head->func = func;
    do {
        old = rcu_queued;
        head->next = old;
    } while (!__sync_bool_compare_and_swap(&rcu_queued, old, head));
}

/** Run the callbacks queued so far once a grace period has passed after
 * them: at once while no reader runs, or once the reader has passed a
 * quiescent state since they were taken. The number run. */
static int grace(void)
{
    struct rcu_head *list = __sync_lock_test_and_set(&rcu_queued, (struct rcu_head *)0);
    unsigned long seen;
    struct rcu_head *next;
    int n = 0;

    __sync_synchronize();
    seen = reader_qs;
    while (reader_on && reader_qs == seen) {
    }
    for (; list; list = next) {
        next = list->next;
        list->func(list);
        n++;
    }
    return n;
}

/** Whether no callback waits. */
static int none_queued(void)
{
    return rcu_queued == 0;
}

/** Make a standalone record, and fail the row when the heap has none. */
static process_t *make(void)
{
    process_t *p = alloc_process();

    check(p != 0, "a record is made");
    return p;
}

/** Reap `p` and run the grace periods after it. */
static void reap(process_t *p)
{
    free_process(p);
    (void)grace();
}

/** A lookup finds a record by its id and takes a reference; an id nobody
 * has finds nothing. */
static void lookup_rows(void)
{
    process_t *a = make(), *b = make(), *c = make();
    process_t *found;

    found = get_process_ref(b->pid);
    check(found == b && b->refs == 2, "the lookup finds the record and takes a reference");
    proc_ref_put(found);
    check(b->refs == 1 && none_queued(), "a put that is not the last frees nothing");
    check(get_process_ref(c->pid + 1000) == 0, "an id nobody has finds nothing");
    proc_ref_get(a);
    check(a->refs == 2, "a holder of the record takes another reference");
    proc_ref_put(a);
    reap(a);
    reap(b);
    reap(c);
}

/**
 * A record reaped while a reference is held leaves the list and the
 * indexes at once, and its memory, its floating-point area and its thread
 * group stay until the reference goes and a grace period has passed.
 */
static void reap_rows(void)
{
    process_t *keep = make();
    int before = allocations;
    process_t *p = make();
    process_t *held, *q;
    struct tgroup *tg;
    uint32_t pid;
    int found = 0;

    if (!p || !keep) {
        return;
    }
    pid = p->pid;
    tg = p->tg;
    held = get_process_ref(pid);
    check(held == p, "the lookup finds the record before the reap");
    free_process(p);
    check(get_process(pid) == 0 && get_process_ref(pid) == 0,
          "neither lookup finds a reaped record");
    for (q = proc_list; q; q = q->next) {
        found |= q == p;
    }
    check(!found && p->state == PROC_UNUSED, "the reaped record is off the list");
    check(p->refs == 1 && none_queued(), "the reference keeps the record from its free");
    (void)grace();
    check(held->tgid == pid && held->tg == tg && tg->refs == 1 && tg->leader == held &&
              held->fpu != 0,
          "the holder reads the record and its group after the reap");
    proc_ref_put(held);
    check(!none_queued() && allocations > before,
          "the last reference queues the free and frees nothing yet");
    check(grace() == 1 && allocations == before && last_freed == (void *)p,
          "a grace period frees the record, its area and its group");
    reap(keep);
}

/** A record the lookup finds in the index whose count has reached 0, as
 * one whose last reference went while the lookup walked to it, is not
 * taken. */
static void zero_rows(void)
{
    process_t *p = make();

    if (!p) {
        return;
    }
    p->refs = 0;
    check(get_process_ref(p->pid) == 0, "a record whose count is 0 is not taken");
    p->refs = 1;
    reap(p);
}

/** A growth of the pid index keeps every record findable, and frees the
 * old index only after a grace period. */
static void growth_rows(void)
{
    static process_t *many[300];
    struct pid_index *old = pid_index;
    int i, all = 1, before;

    for (i = 0; i < 300; i++) {
        many[i] = make();
        if (!many[i]) {
            return;
        }
    }
    check(pid_index != old && pid_capacity >= 512 && (pid_seq & 1u) == 0,
          "the index grew and no move is under way");
    before = allocations;
    check(!none_queued(), "the old indexes wait for a grace period");
    check(grace() >= 1 && allocations < before, "a grace period frees the old indexes");
    for (i = 0; i < 300; i++) {
        process_t *p = get_process_ref(many[i]->pid);

        all &= p == many[i];
        if (p) {
            proc_ref_put(p);
        }
    }
    check(all, "every record is found after the growth");
    for (i = 0; i < 300; i++) {
        free_process(many[i]);
    }
    (void)grace();
}

/** An exchange of ids at exec gives the lookup of each id the other
 * record, as `de_thread` gives the leader's id to the thread. */
static void exchange_rows(void)
{
    process_t *a = make(), *b = make();
    uint32_t pa, pb;
    process_t *fa, *fb;

    if (!a || !b) {
        return;
    }
    pa = a->pid;
    pb = b->pid;
    proc_exchange_pids(a, b);
    fa = get_process_ref(pa);
    fb = get_process_ref(pb);
    check(fa == b && fb == a && (pid_seq & 1u) == 0, "each id finds the other record");
    if (fa) {
        proc_ref_put(fa);
    }
    if (fb) {
        proc_ref_put(fb);
    }
    reap(a);
    reap(b);
}

/* The record the waiting lookup looks for, and what it found. */
static process_t *wanted, *waited;
static volatile int wait_started, wait_done;

/* The roles of the threads of a row, which `host_threads_run` passes. */
static int role_reader = 0, role_writer = 1;

/** The move row's threads: the reader looks `wanted` up while a move is
 * marked under way; the writer ends the move once the lookup has spun a
 * while without returning. */
static void *move_thread(void *arg)
{
    volatile long spin;

    if (arg == &role_reader) {
        wait_started = 1;
        waited = get_process_ref(wanted->pid);
        wait_done = 1;
        return 0;
    }
    while (!wait_started) {
    }
    for (spin = 0; spin < 20000000L; spin++) {
    }
    check(!wait_done, "a lookup does not end while a move is under way");
    pid_move();
    return 0;
}

/** A lookup that begins while a move of records between chains is under
 * way walks again once it ends, and finds the record. */
static void move_rows(void)
{
    void *args[2];

    args[0] = &role_reader;
    args[1] = &role_writer;
    wanted = make();
    if (!wanted) {
        return;
    }
    pid_move();
    check(host_threads_run(2, move_thread, args) == 0, "the threads of the move row run");
    check(waited == wanted && (pid_seq & 1u) == 0, "the lookup finds the record after the move");
    if (waited) {
        proc_ref_put(waited);
    }
    reap(wanted);
}

/* The concurrent row: the records the main thread keeps throughout, which
 * every lookup must find, and the ids of the records it makes and reaps,
 * which the reader looks up as it finds them. */
#define KEPT   8
#define CHURN  200
#define ROUNDS 4000
static process_t *kept[KEPT];
static uint32_t kept_pid[KEPT];
static volatile uint32_t churn_pid[CHURN];
static volatile int churn_over;
static volatile unsigned long reads, misses_kept;

/** The reader: look each kept id and each churned id up, read what a
 * holder may read, let the reference go, and pass a quiescent state between
 * two lookups, until the writer is done. */
static void reader_loop(void)
{
    unsigned long n = 0;

    while (!churn_over) {
        uint32_t pid = (n & 1) ? churn_pid[(n >> 1) % CHURN] : kept_pid[(n >> 1) % KEPT];
        process_t *p = pid != 0 ? get_process_ref(pid) : 0;

        if (p) {
            if (p->tg->serial != p->pgrp_serial || p->tgid == 0 || p->refs == 0) {
                check(0, "a held record and its group read as they were made");
            }
            reads++;
            proc_ref_put(p);
        } else if (!(n & 1)) {
            misses_kept++;
        }
        n++;
        __sync_synchronize();
        reader_qs++;
    }
    reader_qs++;
}

/** The writer: make and reap records in a ring of CHURN, exchange the ids
 * of two of them now and then, and run the grace periods as it goes, so
 * that a record the reader holds is reaped under it and the index grows
 * while it walks. */
static void writer_loop(void)
{
    static process_t *ring[CHURN];
    int r, i;

    for (r = 0; r < ROUNDS; r++) {
        i = r % CHURN;
        if (ring[i]) {
            churn_pid[i] = 0;
            free_process(ring[i]);
            ring[i] = 0;
        }
        ring[i] = make();
        if (!ring[i]) {
            break;
        }
        churn_pid[i] = ring[i]->pid;
        if (r % 7 == 0 && ring[(i + 1) % CHURN]) {
            process_t *other = ring[(i + 1) % CHURN];

            proc_exchange_pids(ring[i], other);
            churn_pid[i] = ring[i]->pid;
            churn_pid[(i + 1) % CHURN] = other->pid;
        }
        if (r % 16 == 0) {
            (void)grace();
        }
    }
    for (i = 0; i < CHURN; i++) {
        if (ring[i]) {
            free_process(ring[i]);
        }
    }
    churn_over = 1;
    reader_on = 0;
    (void)grace();
}

/** The concurrent row's threads. */
static void *churn_thread(void *arg)
{
    if (arg == &role_reader) {
        reader_loop();
    } else {
        writer_loop();
    }
    return 0;
}

/**
 * A reader looks records up and reads them while the writer makes, reaps
 * and exchanges them and the index grows: the sanitizer stops the test at
 * a read of a freed record or group, the kept records are always found,
 * and once both are done every record and index is freed but the kept
 * records'.
 */
static void churn_rows(void)
{
    void *args[2];
    int i, before;

    args[0] = &role_reader;
    args[1] = &role_writer;
    for (i = 0; i < KEPT; i++) {
        kept[i] = make();
        if (!kept[i]) {
            return;
        }
        kept_pid[i] = kept[i]->pid;
    }
    check(pid_capacity == 64, "the index has room for 64 records before the concurrent row");
    before = allocations;
    reader_on = 1;
    check(host_threads_run(2, churn_thread, args) == 0, "the threads of the concurrent row run");
    (void)grace();
    check(reads > 0 && pid_capacity == 256, "the reader held records while the index grew");
    check(misses_kept == 0, "every lookup of a kept record finds it");
    check(allocations <= before + 2, "every churned record is freed");
    for (i = 0; i < KEPT; i++) {
        reap(kept[i]);
    }
}

int main(void)
{
    lookup_rows();
    reap_rows();
    zero_rows();
    churn_rows();
    growth_rows();
    exchange_rows();
    move_rows();
    if (failures) {
        printf("proc-ref-test: %d failures\n", failures);
        return 1;
    }
    printf("proc-ref-test: ok\n");
    return 0;
}
