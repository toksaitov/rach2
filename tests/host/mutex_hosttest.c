/*
 * mutex_hosttest.c: the kernel mutex of rach2/sys/proc/mutex.c: its spin
 * while the holder runs on another processor, the sleeps it saves for a
 * short hold, the handoff to a sleeper that spinners would starve, and the
 * queue of spinners.
 *
 * host-deps: tests/host/compat/park_host.c
 * host-cflags: -DRACH2_HOST_CPU
 *
 * One host thread plays every processor. `host_cpu` is the processor that
 * runs, and each processor's current thread is its record in `cpu_table`,
 * so `current_proc` follows a switch of `host_cpu`. The scheduler's owner
 * spin is the model's: a holder runs while its processor's `running` is
 * set, and a row's `hold` counts the pauses it runs before it gives the
 * mutex back, which the model then does as that holder. The wait is the
 * model's too: each time a taker would sleep, it counts the sleep and runs
 * the row's next step as another thread; a wait with no step left fails,
 * since nothing would wake the taker. A third processor's spinner runs on
 * a stack of its own (`park_host.c`), so that the queue of spinners holds
 * three at once.
 */

#include "proc/mutex.c"

extern int printf(const char *fmt, ...);

int park_run(void (*fn)(void));
int park_resume(void);
void park(void);

unsigned int host_cpu;
struct percpu cpu_table[NR_CPUS];

static struct tgroup group;
static process_t holder, waiter, stealer, third;
static struct mutex m;
static int fails;

/* The model's scheduler: whether each processor's thread runs, the pauses
 * the holder runs before its release (0 for no release), the pauses and
 * sleeps counted, and a row's hook for a pause, which may act as another
 * processor and says whether the pause goes on. */
static int running[NR_CPUS];
static int preempted[NR_CPUS];
static int hold, hold_cpu;
static int pauses, sleeps, wakes, firsts, queued;
static int (*pause_hook)(void);
static int (*step)(void);
static int signal_flag;

/** Count a failed check and print `what`. */
static void check(int ok, const char *what)
{
    if (!ok) {
        printf("mutex_hosttest: fail: %s\n", what);
        fails++;
    }
}

