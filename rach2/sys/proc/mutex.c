/* The mutex and the reader-writer semaphore of mutex.h. */

#include "mutex.h"

#include "process.h"
#include "sched.h"
#include "tgroup.h"
#include "smp.h"
#include "../lib/kerrno.h"
#include "../lib/ksignal.h"
#include "../panic.h"
#include "../arch/arch.h"

/* The checker's notes of a sleeping lock taken and let go, from the caller
 * of the function that uses them. */
#define DEP_CALLER ((uintptr_t)(const char *)__builtin_return_address(0))
#ifdef RACH2_LOCKDEP
#define DEP_ACQUIRE(map) lockdep_acquire((map), 0, LOCKDEP_SLEEPING, DEP_CALLER)
#define DEP_RELEASE(map) lockdep_release((map), DEP_CALLER)
#else
#define DEP_ACQUIRE(map) ((void)0)
#define DEP_RELEASE(map) ((void)0)
#endif

/** The record that stands for the caller as a holder: its thread's, or this
 * processor's before the first thread runs. */
static const void *mutex_me(void)
{
    return current_proc ? (const void *)current_proc : (const void *)this_cpu();
}

/** `mutex_me` as a mutex's word holds it. */
static uintptr_t mutex_me_word(void)
{
    const void *me = mutex_me();

    return (uintptr_t)me;
}

/**
 * Wake the sleepers on `wq` of a lock just let go, if it has any, as Linux's
 * __up_read and __mutex_unlock_slowpath wake only with a waiter recorded
 * (kernel/locking/rwsem.c, kernel/locking/mutex.c): a wake of an empty queue
 * costs a pass with interrupts off, which QEMU's emulator makes dear. The
 * release's atomic exchange of the lock's word is a full barrier between
 * its store and this read of the queue, and so is the unlock of the
 * queue a sleeper files itself on before it tests the lock (`wq_wait`):
 * either the sleeper's test sees the release or this read sees the sleeper.
 */
static void wake_waiters(wait_queue_t *wq)
{
    if (wq_active(wq)) {
        wq_wake_all(wq);
    }
}

/* The holder's record in a mutex's word, without the flag. */
#define MUTEX_OWNER(word) ((word) & ~(uintptr_t)MUTEX_HANDOFF)

/*
 * A spinner's place in a mutex's queue (`osq_lock`), one for each processor,
 * since a processor spins for one mutex at a time and its thread does not
 * leave it while it spins (`sched_spin_pinned`). `next` and `prev` name
 * processors as their index + 1, 0 for none, as Linux encodes its tail
 * (kernel/locking/osq_lock.c:22-36), so that 32-bit exchanges serve on both
 * architectures. The places outlive every queue, so a spinner may write the
 * one ahead after that one has left.
 */
struct osq_node {
    volatile unsigned int next;   /* the spinner behind, + 1 */
    volatile unsigned int prev;   /* the spinner ahead, + 1 */
    volatile unsigned int locked; /* 1 once the one ahead hands over the head */
};

static struct osq_node osq_nodes[NR_CPUS];

void mutex_init_key(struct mutex *m, const char *name, const struct lockdep_key *key)
{
    m->owner = 0;
    m->owner_cpu = -1;
    m->osq = 0;
    wait_queue_init(&m->wq);
    lockdep_init_map(&m->dep, name, key);
    (void)name;
    (void)key;
}

/** Take `m` if it is free: 1 when taken, else 0. The compare-and-exchange
 * records the holder with the take; the hint of its processor follows. */
static int mutex_take(struct mutex *m)
{
    uintptr_t me = mutex_me_word();

    if (m->owner != 0 || arch_atomic_cmpxchg_ptr(&m->owner, 0, me) != 0) {
        return 0;
    }
    m->owner_cpu = (int)arch_cpu_id();
    return 1;
}

/** Whether the caller holds `m`, by its own take or by a release that
 * handed it over (`mutex_grant`); when it does, its processor is noted. */
static int mutex_mine(struct mutex *m)
{
    if (MUTEX_OWNER(m->owner) != mutex_me_word()) {
        return 0;
    }
    m->owner_cpu = (int)arch_cpu_id();
    return 1;
}

/**
 * Whether the holder whose word is `word` runs on another processor, for a
 * spinner to pause once and look again (`sched_spin_on`). A word of 0 is a
 * free mutex, which the spinner may try, and then only its own state
 * counts. The holder's processor is checked first as its note gives it; a
 * note not yet written by a holder that has just taken the mutex, or left
 * behind by one that slept and woke elsewhere, is corrected by a look at
 * every processor (`sched_running_cpu`).
 */
