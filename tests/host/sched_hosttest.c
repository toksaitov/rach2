/**
 * Host test of the scheduler's selection and the wait queue's wakeups,
 * proc/sched.c and proc/wait_queue.c, with no switch of the processor.
 *
 * host-deps:
 *
 * The interrupt flag, the floating-point saves and the switch of stacks are
 * the test's; the selection, the process states, the wake marks and the
 * scheduler's cursors are the kernel's. The rows judge the order of
 * dispatch and the counts, never the host's speed.
 */
#define WAIT_QUEUE_HOSTTEST

#include "proc/sched.c"
#include "proc/wait_queue.c"

extern int dprintf(int fd, const char *format, ...);

process_t *proc_list;
struct percpu cpu_table[NR_CPUS];
process_t *proc_idle;
volatile uint32_t kernel_ticks;
static process_t procs[7];
/* Each process's floating-point area, which the stand-ins tell apart. */
static uint8_t areas[7][16];
static uint32_t irq_flags;
static unsigned switches;
static int failures;

#define CHECK(test, message)                                     \
    do {                                                         \
        if (!(test)) {                                           \
            dprintf(2, "sched-hosttest: failed: %s\n", message); \
            failures++;                                          \
        }                                                        \
    } while (0)

/** Clear the model's interrupt flag and return what it was. */
uint32_t kirq_save(void)
{
    uint32_t flags = irq_flags;

    irq_flags = 0;
    return flags;
}

/** Set the model's interrupt flag to `flags`. */
void kirq_restore(uint32_t flags)
{
    irq_flags = flags;
}

/** Nothing: no row waits for an interrupt. */
void arch_idle(void)
{}

/** Nothing: no row waits for an interrupt. */
void arch_idle_masked(void)
{}

/** Nothing: no process arms a timer of processor time, which would post a
 * signal. */
void signal_post(process_t *target, int signo)
{
    (void)target;
    (void)signo;
}

/** 1: no suspend to idle holds a process. */
int suspend_lets_run(const struct process *p)
{
    (void)p;
    return 1;
}

/** Nothing: no process has a POSIX timer on a clock of processor time. */
void ptimer_cpu_tick(const struct process *p, int user)
{
    (void)p;
    (void)user;
}

/** Check that interrupts are off and that `area` is the outgoing
 * process's. */
void fpu_save(void *area)
{
    CHECK(!irq_flags, "floating-point save with interrupts on");
    CHECK(area == current_proc->fpu, "save belongs to outgoing process");
}

/** Check that interrupts are off and that `area` is not the outgoing
 * process's. */
void fpu_restore(const void *area)
{
    CHECK(!irq_flags, "floating-point restore with interrupts on");
    CHECK(area != current_proc->fpu, "restore belongs to incoming process");
}

/** Check that interrupts are off; no row sets a debug register. */
void arch_debug_switch(struct process *old, const struct process *next)
{
    (void)old;
    (void)next;
    CHECK(!irq_flags, "debug register switch with interrupts on");
}

/** Check the states of a dispatch from `old` to `next` and count it; the
 * test stays on its own stack. */
void switch_context(process_t *old, process_t *next)
{
    CHECK(!irq_flags, "switch with interrupts on");
    CHECK(old->state != PROC_RUNNING, "outgoing process still running");
    CHECK(next == current_proc && next->state == PROC_RUNNING, "incoming state");
    CHECK(!next->woken, "wake mark survived selection");
    switches++;
}

/** One processor: the run queues' lock is the interrupt flag alone. */
uint32_t spin_lock_irqsave(struct spinlock *lock)
{
    (void)lock;
    return kirq_save();
}

/** Restore the interrupt flag to `flags`. */
void spin_unlock_irqrestore(struct spinlock *lock, uint32_t flags)
{
    (void)lock;
    kirq_restore(flags);
}

/** Nothing: one processor needs no lock. */
void spin_lock(struct spinlock *lock)
{
    (void)lock;
}

/** Nothing: one processor needs no lock. */
void spin_unlock(struct spinlock *lock)
{
    (void)lock;
}

/** 0: there is no big lock to let go around an idle wait. */
int bkl_drop(void)
{
    return 0;
}