void k_panic(const char *message)
{
    printf("mutex_hosttest: fail: panic: %s\n", message);
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

/** Whether a row has the hypervisor preempt processor `cpu`. */
int arch_cpu_preempted(int cpu)
{
    return cpu >= 0 && cpu < NR_CPUS && preempted[cpu];
}

/** Give `m` back as the thread of processor `cpu`. */
static void unlock_as(int cpu)
{
    unsigned int me = host_cpu;

    host_cpu = (unsigned int)cpu;
    mutex_unlock(&m);
    host_cpu = me;
}

/** Take `m` as the thread of processor `cpu`, as a spinner's take would. */
static int take_as(int cpu)
{
    unsigned int me = host_cpu;
    int took;

    host_cpu = (unsigned int)cpu;
    took = mutex_trylock(&m);
    host_cpu = me;
    return took;
}

/**
 * The scheduler's owner spin: the spinner pauses while `owner` is processor
 * `cpu`'s thread and runs, as sched.c's does; with no owner only its own
 * state counts. Each pause of a holder that runs counts down `hold`, and
 * at 0 the holder gives the mutex back.
 */
int sched_spin_on(const process_t *owner, int cpu)
{
    pauses++;
    if (pause_hook) {
        return pause_hook();
    }
    if (!owner) {
        return 1;
    }
    if (cpu < 0 || cpu >= NR_CPUS || cpu_table[cpu].current != owner || !running[cpu]) {
        return 0;
    }
    if (hold > 0 && --hold == 0) {
        unlock_as(hold_cpu);
    }
    return 1;
}

int sched_running_cpu(const process_t *owner)
{
    int c;

    for (c = 0; c < NR_CPUS; c++) {
        if (owner && cpu_table[c].current == owner && running[c]) {
            return c;
        }
    }
    return -1;
}

/** As sched.c's: a thread no interrupt switches. */
int sched_spin_pinned(void)
{
    const process_t *p = current_proc;

    return p && (p->no_tick_switch || p->in_syscall || p->in_fault);
}

/** The waits: the sleeper is on the queue from before its first test to
 * after its last, and each sleep runs the row's next step. */
static int model_wait(wq_cond_fn cond, void *arg, int interruptible)
{
    int rc = 0;

    queued = 1;
    while (!cond(arg)) {
        sleeps++;
        if (!step || !step()) {
            check(0, "a sleep that no step ends");
            rc = -EINVAL;
            break;
        }
        if (interruptible && signal_flag) {
            rc = -EINTR;
            break;
        }
    }
    queued = 0;
    return rc;
}

int wq_wait_event_nointr(wait_queue_t *wq, wq_cond_fn cond, void *arg)
{
    (void)wq;
    return model_wait(cond, arg, 0);
}

int wq_wait_event_killable(wait_queue_t *wq, wq_cond_fn cond, void *arg, const char *name)
{
    (void)wq;
    (void)name;
    return model_wait(cond, arg, 0);
}

int wq_wait_event_sigflag(wait_queue_t *wq, wq_cond_fn cond, void *arg, const char *name)
{
    (void)wq;
    (void)name;
    return model_wait(cond, arg, 1);
}

int wq_active(const wait_queue_t *wq)
{
    (void)wq;
    return queued;
}

void wq_wake_all(wait_queue_t *wq)
{
    (void)wq;
    wakes += queued;
}

/** The sleeper is the row's waiter, of processor 0. */
void wq_wake_first(wait_queue_t *wq, void (*fn)(struct process *first, void *arg), void *arg)
{
    (void)wq;
    fn(queued ? &waiter : NULL, arg);
    firsts += queued;
}

void wait_queue_init(wait_queue_t *wq)
{
    (void)wq;
}

int bkl_held(void)
{
    return 1;
}

void bkl_enter(void)
{}

void bkl_exit(void)
{}

/** A signal the row raised is pending. */
int signal_pending(void)
{
    return signal_flag;
}

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

/** Zero the `n` bytes at `p`. */
static void zero(void *p, size_t n)
{
    unsigned char *b = p;

    while (n--) {
        *b++ = 0;
    }
}

/** Start a row: processor 1's holder has `m`, running unless `asleep`, and
 * processor 0's waiter, inside a system call, is about to take it. */
static void row_start(int asleep)
{
    mutex_init(&m, "row");
    zero(running, sizeof(running));
    zero(preempted, sizeof(preempted));
    zero(osq_nodes, sizeof(osq_nodes));
    cpu_table[0].current = &waiter;
    cpu_table[1].current = &holder;
    cpu_table[2].current = &stealer;
    cpu_table[3].current = &third;
    waiter.in_syscall = 1;
    group.pending = 0;
    running[0] = running[2] = running[3] = 1;
    running[1] = !asleep;
    hold = 0;
    hold_cpu = 1;
    pauses = sleeps = wakes = firsts = queued = 0;
    pause_hook = 0;
    step = 0;
    signal_flag = 0;
    host_cpu = 1;
    mutex_lock(&m);
    host_cpu = 0;
}

/** A step: the holder, which slept, wakes and gives `m` back. */
static int holder_gives(void)
{
    running[1] = 1;
    unlock_as(1);
    return 1;
}

/**
 * A short hold costs no sleep: the taker spins while the holder runs and has
 * the mutex once the holder gives it back. The same hold with the spin off,
 * for a thread an interrupt may switch, which is every taker before the
 * change, costs one sleep and one wake; so does a holder asleep.
 */
static void row_short_hold(void)
{
    row_start(0);
    hold = 40;
    step = holder_gives;
    mutex_lock(&m);
    printf("mutex_hosttest: short hold, spin: %d sleeps, %d pauses\n", sleeps, pauses);
    check(sleeps == 0 && wakes == 0, "a short hold by a running holder costs no sleep");
    check(pauses >= 40, "the taker pauses through the hold");
    check(m.owner == (uintptr_t)&waiter && m.owner_cpu == 0, "the word names the taker");
    check(m.osq == 0, "the queue of spinners is empty after the take");
    host_cpu = 0;
    mutex_unlock(&m);
    check(m.owner == 0, "the release frees the word");

    row_start(0);
    waiter.in_syscall = 0;
    hold = 40;
    step = holder_gives;
    mutex_lock(&m);
    printf("mutex_hosttest: short hold, no spin: %d sleeps, %d pauses\n", sleeps, pauses);
    check(sleeps == 1 && wakes == 1 && pauses == 0, "without the spin the same hold costs a sleep");
    check(mutex_held(&m), "the sleeper has the mutex");

    row_start(1);
    step = holder_gives;
    mutex_lock(&m);
    check(sleeps == 1 && m.osq == 0, "a holder asleep sends the taker to sleep at once");
}

/** A step for the hold below: the holder stops running part way. */
static int stop_after_ten(void)
{
    if (pauses == 10) {
        running[1] = 0;
    }
    return 1;
}

/** The pause hook that runs `stop_after_ten` before the model's own. */
static int pause_stop(void)
{
    const process_t *owner = (const process_t *)MUTEX_OWNER(m.owner);

    (void)stop_after_ten();
    return owner && cpu_table[1].current == owner && running[1];
}

/**
 * A spinner whose holder sleeps part way through the hold leaves the queue
 * and sleeps; one whose holder took the mutex on another processor than the
 * one it runs on now finds it there and spins.
 */
static void row_holder_moves(void)
{
    row_start(0);
    pause_hook = pause_stop;
    step = holder_gives;
    mutex_lock(&m);
    check(sleeps == 1 && pauses == 10 && m.osq == 0,
          "a holder that sleeps mid-hold sends the spinner to sleep, the queue left");

    row_start(0);
    m.owner_cpu = 3;
    hold = 5;
    mutex_lock(&m);
    check(sleeps == 0 && mutex_held(&m), "a stale note of the holder's processor still spins");
}

/** A step: the holder gives `m` back, and before the woken sleeper runs a
 * spinner of processor 2 takes it. */
static int give_and_steal(void)
{
    unlock_as(1);
    check(take_as(2), "the spinner takes the mutex the release freed");
    return 1;
}

/** A step: the spinner gives back the mutex the sleeper asked to have. */
static int stealer_gives(void)
{
    check((m.owner & MUTEX_HANDOFF) != 0, "the woken sleeper asked for the handoff");
    unlock_as(2);
    check(m.owner == (uintptr_t)&waiter, "the release hands the word straight to the sleeper");
    check(!take_as(3), "no spinner takes a mutex handed over");
    return 1;
}

/** Run `give_and_steal`, then `stealer_gives`. */
static int steal_then_give(void)
{
    if (sleeps == 1) {
        return give_and_steal();
    }
    return stealer_gives();
}

/**
 * A sleeper woken to a mutex a spinner took again asks for the handoff, and
 * the spinner's release gives the mutex to the sleeper and wakes it alone,
 * so the sleeper is not starved. With no sleeper left, a release that finds
 * the flag frees the word.
 */
static void row_handoff(void)
{
    row_start(1);
    step = steal_then_give;
    mutex_lock(&m);
    check(sleeps == 2 && firsts == 1 && mutex_held(&m) && m.owner_cpu == 0,
          "the starved sleeper has the mutex after two sleeps");
    check((m.owner & MUTEX_HANDOFF) == 0, "the handoff clears the flag");

    row_start(0);
    m.owner |= MUTEX_HANDOFF;
    unlock_as(1);
    check(m.owner == 0 && firsts == 0, "a handoff with no sleeper frees the word");
}

/** A step: the holder hands `m` over, and a signal comes before the sleeper
 * runs. */
static int hand_over_then_signal(void)
{
    m.owner |= MUTEX_HANDOFF;
    unlock_as(1);
    signal_flag = 1;
    return 1;
}

/** A step: a signal comes, and nothing hands `m` over. */
static int signal_only(void)
{
    signal_flag = 1;
    return 1;
}

/** A step: SIGKILL comes for the waiter's process. */
static int kill_waiter(void)
{
    group.pending |= KSIG_BIT(SIGKILL);
    return 1;
}

/**
 * A wait a signal ends holds the mutex when it was handed over first, as
 * Linux's waiter takes the lock before it tests the signal; otherwise it
 * returns -EINTR without it. A killable wait SIGKILL ends returns -EINTR.
 */
static void row_signals(void)
{
    waiter.tg = &group;
    row_start(1);
    step = hand_over_then_signal;
    check(mutex_lock_interruptible(&m) == 0 && mutex_held(&m),
          "a wait a signal ends after the handoff holds the mutex");

    row_start(1);
    step = signal_only;
    check(mutex_lock_interruptible(&m) == -EINTR && !mutex_held(&m),
          "a wait a signal ends with no handoff returns -EINTR");

    row_start(1);
    step = kill_waiter;
    check(mutex_lock_killable(&m) == -EINTR && MUTEX_OWNER(m.owner) == (uintptr_t)&holder,
          "a killable wait SIGKILL ends returns -EINTR, the holder keeping the mutex");
    unlock_as(1);
    check(m.owner == 0, "the holder's release after the killed sleeper left frees the word");
}

/* The third spinner's result, for the row that runs it parked. */
static int third_rc;

/** Processor 3's spinner: queue, wait, and give the head back. */
static void third_spins(void)
{
    host_cpu = 3;
    third_rc = osq_lock(&m);
    if (third_rc) {
        osq_unlock(&m);
    }
}

/** The pause hook of the middle row: start processor 3's spinner from
 * processor 2's first pause, park it in its own, and have processor 2 give
 * way. */
static int pause_middle(void)
{
    if (host_cpu == 2) {
        check(park_run(third_spins) == 0, "processor 3's spinner waits in the queue");
        host_cpu = 2;
        return 0;
    }
    park();
    host_cpu = 3;
    return 1;
}

/** The pause hook of the pass row: processor 1 passes the head on. */
static int pause_pass(void)
{
    host_cpu = 1;
    osq_unlock(&m);
    host_cpu = 2;
    return 1;
}

/** The pause hook of the leave row: processor 2 must give way. */
static int pause_leave(void)
{
    return 0;
}

/**
 * The queue of spinners, as Linux's OSQ: the head passes to the one behind;
 * the last one leaves and the tail goes back to the one ahead; one ahead
 * preempted by its hypervisor sends the one behind away; and one in the
 * middle leaves by linking the ones on either side, after which the head
 * passes straight to the third.
 */
static void row_queue(void)
{
    row_start(0);
    host_cpu = 1;
    check(osq_lock(&m) == 1 && m.osq == 2, "the first spinner is the head at once");
    host_cpu = 2;
    pause_hook = pause_pass;
    check(osq_lock(&m) == 1 && m.osq == 3, "the head passes to the spinner behind");
    osq_unlock(&m);
    check(m.osq == 0, "the last spinner's release empties the queue");

    row_start(0);
    host_cpu = 1;
    (void)osq_lock(&m);
    host_cpu = 2;
    pause_hook = pause_leave;
    check(osq_lock(&m) == 0 && m.osq == 2 && osq_nodes[1].next == 0,
          "the last spinner leaves and the tail is the one ahead again");
    host_cpu = 1;
    osq_unlock(&m);
    check(m.osq == 0, "the head's release then empties the queue");

    row_start(0);
    host_cpu = 1;
    (void)osq_lock(&m);
    host_cpu = 2;
    preempted[1] = 1;
    check(osq_lock(&m) == 0 && m.osq == 2 && pauses == 0,
          "a spinner behind one preempted by its hypervisor leaves without a pause");
    host_cpu = 1;
    osq_unlock(&m);

    row_start(0);
    host_cpu = 1;
    (void)osq_lock(&m);
    host_cpu = 2;
    pause_hook = pause_middle;
    check(osq_lock(&m) == 0, "the middle spinner gives way");
    check(m.osq == 4 && osq_nodes[1].next == 4 && osq_nodes[3].prev == 2,
          "the middle spinner's leave links the head to the third");
    host_cpu = 1;
    osq_unlock(&m);
    check(osq_nodes[3].locked == 1, "the head passes to the third");
    check(park_resume() == 1 && third_rc == 1 && m.osq == 0,
          "the third spinner is the head and empties the queue");
}

int main(void)
{
    holder.tg = waiter.tg = stealer.tg = third.tg = &group;
    holder.in_syscall = stealer.in_syscall = third.in_syscall = 1;
    row_short_hold();
    row_holder_moves();
    row_handoff();
    row_signals();
    row_queue();
    if (fails) {
        printf("mutex_hosttest: %d failed\n", fails);
        return 1;
    }
    printf("mutex_hosttest: ok\n");
    return 0;
}
