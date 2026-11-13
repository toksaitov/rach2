/**
 * Host test of the scheduler's selection and the wait queue's wakeups,
 * proc/sched.c and proc/wait_queue.c, with no switch of the processor.
 *
 * host-deps: rach2/sys/lib/kstring.c rach2/sys/lib/kctypes.c rach2/sys/tick.c
 * host-deps: tests/host/compat/bkl_held.c tests/host/compat/rcu_none.c
 * host-cflags: -DRACH2_HOST_CPU
 *
 * The interrupt flag, the floating-point saves and the switch of stacks are
 * the test's; the selection, the process states, the wake marks and the
 * ready lists are the kernel's, and the big lock is a depth the model keeps
 * on each processor. The rows judge the order of dispatch and the counts,
 * never the host's speed.
 */

#include "proc/sched.c"
#include "compat/waketime_none.h"
#include "proc/wait_queue.c"
#include "proc/rseq.c"

extern int dprintf(int fd, const char *format, ...);

process_t *proc_list;
struct percpu cpu_table[NR_CPUS];
process_t *proc_idle;
volatile uint64_t kernel_ticks;
unsigned int host_cpu;
extern int bkl_host_free, bkl_host_takes;

static void poll_wake_step(void);

/** Let the poll row deliver one remote wake at a modeled pause. */
void arch_relax(void)
{
    poll_wake_step();
}
static process_t procs[7];
/* Each process's floating-point area, which the stand-ins tell apart. */
static uint8_t areas[7][16];
static uint32_t irq_flags;
static unsigned switches;
static int failures;
static int idle_timer_ready, idle_timer_exits, idle_timer_enters, idle_halts;
static int idle_wake_on_enter, poll_wake, poll_tick, poll_service;
static uint32_t poll_rate;
static uint64_t poll_cycles;
static uint32_t halt_cycles;
static int halt_tick;
static int idle_hw_pending, idle_retains, service_pending;
static int64_t tick_due_model;
static int affinity_step;
static unsigned int affinity_kicks;
static uint32_t rseq_area[8];

#define CHECK(test, message)                                     \
    do {                                                         \
        if (!(test)) {                                           \
            dprintf(2, "sched-hosttest: failed: %s\n", message); \
            failures++;                                          \
        }                                                        \
    } while (0)

/* The modeled monotonic clock in nanoseconds, which the rows move. */
static uint64_t clock_ns;

/** Read the modeled monotonic clock, which only the rows move. */
void rtc_monotonic(int64_t *sec, int64_t *ns)
{
    *sec = (int64_t)(clock_ns / 1000000000u);
    *ns = (int64_t)(clock_ns % 1000000000u);
}

/** The modeled monotonic clock in nanoseconds, as clock.c reads it. */
int64_t rtc_monotonic_ns(void)
{
    return (int64_t)clock_ns;
}

/* The hypervisor's modeled steal time in nanoseconds, which the rows move. */
static uint64_t steal_clock_ns;

/** The modeled steal time, which only the rows move. */
uint64_t arch_steal_ns(void)
{
    return steal_clock_ns;
}

/* The modeled times of the processor's last return to user mode and start
 * of an idle wait, which the rows set, or 0 for long ago. */
static int64_t user_return_ns, idle_start_ns;

/** The modeled time of a change of mode, by the count the rows give its
 * field: 3 the idle wait's start, any other the return to user mode. */
int64_t timer_cycles_when(uint64_t cycles, uint64_t at, int64_t now)
{
    (void)at;
    (void)now;
    return cycles == 3u ? idle_start_ns : user_return_ns;
}

/** Timer list membership is checked by process_timer_hosttest. */
void proc_timer_update(process_t *p)
{
    (void)p;
}

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

/** Whether the model's interrupt flag is set. */
int kirq_enabled(void)
{
    return irq_flags != 0;
}

/** Nothing runs in an interrupt here. */
int in_interrupt(void)
{
    return 0;
}

/* The windows of one instruction with interrupts on that the idle waits
 * opened in place of a halt (`idle_kicked_window`): inside the idle marker,
 * and before it, where a loopback drain or ready work ends the wait before
 * the marker is set. */
static unsigned int irq_windows, irq_early_windows;
/* Nonzero while a row drives a wait that ends before the idle marker, the
 * one place a window may open unmarked. */
static int irq_early_ok;
/* An interrupt the row made pending, which the next window takes, and the
 * interrupts windows took. */
static int irq_pending;
static unsigned int irq_taken;
/* A loopback datagram waiting (`ip_loop_pending`), and the drains made. */
static int loop_pending;
static unsigned int loop_drains;
/* Nonzero to make a thread ready when the idle wait lets the big lock go,
 * after the idle marker is set: a wake from another processor there. */
static int resched_on_drop;

/** Count a halt; the interrupt that would end it is not modeled. */
void arch_idle_masked(void)
{
    idle_halts++;
    poll_cycles += halt_cycles;
    if (halt_tick) {
        tick_due_model += 4000000;
    }
}

/** Install a published target, including a kick which sent no IPI. */
void timer_service_ipi(void)
{
    if (service_pending) {
        service_pending = 0;
        idle_hw_pending = 1;
    }
}

/* The last requested event and the number of timer programming requests. */
static int64_t slice_event;
static unsigned slice_arms;

/** Record a requested event without programming a host timer. */
void timer_wake_at(int64_t ns)
{
    slice_event = ns;
    slice_arms++;
}

/** A local event timer is present in the idle prediction model. */
int timer_wakes_between(void)
{
    return 1;
}

/** This model normally has no deferred hardware change. */
int timer_idle_pending(void)
{
    return idle_hw_pending;
}

/** No hardware is programmed by the retained-tick model. */
void timer_idle_retain(void)
{
    idle_retains++;
    idle_hw_pending = 0;
}

/** The model selects one four-millisecond scheduler tick. */
uint32_t timer_tick_period(void)
{
    return 1;
}

/** Return the last modeled scheduler tick's expiry. */
int64_t timer_tick_due(void)
{
    return tick_due_model;
}

/** Nothing: the host model has no lockup detector. */
void watchdog_sched(void)
{}

/** Never inside klog here: no row logs from a thread a tick switches. */
int klog_inside(void)
{
    return 0;
}

/** 0: the model's counter is not measured, so an idle wait does not poll. */
uint32_t timer_calibrate(void)
{
    return poll_rate;
}

/** 0: the model's idle waits do not poll, the other reason they do not. */
int arch_idle_poll(void)
{
    return poll_rate != 0;
}

/** 0: no interrupt waits; only an idle wait's poll asks, which none
 * runs. */
int arch_irq_waiting(void)
{
    return 0;
}

/** 0: no row reads the cycle counter but an idle wait's poll, which none
 * runs. */
uint64_t arch_cycles(void)
{
    return poll_rate ? poll_cycles++ : 0;
}

/** Nothing: no process arms a timer of processor time, which would post a
 * signal. */
void signal_post(process_t *target, int signo)
{
    (void)target;
    (void)signo;
}

/** Nothing: no process arms a timer of processor time, whose signal the
 * tick would leave for a holder of the big lock. */
void proc_tick_signal(process_t *p, int signo)
{
    (void)p;
    (void)signo;
}

/** 1: no suspend to idle holds a process. */
int suspend_lets_run(const struct process *p)
{
    (void)p;
    return 1;
}

/** Nothing: no process has a POSIX timer on a clock of processor time. */
void ptimer_cpu_tick(struct process *p, int user)
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

/** Copy a modeled user area without faults. */
int copy_from_user(void *to, const void __user *from, size_t size)
{
    k_memcpy(to, from, size);
    return 0;
}

/** Copy an rseq update into the modeled user area. */
int copy_to_user(void __user *to, const void *from, size_t size)
{
    k_memcpy(to, from, size);
    return 0;
}

/** No valid area should force a signal. */
void signal_force(process_t *p, int signo, const struct k_siginfo *info)
{
    (void)p;
    (void)signo;
    (void)info;
    CHECK(0, "rseq forced a signal");
}

/** No row has a pending signal to deliver. */
void deliver_signals(isr_params_t *params)
{
    (void)params;
}

/** No process registers for the sync-core barrier in the host model. */
void membarrier_switch(const struct process *p)
{
    (void)p;
}

/** Check that interrupts are off; no row sets a debug register. */
void arch_debug_switch(const struct process *next)
{
    (void)next;
    CHECK(!irq_flags, "debug register switch with interrupts on");
}

/** Check the states of a dispatch from `old` to `next` and count it; the
 * test stays on its own stack. An affinity row runs what comes next on the
 * processors here, after the incoming thread's tail, which on the machine
 * runs first and lets `old` be switched to again. */
