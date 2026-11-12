/**
 * Host test of the scheduler's selection and the wait queue's wakeups,
 * proc/sched.c and proc/wait_queue.c, with no switch of the processor.
 *
 * host-deps: rach2/sys/lib/kstring.c rach2/sys/lib/kctypes.c rach2/sys/tick.c
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

/** A spin's pause, which the host does not need. */
void arch_relax(void)
{}
static process_t procs[7];
/* Each process's floating-point area, which the stand-ins tell apart. */
static uint8_t areas[7][16];
static uint32_t irq_flags;
static unsigned switches;
static int failures;
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

/** Nothing: no row waits for an interrupt. */
void arch_idle_masked(void)
{}

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
    return 0;
}

/** 0: the model is no guest, the other reason an idle wait does not poll. */
int arch_guest(void)
{
    return 0;
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
    return 0;
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

/* The big lock's takes the model saw: how many, and the last one's key. */
static int bkl_takes, bkl_last_key;

/** Let the modelled big lock go and return the processor's depth, which is
 * 0 in every row but the big lock's. */
int bkl_drop(void)
{
    int depth = this_cpu()->bkl_depth;

    this_cpu()->bkl_depth = 0;
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

/** Count a request to the remote processor, which the switch model services. */
void smp_kick(int cpu)
{
    if (cpu == 1) {
        affinity_kicks++;
    }
}

/** Nothing: no row runs the idle task's loop. */
void arch_irq_disable(void)
{}

/** Nothing: no row runs an idle wait's poll. */
void arch_irq_enable(void)
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
    for (i = 1; i < 7; i++) {
        sched_make_ready(&procs[i]);
    }
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
    sched_account_tick(0);
    CHECK(cpu->idle_ticks == idle + 3 && cpu->busy_ticks == busy,
          "three ticks of the processor's clock since its last count are three idle ticks");
    clock_ns += TICK_NS / 4u;
    sched_account_tick(0);
    CHECK(cpu->idle_ticks == idle + 3, "a quarter tick later counts none");
    cpu->idle_wait = 0;
    clock_ns += TICK_NS;
    sched_account_tick(1);
    CHECK(cpu->busy_ticks == busy + 1 && cpu->user_ticks == user + 1 && cpu->idle_ticks == idle + 3,
          "the next tick in ring 3 is one busy tick of user time");
    cpu->idle_wait = 1;
    steal_clock_ns = 2u * TICK_NS + TICK_NS / 2u;
    clock_ns += 3u * TICK_NS;
    sched_account_tick(0);
    CHECK(cpu->steal_ticks == steal + 2 && cpu->idle_ticks == idle + 4 &&
              cpu->busy_ticks == busy + 1,
          "two and a half ticks stolen of three are two steal ticks and one idle");
    steal_clock_ns += TICK_NS / 2u;
    clock_ns += TICK_NS;
    sched_account_tick(0);
    CHECK(cpu->steal_ticks == steal + 3 && cpu->idle_ticks == idle + 4,
          "the stolen half tick left over joins the next to make a steal tick");
    clock_ns += 5000u * (uint64_t)TICK_NS;
    sched_account_tick(0);
    CHECK(cpu->idle_ticks == idle + 5004 && cpu->steal_ticks == steal + 3,
          "a gap of five seconds, past 2^32 nanoseconds, is five thousand ticks");
    cpu->current = saved;
    clock_ns = saved_clock;
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
 * (kernel/sched/core.c:1094-1116); the second, finding it ready under the
 * run queues' lock, asks for none and leaves it where it is in its list,
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
    CHECK(sched_wake_blocked(&procs[4]) == -1 && !current_proc->need_resched,
          "an equal record asks for no interrupt and no switch");
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

/** Run every row; 0 when each passes, 1 otherwise. */
int main(int argc, char **argv)
{
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
    load_phase();
    all_idle();
    ready_lists();
    wake_twice();
    migration();
    run_delay();
    tick_slice();
    tick_rr();
    tick_rt();
    affinity_rows();
    if (failures) {
        return 1;
    }
    dprintf(1, "sched-hosttest: result: pass: priority, fairness, states, lifecycle, big lock, "
               "preempt due, "
               "accounting, "
               "load phase, "
               "idle machine, ready lists, wake twice, migration, run delay, tick slice, tick rr, "
               "tick rt\n");
    return 0;
}

/** Nothing: no row records USB input. */
void usb_stamp_dispatch(uint32_t pid)
{
    (void)pid;
}