static int mutex_owner_runs(const struct mutex *m, uintptr_t word)
{
    const process_t *owner = (const process_t *)MUTEX_OWNER(word);
    int cpu = m->owner_cpu, now;

    if (word == 0) {
        return sched_spin_on(0, -1);
    }
    if (sched_spin_on(owner, cpu)) {
        return 1;
    }
    now = sched_running_cpu(owner);
    return now >= 0 && now != cpu && sched_spin_on(owner, now);
}

/** Wait until the processor ahead (`osq_lock`) or the queue's last place
 * hands over, or take the queue back to `old` when `node` is the last:
 * the processor behind, + 1, or 0 when `node` was the last, as Linux's
 * osq_wait_next (osq_lock.c:44-80). */
static unsigned int osq_wait_next(volatile unsigned int *tail, struct osq_node *node,
                                  unsigned int old)
{
    unsigned int curr = (unsigned int)arch_cpu_id() + 1u;
    unsigned int next;

    for (;;) {
        if (*tail == curr && arch_atomic_cmpxchg(tail, curr, old) == curr) {
            return 0;
        }
        if (node->next != 0) {
            next = arch_atomic_xchg(&node->next, 0);
            if (next != 0) {
                return next;
            }
        }
        arch_relax();
    }
}

/**
 * Take the head of `m`'s queue of spinners: 1 once the caller is the one
 * spinner that tries the lock word, or 0 when it should sleep instead,
 * having left the queue, as Linux's osq_lock (osq_lock.c:91-210). A queued
 * spinner waits on its own place and reads neither the lock word nor the
 * holder, so the spinners behind the head touch no line the holder writes;
 * it leaves when the processor ahead is preempted by its hypervisor or the
 * caller must give way, a claim of the big lock it holds among the reasons
 * (`sched_spin_on`), and the head that finds the holder asleep passes the
 * queue on and leaves too. The place ahead is unlinked in Linux's three
 * steps: take back its pointer to this one, wait for the one behind or
 * become the last, and link the two.
 */
static int osq_lock(struct mutex *m)
{
    unsigned int curr = (unsigned int)arch_cpu_id() + 1u;
    struct osq_node *node = &osq_nodes[curr - 1u];
    unsigned int prev, next;

    node->locked = 0;
    node->next = 0;
    prev = arch_atomic_xchg(&m->osq, curr);
    if (prev == 0) {
        return 1;
    }
    node->prev = prev;
    osq_nodes[prev - 1u].next = curr;
    while (!node->locked) {
        if (arch_cpu_preempted((int)node->prev - 1) || !sched_spin_on(0, -1)) {
            break;
        }
    }
    if (node->locked) {
        return 1;
    }
    for (;;) {
        if (osq_nodes[prev - 1u].next == curr &&
            arch_atomic_cmpxchg(&osq_nodes[prev - 1u].next, curr, 0) == curr) {
            break;
        }
        if (node->locked) {
            return 1;
        }
        arch_relax();
        prev = node->prev;
    }
    next = osq_wait_next(&m->osq, node, prev);
    if (next == 0) {
        return 0;
    }
    osq_nodes[next - 1u].prev = prev;
    osq_nodes[prev - 1u].next = next;
    return 0;
}

/** Give the head of `m`'s queue to the spinner behind, if any, as Linux's
 * osq_unlock (osq_lock.c:212-236). */
static void osq_unlock(struct mutex *m)
{
    unsigned int curr = (unsigned int)arch_cpu_id() + 1u;
    struct osq_node *node = &osq_nodes[curr - 1u];
    unsigned int next;

    if (arch_atomic_cmpxchg(&m->osq, curr, 0) == curr) {
        return;
    }
    next = arch_atomic_xchg(&node->next, 0);
    if (next == 0) {
        next = osq_wait_next(&m->osq, node, 0);
    }
    if (next != 0) {
        osq_nodes[next - 1u].locked = 1;
    }
}

/**
 * Spin for `m` while its holder runs on another processor: 1 once taken, or
 * 0 for the caller to sleep, as Linux's mutex_optimistic_spin
 * (kernel/locking/mutex.c:441-510). A hold is often shorter than a sleep and
 * a wake, which for a halted processor costs an interprocessor interrupt.
 * The spinner tries the word whenever it changes, and gives up when the
 * holder sleeps, is preempted by its hypervisor or waits for the big lock
 * the spinner holds, or when the spinner must give way (`sched_spin_on`).
 * Only a thread that no interrupt switches spins, since its queue's place
 * is its processor's (`sched_spin_pinned`).
 */