void switch_context(process_t *old, process_t *next)
{
    CHECK(!irq_flags, "switch with interrupts on");
    CHECK(old == this_cpu()->idle || old->state != PROC_RUNNING, "outgoing process still running");
    CHECK(next == current_proc && next->state == PROC_RUNNING, "incoming state");
    CHECK(!next->woken, "wake mark survived selection");
    switches++;
    if (affinity_step && old == &procs[0]) {
        int step = affinity_step;

        schedule_tail();
        affinity_step = 0;
        CHECK(old->state == PROC_BLOCKED && affinity_waits != NULL,
              "remote affinity waits for the running target");
        CHECK(procs[1].need_resched && affinity_kicks != 0,
              "remote affinity requests and kicks a switch");
        if (step == 2) {
            sched_set_affinity(&procs[1], 3);
        } else {
            host_cpu = 1;
            if (step == 3) {
                procs[1].state = PROC_ZOMBIE;
            }
            sched_take_preempt();
            CHECK(cpu_table[1].current != &procs[1], "target left the forbidden processor");
            if (step == 3) {
                sched_forget(&procs[1]);
                k_memset(&procs[1], 0, sizeof(procs[1]));
            }
            host_cpu = 0;
        }
        CHECK(affinity_waits == NULL && old->state == PROC_READY,
              "completion drops the task pointer and wakes the caller");
        schedule();
        CHECK(current_proc == old, "affinity caller resumes");
    }
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

/** Nothing: one processor needs no lock, at any nesting `subclass`. */
void spin_lock_nested(struct spinlock *lock, unsigned int subclass)
{
    (void)lock;
    (void)subclass;
}

/* The big lock's takes the model saw: how many, and the last one's key. */
static int bkl_takes, bkl_last_key;

/** Let the modelled big lock go and return the processor's depth, which is
 * 0 in every row but the big lock's. */
int bkl_drop(void)
{
    int depth = this_cpu()->bkl_depth;

    this_cpu()->bkl_depth = 0;
    if (resched_on_drop) {
        resched_on_drop = 0;
        sched_make_ready(&procs[1]);
    }
    return depth;
}

/** Nothing: there is no big lock. */
void bkl_take(int depth)
{
    (void)depth;
}

/** Nothing: there is no big lock. */
void bkl_yield(void)
{}

/** 1: there is no big lock to hold up a lock's holder. */
int bkl_spin_ok(int cpu)
{
    (void)cpu;
    return 1;
}

/** 0: no hypervisor takes a processor away. */
int arch_cpu_preempted(int cpu)
{
    (void)cpu;
    return 0;
}

/* The kicks each processor was sent, which the balancer's row reads. */
static unsigned int cpu_kicks[NR_CPUS];

/** Count a request to the remote processor, which the switch model services. */
void smp_kick(int cpu)
{
    if (cpu != (int)host_cpu && cpu_table[cpu].idle_poll == CPU_POLL_ON) {
        cpu_table[cpu].idle_poll = CPU_POLL_KICKED;
    }
    cpu_kicks[cpu]++;
    if (cpu == 1) {
        affinity_kicks++;
    }
}

/** Count each processor of the set as `smp_kick` counts one. */
void smp_kick_many(uint64_t cpus)
{
    int c;

    for (c = 0; cpus != 0; c++, cpus >>= 1) {
        if (cpus & 1u) {
            smp_kick(c);
        }
    }
}

/** Clear the model's interrupt flag: no row runs the idle task's loop,
 * and an idle wait closes its window with this. */
void arch_irq_disable(void)
{
    irq_flags = 0;
}

/** Count an idle wait's window with interrupts on, marked idle or, where
 * the row expects it (`irq_early_ok`), before the marker, take a pending
 * interrupt and set the model's flag. */
void arch_irq_enable(void)
{
    if (this_cpu()->idle_wait) {
        irq_windows++;
    } else {
        CHECK(irq_early_ok, "an interrupt window opened outside the idle marker");
        irq_early_windows++;
    }
    if (irq_pending) {
        irq_pending = 0;
        irq_taken++;
    }
    irq_flags = 0x200;
}

/** 0: no signal is pending. */
int signal_pending(void)
{
    return 0;
}

/** Whether the row left a loopback datagram waiting. */
int ip_loop_pending(void)
{
    return loop_pending;
}

/** Deliver the waiting datagram, which an idle wait does with interrupts
 * off and with the big lock, and count the drain. */
void ip_loop_drain_from(int from)
{
    CHECK(
        from == IP_LOOP_FROM_IDLE && !irq_flags && !this_cpu()->idle_wait &&
            (this_cpu()->bkl_depth > 0 || bkl_held()),
        "an idle wait drains the loopback backlog with interrupts off and the big lock, unmarked");
    loop_pending = 0;
    loop_drains++;
}

/** Reset seven processes, 0 running and the rest ready, on one list, with
 * interrupts on and the scheduler started, and no request left on 0: the
 * six are queued as if 0 had been asked already, so that their starts ask
 * it nothing (`sched_wake_preempt`). */
static void setup(void)
{
    unsigned i;
    unsigned char *bytes = (unsigned char *)procs;

    host_cpu = 0;
    k_memset(cpu_table, 0, sizeof(cpu_table));
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
    procs[0].need_resched = 1;
    for (i = 1; i < 7; i++) {
        sched_make_ready(&procs[i]);
    }
    procs[0].need_resched = 0;
    sched_enable();
}

/** Force remote affinity completion, supersession and exit, then consume
 * the moved thread's rseq notification on its new processor. */
static void affinity_rows(void)
{
    isr_params_t frame;
    int step;
    unsigned int i;

    for (step = 1; step <= 3; step++) {
        setup();
        for (i = 1; i < 7; i++) {
            sched_forget(&procs[i]);
        }
        cpu_table[0].idle = &procs[2];
        cpu_table[1].idle = &procs[3];
        cpu_table[1].current = &procs[1];
        cpu_table[1].online = 1;
        procs[0].affinity = 1;
        procs[1].state = PROC_RUNNING;
        procs[1].cpu = procs[3].cpu = 1;
        procs[2].state = procs[3].state = PROC_READY;
        k_memset(rseq_area, 0, sizeof(rseq_area));
        rseq_area[0] = rseq_area[1] = 1;
        procs[1].rseq = (uintptr_t)rseq_area;
        affinity_kicks = 0;
        affinity_step = step;
        sched_set_affinity(&procs[1], 1);
        CHECK(!affinity_step && current_proc == &procs[0] && !affinity_waits,
              "affinity returns after completion with no wait left");
        if (step == 1) {
            CHECK(procs[1].rseq_notify && (procs[1].rseq_event & RSEQ_EVENT_MIGRATE),
                  "moving a task marks rseq for the return");
            procs[0].state = PROC_BLOCKED;
            schedule();
            CHECK(current_proc == &procs[1], "moved thread runs on the allowed processor");
            k_memset(&frame, 0, sizeof(frame));
            FRAME_PC(&frame) = 0x4000;
            rseq_return(&frame);
            CHECK(rseq_area[0] == host_cpu && rseq_area[1] == host_cpu && rseq_area[5] == 0 &&
                      rseq_area[6] == host_cpu && !procs[1].rseq_notify,
                  "rseq CPU numbers and mm_cid follow the move");
        } else if (step == 2) {
            CHECK(cpu_table[1].current == &procs[1] && procs[1].affinity == 3,
                  "a newer mask allowing the current processor completes the wait");
        }
    }
    setup();
}

/** Block `p` on a queue and wake it with `wq_wake_all`, as a device's
 * completion does; check that it is ready, marked woken, and off the
 * queue. */
static void wake(process_t *p)
{
    wait_queue_t queue;

    wait_queue_init(&queue);
    sched_forget(p);
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
 * still off. A removed ready entry leaves the next entry selectable.
 * A selection over an empty list restores the interrupt flag, and one with
 * no current process returns. A kernel thread (`no_tick_switch`) keeps the
 * tick's request in its record for its next sleep or service point, and a
 * selection that keeps the current thread clears the request it answered.
 */
static void lifecycle(void)
{
    unsigned i;

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
    current_proc->no_tick_switch = 1;
    wake(&procs[6]);
    sched_preempt();
    CHECK(!switches && current_proc == &procs[0] && procs[0].need_resched,
          "kernel thread keeps the tick's request for its service point");
    for (i = 1; i < 7; i++) {
        procs[i].state = PROC_BLOCKED;
    }
    schedule();
    CHECK(!switches && current_proc == &procs[0] && !procs[0].need_resched,
          "schedule that keeps the thread clears its request");
    setup();
    schedule();
    sched_forget(&procs[1]);
    procs[0].next = &procs[2];
    procs[2].prev = &procs[0];
    current_proc = &procs[0];
    current_proc->state = PROC_RUNNING;
    schedule();
    CHECK(current_proc == &procs[2], "ready removal preserves the next entry");
    init_sched();
    sched_enable();
    proc_list = NULL;
    schedule();
    CHECK(irq_flags == 0x200, "empty process list restores interrupts");
    current_proc = NULL;
    schedule();
}

/**
 * The big lock does not cross a switch: the outgoing thread's depth is
 * saved in its record and the lock let go, a thread that holds nothing
 * runs without it, and the tail takes it back at the incoming thread's own
 * depth, counted as a switch's take. The tail clears the outgoing thread's
 * `on_cpu` and the incoming one's is set. An idle wait lets the lock go and
 * takes it back at the same depth, counted as an idle wait's take.
 */
static void big_lock(void)
{
    int i;

    setup();
    bkl_takes = 0;
    procs[0].on_cpu = 1;
    this_cpu()->bkl_depth = 2;
    schedule();
    CHECK(current_proc != &procs[0] && procs[0].bkl_depth == 2,
          "a switch saves the outgoing thread's depth");
    CHECK(this_cpu()->bkl_depth == 0 && bkl_takes == 0,
          "and a thread that holds nothing runs without the lock");
    CHECK(!procs[0].on_cpu && current_proc->on_cpu,
          "the tail clears the outgoing thread's on_cpu and the incoming one's is set");
    for (i = 0; i < 7 && current_proc != &procs[0]; i++) {
        schedule();
    }
    CHECK(current_proc == &procs[0] && this_cpu()->bkl_depth == 2 && bkl_takes == 1 &&
              bkl_last_key == BKL_KEY_SWITCH,
          "the tail takes the lock back at the incoming thread's depth, as a switch's take");
    this_cpu()->bkl_depth = 1;
    idle_wait_irq_on();
    CHECK(this_cpu()->bkl_depth == 1 && bkl_takes == 2 && bkl_last_key == BKL_KEY_IDLE,
          "an idle wait takes the lock back at its depth, as an idle wait's take");
    this_cpu()->bkl_depth = 0;
}

/**
 * A processor's tick counts the whole ticks of its own clock since its last
 * count, rounded to the nearest: three after two were lost, none a quarter
 * tick later, and one, charged as user time, when the processor runs ring 3;
 * ticks the hypervisor reports stolen count as steal before any other; and
 * a gap longer than 2^32 nanoseconds counts whole.
 */
static void accounting(void)
{
    struct percpu *cpu = this_cpu();
    process_t *saved = cpu->current;
    uint64_t idle = cpu->idle_ticks, busy = cpu->busy_ticks, user = cpu->user_ticks;
    uint64_t steal = cpu->steal_ticks, saved_clock = clock_ns;

    cpu->current = NULL;
    cpu->idle_wait = 1;
    clock_ns = 5000000000u;
    steal_clock_ns = 0;
    sched_account_start();
    clock_ns += 3u * TICK_NS;
    sched_account_tick(0, 0);
    CHECK(cpu->idle_ticks == idle + 3 && cpu->busy_ticks == busy,
          "three ticks of the processor's clock since its last count are three idle ticks");
    clock_ns += TICK_NS / 4u;
    sched_account_tick(0, 0);
    CHECK(cpu->idle_ticks == idle + 3, "a quarter tick later counts none");
    cpu->idle_wait = 0;
    clock_ns += TICK_NS;
    sched_account_tick(1, 0);
    CHECK(cpu->busy_ticks == busy + 1 && cpu->user_ticks == user + 1 && cpu->idle_ticks == idle + 3,
          "the next tick in ring 3 is one busy tick of user time");
    cpu->idle_wait = 1;
    steal_clock_ns = 2u * TICK_NS + TICK_NS / 2u;
    clock_ns += 3u * TICK_NS;
    sched_account_tick(0, 0);
    CHECK(cpu->steal_ticks == steal + 2 && cpu->idle_ticks == idle + 4 &&
              cpu->busy_ticks == busy + 1,
          "two and a half ticks stolen of three are two steal ticks and one idle");
    steal_clock_ns += TICK_NS / 2u;
    clock_ns += TICK_NS;
    sched_account_tick(0, 0);
    CHECK(cpu->steal_ticks == steal + 3 && cpu->idle_ticks == idle + 4,
          "the stolen half tick left over joins the next to make a steal tick");
    clock_ns += 5000u * (uint64_t)TICK_NS;
    sched_account_tick(0, 0);
    CHECK(cpu->idle_ticks == idle + 5004 && cpu->steal_ticks == steal + 3,
          "a gap of five seconds, past 2^32 nanoseconds, is five thousand ticks");
    cpu->current = saved;
    clock_ns = saved_clock;
}

/**
 * With the cycle account on (`rach2.cycleaccount`), a tick taken as
 * interrupts come back on charges the ticks due before then,
 * which interrupts kept off in the kernel held back, where Linux would have
 * taken them: at a return to user mode as system time, and at an idle
 * wait's halt as system time, or as idle when the stretch that held them
 * began at the end of an earlier wait; a tick whose interrupt's delay would
 * bring it after the return is not held.
 */
static void accounting_held(void)
{
    struct percpu *cpu = this_cpu();
    process_t *saved = cpu->current;
    uint64_t busy, user, idle;
    uint64_t saved_clock = clock_ns;

    sched_cycle_account = 1;
    cpu->current = NULL;
    cpu->idle_wait = 0;
    cpu->entry_cycles = 1u;
    cpu->user_cycles = 2u;
    cpu->idle_cycles = 3u;
    cpu->idle_end_cycles = 4u;
    cpu->tick_late_ns = 0;
    clock_ns = 7000000000u;
    steal_clock_ns = 0;
    sched_account_start();
    busy = cpu->busy_ticks;
    user = cpu->user_ticks;
    idle = cpu->idle_ticks;
    clock_ns += TICK_NS;
    user_return_ns = (int64_t)(clock_ns - TICK_NS / 2u);
    sched_account_tick(1, (int64_t)(clock_ns - TICK_NS / 4u));
    CHECK(cpu->busy_ticks == busy + 1 && cpu->user_ticks == user + 1,
          "a tick due after the return to user mode is user time");
    clock_ns += TICK_NS;
    user_return_ns = (int64_t)(clock_ns - TICK_NS / 8u);
    sched_account_tick(1, (int64_t)(clock_ns - TICK_NS / 4u));
    CHECK(cpu->busy_ticks == busy + 2 && cpu->user_ticks == user + 1,
          "a tick due before it, taken at the first user instruction, is system time");
    clock_ns += 2u * TICK_NS;
    user_return_ns = (int64_t)(clock_ns - TICK_NS / 2u);
    sched_account_tick(1, (int64_t)clock_ns);
    CHECK(cpu->busy_ticks == busy + 4 && cpu->user_ticks == user + 2,
          "of two ticks counted at once, the one due before the return is system time");
    clock_ns += TICK_NS;
    user_return_ns = (int64_t)clock_ns;
    sched_account_tick(1, 0);
    CHECK(cpu->busy_ticks == busy + 5 && cpu->user_ticks == user + 3,
          "with no time of the tick kept, a tick in user mode is user time");
    clock_ns += 2u * TICK_NS;
    cpu->entry_cycles = 5u;
    idle_start_ns = (int64_t)(clock_ns - TICK_NS / 2u);
    cpu->idle_wait = 1;
    sched_account_tick(0, (int64_t)clock_ns);
    CHECK(cpu->busy_ticks == busy + 6 && cpu->user_ticks == user + 3 && cpu->idle_ticks == idle + 1,
          "a halt after a call charges the tick the call held as system time, the next as idle");
    clock_ns += TICK_NS;
    cpu->entry_cycles = 1u;
    idle_start_ns = (int64_t)(clock_ns - TICK_NS / 4u);
    sched_account_tick(0, (int64_t)(clock_ns - 3u * TICK_NS / 4u));
    cpu->idle_wait = 0;
    CHECK(cpu->busy_ticks == busy + 6 && cpu->idle_ticks == idle + 2,
          "a halt after the end of an earlier wait charges the tick held there as idle");
    clock_ns += TICK_NS;
    cpu->tick_late_ns = (int64_t)(TICK_NS / 2u);
    user_return_ns = (int64_t)(clock_ns - TICK_NS / 4u);
    sched_account_tick(1, (int64_t)(clock_ns - TICK_NS / 2u));
    CHECK(cpu->busy_ticks == busy + 7 && cpu->user_ticks == user + 4,
          "a tick due before the return that its interrupt's delay brings after it is user time");
    cpu->tick_late_ns = 0;
    user_return_ns = 0;
    idle_start_ns = 0;
    cpu->idle_wait = 1;
    cpu->current = saved;
    clock_ns = saved_clock;
    sched_cycle_account = 0;
}

/**
 * The load average's sample keeps the period's phase when the call that
 * makes it comes late, as the boot processor's does after ticks that found
 * the big lock held, and a gap of three periods folds three samples.
 */
static void load_phase(void)
{
    uint32_t active, total, avg, i;
    uint64_t want;

    load_tick = (uint32_t)kernel_ticks;
    load_wait = 10;
    kernel_ticks += 12;
    sched_load_tick();
    CHECK(load_wait == LOAD_FREQ - 2, "a sample two ticks late leaves the next two ticks sooner");
    sched_task_counts(&active, &total);
    active *= LOAD_ONE;
    load_avg[0] = 3u * LOAD_ONE;
    avg = load_avg[0];
    for (i = 0; i < 3; i++) {
        want = (uint64_t)avg * load_decay[0] + (uint64_t)active * (LOAD_ONE - load_decay[0]);
        avg = (uint32_t)((want + (active >= avg ? LOAD_ONE - 1u : 0u)) >> SCHED_LOAD_SHIFT);
    }
    kernel_ticks += load_wait + 2u * LOAD_FREQ;
    sched_load_tick();
    CHECK(load_avg[0] + 2u >= avg && load_avg[0] <= avg + 2u && load_wait == LOAD_FREQ,
          "a gap of three periods folds three samples and keeps the phase");
}

/**
 * The machine is idle when every processor online is halted in an idle
 * wait: one that runs makes it busy, and one that is not online does not
 * count, whatever its marker says.
 */
static void all_idle(void)
{
    cpu_table[0].idle_wait = 1;
    cpu_table[2].idle_wait = 0;
    CHECK(sched_all_idle(), "one processor online and halted is an idle machine");
    cpu_table[1].online = 1;
    cpu_table[1].idle_wait = 0;
    CHECK(!sched_all_idle(), "a second processor that runs makes it busy");
    cpu_table[1].idle_wait = 1;
    CHECK(sched_all_idle(), "and with both halted it is idle again");
    cpu_table[0].idle_wait = 0;
    CHECK(!sched_all_idle(), "as the boot processor running makes it busy");
    cpu_table[1].online = 0;
    cpu_table[1].idle_wait = 0;
}

/** Blocked records add no selection work, and a repeated wake links once. */
static void ready_lists(void)
{
    static process_t blocked[256];
    uint32_t before, baseline;
    unsigned int i;

    setup();
    before = sched_work_count(1);
    for (i = 0; i < 140; i++) {
        schedule();
    }
    baseline = sched_work_count(1) - before;
    for (i = 0; i < 256; i++) {
        blocked[i].state = PROC_BLOCKED;
        blocked[i].prev = i ? &blocked[i - 1] : &procs[6];
        blocked[i].next = i < 255 ? &blocked[i + 1] : NULL;
    }
    procs[6].next = blocked;
    before = sched_work_count(1);
    for (i = 0; i < 140; i++) {
        schedule();
    }
    CHECK(sched_work_count(1) - before == baseline, "blocked records add no pick entries");
    sched_make_ready(&procs[3]);
    sched_make_ready(&procs[3]);
    CHECK(run_queues[0].count == 6, "repeated wake has one ready entry");
    CHECK(!current_proc->ready_linked, "current process is off the ready list");
    sched_forget(&procs[3]);
    sched_forget(&procs[3]);
    CHECK(run_queues[0].count == 5, "removal is idempotent");
}

/**
 * Of two wakes of one blocked record, the first makes it ready, marks it
 * woken and asks for an interrupt to the processor it outranks the current
 * thread of, whose `need_resched` it sets, as Linux's resched_curr
 * (kernel/sched/core.c:1094-1116), and so does an ordinary record woken
 * where an ordinary thread runs (`sched_wake_preempt`); the second,
 * finding it ready under the run queues' lock, asks for none and leaves it
 * where it is in its list,
 * ahead of a record made ready after the first, as Linux's `try_to_wake_up`
 * leaves a task whose state no longer matches (kernel/sched/core.c:4191-
 * 4193). A running or a stopped record is left as it was. A signal's wake
 * clears the deadline and marks the record woken; the tick's wakes a
 * sleeper only once its deadline is due, and marks it woken too, so that
 * the slice gives way to either.
 */
static void wake_twice(void)
{
    process_t *p = &procs[3];

    setup();
    sched_forget(p);
    sched_forget(&procs[4]);
    p->state = PROC_BLOCKED;
    procs[4].state = PROC_BLOCKED;
    p->rt_priority = 1;
    CHECK(sched_wake_blocked(p) == 0, "the first wake asks for the outranked processor");
    CHECK(current_proc->need_resched, "and asks the thread it outranks to switch");
    current_proc->need_resched = 0;
    CHECK(p->state == PROC_READY && p->woken && p->ready_linked, "the first wake queues and marks");
    CHECK(run_queues[0].count == 5 && run_queues[0].tail == p, "the first wake queues at the tail");
    CHECK(sched_wake_blocked(&procs[4]) == 0 && current_proc->need_resched,
          "an equal ordinary record asks the ordinary thread to switch, with an interrupt");
    CHECK(sched_wake_blocked(p) == -1, "the second wake asks for no interrupt");
    CHECK(run_queues[0].count == 6 && run_queues[0].tail == &procs[4],
          "the second wake leaves the record ahead of a later one");
    p->rt_priority = 0;
    CHECK(sched_wake_blocked(current_proc) == -1, "a running record asks for nothing");
    CHECK(current_proc->state == PROC_RUNNING && !current_proc->woken &&
              !current_proc->ready_linked,
          "a running record is left as it was");
    sched_forget(&procs[2]);
    procs[2].state = PROC_STOPPED;
    CHECK(sched_wake_blocked(&procs[2]) == -1, "a stopped record asks for nothing");
    CHECK(procs[2].state == PROC_STOPPED && !procs[2].woken && !procs[2].ready_linked,
          "a stopped record is left as it was");

    sched_forget(&procs[5]);
    procs[5].state = PROC_BLOCKED;
    procs[5].woken = 0;
    procs[5].wakeup_tick = 9;
    sched_wake_due(&procs[5], 8);
    CHECK(procs[5].state == PROC_BLOCKED && procs[5].wakeup_tick == 9,
          "the tick leaves a sleeper not yet due");
    sched_wake_signal(&procs[5]);
    CHECK(procs[5].state == PROC_READY && procs[5].wakeup_tick == 0 && procs[5].woken &&
              run_queues[0].tail == &procs[5],
          "a signal's wake queues the sleeper, clears its deadline and marks it");
    sched_forget(&procs[4]);
    procs[4].state = PROC_BLOCKED;
    procs[4].woken = 0;
    procs[4].wakeup_tick = 9;
    sched_wake_due(&procs[4], 9);
    CHECK(procs[4].state == PROC_READY && procs[4].wakeup_tick == 0 && procs[4].woken &&
              run_queues[0].tail == &procs[4],
          "the tick's wake queues a due sleeper, clears its deadline and marks it");
    procs[5].wakeup_tick = 9;
    sched_wake_signal(&procs[5]);
    sched_wake_due(&procs[5], 9);
    CHECK(procs[5].wakeup_tick == 9 && run_queues[0].tail == &procs[4],
          "a ready record is left in place, its deadline as it was");
}

/** Affinity moves queued work, and an idle processor steals eligible work. */
static void migration(void)
{
    unsigned int i;

    setup();
    cpu_table[1].online = 1;
    cpu_table[1].current = cpu_table[1].idle = NULL;
    for (i = 1; i < 7; i++) {
        sched_set_affinity(&procs[i], 2);
    }
    CHECK(run_queues[0].count == 0 && run_queues[1].count == 6, "affinity transfers queue counts");
    sched_set_affinity(&procs[4], 3);
    current_proc->state = PROC_BLOCKED;
    schedule();
    CHECK(current_proc == &procs[4], "idle processor steals an allowed donor entry");
    CHECK(run_queues[1].count == 5 && !procs[4].ready_linked, "steal unlinks before dispatch");
    sched_make_ready(current_proc);
    sched_make_ready(current_proc);
    CHECK(!current_proc->ready_linked, "wake before switching keeps current unqueued");
    schedule();
    CHECK(current_proc == &procs[4] && current_proc->state == PROC_RUNNING,
          "in-place wake continues when no other local process is ready");
    cpu_table[1].online = 0;
    init_sched();
}

/** Block procs[1] to procs[6] off every list and clear the request and the
 * kicks of the threads the rows run, for a row's next wake. */
static void wake_quiet(void)
{
    unsigned int i;

    for (i = 1; i < 7; i++) {
        sched_forget(&procs[i]);
        procs[i].state = PROC_BLOCKED;
        procs[i].need_resched = 0;
        procs[i].work = 0;
    }
    procs[0].need_resched = 0;
    procs[0].work = 0;
    k_memset(cpu_kicks, 0, sizeof(cpu_kicks));
}

/**
 * A wake of an ordinary thread where an ordinary thread runs asks that
 * thread to switch and kicks its processor, as Linux's
 * check_preempt_wakeup_fair calls resched_curr (kernel/sched/fair.c:
 * 8862-8951), once: a second wake before the switch sends no second kick.
 * A SCHED_BATCH or SCHED_IDLE thread preempts nothing; a SCHED_IDLE thread
 * gives way to an ordinary one and not to another SCHED_IDLE one
 * (fair.c:8911-8925); and a kernel thread (`no_tick_switch`) is asked with
 * no kick, since the interrupt would switch nothing. An affinity change
 * that moves a woken thread to a busy processor's queue asks the same of
 * the thread running there, as move_queued_task calls wakeup_preempt
 * (kernel/sched/core.c:2449-2465).
 */
static void wake_preempt(void)
{
    setup();
    wake_quiet();
    CHECK(sched_wake_blocked(&procs[1]) == 0 && procs[0].need_resched &&
              (procs[0].work & WORK_RESCHED),
          "an ordinary wake asks the ordinary thread running to switch, with a kick");
    CHECK(sched_wake_blocked(&procs[2]) == -1 && procs[0].need_resched,
          "a second wake before the switch sends no second kick");

    wake_quiet();
    procs[1].sched_policy = SCHED_BATCH;
    CHECK(sched_wake_blocked(&procs[1]) == -1 && !procs[0].need_resched,
          "a woken SCHED_BATCH thread preempts nothing");
    wake_quiet();
    procs[1].sched_policy = SCHED_IDLE;
    CHECK(sched_wake_blocked(&procs[1]) == -1 && !procs[0].need_resched,
          "a woken SCHED_IDLE thread preempts nothing");
    wake_quiet();
    procs[0].sched_policy = SCHED_IDLE;
    CHECK(sched_wake_blocked(&procs[1]) == -1 && !procs[0].need_resched,
          "a SCHED_IDLE thread does not give way to another");
    wake_quiet();
    procs[1].sched_policy = SCHED_OTHER;
    CHECK(sched_wake_blocked(&procs[1]) == 0 && procs[0].need_resched,
          "a SCHED_IDLE thread gives way to a woken ordinary one");
    wake_quiet();
    procs[0].sched_policy = SCHED_OTHER;
    procs[0].no_tick_switch = 1;
    CHECK(sched_wake_blocked(&procs[1]) == -1 && procs[0].need_resched,
          "a kernel thread is asked to switch, with no kick");
    procs[0].no_tick_switch = 0;

    wake_quiet();
    cpu_table[1].online = 1;
    cpu_table[1].current = &procs[3];
    procs[3].state = PROC_RUNNING;
    procs[3].cpu = 1;
    CHECK(sched_wake_blocked(&procs[1]) == 0 && procs[1].cpu == 0 && procs[1].woken,
          "a wake queues on the waker's processor, which shares no cache with another");
    procs[0].need_resched = 0;
    sched_set_affinity(&procs[1], 2);
    CHECK(procs[1].cpu == 1 && run_queues[1].count == 1 && procs[3].need_resched &&
              (procs[3].work & WORK_RESCHED) && cpu_kicks[1] == 1,
          "an affinity move onto a busy processor asks its ordinary thread to switch, with a kick");
    cpu_table[1].online = 0;
    init_sched();
}

/**
 * Of four processors sharing a cache, 0 and 1 one core and 2 and 3 another,
 * a wake whose last processor, 0, is busy goes to the idle core before 0's
 * idle sibling, as Linux's select_idle_cpu takes a whole idle core first
 * (select_idle_core, kernel/sched/fair.c:7720-7748, 7843-7848); with no
 * core all idle, it goes to 0's idle sibling (select_idle_smt,
 * fair.c:7751-7768, 8035-8041).
 */
static void wake_place_core(void)
{
    int c;

    setup();
    wake_quiet();
    for (c = 1; c < 4; c++) {
        cpu_table[c].online = 1;
        cpu_table[c].current = cpu_table[c].idle = &procs[3 + c];
        procs[3 + c].state = PROC_RUNNING;
        procs[3 + c].cpu = c;
    }
    for (c = 0; c < 4; c++) {
        host_cpu = (unsigned int)c;
        sched_cache_join(0xF, c < 2 ? 0x3 : 0xC);
    }
    host_cpu = 0;
    CHECK(sched_wake_blocked(&procs[1]) == 2 && procs[1].cpu == 2 && procs[5].need_resched &&
              !procs[4].need_resched,
          "a wake goes to an idle core before the busy core's idle sibling");
    CHECK(sched_wake_blocked(&procs[2]) == 1 && procs[2].cpu == 1 && procs[4].need_resched,
          "with no core all idle, a wake goes to its last processor's idle sibling");
    k_memset(cache_peers, 0, sizeof(cache_peers));
    k_memset(core_peers, 0, sizeof(core_peers));
    smt_active = 0;
    for (c = 1; c < 4; c++) {
        cpu_table[c].online = 0;
    }
    init_sched();
}

/**
 * A thread made ready while the scheduler is on is credited, when it is
 * selected, with its whole wait on the run queue in nanoseconds, as Linux's
 * sched_info, and each switch onto a processor counts in pcount: a wait of
 * 2^32 + 10^6 microseconds, past any 32-bit count of microseconds, counts
 * whole; a wait begun before the scheduler is on counts nothing; a move to
 * another processor's queue keeps the wait, which counts whole when an idle
 * processor steals the thread; and a suspend's hold adds the wait so far,
 * counts none while it lasts, and starts each queued thread's wait again.
 */
static void run_delay(void)
{
    const uint64_t wait = ((uint64_t)1 << 32) * 1000u + 1000000000u;
    unsigned int i;

    clock_ns = (uint64_t)7 * 1000000000u + 123u;
    setup();
    wake(&procs[6]);
    clock_ns += wait;
    schedule();
    CHECK(current_proc == &procs[6] && procs[6].run_delay == wait && procs[6].pcount == 1,
          "a wait past 2^32 microseconds counts whole in run_delay");
    clock_ns += 5000u;
    schedule();
    CHECK(current_proc == &procs[1] && procs[1].run_delay == 0 && procs[1].pcount == 1,
          "a wait begun before the scheduler was on counts nothing");

    setup();
    cpu_table[1].online = 1;
    cpu_table[1].current = cpu_table[1].idle = NULL;
    wake(&procs[4]);
    clock_ns += 1000u;
    for (i = 1; i < 7; i++) {
        sched_set_affinity(&procs[i], 2);
    }
    clock_ns += 2000u;
    sched_set_affinity(&procs[4], 3);
    current_proc->state = PROC_BLOCKED;
    schedule();
    CHECK(current_proc == &procs[4] && procs[4].run_delay == 3000u && procs[4].pcount == 1,
          "a wait moved to another queue counts whole when an idle processor steals it");
    cpu_table[1].online = 0;
    init_sched();

    setup();
    wake(&procs[6]);
    clock_ns += 1000u;
    sched_hold_waits(1);
    CHECK(procs[6].run_delay == 1000u && !procs[6].run_queued,
          "a suspend's hold adds the wait so far and ends it");
    clock_ns += 1000000000u;
    sched_hold_waits(0);
    clock_ns += 500u;
    schedule();
    CHECK(current_proc == &procs[6] && procs[6].run_delay == 1500u,
          "the suspend counts nothing and the wait starts again at its end");
    clock_ns += 700u;
    schedule();
    CHECK(current_proc == &procs[1] && procs[1].run_delay == 1200u,
          "every queued thread's wait starts at the suspend's end");
    clock_ns = 0;
}

/**
 * The tick switches a thread for another of its processor's queue only once
 * it has run SCHED_SLICE_NS, 0.75 ms, since it was switched in, unless a
 * woken or real-time thread waits there. So a thread that a yield runs for
 * 200 us, as KVM's steal_time selftest runs one, ends before the yielder
 * runs again although a tick falls inside its run, and the yielder's
 * run_delay grows by the whole of it; a thread that has run its slice gives
 * way at the tick, and a real-time thread made ready with no wake's mark
 * takes the processor at the next tick within the slice.
 */
static void tick_slice(void)
{
    unsigned int i;

    clock_ns = 1000000000u;
    setup();
    for (i = 2; i < 7; i++) {
        sched_forget(&procs[i]);
        procs[i].state = PROC_BLOCKED;
    }
    procs[0].sched_yielded = 1;
    schedule();
    CHECK(current_proc == &procs[1] && procs[0].run_delay == 0, "the yield runs the other thread");
    clock_ns += 100000u;
    irq_flags = 0;
    sched_tick();
    CHECK(current_proc == &procs[1] && switches == 1,
          "a tick 100 us into a thread's run leaves it running");
    clock_ns += 150000u;
    procs[1].state = PROC_BLOCKED;
    schedule();
    CHECK(current_proc == &procs[0] && procs[0].run_delay == 250000u,
          "the yielder waits the whole run of the thread it yielded to");
    sched_make_ready(&procs[1]);
    clock_ns += 749999u;
    sched_tick();
    CHECK(current_proc == &procs[0], "a thread 1 ns short of its slice keeps the processor");
    clock_ns += 1u;
    sched_tick();
    CHECK(current_proc == &procs[1], "a thread that has run its slice gives way at the tick");
    wake(&procs[2]);
    sched_tick();
    CHECK(current_proc == &procs[2], "a woken thread takes the processor at the tick at once");
    procs[3].sched_policy = SCHED_FIFO;
    procs[3].rt_priority = 10;
    sched_make_ready(&procs[3]);
    CHECK(current_proc == &procs[2] && !procs[3].woken,
          "a real-time thread made ready, unmarked, waits for the tick");
    sched_tick();
    CHECK(current_proc == &procs[3],
          "and takes the processor at the tick within the current thread's slice");
    clock_ns = 0;
}

/** A lower-priority queue never arms RR's slice, an expired slice asks
 * once for selection, and selection that keeps the current thread renews
 * its slice even when its competitor became ineligible. */
static void slice_events(void)
{
    unsigned i, armed;

    clock_ns = 1000000000u;
    setup();
    for (i = 2; i < 7; i++) {
        sched_forget(&procs[i]);
        procs[i].state = PROC_BLOCKED;
    }
    procs[0].sched_policy = procs[1].sched_policy = SCHED_RR;
    procs[0].rt_priority = 50;
    procs[1].rt_priority = 40;
    sched_priority_changed(&procs[1]);
    procs[0].slice_ns = clock_ns;
    irq_flags = 0;
    slice_arms = 0;
    sched_slice_rearm();
    clock_ns += 2u * SCHED_INTERVAL_NS;
    sched_slice_rearm();
    CHECK(!slice_arms && !procs[0].need_resched,
          "lower-priority work arms no RR event, even after expiry");
    schedule();
    CHECK(current_proc == &procs[0] && procs[0].slice_ns == clock_ns,
          "selection that keeps RR consumes the old slice");

    procs[1].rt_priority = 50;
    sched_slice_rearm();
    CHECK(slice_arms == 1 && slice_event == (int64_t)(clock_ns + SCHED_INTERVAL_NS),
          "an eligible equal RR peer arms the slice once");
    clock_ns += SCHED_INTERVAL_NS;
    armed = slice_arms;
    sched_slice_rearm();
    sched_slice_rearm();
    CHECK(procs[0].need_resched && (procs[0].work & WORK_RESCHED) && slice_arms == armed,
          "expiry requests selection without an immediate event");
    procs[1].affinity = 2;
    schedule();
    CHECK(current_proc == &procs[0] && !procs[0].need_resched && procs[0].slice_ns == clock_ns &&
              slice_arms == armed,
          "an ineligible peer leaves a fresh slice and no repeated event");
    clock_ns = 0;
}

/**
 * Within the slice the tick takes a queued real-time thread only when it
 * outranks the current one: an equal SCHED_RR peer waits for the slice, as
 * Linux requeues an RR thread only once its time slice has run out
 * (task_tick_rt, kernel/sched/rt.c:2586-2602), and so it does with an
 * ordinary thread a wake marked queued under the pair, which selection
 * would not take, as wakeup_preempt_rt preempts only a lower current task
 * (rt.c:1655-1660). A priority raise of the queued peer asks the running
 * one to switch, not itself, as prio_changed_rt calls resched_curr
 * (rt.c:2526-2533), and the tick switches. A fall of the running one below
 * the queued one asks the running one, as prio_changed_rt reschedules a
 * running task once the queue's highest priority is above its own
 * (rt.c:2519-2520), and the tick switches back.
 */
static void tick_rr(void)
{
    unsigned int i;

    clock_ns = 1000000000u;
    setup();
    for (i = 1; i < 7; i++) {
        sched_forget(&procs[i]);
        procs[i].state = PROC_BLOCKED;
    }
    procs[0].sched_policy = procs[1].sched_policy = SCHED_RR;
    procs[0].rt_priority = procs[1].rt_priority = 10;
    sched_make_ready(&procs[1]);
    procs[0].slice_ns = clock_ns;
    clock_ns += 100000u;
    irq_flags = 0;
    sched_tick();
    CHECK(current_proc == &procs[0] && switches == 0,
          "an equal SCHED_RR peer waits for the slice at the tick");
    wake(&procs[2]);
    sched_tick();
    CHECK(current_proc == &procs[0] && switches == 0 && procs[2].woken && !procs[0].need_resched,
          "an ordinary woken thread under the RR pair lets no tick rotate it");
    procs[1].rt_priority = 11;
    sched_priority_changed(&procs[1]);
    CHECK(procs[0].need_resched && !procs[1].need_resched,
          "a raise of the queued peer asks the running thread to switch");
    sched_tick();
    CHECK(current_proc == &procs[1] && switches == 1 && !procs[0].need_resched,
          "a higher real-time thread takes the processor at the tick within the slice");
    procs[1].rt_priority = 9;
    sched_priority_changed(&procs[1]);
    CHECK(procs[1].need_resched && !procs[0].need_resched,
          "a running thread lowered below a queued one asks itself to switch");
    sched_tick();
    CHECK(current_proc == &procs[0] && switches == 2,
          "and the tick gives the processor to the queued one");
    clock_ns = 0;
}

/** Reset with processor 1 started, running procs[1] as SCHED_FIFO 99, and
 * procs[2], SCHED_FIFO 50, blocked with processor 1 last and `affinity`;
 * processor 0 runs procs[0], an ordinary thread, 100 us into its slice. */
static void two_cpus(uint64_t affinity)
{
    unsigned int i;

    clock_ns = 1000000000u;
    setup();
    for (i = 1; i < 7; i++) {
        sched_forget(&procs[i]);
        procs[i].state = PROC_BLOCKED;
    }
    cpu_table[1].online = 1;
    cpu_table[1].current = &procs[1];
    procs[1].state = PROC_RUNNING;
    procs[1].cpu = procs[2].cpu = 1;
    procs[1].sched_policy = procs[2].sched_policy = SCHED_FIFO;
    procs[1].rt_priority = 99;
    procs[2].rt_priority = 50;
    procs[2].affinity = affinity;
    procs[0].slice_ns = clock_ns;
    clock_ns += 100000u;
    irq_flags = 0;
}

/**
 * A real-time thread made ready where a higher one runs goes to the
 * processor running the lowest work, whose thread it asks to switch, as
 * Linux's select_task_rq_rt and find_lowest_rq place it
 * (kernel/sched/rt.c:1577-1597, 1807-1880): processor 1 runs a SCHED_FIFO
 * thread of priority 99, and one of priority 50 last ran there. Pinned to
 * processor 1, the 50 queues there; processor 0's tick within an ordinary
 * slice takes it once its affinity widens, as Linux's push_rt_task moves
 * it to a processor running lower work (rt.c:1982-2092). An affinity that
 * moves it off processor 1 asks processor 0's thread to switch, as
 * move_queued_task calls wakeup_preempt (core.c:2462-2463). Processor 1's
 * FIFO thread lowered to 10 and pinned there sends the 50 to processor 0
 * all the same, as select_task_rq_rt searches when the running task has
 * nr_cpus_allowed < 2. A processor ranks by the highest real-time priority
 * of its running and queued threads, as find_lowest_rq ranks it by
 * highest_prio.curr: with a FIFO 90 queued on processor 0 and a third
 * processor running FIFO 10, the 50 goes to the third.
 */
static void tick_rt(void)
{
    two_cpus(~(uint64_t)0);
    sched_make_ready(&procs[2]);
    CHECK(run_queues[0].count == 1 && run_queues[1].count == 0 && procs[2].cpu == 0 &&
              procs[0].need_resched && !procs[1].need_resched,
          "a woken FIFO 50 thread goes to the processor running ordinary work");
    sched_take_preempt();
    CHECK(current_proc == &procs[2] && switches == 1, "and takes it at that thread's request");

    two_cpus(2);
    sched_make_ready(&procs[2]);
    CHECK(run_queues[1].count == 1 && run_queues[0].count == 0 && !procs[0].need_resched &&
              !procs[1].need_resched,
          "a FIFO 50 thread pinned to processor 1 queues behind the FIFO 99 one");
    sched_set_affinity(&procs[2], 3);
    CHECK(run_queues[1].count == 1 && !procs[0].need_resched,
          "an affinity that keeps processor 1 leaves it queued there");
    sched_tick();
    CHECK(current_proc == &procs[2] && procs[2].cpu == 0 && switches == 1,
          "processor 0's tick within an ordinary slice takes it from processor 1's queue");

    two_cpus(2);
    sched_make_ready(&procs[2]);
    sched_set_affinity(&procs[2], 1);
    CHECK(run_queues[0].count == 1 && procs[2].cpu == 0 && procs[0].need_resched,
          "an affinity move onto processor 0 asks its ordinary thread to switch");

    two_cpus(~(uint64_t)0);
    procs[1].rt_priority = 10;
    procs[1].affinity = 2;
    sched_make_ready(&procs[2]);
    CHECK(run_queues[0].count == 1 && run_queues[1].count == 0 && procs[2].cpu == 0 &&
              procs[0].need_resched && !procs[1].need_resched,
          "a FIFO 50 thread whose processor runs a pinned FIFO 10 one goes to the ordinary one");

    two_cpus(~(uint64_t)0);
    procs[3].sched_policy = procs[4].sched_policy = SCHED_FIFO;
    procs[3].rt_priority = 90;
    procs[3].affinity = 1;
    procs[3].cpu = 0;
    sched_make_ready(&procs[3]);
    procs[0].need_resched = 0;
    cpu_table[2].online = 1;
    cpu_table[2].current = &procs[4];
    procs[4].state = PROC_RUNNING;
    procs[4].cpu = 2;
    procs[4].rt_priority = 10;
    sched_make_ready(&procs[2]);
    CHECK(run_queues[0].count == 1 && run_queues[2].count == 1 && procs[2].cpu == 2 &&
              procs[4].need_resched && !procs[0].need_resched,
          "a FIFO 50 thread passes over an ordinary processor with a FIFO 90 one queued");
    cpu_table[2].online = 0;
    cpu_table[1].online = 0;
    init_sched();
    clock_ns = 0;
}

/* The calls of the `sched_out` that `sched_out_rows` gives its threads,
 * and those of them for a switch that left the thread able to run. */
static int out_calls, out_preempted;
static void *out_arg;

/** Count a call of a thread's `sched_out`, which comes with interrupts
 * off, the run queues' lock in this model, while the thread is current. */
static void count_out(void *arg, int preempted)
{
    CHECK(!irq_flags, "sched_out with interrupts on");
    CHECK(arg == current_proc, "sched_out after the thread stopped being current");
    out_calls++;
    out_preempted += preempted != 0;
    out_arg = arg;
}

/**
 * A thread's `sched_out` is called at each switch away from it, and only
 * then, saying whether the thread can still run: not when selection keeps
 * a SCHED_FIFO thread over ordinary or equal ones, once, preempted, when a
 * SCHED_RR thread gives way to an equal one, and once, not preempted, when
 * a thread blocks.
 */
static void sched_out_rows(void)
{
    unsigned long nivcsw;

    setup();
    out_calls = 0;
    out_preempted = 0;
    out_arg = NULL;
    procs[0].sched_out = count_out;
    procs[0].sched_out_arg = &procs[0];
    current_proc->in_syscall = 1;
    current_proc->sched_policy = SCHED_FIFO;
    current_proc->rt_priority = 10;
    current_proc->need_resched = 1;
    irq_flags = 0;
    sched_take_preempt();
    CHECK(current_proc == &procs[0] && !switches && !out_calls,
          "a real-time thread kept over ordinary ones is not switched out, nor told so");
    procs[3].sched_policy = SCHED_FIFO;
    procs[3].rt_priority = 10;
    sched_priority_changed(&procs[3]);
    current_proc->need_resched = 1;
    sched_take_preempt();
    CHECK(current_proc == &procs[0] && !switches && !out_calls,
          "a SCHED_FIFO thread kept over an equal one is not told either");
    current_proc->sched_policy = SCHED_RR;
    current_proc->need_resched = 1;
    nivcsw = procs[0].nivcsw;
    sched_take_preempt();
    CHECK(current_proc == &procs[3] && switches == 1 && out_calls == 1 && out_preempted == 1 &&
              out_arg == &procs[0] && procs[0].nivcsw == nivcsw + 1,
          "a SCHED_RR thread that gives way to an equal one is told once, at the switch");
    procs[3].sched_out = count_out;
    procs[3].sched_out_arg = &procs[3];
    procs[3].state = PROC_BLOCKED;
    schedule();
    CHECK(current_proc != &procs[3] && switches == 2 && out_calls == 2 && out_preempted == 1 &&
              out_arg == &procs[3],
          "a thread that blocks is told at the switch, not preempted");
    procs[0].sched_out = NULL;
    procs[3].sched_out = NULL;
    procs[0].in_syscall = 0;
}

/** One processor: nothing to answer. */
void smp_service(void)
{}

/** Store `value` at `p` and return what was there: one thread. */
unsigned int arch_atomic_xchg(volatile unsigned int *p, unsigned int value)
{
    unsigned int old = *p;

    *p = value;
    return old;
}

/** Take the modelled big lock back at `depth`, counting the take and its
 * key. */
void bkl_take_as(int depth, int key)
{
    if (depth > 0) {
        this_cpu()->bkl_depth = depth;
        bkl_takes++;
        bkl_last_key = key;
    }
}

/** No thread is counted alone in `bkl/calls` here: nothing to note. */
void bkl_note_switch(const struct process *old, const struct process *next, int yielded)
{
    (void)old;
    (void)next;
    (void)yielded;
}

/** Check idle projection, steal subtraction, catch-up and the first
 * busy tick. The counts are kernel ticks of TICK_NS, 4 ms at 250 Hz, and
 * each wake comes more than half a tick past its edge, where rounding to
 * the nearest tick would count one tick more than the edge does. */
static void nohz_accounting(void)
{
    uint64_t user, busy, idle, steal;

    k_memset(cpu_table, 0, sizeof(cpu_table));
    k_memset(procs, 0, sizeof(procs));
    host_cpu = 0;
    current_proc = &procs[0];
    this_cpu()->idle_wait = 1;
    this_cpu()->tick_stop_period = 1;
    this_cpu()->tick_stopped = 1;
    /* 500 ticks of idle and a quarter tick, 31 1/4 ticks of them stolen. */
    this_cpu()->accounted_ns = 1000000000;
    clock_ns = 1000000000u + 500u * (uint64_t)TICK_NS + TICK_NS / 4u;
    steal_clock_ns = 31u * (uint64_t)TICK_NS + TICK_NS / 4u;
    host_cpu = 1;
    sched_cpu_ticks(0, &user, &busy, &idle, &steal);
    CHECK(user == 0 && busy == 0 && idle == 469 && steal == 31,
          "a remote reading projects the idle span less its steal time");
    CHECK(cpu_table[0].idle_ticks == 0 && cpu_table[0].steal_ticks == 0,
          "a projected reading changes no stored account");
    host_cpu = 0;
    sched_account_tick(0, 0);
    CHECK(this_cpu()->idle_ticks == 469 && this_cpu()->steal_ticks == 31 &&
              this_cpu()->accounted_ns == 3000000000,
          "waking accounts the idle span and steal time once");
    sched_account_tick(0, 0);
    CHECK(this_cpu()->idle_ticks == 469, "a second accounting counts no tick twice");
    this_cpu()->tick_stopped = 0;
    this_cpu()->idle_wait = 0;
    clock_ns += TICK_NS;
    sched_account_tick(1, 0);
    CHECK(procs[0].utime == 1 && this_cpu()->user_ticks == 1 && this_cpu()->idle_ticks == 469,
          "the first busy tick charges no idle span to the new thread");
    /* A one-tick period: the thread ran 2.5 ms past the edge at 3004 ms
     * and halted, and the processor woke at 3019 ms, 3 ms past the edge at
     * 3016 ms. Rounding would charge idle to 3020 ms. */
    this_cpu()->accounted_ns = 3004000000;
    this_cpu()->idle_wait = 1;
    this_cpu()->tick_stop_period = 1;
    this_cpu()->tick_stopped = 1;
    clock_ns = 3019000000u;
    host_cpu = 1;
    sched_cpu_ticks(0, &user, &busy, &idle, &steal);
    CHECK(idle == 472 && steal == 31, "a remote reading projects a stopped span to its last edge");
    host_cpu = 0;
    sched_account_tick(0, 0);
    CHECK(this_cpu()->idle_ticks == 472 && this_cpu()->accounted_ns == 3016000000,
          "waking charges idle the periods whose edges the stopped tick passed");
    this_cpu()->tick_stopped = 0;
    this_cpu()->idle_wait = 0;
    clock_ns = 3020000000u;
    sched_account_tick(1, 0);
    CHECK(procs[0].utime == 2 && this_cpu()->idle_ticks == 472,
          "the next tick charges its whole period to the thread, the run before the halt with it");
    /* A halt under a running tick, whose ticks charged its edges, ending
     * 2.5 ms past the edge at 3020 ms. */
    this_cpu()->idle_wait = 1;
    clock_ns = 3022500000u;
    sched_idle_account(this_cpu());
    CHECK(this_cpu()->idle_ticks == 472 && this_cpu()->accounted_ns == 3020000000,
          "the end of a halt under a running tick charges the last thread's run to no one");
    this_cpu()->idle_wait = 0;
    clock_ns = 3024000000u;
    sched_account_tick(1, 0);
    CHECK(procs[0].utime == 3, "the tick after it charges that run with its period");
    /* A two-tick period, 8 ms: the wake at 3037 ms is 5 ms past the
     * period's edge at 3032 ms and 1 ms past the tick's at 3036 ms. */
    this_cpu()->idle_wait = 1;
    this_cpu()->tick_stop_period = 2;
    this_cpu()->tick_stopped = 1;
    clock_ns = 3037000000u;
    sched_account_tick(0, 0);
    CHECK(this_cpu()->idle_ticks == 474 && this_cpu()->accounted_ns == 3032000000,
          "a stopped span ends at the edge of its own period, not of a tick");
    this_cpu()->tick_stopped = 0;
    this_cpu()->idle_wait = 0;
    load_tick = 1000;
    load_wait = 5001;
    kernel_ticks = 5999;
    CHECK(sched_load_idle_ticks() == 2, "the next load sample keeps its phase");
    kernel_ticks = 6001;
    CHECK(sched_load_idle_ticks() == 1, "a load sample already due is serviced at once");
}

/** Keep the tick stopped across a wake that returns to idle, and restart
 * it only when a ready thread leaves the idle task. */
static void nohz_switch(void)
{
    unsigned int i;

    setup();
    for (i = 1; i < 7; i++) {
        sched_forget(&procs[i]);
    }
    this_cpu()->idle = &procs[0];
    clock_ns = 4000000000u;
    this_cpu()->accounted_ns = clock_ns;
    this_cpu()->steal_ns = steal_clock_ns;
    idle_timer_ready = 1;
    idle_timer_exits = 0;
    idle_wait_irq_off();
    CHECK(this_cpu()->tick_stopped && !idle_timer_exits,
          "an interrupt returning to idle leaves the tick stopped");
    schedule();
    CHECK(current_proc == &procs[0] && this_cpu()->tick_stopped && !idle_timer_exits,
          "an empty run queue keeps the stopped tick across a selection");
    /* Three quarters of a tick past the tenth edge: rounding would count
     * eleven. */
    clock_ns += 10u * (uint64_t)TICK_NS + 3u * TICK_NS / 4u;
    sched_make_ready(&procs[1]);
    schedule();
    CHECK(current_proc == &procs[1] && !this_cpu()->tick_stopped && idle_timer_exits == 1 &&
              this_cpu()->idle_ticks == 10 && !procs[1].stime,
          "the switch accounts the idle span to its last edge and restarts before the new thread");
    idle_timer_ready = 0;
}

/** A wake before or during idle entry must return to selection without
 * stopping again for a halt or touching the event device unnecessarily. */
static void nohz_ready_entry(void)
{
    unsigned int i;

    setup();
    for (i = 1; i < 7; i++) {
        sched_forget(&procs[i]);
    }
    this_cpu()->idle = &procs[0];
    irq_flags = 0;
    idle_timer_ready = 1;
    idle_timer_enters = idle_halts = 0;
    sched_make_ready(&procs[1]);
    CHECK(procs[0].need_resched && !(procs[0].work & WORK_RESCHED),
          "a wake marks the idle task without user-return work");
    this_cpu()->idle_wait = 1;
    sched_take_preempt();
    CHECK(current_proc == &procs[0] && this_cpu()->idle_wait && !switches,
          "a service point cannot switch inside post-halt idle work");
    this_cpu()->idle_wait = 0;
    irq_early_ok = 1;
    idle_wait_irq_off();
    irq_early_ok = 0;
    CHECK(!idle_timer_enters && !idle_halts,
          "work already ready avoids idle timer programming and a halt");
    schedule();
    CHECK(current_proc == &procs[1] && !procs[0].need_resched && !(procs[0].work & WORK_RESCHED),
          "selection consumes the idle task's request under the run queue lock");
    setup();
    for (i = 1; i < 7; i++) {
        sched_forget(&procs[i]);
    }
    this_cpu()->idle = &procs[0];
    irq_flags = 0;
    idle_timer_enters = idle_halts = 0;
    idle_wake_on_enter = 1;
    idle_wait_irq_off();
    CHECK(idle_timer_enters == 1 && !idle_halts && procs[0].need_resched,
          "a wake during preparation is checked again before the halt");
    idle_timer_ready = 0;
}

/** Make the scheduler's idle task current on one processor with nothing
 * ready, interrupts off, the stopped tick's timer ready and the counts of
 * the idle wait's halts, windows, interrupts and drains cleared. */
static void cancel_setup(void)
{
    unsigned int i;

    setup();
    for (i = 1; i < 7; i++) {
        sched_forget(&procs[i]);
    }
    this_cpu()->idle = &procs[0];
    irq_flags = 0;
    idle_timer_ready = 1;
    idle_timer_enters = idle_halts = 0;
    irq_windows = irq_early_windows = irq_taken = 0;
    loop_drains = 0;
    irq_pending = 1;
}

/** Whether the idle wait just made took the row's pending interrupt
 * through one window, before the marker when `early` is set and marked idle
 * otherwise, and returned unmarked with interrupts off and no halt. */
static int cancel_taken(int early)
{
    return irq_early_windows == (early ? 1u : 0u) && irq_windows == (early ? 0u : 1u) &&
           irq_taken == 1 && !irq_pending && !irq_flags && !idle_halts && !this_cpu()->idle_wait;
}

/** Run `idle_wait_irq_off` where the row expects it to end before the idle
 * marker. */
static void cancel_early(void)
{
    irq_early_ok = 1;
    idle_wait_irq_off();
    irq_early_ok = 0;
}

/**
 * A loopback drain in an idle wait takes a pending interrupt through its
 * window of one instruction before the idle marker is set, and returns with
 * interrupts off and no halt, with the big lock held as it was, or with a
 * tried lock taken for the drain and let go.
 */
static void idle_cancel_drains(void)
{
    cancel_setup();
    this_cpu()->bkl_depth = 1;
    loop_pending = 1;
    cancel_early();
    CHECK(loop_drains == 1 && !loop_pending && cancel_taken(1) && this_cpu()->bkl_depth == 1,
          "a drain under the big lock takes a pending interrupt before the marker, then returns");
    this_cpu()->bkl_depth = 0;

    cancel_setup();
    bkl_host_free = 1;
    bkl_host_takes = 0;
    loop_pending = 1;
    cancel_early();
    CHECK(loop_drains == 1 && !loop_pending && bkl_host_takes == 1 && !bkl_held() &&
              cancel_taken(1),
          "a drain under a tried big lock lets it go and takes a pending interrupt before the "
          "marker");
    bkl_host_free = 0;
    irq_pending = 0;
    idle_timer_ready = 0;
    irq_flags = 0x200;
}

/**
 * Ready work that cancels the scheduler idle task's wait takes a pending
 * interrupt through its window of one instruction and returns with
 * interrupts off and no halt: before the idle marker when it was ready
 * before the marker was set, and marked idle when it came once the marker
 * was set or while the tick stopped, which declines the sleep.
 */
static void idle_cancel_ready(void)
{
    cancel_setup();
    sched_make_ready(&procs[1]);
    cancel_early();
    CHECK(cancel_taken(1) && !idle_timer_enters && procs[0].need_resched,
          "ready work before the marker takes a pending interrupt, unmarked, with no halt");

    cancel_setup();
    resched_on_drop = 1;
    idle_wait_irq_off();
    CHECK(!resched_on_drop && cancel_taken(0) && !idle_timer_enters && procs[0].need_resched,
          "ready work after the marker takes a pending interrupt marked idle, with no halt");

    cancel_setup();
    idle_wake_on_enter = 1;
    idle_wait_irq_off();
    CHECK(!idle_wake_on_enter && idle_timer_enters == 1 && cancel_taken(0) && procs[0].need_resched,
          "a sleep a wake declines while the tick stops takes a pending interrupt marked idle");
    irq_pending = 0;
    idle_timer_ready = 0;
    irq_flags = 0x200;
}

/** Deliver a ready thread from another modeled CPU during the poll. */
static void poll_wake_step(void)
{
    if (poll_service) {
        poll_service = 0;
        service_pending = 1;
        host_cpu = 1;
        smp_kick(0);
        host_cpu = 0;
    }
    if (poll_tick) {
        poll_tick = 0;
        tick_due_model += 4000000;
        sched_account_tick(0, tick_due_model);
        this_cpu()->idle_poll = CPU_POLL_KICKED;
    }
    if (poll_wake) {
        poll_wake = 0;
        host_cpu = 1;
        sched_make_ready(&procs[1]);
        host_cpu = 0;
    }
}

/** A short poll which catches a wake keeps its existing tick; one which
 * finds no work stops the tick before actually halting. */
static void nohz_poll_before_stop(void)
{
    unsigned int i;

    setup();
    for (i = 1; i < 7; i++) {
        sched_forget(&procs[i]);
    }
    this_cpu()->idle = &procs[0];
    procs[1].affinity = 1;
    irq_flags = 0;
    this_cpu()->idle_poll_us = 20;
    poll_rate = TICK_US;
    poll_cycles = 0;
    idle_timer_ready = 1;
    idle_timer_enters = idle_halts = 0;
    poll_wake = 1;
    idle_wait_irq_off();
    CHECK(!idle_timer_enters && !idle_halts && procs[0].need_resched && !this_cpu()->tick_stopped,
          "a wake caught in the poll avoids stopping and restarting the device");
    schedule();
    procs[1].state = PROC_BLOCKED;
    schedule();
    this_cpu()->idle_poll_us = 20;
    idle_timer_enters = idle_halts = 0;
    idle_wait_irq_off();
    CHECK(idle_timer_enters == 1 && idle_halts == 1 && this_cpu()->tick_stopped,
          "a poll without a wake stops the tick before halting");
    poll_wake = 1;
    idle_timer_enters = idle_halts = 0;
    idle_wait_irq_off();
    CHECK(poll_wake && idle_timer_enters == 1 && idle_halts == 1,
          "a stopped CPU skips polling and commits its next short event before halting");
    poll_wake = 0;
    poll_rate = 0;
    idle_timer_ready = 0;
}

/** A short non-tick wake retains the next tick; that tick clears the
 * prediction and the next quiet idle entry stops it. */
static void nohz_short_waits(void)
{
    unsigned int i;

    setup();
    for (i = 1; i < 7; i++) {
        sched_forget(&procs[i]);
    }
    this_cpu()->idle = &procs[0];
    this_cpu()->idle_short = 1;
    this_cpu()->idle_poll_us = 0;
    poll_rate = TICK_US;
    halt_cycles = 100;
    halt_tick = 0;
    idle_timer_ready = 1;
    idle_timer_enters = idle_halts = 0;
    idle_wait_irq_off();
    CHECK(!idle_timer_enters && idle_halts == 1 && this_cpu()->idle_short &&
              !this_cpu()->tick_stopped,
          "a predicted short wait retains the running tick without programming it");
    halt_tick = 1;
    idle_wait_irq_off();
    CHECK(!idle_timer_enters && idle_halts == 2 && !this_cpu()->idle_short,
          "a scheduler tick clears the short-wait prediction even on an early phase");
    halt_tick = 0;
    idle_wait_irq_off();
    CHECK(idle_timer_enters == 1 && idle_halts == 3 && this_cpu()->tick_stopped,
          "a quiet CPU stops at the next idle entry after its retained tick");
    halt_cycles = halt_tick = 0;
    poll_rate = 0;
    idle_timer_ready = 0;
}

/** A logical tick during an open poll clears a retained-tick prediction
 * even though the poll returns without another halt. */
static void nohz_poll_tick(void)
{
    unsigned int i;

    setup();
    for (i = 1; i < 7; i++) {
        sched_forget(&procs[i]);
    }
    this_cpu()->idle = &procs[0];
    this_cpu()->idle_short = 1;
    this_cpu()->idle_poll_us = 20;
    poll_rate = TICK_US;
    poll_tick = 1;
    idle_timer_enters = idle_halts = 0;
    idle_wait_irq_off();
    CHECK(!poll_tick && !idle_timer_enters && !idle_halts && !this_cpu()->idle_short,
          "a tick in the open poll clears the prediction before returning to selection");
    poll_rate = 0;
}

/** A deferred earlier event skips the poll even when an older nonzero
 * device deadline remains; final retained entry commits it before HLT. */
static void nohz_dirty_poll(void)
{
    unsigned int i;

    setup();
    for (i = 1; i < 7; i++) {
        sched_forget(&procs[i]);
    }
    this_cpu()->idle = &procs[0];
    this_cpu()->idle_short = 1;
    this_cpu()->idle_poll_us = 20;
    poll_rate = TICK_US;
    poll_wake = 1;
    idle_hw_pending = 1;
    idle_retains = idle_halts = 0;
    idle_wait_irq_off();
    CHECK(poll_wake && idle_retains == 1 && idle_halts == 1 && !idle_hw_pending,
          "a dirty retained timer skips polling and commits before halting");
    poll_wake = 0;
    poll_rate = 0;
}

/** Absorbed service kicks install their earlier event before selection
 * can retain the tick or leave idle, from awake and open-poll states. */
static void nohz_absorbed_service(void)
{
    unsigned int i;

    setup();
    for (i = 1; i < 7; i++) {
        sched_forget(&procs[i]);
    }
    this_cpu()->idle = &procs[0];
    this_cpu()->idle_short = 1;
    sched_idle_awake();
    service_pending = 1;
    host_cpu = 1;
    smp_kick(0);
    host_cpu = 0;
    idle_retains = idle_halts = 0;
    idle_wait_irq_off();
    CHECK(!service_pending && idle_hw_pending && !idle_halts,
          "an awake service kick installs its target before returning to selection");
    idle_wait_irq_off();
    CHECK(idle_retains == 1 && idle_halts == 1 && !idle_hw_pending,
          "the next retained halt commits the absorbed awake service target");
    this_cpu()->idle_poll_us = 20;
    poll_rate = TICK_US;
    poll_service = 1;
    idle_retains = idle_halts = 0;
    idle_wait_irq_off();
    CHECK(!poll_service && !service_pending && idle_hw_pending && !idle_halts,
          "a service kick during the open poll installs its target before return");
    procs[1].affinity = 1;
    sched_make_ready(&procs[1]);
    schedule();
    CHECK(current_proc == &procs[1] && !idle_hw_pending,
          "selection commits an absorbed service event even with a retained tick");
    poll_rate = 0;
}

/**
 * An idle task kicked while awake, after an interrupt ended its halt and
 * before its next selection (`sched_idle_awake`), ends its wait with no
 * halt and lets a pending interrupt in first, with the processor still
 * marked idle, as a kicked poll does: a tick pending there would otherwise
 * be taken after the switch and charged to the incoming thread. With no
 * kick the wait halts.
 */
static void idle_kicked(void)
{
    setup();
    cpu_table[0].idle = current_proc;
    irq_flags = 0;
    idle_halts = irq_windows = 0;
    sched_idle_awake();
    CHECK(cpu_table[0].idle_poll == CPU_POLL_ON,
          "an interrupt that ends the halt marks the idle task awake");
    cpu_table[0].idle_poll = CPU_POLL_KICKED; /* a remote wake's `smp_kick` */
    idle_wait_irq_off();
    CHECK(irq_windows == 1 && idle_halts == 0 && !irq_flags && !cpu_table[0].idle_wait &&
              cpu_table[0].idle_poll == CPU_POLL_NONE,
          "a kick while awake ends the wait through the interrupt window, with no halt");
    sched_idle_awake();
    idle_wait_irq_off();
    CHECK(irq_windows == 1 && idle_halts == 1 && !irq_flags &&
              cpu_table[0].idle_poll == CPU_POLL_NONE,
          "an idle task awake with no kick halts");
    cpu_table[0].idle = NULL;
    irq_flags = 0x200;
}

/**
 * Two busy processors whose ticks choose one idle processor with a stopped
 * tick, before that processor's selection, kick it once: the first claims
 * its idle task's request (`need_resched`), and the second finds it set, as
 * a wake's kick finds it (`sched_preempt_cpu`). Processors 0 and 1 each
 * queue a thread that processor 2, idle, may take.
 */
static void balance_once(void)
{
    unsigned int i;

    setup();
    for (i = 1; i < 7; i++) {
        sched_forget(&procs[i]);
        procs[i].state = PROC_BLOCKED;
    }
    cpu_table[1].online = 1;
    cpu_table[1].current = &procs[1];
    procs[1].state = PROC_RUNNING;
    procs[1].cpu = procs[3].cpu = 1;
    procs[2].cpu = 0;
    sched_make_ready(&procs[2]);
    sched_make_ready(&procs[3]);
    CHECK(run_queues[0].count == 1 && run_queues[1].count == 1,
          "each busy processor queues a thread");
    cpu_table[2].online = 1;
    cpu_table[2].current = cpu_table[2].idle = &procs[6];
    procs[6].state = PROC_RUNNING;
    procs[6].cpu = 2;
    procs[6].need_resched = 0;
    cpu_table[2].tick_stopped = 1;
    k_memset(cpu_kicks, 0, sizeof(cpu_kicks));
    irq_flags = 0;
    sched_idle_balance();
    host_cpu = 1;
    sched_idle_balance();
    host_cpu = 0;
    CHECK(cpu_kicks[2] == 1 && procs[6].need_resched,
          "two balancers that choose one idle processor kick it once");
    CHECK(run_queues[0].count == 1 && run_queues[1].count == 1,
          "the balancers leave the threads for the idle processor to take");
    cpu_table[2].tick_stopped = 0;
    cpu_table[2].online = 0;
    cpu_table[1].online = 0;
    init_sched();
    irq_flags = 0x200;
}

/** Run every row; 0 when each passes, 1 otherwise. */
int main(int argc, char **argv)
{
    if (argc == 2 && __builtin_strcmp(argv[1], "--idle-only") == 0) {
        nohz_accounting();
        nohz_switch();
        nohz_ready_entry();
        idle_cancel_drains();
        idle_cancel_ready();
        nohz_poll_before_stop();
        nohz_short_waits();
        nohz_poll_tick();
        nohz_dirty_poll();
        nohz_absorbed_service();
        dprintf(1, "sched-hosttest: idle: %s\n", failures ? "fail" : "pass");
        return failures != 0;
    }
    if (argc == 2 && __builtin_strcmp(argv[1], "--affinity-only") == 0) {
        affinity_rows();
        dprintf(1, "sched-hosttest: affinity: %s\n", failures ? "fail" : "pass");
        return failures != 0;
    }
    priority();
    fairness();
    states();
    lifecycle();
    big_lock();
    sched_out_rows();
    accounting();
    accounting_held();
    load_phase();
    all_idle();
    ready_lists();
    wake_twice();
    migration();
    wake_preempt();
    wake_place_core();
    run_delay();
    tick_slice();
    slice_events();
    tick_rr();
    tick_rt();
    affinity_rows();
    nohz_accounting();
    nohz_switch();
    nohz_ready_entry();
    idle_cancel_drains();
    idle_cancel_ready();
    nohz_poll_before_stop();
    nohz_short_waits();
    nohz_poll_tick();
    nohz_dirty_poll();
    nohz_absorbed_service();
    idle_kicked();
    balance_once();
    if (failures) {
        return 1;
    }
    dprintf(1, "sched-hosttest: result: pass: priority, fairness, states, lifecycle, big lock, "
               "preempt due, "
               "accounting, "
               "load phase, "
               "idle machine, ready lists, wake twice, migration, wake preempt, wake place core, "
               "run delay, tick slice, tick rr, "
               "tick rt, idle cancel drains, idle cancel ready, idle kicked, balance once\n");
    return 0;
}

/** Nothing: no row records USB input. */
void usb_stamp_dispatch(uint32_t pid)
{
    (void)pid;
}

/** Read the same modeled steal clock from any processor. */
uint64_t arch_cpu_steal_ns(int cpu)
{
    (void)cpu;
    return steal_clock_ns;
}

/** No timer hardware is modeled by the scheduler's rows. */
int timer_idle_enter(void)
{
    idle_timer_enters++;
    if (idle_wake_on_enter) {
        idle_wake_on_enter = 0;
        sched_make_ready(&procs[1]);
    }
    return idle_timer_ready;
}

/** Nothing: no timer hardware is modeled. */
void timer_idle_exit(void)
{
    idle_hw_pending = 0;
    idle_timer_exits++;
}

/** Nothing: no device services are modeled. */
void timer_idle_work(void)
{}
