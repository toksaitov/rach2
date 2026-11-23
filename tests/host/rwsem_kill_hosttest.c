/*
 * rwsem_kill_hosttest.c: a reader's wait for an rwsem that a writer holds,
 * on the real wait queue, ended by SIGKILL, as down_read_killable of
 * rach2/sys/proc/mutex.c makes it.
 *
 * host-deps: tests/host/compat/bkl_held.c
 * host-cflags: -DRACH2_HOST_CPU
 *
 * The test includes mutex.c and wait_queue.c whole, the latter under
 * WAIT_QUEUE_HOSTTEST, so the reader files itself on the rwsem's queue,
 * is marked blocked, killable and uninterruptible, and calls `schedule`,
 * as on the machine. `schedule` is the model's: it runs what another thread
 * does while the reader sleeps, a row's step, and returns to the reader
 * with whatever state the step left. One host thread plays both processors:
 * `host_cpu` names the one that runs, and each processor's current thread
 * is its record in `cpu_table`.
 *
 * The rows: a reader blocked behind a writer and sent SIGKILL, which marks
 * it ready as `signal_post` does through `sig_wake`, wakes, finds its
 * condition true and returns -EINTR, off the queue, running, and neither
 * killable nor uninterruptible, with the writer still holding the rwsem
 * and no reader counted, after which the writer's release frees the rwsem;
 * a reader blocked behind a writer that gives the rwsem back is woken by the
 * release's wake and holds the rwsem to read; and a reader with SIGKILL
 * pending before it would wait returns -EINTR without blocking. A wait
 * that blocks a third time ends the test with a failure, since no step
 * would wake it.
 */
#define WAIT_QUEUE_HOSTTEST

#include "proc/mutex.c"
#include "proc/wait_queue.c"

extern int printf(const char *fmt, ...);
extern void exit(int status);

unsigned int host_cpu;
struct percpu cpu_table[NR_CPUS];

static struct tgroup reader_group, writer_group;
static process_t reader, writer;
static struct rwsem rw;
static int fails;

/* The step another thread runs while the reader sleeps, the blocks the
 * reader made, and its marks at its first block. */
static void (*step)(void);
static int blocks;
static int blocked_killable, blocked_uninterruptible, blocked_queued;

/** Count a failed check and print `what`. */
static void check(int ok, const char *what)
{
    if (!ok) {
        printf("rwsem-kill-hosttest: fail: %s\n", what);
        fails++;
    }
}

void k_panic(const char *message)
{
    printf("rwsem-kill-hosttest: fail: panic: %s\n", message);
    fails++;
}

unsigned int arch_atomic_xchg(volatile unsigned int *p, unsigned int value)
{
    unsigned int was = *p;

    *p = value;
    return was;
}

unsigned int arch_atomic_cmpxchg(volatile unsigned int *p, unsigned int old, unsigned int value)
{
    unsigned int was = *p;

    if (was == old) {
        *p = value;
    }
    return was;
}

uintptr_t arch_atomic_xchg_ptr(volatile uintptr_t *p, uintptr_t value)
{
    uintptr_t was = *p;

    *p = value;
    return was;
}

uintptr_t arch_atomic_cmpxchg_ptr(volatile uintptr_t *p, uintptr_t old, uintptr_t value)
{
    uintptr_t was = *p;

    if (was == old) {
        *p = value;
    }
    return was;
}

void arch_relax(void)
{}

/** One host thread: a spinlock has nothing to wait for. */
uint32_t spin_lock_irqsave(struct spinlock *lock)
{
    (void)lock;
    return 0;
}

void spin_unlock_irqrestore(struct spinlock *lock, uint32_t flags)
{
    (void)lock;
    (void)flags;
}

/** No processor is preempted by a hypervisor. */
int arch_cpu_preempted(int cpu)
{
    (void)cpu;
    return 0;
}

/** No owner runs elsewhere for a mutex's spin, which no row takes. */
int sched_spin_on(const process_t *owner, int cpu)
{
    (void)owner;
    (void)cpu;
    return 0;
}

int sched_running_cpu(const process_t *owner)
{
    (void)owner;
    return -1;
}

int sched_spin_pinned(void)
{
    return 1;
}

/** No signal but SIGKILL, which the killable wait asks for itself. */
int signal_pending(void)
{
    return 0;
}