/** Nothing: there is no big lock. */
void bkl_take(int depth)
{
    (void)depth;
}

/** Nothing: there is no other processor to wake. */
void smp_kick(int cpu)
{
    (void)cpu;
}

/** Nothing: no row runs the idle task's loop. */
void arch_irq_disable(void)
{}

/** 0: no signal is pending. */
int signal_pending(void)
{
    return 0;
}

/** 0: no loopback packet is pending. */
int ip_loop_pending(void)
{
    return 0;
}

/** Nothing: no loopback packet is pending. */
void ip_loop_drain_from(int from)
{
    (void)from;
}

/** Reset seven processes, 0 running and the rest ready, on one list, with
 * interrupts on and the scheduler started. */
static void setup(void)
{
    unsigned i;
    unsigned char *bytes = (unsigned char *)procs;

    for (i = 0; i < sizeof(procs); i++) {
        bytes[i] = 0;
    }
    for (i = 0; i < 7; i++) {
        procs[i].pid = i;
        procs[i].state = PROC_READY;
        procs[i].prev = i ? &procs[i - 1] : NULL;
        procs[i].next = i < 6 ? &procs[i + 1] : NULL;
        procs[i].waiter.proc = &procs[i];
        procs[i].affinity = ~(uint64_t)0;
        procs[i].fpu = areas[i];
    }
    cpu_table[0].online = 1;
    proc_list = procs;
    current_proc = procs;
    current_proc->state = PROC_RUNNING;
    switches = 0;
    irq_flags = 0x200;
    init_sched();
    sched_enable();
}

/** Block `p` on a queue and wake it with `wq_wake_all`, as a device's
 * completion does; check that it is ready, marked woken, and off the
 * queue. */
static void wake(process_t *p)
{
    wait_queue_t queue;

    wait_queue_init(&queue);
    p->state = PROC_BLOCKED;
    wq_poll_add(&queue, &p->waiter);
    wq_wake_all(&queue);
    CHECK(p->state == PROC_READY && p->woken, "blocked waiter receives preference");
    CHECK(!queue.head, "wake empties queue");
}

/** Without wakes the selection rotates through the list. A woken process
 * is chosen ahead of the next in turn once; the selection after it is
 * ordinary, and a second woken process waits for the one after that. */
static void priority(void)
{
    unsigned i;

    setup();
    for (i = 1; i <= 14; i++) {
        schedule();
        CHECK(current_proc == &procs[i % 7], "ordinary rotation without wakes");
    }
    setup();
    schedule();
    CHECK(current_proc == &procs[1], "ordinary first selection");
    wake(&procs[6]);
    schedule();
    CHECK(current_proc == &procs[6], "woken sleeper precedes spinner");
    wake(&procs[5]);
    schedule();
    CHECK(current_proc == &procs[2], "ordinary turn follows preferred wake");
    schedule();
    CHECK(current_proc == &procs[5], "pending wake follows ordinary turn");
    CHECK(irq_flags == 0x200, "enabled interrupts restored");
}

/** Two processes woken before every selection never keep one of the other
 * five from running for fourteen selections, two rotations. */
static void fairness(void)
{
    unsigned step, i;
    unsigned last[5] = {0, 0, 0, 0, 0};

    setup();
    for (step = 1; step <= 200; step++) {
        wake(&procs[5]);
        wake(&procs[6]);
        schedule();
        if (current_proc->pid < 5) {
            last[current_proc->pid] = step;
        }
        for (i = 0; i < 5; i++) {
            if (step - last[i] >= 14) {
                CHECK(0, "spinner missed fourteen dispatches");
                return;
            }
        }
    }
}

/** A condition that is always true. */
static int ready(void *arg)
{
    (void)arg;
    return 1;
}

/**
 * A wait whose condition is true returns and clears the wake mark; a
 * blocked, stopped or dead process is not selected, whatever its mark; the
 * running process keeps no mark through a selection; with no process ready
 * nothing is switched and interrupts stay off; and a current process woken
 * after it blocked runs again with no switch.
 */
