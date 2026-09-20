/**
 * Run the production scheduler and wait-queue wake path without a CPU switch.
 *
 * host-deps:
 *
 * Interrupt flags, floating-point saves and the stack switch are stand-ins.
 * Selection, process states, wake marks and cursors are the kernel's code.
 * These rows judge dispatch order and counts, independent of host speed.
 */
#define WAIT_QUEUE_HOSTTEST

#include "proc/sched.c"
#include "proc/wait_queue.c"

extern int dprintf(int fd, const char *format, ...);

process_t *proc_list;
process_t *current_proc;
static process_t procs[7];
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

/** Model the interrupt mask around scheduler state changes. */
uint32_t kirq_save(void)
{
    uint32_t flags = irq_flags;

    irq_flags = 0;
    return flags;
}

/** Restore the caller's interrupt mask. */
void kirq_restore(uint32_t flags)
{
    irq_flags = flags;
}

/** Check the mask at the outgoing floating-point save. */
void fpu_save(void *area)
{
    CHECK(!irq_flags, "floating-point save with interrupts on");
    CHECK(area == current_proc->fpu, "save belongs to outgoing process");
}

/** Check the mask at the incoming floating-point restore. */
void fpu_restore(const void *area)
{
    CHECK(!irq_flags, "floating-point restore with interrupts on");
    CHECK(area != current_proc->fpu, "restore belongs to incoming process");
}

/** Record a dispatch; the host remains on the test's stack. */
void switch_context(process_t *old, process_t *next)
{
    CHECK(!irq_flags, "switch with interrupts on");
    CHECK(old->state != PROC_RUNNING, "outgoing process still running");
    CHECK(next == current_proc && next->state == PROC_RUNNING, "incoming state");
    CHECK(!next->woken, "wake mark survived selection");
    switches++;
}

/** No signal is pending in the wait-return row. */
int signal_pending(void)
{
    return 0;
}

/** These rows deliver no loopback traffic. */
int ip_loop_pending(void)
{
    return 0;
}

/** An idle network drain is not needed by a selection row. */
void ip_loop_drain_from(int from)
{
    (void)from;
}

/** Reset seven records with process zero running and the rest ready. */
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
    }
    proc_list = procs;
    current_proc = procs;
    current_proc->state = PROC_RUNNING;
    switches = 0;
    irq_flags = 0x200;
    init_sched();
    sched_enable();
}

/** Wake through the same queue operation used by device completions. */
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

/** A late-list sleeper precedes the next spinner only once. */
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

/** Two repeated wakeups cannot postpone a spinner beyond two rotations. */
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

/** A true condition returns without leaving a wake for an unrelated yield. */
static int ready(void *arg)
{
    (void)arg;
    return 1;
}

/** Cover wait return, ineligible states and a current sleeper waking idle. */
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

/** A removed cursor and syscall deferral preserve ordinary selection. */
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

/** Run the bounded scheduling rows with the production selection code. */
int main(void)
{
    priority();
    fairness();
    states();
    lifecycle();
    if (failures) {
        return 1;
    }
    dprintf(1, "sched-hosttest: result: pass: priority, fairness, states, lifecycle\n");
    return 0;
}