/** The reader sleeps: note its marks at the first block, then run the
 * row's step as the writer's processor, and return to the reader. A third
 * block ends the test with a failure, since no step would wake it. */
void schedule(void)
{
    unsigned int me = host_cpu;

    if (current_proc != &reader || reader.state != PROC_BLOCKED) {
        return;
    }
    if (++blocks == 1) {
        blocked_killable = reader.killable;
        blocked_uninterruptible = reader.uninterruptible;
        blocked_queued = rw.wq.head && rw.wq.head->proc == &reader;
    }
    if (blocks > 2 || !step) {
        printf("rwsem-kill-hosttest: fail: a wait that no step ends\n");
        exit(1);
    }
    host_cpu = 1;
    step();
    host_cpu = me;
}

/** The writer's processor posts SIGKILL to the reader's process, which
 * readies a sleeping reader, as `sig_wake` does through
 * `sched_wake_signal`. */
static void post_kill(void)
{
    reader_group.pending |= KSIG_BIT(SIGKILL);
    if (reader.state == PROC_BLOCKED) {
        reader.state = PROC_READY;
    }
}

/** The writer gives the rwsem back, which wakes the queue. */
static void writer_gives(void)
{
    up_write(&rw);
}

/** Start a row: the writer, on processor 1, holds `rw` to write, and the
 * reader, on processor 0, runs with no signal pending. */
static void row_start(void)
{
    rwsem_init(&rw, "row");
    reader.tg = &reader_group;
    writer.tg = &writer_group;
    reader.waiter.proc = &reader;
    writer.waiter.proc = &writer;
    reader.state = writer.state = PROC_RUNNING;
    reader_group.pending = 0;
    cpu_table[0].current = &reader;
    cpu_table[1].current = &writer;
    blocks = 0;
    blocked_killable = blocked_uninterruptible = blocked_queued = 0;
    host_cpu = 1;
    down_write(&rw);
    host_cpu = 0;
}

/** A reader blocked behind a writer that SIGKILL wakes returns -EINTR and
 * leaves no trace in the rwsem or its queue. */
static void row_killed(void)
{
    int rc;

    row_start();
    step = post_kill;
    rc = down_read_killable(&rw);
    check(rc == -EINTR, "a reader blocked behind a writer and sent SIGKILL returns -EINTR");
    check(blocks == 1 && blocked_killable && blocked_uninterruptible && blocked_queued,
          "after one block on the queue, killable and uninterruptible");
    check(rw.state == RWSEM_WRITER && rw.wowner == &writer,
          "the writer still holds the rwsem and no reader is counted");
    check(!rw.wq.head && reader.state == PROC_RUNNING && !reader.killable &&
              !reader.uninterruptible,
          "the reader is off the queue, running, and no longer killable or uninterruptible");
    host_cpu = 1;
    up_write(&rw);
    host_cpu = 0;
    check(rw.state == 0, "the writer's release then frees the rwsem");
}

/** A reader blocked behind a writer that gives the rwsem back holds it. */
static void row_released(void)
{
    int rc;

    row_start();
    step = writer_gives;
    rc = down_read_killable(&rw);
    check(rc == 0 && rw.state == 1u && blocks == 1,
          "a reader blocked behind a writer has the rwsem once the writer gives it back");
    check(!rw.wq.head && reader.state == PROC_RUNNING, "and is off the queue, running");
    up_read(&rw);
    check(rw.state == 0, "the reader's release frees the rwsem");
}

/** A reader with SIGKILL pending returns -EINTR without blocking. */
static void row_pending(void)
{
    int rc;

    row_start();
    step = NULL;
    reader_group.pending |= KSIG_BIT(SIGKILL);
    rc = down_read_killable(&rw);
    check(rc == -EINTR && blocks == 0 && !rw.wq.head && rw.state == RWSEM_WRITER,
          "a reader with SIGKILL pending behind a writer returns -EINTR without blocking");
    host_cpu = 1;
    up_write(&rw);
    host_cpu = 0;
}

/** Run every row; 0 when each passes, 1 otherwise. */
int main(void)
{
    row_killed();
    row_released();
    row_pending();
    if (fails) {
        printf("rwsem-kill-hosttest: result: fail: %d checks\n", fails);
        return 1;
    }
    printf("rwsem-kill-hosttest: result: pass\n");
    return 0;
}