static void states(void)
{
    wait_queue_t queue;
    unsigned i;

    setup();
    wait_queue_init(&queue);
    current_proc->woken = 1;
    CHECK(wq_wait_event(&queue, ready, NULL) == 0, "ready wait returns");
    CHECK(!current_proc->woken, "wait return consumes wake mark");
    current_proc->woken = 1;
    procs[1].state = PROC_BLOCKED;
    procs[2].state = PROC_STOPPED;
    procs[3].state = PROC_ZOMBIE;
    for (i = 1; i < 4; i++) {
        procs[i].woken = 1;
    }
    schedule();
    CHECK(current_proc == &procs[4], "only ready records are selected");
    CHECK(!procs[0].woken, "running process cannot keep an idle wake");
    for (i = 0; i < 7; i++) {
        procs[i].state = PROC_BLOCKED;
    }
    switches = 0;
    irq_flags = 0;
    schedule();
    CHECK(!switches && !irq_flags, "empty ready list preserves disabled interrupts");
    wake(current_proc);
    schedule();
    CHECK(!switches && !current_proc->woken, "current sleeper consumes wake without switch");
    CHECK(current_proc->state == PROC_RUNNING, "current sleeper runs after wake");
}

/**
 * A stopped scheduler selects nothing. In a system call a preemption only
 * marks the process, and the exit from the call takes it with interrupts
 * still off. A cursor whose process was removed resumes at its successor.
 * A selection over an empty list restores the interrupt flag, and one with
 * no current process returns.
 */
static void lifecycle(void)
{
    setup();
    sched_disable();
    schedule();
    CHECK(!switches, "disabled scheduler does nothing");
    sched_enable();
    current_proc->in_syscall = 1;
    wake(&procs[6]);
    sched_preempt();
    CHECK(!switches && current_proc->need_resched, "syscall defers preemption");
    irq_flags = 0;
    sched_take_preempt();
    CHECK(current_proc == &procs[6] && !procs[0].need_resched, "syscall exit takes wake");
    CHECK(!irq_flags, "syscall exit keeps interrupts off");
    setup();
    schedule();
    sched_forget(&procs[1]);
    procs[0].next = &procs[2];
    procs[2].prev = &procs[0];
    current_proc = &procs[0];
    current_proc->state = PROC_RUNNING;
    schedule();
    CHECK(current_proc == &procs[2], "removed cursor resumes at successor");
    init_sched();
    sched_enable();
    proc_list = NULL;
    schedule();
    CHECK(irq_flags == 0x200, "empty process list restores interrupts");
    current_proc = NULL;
    schedule();
}

/**
 * A processor's tick counts the global ticks it has not counted yet: three
 * after two were lost, none when its own tick comes before the next global
 * one, and one, charged as user time, when the processor runs ring 3.
 */
static void accounting(void)
{
    struct percpu *cpu = this_cpu();
    process_t *saved = cpu->current;
    uint32_t idle = cpu->idle_ticks, busy = cpu->busy_ticks, user = cpu->user_ticks;

    cpu->current = NULL;
    cpu->idle_wait = 1;
    kernel_ticks = cpu->accounted_tick + 3;
    sched_account_tick(0);
    CHECK(cpu->idle_ticks == idle + 3 && cpu->busy_ticks == busy,
          "three global ticks since the processor's last count are three idle ticks");
    sched_account_tick(0);
    CHECK(cpu->idle_ticks == idle + 3, "a tick before the next global tick counts none");
    cpu->idle_wait = 0;
    kernel_ticks++;
    sched_account_tick(1);
    CHECK(cpu->busy_ticks == busy + 1 && cpu->user_ticks == user + 1 && cpu->idle_ticks == idle + 3,
          "the next global tick in ring 3 is one busy tick of user time");
    cpu->current = saved;
}

/** Run every row; 0 when each passes, 1 otherwise. */
int main(void)
{
    priority();
    fairness();
    states();
    lifecycle();
    accounting();
    if (failures) {
        return 1;
    }
    dprintf(1, "sched-hosttest: result: pass: priority, fairness, states, lifecycle, accounting\n");
    return 0;
}

/** Nothing: no row records USB input. */
void usb_stamp_dispatch(uint32_t pid)
{
    (void)pid;
}
