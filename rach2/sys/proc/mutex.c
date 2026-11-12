/* The mutex and the reader-writer semaphore of mutex.h. */

#include "mutex.h"

#include "process.h"
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

/**
 * Wake the sleepers on `wq` of a lock just let go, if it has any, as Linux's
 * __up_read and __mutex_unlock_slowpath wake only with a waiter recorded
 * (kernel/locking/rwsem.c, kernel/locking/mutex.c): a wake of an empty queue
 * costs a pass with interrupts off, which QEMU's emulator makes dear. The
 * release's unlock of the lock's spinlock is an exchange, a full barrier,
 * between its store and this read of the queue, and so is the unlock of the
 * queue a sleeper files itself on before it tests the lock (`wq_wait`):
 * either the sleeper's test sees the release or this read sees the sleeper.
 */
static void wake_waiters(wait_queue_t *wq)
{
    if (wq_active(wq)) {
        wq_wake_all(wq);
    }
}

void mutex_init_key(struct mutex *m, const char *name, const struct lockdep_key *key)
{
    spin_lock_init(&m->spin, "mutex.spin");
    m->owner = 0;
    wait_queue_init(&m->wq);
    lockdep_init_map(&m->dep, name, key);
    (void)name;
    (void)key;
}

/** The wait condition of `mutex_lock`: `arg`, a mutex, is free. */
static int mutex_free(void *arg)
{
    const struct mutex *m = arg;

    return m->owner == 0;
}

/** Take `m` if it is free: 1 when taken, else 0. */
static int mutex_take(struct mutex *m)
{
    uint32_t flags = spin_lock_irqsave(&m->spin);
    int got = m->owner == 0;

    if (got) {
        m->owner = mutex_me();
    }
    spin_unlock_irqrestore(&m->spin, flags);
    return got;
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

void mutex_lock(struct mutex *m)
{
    DEP_ACQUIRE(&m->dep);
    while (!mutex_take(m)) {
        wq_wait_event_nointr(&m->wq, mutex_free, m);
    }
}

void mutex_unlock(struct mutex *m)
{
    uint32_t flags = spin_lock_irqsave(&m->spin);

    if (m->owner != mutex_me()) {
        spin_unlock_irqrestore(&m->spin, flags);
        k_panic("mutex: an unlock by a thread that does not hold it");
    }
    m->owner = 0;
    spin_unlock_irqrestore(&m->spin, flags);
    DEP_RELEASE(&m->dep);
    wake_waiters(&m->wq);
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

int mutex_lock_interruptible(struct mutex *m)
{
    for (;;) {
        DEP_ACQUIRE(&m->dep);
        if (mutex_take(m)) {
            return 0;
        }
        DEP_RELEASE(&m->dep);
        if (wq_wait_event_sigflag(&m->wq, mutex_free, m, "mutex_lock_interruptible") != 0 &&
            mutex_signal_pending()) {
            return -EINTR;
        }
    }
}

/** Whether the calling thread has SIGKILL pending, its own or its
 * process's. */
static int mutex_killed(void)
{
    return ((current_proc->sig_pending | current_proc->tg->pending) & KSIG_BIT(SIGKILL)) != 0;
}

/** The killable waiter's condition: `m` free, or the waiter killed. */
static int mutex_free_or_killed(void *arg)
{
    return mutex_free(arg) || mutex_killed();
}

int mutex_lock_killable(struct mutex *m)
{
    DEP_ACQUIRE(&m->dep);
    while (!mutex_take(m)) {
        if (mutex_killed()) {
            DEP_RELEASE(&m->dep);
            return -EINTR;
        }
        (void)wq_wait_event_killable(&m->wq, mutex_free_or_killed, m, "__mutex_lock");
    }
    return 0;
}

int mutex_held(const struct mutex *m)
{
    return m->owner == mutex_me();
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