static int mutex_spin(struct mutex *m)
{
    uintptr_t word;

    if (!sched_spin_pinned() || !mutex_owner_runs(m, m->owner) || !osq_lock(m)) {
        return 0;
    }
    while (!mutex_take(m)) {
        word = m->owner;
        while (word != 0 && m->owner == word) {
            if (!mutex_owner_runs(m, word)) {
                osq_unlock(m);
                return 0;
            }
        }
    }
    osq_unlock(m);
    return 1;
}

/** A sleeper's wait for `m` (`mutex_wait_cond`). */
struct mutex_wait {
    struct mutex *m;
    int tests;    /* the condition's tests so far: past the first, it slept */
    int killable; /* SIGKILL ends the wait */
};

/** Whether the calling thread has SIGKILL pending, its own or its
 * process's. */
static int mutex_killed(void)
{
    return ((current_proc->sig_pending | current_proc->tg->pending) & KSIG_BIT(SIGKILL)) != 0;
}

/** Set MUTEX_HANDOFF in `m`'s word while a holder has it, so that its
 * release hands it to the oldest sleeper (`mutex_unlock`). */
static void mutex_ask_handoff(struct mutex *m)
{
    uintptr_t word = m->owner;

    while (word != 0 && (word & MUTEX_HANDOFF) == 0) {
        uintptr_t was = arch_atomic_cmpxchg_ptr(&m->owner, word, word | MUTEX_HANDOFF);

        if (was == word) {
            break;
        }
        word = was;
    }
}

/**
 * The condition of a sleeper's wait, `arg` a `struct mutex_wait`: the
 * caller has `m`, by its own take or handed over, or is killed in a
 * killable wait. A test after the first follows a sleep, and one that then
 * finds `m` held asks for the handoff, as Linux's first waiter sets
 * MUTEX_FLAG_HANDOFF when it wakes to a mutex taken again
 * (kernel/locking/mutex.c:110-121, 693-699). The sleeper is on the queue,
 * so a release after the test wakes it.
 */
static int mutex_wait_cond(void *arg)
{
    struct mutex_wait *w = arg;

    if (mutex_take(w->m) || mutex_mine(w->m)) {
        return 1;
    }
    if (w->tests++ > 0) {
        mutex_ask_handoff(w->m);
    }
    return w->killable && mutex_killed();
}

/** Ready `w` for a sleeper's wait for `m`. */
static void mutex_wait_init(struct mutex_wait *w, struct mutex *m, int killable)
{
    w->m = m;
    w->tests = 0;
    w->killable = killable;
}

/** A trylock that succeeds is noted as a take, which adds its order. */
int mutex_trylock(struct mutex *m)
{
    if (!mutex_take(m)) {
        return 0;
    }
    DEP_ACQUIRE(&m->dep);
    return 1;
}

/** Take `m` for `mutex_lock` or `mutex_lock_nested`, which have told the
 * lock checker of the take. Before the first thread nothing sleeps and no
 * other processor runs, so the take is tried in place. */
static void mutex_lock_held(struct mutex *m)
{
    struct mutex_wait w;

    if (mutex_take(m) || mutex_spin(m)) {
        return;
    }
    if (!current_proc) {
        while (!mutex_take(m)) {
            arch_relax();
        }
        return;
    }
    mutex_wait_init(&w, m, 0);
    (void)wq_wait_event_nointr(&m->wq, mutex_wait_cond, &w);
}

void mutex_lock(struct mutex *m)
{
    DEP_ACQUIRE(&m->dep);
    mutex_lock_held(m);
}

void mutex_lock_nested(struct mutex *m, unsigned int subclass)
{
#ifdef RACH2_LOCKDEP
    lockdep_acquire(&m->dep, subclass, LOCKDEP_SLEEPING, DEP_CALLER);
#endif
    (void)subclass;
    mutex_lock_held(m);
}

/**
 * Hand `m` to the oldest sleeper `first`, or free it when none sleeps, under
 * the queue's lock (`wq_wake_first`), as Linux's __mutex_handoff gives the
 * first waiter the word (kernel/locking/mutex.c:225-248). The word goes from
 * holder to sleeper in one exchange, so no spinner takes it between them;
 * the sleeper's processor is not known until it runs.
 */
static void mutex_grant(struct process *first, void *arg)
{
    struct mutex *m = arg;

    m->owner_cpu = -1;
    (void)arch_atomic_xchg_ptr(&m->owner, (uintptr_t)first);
}

/** A release with no flag set is one compare-and-exchange; one that finds
 * MUTEX_HANDOFF hands `m` over and wakes its new holder alone. The exchange
 * that frees `m` is the full barrier `wake_waiters` needs. */
void mutex_unlock(struct mutex *m)
{
    uintptr_t word = m->owner, was;

    if (word == 0 || MUTEX_OWNER(word) != mutex_me_word()) {
        k_panic("mutex: an unlock by a thread that does not hold it");
    }
    DEP_RELEASE(&m->dep);
    while ((word & MUTEX_HANDOFF) == 0) {
        was = arch_atomic_cmpxchg_ptr(&m->owner, word, 0);
        if (was == word) {
            wake_waiters(&m->wq);
            return;
        }
        word = was;
    }
    wq_wake_first(&m->wq, mutex_grant, m);
}

/** Judge a signal-work flag with no mutex held, under the big lock that
 * signal_pending needs while it removes ignored signals from the queues. */
static int mutex_signal_pending(void)
{
    int big = !bkl_held();
    int pending;

    if (big) {
        bkl_enter();
    }
    pending = signal_pending();
    if (big) {
        bkl_exit();
    }
    return pending;
}

/** A wait the signal-work flag ends may have been handed `m` before it left
 * the queue, and then holds it whatever the signal. */
int mutex_lock_interruptible(struct mutex *m)
{
    struct mutex_wait w;

    DEP_ACQUIRE(&m->dep);
    if (mutex_take(m) || mutex_spin(m)) {
        return 0;
    }
    mutex_wait_init(&w, m, 0);
    for (;;) {
        if (wq_wait_event_sigflag(&m->wq, mutex_wait_cond, &w, "mutex_lock_interruptible") == 0 ||
            mutex_mine(m)) {
            return 0;
        }
        DEP_RELEASE(&m->dep);
        if (mutex_signal_pending()) {
            return -EINTR;
        }
        DEP_ACQUIRE(&m->dep);
    }
}

/** The wait's condition is true at SIGKILL as well; the caller holds `m`
 * if it was handed over first. */
int mutex_lock_killable(struct mutex *m)
{
    struct mutex_wait w;

    DEP_ACQUIRE(&m->dep);
    if (mutex_take(m) || mutex_spin(m)) {
        return 0;
    }
    mutex_wait_init(&w, m, 1);
    (void)wq_wait_event_killable(&m->wq, mutex_wait_cond, &w, "__mutex_lock");
    if (mutex_mine(m)) {
        return 0;
    }
    DEP_RELEASE(&m->dep);
    return -EINTR;
}

int mutex_held(const struct mutex *m)
{
    return MUTEX_OWNER(m->owner) == mutex_me_word();
}

void rwsem_init_key(struct rwsem *s, const char *name, const struct lockdep_key *key)
{
    spin_lock_init(&s->spin, "rwsem.spin");
    s->state = 0;
    s->writers = 0;
    s->wowner = 0;
    wait_queue_init(&s->wq);
    lockdep_init_map(&s->dep, name, key);
    (void)name;
    (void)key;
}

/** The wait condition of `down_read`: no writer holds or waits for `arg`. */
static int rwsem_readable(void *arg)
{
    const struct rwsem *s = arg;

    return (s->state & (RWSEM_WRITER | RWSEM_WAITING)) == 0;
}

/** The wait condition of `down_write`: nobody holds `arg`. */
static int rwsem_free(void *arg)
{
    const struct rwsem *s = arg;

    return (s->state & (RWSEM_READERS | RWSEM_WRITER)) == 0;
}

/** Clear the bits `clear` of `s`'s state and set `set`, in one atomic step
 * that no reader's or writer's exchange comes between, and return the state
 * it left. */
static unsigned int state_change(struct rwsem *s, unsigned int clear, unsigned int set)
{
    unsigned int old = s->state;

    for (;;) {
        unsigned int now = (old & ~clear) | set;
        unsigned int was = arch_atomic_cmpxchg(&s->state, old, now);

        if (was == old) {
            return now;
        }
        old = was;
    }
}

/** Take `s` to read if no writer holds or waits for it: 1 when taken. One
 * exchange, retried only when another reader's changed the count. */
static int read_take(struct rwsem *s)
{
    unsigned int old = s->state;

    while ((old & (RWSEM_WRITER | RWSEM_WAITING)) == 0) {
        unsigned int was = arch_atomic_cmpxchg(&s->state, old, old + 1u);

        if (was == old) {
            return 1;
        }
        old = was;
    }
    return 0;
}

int down_read_trylock(struct rwsem *s)
{
    if (!read_take(s)) {
        return 0;
    }
    DEP_ACQUIRE(&s->dep);
    return 1;
}

void down_read(struct rwsem *s)
{
    DEP_ACQUIRE(&s->dep);
    while (!read_take(s)) {
        wq_wait_event_nointr(&s->wq, rwsem_readable, s);
    }
}

/** Take `s` to write if nobody holds it, keeping RWSEM_WAITING as it is:
 * 1 when taken. */
static int write_take(struct rwsem *s)
{
    unsigned int old = s->state;

    while ((old & (RWSEM_READERS | RWSEM_WRITER)) == 0) {
        unsigned int was = arch_atomic_cmpxchg(&s->state, old, old | RWSEM_WRITER);

        if (was == old) {
            s->wowner = mutex_me();
            return 1;
        }
        old = was;
    }
    return 0;
}

/**
 * A free lock is taken by one exchange. Otherwise the writer counts itself
 * waiting, under the spinlock, for its whole wait, so that readers that come
 * meanwhile queue behind it (RWSEM_WAITING), and the last waiting writer to
 * take the lock clears the bit.
 */
void down_write(struct rwsem *s)
{
    uint32_t flags;

    DEP_ACQUIRE(&s->dep);
    if (arch_atomic_cmpxchg(&s->state, 0, RWSEM_WRITER) == 0) {
        s->wowner = mutex_me();
        return;
    }
    flags = spin_lock_irqsave(&s->spin);
    if (s->writers++ == 0) {
        (void)state_change(s, 0, RWSEM_WAITING);
    }
    while (!write_take(s)) {
        spin_unlock_irqrestore(&s->spin, flags);
        wq_wait_event_nointr(&s->wq, rwsem_free, s);
        flags = spin_lock_irqsave(&s->spin);
    }
    if (--s->writers == 0) {
        (void)state_change(s, RWSEM_WAITING, 0);
    }
    spin_unlock_irqrestore(&s->spin, flags);
}

/** The wait condition of `down_write_killable`: no reader or writer holds
 * `arg`, or the caller has SIGKILL pending. */
static int rwsem_free_or_killed(void *arg)
{
    return rwsem_free(arg) || mutex_killed();
}

/** A writer that SIGKILL ends wakes the readers its wait held back, when
 * it was the last writer waiting. */
int down_write_killable(struct rwsem *s)
{
    uint32_t flags;
    int rc = 0;

    DEP_ACQUIRE(&s->dep);
    if (arch_atomic_cmpxchg(&s->state, 0, RWSEM_WRITER) == 0) {
        s->wowner = mutex_me();
        return 0;
    }
    flags = spin_lock_irqsave(&s->spin);
    if (s->writers++ == 0) {
        (void)state_change(s, 0, RWSEM_WAITING);
    }
    while (!write_take(s)) {
        if (mutex_killed()) {
            rc = -EINTR;
            break;
        }
        spin_unlock_irqrestore(&s->spin, flags);
        (void)wq_wait_event_killable(&s->wq, rwsem_free_or_killed, s, "rwsem_down_write_slowpath");
        flags = spin_lock_irqsave(&s->spin);
    }
    if (--s->writers == 0) {
        (void)state_change(s, RWSEM_WAITING, 0);
    }
    spin_unlock_irqrestore(&s->spin, flags);
    if (rc != 0) {
        wake_waiters(&s->wq);
        DEP_RELEASE(&s->dep);
    }
    return rc;
}

int down_write_trylock(struct rwsem *s)
{
    if (!write_take(s)) {
        return 0;
    }
    DEP_ACQUIRE(&s->dep);
    return 1;
}

/** The last reader wakes the sleepers, if any (`wake_waiters`); the
 * exchange that drops the count is the full barrier it needs. */
void up_read(struct rwsem *s)
{
    unsigned int old = s->state;

    for (;;) {
        unsigned int was;

        if ((old & RWSEM_READERS) == 0) {
            k_panic("rwsem: a read release with no reader");
        }
        was = arch_atomic_cmpxchg(&s->state, old, old - 1u);
        if (was == old) {
            break;
        }
        old = was;
    }
    DEP_RELEASE(&s->dep);
    if (((old - 1u) & RWSEM_READERS) == 0) {
        wake_waiters(&s->wq);
    }
}

/** The owner is cleared before the bit, so that a writer that takes the
 * lock at once does not lose its record to this release. */
void up_write(struct rwsem *s)
{
    if (!(s->state & RWSEM_WRITER) || s->wowner != mutex_me()) {
        k_panic("rwsem: a write release by a thread that does not hold it");
    }
    s->wowner = 0;
    (void)state_change(s, RWSEM_WRITER, 0);
    DEP_RELEASE(&s->dep);
    wake_waiters(&s->wq);
}
